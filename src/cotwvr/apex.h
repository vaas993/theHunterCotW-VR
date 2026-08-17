#pragma once
#include <cstdint>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// ---------------------------------------------------------------------------
// Every engine address lives in THIS file and nowhere else.
//
// theHunter: Call of the Wild runs on Avalanche's Apex Engine, statically
// linked into the executable (there is no engine DLL), so all RVAs are
// relative to theHunterCotW_F.exe's image base.
//
// BUILD FINGERPRINT - an RVA is worthless without it, and the game patches
// silently.  Verified at runtime by apex::Init(); a mismatch is logged loudly
// and every RVA-dependent feature stays switched off.
//
//   theHunterCotW_F.exe   41,850,880 bytes
//   ImageBase             0x140000000
//   SizeOfImage           0x02A13000
//   PE timestamp          0x6A5A5133  (2026-07-17 15:58:43 UTC)
//   Steam buildid         24282372   (versions.518791.txt: code_final 3304878)
//
// Each RVA below must carry: how it was found, its calling convention, and
// what it does.  No exceptions - they get re-checked constantly.
// ---------------------------------------------------------------------------

namespace cotwvr {
namespace apex {

// Fingerprint of the build these RVAs were taken from.
constexpr uint32_t kExpectedSizeOfImage = 0x02A13000;
constexpr uint32_t kExpectedTimestamp   = 0x6A5A5133u;  // 2026-07-17 15:58:43 UTC

bool Init();                 // resolve module base, check fingerprint
uintptr_t Base();            // image base of theHunterCotW_F.exe, 0 until Init
bool FingerprintMatches();   // false = game updated; RVA features must stay off
size_t ImageSize();

// Resolve an RVA against the running module.  Returns nullptr when the
// fingerprint does not match, so a stale address can never be called.
void* Rva(uint32_t rva);

// --- engine addresses ------------------------------------------------------

// Transform builders, two 464-byte sibling overloads:
//
//   void* __fastcall Build(Matrix4* out /*rcx*/, const float3* pos /*rdx*/,
//                          const float3* dir /*r8*/)
//
// Writes a ROW-MAJOR 4x4 into `out` and returns it:
//   rows 0..2 = orthonormal basis (w = 0), built with SSE mulss/subss, i.e. a
//               cross product - `right = cross(fwd, up)`
//   row 3     = position, copied verbatim from `pos`, w = 1.0f
//
// Found by: the "Camera_Detach" AOB in a community cheat table
// (theHunterCotW_v.1.22) matched the tail of both;
//   89 41 30 8B 42 04 89 41 34 8B 42 08 89 41 38 48 8B C1 C7 41 3C 00 00 80 3F
// Exact bounds then read from the PE .pdata runtime-function table, NOT from a
// prologue scan (prologue finders happily return a function that does not
// contain the target).
//
// These are GENERIC math helpers - A has 7 direct call sites, B has 36 - so the
// player camera is one caller among many and must be identified at runtime.
// That is what camera_probe.cpp does.
constexpr uint32_t kBuildTransformA = 0x000D8570;   // .. 0x000D8740
constexpr uint32_t kBuildTransformB = 0x000D8740;   // .. 0x000D8910

// THE PLAYER CAMERA, identified by MEASUREMENT (camera_probe, 2026-08-01,
// 35,400 frames of walking and looking).  This is the RETURN address of the
// call to kBuildTransformB, i.e. the call instruction sits at 0x006DBDC3 and
// lives inside function 0x006DBB3A.
//
// Why this site and not one of the other five that survived filtering:
//   0x006DBDC8  0.98 calls/frame  moved 245.7  pos (-5310.04, 1035.95, 6027.54)
//   0x004B6F11  0.35 calls/frame  moved 117.5  pos (-5310.08, 1034.23, 6027.64)
//   0x006445CC  0.35 calls/frame  moved 117.5  pos (-5310.08, 1034.23, 6027.64)
// The last two are the player's BODY transform (same X/Z, ground level, called
// on a slower cadence).  The camera sits exactly **1.72 above** them on Y -
// eye height, in metres.  The remaining sites never move at all (rotation-only
// transforms) or swing wildly (0x00736624, 1.93/frame, swing 69908 = shadow
// cascades).
//
// Consequences worth writing down, both derived from that 1.72:
//   * the world is Y-UP (X/Z horizontal) - the SAME handedness convention as
//     OpenXR, which makes the basis change far cheaper than Far Cry 2's Z-up
//     Dunia.
//   * world units are METRES, so IPD and 6DOF need no scale factor.
constexpr uint32_t kCameraBuildCaller = 0x006DBDC8;   // return address
constexpr uint32_t kCameraBuildCallerFn = 0x006DBB3A; // the function it lives in

// THE SITE THAT ACTUALLY MOVES THE PICTURE, found by displacing each call site
// in turn at runtime and having a human look (2026-08-01).  Displacing all six
// sites moved the view; then, one at a time:
//   0x006445CC  -> the view clearly moved          <-- this one
//   0x006DBDC8  -> the view moved slightly
//   the other four -> no visible effect
//
// So kCameraBuildCaller above is a WEAK lever (it tracks the view and feeds
// something, but it is not what draws the frame) and this is the strong one.
// The call instruction is at 0x006445C7, inside function 0x00642E80.
//
// Note it builds at BODY height, not eye height - same X/Z as the body
// transform at 0x004B6F11, which does nothing when displaced.  Two sites at
// the same position where only one steers the picture is exactly why this had
// to be measured rather than reasoned about.
constexpr uint32_t kCameraMoverCaller = 0x006445CC;
constexpr uint32_t kCameraMoverCallerFn = 0x00642E80;

// *** WHERE THE AIM IS BUILT, AND WHY THE WEAPON DID NOT FOLLOW THE HEAD. ***
//
// Found from a CHEAT TABLE, not by probing: the "Disable Recoil" script's AOB
// lands at 0x00643B26 - INSIDE kCameraMoverCallerFn. Recoil and the camera
// lever are the same function, because that function IS the player aim update.
// (Recoil was the right thing to chase: it moves the view and the weapon
// together, so whatever it writes is the coupled aim.) Reading the function end
// to end gives the whole chain:
//
//   0x006441CC  pitch and yaw loaded from locals [rbp-0x64] / [rbp-0x60]
//   0x00644200  maxss/minss vs [r12+0x128]/[r12+0x12C] - the pitch clamp
//   0x0064422A  call 0x004A03A0 with rcx = &camera[0x4C], xmm1 = yaw,
//               xmm2 = pitch, xmm3 = roll. That helper does
//               "addss xmm1,[rcx]" and wraps to +/-PI, so camera+0x4C is the
//               ACCUMULATED VIEW YAW - the very field head tracking writes.
//   0x0064424E  call kAimConsumer(rsi, r15, camera, byte, float)  <-- the
//               consumer; what follows the aim reads it from here.
//   0x006445C7  the camera transform builder (kCameraMoverCaller)
//
// So ORDERING was the whole problem, and there is no second aim field to find.
// Head tracking writes camera+0x4C at the transform builder, which runs AFTER
// kAimConsumer: the view is built later still and follows, while the weapon was
// already placed and does not. Writing the same offset at kAimConsumer instead
// puts it ahead of both.
//
// Argument count read off the CALL SITE, never the prologue - rcx=rsi,
// rdx=r15, r8=camera, r9d=byte, [rsp+0x20]=float. FIVE arguments. Getting this
// wrong has crashed this game four times.
constexpr uint32_t kAimConsumer = 0x0063C0A0;   // .. 0x0063C0D9
constexpr uint32_t kAimYawOffset = 0x4C;        // accumulated yaw, radians
constexpr uint32_t kAimPitchOffset = 0x50;      // pitch, radians

// The player's own look accumulators, one field pair earlier on the SAME object
// the aim consumer receives in rcx. Disassembled at 0x00643F22/0x00643F27:
//     addss xmm0, xmm8          ; this frame's look delta
//     movss [rsi+0x1c], xmm0    ; accumulate, then wrap to +/-pi
// These are what the crouch ratchet moves, measured with Cheat Engine, and what
// the re-seed at kAimReseed rewrites once per stance change.
constexpr uint32_t kAimInputYaw = 0x1C;
constexpr uint32_t kAimInputPitch = 0x20;

// --- TIME OF DAY AND WEATHER (from the community cheat tables) -------------
//
// The hour getter reads a GLOBAL, so the time manager needs no hook:
//   mov rax,[rip+0x20694B9] / test rax,rax / movss xmm0,[rax+0xE8] / ret
// found by the table's AOB `F3 0F 10 80 E8 00 00 00 C3` (unique on our build at
// 0x0075F68C). Global = 0x0075F687 + 0x20694B9.
// *** THE ONLY WAY THE HEAD OFFSET CAN ENTER PERSISTENT STATE. ***
//
// Found by adversarial re-analysis after ~10 failed fixes. camera+0x4C is read
// at exactly THREE sites in the aim update:
//   0x00644226  -> 0x004A03A0   accumulate (field += delta, yaw wrap +/-PI,
//                               pitch HARD CLAMP +/-PI/2 at 0x004A03F9/0404)
//   0x00644257  -> 0x004A6770   euler -> matrix (the view; this is the feature)
//   0x00644278  -> 0x004993E0   Copy6Floats(rsi+0x5C, camera+0x4C)
//
// That third one is the bake. 0x004993E0 has EXACTLY ONE caller in the whole
// image, and it is guarded:
//   0x00644260  cmp byte [rsi+0xBC], 0     ; latch armed?
//   0x00644278  copy camera+0x4C -> rsi+0x5C
//   0x006442AA  mov byte [rsi+0xBC], 1     ; latched
//   0x006445DC  mov byte [rsi+0xBC], 0     ; re-armed, when the engine's own
//                                          ; "camera is script-driven" bool is 0
//
// So the engine ANNOUNCES the bake one byte in advance, and the announcement is
// readable from the aim-consumer hook - rcx there IS rsi (0x0064424B mov
// rcx,rsi), and the consumer at 0x0064424E runs BEFORE the latch check at
// 0x00644260. Reading it beats every retrospective estimator tried so far: no
// thresholds, no detection, no fighting the player - just do not have an offset
// in the field on the tick the engine is about to copy it.
// *** ROTATE THE MATRIX, NEVER THE ANGLE. ***
//
// Measured across many runs: writing camera+0x4C causes the ratchet no matter
// WHEN it is written - early, late, or suspended during stance changes. The
// whole family of "write the angle, but more cleverly" is dead.
//
// So do not write an angle at all. 0x004A6770 is
//   void __fastcall BuildMatrixFromEulerAngles(const float* angles /*rcx*/,
//                                              float* out4x4 /*rdx*/)
// - TWO arguments, counted at the call site (0x00644253 lea rdx,[rbp-0x30];
// 0x00644257 lea rcx,[r13+0x4C]; 0x0064425B call), and confirmed by the
// prologue reading [rcx] and [rcx+4] with rdi=rdx. It is the point where the
// view's ORIENTATION is actually constructed. Rotating its OUTPUT gives the
// head rotation to everything downstream while leaving the engine's angle state
// untouched - and a value we never write cannot be read back.
//
// It has 13 callers, so the hook must filter by return address. Two matter:
//   0x00644260  built from camera+0x4C   (the live aim)
//   0x006442CC  built from rsi+0x5C      (the copied aim)
// *** THE ONE INSTRUCTION THAT MAKES THE OFFSET PERMANENT. ***
//
// `void* __fastcall Copy6Floats(void* dst /*rcx*/, const void* src /*rdx*/)` -
// 24 bytes, returns rcx. It has EXACTLY ONE caller in the whole image:
//   0x00644270 lea rcx,[rsi+0x5C]     ; dst - the saved aim
//   0x00644274 lea rdx,[r13+0x4C]     ; src - the LIVE aim, containing our offset
//   0x00644278 call 0x004993E0
// and that call only runs when the engine's scripted-camera flag [rsp+0x51] is
// set. Crouch, stand, prone, jump and stance adjustment on slopes are all
// scripted camera episodes: the engine SAVES the aim when one begins and
// restores it when it ends. It was saving a value that contained our head
// offset, and handing it back as the player's real aim. That is the ratchet.
//
// It also explains why suspending the offset during the event never worked - the
// save happens at the START, before any detector could notice.
//
// One caller means no return-address filter is needed: neutralise our offset for
// the duration of this call and the engine saves its own value, unpolluted.
constexpr uint32_t kSaveAimCopy = 0x004993E0;

constexpr uint32_t kEulerToMatrix = 0x004A6770;
constexpr uint32_t kEulerToMatrixRetLive = 0x00644260;
constexpr uint32_t kEulerToMatrixRetCopy = 0x006442CC;

// *** THE CALL SITE THAT BUILDS THE VIEW - the one never hooked. ***
//
// The two above feed BuildTransformB and become the weapon and body transform.
// That is why head_matrix_rotate never drifted but never turned the view either:
// right idea, wrong caller.
//
// The VIEW's orientation is built inside kViewCommit, out of the aim angles:
//   0x0049E8DF  mov  rdi, rcx
//   0x0049E8E5  add  rcx, 0x4c        ; the aim euler
//   0x0049E8EC  call 0x004A6770       ; EulerToMatrix -> the view's basis
//   0x0049E8F1                        ; <- the return address, i.e. this site
//
// Rotating THAT output turns the view without writing any angle the engine can
// read back later - and reading it back is the whole bug. The owner's
// observation is what pinned it down: every stance change produces TWO
// rotations, one as the key goes down and one after, and AIMING does it too.
// That is not five separate triggers, it is one - a camera transition samples
// the aim at its START and again at its END, and aiming is a camera transition
// like any other. So no amount of writing into the aim can win; the offset has
// to live somewhere the engine never samples.
constexpr uint32_t kEulerToMatrixRetView = 0x0049E8F1;

// *** WHERE A ROTATED MATRIX GETS BAKED BACK INTO AN ANGLE. ***
//
//   0x004B7203  lea  rdx, [rbp+0x70]   ; a matrix
//   0x004B7207  lea  rcx, [rbx+0x4c]   ; ...decomposed STRAIGHT into the aim
//   0x004B720B  call 0x004B2560        ; MatrixToEuler(out = rcx, m = rdx)
//   0x004B7210                         ; <- the return address
//
// Measured earlier as writing back bit-identical, and correctly dismissed then:
// nothing of ours was in that matrix. Turning the view by rotating a matrix puts
// something of ours in it, so the same instruction that was innocent becomes the
// one that stores our offset as the player's aim. Signature reads off the call
// site: rcx is the destination euler, rdx the source matrix.
constexpr uint32_t kMatrixToEuler = 0x004B2560;
constexpr uint32_t kMatrixToEulerRetBake = 0x004B7210;

// *** THE BAKE, FOUND AT LAST - AND BY THE GAME ITSELF, NOT BY REASONING. ***
//
// Cheat Engine's "find out what writes to this address", pointed at the yaw
// accumulator, listed exactly two writers after three crouches:
//
//   20396  0x00643F27  movss [rsi+1C],xmm0   ; the normal per-frame look update
//       3  0x006388CC  movss [rbx+1C],xmm8   ; <- ONCE PER CROUCH
//
// Count 3 for three crouches. That is the whole two-day search, answered in one
// screenshot. Everything before this - the servo, the latch gate, the re-anchor
// repair, the clean-saved-aim hook, the bake undo - was aimed at writers that
// were never writing.
//
// The instruction sits inside a routine that rewrites the ENTIRE aim at once:
//   0x0063888E  mov   [rbx+0x4c], eax      ; the aim yaw
//   0x006388AE  call  0x005B3390           ; a hashed script lookup (0x58252FCA)
//   0x006388CC  movss [rbx+0x1c], xmm8     ; the yaw accumulator
//   0x006388D2  movss [rbx+0x20], xmm0     ; the pitch accumulator
// i.e. a scripted-camera RE-SEED: it takes the view as it currently stands and
// makes that the player's aim. With head tracking on, "as it currently stands"
// includes our rotation - so the head offset becomes permanent aim. It explains
// the two rotations per stance change (once entering, once leaving) and why
// aiming does it too: they are all camera transitions.
//
// rcx is the aim object: `mov rbx, rcx` at 0x0063874D. At least three arguments
// (rcx, rdx, r8 - r8 goes to rbp at 0x00638757); the prologue writes only the
// shadow space, so nothing is passed on the stack.
constexpr uint32_t kAimReseed = 0x00638730;        // .. 0x00638972

// The single instruction the whole ratchet comes down to, and the one the code
// cave detours. Six bytes, which is one more than a rel32 jump needs:
//   0x006388CC  f3 44 0f 11 43 1c   movss [rbx+0x1c], xmm8
constexpr uint32_t kReseedAccumWrite = 0x006388CC;

// *** THE SECOND RE-SEED - HOLD BREATH. ***
//
// A separate mechanism from kAimReseed, living inside the aim update
// (0x00642E80), and NOT reachable from it: the cave at kReseedAccumWrite could
// never have caught this wherever it was placed. Established by disassembly, not
// inference - kAimReseed has no direct callers at all, it is slot 10 of a
// state-descriptor table at .rdata 0x01A5C120, and of that whole slot-10 family
// it is the only member that writes the accumulators.
//
// It is a DEFERRED LATCH. Pressing hold breath arms a byte on the rising edge:
//     0x00643E9E  mov byte [rsi+0x108], 1     gated on the weapon's breath
//                                             capacity at item+0x1FC > 0
// and the NEXT pass through the aim update re-seeds from the camera, then
// disarms:
//     0x00643DC5  cmp byte [rsi+0x108], 0
//     0x00643DD5  mov rbx, [rax+0x5C0]        the camera object
//     0x00643DDC  lea rcx, [rbx+0xD4]         its row-major 4x4
//     0x00643DE3  call 0x004A6E40             -> yaw
//     0x00643DE8  movss [rsi+0x1c], xmm0      <-- kHoldBreathReseedYaw
//     0x00643DF4  call 0x004A57D0             -> pitch
//     0x00643DF9  movss [rsi+0x20], xmm0      <-- kHoldBreathReseedPitch
//     0x00643DFE  mov byte [rsi+0x108], 0
//
// rsi is the same aim object rbx is in kAimReseed - both touch the
// +0x108..+0x10C flag cluster, this path owning +0x108 and +0x109.
//
// Each store is EXACTLY 5 bytes, which is exactly what a jmp rel32 needs, so
// these detours steal nothing and need no padding - easier than
// kReseedAccumWrite, which had to swallow 11. A full-image scan found no branch
// landing anywhere inside the block.
constexpr uint32_t kHoldBreathReseedYaw   = 0x00643DE8;
constexpr uint32_t kHoldBreathReseedPitch = 0x00643DF9;

constexpr uint32_t kAimLatchByte = 0xBC;   // on the aim object: 0 = about to copy
constexpr uint32_t kAimLatchDest = 0x5C;   // where camera+0x4C is copied to

constexpr uint32_t kTimeManagerPtr = 0x027C8B40;   // -> time manager, or null
constexpr uint32_t kTimeHourOffset = 0xE8;         // float, 0..24
// SUSPECTED, inherited from a table for a different build and NOT yet confirmed
// on ours - the panel logs the surrounding floats once so it can be checked
// before anything writes here.
constexpr uint32_t kTimeMultOffset = 0xF4;         // float, time speed

// Weather has no global - a bare leaf accessor with `this` in rcx, so the
// pointer has to be caught as it goes past. AOB `F3 0F 10 81 D0 01 00 00 C3`.
constexpr uint32_t kWeatherSpeedGetter = 0x002FB6B0;  // movss xmm0,[rcx+0x1D0]
constexpr uint32_t kWeatherSpeedOffset = 0x1D0;       // float

// AXIS TEST RESULT (2026-08-01, displacing kCameraMoverCaller 5 m and asking a
// human which way the picture went):
//   row1 -> "definitely straight up"      => row1 IS world up. Agrees with the
//                                            measured basis, row1 ~ (0, 1, 0).
//   row0 -> "forward-LEFT", i.e. DIAGONAL  => and that is the useful finding.
//
// A diagonal is only possible because the mover's basis is NOT the view's
// basis.  Measured in the same frame:
//   mover  (0x006445CC) row0 = ( 0.792, 0.000, -0.610)
//   camera (0x006DBDC8) row0 = (-1.000, -0.005, 0.000)
// The mover is body-aligned and the body can face a different way from the
// view, so offsetting along the MOVER's own rows produces a direction that is
// not camera-relative - useless for stereo, which needs the eye separation
// along the VIEW's right axis.
//
// Consequence for the stereo implementation: take the world-space basis from
// the CAMERA site (which tracks the view perfectly) and apply the offset at the
// MOVER site (which is what actually steers the picture).  Two different
// objects, one for reading orientation and one for writing position.
constexpr uint32_t kCameraBasisSource = kCameraBuildCaller;   // read orientation here
constexpr uint32_t kCameraPositionLever = kCameraMoverCaller; // write position here

// THE PER-FRAME RENDER FUNCTION - the thing full-rate stereo calls twice.
//
// Found 2026-08-01 by capturing the stack at Present and at the camera lever
// and intersecting them (framescan.cpp).  Both stacks pass through this one
// function at two different call sites, 42 bytes apart:
//
//   +0x9AC  (0x007BA6DC)  -> ... -> the camera position lever
//   +0x9D6  (0x007BA706)  -> ... -> IDXGISwapChain::Present
//
// So it "sets up the player view camera AND reaches Present within one call",
// which is the definition of the frame function.  Bounds from .pdata.
//
// CRITICAL, MEASURED: Present and the camera lever run on the SAME THREAD
// (23560 in that run).  The scene is therefore drawn inline on the presenting
// thread, which is what makes calling this twice viable at all.  Had they been
// different threads, full-rate stereo would need a completely different attack.
//
// RISK not yet cleared: 2,716 bytes is large enough to contain more than
// rendering.  If it also steps simulation/physics/time, calling it twice
// double-steps the game.  Must be checked before shipping - the symptom would
// be the world running at double speed.
// THE COMPLETE TRANSFORM-BUILDER SITE MAP, from a full sweep 2026-08-02.
// Only SIX sites ever fire, and each was identified by displacing it live and
// looking - worth keeping, because the next lever this mod needs is probably
// one of them:
//
//   0x006445CC  ~1/frame   THE POSITION LEVER - what stereo and the eye offset
//                          use. Moves the rendered picture.
//   0x006DBDC8  ~1/frame   basis source. Its matrix does NOT yaw (measured:
//                          forward.y swing 0.024) - near-constant, do not use.
//   0x004B6F11  ~1/frame   ALSO MOVES THE WHOLE VIEW, world and weapon together.
//                          A second, independent world-camera lever. Untried as
//                          an alternative to 0x006445CC - if the lever ever
//                          proves awkward, start here.
//   0x006FD788  ~30/s      below frame rate, so not a per-frame camera.
//   0x00736624  ~510/s     shadow cascades. Never write to it.
//   0x00C26398  ~540/s     many per frame - shadows/reflections class.
//
// NONE of them moves the held weapon on its own. Swept 25 cm each, in turn,
// with a weapon equipped: the viewmodel never moved independently. So the
// first-person pass does NOT build its transform through these helpers, and
// giving the weapon stereo depth needs the render view/projection itself
// (the D3D11 constant buffer), not a transform-builder call site.
constexpr uint32_t kRenderFrame = 0x007B9D30;      // .. 0x007BA7CC
constexpr uint32_t kRenderFrameEnd = 0x007BA7CC;

// DRAW THE SCENE - void __fastcall(void* this), 515 bytes.
//
// This is the inner call kRenderFrame makes at +0x9A7:
//     7BA6D4  mov  rcx, rdi
//     7BA6D7  call 0x1407C0DE0      <- this, returns to 0x7BA6DC
//     ...
//     7BA701  call 0x140797420      <- presents, returns to 0x7BA706
//     7BA706  inc  dword [rdi+0xAC] <- frame counter
//
// The camera position lever sits inside it (the camera stack passes through
// 0x007C0E2B, which .pdata puts in this function), so it both sets up the view
// and draws - without presenting, and without whatever else the frame driver
// does.
//
// THIS, NOT kRenderFrame, IS WHAT FULL-RATE STEREO CALLS TWICE.  Hooking
// kRenderFrame and calling it twice crashed the game instantly (2026-08-01):
// re-entering the whole frame driver re-enters the present path and everything
// else it owns.  Call the narrowest function that still draws the scene.
// THE FIRST-PERSON CAMERA MODIFIER'S Update - void __fastcall(void* self,
// void* pose, float* lensOut, void* cam).  vtable 0x01A5C4D8, slot +0x28.
//
// Signature read off the call site at 0x004B763C..0x004B7648:
//     mov r9, rdi ; lea r8,[rsp+0x20] ; lea rdx,[rbp-0x40] ; call [rax+0x28]
//
//     lensOut[0] = WORLD field of view, radians
//     lensOut[1] = viewmodel/foreground field of view, radians
//
// written at 0x006434C3 / 0x006434FC (deg2rad 0.0174532942 at RVA 0x01938138).
// The ADS/zoom path rewrites lensOut[0] later at 0x00643AC0, so anything that
// wants the final value must run AFTER the original returns.
//
// *** DO NOT DETOUR INSIDE THIS FUNCTION. *** It is 8201 bytes and also owns
// recoil (0x643B26), the pitch clamp (0x644200), the hold-breath reseed
// (0x643DE8/0x643DF9) and the aim consumer (0x64424E). This file already
// records four crashes from getting argument counts wrong in exactly this
// neighbourhood. An entry detour that calls the original and then edits
// lensOut is safe, and is the whole of the change.
constexpr uint32_t kCamModUpdate = 0x00642E80;

// *** THE TAA JITTER GENERATOR - found by the ladder hunt of 2026-08-12 ***
// (jitterhunt.cpp: memory scan -> hwbp write-watch x3 -> disassembly).
//
// bool __fastcall JitterGen(void* obj, float* m16, uint32_t w, uint32_t h):
// reads the AA mode at [obj+0x3D8] (only 2 and 3 jitter - 2 = the shipped
// 2-phase, 3 = a SIXTEEN-phase mode the game's settings never enable),
// indexes its table by the frame counter below, scales, divides by w/h, and
// emits an identity matrix with the NDC offset in row3 ([m+0x30], [m+0x34]).
// The composer at 0xCF3D0 post-multiplies the projection by it. Every
// consumer - per-object baked WVPs, the shared block, every compensating
// pass - derives from this ONE call, which is why detouring it is the
// self-consistent way to own the jitter (DLSS_IMPLEMENTATION.md 7e). No
// float args, bool return: a safe C++ signature, rare in this exe.
constexpr uint32_t kJitterGenerator = 0x0012DFB0;
// The frame counter both stock modes index by (mode 2: &1, mode 3: &15).
constexpr uint32_t kJitterFrameCounter = 0x025AFB30;

constexpr uint32_t kRenderScene = 0x007C0DE0;      // .. 0x007C0FE3
constexpr uint32_t kRenderSceneEnd = 0x007C0FE3;

// SUBMIT THE SCENE - void __fastcall(void* desc), 79 bytes, one argument.
// Called by the frame driver at 0x007BA6F5 with the SAME stack buffer the build
// filled.  Not a destructor: it has a global state machine (compares a global
// against 6 and sets it), reads a float from desc+0x40, and tail-jumps into
// 0x00140D60.  This is where the drawing actually happens.
//
// The pair matters: kRenderScene only FILLS a description; kSubmitScene
// consumes it.  Calling the build twice just overwrites the description, which
// is why "draw the scene twice" was the wrong mental model.
constexpr uint32_t kSubmitScene = 0x0080C780;      // .. 0x0080C7CF

// THE PRESENT PATH - called by the frame driver at 0x007BA701 as:
//     movaps xmm1, xmm9      ; a 128-bit second argument
//     mov    rcx, rdi        ; this
//     call   0x140797420
//     inc    dword [rdi+0xAC]  ; frame counter
//
// This is where post-processing and the resolve into the BACKBUFFER happen -
// which is why capturing the backbuffer straight after kSubmitScene got a stale
// frame and produced no depth. A complete eye image only exists after this runs.
//
// Declared with __m128 rather than float: the caller uses MOVAPS, so all 128
// bits are meaningful and narrowing it to a float would drop three lanes.
constexpr uint32_t kPresentPath = 0x00797420;

// THE FRAME CLOCK. The present path opens by calling 0x00143330 (the clock
// update) and then computing a delta from two global counters:
//
//   79745A  mov      eax, [rip+0x1E18838]   -> rva 0x025AFC98
//   797475  cvtsi2ss xmm6, rax
//   79747A  mov      eax, [rip+0x1E18814]   -> rva 0x025AFC94
//   797480  cvtsi2ss xmm0, rax
//   797485  divss    xmm6, xmm0             ; A / B
//   797489  mulss    xmm6, <const>          ; = seconds
//
// This is why calling the present path twice per frame put the game into SLOW
// MOTION: the second call measures ~0 elapsed time, so the world advances on a
// near-zero delta. BioShock VR's `DeltaClamp` ("one world advance per eye PAIR
// instead of per eye") exists for exactly this.
//
// Fix: snapshot both counters before the second-eye replay and restore them
// after, so the engine's clock never sees the extra frame.
constexpr uint32_t kFrameClockA = 0x025AFC98;
constexpr uint32_t kFrameClockB = 0x025AFC94;

// THE ENGINE'S GLOBAL TIME OBJECT - a POINTER, not the object.
//
// The frame driver asks it for the frame delta three times in a row, and all
// three reads resolve to the same global:
//
//   7B9E4F  mov rcx, [rip+0x1EE619A]  -> rva 0x0269FFF0   (dl=0)
//   7B9E6A  mov rcx, [rip+0x1EE617F]  -> rva 0x0269FFF0   (dl=1)
//   7B9E7C  mov rcx, [rip+0x1EE616D]  -> rva 0x0269FFF0   (dl=1)
//
// The accessor at 0x000EB270 is a pure GETTER - it resets nothing:
//
//   EB274  cmp   byte [rcx+0x30], dl   ; paused
//   EB282  cmp   byte [rcx+0x31], 0    ; use the scaled value?
//   EB288  movss xmm0, [rcx+0x2c]      ;   yes -> SCALED delta
//   EB28E  movss xmm0, [rcx+0x14]      ;   no  -> raw delta, in seconds
//
// So the delta is computed elsewhere and PARKED in these fields. Whatever ticks
// them runs somewhere inside the replayed build/submit/present region, which is
// why replaying it splits one real frame's elapsed time across two ticks: the
// engine then believes each frame took half as long as it did. That reads as
// "the game's fps counter doubled AND everything time-based runs at half
// speed" - the fps counter being 1/delta is the same fact stated twice.
constexpr uint32_t kTimeObjectPtr = 0x0269FFF0;
constexpr uint32_t kTimeDeltaOffset       = 0x14;   // float, seconds
constexpr uint32_t kTimeScaledDeltaOffset = 0x2C;   // float, seconds
constexpr uint32_t kTimePausedOffset      = 0x30;   // byte
constexpr uint32_t kTimeUseScaledOffset   = 0x31;   // byte

// A THIRD delta, returned by a THIRD getter at 0x000EB020:
//   EB032  movss xmm0, [rcx+0x20]
// The frame driver reads all three in a row (0x0EB270 twice, 0x0EB020 once), so
// different subsystems consume different ones - which is why correcting only
// +0x14 left the in-game UI running slow while the world looked right.
constexpr uint32_t kTimeDelta3Offset      = 0x20;   // float, seconds

// +0x2C IS NOT A SCALED DELTA. Measured live it holds a constant 0.03333 =
// exactly 1/30, so it is a FIXED TIMESTEP, and the same 1/30 turned up twice
// before: as "description field 0", and as the false pitch candidates ranging
// 0..0.0333. Treating it as "raw times the game's dilation" produced a
// nonsense ratio of 2.835 and wrote garbage over a constant. Do not write it.

// *** THE TICK, and the baseline this whole problem hinged on. ***
//
// 0x000EB040 is the update:
//   EB04E  call [QueryPerformanceCounter]
//   EB054  mov  r9,  [rbx+0x70]     ; the PREVIOUS timestamp
//   EB058  mov  r8,  [rsp+0x30]     ; now
//   EB063  mov  [rbx+0x70], r8      ; now becomes the previous
//   EB06A  sub  rax, r9             ; elapsed = now - previous
//
// So the baseline is a u64 at +0x70 - OUTSIDE the 0x40-byte window that was
// being snapshotted around the second-eye replay, which is exactly why
// restoring the object did not stop the delta halving and why a workaround
// (measuring the frame ourselves and overwriting +0x14) was needed at all.
//
// Restore +0x70 and the engine's own next tick measures the true full frame,
// so EVERY consumer is fixed at once instead of one field at a time.
constexpr uint32_t kTimeBaselineOffset    = 0x70;   // uint64, QPC ticks

// The tick itself, hooked so EVERY object it advances during the second-eye
// replay can be put back - not just the one global above. Restoring only that
// global fixed the world but left the player and the UI running at half speed,
// which means they read a clock this function ticks on some other object.
// One argument, rcx = the object; both call sites read set nothing else:
//     503283  mov rcx, [rip+0x219CD66]
//     50328A  call 0x1400EB040
constexpr uint32_t kTimerTick = 0x000EB040;

// THE DELTA GETTER - the one place every consumer of the frame delta goes
// through, and therefore the right place to correct it.
//
//   EB270  test  dl, dl
//   EB274  cmp   byte [rcx+0x30], dl     ; paused?
//   EB282  cmp   byte [rcx+0x31], 0      ; use the fixed timestep?
//   EB288  movss xmm0, [rcx+0x2c]        ;   yes -> the fixed 1/30
//   EB28E  movss xmm0, [rcx+0x14]        ;   no  -> the frame delta
//
// TWO arguments, confirmed at the call sites (`xor edx,edx` / `mov dl,1`, then
// `mov rcx,[global]`), returning a float in xmm0. It reads a field and nothing
// else, so replacing its RESULT is safe in a way that rewriting the field, the
// baseline or the counter all turned out not to be.
constexpr uint32_t kTimeGetter = 0x000EB270;

// The SECOND getter, returning +0x20, same shape:
//   EB024  cmp   byte [rcx+0x30], dl
//   EB032  movss xmm0, [rcx+0x20]
// Different subsystems read different getters. Correcting only the first fixed
// the player and left the in-game UI slow, which is how this one was found.
constexpr uint32_t kTimeGetter2 = 0x000EB020;

// THE ONLY TWO PLACES THE ENGINE ASKS FOR THE FRAME DELTA - measured by
// capturing the return address inside the getter, not by reading code. Both sit
// in the frame driver, both are called exactly ONCE per frame, and under
// full-rate both receive HALF the real frame (5.86 ms against 10.89 ms):
//
//   7B9E56  call 0x1400EB270   (dl=0)  -> xmm9,  fed to build/submit/present
//   7B9E71  call 0x1400EB270   (dl=1)  -> xmm11
//
// The return addresses are the instruction AFTER each call.
constexpr uint32_t kDeltaReadA = 0x007B9E5B;
constexpr uint32_t kDeltaReadB = 0x007B9E76;

// THE CLOCK UPDATE, and the right place to stop this problem rather than repair
// it. The present path's very first act:
//     79743B  mov  rcx, [rip+0x1ECF0BE]
//     797446  call 0x140143330
// and the frame driver calls it the same way at 7BA712/7BA719 - rcx only, so
// one argument.
//
// SKIPPING IT DURING THE REPLAY WAS TRIED AND IS WRONG: the world flickered
// black, so this call does RENDER work as well as timing and the second eye
// needs it. Hooked, but passed straight through by default.
constexpr uint32_t kClockUpdate = 0x00143330;

// ---------------------------------------------------------------------------
// TOBII "EXTENDED VIEW" - THE GAME SHIPS ITS OWN HEAD TRACKING.
//
// settings.json has TobiiExtendedViewEnabled/Responsiveness/HeadTrackingSpeed/
// MaxYawAngle, and the exe carries the full Tobii stream integration. Extended
// View turns the player's view from an external head pose - which is exactly
// what we spent a day failing to do by writing camera matrices.
//
// kTobiiPoseCallback (833 bytes, ZERO direct callers = the SDK's stream
// callback) has signature `void __fastcall(void* this /*rcx*/,
// tobii_head_pose_t* pose /*rdx*/)`. The struct matches the SDK exactly:
//     +0x00 timestamp   +0x08 position validity   +0x0C..0x14 position xyz
//     +0x18 rotation validity[3]                  +0x24..0x2C rotation xyz
// (identified from `cmp [rdx+8],1`, the position reads at +0xC/0x10/0x14, and
//  the rotation reads at +0x24/0x28/0x2C; two rotation components are negated
//  via `xorps`, an axis-convention conversion).
//
// IT STORES THE POSE INTO ITS OWN OBJECT, exponentially smoothed:
//     10E7DA  addss xmm0, [rbx+0x3C4]      ; rot X
//     10E7E2  addss xmm1, [rbx+0x3C8]      ; rot Y
//     10E7EA  mulss xmm0, <k>              ; stored = (new + stored) * k
//     10E7F2  movss [rbx+0x3C4], xmm0
//     10E808  movss [rbx+0x3C8], xmm1
//     10E814  movss [rbx+0x3CC], xmm0      ; rot Z
//     10E81E  mov   dword [rbx+0x3C0], 1   ; "pose valid" flag
//     10E7B1  mov   [rbx+0x3B8], rax       ; position (packed x,y)
//
// SO: write kTobiiRot* every frame with the headset's orientation and set
// kTobiiPoseValid, and the game's OWN Extended View should turn the view. No
// camera-matrix fighting, no re-entrancy, and it is the engine's supported path.
//
// STILL NEEDED: the `this` pointer. The callback only fires with a real Tobii
// device, so it cannot be used to capture it. Next step: hook the Extended View
// consumer (kTobiiExtendedViewUpdate, which references the "extended_view"
// string at 0x01A30070 and runs per frame) READ-ONLY, log its `this`, and check
// whether +0x3C0..0x3CC looks like the pose block.
// READ ITS CALL SITE FOR THE ARGUMENT COUNT BEFORE HOOKING IT.
constexpr uint32_t kTobiiPoseCallback = 0x0010E620;   // .. 0x0010E961
constexpr uint32_t kTobiiExtendedViewUpdate = 0x004390D0;  // .. 0x0043A5D9
constexpr uint32_t kTobiiExtViewCaller = 0x009D1000;  // .. 0x009D1262
// THE MANAGER OBJECT, straight out of a global - no hook needed to get it.
// Read off the Extended View call site at 0x009D1227:
//     xor  r8d, r8d               ; arg3 = 0
//     mov  rdx, [rip+0x1E405CA]   ; arg2 = the object  -> rva 0x028117F8
//     lea  rcx, [rbp-0x69]        ; arg1 = a local buffer
//     call 0x1404390D0            ; kTobiiExtendedViewUpdate (THREE arguments)
// The global is a POINTER; dereference it to reach the object whose +0x3C0..
// +0x3CC hold the pose.
constexpr uint32_t kTobiiManagerPtr = 0x028117F8;

constexpr uint32_t kTobiiPoseValid = 0x3C0;           // object-relative
constexpr uint32_t kTobiiRotX = 0x3C4;
constexpr uint32_t kTobiiRotY = 0x3C8;
constexpr uint32_t kTobiiRotZ = 0x3CC;

// ---------------------------------------------------------------------------
// FULL-RATE STEREO: BOTH ATTEMPTS CRASHED. READ THIS BEFORE TRYING AGAIN.
//
// Attempt 1 - call kRenderFrame (the frame driver) twice.
//   Instant crash. It presents and owns per-frame state; re-entering it
//   re-enters the present path. Too coarse, and predictable in hindsight.
//
// Attempt 2 - call kRenderScene (draws only, does not present) twice.
//   Also crashed, ~1 ms after full-rate engaged (log: OpenXR ready 0.712,
//   dead 0.713). Minidump:
//       ACCESS_VIOLATION reading 0x0 at exe+0xCCA167
//   which is inside the 128-byte helper 0x00CCA140, whose only caller is
//   0x00CC7846 (in 0x00CC7810). That helper is a **Steamworks accessor**:
//       lea  rcx, [static context]
//       call [steam_api64.dll!SteamInternal_ContextInit]
//       mov  rcx, [rax]        ; the ISteam* interface pointer
//       mov  rax, [rcx]        ; <- NULL here
//       call [rax+0xA8]
//   So something on the scene path calls into Steam once per frame, and
//   re-entering it finds the interface pointer null.
//
// CONCLUSION: drawing the scene twice is not safe as a plain re-entrant call -
// the scene path has per-frame state (at least a Steam interface, likely render
// state too) that is set up once by the frame driver and consumed by the draw.
//
// UNTRIED, in order of promise:
//   1. Find what the frame driver does BEFORE kRenderScene (between 0x7B9D30
//      and +0x9A7) and re-run that setup before the second draw.
//   2. Identify the Steam-touching call at 0x00CC7810 and skip it on the second
//      pass - if it is telemetry/screenshot plumbing, it need not run twice.
//   3. Hook lower: find where the scene draw sets the view matrix and issue the
//      second eye's geometry pass only, rather than the whole scene.
// ---------------------------------------------------------------------------

}  // namespace apex
}  // namespace cotwvr
