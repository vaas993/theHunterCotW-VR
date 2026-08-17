#include "headtrack.h"

#include "apex.h"
#include "camera_probe.h"
#include "cbscan.h"
#include "config.h"
#include "overlay.h"
#include "log.h"
#include "vr.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstring>

namespace cotwvr {
namespace {

// Head rotation as a 3x3, in the HEADSET's frame (x right, y up, z back).
//
// ABSOLUTE, not relative to the recentre reference - and that distinction is a
// bug fix, not a detail. Taking the quaternion difference
// conj(recentre) * head expresses the rotation in the RECENTRE'S frame, so if
// the player was not perfectly level when he recentred (nobody is), "yaw" in
// that frame is a rotation about a TILTED axis. Turning the head then produces
// a pitch component as well, and the view travels in a curve - looking left
// also dips. Extracting the angles in the world frame, where OpenXR's Y really
// is up, and subtracting the recentre angles as SCALARS keeps yaw about true
// vertical.
//
// Cached on the render thread and read on the game thread; a one-frame-stale
// read is invisible, a lock in a hot hook is not.
float g_head[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
volatile LONG g_haveHead = 0;

// The recentre reference, as ANGLES rather than a frame.
float g_yawRef = 0.0f, g_pitchRef = 0.0f, g_rollRef = 0.0f;

volatile LONG g_recentreRequested = 1;   // recentre on the first good pose
XrVector3f g_posRef = {0, 0, 0};
bool g_havePosRef = false;

// GetTickCount at the moment an offset last actually reached the picture,
// stamped by the apply sites in camera_probe.cpp. 0 = never.
volatile LONG g_lastAppliedTick = 0;
// Set by the scheduled commit when recentre_reset_smoother is on. There is no
// other way to reach SteadyAim's function-local statics from outside.
volatile LONG g_steadyReset = 0;

// THE SCHEDULED REFERENCE ADVANCE. Written by the credit on the game thread,
// applied at the top of the next HeadTrackTick - one frame later, deliberately,
// so both eye passes of the credit frame still carry the old offset and both
// passes of the next carry none. The flag is the publish barrier: the floats go
// down first, the interlocked store makes them visible.
volatile LONG g_commitPending = 0;
float g_commitYaw = 0.0f, g_commitPitch = 0.0f;
// GetTickCount deadline for the accumulator watch in camera_probe.cpp.
volatile LONG g_recentreWatchUntil = 0;
// How many ramp slices are still owed. 0 = not ramping. Set when the key is
// pressed, spent one slice per frame by RecentreRampFraction().
volatile LONG g_rampLeft = 0;

// THE SETTLE GATE. A press arms g_settlePending instead of firing; the tick
// then watches how fast the head is turning and releases it once it has been
// still long enough, or when the deadline runs out.
volatile LONG g_settlePending = 0;
volatile LONG g_settleDeadline = 0;    // GetTickCount, fire regardless past this
volatile LONG g_settleStillSince = 0;  // GetTickCount the head last became still
float g_prevFwd[3] = {0, 0, -1};
bool  g_havePrevFwd = false;
ULONGLONG g_prevFwdTick = 0;

void QuatToMatrix(const XrQuaternionf& q, float* m) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    m[0] = 1 - 2 * (y * y + z * z);  m[1] = 2 * (x * y - z * w);      m[2] = 2 * (x * z + y * w);
    m[3] = 2 * (x * y + z * w);      m[4] = 1 - 2 * (x * x + z * z);  m[5] = 2 * (y * z - x * w);
    m[6] = 2 * (x * z - y * w);      m[7] = 2 * (y * z + x * w);      m[8] = 1 - 2 * (x * x + y * y);
}

XrQuaternionf Conjugate(const XrQuaternionf& q) { return {-q.x, -q.y, -q.z, q.w}; }

XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// The angle the axis test should use right now.
//
// A FIXED offset turned out to be nearly impossible to judge by eye - the world
// is simply rotated a bit, with nothing to compare it against, and the honest
// report was "I didn't see it". A SWEEP removes the judgement entirely: if the
// write reaches the view, the world rocks back and forth on its own while the
// player stands still. Nobody can miss that, and nobody has to estimate an
// angle to report it.
float TestDegrees() {
    float deg = Cfg().head_test_degrees;
    if (!Cfg().head_test_sweep) return deg;
    const double t = double(GetTickCount64()) * 0.001;
    return deg * sinf(float(t * 2.0));       // ~0.32 Hz, a slow obvious swing
}

// Yaw and pitch of a head matrix in the WORLD frame. Forward is the -Z column,
// so yaw is measured about world up and pitch is the forward vector's elevation
// - neither can leak into the other however the head is held.
void AnglesFromMatrix(const float* h, float* yaw, float* pitch, float* roll) {
    const float fx = -h[2], fy = -h[5], fz = -h[8];
    *yaw = atan2f(fx, -fz);
    const float c = fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy);
    *pitch = asinf(c);
    *roll = atan2f(h[3], h[4]);
}

// Yaw is circular: a reference of +170 deg and a head at -170 deg are 20 deg
// apart, not 340. Subtracting without wrapping would fling the view a full turn
// the moment the player crossed behind himself.
float WrapPi(float a) {
    const float kPi = 3.14159265358979f, kTwoPi = 6.28318530717959f;
    while (a > kPi) a -= kTwoPi;
    while (a < -kPi) a += kTwoPi;
    return a;
}

// *** ONE POSE PER FRAME, SERVED TO EVERY CONSUMER. ***
//
// The mod writes the head offset at several points in a frame - the early
// write, the camera transform builder, the reseed code cave - and each one
// used to READ THE POSE FRESH. The pose is republished from the OpenXR
// thread, so two reads inside one frame can differ, and the difference is
// injected into the engine's camera as a per-frame residual: a tiny apparent
// camera movement, which is several pixels on close geometry and nothing at
// distance. That is the measured signature of the object shake (2026-08-13),
// and it survives orientation smoothing because smoothing reduces the noise
// without making the two reads agree.
//
// Prior art says the same thing in one line (VR_PRIOR_ART_LESSONS #15,
// KisakCOD-VR): the pose must travel WITH the frame.
float g_latchYaw = 0.0f, g_latchPitch = 0.0f, g_latchRoll = 0.0f;
volatile LONG g_latchValid = 0;
volatile LONG g_latchServed = 0;

bool HeadAnglesRaw(float* yaw, float* pitch, float* roll) {
    if (!g_haveHead) return false;
    float h[9];
    memcpy(h, g_head, sizeof(h));
    float y, p, r;
    AnglesFromMatrix(h, &y, &p, &r);
    *yaw = WrapPi(y - g_yawRef);
    *pitch = p - g_pitchRef;
    *roll = WrapPi(r - g_rollRef);
    return true;
}

bool HeadAngles(float* yaw, float* pitch, float* roll) {
    if (Cfg().head_latch_per_frame && g_latchValid) {
        *yaw = g_latchYaw;
        *pitch = g_latchPitch;
        *roll = g_latchRoll;
        InterlockedIncrement(&g_latchServed);
        return true;
    }
    return HeadAnglesRaw(yaw, pitch, roll);
}

void HeadLatchFrameImpl() {
    if (!Cfg().head_latch_per_frame) {
        InterlockedExchange(&g_latchValid, 0);
        return;
    }
    float y = 0.0f, p = 0.0f, r = 0.0f;
    if (!HeadAnglesRaw(&y, &p, &r)) return;
    g_latchYaw = y;
    g_latchPitch = p;
    g_latchRoll = r;
    InterlockedExchange(&g_latchValid, 1);
}

// HOLD THE VIEW STEADIER WHILE AIMING.
//
// Exponential smoothing of the head ORIENTATION, in place, before the pose is
// published. Applied here rather than in each reader so that the view, the
// weapon and the scope all steady by exactly the same amount - three consumers
// smoothing separately is how two of them end up disagreeing.
//
// Smoothed as a matrix and re-orthonormalised, not as euler angles: blending
// angles goes wrong at the wrap point, and a yaw crossing 180 degrees would
// swing the whole way round the other way.
//
// The state is dropped whenever smoothing is not running, so that letting go of
// the trigger cannot make the view slide from a stale pose towards the live one.
void SteadyAim(float* m) {
    static bool active = false;
    static float prev[9];

    static LARGE_INTEGER last = {};

    // RECENTRE_REQUIREMENT item 3, behind recentre_reset_smoother, DEFAULT OFF.
    // RECENTRE_DESIGN section 4: dropping the state makes the next published
    // pose RAW instead of smoothed, stepping every reader's offset by
    // (raw - smoothed) - about 6 deg at tau=60 ms and 100 deg/s, up to ~25 deg
    // at the 250 ms ceiling. That is a picture jump, on the one press that must
    // not have one. Here so the owner can feel it without a rebuild.
    if (InterlockedExchange(&g_steadyReset, 0)) active = false;

    float tau = Cfg().aim_steady_ms;
    if (tau > 250.0f) tau = 250.0f;     // the ceiling prior art warns about
    const bool wantSteady = Cfg().aim_steady && tau > 1.0f &&
                            (!Cfg().aim_steady_scoped_only || PlayerIsScoped());
    // The tremor dead-band runs on its own terms - see below - so either can
    // ask for the smoother.
    const bool want = wantSteady || Cfg().head_tremor_level > 0;
    if (!want) {
        active = false;         // next entry starts from the live pose
        return;
    }
    if (!active) {
        memcpy(prev, m, sizeof(prev));
        QueryPerformanceCounter(&last);
        active = true;
        return;                 // first frame is raw; there is nothing to blend
    }

    // HOW MUCH OF THE LIVE POSE TO TAKE THIS FRAME, FROM THE CLOCK.
    //
    // k = 1 - exp(-dt / tau). Reading the real elapsed time rather than assuming
    // a frame length is what makes this behave identically at 90 fps and at the
    // 45 the AER switch produces - a fixed per-frame blend is quietly a
    // different setting at each.
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    float dtMs = freq.QuadPart
        ? float(double(now.QuadPart - last.QuadPart) * 1000.0 / double(freq.QuadPart))
        : 11.0f;
    last = now;
    // A hitch, a loading screen or a breakpoint must not teleport the view: past
    // a quarter second, take the live pose and start again.
    if (!(dtMs > 0.0f) || dtMs > 250.0f) {
        memcpy(prev, m, sizeof(prev));
        return;
    }
    float k = wantSteady ? (1.0f - expf(-dtMs / tau)) : 1.0f;
    if (k < 0.0f) k = 0.0f;
    if (k > 1.0f) k = 1.0f;

    // *** THE TREMOR DEAD-BAND - hold, release, leash. ***
    //
    // Gate on the ERROR (how far the held view is from the live pose), not on
    // a rate: a pulse is a few HUNDREDTHS of a degree and a real look is
    // degrees, which is the only axis on which the two separate (they share a
    // frequency band, so no filter can do it - see config.h). Inside the
    // dead-band the view simply holds, which costs no latency at all. Outside
    // it, a smoothstep release ramps in a short ema, and a hard leash caps the
    // lag absolutely - that leash is what stops the "floaty" the first
    // version had, where lag was rate-gate x frame time and so doubled at
    // 45 fps. Shape from opentrack's Accela, release curve from the Oculus
    // prediction patent, tau inside Casiez's 10-20 ms budget.
    if (Cfg().head_tremor_level > 0) {
        const float f0[3] = {-prev[2], -prev[5], -prev[8]};   // held forward
        const float f1[3] = {-m[2], -m[5], -m[8]};            // live forward
        float dot = f0[0] * f1[0] + f0[1] * f1[1] + f0[2] * f1[2];
        if (dot > 1.0f) dot = 1.0f;
        if (dot < -1.0f) dot = -1.0f;
        const float errDeg = acosf(dot) * 57.2957795f;

        int lvl = Cfg().head_tremor_level;
        if (lvl > 3) lvl = 3;
        const float kBand[4] = {0.0f, 0.03f, 0.05f, 0.10f};   // opentrack's scale
        const float D = kBand[lvl];
        const float E = Cfg().head_tremor_release_deg > 0.01f
                            ? Cfg().head_tremor_release_deg : 0.01f;
        const float L = Cfg().head_tremor_leash_deg;

        float kt;
        if (errDeg <= D) {
            kt = 0.0f;                                   // hold: nothing moves
        } else if (L > D && errDeg >= L) {
            kt = 1.0f;                                   // leash: never lag more
        } else {
            float t = (errDeg - D) / E;
            if (t > 1.0f) t = 1.0f;
            const float a = t * t * (3.0f - 2.0f * t);   // smoothstep(3t^2-2t^3)
            const float tauEf = Cfg().head_tremor_tau_ms * (1.0f - a);
            kt = a + (1.0f - a) *
                         (1.0f - expf(-dtMs / (tauEf > 0.01f ? tauEf : 0.01f)));
        }
        if (kt < k) k = kt;              // whichever asks for more steadiness
    }

    float out[9];
    for (int i = 0; i < 9; ++i) out[i] = prev[i] + (m[i] - prev[i]) * k;

    // Blending two rotation matrices gives something very slightly off being a
    // rotation - the axes drift out of square and out of unit length. Left alone
    // that accumulates into a shear, which reads as the world subtly stretching.
    // Gram-Schmidt puts it back.
    //
    // *** THE AXES ARE COLUMNS, NOT ROWS. *** This matrix is ROW-major:
    // QuatToMatrix writes m[0..2] as the first row, and AnglesFromMatrix reads
    // forward as -(m[2], m[5], m[8]) - the third COLUMN. Orthonormalising the
    // rows instead would silently transpose the frame, and a transposed rotation
    // is its inverse: the view would turn the wrong way while still looking like
    // a perfectly valid matrix.
    float b[3] = {out[2], out[5], out[8]};      // backward, +z (forward is -b)
    float u[3] = {out[1], out[4], out[7]};      // up, +y
    float bl = sqrtf(b[0]*b[0] + b[1]*b[1] + b[2]*b[2]);
    if (bl < 1e-6f) { active = false; return; }
    b[0] /= bl; b[1] /= bl; b[2] /= bl;
    const float d = u[0]*b[0] + u[1]*b[1] + u[2]*b[2];
    u[0] -= d * b[0]; u[1] -= d * b[1]; u[2] -= d * b[2];
    const float ul = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]);
    if (ul < 1e-6f) { active = false; return; }
    u[0] /= ul; u[1] /= ul; u[2] /= ul;
    // right = up x backward, which closes a right-handed frame.
    const float r0 = u[1]*b[2] - u[2]*b[1];
    const float r1 = u[2]*b[0] - u[0]*b[2];
    const float r2 = u[0]*b[1] - u[1]*b[0];

    m[0] = r0;   m[1] = u[0]; m[2] = b[0];
    m[3] = r1;   m[4] = u[1]; m[5] = b[1];
    m[6] = r2;   m[7] = u[2]; m[8] = b[2];
    memcpy(prev, m, sizeof(prev));
}

