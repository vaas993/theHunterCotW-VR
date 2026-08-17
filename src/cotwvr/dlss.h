#pragma once

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Resource;

namespace cotwvr {

// DLSS, through NVIDIA's NGX SDK.
//
// STEP ONE ONLY: bring NGX up against the game's own device and ask whether
// DLSS is available, then say so in the log. Nothing is rendered and no feature
// is created yet.
//
// That order is deliberate. NGX init inside a modded process is the step most
// likely to fail outright - it has to find the runtime, agree with the driver,
// and coexist with our own D3D hooks, an OpenXR session and an overlay - and
// every later piece is wasted if it cannot. It also reports WHY when it fails,
// which is the difference between "DLSS is off" and "DLSS needs a newer driver".
//
// Called from the frame boundary, once, and only when dlss_probe is on.
void DlssProbe(ID3D11Device* dev);

// True once NGX is up and DLSS reported itself available.
bool DlssAvailable();

// Run DLSS in place of our own resolve, for one eye.
//
// One feature per eye, created on first use at the render size and swapped by
// the caller's eye index - the AC Black Flag pattern from
// VR_PRIOR_ART_LESSONS.md lesson 1. Because each eye owns its own feature, it
// owns its own history, and the cross-eye contamination this whole effort has
// been about cannot occur.
//
// DLAA to begin with: render size == output size, so nothing in the VR submit
// path changes dimensions and the result is comparable like-for-like with the
// picture the mod produces today.
//
// Returns false for ANY reason it could not run, and the caller must then issue
// its own resolve exactly as before. Never a half-done frame.
bool DlssEvaluate(ID3D11DeviceContext* ctx, int eye,
                  ID3D11Resource* colour, ID3D11Resource* depth,
                  ID3D11Resource* motion, ID3D11Resource* target,
                  unsigned w, unsigned h, float jitterX, float jitterY,
                  bool reset);

// *** UPSCALING - a SECOND feature set, sized in -> out. ***
//
// The DLAA path above lives inside the engine's temporal resolve, where the
// colour is still pre-tonemap HDR and every pass after it expects the render
// resolution - so it cannot change size there without redirecting the whole
// post chain and the UI with it. Upscaling therefore happens where each eye's
// FINISHED image is captured for the headset: colour is the final backbuffer
// (tonemapped, HUD included), while depth and motion vectors are the ones that
// eye was just rendered with. All three are addressed through one common
// subrect, so DLSS sees exactly the cropped region the headset is shown.
//
// srcX/srcY/srcW/srcH describe that crop inside the full-size inputs; outW/outH
// are the (larger) size the eye's hold texture and swapchain were made at.
bool DlssUpscale(ID3D11DeviceContext* ctx, int eye,
                 ID3D11Resource* colour, ID3D11Resource* depth,
                 ID3D11Resource* motion, ID3D11Resource* out,
                 unsigned srcX, unsigned srcY, unsigned srcW, unsigned srcH,
                 unsigned outW, unsigned outH,
                 float jitterX, float jitterY, bool reset);

// Drop the upscaler's features and history. Call when the textures it writes
// into have been destroyed and remade - switching DLSS off and on does that -
// so nothing can be left accumulating against a resource that no longer exists.
// The next evaluate rebuilds, at the cost of one soft frame.
void DlssUpscaleInvalidate();

// Released on shutdown. Safe to call if init never succeeded.
void DlssShutdown();

}  // namespace cotwvr
