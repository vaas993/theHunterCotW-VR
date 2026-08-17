#include "cheats.h"

#include "apex.h"
#include "config.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cmath>

#include <MinHook.h>

namespace cotwvr {
namespace {

// *** THE TIME MANAGER NEEDS NO HOOK AT ALL. ***
//
// The cheat table hooks the hour getter to capture the object pointer, which is
// what you must do from Cheat Engine. Reading the same getter shows it does not
// need capturing:
//
//   mov   rax, [rip+0x20694B9]     ; a GLOBAL pointer to the time manager
//   test  rax, rax
//   je    no_time
//   movss xmm0, [rax+0xE8]         ; the hour
//   ret
//
// So the object is reachable from a global every frame - no injection, no
// trampoline, nothing to restore on exit, and nothing that can be left behind
// in a bad state if we crash. Prefer a global the code already dereferences
// over a hook whenever the disassembly offers one.
void* TimeManager() {
    void** slot = static_cast<void**>(apex::Rva(apex::kTimeManagerPtr));
    if (!slot) return nullptr;
    __try {
        return *slot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

float* TimeField(uint32_t offset) {
    void* tm = TimeManager();
    if (!tm) return nullptr;
    return reinterpret_cast<float*>(static_cast<char*>(tm) + offset);
}

// The weather object is NOT reachable from a global - its accessor is a bare
// leaf `movss xmm0,[rcx+0x1D0]; ret`, so `this` only exists while it is being
// called. Hook it and keep the pointer.
using PFN_WeatherGet = float(__fastcall*)(void*);
PFN_WeatherGet o_WeatherGet = nullptr;
void* volatile g_weather = nullptr;

float __fastcall Hook_WeatherGet(void* self) {
    if (self) g_weather = self;
    return o_WeatherGet(self);
}

bool g_installed = false;

// Frozen-at values, captured the moment the switch goes on so the world stops
// where the player actually is rather than snapping somewhere.
bool  g_timeFrozen = false;
float g_frozenHour = -1.0f;

// Print the neighbourhood of the hour ONCE, because +0xF4 being the time
// multiplier is inherited from a cheat table for a different build and has not
// been verified on ours. Writing an unverified offset in a live game is how you
// corrupt something unrelated and spend a day finding it.
void LogTimeLayoutOnce() {
    static bool done = false;
    if (done) return;
    void* tm = TimeManager();
    if (!tm) return;
    done = true;
    __try {
        const float* f = reinterpret_cast<const float*>(static_cast<char*>(tm) + 0xE0);
        const int* i = reinterpret_cast<const int*>(static_cast<char*>(tm) + 0xE0);
        COTW_LOG("[cheat] time manager %p - layout around the hour:", tm);
        for (int k = 0; k < 8; ++k) {
            COTW_LOG("[cheat]   +0x%02X  float %10.4f   int %d", 0xE0 + k * 4, f[k], i[k]);
        }
        COTW_LOG("[cheat]   (+0xE8 is the hour 0..24; +0xF4 is the suspected time "
                 "multiplier - confirm it against these numbers before trusting it)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

}  // namespace

bool GameHour(float* hour) {
    float* f = TimeField(apex::kTimeHourOffset);
    if (!f || !hour) return false;
    __try {
        const float v = *f;
        if (!(v >= -1.0f && v <= 25.0f)) return false;   // not a plausible hour
        *hour = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool InstallCheats() {
    if (g_installed) return true;
    if (!apex::FingerprintMatches()) {
        COTW_LOG("[cheat] not installed - build fingerprint does not match");
        return false;
    }
    if (void* wg = apex::Rva(apex::kWeatherSpeedGetter)) {
        MH_STATUS s = MH_CreateHook(wg, &Hook_WeatherGet, (void**)&o_WeatherGet);
        if ((s == MH_OK || s == MH_ERROR_ALREADY_CREATED) && MH_EnableHook(wg) == MH_OK) {
            COTW_LOG("[cheat] weather accessor hooked @0x%X", apex::kWeatherSpeedGetter);
        } else {
            COTW_LOG("[cheat] weather accessor hook failed (%d) - weather speed will "
                     "do nothing", (int)s);
        }
    }
    g_installed = true;
    return true;
}

void CheatsTick() {
    LogTimeLayoutOnce();

    // --- time of day ------------------------------------------------------
    float* hour = TimeField(apex::kTimeHourOffset);
    if (hour) {
        __try {
            // Setting a time is a one-shot: the panel puts an hour in, we apply
            // it and hand the value back to -1 so the player can still watch
            // time pass afterwards. A permanent write would be a freeze wearing
            // a different name.
            // ONE write is not enough, and freeze working while this did not is
            // what says why: the game writes the hour itself every frame, so a
            // single write is overwritten before it is ever displayed. Freeze
            // works precisely because it writes every frame. So hold the new
            // hour briefly - long enough to outlast the engine's own update and
            // for the sky to settle - then let go so time carries on.
            static int holdFrames = 0;
            static float holdHour = -1.0f;
            if (Cfg().cheat_set_hour >= 0.0f) {
                const float want = Cfg().cheat_set_hour;
                holdHour = want > 24.0f ? 24.0f : want;
                holdFrames = 90;                 // about a second at 90 fps
                COTW_LOG("[cheat] time of day -> %.2f (holding %d frames so the "
                         "engine's own update cannot undo it)", holdHour, holdFrames);
                Cfg().cheat_set_hour = -1.0f;
                g_frozenHour = holdHour;         // freeze lands here if it is on
            }
            if (holdFrames > 0) {
                *hour = holdHour;
                --holdFrames;
            }

            // --- how fast the day runs ---------------------------------
            //
            // NOT by writing the engine's own multiplier. The offset inherited
            // from a cheat table (+0xF4) is for a different build and has never
            // been checked against ours, and writing an unverified offset into a
            // live game is how you corrupt something unrelated and lose a day
            // finding it. LogTimeLayoutOnce prints that neighbourhood precisely
            // so nobody trusts it on faith.
            //
            // There is no need for it. The engine advances the hour itself every
            // frame - the very fact that made a single write useless above - so
            // the size of its own step can simply be MEASURED and re-applied
            // scaled. Whatever the engine did this frame, we do `scale` times as
            // much. Works below 1 and above it, needs no unknown field, and
            // stops dead the moment the setting returns to 1.
            static float ourHour = -1.0f;    // the hour we are driving
            static float lastWrote = -1.0f;  // so the engine's own step is the difference
            const float scale = Cfg().cheat_time_scale;
            const bool scaling = scale >= 0.0f && scale != 1.0f &&
                                 !Cfg().cheat_freeze_time && holdFrames <= 0;
            if (scaling) {
                const float now = *hour;
                if (ourHour < 0.0f || lastWrote < 0.0f) {
                    ourHour = now;               // first frame: start from the game's
                } else {
                    float step = now - lastWrote;   // what the engine added by itself
                    if (step < -12.0f) step += 24.0f;   // rolled past midnight
                    else if (step > 12.0f) step -= 24.0f;
                    if (step < 0.0f) step = 0.0f;       // never run the day backwards
                    ourHour += step * scale;
                    while (ourHour >= 24.0f) ourHour -= 24.0f;
                    while (ourHour < 0.0f) ourHour += 24.0f;
                }
                *hour = ourHour;
                lastWrote = ourHour;
            } else {
                ourHour = -1.0f;                 // let go cleanly; the engine's hour
                lastWrote = -1.0f;               // is already the live one
            }

            if (Cfg().cheat_freeze_time) {
                // Capture on the RISING EDGE, so turning it on holds the time
                // the player is actually in.
                if (!g_timeFrozen) {
                    g_timeFrozen = true;
                    g_frozenHour = *hour;
                    COTW_LOG("[cheat] time frozen at %.2f", g_frozenHour);
                }
                if (g_frozenHour >= 0.0f) *hour = g_frozenHour;
            } else if (g_timeFrozen) {
                g_timeFrozen = false;
                COTW_LOG("[cheat] time running again");
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    // --- weather ----------------------------------------------------------
    void* w = g_weather;
    if (w && Cfg().cheat_weather_speed >= 0.0f) {
        __try {
            float* speed = reinterpret_cast<float*>(
                static_cast<char*>(w) + apex::kWeatherSpeedOffset);
            *speed = Cfg().cheat_weather_speed;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

}  // namespace cotwvr