// Euler angles from the head matrix, for the axis test.
void MakeTestRotation(float* m, int axis, float degrees) {
    const float r = degrees * 0.01745329252f;
    const float c = cosf(r), s = sinf(r);
    memset(m, 0, sizeof(float) * 9);
    switch (axis) {
        case 1:  // yaw   - about the up axis
            m[0] = c;  m[2] = s;  m[4] = 1;  m[6] = -s; m[8] = c;  break;
        case 2:  // pitch - about the right axis
            m[0] = 1;  m[4] = c;  m[5] = -s; m[7] = s;  m[8] = c;  break;
        case 3:  // roll  - about the forward axis
            m[0] = c;  m[1] = -s; m[3] = s;  m[4] = c;  m[8] = 1;  break;
        default:
            m[0] = m[4] = m[8] = 1; break;
    }
}

}  // namespace

// *** FREE LOOK, AND WHY IT IS ALMOST FREE. ***
//
// Coupled and decoupled differ ONLY in whether the head offset is applied
// before or after the weapon is placed - the view ends up at
// (engineYaw - headYaw) either way. So switching between them mid-frame does
// not move the view at all; it only changes whether the gun comes along.
// That is what makes a hold-to-free-look key seamless here: press it and the
// gun stops following while your view carries on exactly where it was.
//
// Polled once per frame on the render thread and cached, because the aim hook
// runs deep inside the engine and has no business calling into the input API.
volatile LONG g_freeLook = 0;

