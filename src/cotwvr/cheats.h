#pragma once

// Game cheats driven from the in-headset panel.
//
// Everything here came out of community cheat tables (see the .CT files in the
// workspace and in cheat engine\). Their value is not the cheat - it is that a
// working script is a PROVEN POINTER into the code that owns a value, which is
// worth more than any probe. That is how the aim was finally found, and the
// same tables carry the time of day and the weather.

namespace cotwvr {

// Once per frame, from Present. Applies whatever the panel has switched on.
void CheatsTick();

// Install the hooks the cheats need (only the weather object needs one; the
// time manager is reachable through a global pointer and needs nothing).
bool InstallCheats();

// Current in-game hour, 0..24. Returns false if the time manager is not up yet
// - in the main menu there is no world and nothing to read.
bool GameHour(float* hour);

}  // namespace cotwvr
