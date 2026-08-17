#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#include <MinHook.h>

#include <string>

#include "apex.h"
#include "camera_probe.h"
#include "cbscan.h"
#include "config.h"
#include "frame_hook.h"
#include "gamesettings.h"
#include "headtrack.h"
#include "log.h"
#include "overlay.h"
#include "rdoc.h"
#include "render_hook.h"
#include "vr.h"

// The DLL's own module handle - config.cpp keeps cotwvr.ini beside the DLL.
HMODULE g_selfModule = nullptr;

namespace {

// The game statically imports XINPUT9_1_0.dll, dxgi.dll and d3d11.dll, but the
// import directory is walked in order and XInput comes first - so at our
// DllMain the renderer's DLLs may not be mapped yet.  Waiting for them beats
// racing them.
bool WaitForRendererModules(DWORD timeoutMs) {
    const DWORD start = GetTickCount();
    for (;;) {
        const bool haveDxgi = GetModuleHandleW(L"dxgi.dll") != nullptr;
        const bool haveD3d11 = GetModuleHandleW(L"d3d11.dll") != nullptr;
        if (haveDxgi && haveD3d11) return true;
        if (GetTickCount() - start > timeoutMs) {
            COTW_LOG("[init] timed out waiting for the renderer (dxgi=%d d3d11=%d)",
                     haveDxgi, haveD3d11);
            return false;
        }
        Sleep(50);
    }
}

DWORD WINAPI StartupThread(LPVOID) {
    cotwvr::LogInit();

    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    COTW_LOG("Host process: %ls", exe);

    // ReShade registers an OpenXR API layer that, when it fails to load, takes
    // xrCreateInstance down with it (XR_ERROR_FILE_ACCESS_ERROR) for every
    // OpenXR app on the machine.  The layer exposes an opt-out variable; setting
    // it here affects THIS PROCESS ONLY, so ReShade keeps working everywhere
    // else.
    SetEnvironmentVariableA("DISABLE_XR_APILAYER_reshade_1", "1");
    COTW_LOG("[init] disabled the ReShade OpenXR layer for this process");

    cotwvr::Cfg().Load();
    // Only now, and on purpose: everything above is what a bug report is
    // useless without, so it is written whatever this says.
    if (!cotwvr::Cfg().logging) {
        COTW_LOG("[init] logging = 0 - this is the last line until the game exits");
    }
    cotwvr::LogSetEnabled(cotwvr::Cfg().logging);
    if (!cotwvr::Cfg().enabled) {
        COTW_LOG("[init] enabled=0 in cotwvr.ini - installing nothing, the game runs stock");
        return 0;
    }

    cotwvr::apex::Init();

    // *** AN UNKNOWN BUILD STOPS HERE. ***
    //
    // Without a matching fingerprint every engine address is refused, so there
    // is no stereo, no head tracking and no camera - the mod cannot do the one
    // thing it is for. It used to carry on regardless: install the graphics
    // hooks, open an OpenXR session against the runtime, and only then fail.
    // A report from the Epic build showed where that ends - the log runs
    // happily to "adapter MATCH" and then the game dies inside xrCreateSession,
    // which reads as "the VR mod crashes my game" when the truth was decided
    // four hundred lines earlier and printed in capitals.
    //
    // Refusing here costs the player nothing they were going to get, and turns
    // a crash into a sentence they can act on.
    if (!cotwvr::apex::FingerprintMatches()) {
        COTW_LOG("[init] *** THIS GAME BUILD IS NOT SUPPORTED - INSTALLING NOTHING. ***");
        COTW_LOG("[init]   The mod is built for a specific version of the game and "
                 "recognises it by fingerprint. This executable is a different one:");
        COTW_LOG("[init]     - a game update usually means waiting for a mod update;");
        COTW_LOG("[init]     - a different store (Epic, Game Pass) is a different "
                 "build of the same version, and is not supported yet.");
        COTW_LOG("[init]   The game will now run completely normally, with no mod "
                 "loaded and nothing changed.");
        COTW_LOG("[init]   To try it anyway on your own head, set "
                 "apex_ignore_fingerprint = 1 in cotwvr.ini. It may crash; that is "
                 "the deal. If it WORKS, please say so - it is how the build gets "
                 "added properly.");
        return 0;
    }

    // Before the game reads its own settings. The mod is loaded through the
    // XInput import, which happens at process start, so there is time.
    cotwvr::ApplyGameSettings();

    // Capture mode: RenderDoc and this mod hook the same D3D entry points and
    // cannot coexist - with both active the game dies during startup.  When the
    // marker file is present, load RenderDoc and install none of our own
    // graphics hooks.  The game then runs flat, which is all a frame capture
    // needs.
    {
        wchar_t local[MAX_PATH]{};
        bool captureMode = false;
        if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
            const std::wstring marker =
                std::wstring(local) + L"\\theHunterCotWVR\\capture_mode";
            captureMode = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
        }
        if (captureMode) {
            COTW_LOG("[init] CAPTURE MODE: loading RenderDoc, installing NO VR hooks");
            cotwvr::InitRenderDoc();
            // The trigger poll normally rides on the Present hook, which capture
            // mode does not install - so it needs its own thread.
            CreateThread(
                nullptr, 0,
                [](LPVOID) -> DWORD {
                    for (;;) {
                        cotwvr::RenderDocPoll();
                        Sleep(250);
                    }
                },
                nullptr, 0, nullptr);
            return 0;
        }
    }