bool FreeLookHeld() { return g_freeLook != 0; }

// Arm the ramp and raise the request. Split out of RecentreHead so the settle
// gate can call it later, once the head has stopped moving.
void FireRecentre() {
    int slices = Cfg().recentre_ramp_frames;
    if (slices < 1) slices = 1;
    if (slices > 60) slices = 60;
    InterlockedExchange(&g_rampLeft, slices);
    InterlockedExchange(&g_recentreRequested, 1);
}

void RecentreHead() {
    // Arm the accumulator watch first, so the log carries the BEFORE state of
    // both fields as well as the after. Extended again when the credit lands.
    InterlockedExchange(&g_recentreWatchUntil, (LONG)(GetTickCount() + 3000));

    // WAIT FOR THE HEAD TO BE STILL, rather than firing on the key-down.
    //
    // Pressing it mid-turn hands over an offset that includes wherever the head
    // was still travelling to - the largest and least meaningful moment to
    // measure. Luke Ross's mod waits for the same reason.
    //
    // 0 disables the wait and restores the immediate behaviour.
    if (Cfg().recentre_settle_ms > 0) {
        InterlockedExchange(&g_settleDeadline,
                            (LONG)(GetTickCount() + (DWORD)Cfg().recentre_settle_max_ms));
        InterlockedExchange(&g_settleStillSince, 0);
        InterlockedExchange(&g_settlePending, 1);
        return;
    }
    FireRecentre();
}

bool TakeRecentreRequest() {
    return InterlockedExchange(&g_recentreRequested, 0) != 0;
}

