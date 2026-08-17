#pragma once

namespace cotwvr {

// Small synthesised UI sounds for the in-headset panel.
//
// Synthesised rather than shipped as files: the mod stays three DLLs with no
// asset folder to lose, and a tone is trivially tunable. Everything is
// asynchronous - a panel that blocks the render thread to make a noise would be
// worse than a silent one.
namespace audio {

bool Init();
void Shutdown();

enum class Cue {
    Move,      // moved the selection
    Adjust,    // changed a value
    On,        // switched something on
    Off,       // switched something off
    Open,      // panel opened
    Close,     // panel closed
    Limit,     // hit the end of a range / nothing to do
};

void Play(Cue cue);
void SetVolume(float v);   // 0..1
float Volume();

}  // namespace audio
}  // namespace cotwvr
