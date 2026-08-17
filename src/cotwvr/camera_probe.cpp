#include "camera_probe.h"

#include "apex.h"
#include "config.h"
#include "framescan.h"
#include "headtrack.h"
#include "log.h"
#include "stereo.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <MinHook.h>

#include <cmath>
#include <cstring>

namespace cotwvr {
namespace {

// The engine's transform builder, both overloads:
//   void* __fastcall Build(Matrix4* out /*rcx*/, const float3* pos /*rdx*/,
//                          const float3* dir /*r8*/)
// Writes a row-major 4x4 into `out`: rows 0..2 = basis (w=0), row 3 = position
// copied verbatim from `pos` (w=1.0f).  Returns `out`.
// FOUR arguments, read off the camera's own call site at 0x006445AF:
//     lea r9,  [rbp+0xbc]     <- 4th: a float3
//     lea r8,  [rbp+0xc8]     <- 3rd: a float3, measured as (0,1,0) = UP
//     lea rdx, [rbp+0xb0]     <- 2nd: position (copied from cameraObj+0x30)
//     mov rcx, r13            <- 1st: the camera object / output
//
// Declaring only three let the compiler clobber R9 before the pass-through, so
// the ORIGINAL received a garbage fourth argument on every camera call. That is
// the third time today that not counting the argument registers has cost a
// crash - read the CALL SITE, not just the prologue, because the prologue only
// shows what the callee happens to touch early.
using PFN_Build = void*(__fastcall*)(void*, const float*, const float*, const float*);

PFN_Build o_BuildA = nullptr;
PFN_Build o_BuildB = nullptr;

constexpr int kMaxSites = 96;

struct Site {
    volatile LONG claimed;      // 0 = free, 1 = being claimed, 2 = owned
    uint32_t callerRva;
    int which;                  // 0 = A, 1 = B
    volatile LONG countTotal;
    volatile LONG countFrame;