// THE RAMP. How much of what is still outstanding to hand over THIS frame.
//
// Called once per credit, immediately after the request has been taken. Returns
// 1/N on the first slice, 1/(N-1) on the next, and finally 1/1 - so the last
// slice lands exactly rather than leaving a shrinking remainder forever. Each
// slice re-arms the request, and the commit-pending guard in TryRecentreCredit
// then spaces them one frame apart on its own.
//
// The offset is re-read live every frame, so a head that moves during the ramp
// is absorbed rather than fought: the slice is always a fraction of what is
// outstanding NOW, not of what it was when the key went down.
float RecentreRampFraction() {
    LONG left = InterlockedCompareExchange(&g_rampLeft, 0, 0);
    if (left <= 1) {
        InterlockedExchange(&g_rampLeft, 0);
        return 1.0f;                 // last slice, or ramping switched off
    }
    InterlockedExchange(&g_rampLeft, left - 1);
    PutBackRecentreRequest();        // there is more to come; fire again
    return 1.0f / float(left);
}

int RecentreRampRemaining() {
    return (int)InterlockedCompareExchange(&g_rampLeft, 0, 0);
}

void PutBackRecentreRequest() { InterlockedExchange(&g_recentreRequested, 1); }

void NoteHeadOffsetApplied() {
    InterlockedExchange(&g_lastAppliedTick, (LONG)GetTickCount());
}

bool HeadOffsetIsLive() {
    const LONG t = InterlockedCompareExchange(&g_lastAppliedTick, 0, 0);
    if (!t) return false;
    return (GetTickCount() - (DWORD)t) < 250;
}

bool HeadOffsetEverApplied() {
    return InterlockedCompareExchange(&g_lastAppliedTick, 0, 0) != 0;
}

bool RecentreWatchActive() {
    const LONG t = InterlockedCompareExchange(&g_recentreWatchUntil, 0, 0);
    if (!t) return false;
    return (LONG)((DWORD)t - GetTickCount()) > 0;
}

bool CurrentRawOffset(float* yaw, float* pitch) {
    float y, p, r;
    if (!HeadAngles(&y, &p, &r)) return false;   // the SMOOTHED pose, deliberately
    if (yaw) *yaw = y;
    if (pitch) *pitch = p;
    return true;
}

bool RecentreCommitPending() {
    return InterlockedCompareExchange(&g_commitPending, 0, 0) != 0;
}

void CommitRecentre(float rawYaw, float rawPitch) {
    // SCHEDULE the advance; ApplyPendingCommit performs it at the top of the
    // next HeadTrackTick. Doing it here would drop the offset to zero midway
    // through the frame, and the aim consumer is entered TWICE per frame with
    // two alternating camera objects - so the second eye would find nothing to
    // apply and render one whole offset away from the first.
    g_commitYaw = rawYaw;
    g_commitPitch = rawPitch;
    InterlockedExchange(&g_commitPending, 1);
    // Keep the accumulator watch running past the credit, so the log shows what
    // the engine did with the value on the frames AFTER it was written - which
    // is the only place its own clamp can show up.
    InterlockedExchange(&g_recentreWatchUntil, (LONG)(GetTickCount() + 3000));
}

// The other half of CommitRecentre, one frame later. ADVANCE, never re-latch:
// re-latching from a fresh AnglesFromMatrix would sample a different head pose
// from the one that was credited, and that difference IS the jump this whole
// mechanism exists to remove.
static void ApplyPendingCommit() {
    if (!InterlockedExchange(&g_commitPending, 0)) return;
    const float ry = g_commitYaw, rp = g_commitPitch;
    g_yawRef = WrapPi(g_yawRef + ry);
    g_pitchRef = g_pitchRef + rp;            // bounded by asinf, no wrap
    // ROLL IS NEVER REFERENCED. It has a gravity datum (OpenXR's Y really is up)
    // and no engine-side accumulator to credit an offset to, so a roll reference
    // is not merely unnecessary - it bakes whatever cant the head happened to
    // have into the horizon, permanently, with nothing able to clear it.
    g_rollRef = 0.0f;
    // Position is the one part of a press that can still move the picture: this
    // feed drives the game's own Extended View, so re-anchoring while the player
    // is leaning slides the view back by the lean. Kept on by default because it
    // is what the mod already did and what puts the body under the head, but it
    // is a switch now rather than an unmentioned side effect.
    if (Cfg().recentre_position) g_havePosRef = false;
    if (Cfg().recentre_reset_smoother) InterlockedExchange(&g_steadyReset, 1);
    COTW_LOG("[head] recentre settled: reference advanced by yaw %+.2f pitch %+.2f "
             "deg, so your head offset is now zero and the picture is unchanged",
             ry * 57.2957795f, rp * 57.2957795f);
}

