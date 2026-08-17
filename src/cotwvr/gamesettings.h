#pragma once

namespace cotwvr {

// Writing the game's own settings.json before it reads it.
//
// The headset's eye frustum is TALLER than wide (0.916), while the game may be
// rendering anything - it was found running 5760x2160 (2.667), which leaves
// only ~39 degrees of vertical view and reads as looking through a letterbox.
// Cropping fixes the shape but throws away most of the rendered pixels (66% at
// that resolution), so the render itself wants to be near-square.
//
// Rather than requiring hand-made custom display modes, the mod rewrites
// DisplayWidth/DisplayHeight/GameFOV at process start. The mod's DLL is loaded
// via the XInput import, which happens before the game reads its settings, so
// the values are in place in time.
//
// Applied on the NEXT launch, because that is when the game reads the file.
struct RenderPreset {
    const char* name;
    int width;
    int height;
    const char* note;
};

int RenderPresetCount();
const RenderPreset& GetRenderPreset(int index);

// The full-frame size THIS LAUNCH was set up for, stamped when the resolution
// was written. Zero if no preset is selected or the write never happened.
//
// Anything that has to know "what should the headset be handed" reads this
// rather than the live panel row: the row takes effect at the next launch by
// design, and letting it act now made scrolling it rebuild the eye swapchains
// and change the picture mid-game.
void LaunchRenderTarget(int* w, int* h);

// Patch the game's settings.json with the selected preset. Safe to call when no
// preset is selected - it then does nothing.
void ApplyGameSettings();

}  // namespace cotwvr
