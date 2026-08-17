#pragma once

namespace cotwvr {

// Full-rate stereo: render the whole frame twice, once per eye.
//
// apex::kRenderFrame sets up the player view camera AND reaches Present within
// one call (proven by intersecting the stacks at both - framescan.cpp), and it
// runs on the presenting thread, so it can simply be called twice with the eye
// forced between the calls.
//
// Because it presents internally, the Present hook fires TWICE per real frame:
//   pass 0 - stash the left eye, submit nothing
//   pass 1 - stash the right eye, then submit both in one xrEndFrame
// Getting that wrong unbalances xrBeginFrame/xrEndFrame, which desynchronises
// the runtime for the whole session.
bool InstallFrameHook();
void RemoveFrameHook();

// 0 while the left eye is being rendered, 1 for the right. Only meaningful
// while FullRateActive().
int  CurrentRenderPass();

// True while we are inside our own second call, so re-entrancy is visible to
// anything that needs to care.
bool InDoubleRender();

// True for the whole of a full-rate frame. While this is set, the DXGI Present
// hook must NOT run the OpenXR frame loop on its own: it captures the eye for
// the current pass, and only pass 1 submits.
bool FullRateDriving();

// Reported by whoever actually captures the eye, so the failure count stays in
// one place next to the rest of the full-rate diagnostics.
void NoteStashResult(bool ok);

// The frame delta the ENGINE believes it is running at, in seconds, or -1 if it
// cannot be read. Compared against the real wall-clock frame interval in the
// perf log: if the engine's number is half of reality, everything time-based in
// the game runs at half speed and the game's own fps counter reads double.
float EngineFrameDelta();

}  // namespace cotwvr
