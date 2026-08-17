#pragma once

namespace cotwvr {

// Finding the engine's per-frame render entry point.
//
// Full-rate stereo needs to call "render one whole frame" twice with the eye
// swapped between calls, so both eyes come from the SAME instant. Two things
// have to be established first, and both are answered by observation rather
// than by reading disassembly:
//
//   1. Is the scene built on the same thread that calls Present? If the engine
//      defers drawing to a worker, calling the frame function twice cannot
//      work and stereo needs a completely different attack.
//   2. Which function is it? It must set up the player view camera AND reach
//      Present within one call - so it is an ANCESTOR of Present on the stack,
//      and also an ancestor of the camera hook we already own.
//
// The stack at Present names the candidates directly. Intersecting that with
// the stack at the camera lever gives the function that contains both, which is
// the frame function by definition.
void FrameScanNotePresent();     // called from the Present hook
void FrameScanNoteCamera();      // called from the camera position lever hook
void FrameScanReport();          // called once, from Present, after both are seen

}  // namespace cotwvr
