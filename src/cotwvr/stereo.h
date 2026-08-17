#pragma once

namespace cotwvr {

// Alternate-eye stereo.
//
// The engine renders one frame per Present, so true stereo needs that frame
// rendered twice - which needs the engine's per-frame render entry point, and
// whether it can be called twice depends on which thread actually draws.  That
// is not established yet.
//
// Alternate-eye gets stereo working without any of that: shift the camera to
// the left eye on one frame and the right eye on the next, and hold each eye's
// most recent image in its swapchain.  Each eye then updates at half rate
// (45 Hz against a 90 Hz headset) but the pair is a genuine stereo pair, with
// real disparity, from the game's own renderer.  The playbook recommends
// exactly this as the first version, and it ships as a low-spec mode in real
// mods.
//
// The awkward part is the split found by measurement (see apex.h): the object
// whose position steers the picture is BODY-aligned, while the eye separation
// has to run along the VIEW's right axis.  So orientation is read at one hook
// and position is written at another.

// Which eye the frame currently being built belongs to. 0 = left, 1 = right.
int  CurrentRenderEye();

// This eye's camera world position for the render just set up, taken at the
// position lever AFTER the eye offset is applied - i.e. the exact origin the
// engine's camera-relative matrices are relative to. False until the first
// render pass has gone through.
bool EyeWorldPosition(int eye, float* xyz);

// Called once per Present, after the frame has been consumed. Alternate-eye
// only - full-rate sets the eye explicitly instead.
void AdvanceRenderEye();

// --- full-rate stereo ------------------------------------------------------
// The engine's frame function is called twice per real frame with the eye
// forced between calls, so both eyes come from the SAME instant. This is what
// alternate-eye cannot do, and it is why near objects were uncomfortable: the
// two eyes were seeing moments 11 ms apart, and motion corrupted the disparity.
void SetRenderEye(int eye);
bool FullRateActive();
void SetFullRateActive(bool on);

// Called from the camera hook that tracks the view (apex::kCameraBasisSource).
// Stores the world-space basis rows so the eye offset can be built from them.
void NoteViewBasis(const float* matrix16);

// Called from the hook that actually steers the picture
// (apex::kCameraPositionLever).  Adds this frame's eye offset to the matrix's
// position row, in world space, so the body-aligned basis of that matrix does
// not matter.
void ApplyEyeOffset(float* matrix16);

// The same shift, scaled, and the scale may be negative. The viewmodel needs
// -1: the held weapon rides the camera, so it inherits the eye shift, lands on
// identical pixels in both eyes and reads as infinitely distant - which is why
// the world has depth and the weapon does not. Cancelling the inherited shift
// leaves it world-stable and it gains the disparity of a real nearby object.
void ApplyEyeOffsetScaled(float* matrix16, float scale);

// The camera's world position and basis as last seen at the lever. Used as a
// SIGNATURE for finding the view matrix inside the game's D3D11 constant
// buffers: the weapon is not built by any transform-builder call site, so the
// only remaining place to give it stereo depth is the render view itself, and
// the way to find that is to look for numbers we already know.
bool CurrentCameraMatrix(float* out16);

// Diagnostics: how many times each hook fired, so an uneven cadence (the lever
// fires ~0.65x per frame, not once) is visible rather than guessed at.
void StereoTick(int everyFrames);

bool StereoActive();

}  // namespace cotwvr
