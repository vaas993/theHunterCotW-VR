#include "render_hook.h"

#include "camera_probe.h"
#include "cbscan.h"
#include "cheats.h"
#include "config.h"
#include "dlss.h"
#include "frame_hook.h"
#include "framescan.h"
#include "headtrack.h"
#include "log.h"
#include "overlay.h"
#include "stereo.h"
#include "taa.h"
#include "vr.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include <MinHook.h>

namespace cotwvr {
namespace {

// IDXGISwapChain vtable slots.
//   0-2  IUnknown          QueryInterface / AddRef / Release
//   3-6  IDXGIObject       Set/GetPrivateData, SetPrivateDataInterface, GetParent
//   7    IDXGIDeviceSubObject::GetDevice
//   8    Present
//   9    GetBuffer
//  10-11 Set/GetFullscreenState
//  12    GetDesc
//  13    ResizeBuffers
constexpr int kSlotPresent = 8;
constexpr int kSlotResizeBuffers = 13;

using PFN_Present = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using PFN_ResizeBuffers = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT,
                                                      DXGI_FORMAT, UINT);

PFN_Present o_Present = nullptr;
PFN_ResizeBuffers o_ResizeBuffers = nullptr;

bool g_installed = false;
bool g_vrTried = false;
volatile LONG g_inPresent = 0;
uint64_t g_presentCount = 0;
IDXGISwapChain* g_swapChain = nullptr;   // not owned; valid for the process life

HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    // Some overlays re-enter Present; only the outermost call owns the frame.
    const LONG depth = InterlockedIncrement(&g_inPresent);

    // Whether THIS call is the one that submits the OpenXR frame and does the
    // once-per-frame housekeeping. Without full-rate every Present owns its
    // frame; with it, only the second eye's does.
    bool ownFrame = (depth == 1);

    if (depth == 1) {
        ++g_presentCount;
        g_swapChain = sc;
        if (g_presentCount == 1) {
            COTW_LOG("[render] first Present - we own the frame (swapchain %p)", (void*)sc);
            // Report the backbuffer whether or not OpenXR is switched on: an HDR
            // backbuffer cannot be copied into an 8-bit XR swapchain, and that
            // shows up as a black headset with a perfect-looking monitor.  Better
            // to know before anyone puts a headset on.
            DXGI_SWAP_CHAIN_DESC d{};
            if (SUCCEEDED(sc->GetDesc(&d))) {
                COTW_LOG("[render] backbuffer %ux%u format %d (%s) windowed=%d "
                         "buffers=%u refresh=%u/%u",
                         d.BufferDesc.Width, d.BufferDesc.Height,
                         (int)d.BufferDesc.Format, DxgiFormatName(d.BufferDesc.Format),
                         d.Windowed, d.BufferCount,
                         d.BufferDesc.RefreshRate.Numerator,
                         d.BufferDesc.RefreshRate.Denominator);
                if (DxgiFormatIsHdr(d.BufferDesc.Format)) {
                    COTW_LOG("[render] *** HDR IS ON. Turn it off in Windows display "
                             "settings and in the game, or the headset will be black "
                             "while the monitor looks fine. ***");
                } else {
                    COTW_LOG("[render] backbuffer is 8-bit - CopyResource into the XR "
                             "swapchain will work (HDR is off)");
                }
            }
        }

        // *** BEFORE OpenXR, AND THAT ORDER IS THE WHOLE FEATURE. ***
        //
        // The context hooks used to go in below, after VR().Init(). With a
        // headset connected that init costs ~0.6 s - session, swapchains,
        // layers - and in that window the engine finishes building the deferred
        // contexts it records the frame on. We then hook contexts it will never
        // use again, see 0.4% of the frame, and the weapon flickers instead of
        // responding.
        //
        // Proven by the owner, three launches, alternating: no headset -> the
        // weapon vanishes cleanly (we win the race because OpenXR init
        // short-circuits); headset on -> flicker; headset off again -> vanishes.
        // Same binary, same config, the only variable being ~0.6 s of VR init.
        //
        // So the hooks go in FIRST, at the top of the very first Present, before
        // anything slow. Nothing here depends on VR being up.
        // Same gate as the call further down - this is a REORDER, not a new
        // reason to install hooks. InstallCBScan is idempotent, so that call
        // becomes a no-op.
        if (g_presentCount == 1 && Cfg().enabled &&
            (Cfg().weapon_cb_scan || Cfg().shader_list ||
             Cfg().shader_hide_index >= 0 || Cfg().weapon_screen_stereo ||
             Cfg().weapon_hide || Cfg().weapon_pass_diag)) {
            ID3D11Device* dev = nullptr;
            if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev) {
                ID3D11DeviceContext* dc = nullptr;
                dev->GetImmediateContext(&dc);
                if (dc) { InstallCBScan(dc); dc->Release(); }
                dev->Release();
            }
        }

