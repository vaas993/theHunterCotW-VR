#pragma once

#include <cstdint>

struct ID3D11DeviceContext;
struct ID3D11Device;
struct ID3D11Resource;

namespace cotwvr {

// Find the view matrix inside the game's D3D11 constant buffers.
//
// The held weapon renders in its own pass with its own projection, and none of
// the engine's six transform-builder call sites moves it - proven by sweeping
// every one of them with a weapon equipped. So the only remaining place to give
// it stereo depth is the render view, and the way to find that is to search what
// the game uploads for numbers we already know: the camera position and basis,
// captured every frame at the position lever.
bool InstallCBScan(ID3D11DeviceContext* ctx);

// Shader creation, hooked as early as the probe device exists. Hooking it at
// first Present was too late: the shaders the player marked in Shader Toggler
// had already been created, so none of the three could ever be recognised.
bool InstallShaderCreationHooks(ID3D11Device* dev);

// *** THE EARLIEST HOOK IN THE MOD, AND IT HAS TO STAY THAT WAY. ***
//
// Hooks D3D11CreateDevice / D3D11CreateDeviceAndSwapChain in d3d11.dll so every
// device the game creates is seen the moment it exists. Called immediately after
// MH_Initialize and before every other hook: a device created before this runs
// is a device whose shaders can never be fingerprinted (D3D11 cannot hand back
// bytecode) and whose contexts we only ever find if it happens to present.
//
// That was measurable, and it was the real reason the viewmodel was unreachable:
// 201 of 1226 pixel shaders seen, and 0.4% of the frame's draws arriving at all.
bool InstallDeviceCreationHooks();

// Bracket the mod's OWN D3D device creation with these. Once device creation is
// hooked, the mod's throwaway probe device re-enters our hook and we start
// patching vtables inside the call that is building it - which is exactly the
// "a second D3D hooking layer is fighting us" failure render_hook warns about,
// and it killed startup at 0.064 s.
void BeginOwnDeviceCreation();
void EndOwnDeviceCreation();

// Called once per frame from Present; reports periodically.
// The camera's REAL half-tangents, recovered from the shared world->clip matrix
// the GPU is actually being given. false until a reading exists.
//
// This is what the per-eye crop must be sized from. Deriving it from the config
// instead is what produced a full-size rectangle and a double image: the
// swapchains are made ~6 s in, on the MENU camera, and the formula does not
// describe that camera at all - measured 64.97 deg there against 112.64 in play.
bool MeasuredCameraTangents(float* tanH, float* tanV);

// *** CAMERA SELECTION (taa_camera_lock v2) ***
//
// The resolve used to reproject through whatever camera block happened to be
// bound at its draw, and that binding is incidental - measured wandering by
// meters on 87 of 90 frames at rest, which is the universal object shake.
// Every main-view camera written during an eye's pass is collected here, and
// the resolve selects the one continuous with last frame's choice.
struct CamPick {
    int   candidates;   // distinct cameras offered this pass
    int   hits;         // writes that carried the chosen one
    int   maxHits;      // writes that carried the most-used one
    float wobble;       // how much the chosen camera moved WITHIN the pass
    float moved;        // how far the choice sits from last frame's
    float spread;       // how far the rejected candidates sat from it
};
void NoteCameraCandidate(ID3D11DeviceContext* ctx, const uint8_t* data,
                         uint32_t bytes);
bool PickCameraCandidate(int eye, const float* prev, bool hasPrev, float* out,
                         CamPick* info);
void CameraCandidatesReset();
long CameraCandidateOverflows();

// *** THE JITTER OVERLAY (taa_jitter_inject) ***
// The 8-position Halton offset added on top of the engine's 2-phase jitter at
// every main-view camera-block upload. The resolve publishes its render size
// (needed for the pixel->NDC conversion), advances the phase once per real
// frame, and reads the exact value applied so it can subtract it before
// estimating the engine's own part and report the exact sum to DLSS.
void TaaPublishRenderSize(unsigned w, unsigned h);
void JitterOverlayAdvance();
bool JitterOverlayNow(float* px, float* py);          // pixels, MV convention
long JitterOverlayInjectedLastSecond();
void JitterOverlayM15Stats(float* mn, float* mx, long* skipped);

// Is the player looking down a scope or a pair of binoculars, for the frame
// being drawn now? Answered by the optic's own lens-mask draw rather than by a
// game-state pointer, so there is nothing to re-find when the game updates.
// Latched one frame behind, so the answer is stable across every draw of a frame.
bool PlayerIsScoped();

void CBScanReport();

// Read-only. Searches the constant buffers bound RIGHT NOW to the TAA resolve
// for a camera matrix this file already tracks, straight or transposed, and logs
// where it found one. Called from the TAA probe at most once a second - it walks
// up to 8 KB per buffer and logs several lines, so it is a diagnostic, not a
// per-draw routine.
void CBScanResolveConstants(ID3D11DeviceContext* ctx, int eye, const char* what);

// Called at EVERY velocity-pass draw while the probe is armed - cheap, one
// snapshot lookup and a 64-byte copy. Remembers that pass's own +0x000 and asks,
// one frame later, whether +0x1D0 / +0x210 / +0x260 are bitwise equal to it.
// That is the only apples-to-apples test of the claim the matrix half rests on.
void CBNoteVelocityPass(ID3D11DeviceContext* ctx, int eye);
void CBReportVelocityMatch();

// The bytes last written to a constant buffer, from the snapshot table that
// Hook_Unmap and Hook_UpdateSubresource already fill. Read-only, and only valid
// until that buffer is written again - copy anything you need to keep.
bool CBSnapshotFor(ID3D11Resource* res, const uint8_t** dataOut,
                   uint32_t* bytesOut);

// Is this 4x4 the MAIN VIEW's world-to-clip matrix, rather than a shadow
// cascade, a reflection or the viewmodel's? The engine rewrites that constant
// block about ninety times a frame with different cameras, so anything that
// takes a matrix out of it and does not ask this question will sooner or later
// get one belonging to a different render. Returns the two projection scales
// and the near plane, which is how a caller can also reject a camera whose
// field of view is not the one it saw last time.
bool CBIsCameraClip(const float* m, float* aOut, float* bOut, float* nOut);

}  // namespace cotwvr