    // Last observed matrix. Racy by design - a probe wants a recent sample, not
    // a consistent one, and a lock here would be exactly the mistake that makes
    // a hot hook unusable.
    float m[16];
    float prevPos[3];
    float movedAccum;           // how far the position has travelled
    float basisSwingAccum;      // how much the forward axis has swung
};

Site g_sites[kMaxSites];
volatile LONG g_siteCount = 0;
uintptr_t g_selfLo = 0, g_selfHi = 0;

Site* ClaimSite(uint32_t rva, int which) {
    // Linear scan: kMaxSites is small and this runs in a hot path, so no
    // allocation and no hashing.
    const LONG n = g_siteCount;
    for (LONG i = 0; i < n && i < kMaxSites; ++i) {
        if (g_sites[i].claimed == 2 && g_sites[i].callerRva == rva &&
            g_sites[i].which == which) {
            return &g_sites[i];
        }
    }
    if (n >= kMaxSites) return nullptr;
    const LONG idx = InterlockedIncrement(&g_siteCount) - 1;
    if (idx >= kMaxSites) return nullptr;
    Site* s = &g_sites[idx];
    s->callerRva = rva;
    s->which = which;
    s->movedAccum = 0.0f;
    s->basisSwingAccum = 0.0f;
    InterlockedExchange(&s->claimed, 2);
    return s;
}

// Snapshot of the player camera's matrix, taken in the hook and printed later.
float g_camMatrix[16];
volatile LONG g_camSeen = 0;

// Which call site currently receives the test offset.
//   -2 = none, -1 = EVERY site, >=0 = index into g_sites
// Cycled live with F8 so one game session can test every candidate, instead of
// one relaunch (and one save reload) per candidate.
volatile LONG g_armed = -2;

// The input-vs-derived test (playbook section 9.2), and the mechanism stereo
// will use.  Writes a large, obvious offset along one basis row.  If the view
// moves, the position is a real INPUT and stereo is a matter of changing the
// sign per eye.  If nothing happens, this matrix is derived output.
void ApplyTestOffset(float* m) {
    const int axis = Cfg().camera_test_axis;
    if (axis < 1 || axis > 3) return;
    const float amt = Cfg().camera_test_amount;
    const int r = (axis - 1) * 4;      // row 0, 1 or 2 of the row-major basis
    m[12] += m[r + 0] * amt;
    m[13] += m[r + 1] * amt;
    m[14] += m[r + 2] * amt;
}

void ProbeCameraObject(const void* obj, const float* outMatrix);
void ProbeLeverObject(const void* obj, const float* outMatrix);

// Drive the engine's OWN yaw field instead of rewriting the matrix it derives.
//
// The field was identified by correlation, not by guessing: +0x4C of the lever's
// object fits "yaw in radians" with r = -1.000 and slope -1.0000, while the four
// matrix components around it fit cos/sin/-sin/cos exactly as a Y-up rotation
// should. The negative slope is why the offset is SUBTRACTED - the field stores
// the negation of the heading measured from row 0.
//
// Whether it is a real INPUT or just derived state parked in the object is the
// question this answers: read what the engine put there, add our offset, write
// it back, and report whether the engine overwrote us next frame.
// *** THE WRITE HAS TO BE IDEMPOTENT, NOT INCREMENTAL. ***
//
// `*f = engineValue - delta` is only safe while the engine RECOMPUTES the field
// every frame. It normally does - that was measured and logged. But crouching
// takes a path where it does NOT, and then engineValue is simply what WE left
// there last frame, so the offset is subtracted again, and again. The player's
// report is the exact shape of a compounding subtraction: "each time I crouch
// and stand the view keeps going up until eventually the character was looking
// up".
//
// The fix is to remember the BASE we last derived from, not only what we wrote.
// If the field comes back exactly as we left it the engine has not touched it,
// so the base still stands and the offset is applied to that base rather than
// to our own output. The write then lands in the same place however many times
// it runs, and cannot drift.
struct AngleWrite {
    const char* tag;
    float lastWritten;
    bool  haveLast;
    uint64_t frames;
    float lastBase;      // the engine value the last write was derived FROM
    uint64_t kept;       // frames the engine did not recompute - drift risk
};
AngleWrite g_yawWrite{"yaw", 0.0f, false, 0};
AngleWrite g_pitchWrite{"pitch", 0.0f, false, 0};

// When the player camera last drew. Full-rate must not engage in the main menu:
// there is no player camera there, the menu runs on its own timing, and
// replaying its frame put the menu into slow motion.
volatile LONG g_leverTick = 0;
void NoteLeverAlive() { InterlockedExchange(&g_leverTick, (LONG)GetTickCount()); }

void WriteAngleField(void* obj, int off, float delta, AngleWrite& w) {
    // Must be past the 0x40-byte matrix, aligned, and inside what is readable.
    if (!obj || off < 0x40 || off > 0x5F0 || (off & 3)) return;

    float* f = reinterpret_cast<float*>(static_cast<char*>(obj) + off);

    __try {
        const float engineValue = *f;

        // Did the engine recompute this field since our last write? If it comes
        // back exactly as we left it, it did not - and the value in front of us
        // is our OWN output, which must never be used as the base again.
        const bool recomputed =
            !w.haveLast || fabsf(engineValue - w.lastWritten) > 1e-6f;
        const float base = recomputed ? engineValue : w.lastBase;
        if (!recomputed) ++w.kept;

        if (w.haveLast && (++w.frames % 600) == 1) {
            COTW_LOG("[%s] field +0x%X: engine has %.4f rad, we left %.4f -> the engine "
                     "%s it. Offset %+.1f deg. Frames the engine did NOT recompute: "
                     "%llu (those would drift if the base were not held)",
                     w.tag, off, engineValue, w.lastWritten,
                     recomputed ? "RECOMPUTED" : "KEPT", delta * 57.2957795f,
                     (unsigned long long)w.kept);
        }

        *f = base - delta;
        w.lastBase = base;
        w.lastWritten = *f;
        w.haveLast = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
}

// *** WHICH FIELD DOES THE WEAPON'S HEADING COME FROM? ***
//
// The symptom: head tracking turns the VIEW, but the weapon keeps pointing
// where the body is aimed. That is the exact signature of the view being
// offset DOWNSTREAM of wherever the weapon reads its heading - we subtract the
// head yaw at +0x4C, the engine builds the view from it, and the weapon's own
// chain never sees it.
//
// THE DISCRIMINATOR, and it is exact because WE generate the signal: with head
// tracking running, every field in the VIEW chain carries our injected yaw and
// every field in the AIM chain does not. So correlate each field against the
// yaw WE INJECTED - never against the player turning, which moves both and is
// why "find the field that tracks the heading" produced 95 useless matches
// last time.
//
//    |r| near 1, and it moves      -> carries our offset: view chain, already
//                                     following the head.
//    |r| near 0, but it MOVES      -> aim chain. The weapon reads from one of
//                                     these.
//    does not move                 -> not an angle we care about.
//
// POSITIVE CONTROL: this must rediscover +0x4C as a view-chain field. It is
// printed FIRST and UNCONDITIONALLY. If the control is absent the instrument is
// broken and nothing below it means anything - two whole test cycles were lost
// to confident false negatives from probes that could not find what was already
// known to be there.
constexpr int kRotLo = 0x40;
constexpr int kRotHi = 0x600;
constexpr int kRotN  = (kRotHi - kRotLo) / 4;

struct RotStat {
    double n, sx, sy, sxx, syy, sxy;
    double motion;      // total absolute movement - separates live from dead
    float  lo, hi, last;
    bool   haveLast;
};
RotStat g_rot[kRotN];
uint64_t g_rotSamples = 0;
AngleWrite g_weaponRotWrite{"wrot", 0.0f, false, 0};

double RotCorr(const RotStat& s) {
    if (s.n < 30) return 0.0;
    const double vx = s.sxx - s.sx * s.sx / s.n;
    const double vy = s.syy - s.sy * s.sy / s.n;
    if (vx <= 1e-12 || vy <= 1e-12) return 0.0;
    return (s.sxy - s.sx * s.sy / s.n) / sqrt(vx * vy);
}

// SLOPE, not correlation - and the difference is the whole probe.
//
// The first run showed the control decaying from r=-1.000 to r=-0.228 as the
// player turned: correlation measures how much of a field's movement OUR
// signal explains, and the player's turning is far larger than his head
// movement, so it drowns the thing we are looking for. Slope does not care.
// The player's turning is INDEPENDENT of the yaw we inject, so it adds noise
// to the estimate but no bias:
//     view chain (f = engineValue - headYaw) -> slope -1, however much he turns
//     aim chain  (f never sees our offset)   -> slope 0
// The statistics are also windowed now. Cumulative sums meant a probe that
// silently got worse the longer it ran, which is the opposite of what a
// measurement should do.
double RotSlope(const RotStat& s) {
    if (s.n < 60) return 0.0;
    const double vx = s.sxx - s.sx * s.sx / s.n;
    if (vx <= 1e-9) return 0.0;
    return (s.sxy - s.sx * s.sy / s.n) / vx;
}

void ResetRotWindow() {
    for (int i = 0; i < kRotN; ++i) {
        RotStat& s = g_rot[i];
        s.n = s.sx = s.sy = s.sxx = s.syy = s.sxy = 0.0;
        s.motion = 0.0;
        s.haveLast = false;
    }
}

void ReportWeaponRotation() {
    // The control, first and always.
    const int ctlIdx = (Cfg().head_yaw_field - kRotLo) / 4;
    if (ctlIdx < 0 || ctlIdx >= kRotN) {
        COTW_LOG("[wrot] *** THE PROBE IS BROKEN: the known yaw field +0x%X is outside "
                 "the scanned range - believe nothing below. ***", Cfg().head_yaw_field);
        return;
    }
    const RotStat& ctl = g_rot[ctlIdx];
    const double ctlSlope = RotSlope(ctl);
    const bool ok = fabs(ctlSlope + 1.0) < 0.35;
    COTW_LOG("[wrot] POSITIVE CONTROL: known view yaw +0x%X has slope=%+.3f "
             "(expected -1), r=%+.3f, n=%.0f -- %s",
             Cfg().head_yaw_field, ctlSlope, RotCorr(ctl), ctl.n,
             ok ? "the probe works" :
                  "*** BROKEN - ignore everything below. Move your HEAD more; the "
                  "injected signal is too small to measure against. ***");
    if (!ok) return;

    // Everything that carries our offset. Useful in itself: it says how far
    // down the view chain the head rotation actually propagates.
    COTW_LOG("[wrot] VIEW CHAIN (slope ~ -1, already follows your head):");
    int shown = 0;
    for (int i = 0; i < kRotN && shown < 8; ++i) {
        const RotStat& s = g_rot[i];
        if (s.n < 60) continue;
        if (fabs(RotSlope(s) + 1.0) > 0.35) continue;
        COTW_LOG("[wrot]   +0x%03X  slope=%+.3f  range %+.3f..%+.3f  moved %.1f%s",
                 kRotLo + i * 4, RotSlope(s), s.lo, s.hi, s.motion,
                 (kRotLo + i * 4 == Cfg().head_yaw_field) ? "   <-- the control" : "");
        ++shown;
    }

    // The candidates. An aim heading must MOVE when the player turns, must not
    // carry our offset, and must go NEGATIVE - the last clause is what unmasked
    // six confident false positives when the pitch field was found, all of them
    // frame timing sitting entirely on one side of zero.
    COTW_LOG("[wrot] AIM CHAIN candidates (slope ~ 0: they move, but not with your "
             "head). The weapon reads from one of these:");
    bool taken[kRotN] = {};
    int listed = 0;
    for (int rank = 0; rank < 12; ++rank) {
        int best = -1;
        double bestMotion = 0.0;
        for (int i = 0; i < kRotN; ++i) {
            if (taken[i]) continue;
            const RotStat& s = g_rot[i];
            if (s.n < 60) continue;
            const float range = s.hi - s.lo;
            if (range < 0.05f || range > 8.0f) continue;
            if (s.motion < 0.5) continue;
            if (fabs(RotSlope(s)) > 0.30) continue;
            if (s.motion > bestMotion) { bestMotion = s.motion; best = i; }
        }
        if (best < 0) break;
        taken[best] = true;
        const RotStat& s = g_rot[best];
        const int off = kRotLo + best * 4;
        const bool straddles = (s.lo < -0.01f && s.hi > 0.01f);
        COTW_LOG("[wrot]   +0x%03X  slope=%+.3f  range %+.3f..%+.3f (%.0f deg)  "
                 "moved %.1f  %s%s", off, RotSlope(s), s.lo, s.hi,
                 (s.hi - s.lo) * 57.2957795f, s.motion,
                 straddles ? "STRADDLES ZERO - looks like a real look axis"
                           : "one side of zero - probably not an angle",
                 (off == Cfg().head_pitch_field) ? "   <-- our pitch field" : "");
        ++listed;
    }
    if (!listed) {
        COTW_LOG("[wrot]   none. Either the aim heading is not in this object, or it "
                 "did not move - turn right around with the stick during the run.");
    }
    COTW_LOG("[wrot] To test one: set weapon_rot_field = 0x<offset> in cotwvr.ini "
             "(no rebuild) and see whether the WEAPON turns with your head.");
}

void ProbeWeaponRotation(void* obj, float injectedYaw) {
    if (!Cfg().weapon_rot_probe || !obj) return;
    __try {
        const char* base = static_cast<const char*>(obj);
        for (int i = 0; i < kRotN; ++i) {
            const float v = *reinterpret_cast<const float*>(base + kRotLo + i * 4);
            if (!isfinite(v) || fabsf(v) > 1e6f) continue;
            RotStat& s = g_rot[i];
            if (s.n == 0) { s.lo = v; s.hi = v; }
            if (v < s.lo) s.lo = v;
            if (v > s.hi) s.hi = v;
            if (s.haveLast) s.motion += fabs((double)v - (double)s.last);
            s.last = v;
            s.haveLast = true;
            const double x = injectedYaw, y = v;
            s.n += 1; s.sx += x; s.sy += y;
            s.sxx += x * x; s.syy += y * y; s.sxy += x * y;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    // Report on a timer, not a frame count: the useful signal is the player
    // having had time to look around, which is measured in seconds.
    ++g_rotSamples;
    static DWORD lastReport = 0;
    const DWORD now = GetTickCount();
    if (!lastReport) lastReport = now;
    if (now - lastReport > 10000) {
        lastReport = now;
        ReportWeaponRotation();
        ResetRotWindow();   // each report is its own window, never a running total
    }
}

// *** MAKE THE WEAPON FOLLOW THE HEAD BY ROTATING THE LEVER'S MATRIX. ***
//
// The field probe came back almost empty: across every valid window the ONLY
// thing carrying our head yaw was +0x4C itself, and nothing else in the object
// spans a full heading (+0x4C swings -3.18..+3.08, i.e. the whole circle;
// the only candidates were +0x2A4, which never goes negative, and +0x040 at
// +/-1.5 deg). So the weapon's heading is NOT a second angle field here, and
// hunting for one would have been the next dead end.
//
// What it is instead was already in our own notes, recorded as a failure:
// rewriting these basis rows "moves the player/character but never rotates the
// view". That verdict was correct and is still correct - the engine recomputes
// the view from +0x4C and discards our rows. What it missed is that something
// else DOES read this matrix: the held weapon inherits the stereo eye offset we
// apply to it right here, which is the entire reason the viewmodel came out
// flat in the first place.
//
// So the two halves were never in conflict:
//     +0x4C          drives the VIEW
//     this matrix    drives the WEAPON (and the body)
// Writing only the first is exactly "the view turns and the gun does not".
void ApplyHeadRotationToLever(float* m) {
    if (!Cfg().head_rotate_lever || !m) return;

    float yaw = 0.0f, pitch = 0.0f;
    const bool haveYaw = Cfg().head_write_yaw && HeadYawRadians(&yaw);
    const bool havePitch = Cfg().head_rotate_lever_pitch && Cfg().head_write_pitch &&
                           HeadPitchRadians(&pitch);
    if (!haveYaw && !havePitch) return;

    // Separate signs. The values come from HeadYawRadians/HeadPitchRadians,
    // which are tuned for the CONVENTIONS OF THE ANGLE FIELDS (+0x4C stores a
    // negated heading, and head_pitch_invert is already applied). Those
    // conventions have nothing to do with the handedness of a geometric
    // rotation, so one shared sign flag cannot be right for both axes - the
    // same mistake the yaw/pitch field flags were split to avoid.
    if (Cfg().head_rotate_lever_invert) yaw = -yaw;
    if (Cfg().head_rotate_lever_pitch_invert) pitch = -pitch;
    if (!Cfg().head_rotate_lever_yaw) yaw = 0.0f;

    // Rodrigues, so the pitch can go about the view's own right axis rather
    // than a world axis - pitching about world X would roll the gun as soon as
    // the player is not facing down Z.
    auto rotate = [](float* v, const float* k, float a) {
        const float c = cosf(a), s = sinf(a);
        const float kx = k[0], ky = k[1], kz = k[2];
        const float vx = v[0], vy = v[1], vz = v[2];
        const float cx = ky * vz - kz * vy;
        const float cy = kz * vx - kx * vz;
        const float cz = kx * vy - ky * vx;
        const float d = kx * vx + ky * vy + kz * vz;
        v[0] = vx * c + cx * s + kx * d * (1.0f - c);
        v[1] = vy * c + cy * s + ky * d * (1.0f - c);
        v[2] = vz * c + cz * s + kz * d * (1.0f - c);
    };

    // Row 2 is the RIGHT axis - measured, not assumed (row1 is up and never
    // moves, row0 and row2 swing 145 deg together through a look-around).
    //
    // NORMALISED, because Rodrigues is only a rotation for a UNIT axis. With a
    // scaled axis the k*(k.v)*(1-cos) term stops cancelling and leaks a
    // displacement along k that is EVEN in the angle - which is exactly the
    // reported symptom: head up and head down both pushed the weapon the same
    // way vertically while the horizontal component flipped sign. An
    // even-in-angle response is not something a rotation can produce, so it
    // named the bug on its own.
    float right[3] = {m[8], m[9], m[10]};
    const float rlen = sqrtf(right[0] * right[0] + right[1] * right[1] +
                             right[2] * right[2]);
    if (rlen < 1e-4f) return;            // degenerate basis: do nothing at all
    right[0] /= rlen; right[1] /= rlen; right[2] /= rlen;
    const float up[3] = {0.0f, 1.0f, 0.0f};

    for (int r = 0; r < 3; ++r) {
        float* row = m + r * 4;
        if (havePitch && pitch != 0.0f) rotate(row, right, pitch);
        if (haveYaw && yaw != 0.0f)     rotate(row, up, yaw);
    }

    // One line, rarely, so the axis scale is on the record rather than assumed
    // a second time.
    static uint64_t n = 0;
    if ((++n % 1200) == 1) {
        COTW_LOG("[hrot] lever basis right-axis length %.4f (1.0 = clean rotation), "
                 "yaw %+.1f deg, pitch %+.1f deg", rlen, yaw * 57.2957795f,
                 pitch * 57.2957795f);
    }
}

// *** THE EARLY WRITE: AHEAD OF THE WEAPON INSTEAD OF BEHIND IT. ***
//
// kAimConsumer is called at 0x0064424E with the camera object in r8, straight
// after the aim is built into camera+0x4C and BEFORE anything downstream reads
// it. Applying the head offset here puts it ahead of the weapon placement and
// the view alike, which is the one thing the old write site could never do -
// it ran after the weapon had already been positioned.
//
// FIVE arguments, counted at the CALL SITE (rcx, rdx, r8, r9d, one stack
// float). The prologue would not have shown the fifth, and a missing argument
// lets the compiler clobber the register the original still needs - the exact
// mistake that crashed this game four times.
using PFN_AimConsumer = void(__fastcall*)(void*, void*, void*, unsigned int, float);
PFN_AimConsumer o_AimConsumer = nullptr;
AngleWrite g_earlyYaw{"eyaw", 0.0f, false, 0};
AngleWrite g_earlyPitch{"epitch", 0.0f, false, 0};
volatile LONG g_earlyHits = 0;

// COUPLED vs DECOUPLED, and free look, are one decision: does the offset go in
// here (before the weapon is placed) or later at the transform builder (after
// it)? The view lands in the same place either way.
bool CoupledNow() {
    if (!Cfg().head_early_write) return false;   // decoupled by choice
    if (FreeLookHeld()) return false;            // decoupled for this moment
    return true;
}

// Watch for the game moving the view by itself - crouching was reported to feel
// like a recentre.
//
// TWO BUGS OF MY OWN LIVED HERE, and the first one wrecked a play session:
//
// 1. It logged EVERY change of camera pointer. There are exactly TWO camera
//    objects and they alternate every pass, because full-rate stereo renders
//    the frame twice - so "the object changed" is the normal state of affairs,
//    not a discovery. That was ~2 log writes per frame, 7,257 lines and 1.2 MB
//    in 35 seconds, file I/O on the render thread. The judder the player saw
//    was the DIAGNOSTIC, not the game. A watchdog that costs more than the
//    fault it watches for is worse than none.
// 2. It compared each sample against the previous one while those samples came
//    from ALTERNATING objects, so it was diffing eye 0 against eye 1. It read
//    zero jumps, but only because the two eyes happen to share an aim.
//
// Both fixed: a tiny table keeps per-object history, and only genuinely new
// pointers are named, once each.
struct CamWatch {
    void* obj;
    float yaw, pitch;
    bool  have;
    float appliedYaw, appliedPitch;   // what we offset this object by last time
    DWORD blockCorrectionUntil;       // one correction per event, see below

    // What we left in the field, and the engine value it was derived from.
    // These are what make "did the engine recompute this?" answerable PER
    // OBJECT - asking it with one shared pair of variables was meaningless,
    // because the two eye passes alternate and each would see the other's.
    float wroteYaw, wrotePitch;
    float baseYaw, basePitch;
    bool  haveWrote;
};
constexpr int kMaxCamWatch = 6;
CamWatch g_camWatch[kMaxCamWatch] = {};
volatile LONG g_camWatchCount = 0;

void NoteAimReset(void* camera) {
    if (!Cfg().head_reset_watch) return;
    CamWatch* w = nullptr;
    const LONG n = g_camWatchCount;
    for (LONG i = 0; i < n && i < kMaxCamWatch; ++i) {
        if (g_camWatch[i].obj == camera) { w = &g_camWatch[i]; break; }
    }
    if (!w) {
        if (n >= kMaxCamWatch) return;
        w = &g_camWatch[n];
        w->obj = camera;
        w->have = false;
        InterlockedIncrement(&g_camWatchCount);
        COTW_LOG("[reset] camera object #%ld: %p  (two of these is normal - one per "
                 "eye pass)", (long)n, camera);
    }
    __try {
        const char* base = static_cast<const char*>(camera);
        const float y = *reinterpret_cast<const float*>(base + apex::kAimYawOffset);
        const float p = *reinterpret_cast<const float*>(base + apex::kAimPitchOffset);
        // *** PUT BACK WHAT THE ENGINE SWALLOWED. ***
        //
        // Measured, not assumed: the consumer never touches this field (the
        // [aimfx] probe logged nothing at all, which is its "callee left it
        // alone" case), so the undo is exact and the remaining leak is the
        // engine ADOPTING the offset while it is applied - on crouch it
        // re-anchors from whatever is in the field.
        //
        // That leak has an unmistakable signature: between one pass of an eye
        // and the next pass of the SAME eye, the value moves by exactly minus
        // the offset we applied. A person cannot turn 42 degrees in one frame,
        // so nothing else produces it. Detect that and add the offset straight
        // back into the engine's own value, and the drift cancels at the source
        // instead of being chased around the frame with timing changes.
        // ONE correction per event, then hold off.
        //
        // Crouching does not merely re-anchor once - for a moment the engine
        // stops recomputing the field at all, so EVERY pass then looks like a
        // leak (the value equals what we wrote, so the jump equals minus our
        // offset). Correcting on every one of those fought the player's own
        // turning, which is why looking left and right died for about two
        // seconds after a crouch or a jump. The re-anchor is a single event and
        // deserves a single correction; after that, stand back and let the
        // engine and the player have the field.
        const DWORD nowMs = GetTickCount();
        const bool correctionAllowed = (nowMs >= w->blockCorrectionUntil);

        // SUPERSEDED and off by default. Correcting after the theft could not
        // win either way: every frame killed the drift but fought the player's
        // turning, once per event let the drift back because the theft repeats
        // for as long as the engine stays idle. HoldBaseIfEngineIdle prevents
        // the theft instead, which is strictly better - but this is kept behind
        // its own switch because it is the fallback if "idle" ever turns out to
        // have a case the equality test misses.
        if (w->have && Cfg().head_leak_after_the_fact && correctionAllowed) {
            struct Axis { float cur; float last; float applied; uint32_t off; const char* n; };
            const Axis axes[2] = {
                {y, w->yaw, w->appliedYaw, apex::kAimYawOffset, "yaw"},
                {p, w->pitch, w->appliedPitch, apex::kAimPitchOffset, "pitch"},
            };
            for (const Axis& ax : axes) {
                if (fabsf(ax.applied) < 0.03f) continue;      // under ~2 deg: not worth it
                const float jump = ax.cur - ax.last;
                // The engine swallowed it if the value fell by our own offset,
                // within a tenth of that offset.
                if (fabsf(jump + ax.applied) < 0.10f * fabsf(ax.applied)) {
                    float* f = reinterpret_cast<float*>(
                        const_cast<char*>(base) + ax.off);
                    *f += ax.applied;
                    // Latch: no more corrections on this object for a moment, so
                    // the rest of the crouch belongs to the player.
                    w->blockCorrectionUntil = nowMs + 500;
                    static DWORD lastLog = 0;
                    if (nowMs - lastLog > 300) {
                        lastLog = nowMs;
                        COTW_LOG("[leak] the engine adopted our %s offset (%.1f deg) - "
                                 "put it back once, then holding off for 500 ms so it "
                                 "cannot fight your own turning", ax.n,
                                 ax.applied * 57.2957795f);
                    }
                }
            }
            // Re-read: the correction above may have moved them.
            const float y2 = *reinterpret_cast<const float*>(base + apex::kAimYawOffset);
            const float p2 = *reinterpret_cast<const float*>(base + apex::kAimPitchOffset);
            w->yaw = y2;
            w->pitch = p2;
        }

        if (w->have) {
            // 0.35 rad = 20 deg between two passes of the SAME eye is not a
            // person turning; it is the game moving the view.
            const float dy = fabsf(y - w->yaw), dp = fabsf(p - w->pitch);
            if ((dy > 0.35f && dy < 6.0f) || dp > 0.35f) {
                static DWORD last = 0;
                const DWORD now = GetTickCount();
                if (now - last > 250) {          // never faster than 4 lines a second
                    last = now;
                    COTW_LOG("[reset] the game moved the view: yaw %+.1f deg, pitch "
                             "%+.1f deg in one frame (yaw %.3f->%.3f, pitch "
                             "%.3f->%.3f rad)", (y - w->yaw) * 57.2957795f,
                             (p - w->pitch) * 57.2957795f, w->yaw, y, w->pitch, p);
                }
            }
        }
        w->yaw = *reinterpret_cast<const float*>(base + apex::kAimYawOffset);
        w->pitch = *reinterpret_cast<const float*>(base + apex::kAimPitchOffset);
        w->have = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// *** MAKE THE SAVED AIM CLEAN - THE ACTUAL FIX. ***
//
// The engine saves the live aim into rsi+0x5C when a scripted camera episode
// begins - crouch, stand, prone, jump, stance adjustment on a slope - and
// restores it when the episode ends. It was saving OUR offset along with it and
// handing that back as the player's real aim, so every event ratcheted the view
// by exactly the offset in force. That is what the logs measured all along.
//
// It also explains why suspending the offset DURING the event never worked: the
// save happens at the START of the episode, before anything could react to it.
//
// The copy has exactly one caller in the image, so no filtering is needed: hand
// the engine its own value for the duration of the call, let it save that, then
// re-apply. The saved aim is the player's actual aim and nothing accumulates,
// while the view and the weapon keep the offset for the whole frame exactly as
// they did when both followed correctly.
using PFN_SaveAimCopy = void*(__fastcall*)(void*, void*);
PFN_SaveAimCopy o_SaveAimCopy = nullptr;
volatile LONG g_savesCleaned = 0;

void* __fastcall Hook_SaveAimCopy(void* dst, void* src) {
    float dy = 0.0f, dp = 0.0f;
    float* f = nullptr;
    if (src && Cfg().head_tracking && Cfg().head_clean_saved_aim) {
        __try {
            float d = 0.0f;
            if (Cfg().head_write_yaw && HeadYawRadians(&d)) dy = d;
            if (Cfg().head_write_pitch && HeadPitchRadians(&d)) dp = d;
            if (dy != 0.0f || dp != 0.0f) {
                f = static_cast<float*>(src);
                f[0] += dy;      // undo our subtraction: the engine's own value
                f[1] += dp;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            f = nullptr;
        }
    }

    void* r = o_SaveAimCopy(dst, src);

    if (f) {
        __try {
            f[0] -= dy;          // put our offset back for the rest of the frame
            f[1] -= dp;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        InterlockedIncrement(&g_savesCleaned);
        static DWORD lastLog = 0;
        const DWORD now = GetTickCount();
        if (now - lastLog > 2000) {
            lastLog = now;
            COTW_LOG("[save] scripted-camera episode - the engine was handed its OWN "
                     "aim to save (yaw %+.1f, pitch %+.1f deg taken out for the copy), "
                     "so there is nothing of ours to restore afterwards. %ld saves "
                     "cleaned.", dy * 57.2957795f, dp * 57.2957795f,
                     (long)g_savesCleaned);
        }
    }
    return r;
}

// *** THE MATRIX ROUTE: ROTATE THE OUTPUT, WRITE NO ANGLE. ***
//
// Every angle-write variant drifts - early, late, or suspended during stance
// changes - so this writes none. The engine builds its view matrix from its own
// untouched angles and we rotate the RESULT. Its aim state never contains our
// offset, so there is nothing to be read back into it, whatever is doing the
// reading. That is the point: this stops guessing at the cause and removes the
// precondition for it instead.
using PFN_EulerToMatrix = void(__fastcall*)(const float*, float*);
PFN_EulerToMatrix o_EulerToMatrix = nullptr;
volatile LONG g_matrixRotations = 0;

// *** THE OTHER DIRECTION: matrix -> angle, and the one that bakes. ***
//
// Turning the view by rotating a matrix looked as though it could not be
// swallowed, because nothing of ours is stored as an angle. Three tests said
// otherwise: with head tracking OFF nothing rotates, with a ZERO head offset
// nothing rotates, and moving the view with the stick alone does not rotate
// either. So it is still our offset - arriving by a different road.
//
// This is that road. At 0x004B720B a matrix is decomposed STRAIGHT into the aim
// euler (rcx = rbx+0x4C, rdx = the matrix). It was measured earlier writing back
// bit-identical and rightly dismissed - nothing of ours was in that matrix then.
// Rotating the view basis put something of ours in it.
//

// *** THE CODE CAVE DETOUR - correcting the write instead of blocking it. ***
//
// Everything measured leads here, and nothing else survives the evidence:
//
//   * the drift lives in the look accumulator. Blocking only the aim-from-view
//     write (0x0063888E) leaves the ratchet exactly as it was.
//   * blocking the accumulator write (0x006388CC) stops the ratchet dead - and
//     causes the transient swing, because the routine writes a CONSISTENT PAIR
//     and removing one without supplying a replacement breaks the invariant.
//   * the owner's fresh-launch observation proves the mechanism: before the
//     right stick has been touched the accumulator is ~0, blocking a write of ~0
//     changes nothing, and there is no swing. The swing IS the value we prevent
//     from being written.
//
// So neither blocking nor hooking will do. Hooking the containing routine
// (0x00638730) crashed the game twice: it is float-heavy, its real signature is
// unknown, and a guessed trampoline corrupts a register the caller still needs.
//
// A code cave has none of those problems. We do not call the function, we do not
// guess a signature, and we preserve every register we touch. The instruction
// runs exactly as the engine wrote it - only the VALUE has our head offset taken
// out of it first.
//
// LAYOUT. The target is 6 bytes, a rel32 jump is 5, so one instruction is
// relocated and one NOP pads the gap. Nothing else moves.
//
//   0x006388CC  E9 xx xx xx xx 90     jmp cave; nop
//
// THE CAVE, hand-assembled below:
//   sub    rsp,0x10                 make room without touching the red zone
//   movups [rsp],xmm1               xmm1 is ours to borrow, so save it
//   movss  xmm1,[rip+corr]          our correction, updated every frame
//   subss  xmm8,xmm1                take our offset out of the value
//   movss  [rbx+0x1c],xmm8          THE ORIGINAL INSTRUCTION, byte for byte
//   addss  xmm8,xmm1                put xmm8 back - the engine still needs it
//   movups xmm1,[rsp]               restore xmm1
//   add    rsp,0x10
//   jmp    back
//
// xmm0 is deliberately untouched: the very next engine instruction writes the
// pitch accumulator from it.
//
// The correction is read from memory rather than computed in the cave, so the
// cave needs no call, no shadow space and no stack alignment. With head tracking
// off we write 0.0f there, and subtracting zero makes the cave behaviourally
// identical to the original instruction - so it is safe to leave installed.
struct ReseedCave {
    uint8_t* mem = nullptr;         // the allocated cave
    float* correction = nullptr;        // yaw correction the cave subtracts
    float* correctionPitch = nullptr;   // and pitch
    uint8_t original[16] = {};
    bool installed = false;
};
ReseedCave g_cave;

// Allocate within +/-2GB of the target, because the jump back is a rel32.
uint8_t* AllocNear(uintptr_t target, size_t size) {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    for (uintptr_t delta = gran; delta < 0x60000000ull; delta += gran) {
        for (int dir = 0; dir < 2; ++dir) {
            const uintptr_t addr = dir ? (target + delta) : (target - delta);
            if (addr < 0x10000) continue;
            void* p = VirtualAlloc(reinterpret_cast<void*>(addr & ~(gran - 1)), size,
                                   MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p) return static_cast<uint8_t*>(p);
        }
    }
    return nullptr;
}

bool InstallReseedCave() {
    if (g_cave.installed) return true;
    void* target = apex::Rva(apex::kReseedAccumWrite);
    if (!target) return false;
    const uintptr_t t = reinterpret_cast<uintptr_t>(target);

    uint8_t* cave = AllocNear(t, 0x100);
    if (!cave) {
        COTW_LOG("[cave] could not allocate within reach of 0x%X - the correction "
                 "cannot be installed", apex::kReseedAccumWrite);
        return false;
    }

    // BOTH accumulators now, not just yaw.
    //
    // At a correction of exactly -1.0 the horizontal ratchet stopped dead and only
    // the vertical remained - which is the cleanest confirmation this fix could
    // have had. A coefficient of exactly one means the quantity is right, not
    // approximately right; the accumulator was gaining precisely the head yaw.
    //
    // The vertical remainder was simply the instruction left alone: the first
    // cave corrected xmm8 and jumped back, so the very next engine instruction
    // still wrote the PITCH accumulator from xmm0, uncorrected. So the detour now
    // swallows both writes - 11 bytes instead of 6 - and corrects each.
    //
    // xmm0 and xmm8 are both put back before returning, because the engine's own
    // code carries on using them afterwards.
    const size_t kPatchLen = 11;    // the two stores: 6 + 5
    const size_t kCodeLen = 66;
    float* corrYaw = reinterpret_cast<float*>(cave + 0x80);
    float* corrPitch = reinterpret_cast<float*>(cave + 0x84);
    *corrYaw = 0.0f;
    *corrPitch = 0.0f;

    size_t o = 0;
    auto emit = [&](std::initializer_list<uint8_t> b) {
        for (uint8_t v : b) cave[o++] = v;
    };
    auto ripTo = [&](const void* p) {
        const int32_t disp = int32_t(reinterpret_cast<uintptr_t>(p) -
                                     (reinterpret_cast<uintptr_t>(cave) + o + 4));
        memcpy(cave + o, &disp, 4);
        o += 4;
    };

    emit({0x48, 0x83, 0xEC, 0x20});                    // sub rsp,0x20
    emit({0x0F, 0x11, 0x0C, 0x24});                    // movups [rsp],xmm1

    emit({0xF3, 0x0F, 0x10, 0x0D}); ripTo(corrYaw);    // movss xmm1,[rip+corrYaw]
    emit({0xF3, 0x44, 0x0F, 0x5C, 0xC1});              // subss xmm8,xmm1
    emit({0xF3, 0x44, 0x0F, 0x11, 0x43, 0x1C});        // movss [rbx+0x1c],xmm8   ORIGINAL
    emit({0xF3, 0x44, 0x0F, 0x58, 0xC1});              // addss xmm8,xmm1

    emit({0xF3, 0x0F, 0x10, 0x0D}); ripTo(corrPitch);  // movss xmm1,[rip+corrPitch]
    emit({0xF3, 0x0F, 0x5C, 0xC1});                    // subss xmm0,xmm1
    emit({0xF3, 0x0F, 0x11, 0x43, 0x20});              // movss [rbx+0x20],xmm0   ORIGINAL
    emit({0xF3, 0x0F, 0x58, 0xC1});                    // addss xmm0,xmm1

    emit({0x0F, 0x10, 0x0C, 0x24});                    // movups xmm1,[rsp]
    emit({0x48, 0x83, 0xC4, 0x20});                    // add rsp,0x20
    emit({0xE9});                                      // jmp back
    {
        const uintptr_t back = t + kPatchLen;
        const int32_t rel = int32_t(back - (reinterpret_cast<uintptr_t>(cave) + o + 4));
        memcpy(cave + o, &rel, 4);
        o += 4;
    }
    if (o != kCodeLen) {
        COTW_LOG("[cave] assembled %zu bytes, expected %zu - refusing to install",
                 o, kCodeLen);
        VirtualFree(cave, 0, MEM_RELEASE);
        return false;
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(target, kPatchLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return false;
    }
    memcpy(g_cave.original, target, kPatchLen);
    uint8_t* code = static_cast<uint8_t*>(target);
    code[0] = 0xE9;
    const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(cave) - (t + 5));
    memcpy(code + 1, &rel, 4);
    for (size_t i = 5; i < kPatchLen; ++i) code[i] = 0x90;
    VirtualProtect(target, kPatchLen, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, kPatchLen);

    g_cave.mem = cave;
    g_cave.correction = corrYaw;
    g_cave.correctionPitch = corrPitch;
    g_cave.installed = true;
    COTW_LOG("[cave] installed at 0x%X -> %p, covering BOTH accumulator writes. "
             "The engine's own stores still run; only the values have our head "
             "angles taken out.", apex::kReseedAccumWrite, cave);
    return true;
}

void RemoveReseedCave() {
    if (!g_cave.installed) return;
    void* target = apex::Rva(apex::kReseedAccumWrite);
    if (target) {
        DWORD oldProt = 0;
        if (VirtualProtect(target, 11, PAGE_EXECUTE_READWRITE, &oldProt)) {
            memcpy(target, g_cave.original, 11);
            VirtualProtect(target, 11, oldProt, &oldProt);
            FlushInstructionCache(GetCurrentProcess(), target, 11);
        }
    }
    if (g_cave.mem) VirtualFree(g_cave.mem, 0, MEM_RELEASE);
    g_cave.mem = nullptr;
    g_cave.correction = nullptr;
    g_cave.correctionPitch = nullptr;
    g_cave.installed = false;
    COTW_LOG("[cave] removed; the original instruction is back");
}

// *** THE HOLD-BREATH CAVES. ***
//
// Same idea as InstallReseedCave, simpler shape. The two stores are 17 bytes
// apart with a lea and a call between them, so one cave cannot swallow both the
// way kReseedAccumWrite's does - but each is EXACTLY 5 bytes, which is exactly a
// jmp rel32, so two independent detours fit with nothing stolen and nothing
// padded.
//
// Both stores write xmm0 to [rsi+off]. The engine keeps using rsi, rbx and rcx
// afterwards, so nothing but xmm1 is touched, and that is saved and restored.
struct HoldBreathCave {
    uint8_t* mem = nullptr;
    float* correction = nullptr;
    uint8_t original[5] = {};
    uint32_t rva = 0;
    bool installed = false;
};
HoldBreathCave g_hbYaw, g_hbPitch;

bool InstallHoldBreathStore(HoldBreathCave& hb, uint32_t rva, uint8_t accumOffset) {
    if (hb.installed) return true;
    void* target = apex::Rva(rva);
    if (!target) return false;
    const uintptr_t t = reinterpret_cast<uintptr_t>(target);

    // Refuse if the bytes are not the store we expect. A wrong address here is a
    // crash, and the whole point of a cave over a hook is not gambling.
    const uint8_t* have = static_cast<const uint8_t*>(target);
    const uint8_t want[5] = {0xF3, 0x0F, 0x11, 0x46, accumOffset};
    if (memcmp(have, want, 5) != 0) {
        COTW_LOG("[hbcave] 0x%X is not 'movss [rsi+0x%02X],xmm0' "
                 "(%02X %02X %02X %02X %02X) - refusing to patch", rva,
                 accumOffset, have[0], have[1], have[2], have[3], have[4]);
        return false;
    }

    uint8_t* cave = AllocNear(t, 0x100);
    if (!cave) {
        COTW_LOG("[hbcave] could not allocate within reach of 0x%X", rva);
        return false;
    }

    constexpr size_t kPatchLen = 5;      // exactly a jmp rel32, nothing stolen
    // 42, counted rather than estimated:
    //   sub rsp,0x20            4      movss [rsi+off],xmm0    5
    //   movups [rsp],xmm1       4      addss xmm0,xmm1         4
    //   movss xmm1,[rip+corr]   8      movups xmm1,[rsp]       4
    //   subss xmm0,xmm1         4      add rsp,0x20            4
    //                                  jmp rel32               5
    constexpr size_t kCodeLen = 42;
    float* corr = reinterpret_cast<float*>(cave + 0x80);
    *corr = 0.0f;

    size_t o = 0;
    auto emit = [&](std::initializer_list<uint8_t> b) {
        for (uint8_t v : b) cave[o++] = v;
    };
    auto ripTo = [&](const void* p) {
        const int32_t disp = int32_t(reinterpret_cast<uintptr_t>(p) -
                                     (reinterpret_cast<uintptr_t>(cave) + o + 4));
        memcpy(cave + o, &disp, 4);
        o += 4;
    };

    emit({0x48, 0x83, 0xEC, 0x20});                  // sub rsp,0x20
    emit({0x0F, 0x11, 0x0C, 0x24});                  // movups [rsp],xmm1
    emit({0xF3, 0x0F, 0x10, 0x0D}); ripTo(corr);     // movss xmm1,[rip+corr]
    emit({0xF3, 0x0F, 0x5C, 0xC1});                  // subss xmm0,xmm1
    emit({0xF3, 0x0F, 0x11, 0x46, accumOffset});     // movss [rsi+off],xmm0 ORIGINAL
    emit({0xF3, 0x0F, 0x58, 0xC1});                  // addss xmm0,xmm1
    emit({0x0F, 0x10, 0x0C, 0x24});                  // movups xmm1,[rsp]
    emit({0x48, 0x83, 0xC4, 0x20});                  // add rsp,0x20
    emit({0xE9});                                    // jmp back
    {
        const uintptr_t back = t + kPatchLen;
        const int32_t rel = int32_t(back - (reinterpret_cast<uintptr_t>(cave) + o + 4));
        memcpy(cave + o, &rel, 4);
        o += 4;
    }
    if (o != kCodeLen) {
        COTW_LOG("[hbcave] assembled %zu bytes, expected %zu - refusing to install",
                 o, kCodeLen);
        VirtualFree(cave, 0, MEM_RELEASE);
        return false;
    }

    DWORD oldProt = 0;
    if (!VirtualProtect(target, kPatchLen, PAGE_EXECUTE_READWRITE, &oldProt)) {
        VirtualFree(cave, 0, MEM_RELEASE);
        return false;
    }
    memcpy(hb.original, target, kPatchLen);
    uint8_t* code = static_cast<uint8_t*>(target);
    code[0] = 0xE9;
    const int32_t rel = int32_t(reinterpret_cast<uintptr_t>(cave) - (t + 5));
    memcpy(code + 1, &rel, 4);
    VirtualProtect(target, kPatchLen, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, kPatchLen);

    hb.mem = cave;
    hb.correction = corr;
    hb.rva = rva;
    hb.installed = true;
    return true;
}

// *** TIER 1 FOV, LEVER A: THE ENGINE'S OWN WORLD FOV. ***
//
// One float per frame, and it reaches everything: the shared world->clip matrix,
// every baked WVP, the terrain's offset view-projection, the vegetation culling
// PLANES and its LOD scalar, and the depth-linearisation constants are all built
// downstream of it. That self-consistency is exactly what a matrix patch could
// not have given us - and it is why the constant-buffer route was abandoned:
// there is no projection matrix in this renderer to patch (all 26,397 buffers
// were tested), and the ~50 lookalike windows plus the vegetation compute pass
// would have been left behind on the old frustum.
//
// A MULTIPLIER ON THE TANGENT, never an absolute write. Whatever magnification
// the engine chose - and it narrows the WHOLE scene when scoped, a 1.08 -> 2.98
// - survives, scaled by the same k. An absolute write would stomp the scope.
using PFN_CamModUpdate = void(__fastcall*)(void*, void*, float*, void*);
PFN_CamModUpdate o_CamModUpdate = nullptr;

volatile LONG g_leverApplied2 = 0;
volatile LONG g_leverSkipped = 0;
float g_leverLastWorld = 0.0f;
float g_leverLastView = 0.0f;

void __fastcall Hook_CamModUpdate(void* self, void* pose, float* lensOut, void* cam) {
    o_CamModUpdate(self, pose, lensOut, cam);
    if (!Cfg().tier1_fov || !lensOut) return;

    // Stage 1 is flat and headset-free: a raw k, so the lever can be proved
    // before anything depends on xrLocateViews. Stage 2 replaces this with the
    // tangent that covers both eyes.
    const float k = Cfg().tier1_fov_k_override;
    if (!(k > 0.05f) || !(k < 8.0f)) return;

    for (int i = 0; i < 2; ++i) {
        if (i == 1 && !Cfg().tier1_scale_viewmodel_fov) break;
        const float f = lensOut[i];
        // Radians, and a plausible field of view. Anything else means the
        // argument layout is not what we think and we must not write.
        if (!(f > 0.05f) || !(f < 3.0f)) { InterlockedIncrement(&g_leverSkipped); continue; }
        lensOut[i] = 2.0f * atanf(tanf(f * 0.5f) * k);
    }
    g_leverLastWorld = lensOut[0];
    g_leverLastView = lensOut[1];
    InterlockedIncrement(&g_leverApplied2);
}

bool InstallCamModFovLever() {
    static bool tried = false;
    if (tried) return o_CamModUpdate != nullptr;
    tried = true;
    void* target = apex::Rva(apex::kCamModUpdate);
    if (!target) return false;
    MH_STATUS s = MH_CreateHook(target, &Hook_CamModUpdate, (void**)&o_CamModUpdate);
    if (s != MH_OK) {
        COTW_LOG("[tier1] MH_CreateHook(CamModUpdate @0x%X) failed: %d",
                 apex::kCamModUpdate, (int)s);
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        COTW_LOG("[tier1] MH_EnableHook(CamModUpdate) failed");
        return false;
    }
    COTW_LOG("[tier1] FOV lever installed at 0x%X - the camera modifier's Update. "
             "k = %.3f (1.000 = no change)", apex::kCamModUpdate,
             Cfg().tier1_fov_k_override);
    return true;
}
bool InstallHoldBreathCaves() {
    const bool y = InstallHoldBreathStore(g_hbYaw, apex::kHoldBreathReseedYaw, 0x1C);
    const bool p = InstallHoldBreathStore(g_hbPitch, apex::kHoldBreathReseedPitch, 0x20);
    // ONCE. This is called every frame and the install itself is idempotent
    // (InstallHoldBreathStore returns early once patched), but the log line was
    // not - 17,252 identical lines in one session, each a file write on the
    // render thread. Diagnostics that cost frames are not free diagnostics, and
    // this one buried every other message in the log while it was at it.
    static bool s_reported = false;
    static bool s_reportedFail = false;
    if (y && p) {
        if (!s_reported) {
            s_reported = true;
            COTW_LOG("[hbcave] installed at 0x%X and 0x%X - hold breath re-seeds the "
                     "aim through its own deferred latch, which the kAimReseed cave "
                     "never touched", apex::kHoldBreathReseedYaw,
                     apex::kHoldBreathReseedPitch);
        }
    } else if (!s_reportedFail) {
        s_reportedFail = true;
        COTW_LOG("[hbcave] NOT fully installed (yaw=%d pitch=%d) - hold breath "
                 "will still rotate the view", (int)y, (int)p);
    }
    return y && p;
}

void RemoveHoldBreathCaves() {
    for (HoldBreathCave* hb : {&g_hbYaw, &g_hbPitch}) {
        if (!hb->installed) continue;
        if (void* target = apex::Rva(hb->rva)) {
            DWORD oldProt = 0;
            if (VirtualProtect(target, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
                memcpy(target, hb->original, 5);
                VirtualProtect(target, 5, oldProt, &oldProt);
                FlushInstructionCache(GetCurrentProcess(), target, 5);
            }
        }
        hb->installed = false;
        hb->correction = nullptr;
    }
}

// Called every frame. The cave reads this and nothing else, so "off" is simply
// a correction of zero - the store then behaves exactly as the engine wrote it.
void UpdateReseedCorrection() {
    if (!g_cave.installed || !g_cave.correction) return;
    float cy = 0.0f, cp = 0.0f;
    if (Cfg().head_tracking && Cfg().head_reseed_detour) {
        float d = 0.0f;
        if (HeadYawRadians(&d)) cy = d * Cfg().head_reseed_correct_scale;
        // The same scale for pitch. Both accumulators are written by the same
        // routine from the same kind of quantity, so the convention should match
        // - and the yaw scale landed on exactly -1.0, which is a strong hint the
        // pitch one is the same number rather than a coincidence.
        if (HeadPitchRadians(&d)) cp = d * Cfg().head_reseed_correct_scale;
    }
    *g_cave.correction = cy;
    if (g_cave.correctionPitch) *g_cave.correctionPitch = cp;

    // The hold-breath caves read the same correction. Same quantity, same
    // convention, different re-seed - and like the one above, zero means the
    // engine's store behaves exactly as written.
    if (g_hbYaw.correction) *g_hbYaw.correction = cy;
    if (g_hbPitch.correction) *g_hbPitch.correction = cp;
}

// *** THE RATCHET FIX, SECOND ATTEMPT - A BYTE PATCH, NOT A HOOK. ***
//
// The instruction is right; the hook around it was not. Wrapping 0x00638730 in a
// four-argument trampoline crashed the game on the first crouch, twice - and an
// identity check on the object did not help, which says the fault was in the
// call itself rather than in what it wrote. That routine is float-heavy
// (xmm6/7/8 saved in its prologue) and its real signature is unknown; guessing
// it is how you corrupt a register the caller still needs.
//
// So do not call it at all. The two instructions that do the damage are known
// exactly, from Cheat Engine rather than from reasoning:
//
//   0x006388CC  f3 44 0f 11 43 1c   movss [rbx+0x1c], xmm8   ; the yaw accumulator
//   0x006388D2  f3 0f 11 43 20      movss [rbx+0x20], xmm0   ; the pitch one
//
// Replacing them with NOPs means the re-seed simply does not overwrite the
// player's look accumulators. Everything else it does - and it does plenty -
// still runs untouched. No trampoline, no calling convention to get wrong, and
// no way to write to an object that is not the intended one, because we are not
// the ones writing.
//
// Reversible at runtime: the original bytes are kept and written back when the
// setting goes off, so it can be A/B'd in one session.
struct BytePatch {
    uint32_t rva;
    const char* what;
    uint8_t original[8];
    uint8_t length;
    bool saved;
};

BytePatch g_reseedPatches[] = {
    {0x006388CC, "yaw accumulator", {}, 6, false},
    {0x006388D2, "pitch accumulator", {}, 5, false},
};

// The LIVE aim yaw, three instructions earlier in the same routine:
//   0x0063888E  89 43 4c   mov [rbx+0x4c], eax
//
// Blocking the two accumulator writes above stopped the RATCHET dead - the
// rotation stopped accumulating. What was left was a TRANSIENT: the view swung
// going into a crouch and swung back coming out, net zero, and the same for
// aiming. This instruction is why. The re-seed still rewrote the live aim, once
// entering and once leaving, and the two cancelled each other out.
//
// Its own switch, because this is the field the renderer actually reads and so
// the riskier of the two edits. If something that legitimately moves the camera
// stops working, this is the first thing to turn off.
BytePatch g_reseedYawPatch[] = {
    {0x0063888E, "live aim yaw", {}, 3, false},
};

bool g_reseedPatched = false;
bool g_reseedYawPatched = false;

// Shared by both, so the protect/restore logic exists once.
void ApplyPatchList(BytePatch* list, size_t count, bool on) {
    for (size_t i = 0; i < count; ++i) {
        BytePatch& p = list[i];
        void* addr = apex::Rva(p.rva);
        if (!addr) return;
        DWORD old = 0;
        if (!VirtualProtect(addr, p.length, PAGE_EXECUTE_READWRITE, &old)) continue;
        uint8_t* code = static_cast<uint8_t*>(addr);
        if (on) {
            if (!p.saved) {
                memcpy(p.original, code, p.length);
                p.saved = true;
            }
            memset(code, 0x90, p.length);
        } else if (p.saved) {
            memcpy(code, p.original, p.length);
        }
        VirtualProtect(addr, p.length, old, &old);
    }
}

void ApplyReseedYawPatch(bool on) {
    if (on == g_reseedYawPatched) return;
    ApplyPatchList(g_reseedYawPatch, 1, on);
    g_reseedYawPatched = on;
    COTW_LOG("[reseed] the re-seed's write to the LIVE aim yaw (0x%X) is now %s - "
             "this is the one that swung the view into a crouch and back out again",
             g_reseedYawPatch[0].rva, on ? "BLOCKED" : "restored");
}

void ApplyReseedPatch(bool on) {
    if (on == g_reseedPatched) return;
    for (BytePatch& p : g_reseedPatches) {
        void* addr = apex::Rva(p.rva);
        if (!addr) return;
        DWORD old = 0;
        if (!VirtualProtect(addr, p.length, PAGE_EXECUTE_READWRITE, &old)) continue;
        uint8_t* code = static_cast<uint8_t*>(addr);
        if (on) {
            if (!p.saved) {
                memcpy(p.original, code, p.length);
                p.saved = true;
            }
            memset(code, 0x90, p.length);        // NOP it out
        } else if (p.saved) {
            memcpy(code, p.original, p.length);
        }
        VirtualProtect(addr, p.length, old, &old);
    }
    g_reseedPatched = on;
    COTW_LOG("[reseed] the aim re-seed's writes to your look accumulators are now "
             "%s (0x%X and 0x%X). This is the routine Cheat Engine caught writing "
             "once per crouch.",
             on ? "BLOCKED" : "restored",
             g_reseedPatches[0].rva, g_reseedPatches[1].rva);
}

// Signature read off the call site: void(float* outEuler, const float* matrix).
using PFN_MatrixToEuler = void(__fastcall*)(float*, const float*);
PFN_MatrixToEuler o_MatrixToEuler = nullptr;
volatile LONG g_bakeSeen = 0;

void __fastcall Hook_MatrixToEuler(float* out, const float* m) {
    // The value BEFORE, so what this call actually changes can be measured
    // rather than assumed.
    float beforeYaw = 0.0f, beforePitch = 0.0f;
    bool haveBefore = false;
    if (out) {
        __try {
            beforeYaw = out[0];
            beforePitch = out[1];
            haveBefore = true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    o_MatrixToEuler(out, m);

    if (!out || !haveBefore || !Cfg().head_tracking || !Cfg().head_view_basis) return;

    const uintptr_t base = reinterpret_cast<uintptr_t>(apex::Rva(0));
    const uint32_t rva =
        static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - base);
    if (rva != apex::kMatrixToEulerRetBake) return;

    float headYaw = 0.0f, headPitch = 0.0f, d = 0.0f;
    if (Cfg().head_write_yaw && HeadYawRadians(&d)) headYaw = d;
    if (Cfg().head_write_pitch && HeadPitchRadians(&d)) headPitch = d;
    if (Cfg().head_matrix_yaw_invert) headYaw = -headYaw;
    if (Cfg().head_matrix_pitch_invert) headPitch = -headPitch;

    __try {
        const float gotYaw = out[0], gotPitch = out[1];
        const float dy = gotYaw - beforeYaw, dp = gotPitch - beforePitch;

        // Report the first few with everything needed to judge it: what the call
        // changed, and what our head offset was at that moment. If this is the
        // bake, dy tracks headYaw. If it does not, the correction below is wrong
        // and this line says so - rather than another session spent guessing.
        const LONG n = InterlockedIncrement(&g_bakeSeen);
        if (n <= 12 || (n % 600) == 0) {
            const float deg = 57.2957795f;
            COTW_LOG("[bake] aim euler yaw %+.4f -> %+.4f (moved %+.2f deg), pitch "
                     "moved %+.2f deg; head offset yaw %+.2f pitch %+.2f deg%s",
                     beforeYaw, gotYaw, dy * deg, dp * deg,
                     headYaw * deg, headPitch * deg,
                     (fabsf(headYaw) > 0.05f && fabsf(dy) > 0.005f &&
                      fabsf(fabsf(dy / headYaw) - 1.0f) < 0.5f)
                         ? "   <== MATCHES OUR OFFSET - this is the bake" : "");
        }

        // Take our own rotation back out, so what gets STORED is the aim the
        // player actually has, not the one they happen to be looking along.
        if (Cfg().head_bake_undo) {
            out[0] = gotYaw - headYaw;
            out[1] = gotPitch - headPitch;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Rotate the basis rows of a row-major 4x4 in place. Yaw about world up, pitch
// about the matrix's own right axis - pitching about a world axis would roll the
// horizon whenever the player is not facing down that axis.
void RotateMatrixRows(float* m, float yaw, float pitch, int pitchAxisRow) {
    auto rot = [](float* v, const float* k, float a) {
        const float c = cosf(a), s = sinf(a);
        const float vx = v[0], vy = v[1], vz = v[2];
        const float cx = k[1] * vz - k[2] * vy;
        const float cy = k[2] * vx - k[0] * vz;
        const float cz = k[0] * vy - k[1] * vx;
        const float d = k[0] * vx + k[1] * vy + k[2] * vz;
        v[0] = vx * c + cx * s + k[0] * d * (1.0f - c);
        v[1] = vy * c + cy * s + k[1] * d * (1.0f - c);
        v[2] = vz * c + cz * s + k[2] * d * (1.0f - c);
    };
    const int r = (pitchAxisRow == 0) ? 0 : 2;
    float right[3] = {m[r * 4 + 0], m[r * 4 + 1], m[r * 4 + 2]};
    const float len = sqrtf(right[0] * right[0] + right[1] * right[1] +
                            right[2] * right[2]);
    if (len < 1e-4f) return;                    // degenerate basis: leave it be
    right[0] /= len; right[1] /= len; right[2] /= len;
    const float up[3] = {0.0f, 1.0f, 0.0f};
    for (int i = 0; i < 3; ++i) {
        float* row = m + i * 4;
        if (pitch != 0.0f) rot(row, right, pitch);
        if (yaw != 0.0f)   rot(row, up, yaw);
    }
}

void __fastcall Hook_EulerToMatrix(const float* angles, float* out) {
    o_EulerToMatrix(angles, out);      // the engine's own build, from its own angles

    if (!out || !Cfg().head_tracking) return;

    // 13 callers, and WHICH one is calling decides what we are allowed to turn.
    const uintptr_t base = reinterpret_cast<uintptr_t>(apex::Rva(0));
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
    const uint32_t rva = static_cast<uint32_t>(ret - base);

    // The weapon and body transform - what head_matrix_rotate has always driven.
    const bool weaponSite = (rva == apex::kEulerToMatrixRetLive ||
                             rva == apex::kEulerToMatrixRetCopy);
    // The VIEW's own basis, inside kViewCommit. Never hooked until now, and the
    // reason head_matrix_rotate could be drift-free yet leave the view still.
    const bool viewSite = (rva == apex::kEulerToMatrixRetView);

    // *** WHAT WE ASK FOR vs WHAT THE FRAME IS BUILT FROM (head_yaw_probe). ***
    //
    // The headset vibrates while the desktop mirror is clean, it stops when
    // head tracking is switched off, it predates all the TAA/DLSS work, and it
    // survives with the compositor's warping disabled entirely - so the
    // suspect is the rotation the ENGINE actually renders with. This is the
    // one place that can settle it: `angles` here IS cam+0x4C, the yaw/pitch/
    // roll the view basis is about to be built from, at the view site. Log it
    // against the head angle we handed over, frame by frame. If ours moves
    // smoothly while this one moves in steps - or lags, or alternates - the
    // injection path is the fault and the size of the step names the quantum.
    if (viewSite && Cfg().head_yaw_probe && angles) {
        static float prevEng = 0.0f, prevHead = 0.0f;
        static bool have = false;
        static float dE[10] = {}, dH[10] = {};
        static int n = 0;
        static ULONGLONG last = 0;
        float hy = 0.0f;
        HeadYawRadians(&hy);
        const float eng = angles[0];
        if (have) {
            const float kDeg = 57.2957795f;
            dE[n % 10] = (eng - prevEng) * kDeg;
            dH[n % 10] = (hy - prevHead) * kDeg;
            ++n;
        }
        prevEng = eng;
        prevHead = hy;
        have = true;
        const ULONGLONG now2 = GetTickCount64();
        if (n >= 10 && now2 - last >= 1000) {
            last = now2;
            COTW_LOG("[yawprobe] engine yaw %+.5f rad, head %+.5f rad | last 10 "
                     "frames, ENGINE step (deg): %+.4f %+.4f %+.4f %+.4f %+.4f "
                     "%+.4f %+.4f %+.4f %+.4f %+.4f",
                     eng, hy, dE[0], dE[1], dE[2], dE[3], dE[4], dE[5], dE[6],
                     dE[7], dE[8], dE[9]);
            COTW_LOG("[yawprobe]   ...the HEAD asked for (deg): %+.4f %+.4f "
                     "%+.4f %+.4f %+.4f %+.4f %+.4f %+.4f %+.4f %+.4f  "
                     "<- smooth head + stepping engine = the injection quantises",
                     dH[0], dH[1], dH[2], dH[3], dH[4], dH[5], dH[6], dH[7],
                     dH[8], dH[9]);
        }
    }

    const bool doWeapon = weaponSite && Cfg().head_matrix_rotate;
    const bool doView = viewSite && Cfg().head_view_basis;
    if (!doWeapon && !doView) return;

    float yaw = 0.0f, pitch = 0.0f, d = 0.0f;
    if (Cfg().head_write_yaw && HeadYawRadians(&d)) yaw = d;
    if (Cfg().head_write_pitch && HeadPitchRadians(&d)) pitch = d;
    if (Cfg().head_matrix_yaw_invert) yaw = -yaw;
    if (Cfg().head_matrix_pitch_invert) pitch = -pitch;
    if (yaw == 0.0f && pitch == 0.0f) return;

    __try {
        RotateMatrixRows(out, yaw, pitch, Cfg().head_matrix_pitch_axis_row);
        InterlockedIncrement(&g_matrixRotations);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }

    static DWORD lastLog = 0;
    const DWORD now = GetTickCount();
    if (now - lastLog > 5000) {
        lastLog = now;
        COTW_LOG("[mrot] rotating the view matrix built at +0x%X by yaw %+.1f, pitch "
                 "%+.1f deg. The engine's own angles are untouched, so there is "
                 "nothing to bake. %ld rotations so far.", rva,
                 yaw * 57.2957795f, pitch * 57.2957795f, (long)g_matrixRotations);
    }
}

// *** SUSPEND WHILE THE STANCE IS CHANGING. ***
//
// This is a POSITIVE signal about the event, where everything before it was an
// inference drawn from how a float was behaving - "did the field come back
// unchanged", "did it fall by exactly our offset". Those were guesses about
// WHETHER a crouch was happening. This detects the crouch.
//
// Everything measured says the ratchet happens during crouch, stand, prone and
// jump, and that writing camera+0x4C AT ALL during those moments gets adopted -
// early write, late write, any timing. So simply do not be holding an offset
// while one is under way.
//
// Detecting it needs no new reverse engineering: all four events move the
// CAMERA'S HEIGHT, and the camera position is already in our hands at the
// transform builder. A fast vertical rate is a stance change; walking a slope
// is an order of magnitude slower. The measured rate is logged so the threshold
// can be set from data instead of taste.
volatile LONG g_stanceDeadline = 0;    // GetTickCount() at which the hold ends
float  g_lastCamY = 0.0f;
DWORD  g_lastCamYAt = 0;
bool   g_haveCamY = false;
float  g_peakStanceRate = 0.0f;

void NoteCameraHeight(float y) {
    const DWORD now = GetTickCount();
    if (g_haveCamY) {
        const DWORD dtMs = now - g_lastCamYAt;
        if (dtMs > 0 && dtMs < 500) {          // ignore hitches and load pauses
            const float rate = fabsf(y - g_lastCamY) / (dtMs * 0.001f);   // m/s
            if (rate > g_peakStanceRate) g_peakStanceRate = rate;
            if (rate > Cfg().stance_rate_mps) {
                InterlockedExchange(&g_stanceDeadline,
                                    (LONG)(now + (DWORD)Cfg().stance_hold_ms));
                static DWORD lastLog = 0;
                if (now - lastLog > 1000) {
                    lastLog = now;
                    COTW_LOG("[stance] camera height moving %.2f m/s - crouch, stand, "
                             "prone or jump. Head offset suspended for %d ms, so there "
                             "is nothing in the field to be adopted.",
                             rate, Cfg().stance_hold_ms);
                }
            }
        }
    }
    g_lastCamY = y;
    g_lastCamYAt = now;
    g_haveCamY = true;
}

bool StanceChanging() {
    if (!Cfg().head_suspend_on_stance) return false;
    const LONG deadline = g_stanceDeadline;
    if (!deadline) return false;
    return (LONG)(GetTickCount() - (DWORD)deadline) < 0;   // wrap-safe
}

CamWatch* WatchFor(void* camera) {
    const LONG n = g_camWatchCount;
    for (LONG i = 0; i < n && i < kMaxCamWatch; ++i) {
        if (g_camWatch[i].obj == camera) return &g_camWatch[i];
    }
    return nullptr;
}

// *** DO NOT LEAVE ANYTHING TO STEAL. ***
//
// Correcting after the fact could not win. Correcting every frame killed the
// drift but fought the player's turning; correcting once per event let the
// drift back, because the theft is not one event - while crouching, jumping or
// going prone the engine stops recomputing the field for a while and adopts our
// offset on EVERY frame it does not.
//
// But that condition is exactly detectable: if the field comes back bit-for-bit
// as we left it, the engine has not recomputed it, and anything we add now will
// be taken as the engine's own. So on those frames put the base back and apply
// NOTHING. Head tracking pauses for the fraction of a second a crouch takes -
// which is invisible - and there is never an offset sitting there to be adopted.
//
// This is the "idempotent base" idea from the first attempt, which failed only
// because the state was a single shared pair of variables while TWO camera
// objects alternate; each pass saw the other's value and the test never fired.
// Per object, it answers the right question.
bool HoldBaseIfEngineIdle(void* camera, CamWatch* w) {
    if (!w || !w->haveWrote) return false;
    __try {
        char* b = static_cast<char*>(camera);
        float* py = reinterpret_cast<float*>(b + apex::kAimYawOffset);
        float* pp = reinterpret_cast<float*>(b + apex::kAimPitchOffset);
        const bool sameYaw = fabsf(*py - w->wroteYaw) < 1e-6f;
        const bool samePitch = fabsf(*pp - w->wrotePitch) < 1e-6f;
        if (!sameYaw && !samePitch) return false;    // engine recomputed: carry on
        // Hand the engine back its own value and sit this frame out.
        if (sameYaw) *py = w->baseYaw;
        if (samePitch) *pp = w->basePitch;
        static DWORD lastLog = 0;
        const DWORD t = GetTickCount();
        if (t - lastLog > 1000) {
            lastLog = t;
            COTW_LOG("[hold] the engine is not recomputing the aim (crouch / jump / "
                     "prone) - base restored and no offset applied, so there is "
                     "nothing for it to adopt");
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Remember what this object was offset by, so the leak detector above knows
// what a swallowed offset would look like next time round.
void NoteAppliedOffset(void* camera, float yaw, float pitch) {
    const LONG n = g_camWatchCount;
    for (LONG i = 0; i < n && i < kMaxCamWatch; ++i) {
        if (g_camWatch[i].obj == camera) {
            g_camWatch[i].appliedYaw = yaw;
            g_camWatch[i].appliedPitch = pitch;
            return;
        }
    }
}

// Deferred undo, for mode 2: the offset has to survive past the consumer
// (the weapon reads it there) and be gone before the engine re-anchors.
struct PendingUndo { void* obj; float yaw, pitch; bool active; };
constexpr int kMaxUndo = 4;
PendingUndo g_undo[kMaxUndo] = {};

void NotePendingUndo(void* obj, float yaw, float pitch) {
    for (int i = 0; i < kMaxUndo; ++i) {
        if (g_undo[i].obj == obj || g_undo[i].obj == nullptr) {
            g_undo[i].obj = obj;
            g_undo[i].yaw = yaw;
            g_undo[i].pitch = pitch;
            g_undo[i].active = true;
            return;
        }
    }
}

bool ApplyPendingUndo(void* obj) {
    for (int i = 0; i < kMaxUndo; ++i) {
        if (g_undo[i].obj != obj || !g_undo[i].active) continue;
        __try {
            char* b = static_cast<char*>(obj);
            *reinterpret_cast<float*>(b + apex::kAimYawOffset) += g_undo[i].yaw;
            *reinterpret_cast<float*>(b + apex::kAimPitchOffset) += g_undo[i].pitch;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        g_undo[i].active = false;
        return true;
    }
    return false;
}

// How often the transform builder never came, so the consumer had to clean up
// the previous frame's offset itself. If this is not zero, the builder is not a
// reliable place to undo anything - which is exactly the leak.
volatile LONG g_undoFallback = 0;
volatile LONG g_undoAtBuilder = 0;
volatile LONG g_latchSkips = 0;

// *** MEASURE WHAT THE CONSUMER DOES TO THE FIELD. ***
//
// Three guesses have now been wrong - "the engine stops recomputing", "restore
// the saved value", "undo by delta" - and each cost a run. The undo can only be
// written correctly once it is known what the callee itself does to the value,
// so record it: what was there, what we made it, and what came back.
//   came back == what we wrote      -> the callee ignores it; a delta undo is
//                                      exact and the leak is elsewhere
//   came back != what we wrote      -> the callee rewrites or CLAMPS it, and
//                                      adding delta back onto a clamped value
//                                      is precisely a slow leak
void LogConsumerEffect(float before, float applied, float after, const char* tag) {
    if (!Cfg().head_reset_watch) return;
    const float own = after - applied;          // what the callee itself changed
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (fabsf(own) < 1e-6f) return;             // callee left it alone: nothing to say
    if (now - last < 400) return;
    last = now;
    COTW_LOG("[aimfx] %s: engine had %.4f, we set %.4f, it came back %.4f -> the "
             "consumer itself moved it %+.4f rad (%+.2f deg). A delta undo is only "
             "exact when this is 0.",
             tag, before, applied, after, own, own * 57.2957795f);
}


// *** APPLY, LET IT BE USED, THEN PUT IT BACK. ***
//
// Leaving the offset in the field is what made crouching climb. The log named
// it exactly: every crouch moved the view by -42.3 deg yaw and +6.3 deg pitch,
// which is precisely the offset being applied at the time. Crouching makes the
// engine RE-ANCHOR its aim from whatever is in the field - so it read our
// modified value and adopted it as its own, and the next frame we offset again
// from that. Nothing to do with the engine failing to recompute; the value
// simply must not still be ours when the engine looks.
//
// So the offset lives only for the duration of this call. The weapon is placed
// inside it and follows the head; the moment it returns the field is exactly as
// the engine left it, and there is nothing for a re-anchor to pick up. The VIEW
// is handled separately at the transform builder later in the frame - the site
// that drove the view before any of this and never drifted.
// WrapPi is file-local in headtrack.cpp; three lines are cheaper than exporting
// it. The engine's own helper at 0x004A03A0 wraps this field to +/-PI, so a
// credit that leaves it outside that range is a value the engine never holds.
float RecentreWrapPi(float a) {
    const float kPi = 3.14159265358979f, kTwoPi = 6.28318530717959f;
    while (a > kPi) a -= kTwoPi;
    while (a < -kPi) a += kTwoPi;
    return a;
}

// The player's aim object, cached from the consumer hook BEFORE any gate.
// The credit needs it at sites that never receive it: decoupled play and
// free-look both put the offset in at WriteYawField, and that site stands on the
// camera, not on the player's aim.
void* g_aimObj = nullptr;

// *** THE RECENTRE CREDIT, IN ONE PLACE, CALLED FROM WHICHEVER SITE PUT THE
// OFFSET INTO THE PICTURE LAST THIS FRAME. ***
//
// V = B - O. To have V unchanged AND O = 0 afterwards, B must absorb the offset:
// write -O into the player's own look accumulator, which is exactly what the
// engine does to us on every crouch (the ratchet is an uncommanded,
// uncompensated recentre). Same operation, deliberately, once.
//
// AFTER the applying site, never before it. The camera keeps this frame's
// offset, and the next frame's camera is derived from the credited accumulator,
// so the two changes cross over between frames and the picture never shows one
// without the other.
//
// THREE THINGS HERE ARE LOAD-BEARING AND WERE EACH A BUG:
//  * Its OWN __try. Sharing the apply block's handler meant a fault on the first
//    dereference of the aim object consumed the press for good AND wiped the
//    undo booking for an offset already sitting in the camera field.
//  * The reference advance is DERIVED from the credit, not re-read from the
//    head. Two reads of g_head a hundred lines apart are two different poses,
//    and the difference is a permanent picture jump.
//  * The pitch credit is CLAMPED and the advance follows what actually fitted,
//    because the engine hard-clamps that accumulator and an over-long credit is
//    a credit that partly never happened.
bool TryRecentreCredit(float yawDelta, bool haveYaw, float pitchDelta,
                       bool havePitch) {
    if (!Cfg().recentre_compensate) return false;
    // The axis test replaces the head offset with a sine sweep. Crediting that
    // would displace the player's aim, permanently, by a random sample of a wave
    // that has nothing to do with where his head is.
    if (Cfg().head_test_axis > 0) return false;
    void* a = g_aimObj;
    if (!a || (!haveYaw && !havePitch)) return false;
    // Never credit twice before the first advance has settled: the offset is
    // still standing until the next HeadTrackTick, so a second credit would put
    // 2*O into the engine against one O of head, and the picture would move.
    if (RecentreCommitPending()) return false;
    if (!TakeRecentreRequest()) return false;

    // THE RAMP. Hand over a slice of what is outstanding, not the whole
    // thing. The credit is correct either way - measured, asked 15.99 deg and
    // landed 15.99 - but in one step the GAME's camera really does move 16
    // degrees in a single frame, and its screen-space effects produce a
    // frame of rubbish out of a discontinuity that size. Slicing keeps the
    // picture equally still while never handing any one frame a big step.
    const float ramp = RecentreRampFraction();
    yawDelta *= ramp;
    pitchDelta *= ramp;

    const float s = Cfg().recentre_compensate_invert ? -1.0f : 1.0f;
    float landedYaw = 0.0f, landedPitch = 0.0f;
    float accYawBefore = 0.0f, accYawAfter = 0.0f;
    float accPitchBefore = 0.0f, accPitchAfter = 0.0f;
    bool creditedYaw = false, creditedPitch = false;
    char* aim = static_cast<char*>(a);

    __try {
        if (haveYaw) {
            float* acc = reinterpret_cast<float*>(aim + apex::kAimInputYaw);
            accYawBefore = *acc;
            // The engine's own helper at 0x004A03A0 wraps this field to +/-PI,
            // so a credit that leaves it outside that range is a value the
            // engine never holds.
            *acc = RecentreWrapPi(accYawBefore - s * yawDelta);
            accYawAfter = *acc;
            landedYaw = RecentreWrapPi(accYawBefore - accYawAfter);
            creditedYaw = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        creditedYaw = false;
    }

    __try {
        if (havePitch) {
            float* acc = reinterpret_cast<float*>(aim + apex::kAimInputPitch);
            accPitchBefore = *acc;
            // *** CLAMP, AND ADVANCE ONLY BY WHAT FITTED. ***
            // apex.h records a HARD CLAMP of +/-PI/2 inside 0x004A03A0
            // (0x004A03F9/0404) and a tighter data-driven one at 0x00644200.
            // Writing past it and advancing the reference by the whole offset is
            // the owner's own failure report in a new costume: the engine keeps
            // what it can hold, the mod zeroes its side completely, and the view
            // drops by the difference - "it still brings me down, just less."
            // Clamping here leaves the shortfall in the mod's own offset, where
            // V = B' - O' comes out unchanged either way and the recentre is
            // merely partial, which is the honest answer when the engine
            // physically cannot look that far up.
            float lim = float(Cfg().recentre_pitch_limit_deg) * 0.01745329252f;
            if (!(lim > 0.0f) || lim > 1.57079633f) lim = 1.57079633f;
            float want = accPitchBefore - s * pitchDelta;
            if (want > lim) want = lim;
            if (want < -lim) want = -lim;
            *acc = want;
            accPitchAfter = *acc;
            landedPitch = accPitchBefore - accPitchAfter;
            creditedPitch = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        creditedPitch = false;
    }

    if (!creditedYaw && !creditedPitch) {
        PutBackRecentreRequest();     // the aim object was not readable
        COTW_LOG("[head] recentre could NOT be credited - reading the aim object "
                 "faulted. The press is still pending, not lost.");
        return false;
    }

    // Engine units back into head units. s is +/-1 so multiplying undoes it, and
    // the invert flags are the only other difference between the two - each axis
    // has its own, because the two fields' sign conventions are independent.
    const float appliedYaw = creditedYaw ? s * landedYaw : 0.0f;
    const float appliedPitch = creditedPitch ? s * landedPitch : 0.0f;
    CommitRecentre(Cfg().head_invert ? -appliedYaw : appliedYaw,
                   Cfg().head_pitch_invert ? -appliedPitch : appliedPitch);

    COTW_LOG("[head] recentre credited: aim+0x1C %+.4f -> %+.4f (asked %+.2f deg, "
             "landed %+.2f) | aim+0x20 %+.4f -> %+.4f (asked %+.2f deg, landed "
             "%+.2f). The reference advances by what LANDED, so the picture stays "
             "put even where the engine clamps.",
             accYawBefore, accYawAfter, s * yawDelta * 57.2957795f,
             landedYaw * 57.2957795f, accPitchBefore, accPitchAfter,
             s * pitchDelta * 57.2957795f, landedPitch * 57.2957795f);
    if (creditedPitch && fabsf(landedPitch - s * pitchDelta) > 0.002f) {
        COTW_LOG("[head] the pitch credit was CLAMPED short by %+.2f deg - that "
                 "much of your up/down look stays with the mod instead of becoming "
                 "the game's. The picture still did not move. Lower "
                 "recentre_pitch_limit_deg only if it did.",
                 (s * pitchDelta - landedPitch) * 57.2957795f);
    }
    return true;
}

void __fastcall Hook_AimConsumer(void* a, void* b, void* camera, unsigned int d,
                                 float e) {
    if (camera) NoteAimReset(camera);
    // Cached BEFORE every gate. WriteYawField - the site that applies the offset
    // whenever the weapon is decoupled or free-look is held - never receives
    // this object, and without it the recentre had no way to reach the engine's
    // accumulator in either of those modes.
    if (a) g_aimObj = a;

    // *** THE ONE LINK IN THE RECENTRE CHAIN THAT HAS NEVER BEEN MEASURED. ***
    //
    // The credit below writes aimObj+0x1C/+0x20; the picture is driven from
    // camera+0x4C/+0x50. That those are the SAME quantity is a DERIVATION (the
    // camera field must be rebuilt each frame from a source we do not write, or
    // head tracking at head_undo_mode = 0 would run away by one offset per
    // frame) - and the disassembly of the helper at 0x004A03A0 reads like
    // in-place accumulation, which points the other way. The two have never been
    // seen in the same frame.
    //
    // If they track 1:1 through a look-around, the credit lands and the design
    // is right. If they drift apart, the credit target is wrong and the failure
    // in the headset is the nastiest one available: recentre appears to do
    // nothing, and then the view lurches on its own at the next crouch.
    //
    // READ-ONLY, twice a second. Armed BY THE PRESS ITSELF and not only by the
    // diagnostic flag: head_reset_watch is 0 in the shipped ini and has no panel
    // row, so gating it on that alone meant the one measurement this design
    // rests on would print nothing on the owner's machine and the session would
    // settle nothing. RecentreWatchActive() covers a few seconds either side of
    // every press, which is exactly the window that carries the answer.
    if ((Cfg().head_reset_watch || RecentreWatchActive()) && a && camera) {
        static DWORD lastAcc = 0;
        const DWORD tAcc = GetTickCount();
        if (tAcc - lastAcc > 500) {
            lastAcc = tAcc;
            __try {
                const float* aim = reinterpret_cast<const float*>(
                    static_cast<const char*>(a) + apex::kAimInputYaw);
                const float* cam = reinterpret_cast<const float*>(
                    static_cast<const char*>(camera) + apex::kAimYawOffset);
                COTW_LOG("[acc] aim+0x1C %+.4f aim+0x20 %+.4f | camera+0x4C %+.4f "
                         "camera+0x50 %+.4f  (if the pairs move together the "
                         "recentre credit reaches the picture)",
                         aim[0], aim[1], cam[0], cam[1]);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        }
    }

// UNDO BY DELTA, NEVER BY SAVED VALUE.
    //
    // Restoring a saved absolute reading looked right and drifted the OTHER way,
    // which is what gave it away: the consumer WRITES this field too (recoil and
    // smoothing live in the same aim update), so putting the old number back
    // discarded the engine's own contribution for that frame. Deleting a real
    // update every frame accumulates exactly as surely as leaving ours behind
    // does - just in the opposite direction, which is precisely what the player
    // saw.
    //
    // Subtracting delta and then adding the same delta back is reversible and
    // keeps whatever the engine did in between. Our net contribution outside
    // this call is zero, to within float rounding of about 1e-7 rad.
    float yawDelta = 0.0f, pitchDelta = 0.0f;
    float yawBefore = 0.0f, yawApplied = 0.0f;
    float pitchBefore = 0.0f, pitchApplied = 0.0f;
    float* pYaw = nullptr;
    float* pPitch = nullptr;

    // EXACTLY ONCE, whatever the engine does.
    //
    // Mode 2 undoes at the camera transform builder - but that site is not
    // called every frame (it was measured at ~0.65 calls per frame), while this
    // consumer is. Every frame where the offset went in and the builder never
    // came leaked one whole offset, which is precisely the small, one-way,
    // incremental drift left over after mode 2. So if an offset is still
    // outstanding when we arrive here, the builder did not come: clean it up
    // now, before applying the next one.
    if (camera && Cfg().head_undo_mode == 2 && ApplyPendingUndo(camera)) {
        InterlockedIncrement(&g_undoFallback);
    }

    // The accounting that proves the theory rather than assuming it: every
    // offset applied must be undone exactly once, either at the builder or by
    // the fallback above. If applied != builder + fallback, something is still
    // escaping and no amount of retuning will help.
    if (Cfg().head_reset_watch) {
        static uint64_t ticks = 0;
        if ((++ticks % 1800) == 0) {
            COTW_LOG("[undo] applied %ld | latch-gated skips %ld | undone at builder %ld | undone by the "
                     "consumer because the builder never came %ld | leaked %ld",
                     (long)g_earlyHits, (long)g_latchSkips, (long)g_undoAtBuilder, (long)g_undoFallback,
                     (long)(g_earlyHits - g_undoAtBuilder - g_undoFallback));
        }
    }

    // *** THE ENGINE ANNOUNCES THE BAKE. READ IT. ***
    //
    // `a` is rcx at the call site, and rcx is rsi (0x0064424B). [rsi+0xBC] is
    // the latch guarding the ONE instruction in the whole image that can copy
    // camera+0x4C into persistent state (0x00644278, sole caller of
    // Copy6Floats). Zero means "about to copy". We run before that check, so a
    // single byte read tells us, in advance and without any estimation, that an
    // offset left in the field this tick would become permanent.
    //
    // Every previous attempt tried to work out AFTERWARDS whether the engine had
    // taken the offset - detect the jump, hold the base, undo at various points.
    // All of them were guessing at something the engine states outright.
    bool latchArmed = false;
    if (a && Cfg().head_gate_on_latch) {
        __try {
            latchArmed = (*reinterpret_cast<const unsigned char*>(
                              static_cast<const char*>(a) + apex::kAimLatchByte) == 0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            latchArmed = false;
        }
        if (latchArmed) {
            InterlockedIncrement(&g_latchSkips);
            static DWORD lastLog = 0;
            const DWORD t = GetTickCount();
            if (t - lastLog > 1000) {
                lastLog = t;
                COTW_LOG("[gate] the engine is about to copy the aim into persistent "
                         "state ([rsi+0xBC]==0) - applying no offset this tick, so "
                         "there is nothing to bake in");
            }
        }
    }

    // If the engine has gone idle on this field, restore its base and apply
    // nothing this frame - there is then nothing for it to adopt.
    CamWatch* watch = camera ? WatchFor(camera) : nullptr;
    const bool engineIdle = Cfg().head_leak_correct &&
                            HoldBaseIfEngineIdle(camera, watch);

    const bool stance = StanceChanging() || Cfg().head_matrix_rotate ||
                        Cfg().head_view_basis;   // see head_view_basis
    if (camera && !latchArmed && !engineIdle && !stance &&
        Cfg().head_tracking && CoupledNow()) {
        InterlockedIncrement(&g_earlyHits);
        __try {
            char* base = static_cast<char*>(camera);
            float d = 0.0f;
            if (Cfg().head_write_yaw && HeadYawRadians(&d) && d != 0.0f) {
                pYaw = reinterpret_cast<float*>(base + apex::kAimYawOffset);
                yawDelta = d;
                yawBefore = *pYaw;

                // *** WATCHING THE CAMERA FROM INSIDE, because nothing outside
                // can see it. ***
                //
                // The transient swing does not touch the aim object - measured,
                // all four of its fields sat still through a held aim. The next
                // suspect is the camera, and the camera CANNOT be watched with
                // Cheat Engine: it lives at a stack slot ([rbp-0x40], captured as
                // r13), so by the time anything outside the frame reads it the
                // stack has moved on. It reads as "??" and always will.
                //
                // From in here it is simply the value in our hands. yawBefore is
                // the engine's OWN yaw, sampled before our offset goes in, so a
                // jump between frames is the engine moving the camera - which is
                // exactly what a blend into the sights would look like. Anything
                // above a degree in one frame is worth a line; ordinary looking
                // never gets near that.
                // PER CAMERA OBJECT, and that is not optional. There are TWO
                // cameras alternating, one per eye pass, so a single "last value"
                // compares eye 0 against eye 1 and reports their constant
                // difference as a jump every single frame. The first version of
                // this did exactly that and produced 300 lines of
                // 0.0474 -> -0.0001 -> 0.0474 flipping at a steady 2.72 degrees:
                // entirely an artefact, and the same mistake already made once in
                // this file with the reset watch.
                if (Cfg().head_reset_watch) {
                    struct CamYaw { void* cam; float yaw; };
                    static CamYaw seen[4] = {};
                    CamYaw* slot = nullptr;
                    for (CamYaw& c : seen) {
                        if (c.cam == camera) { slot = &c; break; }
                    }
                    if (!slot) {
                        for (CamYaw& c : seen) {
                            if (!c.cam) { c.cam = camera; c.yaw = yawBefore; slot = nullptr; break; }
                        }
                    } else {
                        float step = yawBefore - slot->yaw;
                        while (step > 3.14159265f) step -= 6.2831853f;
                        while (step < -3.14159265f) step += 6.2831853f;
                        if (fabsf(step) > 0.0175f) {          // one degree
                            COTW_LOG("[camjump] camera %p yaw moved %+.2f deg in one "
                                     "frame (%.4f -> %.4f); your head is %+.2f deg "
                                     "off centre",
                                     camera, step * 57.2957795f, slot->yaw, yawBefore,
                                     yawDelta * 57.2957795f);
                        }
                        slot->yaw = yawBefore;
                    }
                }

                *pYaw = yawBefore - yawDelta;
                yawApplied = *pYaw;
                NoteHeadOffsetApplied();   // an offset really did reach the picture
                if (watch) {
                    watch->baseYaw = yawBefore;
                    watch->wroteYaw = yawApplied;
                    watch->haveWrote = true;
                }
            }
            if (Cfg().head_write_pitch && HeadPitchRadians(&d) && d != 0.0f) {
                pPitch = reinterpret_cast<float*>(base + apex::kAimPitchOffset);
                pitchDelta = d;
                pitchBefore = *pPitch;
                *pPitch = pitchBefore - pitchDelta;
                pitchApplied = *pPitch;
                NoteHeadOffsetApplied();
                if (watch) {
                    watch->basePitch = pitchBefore;
                    watch->wrotePitch = pitchApplied;
                    watch->haveWrote = true;
                }
            // Head roll, written exactly the way pitch is, one field along.
            //
            // Deliberately NOT part of the undo bookkeeping that yaw and pitch
            // use: that machinery exists to stop the engine adopting an
            // outstanding offset as the player's permanent aim on a crouch, and
            // roll is not part of the player's aim. If the horizon does start
            // creeping after crouches, that assumption was wrong and the answer
            // is the same Cheat Engine watch that found the yaw field - not a
            // different write.
            if (Cfg().head_write_roll && HeadRollRadians(&d) && d != 0.0f) {
                float* pRoll = reinterpret_cast<float*>(base + Cfg().head_roll_field);
                *pRoll = *pRoll - d * Cfg().head_roll_scale;
                // Say so once, so "the field is wrong" can be told apart from
                // "the code never ran" without a second test session. Two
                // features shipped today did nothing silently and each cost a
                // full run in the headset to diagnose.
                static bool said = false;
                if (!said) {
                    said = true;
                    COTW_LOG("[roll] writing head roll into camera+0x%X (%.1f deg "
                             "right now). If the horizon does not tilt, this field "
                             "is not roll - try another with head_roll_field.",
                             Cfg().head_roll_field, d * 57.2957795f);
                }
            }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            pYaw = pPitch = nullptr;
        }
        // *** THE RECENTRE CREDIT - outside the apply's __try, and after it. ***
        //
        // Outside, because sharing that handler meant one fault on the aim object
        // swallowed the press AND left an offset in the camera field with no undo
        // booked - the exact condition the undo machinery exists to prevent.
        //
        // After, because the camera must keep this frame's offset: the credit is
        // picked up by the NEXT frame's camera, and the two changes crossing over
        // between frames is what makes the picture continuous.
        //
        // Only when this site is the LAST to touch the picture. In undo mode 1
        // the offset applied above is taken straight back out below and
        // WriteYawField re-applies it for the view, so there the credit belongs
        // at that site instead - and it is reached, because this call is skipped.
        if (Cfg().head_undo_mode != 1) {
            TryRecentreCredit(yawDelta, pYaw != nullptr, pitchDelta, pPitch != nullptr);
        }
        NoteAppliedOffset(camera, pYaw ? yawDelta : 0.0f, pPitch ? pitchDelta : 0.0f);
    }

    o_AimConsumer(a, b, camera, d, e);

    // WHEN to undo is now the open question, and the player answered half of it
    // for free: with the undo right here the weapon STOPPED following, so the
    // weapon reads this field AFTER the call returns, not inside it. The offset
    // has to outlive the consumer and still be gone before the engine re-anchors
    // on the next crouch.
    //   0  never undo      - the weapon follows, crouch drifts (the first build)
    //   1  undo here       - crouch is clean, the weapon does not follow
    //   2  undo at the camera transform builder, later in this same function,
    //      after the weapon and the view have both read it
    if (pYaw || pPitch) {
        __try {
            if (pYaw) LogConsumerEffect(yawBefore, yawApplied, *pYaw, "yaw");
            if (pPitch) LogConsumerEffect(pitchBefore, pitchApplied, *pPitch, "pitch");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        if (Cfg().head_undo_mode == 1) {
            __try {
                if (pYaw) *pYaw += yawDelta;
                if (pPitch) *pPitch += pitchDelta;
            } __except (EXCEPTION_EXECUTE_HANDLER) {
            }
        } else if (Cfg().head_undo_mode == 2) {
            NotePendingUndo(camera, pYaw ? yawDelta : 0.0f,
                            pPitch ? pitchDelta : 0.0f);
        }
    }
}

void WriteYawField(void* obj) {
    if (!Cfg().head_tracking || !obj) return;
    // Decoupled drifts too, so this path needs the same suspension - and the
    // matrix route replaces angle writing entirely.
    if (StanceChanging() || Cfg().head_matrix_rotate || Cfg().head_view_basis) return;
    // This site drives the VIEW - but only when the early write is not already
    // sitting in the field.
    //
    // THIS IS WHAT BROKE THE WEAPON. When the early offset is never undone
    // (undo mode 0) it is still in the field when we get here, so applying it
    // again gave the VIEW twice the rotation while the weapon had only the
    // single early one. The gun then lags the view by half, which from the
    // headset is indistinguishable from "the weapon does not follow" - and it
    // is why mode 0 stopped working after I made this site unconditional.
    //
    // Skipped whenever the EARLY offset is still in the field while the engine's
    // own builder runs - modes 0 AND 2, because our hook runs after it.
    //
    // Mode 2 is where this bit. The accounting read "applied 19713 | undone at
    // builder 19713 | leaked 0", so the early write was provably clean and the
    // drift had to come from somewhere else - and there was only one other
    // writer, this one. It applied a SECOND offset that nothing ever removed,
    // leaving the engine something to swallow on every crouch. Skipping it costs
    // nothing: the builder has already run with the early offset in place, so
    // the view has it, and it comes out again a moment later. Only mode 1 needs
    // this site, because there the early offset is gone before the builder ever
    // sees it.
    if (CoupledNow() && Cfg().head_undo_mode != 1) {
        ProbeWeaponRotation(obj, 0.0f);
        return;
    }
    float delta = 0.0f;
    float yawDelta = 0.0f, pitchDelta = 0.0f;
    bool haveYaw = false, havePitch = false;
    if (Cfg().head_write_yaw && HeadYawRadians(&delta)) {
        WriteAngleField(obj, Cfg().head_yaw_field, delta, g_yawWrite);
        NoteHeadOffsetApplied();   // free-look route: the offset lands HERE instead
        yawDelta = delta;
        haveYaw = true;
        // Sampled AFTER the write, so the known yaw field carries our offset and
        // the positive control can actually find it.
        ProbeWeaponRotation(obj, delta);
        // The candidate under test: give the weapon's own heading the same
        // offset the view gets, and see whether the gun comes with us.
        if (Cfg().weapon_rot_field >= 0) {
            const float d = Cfg().weapon_rot_invert ? -delta : delta;
            WriteAngleField(obj, Cfg().weapon_rot_field, d, g_weaponRotWrite);
        }
    }
    // Pitch is a CANDIDATE, not a confirmed field - +0x50 sits next to the yaw
    // and spans a believable clamp, which is grounds to test it, not to trust
    // it. The swept write is what decides.
    if (Cfg().head_write_pitch && HeadPitchRadians(&delta)) {
        WriteAngleField(obj, Cfg().head_pitch_field, delta, g_pitchWrite);
        NoteHeadOffsetApplied();
        pitchDelta = delta;
        havePitch = true;
    }

    // *** THE RECENTRE CREDIT FOR EVERY PATH THE AIM CONSUMER CANNOT SERVE. ***
    //
    // This site owns the picture whenever the early write does not: "Weapon
    // follows your head" turned off, free-look held, or undo mode 1. Before
    // this call the credit sat behind CoupledNow() at the consumer while this
    // function went on stamping NoteHeadOffsetApplied() - so the offset looked
    // live, the plain-latch fallback never fired, and the credit could never
    // run. The result was recentring that did nothing at all, forever, for
    // anyone who used a panel row whose own help text recommends it.
    //
    // Same ordering rule as the consumer: after the writes, so the picture keeps
    // this frame's offset and the next frame picks up the credit.
    TryRecentreCredit(yawDelta, haveYaw, pitchDelta, havePitch);
}

// --- the viewmodel hunt ----------------------------------------------------
//
// Identify FIRST, write second. A swept sideways displacement at one call site
// makes whatever that site draws swing visibly, so "which of these draws the
// held weapon" is answered by looking rather than by reasoning about names we
// do not have. 0x004B6F11 is the first suspect: the probe recorded it at the
// SAME POSITION as the player camera, and an earlier 5 m displacement test
// there "did nothing" to the world - which is exactly what you would see if it
// moves the weapon and the weapon simply left the screen.
// Scanning every call site in turn, ~5 seconds each, so one run tests all of
// them instead of one launch per candidate. The site currently being swept is
// logged with an index, and CTRL+ALT+W marks the one on screen when the WEAPON
// is what moved.
volatile LONG g_scanIndex = 0;
volatile LONG g_scanRva = 0;

void ViewmodelScanTick() {
    if (!Cfg().weapon_scan) return;
    static DWORD last = 0;
    static bool wasMark = false;
    const DWORD now = GetTickCount();

    // CTRL+ALT+W: "it was THIS one". Recorded loudly so the answer survives in
    // the log even if the count is lost track of.
    const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                      (GetAsyncKeyState(VK_MENU) & 0x8000);
    const bool mark = mods && (GetAsyncKeyState('W') & 0x8000) != 0;
    if (mark && !wasMark) {
        COTW_LOG("[vm] *** MARKED: site #%ld = 0x%06X is the one that moved. "
                 "Set weapon_site to that and turn weapon_scan off. ***",
                 (long)g_scanIndex, (unsigned)g_scanRva);
    }
    wasMark = mark;

    const int dwell = Cfg().weapon_scan_seconds > 0 ? Cfg().weapon_scan_seconds : 5;
    if (last && (now - last) < (DWORD)dwell * 1000) return;
    last = now;

    // Only sites that actually fire, and skip the two known world cameras so
    // the scan does not waste dwell time on answers we already have.
    const LONG n = g_siteCount;
    for (LONG step = 0; step < kMaxSites; ++step) {
        LONG i = (g_scanIndex + step + 1) % (n > 0 ? n : 1);
        Site& s = g_sites[i];
        if (s.claimed != 2 || s.countTotal < 10) continue;
        if (s.callerRva == apex::kCameraPositionLever) continue;
        if (s.callerRva == apex::kCameraBasisSource) continue;
        InterlockedExchange(&g_scanIndex, i);
        InterlockedExchange(&g_scanRva, (LONG)s.callerRva);
        COTW_LOG("[vm] #%ld sweeping 0x%06X  (%ld calls so far)",
                 (long)i, (unsigned)s.callerRva, (long)s.countTotal);
        return;
    }
}

void ApplyViewmodelTest(float* m) {
    int row = Cfg().eye_axis_row;
    if (row < 0 || row > 2) row = 0;
    const double t = double(GetTickCount64()) * 0.001;
    const float amt = Cfg().weapon_test_amount * sinf(float(t * 2.0));
    // Swing along the same axis the eyes separate on, so a positive result also
    // confirms the axis is the one stereo would use.
    const float* r = &m[row * 4];
    m[12] += r[0] * amt;
    m[13] += r[1] * amt;
    m[14] += r[2] * amt;
}

void Observe(void* out, void* retAddr, int which) {
    const uintptr_t base = apex::Base();
    const uintptr_t ret = reinterpret_cast<uintptr_t>(retAddr);
    if (!base || ret < base) return;
    const uint32_t rva = static_cast<uint32_t>(ret - base);
    if (rva >= apex::ImageSize()) return;

    Site* s = ClaimSite(rva, which);
    float* m = reinterpret_cast<float*>(out);

    // Orientation is read here; position is written at a DIFFERENT site. The
    // two are not the same object - see apex.h, kCameraBasisSource vs
    // kCameraPositionLever.
    if (rva == apex::kCameraBasisSource) {
        ProbeCameraObject(out, m);
        memcpy(g_camMatrix, m, sizeof(g_camMatrix));
        InterlockedExchange(&g_camSeen, 1);
        // Only used as the eye-separation basis if explicitly asked for - it was
        // MEASURED not to rotate (forward pinned within 14 deg through a full
        // look-around), so separating the eyes along it means a WORLD-FIXED
        // axis, which is correct only while facing one particular direction.
        if (!Cfg().stereo_basis_from_lever) NoteViewBasis(m);
    }
    if (rva == apex::kCameraPositionLever) {
        // THIS site's basis genuinely turns with the player: its forward is a
        // horizontal unit vector that was observed at (0.938,0,0.347),
        // (0.284,0,0.959) and (0.792,0,-0.610) in different samples. Taking the
        // eye separation from here makes it follow the player's facing instead
        // of being pinned to one world direction.
        //
        // Read BEFORE the eye offset is applied, so the basis is the game's own
        // and not one we already moved.
        if (Cfg().stereo_basis_from_lever) NoteViewBasis(m);
        // Probe this object too. THIS is the site whose basis was seen turning
        // with the player in a live run (row0/row2 swinging 32.9 and 145 deg
        // between reports), so if the orientation is stored anywhere it is more
        // likely here than at the basis source - despite the older note above
        // claiming the opposite.
        ProbeLeverObject(out, m);
        NoteCameraHeight(m[13]);   // row3.y - the camera's world height
        WriteYawField(out);
        // Undo mode 2: by now the offset has survived the aim consumer, the
        // weapon placement and this build - late enough to have been used,
        // early enough that a crouch re-anchor never sees it.
        if (Cfg().head_undo_mode == 2 && ApplyPendingUndo(out)) {
            InterlockedIncrement(&g_undoAtBuilder);
        }
        // The view is steered by +0x4C above; the weapon rides THIS matrix, so
        // it needs the same rotation or the two come apart.
        ApplyHeadRotationToLever(m);
        // ApplyHeadRotation is GONE from here. Rewriting these basis rows was
        // the disproven mechanism - it never turned the view (the engine
        // recomputes it from +0x4C/+0x50) but it DID disturb the player, and
        // leaving it switchable in the panel meant one stray setting could
        // silently re-enable a write we had already proven wrong.
        ApplyEyeOffset(m);
        NoteLeverAlive();
        FrameScanNoteCamera();
    }

    // The viewmodel site: sweep it to identify it, or cancel the eye offset it
    // inherited from the camera to give the weapon real depth.
    if (Cfg().weapon_scan) {
        if (rva == (uint32_t)g_scanRva) ApplyViewmodelTest(m);
    } else if (Cfg().weapon_site > 0 && rva == (uint32_t)Cfg().weapon_site) {
        if (Cfg().weapon_test_sweep) {
            ApplyViewmodelTest(m);
        } else if (Cfg().weapon_stereo) {
            ApplyEyeOffsetScaled(m, -Cfg().weapon_depth);
        }
    }

    // The test offset is applied AFTER the engine built the matrix and BEFORE
    // we return to its caller, so the caller sees the modified value.
    const LONG armed = g_armed;
    if (armed == -1 || (s && armed >= 0 && &g_sites[armed] == s)) {
        ApplyTestOffset(m);
    }

    if (!s) return;

    InterlockedIncrement(&s->countTotal);
    InterlockedIncrement(&s->countFrame);

    // Accumulate how much this transform actually MOVES. A camera travels and
    // swings; a static prop does neither, and a shadow cascade swings without
    // travelling the way an eye does.
    const float dx = m[12] - s->prevPos[0];
    const float dy = m[13] - s->prevPos[1];
    const float dz = m[14] - s->prevPos[2];
    const float dist = sqrtf(dx * dx + dy * dy + dz * dz);
    if (dist < 1000.0f) s->movedAccum += dist;   // ignore teleports/first sample
    s->prevPos[0] = m[12];
    s->prevPos[1] = m[13];
    s->prevPos[2] = m[14];

    const float fdot = m[0] * s->m[0] + m[1] * s->m[1] + m[2] * s->m[2];
    if (fdot > -1.5f && fdot < 1.5f) s->basisSwingAccum += (1.0f - fdot);

    memcpy(s->m, m, sizeof(s->m));
}

// The direction vector is the builder's INPUT - it derives the whole basis from
// it. Rotating it here, BEFORE the original runs, is what actually turns the
// camera; rewriting the matrix rows afterwards only edits derived output, which
// is why that barely moved the view.
// What IS the third argument? It was assumed to be a forward direction and
// never checked - and replacing it with a unit vector crashed the game as the
// world loaded, which is exactly what would happen if it is really a LookAt
// TARGET POSITION rather than a direction.
//
// So measure it before touching it: a direction has length ~1 and no relation
// to the camera position; a target sits near the camera in world space and the
// difference between them is the direction.
// Which of the two vectors is up and which is forward? arg3 measured as
// (0,1,0), so arg4 should be the forward - but that gets measured too rather
// than assumed. A forward vector swings as the player looks around; an up
// vector stays pinned near (0,1,0).
// --- camera-object field hunt ---------------------------------------------
//
// The builder's up/forward arguments are constants, yet its OUTPUT basis tracks
// the view - so the orientation must already be inside the camera object passed
// in RCX. This finds which field holds it.
//
// "Which fields changed while looking around" is NOT good enough, and the last
// version of this probe made that mistake: the player's position, every timer
// and every animation cursor change too, so the report was a wall of noise. What
// identifies a yaw field is that it moves WITH the yaw and ONLY with the yaw:
//
//   * measure the view yaw from the matrix the engine just built,
//   * accumulate, per field, the CORRELATION between its change and the yaw's,
//   * and disqualify outright any field that moves while the view is STILL.
//
// A field holding yaw in radians then reports slope ~1.0, one in degrees ~57.3,
// and anything merely busy is either disqualified or shows a low correlation.
// It is also run against BOTH camera sites, because the notes disagree about
// which one carries the orientation and a live measurement settles it.
constexpr int kObjWords = 384;         // 0x00 .. 0x600

// An engine does not store a heading in only one way, and the first version of
// this probe assumed it did. It correlated the CHANGE in a field against the
// CHANGE in yaw, which can only ever find a field holding yaw LINEARLY. A
// matrix or quaternion holds it as a sine or cosine, whose rate of change
// varies with the angle - so those score near zero and get thrown away.
//
// The proof that this mattered: the object's own first 0x40 bytes ARE the
// rotating matrix, and the linear test did not flag them either. A test that
// misses a field we KNOW is there has not proven anything about the fields we
// are looking for.
//
// So correlate the field's VALUE against each of the forms a heading is
// actually stored in. Whichever scores highest names the encoding.
constexpr int kPredictors = 5;
const char* const kPredictorName[kPredictors] = {
    "yaw, linear",              // radians (slope ~1) or degrees (slope ~57.3)
    "sin(yaw)  = matrix/basis",
    "cos(yaw)  = matrix/basis",
    "sin(yaw/2) = quaternion Y",
    "cos(yaw/2) = quaternion W",
};

struct FieldStats {
    double n = 0;
    double sf = 0, sff = 0;
    double sfp[kPredictors] = {0, 0, 0, 0, 0};
    int stillChanges = 0;              // moved while the yaw did not

    // --- pitch hunt -------------------------------------------------------
    // Pitch cannot be found the same way as yaw, because the reference signal
    // does not exist: this matrix is YAW-ONLY (row1 is exactly (0,1,0) in every
    // sample), so there is nothing here to correlate a pitch against.
    //
    // What identifies it instead is BEHAVIOUR while the yaw is held still and
    // the player looks up and down: the pitch field OSCILLATES - it reverses
    // direction repeatedly and stays inside the range an angle can occupy. A
    // timer or an accumulator also changes, but only ever climbs.
    float mn = 0, mx = 0;
    bool  haveRange = false;
    int   reversals = 0;
    int   lastDir = 0;
};

struct ObjProbe {
    const char* name;
    FieldStats f[kObjWords];
    float prev[kObjWords];
    float prevYaw = 0.0f;
    bool  havePrev = false;
    int   samples = 0;
    int   moving = 0;
    int   stillSamples = 0;
    bool  reported = false;
    // Does THIS site's matrix tilt at all? The lever's row1 is (0,1,0) in every
    // sample, i.e. yaw-only - but that is one matrix out of two, and if neither
    // tilts then no transform-builder matrix carries pitch and the search has to
    // move downstream to the render view. Cheap to measure, and it decides which
    // of those two worlds we are in.
    float fwdYmin = 0, fwdYmax = 0, upYmin = 0, upYmax = 0;
    bool  haveTilt = false;
    DWORD lastSample = 0;
    // The predictor sums are the same for every field, so they live here rather
    // than being duplicated 192 times.
    double sp[kPredictors] = {0, 0, 0, 0, 0};
    double spp[kPredictors] = {0, 0, 0, 0, 0};
    double np = 0;
};

ObjProbe g_probeLever{"lever 0x6445CC"};
ObjProbe g_probeBasis{"basis-source 0x6DBDC8"};

// How much of this object is actually readable. An object smaller than the
// window would fault on every sample; SEH would catch it but we would learn
// nothing, so ask the OS rather than guess.
int ReadableWords(const void* obj) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(obj, &mbi, sizeof(mbi))) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    const DWORD ok = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
                     PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & ok)) return 0;
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    const size_t avail = end - reinterpret_cast<uintptr_t>(obj);
    const int words = int(avail / sizeof(float));
    return words > kObjWords ? kObjWords : words;
}

void ReportProbe(ObjProbe& p) {
    p.reported = true;
    COTW_LOG("[obj] === %s: %d samples, %d of them while turning ===",
             p.name, p.samples, p.moving);

    struct Hit { int word; int pred; double r; double slope; };
    Hit hits[24];
    int n = 0;

    for (int i = 0; i < kObjWords; ++i) {
        FieldStats& s = p.f[i];
        // NOT disqualified on "changed while still" any more. That filter threw
        // away EVERYTHING, including the output matrix itself: these are live
        // floats recomputed every frame, so idle sway, breathing and recoil move
        // them by more than 1e-6 constantly. The correlation below is specific
        // enough on its own - a timer does not accidentally track cos(yaw) to
        // r > 0.95 - so noise is now reported as a column instead of a veto.
        if (s.n < 20) continue;
        const double vf = s.sff - s.sf * s.sf / s.n;
        if (vf < 1e-9) continue;                 // constant field

        int best = -1;
        double bestR = 0.0, bestSlope = 0.0;
        for (int k = 0; k < kPredictors; ++k) {
            const double vp = p.spp[k] - p.sp[k] * p.sp[k] / p.np;
            if (vp < 1e-9) continue;
            const double cov = s.sfp[k] - p.sp[k] * s.sf / s.n;
            const double r = cov / sqrt(vp * vf);
            if (fabs(r) > fabs(bestR)) { bestR = r; best = k; bestSlope = cov / vp; }
        }
        if (best < 0 || fabs(bestR) < 0.95) continue;
        if (n < 24) { hits[n].word = i; hits[n].pred = best; hits[n].r = bestR;
                      hits[n].slope = bestSlope; ++n; }
    }

    // The self-check runs FIRST and ALWAYS - including when the list is empty.
    // Last time it was placed after an early return, so the one case it existed
    // to catch (finding nothing) was the one case it never ran in. A negative
    // result is only worth anything if the instrument is known to work.
    bool sawMatrix = false;
    for (int i = 0; i < n; ++i) if (hits[i].word * 4 < 0x30) sawMatrix = true;
    COTW_LOG("[obj]   self-check: the output matrix at +0x00..+0x2F %s",
             sawMatrix ? "WAS found, so the method works and the list below is real"
                       : "was NOT found - THE PROBE IS BROKEN, believe nothing below");

    if (!n) {
        COTW_LOG("[obj]   nothing in the first 0x%X bytes tracks the view yaw in any "
                 "form (linear, matrix or quaternion).", kObjWords * 4);
        return;
    }

    for (int i = 0; i < n; ++i) {
        const int off = hits[i].word * 4;
        const char* note = "";
        if (off < 0x40) note = "  (the output matrix itself - expected)";
        else if (hits[i].pred == 0 && fabs(fabs(hits[i].slope) - 1.0) < 0.1)
            note = "  <== YAW IN RADIANS - the write target";
        else if (hits[i].pred == 0 && fabs(fabs(hits[i].slope) - 57.2958) < 4.0)
            note = "  <== YAW IN DEGREES - the write target";
        else if (hits[i].pred >= 3) note = "  <== quaternion component";
        else note = "  <== a second basis/matrix";
        COTW_LOG("[obj]   +0x%03X  r=%+.3f  best fit: %-26s slope %+.4f  noise %d%s",
                 off, hits[i].r, kPredictorName[hits[i].pred], hits[i].slope,
                 p.f[hits[i].word].stillChanges, note);
    }

    // --- does this matrix tilt at all? -------------------------------------
    const float fwdSwing = p.fwdYmax - p.fwdYmin;
    COTW_LOG("[obj]   matrix tilt: forward.y %.3f..%.3f (swing %.3f), up.y %.3f..%.3f "
             "-> this matrix %s",
             p.fwdYmin, p.fwdYmax, fwdSwing, p.upYmin, p.upYmax,
             fwdSwing > 0.15f ? "DOES carry pitch"
                              : "is YAW-ONLY, pitch is applied somewhere else");

    // --- pitch candidates --------------------------------------------------
    COTW_LOG("[obj]   --- PITCH candidates (straddling zero, oscillating while yaw still) ---");
    struct P { int word; float range; int rev; float mn, mx; };
    P best[10];
    int pn = 0;
    for (int i = 0; i < kObjWords; ++i) {
        FieldStats& s = p.f[i];
        if (!s.haveRange || s.reversals < 4) continue;      // must go BACK and forth
        const float range = s.mx - s.mn;
        if (range < 0.02f) continue;
        // A look-up/down axis STRADDLES ZERO - the player looks down as well as
        // up. Without this the list filled with frame-timing values (0..0.0333
        // = 1/30, 0..30) that oscillate perfectly well but only ever sit on one
        // side of zero, and none of which was ever a pitch.
        if (!(s.mn < -0.02f && s.mx > 0.02f)) continue;
        // An angle, in radians or degrees. Anything outside this is a counter,
        // a position or a timer, not a look direction.
        const bool radians = fabsf(s.mn) < 3.3f && fabsf(s.mx) < 3.3f;
        const bool degrees = fabsf(s.mn) < 190.0f && fabsf(s.mx) < 190.0f;
        if (!radians && !degrees) continue;
        if (i * 4 < 0x40) continue;                          // the matrix itself
        // Insert sorted by how many reversals it survived - the strongest
        // signal that this is a look axis being swung back and forth.
        int at = pn;
        while (at > 0 && best[at - 1].rev < s.reversals) --at;
        if (pn < 10) ++pn;
        for (int k = (pn < 10 ? pn - 1 : 9); k > at; --k) best[k] = best[k - 1];
        if (at < 10) { best[at].word = i; best[at].range = range;
                       best[at].rev = s.reversals; best[at].mn = s.mn; best[at].mx = s.mx; }
    }
    if (!pn) {
        COTW_LOG("[obj]   none - either the vertical look was not exercised, or pitch "
                 "is not stored on this object.");
    }
    for (int i = 0; i < pn; ++i) {
        const bool rad = fabsf(best[i].mn) < 3.3f && fabsf(best[i].mx) < 3.3f;
        COTW_LOG("[obj]   +0x%03X  range %.4f (%.4f .. %.4f)  %d reversals  %s",
                 best[i].word * 4, best[i].range, best[i].mn, best[i].mx, best[i].rev,
                 rad ? "<-- plausible PITCH in radians" : "<-- plausible pitch in degrees");
    }
}

void ProbeObject(const void* obj, const float* outMatrix, ObjProbe& p) {
    if (!Cfg().camera_object_probe || !obj || !outMatrix || p.reported) return;

    // 20 Hz: fast enough that one turn yields many samples, slow enough that a
    // hot hook does not notice.
    const DWORD now = GetTickCount();
    if (now - p.lastSample < 50) return;
    p.lastSample = now;

    // Yaw of the matrix the engine just built. Row 0 is horizontal and turns
    // with the player, so atan2 over it is a clean, continuous heading.
    const float yaw = atan2f(outMatrix[2], outMatrix[0]);

    const int words = ReadableWords(obj);
    if (words < 16) return;

    float cur[kObjWords];
    __try { memcpy(cur, obj, size_t(words) * sizeof(float)); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return; }

    if (!p.havePrev) {
        memcpy(p.prev, cur, size_t(words) * sizeof(float));
        p.prevYaw = yaw;
        p.havePrev = true;
        COTW_LOG("[obj] %s: watching 0x%X bytes - LOOK AROUND for ~20 seconds",
                 p.name, words * 4);
        return;
    }

    // Shortest way round the circle, so crossing +/-180 degrees is not read as
    // a near-full turn and does not poison every correlation at once.
    float dy = yaw - p.prevYaw;
    while (dy > 3.14159265f) dy -= 6.28318531f;
    while (dy < -3.14159265f) dy += 6.28318531f;

    const bool still = fabsf(dy) < 0.0008f;     // ~0.05 degrees
    const bool turning = fabsf(dy) > 0.004f;    // ~0.25 degrees

    ++p.samples;
    if (turning) ++p.moving;
    if (still) ++p.stillSamples;

    // The vertical components of the basis. A matrix carrying pitch has a
    // forward whose Y sweeps as the player looks up and down; a yaw-only matrix
    // keeps forward.y at 0 and up.y pinned at 1.
    const float fy = outMatrix[1], uy = outMatrix[5];
    if (!p.haveTilt) { p.fwdYmin = p.fwdYmax = fy; p.upYmin = p.upYmax = uy; p.haveTilt = true; }
    if (fy < p.fwdYmin) p.fwdYmin = fy;
    if (fy > p.fwdYmax) p.fwdYmax = fy;
    if (uy < p.upYmin) p.upYmin = uy;
    if (uy > p.upYmax) p.upYmax = uy;

    // The forms a heading gets stored in. Correlating the VALUE against these
    // finds a matrix or quaternion, which correlating deltas never could.
    const double pred[kPredictors] = {
        double(yaw), sin(double(yaw)), cos(double(yaw)),
        sin(double(yaw) * 0.5), cos(double(yaw) * 0.5),
    };
    p.np += 1;
    for (int k = 0; k < kPredictors; ++k) {
        p.sp[k] += pred[k];
        p.spp[k] += pred[k] * pred[k];
    }

    for (int i = 0; i < words; ++i) {
        const float a = p.prev[i], b = cur[i];
        if (!isfinite(b)) continue;
        FieldStats& s = p.f[i];
        // Disqualification uses the CHANGE (does it move on its own?), while the
        // correlation uses the VALUE. Two different questions, two measures.
        if (still && isfinite(a) && fabsf(b - a) > 1e-6f) ++s.stillChanges;

        s.n += 1;
        s.sf += b;
        s.sff += double(b) * b;
        for (int k = 0; k < kPredictors; ++k) s.sfp[k] += double(b) * pred[k];

        // Pitch evidence is gathered only while the YAW is holding still, so a
        // field that merely follows the heading cannot score here.
        if (still && isfinite(a)) {
            if (!s.haveRange) { s.mn = s.mx = b; s.haveRange = true; }
            if (b < s.mn) s.mn = b;
            if (b > s.mx) s.mx = b;
            const float d = b - a;
            if (fabsf(d) > 1e-5f) {
                const int dir = d > 0 ? 1 : -1;
                if (s.lastDir != 0 && dir != s.lastDir) ++s.reversals;
                s.lastDir = dir;
            }
        }
    }

    memcpy(p.prev, cur, size_t(words) * sizeof(float));
    p.prevYaw = yaw;

    // BOTH phases must have happened before reporting: turning feeds the yaw
    // correlation, holding-the-yaw-still-while-looking-up-and-down feeds the
    // pitch hunt. Reporting after the turning alone (as it did) would print an
    // empty pitch section and read as "pitch is not on this object".
    // The fallback matters as much as the main condition: the basis-source site's
    // matrix barely yaws at all, so it can never reach a "turning" quota and
    // would silently never report - which is exactly what happened on the last
    // three runs. A plain sample count guarantees every site eventually speaks.
    if ((p.moving >= 150 && p.stillSamples >= 300) || p.samples >= 1000) ReportProbe(p);
}

void ProbeCameraObject(const void* obj, const float* outMatrix) {
    ProbeObject(obj, outMatrix, g_probeBasis);
}

void ProbeLeverObject(const void* obj, const float* outMatrix) {
    ProbeObject(obj, outMatrix, g_probeLever);
}

void ProbeArgs(const float* pos, const float* a3, const float* a4, uint32_t rva) {
    static int logged = 0;
    if (logged >= 8 || !pos) return;
    ++logged;

    auto len = [](const float* v) {
        return v ? sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) : 0.0f;
    };

    COTW_LOG("[args] caller 0x%08X  pos(%.2f, %.2f, %.2f)", rva, pos[0], pos[1], pos[2]);
    if (a3) {
        COTW_LOG("[args]    arg3 (r8) = (%7.4f, %7.4f, %7.4f)  len %.4f", a3[0], a3[1],
                 a3[2], len(a3));
    }
    if (a4) {
        COTW_LOG("[args]    arg4 (r9) = (%7.4f, %7.4f, %7.4f)  len %.4f", a4[0], a4[1],
                 a4[2], len(a4));
    }
}

// arg4 (r9) should be the forward vector - the builder's real orientation
// INPUT. Rotating it before the original runs is the head-tracking lever.
const float* MaybeRotateForward(const float* fwd, const float* up, const float* pos,
                                void* retAddr, float* scratch) {
    const uintptr_t base = apex::Base();
    const uintptr_t ret = reinterpret_cast<uintptr_t>(retAddr);
    if (!base || ret < base) return fwd;
    const uint32_t rva = static_cast<uint32_t>(ret - base);

    const bool isCamera = (rva == apex::kCameraBasisSource) ||
                          (rva == apex::kCameraPositionLever);
    if (!isCamera) return fwd;

    ProbeArgs(pos, up, fwd, rva);

    // The rotation that used to happen here is gone too. This argument was
    // MEASURED to be the constant (0,0,-1) at both camera sites, so rotating it
    // could never have done anything - it was the second disproven mechanism
    // sitting behind a switch.
    (void)scratch;
    return fwd;
}

void* __fastcall Hook_BuildA(void* out, const float* pos, const float* up,
                             const float* fwd) {
    void* ret = _ReturnAddress();
    float scratch[3];
    void* r = o_BuildA(out, pos, up, MaybeRotateForward(fwd, up, pos, ret, scratch));
    Observe(out, ret, 0);
    return r;
}

void* __fastcall Hook_BuildB(void* out, const float* pos, const float* up,
                             const float* fwd) {
    void* ret = _ReturnAddress();
    float scratch[3];
    void* r = o_BuildB(out, pos, up, MaybeRotateForward(fwd, up, pos, ret, scratch));
    Observe(out, ret, 1);
    return r;
}

bool g_installed = false;

}  // namespace

bool PlayerCameraActive() {
    const LONG t = g_leverTick;
    if (!t) return false;
    return (GetTickCount() - (DWORD)t) < 250;
}

bool InstallCameraProbe() {
    if (g_installed) return true;
    if (!apex::FingerprintMatches()) {
        COTW_LOG("[cam] probe not installed - build fingerprint does not match, so "
                 "the transform-builder RVAs are not valid for this exe");
        return false;
    }

    void* a = apex::Rva(apex::kBuildTransformA);
    void* b = apex::Rva(apex::kBuildTransformB);
    if (!a || !b) {
        COTW_LOG("[cam] probe: could not resolve builder RVAs");
        return false;
    }

    MH_STATUS s = MH_CreateHook(a, &Hook_BuildA, (void**)&o_BuildA);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
        COTW_LOG("[cam] MH_CreateHook(BuildA @0x%X) failed: %d", apex::kBuildTransformA, (int)s);
        return false;
    }
    s = MH_CreateHook(b, &Hook_BuildB, (void**)&o_BuildB);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
        COTW_LOG("[cam] MH_CreateHook(BuildB @0x%X) failed: %d", apex::kBuildTransformB, (int)s);
        return false;
    }
    if (MH_EnableHook(a) != MH_OK || MH_EnableHook(b) != MH_OK) {
        COTW_LOG("[cam] MH_EnableHook failed for the transform builders");
        return false;
    }

    // The one instruction that makes our offset permanent: neutralise it there.
    if (void* sc = apex::Rva(apex::kSaveAimCopy)) {
        MH_STATUS ss = MH_CreateHook(sc, &Hook_SaveAimCopy, (void**)&o_SaveAimCopy);
        if ((ss == MH_OK || ss == MH_ERROR_ALREADY_CREATED) &&
            MH_EnableHook(sc) == MH_OK) {
            COTW_LOG("[cam] scripted-camera aim save hooked @0x%X - the engine will "
                     "save its own aim, never ours", apex::kSaveAimCopy);
        } else {
            COTW_LOG("[cam] MH_CreateHook(kSaveAimCopy) failed: %d - the crouch drift "
                     "will come back", (int)ss);
        }
    }

    // Rotate the view matrix instead of writing an angle - the only mechanism
    // left after every angle-write variant was measured to drift.
    // The matrix -> angle direction, which is where a rotated view basis gets
    // stored as the player's permanent aim.
    if (void* me = apex::Rva(apex::kMatrixToEuler)) {
        MH_STATUS ms = MH_CreateHook(me, &Hook_MatrixToEuler, (void**)&o_MatrixToEuler);
        if ((ms == MH_OK || ms == MH_ERROR_ALREADY_CREATED) && MH_EnableHook(me) == MH_OK) {
            COTW_LOG("[cam] MatrixToEuler hooked @0x%X - watching the one call that "
                     "decomposes a matrix straight into the stored aim",
                     apex::kMatrixToEuler);
        } else {
            COTW_LOG("[cam] MH_CreateHook(kMatrixToEuler) failed: %d", (int)ms);
        }
    }

    if (void* em = apex::Rva(apex::kEulerToMatrix)) {
        MH_STATUS es = MH_CreateHook(em, &Hook_EulerToMatrix, (void**)&o_EulerToMatrix);
        if ((es == MH_OK || es == MH_ERROR_ALREADY_CREATED) &&
            MH_EnableHook(em) == MH_OK) {
            COTW_LOG("[cam] euler->matrix hooked @0x%X - the view can be rotated "
                     "without touching the engine's aim", apex::kEulerToMatrix);
        } else {
            COTW_LOG("[cam] MH_CreateHook(kEulerToMatrix) failed: %d", (int)es);
        }
    }

    // The aim consumer, so the head offset can be applied AHEAD of the weapon.
    if (void* ac = apex::Rva(apex::kAimConsumer)) {
        MH_STATUS as = MH_CreateHook(ac, &Hook_AimConsumer, (void**)&o_AimConsumer);
        if ((as == MH_OK || as == MH_ERROR_ALREADY_CREATED) &&
            MH_EnableHook(ac) == MH_OK) {
            COTW_LOG("[cam] aim consumer hooked @0x%X - the head offset can now be "
                     "applied before the weapon is placed", apex::kAimConsumer);
        } else {
            COTW_LOG("[cam] MH_CreateHook(kAimConsumer @0x%X) failed: %d - the early "
                     "write cannot work, the weapon will not follow",
                     apex::kAimConsumer, (int)as);
        }
    }

    g_installed = true;
    COTW_LOG("[cam] transform-builder probe installed (A @0x%X, B @0x%X)",
             apex::kBuildTransformA, apex::kBuildTransformB);
    COTW_LOG("[cam] walk around and look about for ~20 s; the call site with a high "
             "'moved' AND a high 'swing' at ~1 call/frame is the player camera");
    return true;
}

void RemoveCameraProbe() {
    ApplyReseedPatch(false);   // never leave patched bytes behind
    ApplyReseedYawPatch(false);

    if (!g_installed) return;
    void* a = apex::Rva(apex::kBuildTransformA);
    void* b = apex::Rva(apex::kBuildTransformB);
    if (a) MH_DisableHook(a);
    if (b) MH_DisableHook(b);
    g_installed = false;
}

void ViewmodelScanTickPublic() {
    // The re-seed patch is applied from here because this already runs every
    // frame and the setting can be toggled live from the panel. Applying it is
    // idempotent - ApplyReseedPatch returns immediately when nothing changed.
    // INDEPENDENT, and that matters: chaining the second to the first made one
    // combination unreachable - live yaw blocked while the accumulators are left
    // alone - and that is the combination the evidence now points at.
    //
    // Measured: blocking the ACCUMULATORS is what causes the transient swing.
    // With both patches off there is no swing (and the ratchet returns); with the
    // accumulators blocked and the live yaw free, the swing is there. The owner's
    // other observation explains it - on a fresh launch, before the right stick
    // has been touched, there is no swing at all. The accumulator is ~0 then, so
    // blocking a write of ~0 changes nothing. Once it holds a real value, blocking
    // the write leaves a stale number the engine folds into its next delta, and
    // the size of the lurch is the size of what we prevented.
    //
    // The two writes are a consistent PAIR set by the same routine, and blocking
    // one while allowing the other breaks the invariant between them.
    // The detour and the NOP are two answers to the same instruction, so
    // only one may be installed at a time - the NOP would overwrite the
    // jump, or the jump the NOP, and the loser would be a half-written
    // instruction. The detour wins when it is on.
    if (Cfg().head_tracking && Cfg().head_reseed_detour) {
        ApplyReseedPatch(false);
        InstallReseedCave();
        // Hold breath re-seeds through its own deferred latch inside the aim
        // update - a different function entirely, which is why the cave above
        // never fixed it. Same switch: this is the same bug by another route.
        if (Cfg().head_reseed_holdbreath) InstallHoldBreathCaves();
        else RemoveHoldBreathCaves();
        UpdateReseedCorrection();
    } else {
        RemoveReseedCave();
        RemoveHoldBreathCaves();   // off means off - the engine's bytes back
        ApplyReseedPatch(Cfg().head_tracking && Cfg().head_keep_aim_on_reseed);
    }

    // The FOV lever is its own feature and must not inherit the head-tracking
    // gating above - it is installed once and left alone; k is read per call.
    if (Cfg().tier1_fov) InstallCamModFovLever();
    ApplyReseedYawPatch(Cfg().head_tracking && Cfg().head_block_reseed_yaw);
    ViewmodelScanTick();
}

void CameraProbeTick(int everyFrames) {
    if (!g_installed || everyFrames <= 0) return;

    static uint64_t frame = 0;
    ++frame;

    const LONG n = g_siteCount;

    // Hotkeys.  Separated so each press changes exactly ONE thing - a run where
    // two variables moved at once wastes the session it took to set up.
    //   F8  toggle the offset on/off   (the A/B that makes a jump obvious)
    //   F9  next basis row  0 -> 1 -> 2   (which row is right/up/forward)
    //   F10 next call site
    static LONG s_site = -1;            // index of the site under test
    {
        auto pressed = [](int vk) {
            static bool was[256] = {};
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool edge = down && !was[vk & 0xFF];
            was[vk & 0xFF] = down;
            return edge;
        };

        if (s_site < 0 && n > 0) {
            // Default to the site the last session proved actually moves the
            // picture, if it is present in this run's table.
            for (LONG i = 0; i < n; ++i) {
                if (g_sites[i].callerRva == apex::kCameraMoverCaller) { s_site = i; break; }
            }
            if (s_site < 0) s_site = 0;
        }

        if (pressed(VK_F10) && n > 0) {
            s_site = (s_site + 1) % n;
            COTW_LOG("[cam] F10: site under test -> 0x%08X (fn %c)",
                     g_sites[s_site].callerRva, g_sites[s_site].which ? 'B' : 'A');
        }
        if (pressed(VK_F9)) {
            int a = Cfg().camera_test_axis + 1;
            if (a > 3) a = 1;
            Cfg().camera_test_axis = a;
            COTW_LOG("[cam] F9: axis -> row%d", a - 1);
        }
        if (pressed(VK_F8)) {
            const LONG next = (g_armed == -2) ? s_site : -2;
            InterlockedExchange(&g_armed, next);
            if (next == -2) {
                COTW_LOG("[cam] F8: OFF");
            } else {
                COTW_LOG("[cam] F8: ON  - caller 0x%08X, %.2f m along row%d",
                         g_sites[s_site].callerRva, Cfg().camera_test_amount,
                         Cfg().camera_test_axis - 1);
            }
        }
    }

    if ((frame % (uint64_t)everyFrames) != 0) {
        // still roll the per-frame counters so "calls per frame" means something
        if ((frame % 1) == 0) {
            for (LONG i = 0; i < n && i < kMaxSites; ++i) {
                InterlockedExchange(&g_sites[i].countFrame, 0);
            }
        }
        return;
    }

    COTW_LOG("[cam] ---- transform call sites after %llu frames (%ld sites) ----",
             (unsigned long long)frame, n);
    COTW_LOG("[cam]   caller-rva   fn  calls/frame   total    moved     swing   "
             "pos(x,y,z)              fwd(x,y,z)");
    for (LONG i = 0; i < n && i < kMaxSites; ++i) {
        Site& s = g_sites[i];
        if (s.claimed != 2) continue;
        const float perFrame = float(s.countTotal) / float(frame);
        // Only the plausible ones - a site called 500x a frame is not a camera,
        // and printing 96 rows makes the interesting one impossible to see.
        if (perFrame > 8.0f) continue;
        if (s.movedAccum < 0.01f && s.basisSwingAccum < 0.01f) continue;
        COTW_LOG("[cam]   0x%08X   %c   %8.2f  %7ld  %8.2f  %8.3f   "
                 "(%9.2f,%9.2f,%9.2f)  (%6.3f,%6.3f,%6.3f)",
                 s.callerRva, s.which ? 'B' : 'A', perFrame, s.countTotal,
                 s.movedAccum, s.basisSwingAccum,
                 s.m[12], s.m[13], s.m[14], s.m[0], s.m[1], s.m[2]);
    }
    for (LONG i = 0; i < n && i < kMaxSites; ++i) {
        InterlockedExchange(&g_sites[i].countFrame, 0);
    }

    // The player camera's full matrix. Row 0/1/2 are the basis; which of them is
    // right/up/forward is answered by the axis test, not by assumption - a
    // wrong-axis or transposed guess is impossible to reason about from code.
    if (g_camSeen) {
        const float* m = g_camMatrix;
        COTW_LOG("[cam] PLAYER CAMERA (caller 0x%08X) matrix:", apex::kCameraBuildCaller);
        COTW_LOG("[cam]   row0 [% 8.4f % 8.4f % 8.4f]  w=% .2f", m[0], m[1], m[2], m[3]);
        COTW_LOG("[cam]   row1 [% 8.4f % 8.4f % 8.4f]  w=% .2f", m[4], m[5], m[6], m[7]);
        COTW_LOG("[cam]   row2 [% 8.4f % 8.4f % 8.4f]  w=% .2f", m[8], m[9], m[10], m[11]);
        COTW_LOG("[cam]   pos  [% 8.2f % 8.2f % 8.2f]  w=% .2f", m[12], m[13], m[14], m[15]);
        const LONG armed = g_armed;
        if (armed == -2) {
            COTW_LOG("[cam]   test offset OFF (press F8 to arm)");
        } else if (armed == -1) {
            COTW_LOG("[cam]   *** TEST OFFSET on ALL sites: %.2f m along row%d ***",
                     Cfg().camera_test_amount, Cfg().camera_test_axis - 1);
        } else if (armed < n) {
            COTW_LOG("[cam]   *** TEST OFFSET on caller 0x%08X only: %.2f m along row%d ***",
                     g_sites[armed].callerRva, Cfg().camera_test_amount,
                     Cfg().camera_test_axis - 1);
        }
    }
}

}  // namespace cotwvr