    // (No executable check here: apex::Init already fingerprints the build by
    // SizeOfImage AND PE timestamp, which is stricter than a file size, and the
    // features that write to fixed addresses already refuse to install when it
    // fails. A second, weaker check would only be another thing to keep in step.)

    if (!WaitForRendererModules(60000)) {
        COTW_LOG("[init] giving up - no D3D11 renderer appeared");
        return 0;
    }
    COTW_LOG("[init] renderer modules present");

    // Step-by-step from here, because a run that stopped dead at "renderer
    // modules present" gave no clue whether it crashed, hung, or simply
    // returned - and the crash dumps pointed at the game's own crash reporter,
    // which is a known red herring in this project.
    COTW_LOG("[init] initialising MinHook");
    const MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) {
        COTW_LOG("[init] MH_Initialize failed: %d", (int)s);
        return 0;
    }

    // FIRST, before any other hook. The game has not created its device yet -
    // WaitForRendererModules only waits for d3d11.dll to be MAPPED - so this is
    // the one moment where every device, and therefore every shader and every
    // context, can still be caught at birth. Installing it later is what left
    // the mod seeing 0.4% of the frame.
    if (cotwvr::Cfg().hook_device_creation) {
        COTW_LOG("[init] MinHook ready - hooking device creation before anything else");
        cotwvr::InstallDeviceCreationHooks();
    } else {
        COTW_LOG("[init] MinHook ready - device creation NOT hooked "
                 "(hook_device_creation=0). The mod sees only the presenting "
                 "device, which is ~0.4%% of the frame; turning it on finds all "
                 "five devices and currently crashes.");
    }

    COTW_LOG("[init] installing the DXGI hooks");
    if (!cotwvr::InstallRenderHooks()) {
        COTW_LOG("[init] render hooks did not install - the mod is inert this run");
    }

    // The same two hooks serve both jobs: the probe reads them, stereo writes
    // through them. Stereo therefore needs them installed even with the probe
    // switched off.
    COTW_LOG("[init] DXGI stage done - installing the camera hooks");
    if (cotwvr::Cfg().camera_probe || cotwvr::Cfg().stereo) {
        cotwvr::InstallCameraProbe();
    }
    COTW_LOG("[init] camera stage done");

    // *** THE CAUTION BELOW HAS EXPIRED, AND THAT IS WHY THIS CHANGED. ***
    //
    // This used to install the scene-draw hook ONLY when full-rate was already
    // on, and the reasoning was sound at the time: the hook was experimental,
    // it had once been installed with the wrong signature, and putting an
    // unproven detour into the known-good alternate-eye path broke a
    // configuration that had nothing to do with it. "Toggling full-rate is
    // worth a relaunch" was the right call for an unproven hook.
    //
    // It is not unproven any more - full-rate has been verified in the headset
    // since 2026-08-02, locked 90 fps, zero stash failures - and the cost of the
    // old rule was an asymmetry the owner hit immediately: launch in full-rate
    // and the panel can switch both ways, launch in alternate-eye and the
    // full-rate half of the row does nothing at all, silently. A control that
    // works in one direction only is worse than one that needs a restart, since
    // nothing on screen says which case you are in.
    //
    // So it is installed whenever stereo is, and left INERT by its own runtime
    // gate (`want` in frame_hook.cpp reads full_rate_stereo every frame). The
    // old behaviour is one setting away for anyone who suspects it.
    if (cotwvr::Cfg().stereo &&
        (cotwvr::Cfg().full_rate_stereo || cotwvr::Cfg().full_rate_hook_always)) {
        cotwvr::InstallFrameHook();
        if (!cotwvr::Cfg().full_rate_stereo) {
            COTW_LOG("[init] scene-draw hook installed but IDLE - full-rate is off, and "
                     "this is what lets the panel switch to it without a relaunch");
        }
    } else {
        COTW_LOG("[init] full-rate stereo off and full_rate_hook_always off - the "
                 "scene-draw hook is NOT installed, so the panel cannot switch to "
                 "full-rate until the next launch");
    }

    cotwvr::g_overlay.Init();
    return 0;
}

}  // namespace

