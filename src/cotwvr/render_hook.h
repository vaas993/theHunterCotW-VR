#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dxgi.h>

namespace cotwvr {

// Installs the DXGI swapchain hooks (Present + ResizeBuffers).  Safe to call
// once; returns false and logs the reason if anything went wrong.
bool InstallRenderHooks();
void RemoveRenderHooks();

// The game's live swapchain, captured at the first Present. Full-rate stereo
// needs it from inside the scene-draw hook, which the engine calls without any
// reference to it.
IDXGISwapChain* CurrentSwapChain();

}  // namespace cotwvr
