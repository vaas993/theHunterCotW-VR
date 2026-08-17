#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

// openxr_platform.h only declares the D3D11 binding/graphics-requirements/
// swapchain-image structs when these are defined first.  Without them the
// header compiles cleanly and every D3D11 type is simply absent.
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstdint>
#include <vector>

namespace cotwvr {

// OpenXR plumbing.
//
// theHunter: Call of the Wild is a native D3D11 x64 title, so the session binds
// to the GAME's own device directly - no private device, no shared textures.
// (The Far Cry 2 mod needed that dance only because D3D10 has no OpenXR
// graphics binding.)
class VRSystem {
public:
    // Called from the Present hook once the real swapchain exists.
    bool Init(IDXGISwapChain* swapChain);
    void Shutdown();

    bool Ready() const { return m_session != XR_NULL_HANDLE; }
    bool Failed() const { return m_failed; }

    // What the panel needs to tell the player the truth about the quality
    // level: the size the game is rendering an eye at right now, and the size
    // the headset is being handed. Zeroes before the first frame.
    void CurrentSizes(uint32_t* renderW, uint32_t* renderH,
                      uint32_t* outW, uint32_t* outH) const {
        *renderW = m_width; *renderH = m_height;
        *outW = m_outW;     *outH = m_outH;
    }

    // The WHOLE frame the game is drawing, before the crop - which is the
    // number to compare against the resolution written into settings.json, and
    // therefore the only honest way to tell "this setting is already in effect"
    // from "this setting needs a restart".
    void CurrentFrameSize(uint32_t* w, uint32_t* h) const {
        *w = m_srcWidth; *h = m_srcHeight;
    }

    // The whole frame loop, driven from Present, before the original runs
    // (the backbuffer holds the finished frame at that point).
    void SubmitFrame(IDXGISwapChain* swapChain);

    // Full-rate stereo: copy the current backbuffer into one eye's hold texture
    // without touching the OpenXR frame loop. Called from the scene-draw hook
    // between the two passes, before the second overwrites the first.
    bool StashEyeFromBackbuffer(int eye);

    // Resolution changed - cached per-eye resources are the wrong size now.
    void OnResizeBuffers();

    // The head pose from the most recent xrLocateViews, in the LOCAL reference
    // space. Head tracking needs this on the game thread, where no OpenXR call
    // is safe to make, so it is cached here each frame.
    bool HeadPose(XrQuaternionf* orientation, XrVector3f* position) const;

    uint64_t FrameCount() const { return m_frames; }
    uint64_t SubmittedCount() const { return m_submitted; }
    const char* StateName() const;

private:
    struct EyeSwapchain {
        XrSwapchain handle = XR_NULL_HANDLE;
        std::vector<ID3D11Texture2D*> images;  // owned by the runtime
    };

    bool CreateInstance();
    bool CreateSession(ID3D11Device* device);
    bool CreateSwapchains(uint32_t width, uint32_t height);
    void ComputeCrop(uint32_t srcW, uint32_t srcH, const XrFovf& headsetFov);
    void DestroySwapchains();
    void PollEvents();
    bool PickSwapchainFormat(DXGI_FORMAT backbufferFormat, int64_t* chosen) const;

    XrInstance m_instance = XR_NULL_HANDLE;
    XrSystemId m_systemId = XR_NULL_SYSTEM_ID;
    XrSession  m_session  = XR_NULL_HANDLE;
    XrSpace    m_space    = XR_NULL_HANDLE;
    XrSessionState m_state = XR_SESSION_STATE_UNKNOWN;
    bool m_sessionRunning = false;
    bool m_failed = false;
    bool m_hdrWarned = false;

    EyeSwapchain m_eyes[2];
    int64_t m_swapchainFormat = 0;
    DXGI_FORMAT m_backbufferFormat = DXGI_FORMAT_UNKNOWN;
    uint32_t m_width = 0;      // eye swapchain size = the CROPPED region
    uint32_t m_height = 0;

    // What the headset is actually HANDED. Equal to m_width/m_height unless DLSS
    // is upscaling, in which case the swapchains and the hold textures are this
    // size and the crop above is only what the game draws. Nothing else in this
    // class may use these: every crop, projection and FOV calculation is in
    // render pixels, and mixing the two is how a good picture goes crooked.
    uint32_t m_outW = 0;
    uint32_t m_outH = 0;
    // The quality percentage as it stood when this launch's swapchains were
    // built. Only used where there is no launch target to work from, and
    // latched rather than read live for the same reason everything else here
    // is: the quality row describes the next launch, not this one.
    int m_launchPct = 100;

    // What the last capture ACTUALLY wrote into each hold texture. Upscaled
    // size when DLSS ran, render size when it declined, zero before either.
    // The submitted image rect reads these, so a fallback frame is honest.
    uint32_t m_holdW[2] = {0, 0};
    uint32_t m_holdH[2] = {0, 0};