// Called by the XINPUT9_1_0 proxy after every real pad read, so the state can be
// edited in place.  Nothing consumes it yet; it exists now because the settings
// panel and any stick-axis blocking both live here, and wiring it later would
// mean touching the proxy again.
extern "C" __declspec(dllexport) void __cdecl COTWVR_PadPostProcess(DWORD userIndex,
                                                                    XINPUT_STATE* state,
                                                                    DWORD result) {
    if (result != ERROR_SUCCESS || !state) return;
    // The settings panel gets first refusal on the pad, and blanks whatever it
    // consumes so the player does not walk about while adjusting a slider.
    //
    // The index has to go through. This is called once per SLOT the game polls,
    // and the panel keeps a button history per slot: with one shared history, a
    // second connected pad reporting all zeros wiped the real pad's state
    // between polls, so a held button read as a new press every time and one
    // press acted like ten. Steam Input's virtual device makes a second
    // "connected" pad the normal case.
    cotwvr::g_overlay.ConsumePad(userIndex, state);

    // LOOK UP AND DOWN WITH YOUR HEAD ONLY.
    //
    // Zeroed AFTER the panel has read the pad, so the panel's own up/down
    // navigation still works while the lock is on - it is meant for the game,
    // not for the settings list. Only the RIGHT stick's Y: the left stick walks,
    // and taking its Y away would stop the player moving forwards.
    if (cotwvr::Cfg().lock_pitch_input && cotwvr::Cfg().lock_pitch_pad) {
        state->Gamepad.sThumbRY = 0;
    }

    // THE RECENTRE CHORD, HIDDEN FROM THE GAME. In CotW the two stick clicks are
    // sprint and hold breath; both are harmless for the poll or two before the
    // chord completes, and neither is Start (which the panel's flat-screen route
    // needs).
    //
    // *** IT KEEPS MASKING UNTIL EVERY BIT IS UP, NOT ONLY WHILE ALL ARE DOWN.
    // *** XInput state is level-based. Masking on "all bits down" alone meant
    // that the instant the player released one stick the other stopped being
    // masked and arrived at the game as a RISING EDGE, having read as up for the
    // whole hold - so letting go of the chord sprinted or held breath, every
    // time, because nobody releases two thumbsticks inside one 1-4 ms poll.
    // Once the chord has been seen complete, whatever is still down stays hidden
    // until the player has let go of all of it.
    //
    // AFTER ConsumePad, for the same reason the pitch lock is: the panel must
    // still see the pad, only the game must not. The swallow is independent of
    // the hold timer in HeadTrackTick, so it cannot depend on frame ordering.
    {
        // Per slot: the game polls each one separately and a second connected pad
        // reporting zeros must not clear the real pad's latch.
        static bool chordHeld[4] = {false, false, false, false};
        const DWORD slot = userIndex < 4 ? userIndex : 0;
        const int chord = cotwvr::Cfg().recentre_pad_chord;
        // Same conditions the trigger runs under, master switch included: a
        // chord that cannot recentre must not eat the player's buttons either.
        const bool on = chord != 0 && cotwvr::Cfg().recentre_pad_enable &&
                        cotwvr::Cfg().recentre_pad_swallow &&
                        cotwvr::Cfg().recentre_compensate &&
                        cotwvr::Cfg().head_tracking;
        if (!on) {
            chordHeld[slot] = false;
        } else {
            const WORD mask = WORD(chord);
            const WORD down = WORD(state->Gamepad.wButtons & mask);
            if (down == mask) chordHeld[slot] = true;
            if (chordHeld[slot]) {
                state->Gamepad.wButtons = WORD(state->Gamepad.wButtons & ~mask);
                if (down == 0) chordHeld[slot] = false;   // all up: stop masking
            }
        }
    }
}

// Read by the stream_engine.dll proxy, which feeds it to the game's own Tobii
// "Extended View" head tracking. Returns false until the headset has produced a
// pose, so the proxy can report "not available" rather than inventing one.
extern "C" __declspec(dllexport) int __cdecl COTWVR_GetHeadEulerDegrees(float* pyr) {
    return cotwvr::HeadEulerDegrees(pyr) ? 1 : 0;
}

extern "C" __declspec(dllexport) int __cdecl COTWVR_GetHeadPositionMM(float* xyz) {
    return cotwvr::HeadPositionMM(xyz) ? 1 : 0;
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    switch (reason) {
        case DLL_PROCESS_ATTACH:
            g_selfModule = self;
            DisableThreadLibraryCalls(self);
            // Nothing heavy in DllMain: no LoadLibrary, no waiting.
            CreateThread(nullptr, 0, StartupThread, nullptr, 0, nullptr);
            break;
        case DLL_PROCESS_DETACH:
            cotwvr::RemoveRenderHooks();
            cotwvr::VR().Shutdown();
            cotwvr::g_overlay.Shutdown();
            cotwvr::LogShutdown();
            break;
        default:
            break;
    }
    return TRUE;
}