        if (Cfg().enabled && Cfg().submit_to_headset) {
            if (!g_vrTried && !VR().Ready()) {
                g_vrTried = true;   // one attempt; a retry loop in Present is a stutter machine
                COTW_LOG("[render] initialising OpenXR from Present");
                if (!VR().Init(sc)) {
                    COTW_LOG("[render] OpenXR init failed - the game continues on the "
                             "monitor as normal");
                }
            }
            // Full-rate draws the whole scene twice, so the engine's present
            // path - and therefore THIS function - runs twice per real frame.
            // The backbuffer only holds a finished eye image right here, before
            // the flip, so each pass captures its own eye and only the second
            // one owns the OpenXR frame. Submitting on both passes ran the whole
            // xrWaitFrame/Begin/End loop twice per frame, which is what wrecked
            // the engine's timing and desynchronised the runtime.
            if (VR().Ready()) {
                if (FullRateDriving()) {
                    NoteStashResult(VR().StashEyeFromBackbuffer(CurrentRenderPass()));
                    ownFrame = (CurrentRenderPass() == 1);
                }
                if (ownFrame) VR().SubmitFrame(sc);
            }
        }

        if (ownFrame) {
            // Reported from here rather than from inside the transform hook:
            // those helpers are hot, and logging inside one tanks the framerate.
            // Answers "which thread draws" and "is there a frame function" - the
            // two unknowns full-rate stereo depends on. Costs one stack capture,
            // once. Once per real frame, not once per eye.
            FrameScanNotePresent();
            FrameScanReport();

            // One attempt, from the frame path, once the device certainly
            // exists. DlssProbe latches internally so this cannot retry-loop.
            //
            // *** GATED ON dlss_enable, BECAUSE THIS IS NOT OPTIONAL. ***
            //
            // This call IS the NGX initialisation - the only one there is - and
            // it used to run only if `dlss_probe` was set. A setting named
            // "probe" reads as a diagnostic to everyone including the person
            // who wrote it, so shipping it off looked like tidiness and was in
            // fact switching DLSS off: NGX never came up, every evaluate
            // declined, and the log said "NGX NOT AVAILABLE ... see the [dlss]
            // lines for why" while there were no [dlss] lines at all, because
            // the code that writes them had never run.
            //
            // dlss_probe survives as what its name claims - bring NGX up even
            // with DLSS switched off, to see whether this machine has it - and
            // nothing depends on it any more.
            if (Cfg().dlss_enable || Cfg().dlss_probe) {
                ID3D11Device* ngxDev = nullptr;
                if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device),
                                            (void**)&ngxDev)) && ngxDev) {
                    DlssProbe(ngxDev);
                    ngxDev->Release();
                }
            }

            if (Cfg().camera_probe) CameraProbeTick(Cfg().camera_probe_every);
            ViewmodelScanTickPublic();
            // Constant-buffer hunt for the weapon's view matrix. Installed
            // lazily off the game's own immediate context, and only when asked.
            // Install for the shader hider too, not just the constant-buffer
            // scan. They share the same D3D hooks, but the expensive part -
            // inspecting every buffer's CONTENTS - stays gated on
            // weapon_cb_scan, so hunting a shader costs almost nothing.
            // The weapon features live on these same hooks, so they have to be
            // named in the gate. They were not: the viewmodel shift depended on
            // shader_list being left on for an unrelated reason, and turning
            // that off would have killed the shift with no message anywhere -
            // the exact shape of failure this project keeps paying for.
            // hud_probe is named here for the same reason: it lives on these
            // hooks and reports from CBScanReport, so leaving it out would make
            // it depend on an unrelated switch being on - and it would fail
            // SILENTLY, which is the shape this gate already exists to prevent.
            // taa_probe is named here for exactly the reason hud_probe is: it
            // lives on these hooks and reports from CBScanReport, so leaving it
            // out would make it depend on an unrelated switch being on, and it
            // would fail SILENTLY - the shape this gate exists to prevent.
            if (Cfg().weapon_cb_scan || Cfg().shader_list ||
                Cfg().shader_hide_index >= 0 || Cfg().weapon_screen_stereo ||
                Cfg().weapon_hide || Cfg().weapon_pass_diag || Cfg().hud_probe ||
                TaaWantsDraws() || Cfg().hud_isolate != 0) {
                ID3D11Device* dev = nullptr;
                if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev)) && dev) {
                    ID3D11DeviceContext* dc = nullptr;
                    dev->GetImmediateContext(&dc);
                    if (dc) { InstallCBScan(dc); dc->Release(); }
                    dev->Release();
                }
                CBScanReport();
            }
            StereoTick(Cfg().stereo_log_every);
            HeadTrackTick();
            InstallCheats();   // idempotent; the weather accessor may not exist yet
            CheatsTick();
            g_overlay.Tick();

            const int every = Cfg().log_frame_every;
            if (every > 0 && (g_presentCount % (uint64_t)every) == 0 && !VR().Ready()) {
                COTW_LOG("[render] present %llu (no VR session)",
                         (unsigned long long)g_presentCount);
            }
        }
    }

    InterlockedDecrement(&g_inPresent);

    // Only one of the two eyes needs to reach the monitor - they differ by
    // 64 mm and nothing else. The FIRST eye's flip is the one dropped, so the
    // single real Present still lands at the natural end of the frame where
    // DXGI's pacing expects it. Eye 0 has already been captured by then, so
    // letting the replay draw over it costs nothing.
    //
    // Only ever at depth 1: a nested Present belongs to an overlay that is in
    // the middle of its own work, and swallowing that would break it.
    if (depth == 1 && !ownFrame && FullRateDriving() &&
        Cfg().full_rate_skip_second_present) {
        return S_OK;
    }
    // *** THE MONITOR MUST NOT PACE THE HEADSET. ***  (prior art, lesson 7)
    //
    // This call is where the game's frame ends, and the OpenXR frame is
    // submitted inside it - so if the game presents with V-Sync on, DXGI blocks
    // here until the MONITOR's next refresh and the whole VR loop inherits the
    // monitor's clock instead of the runtime's. On a 60 Hz desktop beside a
    // 90 Hz headset that is a beat every third frame.
    //
    // Alternate-eye rendering punishes it hardest, and this is the reason it is
    // worth doing: each eye is only redrawn every other frame, so an uneven
    // beat leaves one eye stale for longer on some frames than others - and
    // irregular staleness is seen as flicker, which is far more noticeable than
    // steady lag. Halo MCC VR ships exactly this switch and states the trade in
    // one line: the desktop window may tear, the headset never does.
    //
    // Only while a session is live, and only the interval - the flags are the
    // game's own and are passed through untouched.
    if (Cfg().desktop_present_unlocked && sync != 0 && VR().Ready()) {
        static bool said = false;
        if (!said) {
            said = true;
            COTW_LOG("[render] desktop present unlocked (the game asked for "
                     "V-Sync %u) - the headset's refresh is now the only clock; "
                     "the monitor image may tear, which is expected", sync);
        }
        sync = 0;
    }

    // Always let the game present to the monitor, headset or not.  Skipping this
    // freezes the desktop window.
    return o_Present(sc, sync, flags);
}

HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* sc, UINT count, UINT w, UINT h,
                                             DXGI_FORMAT fmt, UINT flags) {
    COTW_LOG("[render] ResizeBuffers %ux%u format %d", w, h, (int)fmt);
    VR().OnResizeBuffers();
    return o_ResizeBuffers(sc, count, w, h, fmt, flags);
}

// A throwaway swapchain just to read the vtable.  Hooking through the vtable
// rather than the creation APIs is what makes this robust: the game creates its
// device and swapchain in whatever order it likes and we never have to see it.
HWND CreateDummyWindow() {
    WNDCLASSEXW wc{sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"CotWVRDummy";
    RegisterClassExW(&wc);
    return CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 1, 1,
                           nullptr, nullptr, wc.hInstance, nullptr);
}

}  // namespace

bool InstallRenderHooks() {
    if (g_installed) return true;

    COTW_LOG("[render] creating the probe window");
    HWND wnd = CreateDummyWindow();
    if (!wnd) {
        COTW_LOG("[render] could not create the dummy window (%lu)", GetLastError());
        return false;
    }

    DXGI_SWAP_CHAIN_DESC d{};
    d.BufferCount = 1;
    d.BufferDesc.Width = 1;
    d.BufferDesc.Height = 1;
    d.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Windowed = TRUE;
    d.OutputWindow = wnd;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    d.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    COTW_LOG("[render] creating the probe device - if the log stops HERE, a second D3D hooking layer is fighting us");
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};

    // Device creation is hooked by now, so this call comes straight back into
    // our own hook - which then patches vtables inside the call still building
    // this device. That is the "second D3D hooking layer" the line above warns
    // about, with us as the second layer, and it killed startup at 0.064 s.
    cotwvr::BeginOwnDeviceCreation();
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, _countof(levels),
        D3D11_SDK_VERSION, &d, &sc, &dev, &got, &ctx);
    cotwvr::EndOwnDeviceCreation();
    // Logged IMMEDIATELY: several hung runs end on the line above, and whether
    // the process is stuck inside this call or somewhere after it is exactly
    // what could not be told from the log.
    COTW_LOG("[render] probe device create returned 0x%08X", (unsigned)hr);
    if (FAILED(hr) || !sc) {
        COTW_LOG("[render] D3D11CreateDeviceAndSwapChain for the vtable probe failed: 0x%08X",
                 (unsigned)hr);
        DestroyWindow(wnd);
        return false;
    }

    // Shader creation first, and from THIS device: all D3D11 devices share a
    // vtable, so the probe is enough - and doing it here rather than at first
    // Present is what lets the weapon's shaders be seen at all.
    InstallShaderCreationHooks(dev);

    void** vt = *reinterpret_cast<void***>(sc);
    COTW_LOG("[render] swapchain vtable %p (Present=%p ResizeBuffers=%p)",
             (void*)vt, vt[kSlotPresent], vt[kSlotResizeBuffers]);

    bool ok = true;
    MH_STATUS s = MH_CreateHook(vt[kSlotPresent], &Hook_Present, (void**)&o_Present);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
        COTW_LOG("[render] MH_CreateHook(Present) failed: %d", (int)s);
        ok = false;
    }
    s = MH_CreateHook(vt[kSlotResizeBuffers], &Hook_ResizeBuffers, (void**)&o_ResizeBuffers);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
        COTW_LOG("[render] MH_CreateHook(ResizeBuffers) failed: %d", (int)s);
        ok = false;
    }

    if (ok) {
        s = MH_EnableHook(MH_ALL_HOOKS);
        if (s != MH_OK) {
            COTW_LOG("[render] MH_EnableHook failed: %d", (int)s);
            ok = false;
        }
    }

    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(wnd);

    if (ok) {
        g_installed = true;
        COTW_LOG("[render] DXGI hooks installed - waiting for the game's first Present");
    }
    return ok;
}

IDXGISwapChain* CurrentSwapChain() { return g_swapChain; }

void RemoveRenderHooks() {
    if (!g_installed) return;
    MH_DisableHook(MH_ALL_HOOKS);
    g_installed = false;
    COTW_LOG("[render] hooks removed");
}

}  // namespace cotwvr