    // The game renders whatever shape its own settings dictate - 2.667:1 when
    // this was last measured - while the headset's eye frustum is 0.916:1.
    // Showing the whole frame means a letterbox strip with ~44 degrees of
    // vertical view. Instead a centred region of the backbuffer, shaped like the
    // headset's frustum, is what gets sent.
    uint32_t m_srcWidth = 0;   // full backbuffer
    uint32_t m_srcHeight = 0;
    // TIER 1: each eye takes its OWN rectangle of the rendered frame, and is
    // told the fov that rectangle actually spans. Both rects are the same SIZE
    // here (the headset is symmetric), so the swapchains stay as they are - but
    // the offsets differ, and that difference is the whole feature.
    uint32_t m_eyeCropX[2] = {0, 0};
    uint32_t m_eyeCropY[2] = {0, 0};
    uint32_t m_eyeCropW[2] = {0, 0};
    uint32_t m_eyeCropH[2] = {0, 0};
    XrFovf   m_eyeFov[2] = {};
    bool     m_haveEyeCrop = false;
    uint32_t m_cropX = 0;      // left edge of the region taken
    uint32_t m_cropY = 0;

    // Every image in both eye swapchains has been acquired, cleared and
    // released at least once, so leaving one untouched for a frame re-composites
    // a known-good image instead of whatever was in video memory.
    bool m_eyesPrimed = false;

    // What each eye's image was actually drawn from. Under alternate-eye
    // rendering the two are a frame apart, and submitting one pose for both
    // makes the runtime reproject the stale eye from the wrong origin.
    XrPosef m_eyeRenderPose[2] = {};
    bool m_haveEyePose[2] = {false, false};

    // The position promised to the compositor while submit_frozen_position is
    // on. Latched once: the engine rotates with the head but never MOVES with
    // it, so a live position asks the runtime to reproject for translation
    // the pixels do not contain - which is a headset-only shimmer.
    XrVector3f m_frozenPos = {};
    bool m_haveFrozenPos = false;
    XrQuaternionf m_frozenOri = {};
    bool m_haveFrozenOri = false;

    // What the latched crop was computed from. The rectangle only moves when
    // these really change - float noise in the runtime's per-frame fov used to
    // shift it by a pixel or two every frame, which the headset shows as a
    // vibration and the (uncropped) desktop mirror does not.
    XrFovf m_cropFov[2] = {};
    uint32_t m_cropSrcW = 0;
    uint32_t m_cropSrcH = 0;

    ID3D11Device* m_device = nullptr;          // the game's device (not owned)
    ID3D11DeviceContext* m_context = nullptr;  // its immediate context (owned ref)

    // Alternate-eye stereo needs each eye to keep showing its most recent image
    // while the other eye is the one being rendered.  The XR swapchain cycles
    // through several images, so its previous contents cannot be relied on -
    // hence a texture of our own per eye.
    ID3D11Texture2D* m_hold[2] = {nullptr, nullptr};
    bool CreateHoldTextures();
    void DestroyHoldTextures();
    // Creates (or re-creates) both eye swapchains at m_outW/m_outH and primes
    // them black. Safe to call again while running, but only between frames.
    bool BuildEyeSwapchains();
    // The size the headset should be handed for a given rendered crop: the
    // player's chosen resolution, whatever the game happens to be drawing.
    void UpscaleTarget(uint32_t inW, uint32_t inH,
                       uint32_t* outW, uint32_t* outH) const;
    // Once per frame: if the output size the settings now imply differs from the
    // swapchains we have, rebuild them. This is what makes the quality level a
    // live setting rather than a restart.
    void EnsureOutputSize();
    // Capture one eye's finished image into its hold texture: DLSS upscale when
    // it is on and everything it needs is present, plain crop copy otherwise.
    bool StashInto(int eye, ID3D11Texture2D* back, const D3D11_BOX& boxIn);
    // The rectangle DLSS reconstructs from, latched per eye. Its history is
    // keyed to this rectangle, so it must not move under the accumulation even
    // when the player's own crop does.
    uint32_t m_upRectX[2] = {0, 0}, m_upRectY[2] = {0, 0};
    uint32_t m_upRectW[2] = {0, 0}, m_upRectH[2] = {0, 0};
    uint32_t m_upRectSrcW[2] = {0, 0}, m_upRectSrcH[2] = {0, 0};
    // The frustum that rectangle spans, latched WITH it. Submitting the live
    // angles over latched pixels presents each eye at slightly the wrong angle,
    // by a different amount per eye - which is double vision.
    XrFovf m_upFov[2] = {};
    bool   m_haveUpFov[2] = {false, false};
    // Which crop mode the rectangle was latched in, so switching the per-eye
    // crop re-latches instead of keeping one from the other mode.
    bool   m_upRectPerEye[2] = {false, false};
    D3D11_BOX CropBox() const;
    D3D11_BOX CropBox(int eye) const;
    // Fills m_eyeCropX/Y and m_eyeFov from the runtime's per-eye tangents and
    // the half-tangents the frame was actually rendered with.
    void ComputeEyeCrops(uint32_t srcW, uint32_t srcH, const XrView* views);