void HeadTrackTick() {
    // A credit was written into the engine's accumulator last frame and the
    // matching reference advance was held back until now, on purpose. See
    // CommitRecentre: doing both inside one frame splits the eyes.
    ApplyPendingCommit();

    // Roll is pinned while compensation is on, EVERY tick and not only on a
    // press. Otherwise a cant latched by the old path (master switch off) sits
    // in g_rollRef until the next compensated press discharges it in one frame,
    // rolling the horizon on the one press advertised as moving nothing.
    if (Cfg().recentre_compensate && g_rollRef != 0.0f) {
        COTW_LOG("[head] roll reference cleared (%.1f deg). Roll has no engine "
                 "accumulator to credit, so it is never referenced while "
                 "recentring keeps the picture still.",
                 g_rollRef * 57.2957795f);
        g_rollRef = 0.0f;
    }


    // *** THE RECENTRE TRIGGER: a bindable key, OR a held pad chord. ***
    //
    // Through BindingDown, NEVER GetAsyncKeyState. The raw-input hook rewrites
    // swallowed keys to VKey 0xFF and a swallowed key never reaches the async
    // state, so the old VK_HOME poll was one hook away from going deaf.
    // BindingDown also buys pad buttons for free (kPadBind | wButtons).
    //
    // *** POLLED EVERY TICK, INCLUDING WHILE THE PANEL IS LISTENING. ***
    //
    // Skipping the poll during a capture was not a gate, only a one-frame delay.
    // Capture ends on the key-DOWN that binds, with that key still physically
    // held, so wasDown was left stale at false and the very next frame read a
    // rising edge: a recentre fired by the act of binding the key. Tracking the
    // CODE as well catches the other half - the binding itself changes on that
    // frame, so the new code has no history at all and any held key reads as a
    // fresh press. In both cases the answer is to adopt the current state as the
    // baseline and require a real release first.
    {
        static int lastCode = -1;
        static bool wasDown = false;
        const bool capturing = BindingCaptureActive();
        const int code = Cfg().recentre_key;
        const bool down = BindingDown(code);
        if (capturing || code != lastCode) {
            wasDown = down;                  // adopt; never fire on a rebind
        } else {
            if (down && !wasDown) {
                RecentreHead();
                COTW_LOG("[head] recentre requested (key 0x%X)", code);
            }
            wasDown = down;
        }
        lastCode = code;
    }

    // THE PAD CHORD. Reaching for a keyboard in a headset is the whole problem,
    // but every bare pad binding tried in this project collided with something
    // the game uses. Both sticks clicked AND held is not a gameplay action, and
    // the hold is its own debounce and its own settling window. Fires ONCE per
    // chord; re-arms only on full release.
    //
    // Behind the MASTER SWITCH as well as its own enable, and behind head
    // tracking. An accidental chord is harmless only while the press does not
    // move the picture; with compensation off it would snap the view to the
    // horizon mid-hunt, from a gesture yesterday's build did not have.
    {
        static DWORD chordSince = 0;
        static bool chordFired = false;
        const int chord = Cfg().recentre_pad_chord;
        const WORD mask = WORD(chord);
        const WORD pads = g_overlay.PadButtons();
        const bool armed = chord != 0 && Cfg().recentre_pad_enable &&
                           Cfg().recentre_compensate && Cfg().head_tracking &&
                           !BindingCaptureActive();
        if (!armed) {
            chordSince = 0;
            // A chord already down when the panel opened, or when the switch was
            // flipped, must not fire the instant it re-arms.
            chordFired = mask != 0 && (pads & mask) != 0;
        } else if ((pads & mask) != mask) {
            chordSince = 0;
            if ((pads & mask) == 0) chordFired = false;   // fully released: re-arm
        } else {
            // Clamped at the point of use, not only in the panel row: 0 from the
            // ini fires on the first frame both sticks are down, and a negative
            // value casts to ~4.29e9 ms and can never fire at all - silently.
            int holdMs = Cfg().recentre_pad_hold_ms;
            if (holdMs < 50) holdMs = 50;
            if (holdMs > 5000) holdMs = 5000;
            const DWORD now = GetTickCount();
            if (!chordSince) chordSince = now;
            if (!chordFired && (now - chordSince) >= (DWORD)holdMs) {
                chordFired = true;
                RecentreHead();
                COTW_LOG("[head] recentre requested (pad chord 0x%04X held %d ms)",
                         chord, holdMs);
            }
        }
    }

    // Free look: held, not toggled. A toggle leaves the player unsure which
    // mode they are in, and the whole point is a momentary glance.
    const int key = Cfg().freelook_key;
    // Through BindingDown so free-look can be a PAD button too - the keyboard
    // test alone could never see one.
    const bool held = BindingDown(key);
    if (held != (g_freeLook != 0)) {
        InterlockedExchange(&g_freeLook, held ? 1 : 0);
        COTW_LOG("[head] free look %s", held ? "ON - the weapon stays put"
                                             : "off - the weapon follows again");
    }

    XrQuaternionf q;
    if (!VR().HeadPose(&q, nullptr)) return;

    float m[9];
    QuatToMatrix(q, m);

    if (g_recentreRequested) {
        if (Cfg().recentre_compensate) {
            // *** DO NOT LATCH HERE. ***
            //
            // The mod writes an ABSOLUTE angle, V = B - O. Latching ref = head
            // drops O to zero and throws V to B, which for a standing player is
            // the horizon - the owner's exact report. Carrying the offset
            // mod-side instead is a NO-OP (the mapping comes out byte-identical;
            // RECENTRE_DESIGN 2.2). The offset must be CREDITED to the engine's
            // own accumulator - so the request is left PENDING for
            // camera_probe.cpp, and taken there with an interlocked exchange so
            // only one of the two alternating eye-pass cameras performs it.
            // BOTH apply sites can perform it: the aim consumer when the weapon
            // is coupled, WriteYawField when it is not.
            //
            // TWO EXCEPTIONS, and both have to be named rather than inferred.
            //
            // 1. NO CREDIT SITE EXISTS. head_matrix_rotate and head_view_basis
            //    put the offset into the view MATRIX, downstream of every field
            //    this mod can credit; the axis test replaces the head offset
            //    with a sine sweep that has nothing to do with where the head
            //    is, so crediting it would displace the player's aim by a random
            //    sample of a wave. In all three a compensated press is
            //    impossible - so latch, and SAY SO, instead of waiting forever
            //    for a credit that cannot come or claiming a stillness that did
            //    not happen.
            //
            // 2. NOTHING IS REACHING THE PICTURE. With no offset in the picture
            //    there is nothing to preserve, so a plain latch is already
            //    continuous. But "no apply in the last 250 ms" is NOT that test
            //    on its own: with head_undo_mode = 0 the offset is left in the
            //    field, so when applies merely PAUSE - a map, an inventory, a
            //    loading hitch - the picture goes on showing B - O while the
            //    timestamp goes stale, and latching there is the original bug
            //    with extra steps. So the staleness has to be joined by a reason
            //    the offset cannot be outstanding: nothing was ever applied (the
            //    startup auto-recentre), the writes are switched off, or the
            //    offset is measurably ~zero. Otherwise stay pending and credit
            //    it properly the moment the game starts applying again.
            const bool noCreditSite = Cfg().head_matrix_rotate ||
                                      Cfg().head_view_basis ||
                                      Cfg().head_test_axis > 0;
            bool tinyOffset = false;
            {
                float ry = 0.0f, rp = 0.0f;
                if (CurrentRawOffset(&ry, &rp)) {
                    tinyOffset = fabsf(ry) < 0.0017f && fabsf(rp) < 0.0017f;  // 0.1 deg
                }
            }
            const bool nothingOutstanding =
                !HeadOffsetIsLive() &&
                (!HeadOffsetEverApplied() || !Cfg().head_tracking ||
                 !(Cfg().head_write_yaw || Cfg().head_write_pitch) || tinyOffset);

            if ((noCreditSite || nothingOutstanding) && TakeRecentreRequest()) {
                AnglesFromMatrix(m, &g_yawRef, &g_pitchRef, &g_rollRef);
                g_rollRef = 0.0f;   // roll has no accumulator; never referenced
                if (Cfg().recentre_position) g_havePosRef = false;
                if (noCreditSite) {
                    COTW_LOG("[head] recentred by LATCHING, and the picture WILL have "
                             "moved: %s has no engine accumulator to credit, so "
                             "compensation is not possible in it. (yaw %.1f, pitch "
                             "%.1f deg)",
                             Cfg().head_test_axis > 0 ? "the axis test"
                                                      : "matrix / view-basis mode",
                             g_yawRef * 57.2957795f, g_pitchRef * 57.2957795f);
                } else {
                    COTW_LOG("[head] recentred with no offset outstanding - latched "
                             "(yaw %.1f, pitch %.1f deg)",
                             g_yawRef * 57.2957795f, g_pitchRef * 57.2957795f);
                }
            } else {
                // "Did not run" and "ran and did not help" look identical from
                // inside a headset. Say which, once a second.
                static DWORD lastSaid = 0;
                const DWORD t = GetTickCount();
                if (t - lastSaid > 1000) {
                    lastSaid = t;
                    COTW_LOG("[head] recentre pending - an offset IS in the picture, "
                             "waiting for the site that put it there (%s) to credit "
                             "it to the engine. If this repeats for more than a "
                             "second or two the aim object was never seen.",
                             HeadOffsetIsLive() ? "applying now" : "applies paused");
                }
            }
        } else {
            // TODAY'S BEHAVIOUR, KEPT VERBATIM so that "off" is provably off.
            // The servo failed precisely because it rewired shared paths that
            // were not behind its flag.
            InterlockedExchange(&g_recentreRequested, 0);
            // The reference is stored as ANGLES, taken in the world frame. Storing
            // it as a quaternion and subtracting rotations is what coupled yaw into
            // pitch, because that measures the head inside a frame the player's own
            // posture had already tilted.
            AnglesFromMatrix(m, &g_yawRef, &g_pitchRef, &g_rollRef);
            g_havePosRef = false;      // re-anchor position at the same moment
            COTW_LOG("[head] recentre reference set (yaw %.1f, pitch %.1f, roll %.1f deg)",
                     g_yawRef * 57.2957795f, g_pitchRef * 57.2957795f,
                     g_rollRef * 57.2957795f);
        }
    }

    // THE SETTLE GATE, measured from the RAW pose, before smoothing.
    //
    // Smoothing would flatter the speed and let a press through while the head
    // was still swinging - the smoother's whole job is to hide small motion, and
    // this needs to see it. Angle between this frame's forward and last
    // frame's, over the real elapsed time.
    {
        const float fwd[3] = {-m[2], -m[5], -m[8]};
        const ULONGLONG nowTick = GetTickCount64();
        float degPerSec = 0.0f;
        if (g_havePrevFwd) {
            float d = fwd[0] * g_prevFwd[0] + fwd[1] * g_prevFwd[1] + fwd[2] * g_prevFwd[2];
            if (d > 1.0f) d = 1.0f;
            if (d < -1.0f) d = -1.0f;
            const float dtSec = float(nowTick - g_prevFwdTick) * 0.001f;
            if (dtSec > 0.0005f) degPerSec = (acosf(d) * 57.2957795f) / dtSec;
        }
        g_prevFwd[0] = fwd[0]; g_prevFwd[1] = fwd[1]; g_prevFwd[2] = fwd[2];
        g_prevFwdTick = nowTick;
        g_havePrevFwd = true;

        if (InterlockedCompareExchange(&g_settlePending, 0, 0)) {
            const DWORD now = GetTickCount();
            const bool still = degPerSec <= Cfg().recentre_settle_deg_s;
            if (!still) {
                InterlockedExchange(&g_settleStillSince, 0);
            } else if (!InterlockedCompareExchange(&g_settleStillSince, 0, 0)) {
                InterlockedExchange(&g_settleStillSince, (LONG)now);
            }
            const LONG since = InterlockedCompareExchange(&g_settleStillSince, 0, 0);
            const bool longEnough =
                since && (now - (DWORD)since) >= (DWORD)Cfg().recentre_settle_ms;
            const bool outOfTime =
                (LONG)(now - (DWORD)InterlockedCompareExchange(&g_settleDeadline, 0, 0)) >= 0;
            if (longEnough || outOfTime) {
                InterlockedExchange(&g_settlePending, 0);
                COTW_LOG("[head] recentre firing - head %s (%.1f deg/s)%s",
                         longEnough ? "settled" : "never settled, deadline reached",
                         degPerSec, outOfTime && !longEnough ? "; consider a higher "
                         "recentre_settle_deg_s if this happens every press" : "");
                FireRecentre();
            }
        }
    }

    SteadyAim(m);

    memcpy(g_head, m, sizeof(g_head));
    InterlockedExchange(&g_haveHead, 1);

    // *** ANCHOR THE STANDING POSITION WHEN THE WORLD APPEARS. ***
    //
    // The 6DoF origin used to be taken on its first use, which is whenever the
    // mod first asks - in a menu, on a loading screen, with the headset possibly
    // still on the desk. The player then sits down to play and every difference
    // between those two head positions is applied to the camera for the rest of
    // the session: the owner's "whenever i enter the game i have to press
    // recenter, the camera will be above a little bit", which is precisely a
    // reference captured with the head higher than where it settles.
    //
    // The world appearing is the moment the player is in position, and it is the
    // moment they were pressing recentre by hand. So it re-anchors itself there.
    // Only the POSITION - the direction they are facing is theirs to choose, and
    // re-latching that would move the picture.
    {
        static bool worldWas = false;
        const bool worldNow = PlayerCameraActive();
        if (worldNow && !worldWas && Cfg().six_dof_anchor_on_enter) {
            g_havePosRef = false;
            COTW_LOG("[head] the world appeared - re-anchoring the 6DoF standing "
                     "position here, so entering the game does not need a recentre");
        }
        worldWas = worldNow;
    }

    // The pose has just been republished - stamp the frame with it here, so
    // every consumer in the frame that follows sees this one value.
    HeadLatchFrameImpl();
}

