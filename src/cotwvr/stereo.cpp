#include "stereo.h"

#include "apex.h"
#include "cbscan.h"
#include "config.h"
#include "headtrack.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>
#include <cstring>

namespace cotwvr {
namespace {

volatile LONG g_eye = 0;              // eye the current frame is being built for

// The view's world basis, refreshed every frame from the camera site.  Racy on
// purpose: a one-frame-stale basis is invisible, a lock in a hot hook is not.
float g_viewBasis[16];
volatile LONG g_haveBasis = 0;

volatile LONG g_basisHits = 0;        // per-report counters
volatile LONG g_leverHits = 0;
volatile LONG g_leverApplied = 0;

}  // namespace

float g_eyeWorldPos[2][3] = {};
bool  g_haveEyeWorldPos[2] = {false, false};

int CurrentRenderEye() { return (int)g_eye; }

void AdvanceRenderEye() {
    InterlockedExchange(&g_eye, g_eye ? 0 : 1);
}

namespace { volatile LONG g_fullRate = 0; }

void SetRenderEye(int eye) { InterlockedExchange(&g_eye, eye ? 1 : 0); }
bool FullRateActive() { return g_fullRate != 0; }
void SetFullRateActive(bool on) { InterlockedExchange(&g_fullRate, on ? 1 : 0); }

bool StereoActive() {
    return Cfg().stereo && g_haveBasis;
}

void NoteViewBasis(const float* m) {
    memcpy(g_viewBasis, m, sizeof(g_viewBasis));
    InterlockedExchange(&g_haveBasis, 1);
    InterlockedIncrement(&g_basisHits);
}

bool CurrentCameraMatrix(float* out16) {
    if (!out16 || !g_haveBasis) return false;
    memcpy(out16, g_viewBasis, sizeof(float) * 16);
    return true;
}

// *** 6DoF: THE HEAD'S REAL MOVEMENT, IN THE CAMERA'S OWN BASIS. ***
//
// The rotation half of head tracking steers the engine's own yaw/pitch FIELDS,
// because rewriting the matrix it derives from them is undone the next time it
// recomputes. Position has no such field - the game has no concept of a player
// leaning - so it is added here, to the camera's world position row, exactly
// where the stereo eye offset already goes. Same row, same basis, same site,
// which is why this can be trusted: the eye offset has been correct in the
// headset for weeks and this is arithmetic of the identical shape.
//
// It is applied ONLY at the world-camera site, never through
// ApplyEyeOffsetScaled - the weapon calls that with a NEGATIVE scale to cancel
// the eye shift it inherits, and a negative 6DoF would leave the gun standing
// in world space while the player leans past it.
//
// Axes: the head offset arrives in OpenXR's local space (+x right, +y up, -z
// forward) and has to be expressed in the game's basis rows. Row 1 is up - the
// axis test settled that - and eye_axis_row names right, so forward is the row
// neither of those claims. The SIGNS of right and forward were never settled by
// measurement, so each has a switch, exactly as the eye separation did before
// one headset session pinned it.
// Local wrap, so this file does not depend on headtrack's internals for one
// subtraction that must not fling the offset a full turn near +/-180.
static float WrapPiF(float a) {
    const float kPi = 3.14159265358979f, kTwoPi = 6.28318530717959f;
    while (a > kPi) a -= kTwoPi;
    while (a < -kPi) a += kTwoPi;
    return a;
}

void ApplySixDof(float* m) {
    const Config& c = Cfg();
    if (!c.six_dof || !g_haveBasis || !m) return;

    float h[3];
    if (!HeadPositionMetres(h)) return;
    float hyLogged = 0.0f;      // the yaw actually cancelled, for the log below

    // *** A LEAN IS HORIZONTAL, AND IT BELONGS TO THE BODY. ***
    //
    // (Rewritten after the second headset run. What was here before removed the
    // head's yaw AND pitch from the offset and then applied it along the camera
    // basis - which still carries the STICK's pitch. Two faults came out of
    // that, and the owner found both: sideways leans stopped landing sideways
    // once the head was turned, and "if i move my stick up and then bring my
    // view down with my head then rotate, the rotation will be wrong ... feels
    // i am going up and down with rotation".)
    //
    // A leaning body does not tilt with where it is looking. Lean forward while
    // looking at your boots and you move HORIZONTALLY forward; duck and you go
    // straight DOWN the world's vertical, not the camera's. So the frame the
    // offset is applied in must be gravity-aligned:
    //
    //   up      = the world's up, which this engine keeps in +Y (the camera
    //             height this file already reads is row3.y)
    //   right   = the camera's right with its vertical part removed
    //   forward = the camera's forward with its vertical part removed
    //
    // That single change takes the stick's pitch out of the movement as well,
    // because it takes ALL pitch out - and the stick's pitch was never
    // removable by cancelling head angles, which is why the old approach could
    // not have been finished by tuning.
    //
    // What still has to be cancelled is the head's own YAW, and only its yaw:
    // the horizontal frame above is turned by the stick AND by the head, while
    // the head offset arrives in room space where turning your head does not
    // change which way right points. One angle, one rotation, about the
    // vertical.
    //
    // The camera basis below is the BODY's facing turned by the HEAD's own yaw
    // and pitch - the engine builds it from the fields this mod writes. The head
    // position, meanwhile, arrives in room space, where turning your head does
    // not change which way "right" points.
    //
    // Projecting a room-space offset onto a head-rotated basis rotates the
    // offset every time the head turns, and an offset that rotates about the
    // camera is an ORBIT: the player swings around a point somewhere off to the
    // side instead of turning on the spot. That is exactly what the owner
    // reported - "it seems the center is somewhere else and i rotate on that
    // center, not the player center" - and why the stick made it worse, since
    // the stick turns the basis too.
    //
    // So the head's own rotation is taken back out first:
    //     want    R_body . p
    //     have    R_body . R_headYaw . R_headPitch . X
    //     so      X = R_headPitch^-1 . R_headYaw^-1 . p
    // What is left is the body's facing alone, which is the frame a lean is
    // actually measured in - and the stick, which turns the body, then rotates
    // the lean with the player exactly as it should.
    if (c.six_dof_body_frame) {
        // *** CANCEL THE HEAD'S ABSOLUTE ROOM YAW - the whole of it. ***
        //
        // Prior art settles the shape of this, and it is worth naming because
        // three sessions were spent circling it. The F.E.A.R. VR mod (DR-89,
        // open source, OpenXR) does the conversion in one line:
        //
        //     centerDelta = Rotate(inverseRecenter, current - recenter)
        //
        // - the room offset is rotated by the INVERSE OF THE RECENTRE pose,
        // yaw-only. Its recentre stores a yaw-only pose for the stated reason
        // that "a VR recenter must only choose which horizontal direction is
        // forward", and its offsets drop pitch and roll so that "looking up
        // must not reduce forward speed, and head roll must never introduce
        // strafing" - which is the horizontal frame built below, arrived at
        // independently and confirmed here.
        //
        // Our basis is the CAMERA's, which already carries the head's
        // recentre-relative yaw, so we owe both halves: the inverse recentre
        // rotation AND the head-relative yaw. Those two sum to the head's
        // ABSOLUTE room yaw, so it is one rotation, by the whole angle.
        //
        // Cancelling only the recentre-relative part - which is what the last
        // three builds did - leaves the mapping out by the direction the player
        // was facing when they last recentred. Square to the room, that is
        // zero and everything looks right; anywhere else it is a constant
        // rotation that changes at every recentre. "Leaning left does not
        // always lean left" is exactly what that feels like.
        // *** WHICH FACING TO CANCEL IS A SETTING, BECAUSE IT IS A MEASUREMENT.
        //
        // The right answer depends on whether the basis this offset is finally
        // applied along turns with the head or only with the stick, and that
        // belongs to engine code we do not own. Two builds read it two different
        // ways and each fixed one case while breaking another, so it is now
        // chosen by `six_dof_frame_mode` and settled by one session in the
        // headset rather than by another argument on paper.
        //
        // The owner's own analysis of mode 0, which is exactly right and is what
        // mode 1 exists to fix: "it seems the game remembers the original
        // position and direction after centering and makes my character move
        // based on that; rotating my head and whole body does not change that
        // original state."
        float a = 0.0f, a0 = 0.0f;
        HeadFrameAngles(&a, &a0);
        float psi = 0.0f;
        switch (c.six_dof_frame_mode) {
            case 1:  psi = a; break;                 // absolute head facing
            case 2:  psi = WrapPiF(a - a0); break;   // head relative to recentre
            case 3:  psi = 0.0f; break;              // no rotation at all
            default: psi = a0; break;                // the recentre facing
        }
        hyLogged = psi;

        float body[3];
        if (HeadRoomByYaw(h, body, psi)) {
            h[0] = body[0];
            h[1] = body[1];
            h[2] = body[2];
            // A mirror, not a rotation - the last resort if the head basis ever
            // turns out to be the other handedness. A reflection is what makes a
            // lean correct in two opposite directions and wrong in the other
            // two, so if that symptom EVER returns, this is the switch.
            if (c.six_dof_body_frame_invert) h[0] = -h[0];
        }
    }

    // *** A QUARTER TURN IS ITS OWN KIND OF WRONG, AND IT NEEDS ITS OWN DIAL. ***
    //
    // Owner, third run: "going left with my body moving the character forward
    // and going right the opposite ... up and down seems the only behavior
    // works fine". Vertical right, horizontal rotated by ninety degrees - which
    // no combination of the invert switches can express, because inverting is a
    // half turn.
    //
    // A quarter-turn error means the room's zero direction and the direction
    // the player actually sits facing disagree by that much: OpenXR's local
    // space is pinned to the ROOM, not to the chair, and recentring rotates the
    // VIEW without rotating the frame the position is measured in. How far out
    // it is depends on which way the guardian was drawn, so it cannot be
    // derived - only measured, once, by the person in the headset.
    //
    // Hence a dial in quarter turns rather than more guessing. It subsumes the
    // two horizontal inverts as well: 180 is both of them, 90 and 270 are what
    // they could never reach.
    if (c.six_dof_yaw_offset_deg != 0) {
        const float a = (float)c.six_dof_yaw_offset_deg * 0.01745329252f;
        const float ca = cosf(a), sa = sinf(a);
        const float x = h[0] * ca - h[2] * sa;
        const float z = h[0] * sa + h[2] * ca;
        h[0] = x;
        h[2] = z;
    }

    int rightRow = c.eye_axis_row;
    if (rightRow < 0 || rightRow > 2) rightRow = 0;
    const int upRow = 1;
    // Whichever row is left over. Written as a search rather than 3-row-minus
    // arithmetic so it stays right if eye_axis_row is ever 1.
    int fwdRow = 0;
    for (int i = 0; i < 3; ++i) {
        if (i != rightRow && i != upRow) { fwdRow = i; break; }
    }

    // *** FLATTEN THE FRAME. ***
    //
    // World up is +Y in this engine - the camera height this file reads is
    // row3.y - so the horizontal part of any basis row is the row with its Y
    // dropped. Renormalised, because a camera looking 60 degrees down leaves a
    // forward row only half as long once flattened, and a short axis would make
    // the lean shrink as the player looked down.
    auto flatten = [](const float* v, float* out) {
        out[0] = v[0];
        out[1] = 0.0f;
        out[2] = v[2];
        const float len = sqrtf(out[0] * out[0] + out[2] * out[2]);
        if (len < 1e-4f) return false;      // pointing straight up or down
        out[0] /= len;
        out[2] /= len;
        return true;
    };

    float R[3], F[3];
    const float U[3] = {0.0f, 1.0f, 0.0f};
    const bool okR = flatten(&g_viewBasis[rightRow * 4], R);
    bool okF = flatten(&g_viewBasis[fwdRow * 4], F);
    // Straight up or straight down: forward has no horizontal part left, but
    // right always does (roll is not applied to this camera), so forward is
    // recovered from it. up x right = forward for this handedness; if it ever
    // comes out backwards that is what six_dof_invert_z is for.
    if (!okF && okR) {
        F[0] = U[1] * R[2] - U[2] * R[1];
        F[1] = U[2] * R[0] - U[0] * R[2];
        F[2] = U[0] * R[1] - U[1] * R[0];
        okF = true;
    }
    if (!okR || !okF) return;               // degenerate basis - do nothing

    // Scale first, then clamp - a limit applied to the raw movement would let a
    // high scale walk the camera through a wall while the reading still looked
    // reasonable.
    const float s = c.six_dof_scale;
    float dx = h[0] * s * (c.six_dof_invert_x ? -1.0f : 1.0f);
    float dy = h[1] * s * (c.six_dof_invert_y ? -1.0f : 1.0f);
    // -z is forward in OpenXR, so the natural mapping already carries a minus.
    float dz = -h[2] * s * (c.six_dof_invert_z ? -1.0f : 1.0f);

    // *** A LIMIT, BECAUSE THE GAME HAS NO IDEA THIS IS HAPPENING. ***
    //
    // Nothing here collides, so a big enough offset puts the camera inside a
    // rock or through a hide's wall - and the engine will happily render the
    // inside of the world. A lean is tens of centimetres; anything past the
    // limit is either a room-scale walk the game cannot follow or a tracking
    // glitch, and both are better stopped at the edge than obeyed.
    const float lim = (c.six_dof_limit_m > 0.01f) ? c.six_dof_limit_m : 0.60f;
    auto clampf = [lim](float v) { return v < -lim ? -lim : (v > lim ? lim : v); };
    dx = clampf(dx);
    dy = clampf(dy);
    dz = clampf(dz);

    m[12] += R[0] * dx + U[0] * dy + F[0] * dz;
    m[13] += R[1] * dx + U[1] * dy + F[1] * dz;
    m[14] += R[2] * dx + U[2] * dy + F[2] * dz;

    // *** SAY WHAT WENT IN AND WHAT CAME OUT. ***
    //
    // Three headset runs have been spent on which axis lands where, and each
    // report has had to describe a direction in words. One line a second with
    // the numbers settles the next one from the log instead: lean right and
    // watch which of raw x/y/z moves, then which of the applied right/up/fwd
    // does. If raw x moves and applied fwd moves, the quarter turn is the fix
    // and the log says by how much.
    static ULONGLONG lastLog = 0;
    const ULONGLONG now = GetTickCount64();
    if (Cfg().six_dof_log && now - lastLog >= 1000) {
        lastLog = now;
        float raw[3] = {0, 0, 0};
        HeadPositionMetres(raw);
        float a = 0.0f, a0 = 0.0f;
        HeadFrameAngles(&a, &a0);
        // The basis's own horizontal facing, in the GAME's world. Its absolute
        // value means nothing next to a room angle, but how much it CHANGES when
        // the player turns is the whole question: turn the head 90 degrees and
        // if this moves 90, the basis follows the head; if it does not move, it
        // follows the stick. One line of log answers what two builds guessed at.
        COTW_LOG("[6dof] room x%+.3f y%+.3f z%+.3f  ->  right%+.3f up%+.3f fwd%+.3f | "
                 "head %+.1f  ref %+.1f  cancelled %+.1f (mode %d) | basis fwd "
                 "%+.3f,%+.3f = %+.1f deg | dial %d",
                 raw[0], raw[1], raw[2], dx, dy, dz,
                 a * 57.2957795f, a0 * 57.2957795f, hyLogged * 57.2957795f,
                 Cfg().six_dof_frame_mode, F[0], F[2],
                 atan2f(F[0], -F[2]) * 57.2957795f,
                 Cfg().six_dof_yaw_offset_deg);
    }
}

void ApplyEyeOffset(float* m) {
    InterlockedIncrement(&g_leverHits);
    // Position BEFORE the eye separation, so the separation is still measured
    // about the head where the head now is - which is what happens in a real
    // pair of eyes, and keeps the two effects independent.
    ApplySixDof(m);
    ApplyEyeOffsetScaled(m, 1.0f);
    if (Cfg().stereo && g_haveBasis) InterlockedIncrement(&g_leverApplied);
}

// The same shift, scaled - and the scale may be NEGATIVE.
//
// That is what the viewmodel needs. The held weapon is attached to the camera,
// so when stereo shifts the camera by +/-ipd/2 the weapon shifts WITH it. It
// therefore lands on identical pixels in both eyes, which is zero disparity,
// which the brain reads as infinitely far away - hence "the weapon has no 3D
// effect" while the world around it does.
//
// Cancelling that inherited shift (scale -1) leaves the weapon standing still in
// world space while the camera moves around it, which is exactly the disparity a
// real object at arm's length produces.
void ApplyEyeOffsetScaled(float* m, float scale) {
    if (!Cfg().stereo || !g_haveBasis || !m) return;

    // Which basis row is "right" was NOT settled by the axis test - it told us
    // row1 is up, which leaves rows 0 and 2 as the two horizontal axes without
    // saying which is right and which is forward.  Rather than guess (a wrong
    // axis shows up as double vision and gets blamed on everything else first),
    // it is a config value with a sign flip, so one headset session settles it.
    int row = Cfg().eye_axis_row;
    if (row < 0 || row > 2) row = 0;
    const float* r = &g_viewBasis[row * 4];

    // *** ONE OPTICAL AXIS WHILE SCOPED. ***
    //
    // A real scope has an exit pupil: only one eye can be behind it, which is
    // why shooters close the other. In VR both eyes see the scope picture from
    // their own position, 64 mm apart, so each projects the reticle onto a
    // different point downrange and neither necessarily matches the bullet.
    //
    // The reticle is already at ZERO disparity - weapon_3d_amount_scoped is 0,
    // so both eyes get it in the same pixel. The WORLD is not, and while aiming
    // the engine narrows its projection while the mod pins the submitted field,
    // so everything is presented at about 2.75x. World disparity is multiplied
    // by the same factor while the reticle's stays at zero, which puts the
    // reticle-to-target disagreement at 30 arcmin at 20 m and 6 at 100 m -
    // against a fusion limit near 6-10. At every range this game is played at,
    // the crosshair and the animal cannot both be single.
    //
    // Collapsing the separation while scoped puts both eyes on one axis, which
    // is what closing an eye achieves and what the owner asked for. Verified in
    // the headset first, by hand: ipd_mm = 0 made the two eyes identical, and
    // the per-eye crop was ruled out as a contributor in the same session.
    //
    // Default 0 = off, so nothing changes until it is asked for.
    float ipdMm = Cfg().ipd_mm;
    // *** IRON SIGHTS COUNT TOO. ***
    //
    // PlayerIsScoped() is latched from the optic's own lens-mask stamp, so a
    // weapon with no optic never sets it - and iron sights have exactly the same
    // problem: two posts to line up, seen from two positions 64 mm apart, so
    // each eye reads a different alignment.
    //
    // The signal that does not need an optic is the projection itself. Aiming
    // narrows the whole scene's FOV - measured in this project, 1.08 -> 2.98 in
    // reciprocal tangent, roughly 2.75x. So: if the camera is rendering
    // appreciably narrower than the configured field, the player is aiming,
    // whatever they are aiming with.
    //
    // The threshold is deliberately loose. Hip fire sits at the base value and
    // ADS at about a third of it, so anything below 0.8 is unambiguous and no
    // amount of drift in the reading can put the two states next to each other.
    bool aiming = PlayerIsScoped();
    if (!aiming && Cfg().scope_mono_ads) {
        float tanH = 0.0f, tanV = 0.0f;
        if (MeasuredCameraTangents(&tanH, &tanV)) {
            const float kDeg = 0.01745329252f;
            const float base = tanf(Cfg().game_fov_deg * 0.5f * kDeg);
            if (base > 0.01f && tanV < base * 0.8f) aiming = true;
        }
    }
    if (Cfg().scope_mono && aiming) ipdMm = Cfg().scope_ipd_mm;
    const float half = ipdMm * 0.001f * 0.5f;   // mm -> m, half per eye
    float sign = (CurrentRenderEye() == 0) ? -1.0f : 1.0f;
    if (Cfg().eye_swap) sign = -sign;

    m[12] += r[0] * half * sign * scale;
    m[13] += r[1] * half * sign * scale;
    m[14] += r[2] * half * sign * scale;

    // *** THIS EYE'S CAMERA POSITION, FOR THE TEMPORAL RESOLVE. ***
    //
    // The renderer's matrices are CAMERA-RELATIVE, so reprojecting a pixel into
    // the previous frame has to account for how far the camera moved - otherwise
    // turning your head looks perfect and walking trails on everything solid.
    // This is the right place to read it: the position AFTER the eye offset is
    // exactly the origin those matrices are relative to, it is per eye, and it
    // is written every render pass.
    //
    // Looking for it in the constant buffers instead cost a build: GlobalConstants
    // +0x40 is what the engine's own motion-vector shader calls Globals[4].xyz,
    // and it read a constant 0.0000 between frames while walking.
    const int stashEye = (CurrentRenderEye() == 0) ? 0 : 1;
    g_eyeWorldPos[stashEye][0] = m[12];
    g_eyeWorldPos[stashEye][1] = m[13];
    g_eyeWorldPos[stashEye][2] = m[14];
    g_haveEyeWorldPos[stashEye] = true;
}

bool EyeWorldPosition(int eye, float* xyz) {
    const int e = (eye == 0) ? 0 : 1;
    if (!g_haveEyeWorldPos[e]) return false;
    xyz[0] = g_eyeWorldPos[e][0];
    xyz[1] = g_eyeWorldPos[e][1];
    xyz[2] = g_eyeWorldPos[e][2];
    return true;
}

void StereoTick(int everyFrames) {
    static uint64_t frame = 0;
    ++frame;

    // Live controls. Double vision has several possible causes and they are
    // indistinguishable from a description - but each has a different fix, so
    // the fastest route is to let the person in the headset try them.
    {
        // CTRL+ALT is required: plain F4 is the game's own menu key, which made
        // the last test session fight the game's UI. Diagnostic hotkeys must be
        // ones nobody hits by accident.
        auto pressed = [](int vk) {
            static bool was[256] = {};
            const bool mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                              (GetAsyncKeyState(VK_MENU) & 0x8000);
            const bool down = mods && (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool edge = down && !was[vk & 0xFF];
            was[vk & 0xFF] = down;
            return edge;
        };

        if (pressed(VK_F5)) {
            Cfg().stereo = !Cfg().stereo;
            COTW_LOG("[stereo] F5: stereo %s", Cfg().stereo ? "ON" : "OFF (mono)");
        }
        if (pressed(VK_F6)) {
            int r = Cfg().eye_axis_row + 1;
            if (r > 2) r = 0;
            Cfg().eye_axis_row = r;
            COTW_LOG("[stereo] F6: eye axis -> row%d %s", r,
                     r == 1 ? "(this is UP - expect it to look wrong, it is a control)" : "");
        }
        if (pressed(VK_F7)) {
            Cfg().eye_swap = !Cfg().eye_swap;
            COTW_LOG("[stereo] F7: eyes %s", Cfg().eye_swap ? "SWAPPED" : "normal");
        }
        if (pressed(VK_F4)) {
            // 0 mm is the control: it must look exactly like mono. If it does
            // not, the doubling is not the eye separation at all and the whole
            // alternate-eye timing is the suspect instead.
            const float steps[] = {0.0f, 16.0f, 32.0f, 64.0f, 100.0f};
            int i = 0;
            for (int k = 0; k < 5; ++k) if (Cfg().ipd_mm >= steps[k] - 0.5f) i = k;
            Cfg().ipd_mm = steps[(i + 1) % 5];
            COTW_LOG("[stereo] F4: ipd -> %.0f mm%s", Cfg().ipd_mm,
                     Cfg().ipd_mm == 0.0f ? "  (CONTROL: must look identical to mono)" : "");
        }
    }

    if (everyFrames <= 0) return;
    if ((frame % (uint64_t)everyFrames) != 0) return;

    const LONG b = InterlockedExchange(&g_basisHits, 0);
    const LONG l = InterlockedExchange(&g_leverHits, 0);
    const LONG a = InterlockedExchange(&g_leverApplied, 0);

    // The cadence matters: the lever was measured at ~0.65 calls per frame, not
    // one.  If it really does skip frames, the eyes will not alternate cleanly
    // and that shows up here as applied/frames well below 1.0 - a number worth
    // seeing before blaming the stereo maths.
    COTW_LOG("[stereo] over %d frames: basis %ld (%.2f/frame), lever %ld (%.2f/frame), "
             "offset applied %ld (%.2f/frame), eye now %d, ipd %.1f mm, row%d%s",
             everyFrames, b, (double)b / everyFrames, l, (double)l / everyFrames,
             a, (double)a / everyFrames, (int)g_eye, Cfg().ipd_mm,
             Cfg().eye_axis_row, Cfg().eye_swap ? " (swapped)" : "");

    // Print the actual separation axis. Whether it TURNS with the player is the
    // whole question - a world-fixed axis is only correct facing one way - and
    // this makes it visible instead of inferred.
    // Log ALL THREE basis rows and how much each moved. Picking one row and
    // reporting only that is how the last two conclusions went wrong: row0 was
    // observed changing and row2 was assumed to change with it.
    if (g_haveBasis) {
        static float prev[3][3] = {};
        static bool havePrev = false;
        COTW_LOG("[stereo]   basis %s:",
                 Cfg().stereo_basis_from_lever ? "from the turning site (lever)"
                                               : "from the fixed site");
        for (int row = 0; row < 3; ++row) {
            const float* r = &g_viewBasis[row * 4];
            float moved = 0.0f;
            if (havePrev) {
                const float d = r[0] * prev[row][0] + r[1] * prev[row][1] +
                                r[2] * prev[row][2];
                moved = acosf(d < -1 ? -1 : (d > 1 ? 1 : d)) * 57.2957795f;
            }
            COTW_LOG("[stereo]     row%d = (%6.3f, %6.3f, %6.3f)   moved %5.1f deg%s",
                     row, r[0], r[1], r[2], moved,
                     row == Cfg().eye_axis_row ? "   <-- eyes separate along this" : "");
            prev[row][0] = r[0]; prev[row][1] = r[1]; prev[row][2] = r[2];
        }
        havePrev = true;
    }

    if (StereoActive() && (double)a / everyFrames < 0.8) {
        COTW_LOG("[stereo]   NOTE: the lever fires less than once per frame, so some "
                 "frames carry no eye offset. Expect uneven depth until the offset "
                 "moves to a per-frame site.");
    }
}

}  // namespace cotwvr