    // The settings panel: a head-locked quad layer composited on top of the
    // projection layer.
    XrSpace     m_viewSpace = XR_NULL_HANDLE;
    XrSwapchain m_overlaySwapchain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> m_overlayImages;
    ID3D11Texture2D* m_overlayStaging = nullptr;
    int64_t m_overlayFormat = 0;
    bool CreateOverlayResources();
    void DestroyOverlayResources();

    // The flat screen: the game's own image on a floating panel, so its menus
    // are readable instead of stretched across the whole field of view. Same
    // quad mechanism as the settings panel above, fed from the backbuffer.
    //
    // Manual     - the hotkey or the overlay toggle.
    // MainMenu   - no player camera. Right for the main menu only.
    // InGameMenu - the mouse cursor is showing while the game has focus. The
    //              in-game menu leaves the world running, so the camera test
    //              cannot see it; a menu you click through needs a cursor.
    enum class ScreenReason { None = 0, Manual, MainMenu, InGameMenu };
    XrSwapchain m_screenSwapchain = XR_NULL_HANDLE;
    std::vector<ID3D11Texture2D*> m_screenImages;   // owned by the runtime
    uint32_t m_screenW = 0;
    uint32_t m_screenH = 0;
    ScreenReason m_lastScreenReason = ScreenReason::None;
    bool MenuCursorVisible() const;
    ScreenReason MenuScreenReason() const;
    // GetTickCount when the cursor last BECAME visible; 0 while it is not.
    // The in-game-menu test requires it to STAY up, because a single sample
    // fires on ordinary mouse-look and drops the flat screen in front of the
    // player mid-hunt - measured, 26 times in one session. Mutable because
    // MenuScreenReason() is const and this is only its memory of the last call.
    mutable DWORD m_cursorUpSince = 0;
    // Reports the moment any candidate menu signal changes, so which one
    // actually marks this game's in-game menu can be read off one run instead
    // of guessed at a third time.
    void LogMenuSignals() const;
    bool EnsureMenuScreen(ID3D11Texture2D* back);
    void DestroyMenuScreen();

    // The settings panel, pasted into the BACKBUFFER as well as the headset, so
    // it can be read and driven on the monitor without wearing anything. The
    // pixels already exist every frame - this is one blit, after the eye images
    // have been taken, so the headset is unaffected.
    ID3D11Texture2D* m_monitorPanel = nullptr;
    uint32_t m_monitorW = 0;
    uint32_t m_monitorH = 0;
    DXGI_FORMAT m_monitorFormat = DXGI_FORMAT_UNKNOWN;
    // Its OWN device and context, taken from the backbuffer rather than from
    // the OpenXR session. The whole point of drawing the panel on the monitor is
    // to use it with the headset OFF - and with the headset off there is no
    // session, so anything that lives on the session is exactly the wrong place
    // for this.
    ID3D11Device* m_monDevice = nullptr;
    ID3D11DeviceContext* m_monContext = nullptr;
    bool EnsureMonitorPanel(ID3D11Texture2D* back);
    void PaintPanelOnMonitor(ID3D11Texture2D* back);
    void DestroyMonitorPanel();

public:
    // Called from the Present hook every frame, session or no session.
    void PaintPanelOnMonitorFromSwapchain(IDXGISwapChain* swapChain);

private:

    uint64_t m_frames = 0;
    uint64_t m_submitted = 0;

    // Running totals for the perf report, reset every perf_log_every frames.
    double m_perfWaitMs = 0.0;
    double m_perfWorkMs = 0.0;
    double m_perfEndMs = 0.0;
    double m_perfFrameMs = 0.0;
    double m_perfPrevFrameMs = 0.0;
    uint64_t m_perfSamples = 0;
    const char* m_lastPathName = "starting";

    XrPosef m_headPose{};
    volatile long m_headValid = 0;
};

VRSystem& VR();

// Is the mod reconstructing the eye larger than the game drew it, RIGHT NOW?
//
// The answer comes from the sizes in force this frame, never from the quality
// setting - which describes the next launch, and reading it mid-game switched
// the in-resolve DLAA pass on and off under a headset-side upscale that had not
// changed, blurring the picture on the way in and shimmering on the way back.
bool VRUpscalingNow();

// Exposed so the Present hook can report the backbuffer even when OpenXR is
// switched off - HDR is the most common cause of "black headset, perfect
// monitor", and it is worth knowing before anyone puts a headset on.
const char* DxgiFormatName(DXGI_FORMAT f);
bool DxgiFormatIsHdr(DXGI_FORMAT f);

}  // namespace cotwvr