// Head position relative to the recentre reference, in MILLIMETRES with Tobii's
// convention (x right, y up, z away from the screen toward the user). Extended
// View is largely position-driven, so a constant position means a motionless
// view no matter how good the rotation is.
bool HeadPositionMM(float* xyz) {
    if (!xyz) return false;
    XrVector3f p;
    if (!VR().HeadPose(nullptr, &p)) return false;
    if (!g_havePosRef) {
        g_posRef = p;
        g_havePosRef = true;
    }
    // A Tobii user shifts a few centimetres at a desk; a VR player walks around
    // a room. Feeding raw roomscale millimetres put the head hundreds of mm
    // outside the track box, where Extended View can only treat it as out of
    // range. Scale the movement down and clamp it inside the box we reported.
    const float kScale = 0.25f;
    const float kHalfX = 140.0f, kHalfY = 90.0f;   // just inside the 150/100 box
    const float kZMid = 600.0f, kZHalf = 130.0f;

    auto clamp = [](float v, float lo, float hi) {
        return v < lo ? lo : (v > hi ? hi : v);
    };

    xyz[0] = clamp((p.x - g_posRef.x) * 1000.0f * kScale, -kHalfX, kHalfX);
    xyz[1] = clamp((p.y - g_posRef.y) * 1000.0f * kScale, -kHalfY, kHalfY);
    // Tobii reports distance from the tracker; leaning in reduces it.
    xyz[2] = clamp(kZMid - (p.z - g_posRef.z) * 1000.0f * kScale,
                   kZMid - kZHalf, kZMid + kZHalf);
    return true;
}

