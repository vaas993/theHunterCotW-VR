#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

namespace cotwvr {

// Finding and holding the scene's depth buffer.
//
// Same-instant stereo without re-rendering: take the one frame the game drew,
// and use its depth to synthesise the other eye's viewpoint. Both eyes then come
// from the same moment by construction, which is the thing alternate-eye cannot
// do and the thing that makes near objects uncomfortable while turning.
//
// The depth buffer is found by watching OMSetRenderTargets and keeping the
// depth-stencil view that (a) matches the backbuffer's dimensions and (b) is
// bound for the most draw calls in a frame - the main scene pass, as opposed to
// shadow cascades, which are numerous but a different size.
bool InstallDepthHooks();
void RemoveDepthHooks();

// The scene depth as a shader-readable texture, or nullptr if not found yet.
ID3D11ShaderResourceView* SceneDepthSRV();

// Called once per frame from Present: promotes the best candidate and logs.
void DepthTick(int logEvery);

}  // namespace cotwvr
