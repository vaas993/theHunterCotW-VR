#pragma once

// The jitter hunt: locate the CPU-side home(s) of the view-projection matrix
// and name the instructions that write them. See jitterhunt.cpp for the whole
// story. Gated on Cfg().jitter_hunt; costs nothing when off.

namespace cotwvr {

// Called from the resolve when eye 0's VP has just been captured - the 64
// floats the GPU is provably using this frame.
void JitterHuntOnCapture(int eye, const float* vp16);

// Called once per real frame; logs progress and the writer report.
void JitterHuntTick();

// *** THE TAKEOVER (jitter_take) *** - detours the engine's jitter generator
// (apex::kJitterGenerator) so the whole renderer samples the mod's 8-phase
// Halton instead of the stock 2-phase, self-consistently. Install is lazy and
// once; the hook passes through untouched while the switch is off.
void JitterTakeInstall();
// Read the CURRENT camera's actual jitter from its composed projection - in
// pixels, MV convention. Works for every mode including stock; false if the
// chain is unreadable or the value is not jitter-sized.
bool JitterReadCurrent(unsigned w, unsigned h, float* px, float* py);
// Force the engine's own 16-phase jitter mode (jitter_mode3), once per frame.
void JitterMode3Tick();
// The exact value the given eye's render was jittered with this frame, in
// pixels (MV convention). false while the takeover is not running.
bool JitterTakeNow(int eye, float* px, float* py);
long JitterTakeCallsLastSecond();

}  // namespace cotwvr