// *** THE HEAD'S POSITION, HONESTLY, IN METRES. ***
//
// HeadPositionMM above exists to feed Extended View's Tobii lever: it scales the
// movement to a quarter, clamps it into a desk-sized box and re-bases z around
// 600 mm, because that is what a face tracker reports. None of that is what 6DoF
// wants - it wants the real distance the head really moved, in the units the
// game's world uses, with recentring as the only reference point.
//
// Axes are OpenXR's LOCAL space as the runtime reports it: +x right, +y up,
// -z forward. The caller maps those onto the game's basis rows; it is the only
// place that knows which row is which.
bool HeadPositionMetres(float* xyz) {
    if (!xyz) return false;
    XrVector3f p;
    if (!VR().HeadPose(nullptr, &p)) return false;
    if (!g_havePosRef) {
        g_posRef = p;
        g_havePosRef = true;
    }
    xyz[0] = p.x - g_posRef.x;
    xyz[1] = p.y - g_posRef.y;
    xyz[2] = p.z - g_posRef.z;
    return true;
}

// *** THE HEAD'S ABSOLUTE YAW IN THE ROOM - NOT relative to the recentre. ***
//
// Every other angle in this file is measured from the recentre, because the
// question is always "how far has the player turned since". Position is the
// exception, and getting that wrong cost three headset sessions:
//
// The head's POSITION is reported in the room's own axes, and those axes do not
// move when the player recentres. So converting a room offset into the game
// world needs the FULL angle between the room's zero and the world's - which is
// the head's absolute yaw, not what is left of it after the recentre reference
// has been subtracted. Subtracting it leaves the mapping wrong by exactly the
// yaw the player happened to be facing when they last recentred: a constant
// error, invisible if they recentred square to the room, and different every
// time they recentre otherwise. "Leaning left does not ALWAYS lean left" is
// what that sounds like from inside the headset.
//
// No inversion switch and no axis test either: those belong to the game's yaw
// FIELD, not to the room.
bool HeadPhysicalYaw(float* yaw) {
    if (!yaw) return false;
    if (!g_haveHead) return false;
    float h[9];
    memcpy(h, g_head, sizeof(h));
    float y = 0.0f, p = 0.0f, r = 0.0f;
    AnglesFromMatrix(h, &y, &p, &r);
    *yaw = y;                    // absolute, deliberately not minus g_yawRef
    return true;
}

// *** THE SAME ROTATION, DONE WITH THE MATRIX INSTEAD OF AN ANGLE. ***
//
// Rotating a room vector into the head's own horizontal frame is one line of
// trigonometry, and that line has now been written wrong twice - because the
// angle from AnglesFromMatrix is NOT a right-handed rotation about +Y. It reads
// `atan2f(fx, -fz)`, so a head facing +X (turned to the player's right) reports
// +90 degrees, while a right-handed rotation about +Y by +90 degrees would face
// -X. Undoing with the textbook matrix therefore DOUBLES the rotation instead of
// cancelling it - and a doubled rotation is correct in exactly two places, 0 and
// 180 degrees apart, which is precisely what the owner reported: "leaning left
// works only in two opposite directions".
//
// So this does not use an angle at all. The head matrix already contains the
// basis vectors - forward is the -Z column, and right is forward x up - and
// projecting onto them cannot pick up a sign convention because there is no
// convention left to pick up. Flattening them first is what makes it a yaw-only
// frame, which is the whole intent: a lean is horizontal.
//
// Output is in the head's frame, in OpenXR's own axes: +x to the player's
// right, +y up, +z BEHIND them (so forward is -z, matching the input).
bool HeadRoomToBody(const float* room, float* out) {
    if (!room || !out || !g_haveHead) return false;
    float h[9];
    memcpy(h, g_head, sizeof(h));

    // Forward = -Z column, flattened to the horizontal.
    float fx = -h[2], fz = -h[8];
    float len = sqrtf(fx * fx + fz * fz);
    if (len < 1e-4f) {
        // Looking straight up or straight down: forward has no horizontal part,
        // but the top of the head then points along the room's horizontal, so
        // the up column carries the facing instead.
        fx = h[1];
        fz = h[7];
        len = sqrtf(fx * fx + fz * fz);
        if (len < 1e-4f) return false;
    }
    fx /= len;
    fz /= len;

    // right = forward x up, with up = (0,1,0): (fx,0,fz) x (0,1,0) = (-fz,0,fx).
    const float rx = -fz, rz = fx;

    out[0] = room[0] * rx + room[2] * rz;        // how far to the player's right
    out[1] = room[1];                            // vertical is never rotated
    out[2] = -(room[0] * fx + room[2] * fz);     // -z forward, as it came in
    return true;
}

