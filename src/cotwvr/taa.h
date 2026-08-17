#pragma once

#include <d3d11.h>

namespace cotwvr {

// THE TAA RESOLVE PROBE.
//
// Step 1 of docs\TAA_PER_EYE_PLAN.md: find the engine's temporal-resolve draw at
// runtime, in the headset, and answer the three questions a mono RenderDoc
// capture cannot. Zero behaviour change - it reads state and descriptions and
// issues nothing.
//
// READ-ONLY BY CONSTRUCTION, and that is not a style preference. These hooks run
// on DEFERRED contexts as well as the immediate one, and a Map(D3D11_MAP_READ)
// on a deferred context is not valid D3D11. A probe that did exactly that killed
// the weapon 3D shift for a day (docs\REGRESSION_WEAPON_3D.md). So: Get*,
// GetDesc, pointer identity. Never Map, never CopyResource, never a readback.

// *** THE ONE PLACE THAT KNOWS WHETHER THIS FILE NEEDS DRAWS. ***
//
// Four times now a switch has been added and one of its gates missed - the
// install gate, the call site, the internal gate - and each time the feature was
// simply never reached while looking, from outside, exactly like a feature that
// does not work. The owner found the last one by noticing that step 1 did
// nothing alone but worked when an unrelated switch was also on. That is a gate,
// every time. So there is now exactly one expression, and every gate calls it.
bool TaaWantsDraws();

// Called from Hook_Draw for every Draw() the game issues, ahead of any of the
// mod's own machinery, so an early return elsewhere cannot hide a draw from it.
void TaaOnDraw(ID3D11DeviceContext* ctx, UINT vertexCount);

// Called once per REAL frame from CBScanReport(). Under full-rate stereo that
// report runs on the SECOND pass only, so one call spans both eye passes - which
// is exactly the window "how many resolves per frame" has to be counted over.
void TaaOnFrame();

// *** WHAT THE HEADSET-SIDE UPSCALE NEEDS. ***
//
// The upscale runs after the engine has finished an eye - tonemapped, HUD and
// all - but DLSS needs the depth and motion vectors that eye was RENDERED
// with, and the sub-pixel jitter it was rendered at. The resolve is the last
// moment all three are provably current, so it publishes them here. Null or
// false means the resolve has not run for that eye, and the caller must fall
// back to the plain cropped copy.
ID3D11Resource* TaaMotionVectors();
ID3D11Resource* TaaDepthForEye(int eye);
bool TaaJitterForEye(int eye, unsigned w, unsigned h, float* jx, float* jy);

// PER-EYE TEMPORAL HISTORY.
//
// True when the draw TaaOnDraw was just handed IS the resolve and the
// substitution is live for this eye. The caller must then issue the draw
// between Begin and End - Begin swaps the history the shader reads, End puts
// the game's own binding back exactly and keeps this eye's resolved image.
//
// Set and read per THREAD, and cleared on entry to the next TaaOnDraw, so a
// resolve that some other feature swallows cannot leave it standing.
bool TaaResolvePending();
void TaaSubstituteBegin(ID3D11DeviceContext* ctx);
void TaaSubstituteEnd(ID3D11DeviceContext* ctx);

// STAGE 1 of the replacement route. True means the engine's temporal resolve has
// been intercepted and satisfied by us, and the caller must NOT issue the draw.
// False means fail-closed - issue it exactly as before.
//
// *** origDraw IS NOT OPTIONAL AND IT IS NOT A CONVENIENCE. ***
//
// Our own resolve is itself a Draw, and Draw on that context is OUR HOOK. The
// first build called ctx->Draw() and re-entered Hook_Draw, matched the same
// fingerprint, resolved again, and recursed until the stack died - the game
// crashed on the very first frame after the shaders finished building. The mod
// must issue its own draws through the ORIGINAL function pointer, never through
// the vtable it has hooked.
using TaaDrawFn = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
bool TaaReplacePass(ID3D11DeviceContext* ctx, TaaDrawFn origDraw);

}  // namespace cotwvr
