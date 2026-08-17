#pragma once

namespace cotwvr {

// RenderDoc, driven from inside the game process.
//
// RenderDoc and this mod hook the same D3D entry points and cannot coexist, so
// capture mode installs NO graphics hooks of our own - the game runs flat and
// the capture is all we want from that run.  Entered by creating the marker
// file %LOCALAPPDATA%\theHunterCotWVR\capture_mode.
//
// A capture is fired by dropping %LOCALAPPDATA%\theHunterCotWVR\capture.trigger,
// which means the person playing just plays: they get to a useful in-game
// moment and the capture is taken from outside, with no hotkey to fumble for.
bool InitRenderDoc();
bool RenderDocReady();
void RenderDocPoll();          // watches for the trigger file
void TriggerRenderDocCapture();

}  // namespace cotwvr