// *** ONE ROTATION, FOUR CANDIDATE ANGLES, CHOSEN BY SETTING. ***
//
// Six headset sessions have gone into deciding WHICH facing to cancel, and each
// build fixed one case and broke another. The reason is that the answer depends
// on whether the basis the offset is finally applied along turns with the head
// or only with the stick - and that is a property of engine code this mod does
// not own, cannot read directly, and has now had two contradictory readings of.
//
// So it stops being a guess. The four candidates below are the entire space:
//
//   0  recentre only      cancel a0        (basis turns with the STICK)
//   1  absolute head      cancel a         (basis turns with the HEAD)
//   2  head relative      cancel (a - a0)  (basis is room-aligned + head)
//   3  none               cancel 0         (basis is the room itself)
//
// One of them is right; the owner's report of which cases work and which fail
// identifies it in one session, and the log prints both angles so the answer can
// be read off rather than argued about.
//
// Every one of them is applied through the SAME projection, in the same
// convention AnglesFromMatrix defines (yaw = atan2(fx, -fz), so fx = sin(yaw)
// and fz = -cos(yaw)) - so switching modes cannot introduce a sign error, which
// is the other half of what these sessions have been spent on.
bool HeadRoomByYaw(const float* room, float* out, float psi) {
    if (!room || !out) return false;
    const float fx = sinf(psi);
    const float fz = -cosf(psi);
    const float rx = -fz, rz = fx;
    out[0] = room[0] * rx + room[2] * rz;
    out[1] = room[1];
    out[2] = -(room[0] * fx + room[2] * fz);
    return true;
}

// The angles themselves, for the caller to choose between and for the log.
void HeadFrameAngles(float* absYaw, float* refYaw) {
    float y = 0.0f, p = 0.0f, r = 0.0f;
    if (g_haveHead) {
        float h[9];
        memcpy(h, g_head, sizeof(h));
        AnglesFromMatrix(h, &y, &p, &r);
    }
    if (absYaw) *absYaw = y;
    if (refYaw) *refYaw = g_yawRef;
}

bool HeadRoomToRecentre(const float* room, float* out) {
    if (!room || !out) return false;

    // *** DERIVED FROM g_yawRef, NEVER COPIED FROM IT. ***
    //
    // The first version of this kept its own copy of the recentre facing,
    // stamped where the reference is latched - and MISSED that g_yawRef is also
    // ADVANCED, incrementally, in ApplyPendingCommit when the engine's own
    // accumulator is credited. So the copy went stale the moment a recentre took
    // the compensated path, the frame fell back to the room's raw axes, and
    // every lean came out rotated by the true reference angle. THREE sites
    // maintain that reference; a fourth copy of it was never going to hold.
    //
    // So it is computed here, from the reference itself, in the SAME convention
    // AnglesFromMatrix defines: yaw = atan2(fx, -fz), therefore
    // fx = sin(yaw) and fz = -cos(yaw). Self-consistent by construction - the
    // sign cannot disagree with the angle it came from, which is what every
    // hand-written rotation in this feature has managed to do at least once.
    const float fx = sinf(g_yawRef);
    const float fz = -cosf(g_yawRef);
    const float rx = -fz, rz = fx;

    out[0] = room[0] * rx + room[2] * rz;
    out[1] = room[1];
    out[2] = -(room[0] * fx + room[2] * fz);
    return true;
}

void HeadLatchFrame() { HeadLatchFrameImpl(); }

long HeadLatchServedLastSecond() {
    return (long)InterlockedExchange(&g_latchServed, 0);
}

bool HeadYawRadians(float* yaw) {
    if (!yaw || !Cfg().head_tracking) return false;
    // The axis test wins when it is on, so the yaw field can be exercised with a
    // known, obvious signal before the real head pose is trusted with it.
    if (Cfg().head_test_axis > 0) {
        *yaw = TestDegrees() * 0.01745329252f;
        return true;
    }
    float y, p, r;
    if (!HeadAngles(&y, &p, &r)) return false;
    *yaw = y;
    if (Cfg().head_invert) *yaw = -*yaw;
    return true;
}

bool HeadPitchRadians(float* pitch) {
    if (!pitch || !Cfg().head_tracking) return false;
    if (Cfg().head_test_axis > 0) {
        *pitch = TestDegrees() * 0.01745329252f;
        return true;
    }
    float y, p, r;
    if (!HeadAngles(&y, &p, &r)) return false;
    *pitch = p;
    // Its OWN invert, not the yaw's. The two axes are written to two different
    // fields with independently unknown sign conventions, and sharing one flag
    // would mean fixing pitch by breaking yaw.
    if (Cfg().head_pitch_invert) *pitch = -*pitch;
    return true;
}

bool HeadRollRadians(float* roll) {
    if (!roll || !Cfg().head_tracking) return false;
    if (Cfg().head_test_axis > 0) {
        *roll = TestDegrees() * 0.01745329252f;
        return true;
    }
    float y, p, r;
    if (!HeadAngles(&y, &p, &r)) return false;
    *roll = r;
    // Its own invert, for the same reason pitch has one: the sign convention of
    // whichever field roll lands in is not known until it is tried.
    if (Cfg().head_roll_invert) *roll = -*roll;
    return true;
}

bool HeadEulerDegrees(float* pitchYawRoll) {
    if (!pitchYawRoll) return false;
    // Same world-frame angles, relative to the recentre reference, so this
    // cannot drift away from what the view actually does. Tobii reports head
    // rotation as Euler angles in DEGREES, which is what Extended View consumes.
    float yaw, pitch, roll;
    if (!HeadAngles(&yaw, &pitch, &roll)) return false;

    const float kDeg = 57.2957795f;
    pitchYawRoll[0] = pitch * kDeg;
    pitchYawRoll[1] = yaw * kDeg;
    pitchYawRoll[2] = roll * kDeg;
    return true;
}

// RotateForwardVector and ApplyHeadRotation used to live here. Both are gone:
// they wrote DERIVED OUTPUT (the builder's basis rows) or an argument that was
// measured to be a constant, so neither could ever turn the view - while both
// still disturbed the player. The view is steered by the engine's own angle
// fields instead; see HeadYawRadians / HeadPitchRadians.

}  // namespace cotwvr
