#pragma once

#include <d3d11.h>

namespace cotwvr {

// *** THE LAST THING THAT HAPPENS TO AN EYE BEFORE THE HEADSET SEES IT. ***
//
// Sharpening and saturation, applied to the FINISHED eye image on its way into
// the OpenXR swapchain - which is the only correct place for either.
//
// Sharpening has to be here, after the upscale, not before it. NVIDIA removed
// DLSS's own sharpener outright ("Sharpening is deprecated", Programming Guide
// 310.x) and expect the application to run its own pass on the OUTPUT; a
// sharpener that runs before a reconstruction is sharpening pixels that are
// about to be replaced, and the mod's existing taa_sharpen does exactly that
// when upscaling is on. Prior art agrees - Halo MCC VR ships bicubic + RCAS on
// the final image for the same reason.
//
// Saturation is here for a simpler reason: it is the only stage that sees the
// finished, tonemapped picture, and it is the picture the player is judging.
//
// The pass REPLACES the CopyResource that used to move the hold texture into
// the swapchain image, so when it is off it costs nothing at all and when it is
// on it costs one full-screen draw rather than a copy plus a draw.
//
// Returns false for any reason it could not run - no shader, no view, bad
// arguments - and the caller must then do the plain copy exactly as before.
// Never a half-processed eye.
bool PostProcessActive();

// dstViewFormat is the format the OpenXR swapchain was CREATED with, and it is
// not optional: the runtime hands out TYPELESS images (format 27 here), which
// have no default view format at all, so a null view description can never
// succeed on one. Only the caller knows which concrete format it asked the
// runtime for.
bool PostProcessApply(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                      ID3D11Texture2D* src, ID3D11Texture2D* dst,
                      unsigned w, unsigned h, DXGI_FORMAT dstViewFormat);
void PostProcessShutdown();

}  // namespace cotwvr
