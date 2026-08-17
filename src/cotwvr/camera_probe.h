#pragma once

namespace cotwvr {

// Runtime identification of the player camera.
//
// The engine builds every transform through two generic helpers
// (apex::kBuildTransformA/B).  43 call sites use them, so "which of these is
// the camera" cannot be answered statically - it is answered by watching what
// each CALL SITE actually does per frame:
//
//   the player camera is called ~once per frame, its position drifts as the
//   player walks, and its basis swings as the player looks.
//   a shadow cascade is called several times per frame with the same basis.
//   a prop's transform barely moves at all.
//
// The probe therefore records, per call site: how many times it fired, and the
// resulting matrix.  It logs NOTHING from inside the hook - these helpers are
// hot, and a log call (or worse, a module lookup) inside one is how you tank
// the framerate.  Aggregation is interlocked-only; the report is printed later
// from the Present hook, on a normal thread.
bool InstallCameraProbe();
void RemoveCameraProbe();

// Called from Present. Prints the per-call-site table every `everyFrames`.
void CameraProbeTick(int everyFrames);

// Has the PLAYER camera drawn in the last quarter second? False in the main
// menu, where there is no player camera - and where replaying the frame for a
// second eye put the menu into slow motion, because the menu runs on its own
// timing rather than the world's.
bool PlayerCameraActive();

// Advances the viewmodel scan (sweeps one call site at a time) and watches for
// the CTRL+ALT+W mark. Called once per frame from Present.
void ViewmodelScanTickPublic();

}  // namespace cotwvr
