#include "vr.h"

#include "camera_probe.h"
#include "cbscan.h"
#include "config.h"
#include "dlss.h"
#include "frame_hook.h"
#include "gamesettings.h"
#include "log.h"
#include "overlay.h"
#include "post.h"
#include "render_hook.h"
#include "stereo.h"
#include "taa.h"

#include <algorithm>
#include <cstring>

namespace cotwvr {
namespace {

// The loader ships a name for every result code.  Using it means a failure
// names itself in the log instead of printing a bare number that then has to be
// looked up by hand - which is exactly what happened with -4
// (XR_ERROR_API_VERSION_UNSUPPORTED) on the first headset run.
const char* XrStr(XrResult r) {
    switch (r) {
        case XR_SUCCESS: return "XR_SUCCESS";
        case XR_TIMEOUT_EXPIRED: return "XR_TIMEOUT_EXPIRED";
        case XR_SESSION_LOSS_PENDING: return "XR_SESSION_LOSS_PENDING";
        case XR_EVENT_UNAVAILABLE: return "XR_EVENT_UNAVAILABLE";
        case XR_FRAME_DISCARDED: return "XR_FRAME_DISCARDED";
        case XR_ERROR_VALIDATION_FAILURE: return "XR_ERROR_VALIDATION_FAILURE";
        case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
        case XR_ERROR_OUT_OF_MEMORY: return "XR_ERROR_OUT_OF_MEMORY";
        case XR_ERROR_API_VERSION_UNSUPPORTED:
            return "XR_ERROR_API_VERSION_UNSUPPORTED (we asked for a newer OpenXR "
                   "than this runtime implements)";
        case XR_ERROR_INITIALIZATION_FAILED: return "XR_ERROR_INITIALIZATION_FAILED";
        case XR_ERROR_FUNCTION_UNSUPPORTED: return "XR_ERROR_FUNCTION_UNSUPPORTED";
        case XR_ERROR_FEATURE_UNSUPPORTED: return "XR_ERROR_FEATURE_UNSUPPORTED";
        case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
        case XR_ERROR_LIMIT_REACHED: return "XR_ERROR_LIMIT_REACHED";
        case XR_ERROR_SIZE_INSUFFICIENT: return "XR_ERROR_SIZE_INSUFFICIENT";
        case XR_ERROR_HANDLE_INVALID: return "XR_ERROR_HANDLE_INVALID";
        case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
        case XR_ERROR_SESSION_RUNNING: return "XR_ERROR_SESSION_RUNNING";
        case XR_ERROR_SESSION_NOT_RUNNING: return "XR_ERROR_SESSION_NOT_RUNNING";
        case XR_ERROR_SESSION_LOST: return "XR_ERROR_SESSION_LOST";
        case XR_ERROR_SYSTEM_INVALID: return "XR_ERROR_SYSTEM_INVALID";
        case XR_ERROR_PATH_INVALID: return "XR_ERROR_PATH_INVALID";
        case XR_ERROR_LAYER_INVALID: return "XR_ERROR_LAYER_INVALID";
        case XR_ERROR_LAYER_LIMIT_EXCEEDED: return "XR_ERROR_LAYER_LIMIT_EXCEEDED";
        case XR_ERROR_SWAPCHAIN_RECT_INVALID: return "XR_ERROR_SWAPCHAIN_RECT_INVALID";
        case XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED: return "XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED";
        case XR_ERROR_CALL_ORDER_INVALID: return "XR_ERROR_CALL_ORDER_INVALID";
        case XR_ERROR_GRAPHICS_DEVICE_INVALID: return "XR_ERROR_GRAPHICS_DEVICE_INVALID";
        case XR_ERROR_POSE_INVALID: return "XR_ERROR_POSE_INVALID";
        case XR_ERROR_INDEX_OUT_OF_RANGE: return "XR_ERROR_INDEX_OUT_OF_RANGE";
        case XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED: return "XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED";
        case XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED: return "XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED";
        case XR_ERROR_NAME_DUPLICATED: return "XR_ERROR_NAME_DUPLICATED";
        case XR_ERROR_NAME_INVALID: return "XR_ERROR_NAME_INVALID";
        case XR_ERROR_ACTIONSET_NOT_ATTACHED: return "XR_ERROR_ACTIONSET_NOT_ATTACHED";
        case XR_ERROR_LOCALIZED_NAME_DUPLICATED: return "XR_ERROR_LOCALIZED_NAME_DUPLICATED";
        case XR_ERROR_LOCALIZED_NAME_INVALID: return "XR_ERROR_LOCALIZED_NAME_INVALID";
        case XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING: return "XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING";
        case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE (no active OpenXR runtime)";
        case XR_ERROR_FORM_FACTOR_UNAVAILABLE:
            return "XR_ERROR_FORM_FACTOR_UNAVAILABLE (headset off / streamer not connected)";
        case XR_ERROR_FORM_FACTOR_UNSUPPORTED: return "XR_ERROR_FORM_FACTOR_UNSUPPORTED";
        case XR_ERROR_API_LAYER_NOT_PRESENT: return "XR_ERROR_API_LAYER_NOT_PRESENT";
        case XR_ERROR_FILE_ACCESS_ERROR:
            return "XR_ERROR_FILE_ACCESS_ERROR (broken third-party API layer, or "
                   "streamer not connected)";
        default: return "XR_ERROR_<unmapped - look it up in openxr.h>";
    }
}

#define XR_CHECK(expr, what)                                                    \
    do {                                                                        \
        const XrResult _r = (expr);                                             \
        if (XR_FAILED(_r)) {                                                    \
            COTW_LOG("[vr] %s failed: %d %s", (what), (int)_r, XrStr(_r));       \
            return false;                                                       \
        }                                                                       \
        COTW_LOG("[vr] %s ok", (what));                                         \
    } while (0)

// CopyResource needs identical dimensions and formats from the same typeless
// family.  Mismatches here fail silently or wash the colour out, so the eye
// swapchain format is chosen to match the backbuffer rather than assumed.
DXGI_FORMAT Family(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_R8G8B8A8_UINT:
        case DXGI_FORMAT_R8G8B8A8_SNORM:
        case DXGI_FORMAT_R8G8B8A8_SINT:
            return DXGI_FORMAT_R8G8B8A8_TYPELESS;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            return DXGI_FORMAT_B8G8R8A8_TYPELESS;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UINT:
            return DXGI_FORMAT_R10G10B10A2_TYPELESS;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_UNORM:
            return DXGI_FORMAT_R16G16B16A16_TYPELESS;
        default:
            return f;
    }
}

PFN_xrGetD3D11GraphicsRequirementsKHR g_getGfxReq = nullptr;

// A symmetric FOV shared by both eyes.
//
// The runtime hands out an ASYMMETRIC frustum per eye, mirrored left/right.
// That is correct when each eye's image was rendered through that eye's own
// frustum - but the game renders ONE image through ONE symmetric frustum, so
// showing it through two mirrored asymmetric ones puts it at a different
// angular position in each eye, and the two never fuse.
//
// Rule (from RealVR, and better than hand-tuning): the symmetric half-angle is
// the mean of the runtime's asymmetric half-ANGLES - computed in angles, not
// tangents - which preserves the angular total and centres it.
XrFovf SymmetricFov(const XrFovf& a, const XrFovf& b) {
    const float h = (fabsf(a.angleLeft) + fabsf(a.angleRight) +
                     fabsf(b.angleLeft) + fabsf(b.angleRight)) * 0.25f;
    const float v = (fabsf(a.angleUp) + fabsf(a.angleDown) +
                     fabsf(b.angleUp) + fabsf(b.angleDown)) * 0.25f;
    XrFovf f{};
    f.angleLeft = -h;
    f.angleRight = h;
    f.angleUp = v;
    f.angleDown = -v;
    return f;
}

}  // namespace

bool DxgiFormatIsHdr(DXGI_FORMAT f) {
    const DXGI_FORMAT fam = Family(f);
    return fam == DXGI_FORMAT_R10G10B10A2_TYPELESS ||
           fam == DXGI_FORMAT_R16G16B16A16_TYPELESS;
}

const char* DxgiFormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
        case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM (HDR)";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT (HDR)";
        default: return "<other>";
    }
}

namespace {
// Local aliases so the rest of this file reads as before.
inline // The _UNORM_SRGB counterpart of a plain _UNORM format, or UNKNOWN if there
// isn't one.
DXGI_FORMAT SrgbSibling(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        default:
            return DXGI_FORMAT_UNKNOWN;
    }
}

bool IsHdrFormat(DXGI_FORMAT f) { return DxgiFormatIsHdr(f); }
inline const char* FormatName(DXGI_FORMAT f) { return DxgiFormatName(f); }
}  // namespace

VRSystem& VR() {
    static VRSystem vr;
    return vr;
}

bool VRUpscalingNow() {
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    VR().CurrentSizes(&rw, &rh, &ow, &oh);
    return rw && ow > rw;
}

const char* VRSystem::StateName() const {
    switch (m_state) {
        case XR_SESSION_STATE_IDLE: return "IDLE";
        case XR_SESSION_STATE_READY: return "READY";
        case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
        case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
        case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
        case XR_SESSION_STATE_STOPPING: return "STOPPING";
        case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
        case XR_SESSION_STATE_EXITING: return "EXITING";
        default: return "UNKNOWN";
    }
}

// ---------------------------------------------------------------------------
// instance
// ---------------------------------------------------------------------------

bool VRSystem::CreateInstance() {
    // What is actually available tells us more than a failure code later.
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> exts(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    if (extCount) {
        xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, exts.data());
    }
    bool haveD3D11 = false;
    for (const auto& e : exts) {
        if (!strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) haveD3D11 = true;
    }
    COTW_LOG("[vr] runtime advertises %u extensions, XR_KHR_D3D11_enable = %s",
             extCount, haveD3D11 ? "YES" : "NO");
    if (!haveD3D11) {
        COTW_LOG("[vr] the active OpenXR runtime cannot do D3D11 - nothing to do here");
        return false;
    }

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};

    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = enabled;
    strcpy_s(ci.applicationInfo.applicationName, "theHunter CotW VR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "Apex");
    ci.applicationInfo.engineVersion = 1;
    // Ask for 1.0, NOT XR_CURRENT_API_VERSION.  The bundled headers are 1.1.61,
    // and a runtime that implements only OpenXR 1.0 - which VDXR does - rejects
    // the whole instance with XR_ERROR_API_VERSION_UNSUPPORTED (-4).  Everything
    // this mod uses (D3D11 binding, reference spaces, projection layers) is core
    // 1.0, so there is nothing to gain by asking for more.
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;

    XR_CHECK(xrCreateInstance(&ci, &m_instance), "xrCreateInstance");

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &props))) {
        COTW_LOG("[vr] runtime: %s (version %llu)", props.runtimeName,
                 (unsigned long long)props.runtimeVersion);
    }

    XrSystemGetInfo sysInfo{XR_TYPE_SYSTEM_GET_INFO};
    sysInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XR_CHECK(xrGetSystem(m_instance, &sysInfo, &m_systemId), "xrGetSystem");

    XrSystemProperties sysProps{XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_systemId, &sysProps))) {
        COTW_LOG("[vr] system: %s (max swapchain %ux%u)", sysProps.systemName,
                 sysProps.graphicsProperties.maxSwapchainImageWidth,
                 sysProps.graphicsProperties.maxSwapchainImageHeight);
    }

    XR_CHECK(xrGetInstanceProcAddr(m_instance, "xrGetD3D11GraphicsRequirementsKHR",
                                   reinterpret_cast<PFN_xrVoidFunction*>(&g_getGfxReq)),
             "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)");
    return true;
}

// ---------------------------------------------------------------------------
// session
// ---------------------------------------------------------------------------

bool VRSystem::CreateSession(ID3D11Device* device) {
    // REQUIRED before xrCreateSession, and the adapter it names must be the one
    // the game is rendering on.
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XR_CHECK(g_getGfxReq(m_instance, m_systemId, &req),
             "xrGetD3D11GraphicsRequirementsKHR");

    // Compare the runtime's required adapter against the game's actual adapter.
    IDXGIDevice* dxgiDevice = nullptr;
    IDXGIAdapter* adapter = nullptr;
    if (SUCCEEDED(device->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice)) &&
        SUCCEEDED(dxgiDevice->GetAdapter(&adapter))) {
        DXGI_ADAPTER_DESC desc{};
        adapter->GetDesc(&desc);
        const bool same = desc.AdapterLuid.LowPart == req.adapterLuid.LowPart &&
                          desc.AdapterLuid.HighPart == req.adapterLuid.HighPart;
        char name[128]{};
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
        COTW_LOG("[vr] game adapter '%s' LUID %08X:%08X / runtime wants %08X:%08X -> %s",
                 name, (unsigned)desc.AdapterLuid.HighPart, (unsigned)desc.AdapterLuid.LowPart,
                 (unsigned)req.adapterLuid.HighPart, (unsigned)req.adapterLuid.LowPart,
                 same ? "MATCH" : "*** MISMATCH ***");
        if (!same) {
            COTW_LOG("[vr] the headset is on a different GPU than the game is rendering on.");
            COTW_LOG("[vr] force the game onto the headset's GPU in the Windows graphics settings.");
        }
    }
    if (adapter) adapter->Release();
    if (dxgiDevice) dxgiDevice->Release();

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;

    XrSessionCreateInfo ci{XR_TYPE_SESSION_CREATE_INFO};
    ci.next = &binding;
    ci.systemId = m_systemId;
    XR_CHECK(xrCreateSession(m_instance, &ci, &m_session), "xrCreateSession");

    XrReferenceSpaceCreateInfo space{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;   // seated
    space.poseInReferenceSpace.orientation.w = 1.0f;
    XR_CHECK(xrCreateReferenceSpace(m_session, &space, &m_space),
             "xrCreateReferenceSpace(LOCAL)");
    return true;
}

// ---------------------------------------------------------------------------
// swapchains
// ---------------------------------------------------------------------------

bool VRSystem::PickSwapchainFormat(DXGI_FORMAT backbufferFormat, int64_t* chosen) const {
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(m_session, 0, &count, nullptr)) || !count) {
        COTW_LOG("[vr] xrEnumerateSwapchainFormats returned nothing");
        return false;
    }
    std::vector<int64_t> formats(count);
    if (XR_FAILED(xrEnumerateSwapchainFormats(m_session, count, &count, formats.data()))) {
        return false;
    }

    char buf[512];
    int n = snprintf(buf, sizeof(buf), "[vr] runtime swapchain formats:");
    for (uint32_t i = 0; i < count && n < (int)sizeof(buf) - 16; ++i) {
        n += snprintf(buf + n, sizeof(buf) - n, " %d", (int)formats[i]);
    }
    LogRaw(buf);

    // Prefer the sRGB sibling of the backbuffer format, NOT an exact match.
    //
    // The game's backbuffer holds sRGB-ENCODED pixels in a plain _UNORM
    // texture. Handing the compositor a _UNORM swapchain tells it "this data is
    // linear", so it applies the sRGB transfer curve a second time on the way
    // to the display and everything comes out far too bright and washed out.
    // Declaring the swapchain _UNORM_SRGB tells it the truth; the copy itself
    // is unchanged, because the two formats share a typeless family and
    // CopyResource moves raw bits either way.
    //
    // The runtime listing format 29 (R8G8B8A8_UNORM_SRGB) FIRST is a hint in
    // the same direction - runtimes advertise their preferred format first.
    const DXGI_FORMAT srgbOf = SrgbSibling(backbufferFormat);
    if (srgbOf != DXGI_FORMAT_UNKNOWN) {
        for (int64_t f : formats) {
            if ((DXGI_FORMAT)f == srgbOf) {
                *chosen = f;
                COTW_LOG("[vr] swapchain format %d (%s) - the sRGB sibling of the "
                         "backbuffer (%s). Using _UNORM here instead makes the "
                         "compositor gamma-correct twice, which reads as the whole "
                         "game being too bright.",
                         (int)f, FormatName(srgbOf), FormatName(backbufferFormat));
                return true;
            }
        }
    }

    // Exact match next - no conversion at all.
    for (int64_t f : formats) {
        if ((DXGI_FORMAT)f == backbufferFormat) {
            *chosen = f;
            COTW_LOG("[vr] swapchain format %d (%s) - exact match with the backbuffer "
                     "(no sRGB sibling offered; brightness may be too high)",
                     (int)f, FormatName(backbufferFormat));
            return true;
        }
    }
    // Then anything CopyResource will accept.
    for (int64_t f : formats) {
        if (Family((DXGI_FORMAT)f) == Family(backbufferFormat)) {
            *chosen = f;
            COTW_LOG("[vr] swapchain format %d (%s) - same typeless family as the "
                     "backbuffer (%s), CopyResource is legal",
                     (int)f, FormatName((DXGI_FORMAT)f), FormatName(backbufferFormat));
            return true;
        }
    }

    COTW_LOG("[vr] *** no runtime swapchain format is CopyResource-compatible with "
             "the backbuffer (%s) ***", FormatName(backbufferFormat));
    return false;
}

// Choose the centred region of the backbuffer whose shape matches the headset's
// eye frustum, so the image can be shown at the headset's own FOV without being
// stretched. Cropping only ever removes field of view, never adds it - so this
// fixes the SHAPE; how much world is visible is still the game's FOV setting.
void VRSystem::ComputeCrop(uint32_t srcW, uint32_t srcH, const XrFovf& headsetFov) {
    m_srcWidth = srcW;
    m_srcHeight = srcH;

    const float targetAspect =
        tanf(fabsf(headsetFov.angleRight)) / tanf(fabsf(headsetFov.angleUp));
    const float srcAspect = float(srcW) / float(srcH);

    uint32_t w = srcW, h = srcH;
    if (srcAspect > targetAspect) {
        w = uint32_t(float(srcH) * targetAspect + 0.5f);   // too wide: trim sides
        if (w > srcW) w = srcW;
    } else if (srcAspect < targetAspect) {
        h = uint32_t(float(srcW) / targetAspect + 0.5f);   // too tall: trim top/bottom
        if (h > srcH) h = srcH;
    }
    w &= ~1u;
    h &= ~1u;

    // TIER 1 keeps the swapchains at the FULL cropped size and submits a
    // SUB-RECTANGLE of them instead. Sizing them here was the Stage 2 defect:
    // this runs ~6 s in, on the menu camera, where the render FOV is nothing
    // like the gameplay one (measured 64.97 deg against 112.64 in play), so the
    // rectangle came out full-size and both eyes got identical pixels.

    m_cropX = (srcW - w) / 2;
    m_cropY = (srcH - h) / 2;
    m_width = w;
    m_height = h;

    COTW_LOG("[vr] backbuffer %ux%u (aspect %.3f) -> using the centre %ux%u at "
             "(%u,%u), aspect %.3f, to match the headset's frustum shape %.3f",
             srcW, srcH, srcAspect, w, h, m_cropX, m_cropY,
             float(w) / float(h), targetAspect);
}

// What the runtime asks for per eye, remembered from swapchain creation. The
// ceiling below is expressed against it because it is the only size on this
// machine that means anything physically - it is roughly one rendered pixel per
// display pixel after the compositor's lens warp.
static uint32_t g_recW = 0, g_recH = 0;

// *** THE SIZE THE HEADSET IS HANDED IS THE SIZE THE PLAYER CHOSE. ***
//
// Not "the render times a percentage" - THE PRESET. The player picks a render
// resolution on the VIEW tab; that is what the headset gets, always, and the
// quality level decides only how much of it the game actually draws. Anything
// else makes the picture change size when the quality changes, which is not
// what a DLSS setting does in any game that ships one.
//
// It also makes the whole thing live in the only way the engine allows. The
// game's own video menu can change the resolution while running, and when it
// does the output stays put and DLSS simply reconstructs across a bigger gap -
// so the player can compare quality levels for real, this second, without a
// restart. (The mod's own quality row cannot do that: the game reads its
// resolution once, at startup.)
//
// *** AND IT DOES NOT MOVE WHEN THE QUALITY LEVEL MOVES. ***
//
// The quality level is a statement about what the GAME will draw at the next
// launch. It says nothing about how big the picture handed to the headset
// should be, so this function does not read it - only the launch target and the
// frame the game is actually drawing. An earlier build let DLAA (100%) fall
// through to "no upscaling", which meant switching to DLAA mid-game shrank the
// output to the render size and visibly changed the picture; the owner caught
// it in one sitting. The percentage is used ONLY as a fallback, when there is
// no launch target to work from at all.
//
// *** AND WHY IT IS CAPPED AT 1.2x WHAT THE RUNTIME ASKS FOR. ***
//
// Everything above the runtime's recommended size is resampled back down by the
// compositor before it reaches the panel, and DLSS time and video memory scale
// with OUTPUT pixels. Reconstructing to 2x recommended would cost quadratically
// for pixels nobody ever sees. 1.2 per axis (1.44x the pixels) leaves the usual
// supersampling headroom and stops there.
void VRSystem::UpscaleTarget(uint32_t inW, uint32_t inH,
                             uint32_t* outW, uint32_t* outH) const {
    *outW = inW;
    *outH = inH;
    if (!Cfg().dlss_enable || !inW || !inH) return;

    uint64_t w = 0, h = 0;
    // *** THE PRESET THIS LAUNCH WAS BUILT AROUND, NOT THE PANEL ROW. ***
    //
    // Reading the live row meant that scrolling the resolution row - a setting
    // that plainly states it takes effect at the next launch - rebuilt both eye
    // swapchains on every nudge and changed the picture there and then. It is
    // stamped once, where the resolution is written.
    //
    // The preset is a FULL-FRAME size and inW/inH are a CROP of the frame, so
    // it is applied as a ratio against the frame we are actually being handed -
    // never as an absolute, which would silently undo the crop. That ratio is
    // also what keeps this live in the one way that IS wanted: when the game's
    // own video menu changes the resolution, m_srcWidth moves and the output
    // stays where the player put it.
    int targetW = 0, targetH = 0;
    LaunchRenderTarget(&targetW, &targetH);
    if (targetW > 0 && targetH > 0 && m_srcWidth && m_srcHeight) {
        w = (uint64_t)inW * (uint64_t)targetW / (uint64_t)m_srcWidth;
        h = (uint64_t)inH * (uint64_t)targetH / (uint64_t)m_srcHeight;
    } else {
        // No launch target - "leave the game alone" was selected, or the
        // settings write never happened. Fall back to the percentage, LATCHED
        // at startup: reading it live here would put the same mid-game surprise
        // back through the one door still open to it.
        const int pct = m_launchPct;
        if (pct < 33 || pct > 99) return;
        w = (uint64_t)inW * 100 / (uint64_t)pct;
        h = (uint64_t)inH * 100 / (uint64_t)pct;
    }
    // Even dimensions: half-pixel sizes are a class of bug nobody enjoys.
    w &= ~1ull;
    h &= ~1ull;
    if (g_recW && g_recH) {
        const uint64_t capW = (uint64_t)g_recW * 12 / 10;
        const uint64_t capH = (uint64_t)g_recH * 12 / 10;
        if (w > capW || h > capH) {
            // Cap on the tighter axis and keep the shape - an eye whose aspect
            // no longer matches its crop is a stretched image, not a smaller one.
            const double s = (w > capW || h > capH)
                                 ? (((double)capW / (double)w) < ((double)capH / (double)h)
                                        ? (double)capW / (double)w
                                        : (double)capH / (double)h)
                                 : 1.0;
            w = (uint64_t)((double)w * s) & ~1ull;
            h = (uint64_t)((double)h * s) & ~1ull;
        }
    }
    if (w < inW || h < inH) { w = inW; h = inH; }   // never DOWNscale here
    *outW = (uint32_t)w;
    *outH = (uint32_t)h;
}

bool VRSystem::CreateSwapchains(uint32_t width, uint32_t height) {
    // Per-eye recommended size is logged for reference, but the eye swapchains
    // are made at the game's backbuffer size so the frame moves with a straight
    // CopyResource - no scaling, no surprises.
    uint32_t viewCount = 0;
    xrEnumerateViewConfigurationViews(m_instance, m_systemId,
                                      XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                      0, &viewCount, nullptr);
    std::vector<XrViewConfigurationView> views(viewCount, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    if (viewCount) {
        xrEnumerateViewConfigurationViews(m_instance, m_systemId,
                                          XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                          viewCount, &viewCount, views.data());
        COTW_LOG("[vr] %u views, runtime recommends %ux%u per eye; using the game's "
                 "backbuffer size %ux%u instead",
                 viewCount, views[0].recommendedImageRectWidth,
                 views[0].recommendedImageRectHeight, width, height);
        // *** WHAT THE HEADSET ACTUALLY WANTS, IN THE THREE UNITS THAT MATTER. ***
        //
        // Absolute pixels are right for exactly one headset. A SHAPE and a
        // SHARPNESS are right for all of them, so print both: the aspect the
        // runtime asks for (near-square on every modern headset, because a real
        // per-eye frustum is near-square - which is why Luke Ross lands on ratios
        // like 0.974:1 rather than 16:9), and the pixels-per-degree our current
        // size delivers, which is the one number that means the same thing on
        // every device.
        const float recW = (float)views[0].recommendedImageRectWidth;
        const float recH = (float)views[0].recommendedImageRectHeight;
        g_recW = views[0].recommendedImageRectWidth;
        g_recH = views[0].recommendedImageRectHeight;
        if (recW > 0.0f && recH > 0.0f) {
            COTW_LOG("[vr] runtime per-eye shape %.4f:1 (w:h) - ours %.4f:1%s",
                     recW / recH, (float)width / (float)height,
                     ((float)width / (float)height > 1.15f)
                         ? "   <-- ours is LANDSCAPE where the headset wants "
                           "near-square: MELE1 measured this as squashed UI and a "
                           "blurrier image"
                         : "");
            COTW_LOG("[vr] scale-of-recommended: %.2f x %.2f (1.00 = exactly what "
                     "the runtime asks for)", (float)width / recW,
                     (float)height / recH);
        }
    }
    if (viewCount != 2) {
        COTW_LOG("[vr] expected 2 views for PRIMARY_STEREO, got %u", viewCount);
        return false;
    }

    // *** THE HEADSET MAY BE GIVEN MORE PIXELS THAN THE GAME RENDERS. ***
    //
    // With DLSS upscaling on, the game keeps rendering at its own (smaller)
    // resolution and each eye is reconstructed up to this size on its way to
    // the headset - so the swapchain, and the hold texture that feeds it, are
    // made at the OUTPUT size while m_width/m_height stay the crop size that
    // every other calculation here means by "the frame".
    m_launchPct = DlssRenderPct();      // latched before the first size is asked for
    UpscaleTarget(width, height, &m_outW, &m_outH);
    if (m_outW != width || m_outH != height) {
        const float eff = 100.0f * (float)width / (float)m_outW;
        COTW_LOG("[vr] DLSS upscaling: the game renders %ux%u per eye, the headset "
                 "is handed %ux%u - %.0f%% per axis, %.0f%% of the pixels "
                 "(asked for %d%%).",
                 width, height, m_outW, m_outH, eff, eff * eff / 100.0f,
                 DlssRenderPct());
        if (eff > (float)DlssRenderPct() + 1.0f) {
            COTW_LOG("[vr]   capped at 1.2x the runtime's recommended %ux%u - past that "
                     "the compositor resamples it away. To get the full ratio, lower "
                     "the GAME's resolution (render_preset): this rebuilds what the "
                     "game draws, it does not make the game draw less.", g_recW, g_recH);
        }
    } else if (DlssRenderPct() < 100) {
        COTW_LOG("[vr] DLSS upscaling asked for %d%% but the output is already at or "
                 "past 1.2x the runtime's recommended %ux%u, so there is nothing to "
                 "reconstruct into. Lower the game's resolution (render_preset) first, "
                 "then this rebuilds the size back up.",
                 DlssRenderPct(), g_recW, g_recH);
    }

    m_width = width;
    m_height = height;

    if (!BuildEyeSwapchains()) return false;

    // Only needed by the routes that actually read them: full-rate stashes both
    // eyes into them, and the slow alternate-eye path holds each eye's last
    // image there. The fast path re-uses the swapchain itself and would leave
    // 80 MB of video memory allocated for nothing.
    //
    // Upscaling always needs them: the reconstruction is written through a UAV,
    // and OpenXR swapchain images have no UAV bind - so there has to be a
    // texture of our own to write into whatever the fast path would prefer.
    const bool needHold = Cfg().stereo &&
                          (Cfg().full_rate_stereo || !Cfg().aer_reuse_swapchain ||
                           !m_eyesPrimed || m_outW != m_width);
    if (needHold && !CreateHoldTextures()) {
        COTW_LOG("[vr] stereo hold textures failed - falling back to mono");
    }
    return true;
}

// *** THE EYE SWAPCHAINS, MADE FROM m_outW/m_outH - AND REMAKEABLE. ***
//
// Split out of CreateSwapchains so the output size can change while the game is
// running: the quality level is a live setting, and the game's own video menu
// can change the render size under us at any moment. Destroys whatever is there
// first, so it is safe to call repeatedly. Must be called BETWEEN frames -
// never with an image acquired.
bool VRSystem::BuildEyeSwapchains() {
    for (int eye = 0; eye < 2; ++eye) {
        if (m_eyes[eye].handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(m_eyes[eye].handle);
            m_eyes[eye].handle = XR_NULL_HANDLE;
        }
        m_eyes[eye].images.clear();

        XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                        XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = m_swapchainFormat;
        ci.sampleCount = 1;
        ci.width = m_outW;
        ci.height = m_outH;
        ci.faceCount = 1;
        ci.arraySize = 1;
        ci.mipCount = 1;

        XrResult r = xrCreateSwapchain(m_session, &ci, &m_eyes[eye].handle);
        if (XR_FAILED(r)) {
            COTW_LOG("[vr] xrCreateSwapchain(eye %d) failed: %d %s", eye, (int)r, XrStr(r));
            return false;
        }

        uint32_t imageCount = 0;
        xrEnumerateSwapchainImages(m_eyes[eye].handle, 0, &imageCount, nullptr);
        std::vector<XrSwapchainImageD3D11KHR> images(
            imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        r = xrEnumerateSwapchainImages(
            m_eyes[eye].handle, imageCount, &imageCount,
            reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(r)) {
            COTW_LOG("[vr] xrEnumerateSwapchainImages(eye %d) failed: %d %s", eye, (int)r, XrStr(r));
            return false;
        }
        for (uint32_t i = 0; i < imageCount; ++i) m_eyes[eye].images.push_back(images[i].texture);
        // The size the SWAPCHAIN was made at, which under upscaling is not the
        // size the game renders. Printing the parameter instead of the field
        // made the first upscaling run read as "no upscaling happened".
        COTW_LOG("[vr] eye %d swapchain %ux%u, %u images", eye, m_outW, m_outH, imageCount);
    }

    // Give every image in both swapchains a defined value once, up front. The
    // alternate-eye fast path leaves the eye that was not re-rendered untouched
    // and lets the runtime re-composite its last released image - which only
    // works if something has ever been released, and only looks right if it is
    // black rather than uninitialised video memory.
    m_eyesPrimed = true;
    for (int eye = 0; eye < 2 && m_eyesPrimed; ++eye) {
        for (size_t pass = 0; pass < m_eyes[eye].images.size(); ++pass) {
            uint32_t index = 0;
            XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
            if (XR_FAILED(xrAcquireSwapchainImage(m_eyes[eye].handle, &acq, &index))) {
                m_eyesPrimed = false;
                break;
            }
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(xrWaitSwapchainImage(m_eyes[eye].handle, &wait))) {
                ID3D11RenderTargetView* rtv = nullptr;
                if (SUCCEEDED(m_device->CreateRenderTargetView(m_eyes[eye].images[index],
                                                               nullptr, &rtv))) {
                    const float black[4] = {0, 0, 0, 1};
                    m_context->ClearRenderTargetView(rtv, black);
                    rtv->Release();
                }
            }
            XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(m_eyes[eye].handle, &rel);
        }
    }
    COTW_LOG("[vr] eye swapchains primed: %s", m_eyesPrimed ? "yes" : "NO - fast path off");
    m_holdW[0] = m_holdH[0] = m_holdW[1] = m_holdH[1] = 0;
    return true;
}

// *** THE QUALITY LEVEL IS LIVE, AND THIS IS WHAT MAKES IT LIVE. ***
//
// Called once per frame from the submit, before anything is acquired. Two
// things can change the size the headset should be handed while the game is
// running - the player picking a different quality level in the panel, and the
// GAME's own video menu changing the resolution under us - and both land here
// as "the output should be X but the swapchains are Y". Rebuilding them costs a
// visible hitch, so it only happens when the numbers actually differ.
void VRSystem::EnsureOutputSize() {
    if (!m_width || !m_height || m_session == XR_NULL_HANDLE) return;
    uint32_t wantW = m_width, wantH = m_height;
    UpscaleTarget(m_width, m_height, &wantW, &wantH);
    if (wantW == m_outW && wantH == m_outH) return;

    COTW_LOG("[vr] output size changing live: %ux%u -> %ux%u (game renders %ux%u). "
             "Rebuilding both eye swapchains.", m_outW, m_outH, wantW, wantH,
             m_width, m_height);
    m_outW = wantW;
    m_outH = wantH;
    if (!BuildEyeSwapchains()) {
        COTW_LOG("[vr] *** rebuilding the eye swapchains FAILED - the headset will "
                 "lose its picture. Restart the game. ***");
        m_failed = true;
        return;
    }
    DestroyHoldTextures();
    if (!CreateHoldTextures()) {
        COTW_LOG("[vr] hold textures failed after the resize");
    }
    // The upscaler was writing into the textures that were just destroyed.
    // Nothing it remembers can be trusted across that, so it starts again.
    DlssUpscaleInvalidate();
}

bool VRSystem::CreateOverlayResources() {
    // Head-locked: the panel should stay where the player can read it rather
    // than being left behind in the world when they turn to look at something.
    XrReferenceSpaceCreateInfo si{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    si.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    si.poseInReferenceSpace.orientation.w = 1.0f;
    XrResult r = xrCreateReferenceSpace(m_session, &si, &m_viewSpace);
    if (XR_FAILED(r)) {
        COTW_LOG("[overlay] xrCreateReferenceSpace(VIEW) failed: %s", XrStr(r));
        return false;
    }

    // The panel has alpha, so it needs a format with one; the eye swapchains'
    // format is chosen to match the backbuffer and may not have usable alpha.
    uint32_t count = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &count, nullptr);
    std::vector<int64_t> formats(count);
    if (count) xrEnumerateSwapchainFormats(m_session, count, &count, formats.data());

    const DXGI_FORMAT wanted[] = {DXGI_FORMAT_B8G8R8A8_UNORM,
                                  DXGI_FORMAT_R8G8B8A8_UNORM,
                                  DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
                                  DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    for (DXGI_FORMAT w : wanted) {
        for (int64_t f : formats) {
            if ((DXGI_FORMAT)f == w) { m_overlayFormat = f; break; }
        }
        if (m_overlayFormat) break;
    }
    if (!m_overlayFormat) {
        COTW_LOG("[overlay] no alpha-capable swapchain format offered");
        return false;
    }

    const int W = g_overlay.Width(), H = g_overlay.Height();

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = m_overlayFormat;
    ci.sampleCount = 1;
    ci.width = W;
    ci.height = H;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    r = xrCreateSwapchain(m_session, &ci, &m_overlaySwapchain);
    if (XR_FAILED(r)) {
        COTW_LOG("[overlay] xrCreateSwapchain failed: %s", XrStr(r));
        return false;
    }

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(m_overlaySwapchain, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(imgCount, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(m_overlaySwapchain, imgCount, &imgCount,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    m_overlayImages.clear();
    for (uint32_t i = 0; i < imgCount; ++i) m_overlayImages.push_back(imgs[i].texture);

    // A CPU-writable staging texture the panel bitmap is uploaded into, then
    // copied to whichever swapchain image the runtime hands us.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = W;
    td.Height = H;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = (DXGI_FORMAT)m_overlayFormat;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_device->CreateTexture2D(&td, nullptr, &m_overlayStaging))) {
        COTW_LOG("[overlay] could not create the staging texture");
        return false;
    }

    COTW_LOG("[overlay] quad layer ready (%dx%d, format %d, %u images)",
             W, H, (int)m_overlayFormat, imgCount);
    return true;
}

// *** THE FLAT SCREEN - what makes the game's own menus readable. ***
//
// The menus are drawn flat, at screen scale, then stretched across the headset's
// whole field of view. The edges - and so most of the options - end up outside
// where the eye can comfortably look, which is why the settings screen could not
// be read at all.
//
// This shows the game's image on a floating panel instead. Not one of the game's
// draw calls is touched: the backbuffer is copied verbatim and it is the quad's
// SIZE IN METRES that decides how big it looks. No scaling, no blit shader, and
// no need to work out which draws are UI - which is unreachable anyway, since
// the frame census showed only ~0.3% of the frame reaches this DLL's D3D hooks.
//
// The swapchain is the backbuffer's exact size in the format already chosen to
// be CopyResource-compatible with it, so the per-frame cost is one bit copy.

// WHY THE CURSOR, AND NOT THE PLAYER CAMERA.
//
// The first attempt keyed off PlayerCameraActive(), on the theory that a menu
// means no player camera. The owner then pointed out the thing that kills it:
// "while playing and press menu button, the menu opens but the game stays live
// in the background". The camera keeps ticking and the framerate does not budge,
// so that detector CANNOT fire for the in-game menu. It is still right for the
// main menu, where there really is no player camera.
//
// A visible mouse cursor is the honest signal for the in-game one: a menu you
// click through has to show a cursor, and a first-person game in play does not.
// It costs one GetCursorInfo call and needs no engine knowledge at all.
//
// The foreground check keeps the panel from appearing because the player
// alt-tabbed, where the cursor is visible for a reason that is none of ours.
bool VRSystem::MenuCursorVisible() const {
    // By process rather than by window handle: VRSystem never had the game's
    // HWND, and asking "is the foreground window one of ours" answers the real
    // question anyway.
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return false;

    // *** THE CURSOR ALONE IS NOT ENOUGH, and the owner found the case that
    // proves it: ESC opens the menu and a cursor appears, START on the pad opens
    // the same menu and no cursor ever does. A pad-navigated menu has no use for
    // one. Keying off CURSOR_SHOWING therefore catches only half the ways in.
    //
    // The cursor CLIP is the better signal, and it is about the game's input
    // mode rather than about the mouse. While you are playing, the game pins the
    // cursor inside its own window so looking around cannot walk it onto another
    // monitor - here 3088x2108 against a 5760x2160 desktop. Any menu has to let
    // go of it. That release does not depend on a cursor being drawn, so it
    // should hold however the menu was opened.
    //
    // Either signal is taken as a menu. A false positive costs a panel you can
    // dismiss with the hotkey; a false negative is a menu you cannot read.
    RECT clip{};
    if (GetClipCursor(&clip)) {
        const int cw = int(clip.right - clip.left);
        const int ch = int(clip.bottom - clip.top);
        const int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        const int vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        // Unclipped means the rect covers the whole virtual desktop. A little
        // slack, because a runtime or an overlay can round it by a pixel.
        const bool unclipped = cw >= vw - 2 && ch >= vh - 2;
        if (unclipped && Cfg().menu_screen_use_clip) return true;
    }

    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci)) return false;
    return (ci.flags & CURSOR_SHOWING) != 0;
}

// The cursor test did NOT fire on this game's in-game menu, so rather than
// guess at a third signal, sample several and report the moment any of them
// changes. Open the menu once and the log says which one moved - the same
// measure-first approach that found the aim field in twenty minutes after
// eleven hook probes had failed.
//
// Candidates, all free to read:
//   showing  - CURSOR_SHOWING. What was tried, and apparently not it.
//   handle   - the cursor BITMAP. A game can keep the cursor "showing" the whole
//              time and simply swap in a blank one for gameplay, which would
//              explain the flag never moving.
//   clip     - games clip the cursor to the window while playing and usually
//              release it for a menu.
//   camera   - PlayerCameraActive, known to stay true here; logged as a control
//              so the trace is readable.
void VRSystem::LogMenuSignals() const {
    if (!Cfg().menu_screen_log_signals) return;

    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    const bool haveCi = GetCursorInfo(&ci) != 0;
    const int showing = haveCi ? ((ci.flags & CURSOR_SHOWING) ? 1 : 0) : -1;
    const uintptr_t handle = haveCi ? reinterpret_cast<uintptr_t>(ci.hCursor) : 0;

    RECT clip{};
    const bool haveClip = GetClipCursor(&clip) != 0;
    const int clipW = haveClip ? int(clip.right - clip.left) : -1;
    const int clipH = haveClip ? int(clip.bottom - clip.top) : -1;

    const int cam = PlayerCameraActive() ? 1 : 0;

    static int lastShowing = -2, lastCam = -2, lastClipW = -2, lastClipH = -2;
    static uintptr_t lastHandle = ~uintptr_t(0);
    if (showing == lastShowing && handle == lastHandle && clipW == lastClipW &&
        clipH == lastClipH && cam == lastCam) {
        return;                          // nothing moved; stay quiet
    }
    lastShowing = showing;
    lastHandle = handle;
    lastClipW = clipW;
    lastClipH = clipH;
    lastCam = cam;
    COTW_LOG("[signals] cursor showing=%d  bitmap=0x%IX  clip=%dx%d  playerCamera=%d",
             showing, handle, clipW, clipH, cam);
}

VRSystem::ScreenReason VRSystem::MenuScreenReason() const {
    if (Cfg().menu_screen) return ScreenReason::Manual;
    if (!Cfg().menu_screen_auto) return ScreenReason::None;
    if (!PlayerCameraActive()) return ScreenReason::MainMenu;

    // *** THE CURSOR MUST STAY UP, NOT MERELY APPEAR. ***
    //
    // Measured: 26 false positives in one session. Turning with the MOUSE makes
    // the game show or release the cursor for a moment, this read it as a menu
    // opening, and the flat cinema screen dropped in front of the player - which
    // from inside the headset is "the game became a flat window, on and off as I
    // look around". The pad never triggered it, because the pad never touches
    // the cursor.
    //
    // The signal was always the weak one. The MAIN menu is caught by "no player
    // camera", which is a fact about the game's state; the IN-GAME menu had only
    // this, and the note in config.h already recorded that the cursor test did
    // not fire reliably on this game's menu.
    //
    // A real menu keeps the cursor up for as long as it is open. A mouse-look
    // flicker does not. So require it to hold: sample it, and only believe it
    // once it has been continuously true for menu_cursor_hold_ms. Dropping it is
    // immediate - closing a menu should not leave the screen hanging.
    // *** OFF BY DEFAULT NOW. The owner's call, and the right one. ***
    //
    // Even held for 400 ms this signal is a guess, and the cost of it being
    // wrong is the whole picture turning into a flat window mid-hunt. The main
    // menu is caught above by 'no player camera', which is a FACT about the
    // game's state rather than an inference, and that half keeps working.
    // The in-game menu now needs the hotkey, which is never wrong.
    if (Cfg().menu_cursor_hold_ms <= 0) return ScreenReason::None;

    if (MenuCursorVisible()) {
        const DWORD now = GetTickCount();
        if (!m_cursorUpSince) m_cursorUpSince = now ? now : 1;
        const DWORD held = now - m_cursorUpSince;
        if (held >= (DWORD)Cfg().menu_cursor_hold_ms) return ScreenReason::InGameMenu;
    } else {
        m_cursorUpSince = 0;
    }
    return ScreenReason::None;
}

bool VRSystem::EnsureMenuScreen(ID3D11Texture2D* back) {
    if (!back || m_session == XR_NULL_HANDLE || !m_device) return false;

    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    if (m_screenSwapchain != XR_NULL_HANDLE &&
        m_screenW == bd.Width && m_screenH == bd.Height) {
        return true;                    // already sized for this backbuffer
    }
    DestroyMenuScreen();
    DestroyMonitorPanel();                // resolution changed - rebuild

    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    ci.format = m_swapchainFormat;      // already CopyResource-compatible
    ci.sampleCount = 1;
    ci.width = bd.Width;
    ci.height = bd.Height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(m_session, &ci, &m_screenSwapchain);
    if (XR_FAILED(r)) {
        COTW_LOG("[screen] xrCreateSwapchain(%ux%u) failed: %s - the menus stay "
                 "stretched", bd.Width, bd.Height, XrStr(r));
        m_screenSwapchain = XR_NULL_HANDLE;
        return false;
    }

    uint32_t imgCount = 0;
    xrEnumerateSwapchainImages(m_screenSwapchain, 0, &imgCount, nullptr);
    std::vector<XrSwapchainImageD3D11KHR> imgs(imgCount,
                                               {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(m_screenSwapchain, imgCount, &imgCount,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    m_screenImages.clear();
    for (uint32_t i = 0; i < imgCount; ++i) m_screenImages.push_back(imgs[i].texture);
    if (m_screenImages.empty()) { DestroyMenuScreen(); return false; }

    m_screenW = bd.Width;
    m_screenH = bd.Height;
    COTW_LOG("[screen] flat panel ready (%ux%u, format %d, %u images) - %.1f m wide, "
             "%.1f m away", m_screenW, m_screenH, (int)m_swapchainFormat, imgCount,
             Cfg().menu_screen_width_m, Cfg().menu_screen_distance_m);
    return true;
}


// *** THE SETTINGS PANEL ON THE MONITOR AS WELL AS IN THE HEADSET. ***
//
// Testing a setting should not require putting a headset on. The panel's pixels
// are already produced every frame for the quad layer, so showing them on the
// monitor costs one blit and no extra drawing.
//
// Placed AFTER the eye images have been taken and after the flat screen, so the
// headset never sees it twice - what goes to the monitor is a copy the headset
// has already finished with.
//
// A straight CopySubresourceRegion, not a shader. That means no alpha blending:
// the panel's transparent surround and soft shadow arrive as black, so it lands
// as a plain rectangle. On a monitor being used to read values that is fine, and
// it avoids dragging a vertex/pixel shader, a sampler and a blend state into the
// mod for a convenience feature.
//
// The staging texture is created in the BACKBUFFER's own format, because
// CopySubresourceRegion refuses to convert. The panel is premultiplied BGRA; if
// the backbuffer is RGBA the red and blue bytes are swapped on the way in.
bool VRSystem::EnsureMonitorPanel(ID3D11Texture2D* back) {
    if (!back) return false;
    // The device comes from the BACKBUFFER, so this works with no
    // OpenXR session at all - which is the case this feature exists for.
    if (!m_monDevice) {
        back->GetDevice(&m_monDevice);
        if (m_monDevice) m_monDevice->GetImmediateContext(&m_monContext);
    }
    if (!m_monDevice || !m_monContext) return false;
    const uint32_t w = uint32_t(g_overlay.Width());
    const uint32_t h = uint32_t(g_overlay.Height());
    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    if (m_monitorPanel && m_monitorW == w && m_monitorH == h &&
        m_monitorFormat == bd.Format) {
        return true;
    }
    DestroyMonitorPanel();

    D3D11_TEXTURE2D_DESC td{};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = bd.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(m_monDevice->CreateTexture2D(&td, nullptr, &m_monitorPanel))) {
        m_monitorPanel = nullptr;
        return false;
    }
    m_monitorW = w;
    m_monitorH = h;
    m_monitorFormat = bd.Format;
    COTW_LOG("[monitor] settings panel will also be shown on the monitor "
             "(%ux%u, format %d)", w, h, (int)bd.Format);
    return true;
}

void VRSystem::PaintPanelOnMonitor(ID3D11Texture2D* back) {
    if (!Cfg().panel_on_monitor || !g_overlay.Visible()) return;
    if (!back) return;

    // THE BACKBUFFER'S OWN DEVICE, CHECKED EVERY TIME.
    //
    // Caching one device and trusting it is how this crashed: Present is hooked
    // for whatever swapchains the game makes, and copying a texture created on
    // one device into a backbuffer belonging to another is not a mistake D3D11
    // survives. If the device is not the one this texture was made on, rebuild.
    ID3D11Device* dev = nullptr;
    back->GetDevice(&dev);
    if (!dev) return;
    if (m_monDevice && dev != m_monDevice) DestroyMonitorPanel();
    dev->Release();

    if (!EnsureMonitorPanel(back)) return;

    const uint32_t* src = g_overlay.Pixels();
    if (!src) return;

    D3D11_TEXTURE2D_DESC bd{};
    back->GetDesc(&bd);
    if (bd.Width < m_monitorW || bd.Height < m_monitorH) return;   // no room
    if (bd.SampleDesc.Count > 1) return;                           // MSAA: not copyable

    // The panel is 0xAARRGGBB in memory, which IS B8G8R8A8 byte order. For an
    // R8G8B8A8 backbuffer the two colour bytes have to trade places.
    const bool swapRB = (bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
                         bd.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                         bd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS);
    static std::vector<uint32_t> conv;
    const uint32_t* upload = src;
    if (swapRB) {
        const size_t n = size_t(m_monitorW) * m_monitorH;
        conv.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const uint32_t c = src[i];
            conv[i] = (c & 0xFF00FF00u) | ((c & 0x00FF0000u) >> 16) |
                      ((c & 0x000000FFu) << 16);
        }
        upload = conv.data();
    }

    __try {
        m_monContext->UpdateSubresource(m_monitorPanel, 0, nullptr, upload,
                                        m_monitorW * 4, 0);

        // UNBIND THE BACKBUFFER FIRST. Copying into a resource that is still
        // bound as a render target is illegal in D3D11, and at Present time the
        // game normally still has it bound - which is what killed the game
        // rather than merely doing nothing. Whatever was bound is put straight
        // back, so the game's own state is unchanged.
        ID3D11RenderTargetView* rtv[8] = {};
        ID3D11DepthStencilView* dsv = nullptr;
        m_monContext->OMGetRenderTargets(8, rtv, &dsv);
        m_monContext->OMSetRenderTargets(0, nullptr, nullptr);

        // Centred horizontally, a little above centre vertically - the game's
        // own HUD lives at the edges, so the middle is the least destructive
        // place for it.
        const UINT x = (bd.Width - m_monitorW) / 2;
        const UINT y = (bd.Height > m_monitorH * 2) ? (bd.Height / 2 - m_monitorH / 2)
                                                    : 0;
        m_monContext->CopySubresourceRegion(back, 0, x, y, 0, m_monitorPanel, 0,
                                            nullptr);

        m_monContext->OMSetRenderTargets(8, rtv, dsv);
        for (ID3D11RenderTargetView* v : rtv) if (v) v->Release();
        if (dsv) dsv->Release();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        // A convenience feature must never be able to take the game down.
        COTW_LOG("[monitor] the panel copy faulted - switching it off for this run");
        Cfg().panel_on_monitor = false;
        DestroyMonitorPanel();
    }
}

void VRSystem::PaintPanelOnMonitorFromSwapchain(IDXGISwapChain* swapChain) {
    if (!swapChain || !Cfg().panel_on_monitor || !g_overlay.Visible()) return;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) ||
        !back) {
        return;
    }
    PaintPanelOnMonitor(back);
    back->Release();
}

void VRSystem::DestroyMonitorPanel() {
    if (m_monitorPanel) { m_monitorPanel->Release(); m_monitorPanel = nullptr; }
    if (m_monContext) { m_monContext->Release(); m_monContext = nullptr; }
    if (m_monDevice) { m_monDevice->Release(); m_monDevice = nullptr; }
    m_monitorW = m_monitorH = 0;
    m_monitorFormat = DXGI_FORMAT_UNKNOWN;
}

void VRSystem::DestroyMenuScreen() {
    if (m_screenSwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_screenSwapchain);
        m_screenSwapchain = XR_NULL_HANDLE;
    }
    m_screenImages.clear();             // the runtime owns the textures
    m_screenW = 0;
    m_screenH = 0;
}

void VRSystem::DestroyOverlayResources() {
    if (m_overlayStaging) { m_overlayStaging->Release(); m_overlayStaging = nullptr; }
    if (m_overlaySwapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(m_overlaySwapchain);
        m_overlaySwapchain = XR_NULL_HANDLE;
    }
    m_overlayImages.clear();
    if (m_viewSpace != XR_NULL_HANDLE) {
        xrDestroySpace(m_viewSpace);
        m_viewSpace = XR_NULL_HANDLE;
    }
}

bool VRSystem::CreateHoldTextures() {
    DestroyHoldTextures();

    // CROP size, not the full backbuffer. These only ever feed an eye
    // swapchain, which is itself crop-sized, so holding the whole frame meant
    // copying pixels that were about to be thrown away - at 3360x3360 that is
    // several megabytes of pure waste every frame.
    D3D11_TEXTURE2D_DESC d{};
    // OUTPUT size, which equals the crop size unless DLSS is upscaling - in
    // which case this is what the eye is reconstructed INTO, and it is what
    // the swapchain was made at.
    uint32_t hw = m_width, hh = m_height;
    UpscaleTarget(m_width, m_height, &hw, &hh);
    m_outW = hw;
    m_outH = hh;
    d.Width = hw;
    d.Height = hh;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = m_backbufferFormat;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    // RENDER_TARGET so the first, never-yet-rendered eye can be cleared rather
    // than showing uninitialised video memory. UNORDERED_ACCESS as well when
    // DLSS is upscaling, because NGX writes its output through a UAV.
    // SHADER_RESOURCE as well: the sharpening pass reads the hold texture and
    // writes the swapchain image, replacing the copy that used to move it.
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE |
                  ((hw != m_width) ? D3D11_BIND_UNORDERED_ACCESS : 0);

    for (int eye = 0; eye < 2; ++eye) {
        if (FAILED(m_device->CreateTexture2D(&d, nullptr, &m_hold[eye]))) {
            COTW_LOG("[vr] could not create the hold texture for eye %d", eye);
            DestroyHoldTextures();
            return false;
        }
        ID3D11RenderTargetView* rtv = nullptr;
        if (SUCCEEDED(m_device->CreateRenderTargetView(m_hold[eye], nullptr, &rtv))) {
            const float black[4] = {0, 0, 0, 1};
            m_context->ClearRenderTargetView(rtv, black);
            rtv->Release();
        }
    }
    // The size they were MADE at, not the size the game renders - the second
    // reporting bug of exactly this kind, after the swapchain line said the
    // upscaler was doing nothing while it was working perfectly.
    COTW_LOG("[vr] alternate-eye hold textures created (%ux%u, game renders %ux%u)",
             hw, hh, m_width, m_height);
    return true;
}

void VRSystem::DestroyHoldTextures() {
    for (auto& t : m_hold) {
        if (t) { t->Release(); t = nullptr; }
    }
}

void VRSystem::DestroySwapchains() {
    DestroyHoldTextures();
    for (auto& eye : m_eyes) {
        if (eye.handle != XR_NULL_HANDLE) {
            xrDestroySwapchain(eye.handle);
            eye.handle = XR_NULL_HANDLE;
        }
        eye.images.clear();
    }
    m_width = m_height = 0;
    m_eyesPrimed = false;
}

// ---------------------------------------------------------------------------
// init / shutdown
// ---------------------------------------------------------------------------

bool VRSystem::Init(IDXGISwapChain* swapChain) {
    if (m_failed) return false;

    DXGI_SWAP_CHAIN_DESC scd{};
    if (FAILED(swapChain->GetDesc(&scd))) {
        COTW_LOG("[vr] IDXGISwapChain::GetDesc failed");
        m_failed = true;
        return false;
    }
    m_backbufferFormat = scd.BufferDesc.Format;
    // Needed before the first FOV calculation, which divides by this aspect -
    // leaving it zero produced a NaN frustum on the opening frame.
    m_srcWidth = scd.BufferDesc.Width;
    m_srcHeight = scd.BufferDesc.Height;
    COTW_LOG("[vr] game swapchain %ux%u format %d (%s) windowed=%d buffers=%u",
             scd.BufferDesc.Width, scd.BufferDesc.Height, (int)m_backbufferFormat,
             FormatName(m_backbufferFormat), scd.Windowed, scd.BufferCount);

    if (IsHdrFormat(m_backbufferFormat)) {
        COTW_LOG("[vr] *** the backbuffer is an HDR format. A 10/16-bit backbuffer "
                 "cannot be copied into an 8-bit XR swapchain - the headset will be "
                 "black while the monitor looks fine. TURN HDR OFF in Windows and in "
                 "the game's display settings. ***");
    }

    if (FAILED(swapChain->GetDevice(__uuidof(ID3D11Device), (void**)&m_device)) || !m_device) {
        COTW_LOG("[vr] swapchain is not a D3D11 swapchain - cannot bind OpenXR");
        m_failed = true;
        return false;
    }
    // GetDevice added a ref; keep it for the session's lifetime but do not let
    // it hold the device alive past shutdown.
    m_device->GetImmediateContext(&m_context);

    // ONE FRAME IN FLIGHT.
    //
    // D3D queues up to three frames ahead by default. That suits a monitor,
    // where a deep queue smooths over an uneven CPU - but a headset is paced by
    // the runtime, not by the game, and a frame that was built three frames ago
    // is submitted against a head pose that has since moved. What arrives is not
    // late so much as UNEVENLY late, and uneven is what the eye picks up.
    //
    // Alternate-eye rendering punishes this hardest: each eye already only
    // updates every other frame, so a wobble in pacing leaves one eye stale for
    // longer on some frames than others, and irregular flicker is far more
    // visible than steady flicker. Every UEVR profile in the prior-art notes
    // sets the same thing (D3D12.MaximumFrameLatency=1, r.OneFrameThreadLag=1);
    // this is the D3D11 spelling of it.
    //
    // 0 leaves the game's own setting untouched, which is the way back if this
    // ever costs more framerate than it is worth.
    if (Cfg().frame_latency > 0) {
        IDXGIDevice1* dxgi1 = nullptr;
        if (SUCCEEDED(m_device->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxgi1))
            && dxgi1) {
            UINT before = 0;
            dxgi1->GetMaximumFrameLatency(&before);
            const UINT want = (UINT)Cfg().frame_latency;
            if (SUCCEEDED(dxgi1->SetMaximumFrameLatency(want))) {
                COTW_LOG("[vr] frames in flight %u -> %u", before, want);
            } else {
                COTW_LOG("[vr] could not set frames in flight (left at %u)", before);
            }
            dxgi1->Release();
        } else {
            COTW_LOG("[vr] no IDXGIDevice1 - frames in flight left alone");
        }
    }

    if (!CreateInstance())          { m_failed = true; return false; }
    if (!CreateSession(m_device))   { m_failed = true; return false; }
    if (!PickSwapchainFormat(m_backbufferFormat, &m_swapchainFormat)) {
        m_failed = true; return false;
    }
    // Eye swapchains are created on the first frame instead of here: their size
    // is the CROPPED region, and the crop depends on the headset's field of
    // view, which is only known once xrLocateViews has run.

    if (!CreateOverlayResources()) {
        COTW_LOG("[vr] the settings panel is unavailable this run; everything else "
                 "still works and the ini can be edited by hand");
    }

    COTW_LOG("[vr] === OpenXR ready ===");
    return true;
}

void VRSystem::Shutdown() {
    if (m_sessionRunning) {
        xrEndSession(m_session);
        m_sessionRunning = false;
    }
    DestroyOverlayResources();
    DestroySwapchains();
    if (m_space != XR_NULL_HANDLE)    { xrDestroySpace(m_space); m_space = XR_NULL_HANDLE; }
    if (m_session != XR_NULL_HANDLE)  { xrDestroySession(m_session); m_session = XR_NULL_HANDLE; }
    if (m_instance != XR_NULL_HANDLE) { xrDestroyInstance(m_instance); m_instance = XR_NULL_HANDLE; }
    if (m_context) { m_context->Release(); m_context = nullptr; }
    if (m_device)  { m_device->Release();  m_device = nullptr; }
    COTW_LOG("[vr] shut down");
}

void VRSystem::OnResizeBuffers() {
    if (!Ready()) return;
    COTW_LOG("[vr] ResizeBuffers - dropping eye swapchains, they will be rebuilt");
    DestroySwapchains();
}

// ---------------------------------------------------------------------------
// events
// ---------------------------------------------------------------------------

void VRSystem::PollEvents() {
    // Skipping this leaves the session stuck in IDLE, rendering nothing, with no
    // error anywhere.
    for (;;) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        if (xrPollEvent(m_instance, &ev) != XR_SUCCESS) break;

        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto& s = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            m_state = s.state;
            COTW_LOG("[vr] session state -> %s", StateName());

            if (s.state == XR_SESSION_STATE_READY && !m_sessionRunning) {
                XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                const XrResult r = xrBeginSession(m_session, &begin);
                m_sessionRunning = XR_SUCCEEDED(r);
                COTW_LOG("[vr] xrBeginSession -> %s", XrStr(r));
            } else if (s.state == XR_SESSION_STATE_STOPPING && m_sessionRunning) {
                xrEndSession(m_session);
                m_sessionRunning = false;
                COTW_LOG("[vr] session stopped");
            } else if (s.state == XR_SESSION_STATE_EXITING ||
                       s.state == XR_SESSION_STATE_LOSS_PENDING) {
                COTW_LOG("[vr] session exiting/lost - shutting VR down, the game keeps running");
                Shutdown();
                return;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            COTW_LOG("[vr] instance loss pending");
            Shutdown();
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// the frame
// ---------------------------------------------------------------------------

bool VRSystem::HeadPose(XrQuaternionf* orientation, XrVector3f* position) const {
    if (!m_headValid) return false;
    if (orientation) *orientation = m_headPose.orientation;
    if (position) *position = m_headPose.position;
    return true;
}

// The region of the backbuffer that survives the crop. Everything that reads
// the backbuffer goes through this, so no stage of the pipeline ever moves a
// pixel that is going to be thrown away.
// *** TIER 1: THE PER-EYE RECTANGLE, AND THE FOV THAT MATCHES IT. ***
//
// A sub-rectangle of a symmetric perspective image IS an off-centre sub-frustum,
// exactly - no approximation. Once Lever A has widened the rendered frame so it
// contains both eyes' frusta, each eye can simply be given its own rectangle.
//
// The submitted fov is derived BACK from the clamped bounds rather than copied
// from the runtime. That is deliberate and it is what makes this safe: if the
// render is too narrow, the crop clamps, the eye loses field at that edge - and
// the fov still describes the pixels being sent, so the image still fuses. A fov
// that disagrees with its pixels is precisely what does not fuse, and that is
// the failure the old symmetric path existed to avoid.
void VRSystem::ComputeEyeCrops(uint32_t srcW, uint32_t srcH, const XrView* views) {
    if (!views || !srcW || !srcH) { m_haveEyeCrop = false; return; }

    // *** LATCH THE RECTANGLE. THE HEADSET SEES EVERY PIXEL IT MOVES BY. ***
    //
    // This runs every frame and derives the crop from views[eye].fov - what
    // the runtime reports THIS frame. On paper that is a constant; in practice
    // it wobbles in the last digits, and each wobble moves the rectangle
    // copied into the swapchain by a pixel or two. The desktop mirror never
    // gets cropped, so it stays perfectly clean while the headset vibrates -
    // which is exactly the fault measured 2026-08-13, with every temporal
    // feature off and the submitted pose frozen.
    //
    // TIER1_FOV_PLAN.md said this in as many words for the runtime fov -
    // "latch once, do not recompute per frame" - and the crop path never did.
    // Recompute only when the fov REALLY changes (a different headset mode, a
    // resolution change), never for float noise.
    if (Cfg().tier1_crop_latch && m_haveEyeCrop && m_cropSrcW == srcW &&
        m_cropSrcH == srcH) {
        bool same = true;
        for (int e = 0; e < 2 && same; ++e) {
            const XrFovf& a = views[e].fov;
            const XrFovf& b = m_cropFov[e];
            same = fabsf(a.angleLeft - b.angleLeft) < 1e-4f &&
                   fabsf(a.angleRight - b.angleRight) < 1e-4f &&
                   fabsf(a.angleUp - b.angleUp) < 1e-4f &&
                   fabsf(a.angleDown - b.angleDown) < 1e-4f;
        }
        if (same) return;                 // keep the rectangle we already have
    }
    m_haveEyeCrop = false;
    m_cropSrcW = srcW;
    m_cropSrcH = srcH;
    for (int e = 0; e < 2; ++e) m_cropFov[e] = views[e].fov;

    // *** THE BASE FRUSTUM, NOT THE ONE ON SCREEN RIGHT NOW. ***
    //
    // Aiming narrows the WHOLE scene's projection - measured, 1.08 -> 2.98. If
    // the crop follows that, the submitted field shrinks with it and the runtime
    // is told the image spans ~19 degrees: the picture collapses to a small
    // square with black around it, which is exactly what scoping did.
    //
    // The field the headset is shown must NOT change when you aim. Zoom belongs
    // in the world inside that field, not in the size of the window onto it.
    //
    // So the base is computed, not measured: k x tan(half the game's vertical
    // FOV). That is not a guess - Stage 1 closed the loop on it, expecting
    // 102.68 deg and reading 102.69 back out of the GPU. The measured value is
    // still logged, and a persistent disagreement while NOT scoped would mean
    // this base is wrong.
    const float kDeg = 0.01745329252f;
    float tanV = tanf(Cfg().game_fov_deg * 0.5f * kDeg);
    if (Cfg().fov_is_horizontal) tanV /= (float(srcW) / float(srcH));
    if (Cfg().tier1_fov && Cfg().tier1_fov_k_override > 0.05f) {
        tanV *= Cfg().tier1_fov_k_override;
    }
    const float tanH = tanV * (float(srcW) / float(srcH));
    if (!(tanH > 0.01f) || !(tanV > 0.01f)) return;

    for (int eye = 0; eye < 2; ++eye) {
        const XrFovf& f = views[eye].fov;
        // NDC x = -1 at u = 0; NDC y = +1 (UP) at v = 0 (the TOP row). The y
        // flip is the single most likely thing to get backwards here, and a
        // mirrored v presents as unexplained vertical disparity and eye strain -
        // which gets blamed on IPD, not on the crop.
        float u0 = 0.5f + 0.5f * tanf(f.angleLeft) / tanH;
        float u1 = 0.5f + 0.5f * tanf(f.angleRight) / tanH;
        float v0 = 0.5f - 0.5f * tanf(f.angleUp) / tanV;
        float v1 = 0.5f - 0.5f * tanf(f.angleDown) / tanV;

        const bool clampedL = u0 < 0.0f, clampedR = u1 > 1.0f;
        const bool clampedT = v0 < 0.0f, clampedB = v1 > 1.0f;
        if (u0 < 0.0f) u0 = 0.0f;
        if (u1 > 1.0f) u1 = 1.0f;
        if (v0 < 0.0f) v0 = 0.0f;
        if (v1 > 1.0f) v1 = 1.0f;
        if (!(u1 > u0) || !(v1 > v0)) return;

        // *** THE RECTANGLE FIRST, THE ANGLES SECOND. THIS ORDER IS THE FIX. ***
        //
        // The angles used to be derived here, from the FRACTIONS, and the
        // rectangle built underneath - after which it was rounded to an even
        // width, clamped, and possibly shifted back inside the source. Three
        // changes to the rectangle, all made AFTER the angles that describe it
        // had already been computed and submitted.
        //
        // The runtime is being told "this rectangle spans these angles". If the
        // rectangle then moves or resizes, every pixel in it is presented at
        // slightly the wrong angle - and by a DIFFERENT amount in each eye,
        // because the rounding and the edge clamp land differently on each. That
        // is a residual per-eye disagreement that survives even ipd_mm = 0,
        // where the two eye cameras are identical and nothing else can differ.
        // Measured in the headset: the scope crosshair still disagreed slightly
        // between the eyes at 0 mm separation, which is exactly this.
        //
        // The old comment claimed the two "can never disagree" because the
        // angles came from the CLAMPED bounds. That was half true - it covered
        // the 0..1 clamp above, and nothing below it.
        //
        // The rectangle IS the crop: position and size both come from the
        // fractions. It is submitted as a sub-rectangle of a full-size
        // swapchain, so it may change size freely from frame to frame.
        // *** AN EVEN ORIGIN, BECAUSE AN UPSCALER RECONSTRUCTS ON A LATTICE. ***
        //
        // The SIZE has always been rounded even; the ORIGIN never was. That is
        // harmless for a plain copy and not harmless at all for DLSS: the
        // sub-rectangle's base decides where the reconstruction lattice sits
        // inside the frame, so an odd base puts this eye half a texel off the
        // grid its samples were drawn on - and the two eyes, landing on
        // different parities, disagree with each other as well.
        //
        // The owner found the per-eye crop was the culprit behind the head-turn
        // shimmer by switching it off; this is the first of the two reasons it
        // could be.
        uint32_t x = (uint32_t)(u0 * float(srcW) + 0.5f) & ~1u;
        uint32_t y = (uint32_t)(v0 * float(srcH) + 0.5f) & ~1u;
        uint32_t rw = (uint32_t)((u1 - u0) * float(srcW) + 0.5f) & ~1u;
        uint32_t rh = (uint32_t)((v1 - v0) * float(srcH) + 0.5f) & ~1u;
        if (rw > m_width) rw = m_width;
        if (rh > m_height) rh = m_height;
        if (rw < 64 || rh < 64) return;
        if (x + rw > srcW) x = srcW - rw;
        if (y + rh > srcH) y = srcH - rh;

        // Now the angles, from the FINAL integer rectangle - the actual pixels
        // the runtime is being handed. Whatever rounding, clamping or shifting
        // happened above is already inside these numbers, so the description and
        // the thing described cannot come apart.
        const float fu0 = float(x) / float(srcW);
        const float fu1 = float(x + rw) / float(srcW);
        const float fv0 = float(y) / float(srcH);
        const float fv1 = float(y + rh) / float(srcH);
        m_eyeFov[eye].angleLeft  = atanf((2.0f * fu0 - 1.0f) * tanH);
        m_eyeFov[eye].angleRight = atanf((2.0f * fu1 - 1.0f) * tanH);
        m_eyeFov[eye].angleUp    = atanf((1.0f - 2.0f * fv0) * tanV);
        m_eyeFov[eye].angleDown  = atanf((1.0f - 2.0f * fv1) * tanV);
        m_eyeCropW[eye] = rw;
        m_eyeCropH[eye] = rh;
        // *** NO SECOND CLAMP HERE, AND THIS ONE IS WORTH REMEMBERING. ***
        //
        // A stale clamp lived here reading "if (m_width >= srcW) x = 0;" - and
        // m_width IS srcW whenever the frame needs no aspect crop, which is
        // exactly the case on the FULLVIEW presets. So both eyes' offsets were
        // zeroed AFTER being computed correctly, each eye was handed the same
        // pixels with a different fov, and the result was the sideways double
        // image. The fractions in the log were right the whole time; only these
        // two lines undid them.
        //
        // It survived an earlier delete because this file mixes CRLF and LF, the
        // match failed, and the removal was written without an assert - so
        // nothing complained and the next test looked like a fresh mystery.
        //
        // The rectangle is already bounded by the two lines above, which use the
        // RECT's own size. m_width has no business in this calculation at all.
        m_eyeCropX[eye] = x;
        m_eyeCropY[eye] = y;

        // ONCE A SECOND, not three times ever. Capping it meant every line in
        // the log came from the menu camera at t=4s - the one moment the crop is
        // meaningless - and the gameplay values, which are the ones that decide
        // whether this works, were never printed at all.
        static ULONGLONG lastLog[2] = {0, 0};
        const ULONGLONG nowMs = GetTickCount64();
        if (nowMs - lastLog[eye] >= 1000) {
            lastLog[eye] = nowMs;
            // The snap is how far the integer rectangle sits from the fractions
            // that asked for it, in arcminutes. It used to be invisible AND
            // uncorrected; it is now corrected, and printed so the size of what
            // was being lost is on the record rather than assumed.
            const float snapH = (atanf((2.0f * (float(x) / float(srcW)) - 1.0f) * tanH)
                                 - atanf((2.0f * u0 - 1.0f) * tanH)) / kDeg * 60.0f;
            const float snapV = (atanf((1.0f - 2.0f * (float(y) / float(srcH))) * tanV)
                                 - atanf((1.0f - 2.0f * v0) * tanV)) / kDeg * 60.0f;
            COTW_LOG("[tier1] eye%d crop u[%.5f,%.5f] v[%.5f,%.5f] -> %ux%u at "
                     "(%u,%u); snap %+.2f' h %+.2f' v; "
                     "submitting %.2f/%.2f h, %.2f/%.2f v deg%s%s%s%s",
                     eye, u0, u1, v0, v1, rw, rh, x, y, snapH, snapV,
                     m_eyeFov[eye].angleLeft / kDeg, m_eyeFov[eye].angleRight / kDeg,
                     m_eyeFov[eye].angleUp / kDeg, m_eyeFov[eye].angleDown / kDeg,
                     clampedL ? "  CLAMPED-LEFT" : "",
                     clampedR ? "  CLAMPED-RIGHT" : "",
                     clampedT ? "  CLAMPED-TOP" : "",
                     clampedB ? "  CLAMPED-BOTTOM" : "");
        }
    }
    m_haveEyeCrop = true;
}

D3D11_BOX VRSystem::CropBox(int eye) const {
    if (!Cfg().tier1_per_eye_crop || !m_haveEyeCrop || eye < 0 || eye > 1) {
        return CropBox();
    }
    D3D11_BOX b{};
    b.left = m_eyeCropX[eye];
    b.top = m_eyeCropY[eye];
    b.front = 0;
    b.right = m_eyeCropX[eye] + m_eyeCropW[eye];
    b.bottom = m_eyeCropY[eye] + m_eyeCropH[eye];
    b.back = 1;
    return b;
}

D3D11_BOX VRSystem::CropBox() const {
    D3D11_BOX b{};
    b.left = m_cropX;
    b.top = m_cropY;
    b.front = 0;
    b.right = m_cropX + m_width;
    b.bottom = m_cropY + m_height;
    b.back = 1;
    return b;
}

// Once-a-second truth about the upscale: how many frames it ran, how many it
// declined, why, and at what sizes.
static long g_upRan[2] = {0, 0}, g_upDeclined[2] = {0, 0};
static const char* g_upWhy[2] = {"", ""};
static void ReportUpscale(int eye, uint32_t srcW, uint32_t srcH,
                          uint32_t outW, uint32_t outH) {
    static ULONGLONG last[2] = {0, 0};
    const ULONGLONG now = GetTickCount64();
    if (!last[eye]) { last[eye] = now; return; }
    if (now - last[eye] < 1000) return;
    last[eye] = now;
    COTW_LOG("[vr] upscale eye %d: %ld ran, %ld declined%s%s  (%ux%u -> %ux%u)",
             eye, g_upRan[eye], g_upDeclined[eye],
             g_upDeclined[eye] ? " - " : "", g_upDeclined[eye] ? g_upWhy[eye] : "",
             srcW, srcH, outW, outH);
    g_upRan[eye] = 0;
    g_upDeclined[eye] = 0;
}

// *** THE ONE PLACE AN EYE'S FINISHED IMAGE IS CAPTURED. ***
//
// Every route to the headset comes through here, and it does one of two things:
// the plain crop-sized copy this mod has always done, or - when DLSS upscaling
// is on and the resolve has published what it needs - a DLSS reconstruction of
// the crop into a larger hold texture.
//
// It records what it actually wrote in m_holdW/m_holdH, and that is the point.
// The upscale can decline for perfectly ordinary reasons (the resolve has not
// run yet this session, the depth is a different size, NGX said no) and when it
// does the hold holds a render-sized image. Submitting the upscaled extent over
// that would show a magnified top-left corner - a spectacular-looking bug from
// a one-frame fallback. The submit reads these two numbers instead of assuming.
bool VRSystem::StashInto(int eye, ID3D11Texture2D* back, const D3D11_BOX& boxIn) {
    if (!back || eye < 0 || eye > 1) return false;

    // *** THE UPSCALER GETS ONE RECTANGLE AND KEEPS IT. ***
    //
    // DLSS accumulates into a history that is keyed to the rectangle it was
    // given. Move or resize that rectangle and every pixel of the new frame
    // lands somewhere else in the history - which is a whole-image misalignment
    // once per change, invisible while still and a shimmer the moment anything
    // moves. The per-eye crop is recomputed from the runtime's own per-eye
    // frustum, which is exactly the kind of number that can wobble in its last
    // decimal place, and `tier1_crop_latch` guards the crop but nothing guarded
    // what DLSS was handed.
    //
    // So the first rectangle an eye is upscaled with is kept for as long as the
    // render size does not change. The player's crop can still evolve - it is
    // submitted as an image rect, which costs nothing - while the reconstruction
    // keeps a fixed frame of reference.
    D3D11_BOX box = boxIn;
    // *** DO NOT LATCH A RECTANGLE THAT IS NOT THE FINAL ONE. ***
    //
    // The per-eye crop needs the runtime's per-eye frustum, which arrives with
    // the first xrLocateViews - and an eye can be captured before that has ever
    // happened. Latching then stores the BASE rectangle, while the submit goes
    // on describing it with the PER-EYE angles as soon as they exist: pixels
    // from one rectangle, angles from another, differently per eye. That is
    // double vision, and it is what the owner saw when the crop was switched
    // back on after the first latch shipped.
    //
    // The mode is recorded with the rectangle as well, so turning the crop on or
    // off re-latches instead of keeping a rectangle from the other mode.
    const bool perEyeNow = Cfg().tier1_per_eye_crop && m_haveEyeCrop;
    const bool cropReady = !Cfg().tier1_per_eye_crop || m_haveEyeCrop;
    if (Cfg().dlss_enable && Cfg().dlss_upscale_rect_latch && cropReady) {
        const uint32_t w = boxIn.right - boxIn.left, h = boxIn.bottom - boxIn.top;
        if (m_upRectW[eye] && m_upRectSrcW[eye] == m_srcWidth &&
            m_upRectSrcH[eye] == m_srcHeight && m_upRectPerEye[eye] == perEyeNow) {
            box.left = m_upRectX[eye];
            box.top = m_upRectY[eye];
            box.right = m_upRectX[eye] + m_upRectW[eye];
            box.bottom = m_upRectY[eye] + m_upRectH[eye];
        } else {
            m_upRectX[eye] = boxIn.left;
            m_upRectY[eye] = boxIn.top;
            m_upRectW[eye] = w;
            m_upRectH[eye] = h;
            m_upRectSrcW[eye] = m_srcWidth;
            m_upRectSrcH[eye] = m_srcHeight;
            m_upRectPerEye[eye] = perEyeNow;
            // *** AND THE ANGLES THAT DESCRIBE IT, IN THE SAME BREATH. ***
            //
            // ComputeEyeCrops states the rule this broke: "the runtime is being
            // told THIS RECTANGLE SPANS THESE ANGLES. If the rectangle then
            // moves or resizes, every pixel in it is presented at slightly the
            // wrong angle - and by a DIFFERENT amount in each eye." Latching the
            // pixels while the submitted angles carried on updating is exactly
            // that, and a per-eye angular disagreement is double vision, which
            // is what the owner saw the moment the crop was switched back on.
            //
            // So the rectangle and its angles are one object with one lifetime.
            m_haveUpFov[eye] = perEyeNow;
            if (perEyeNow) m_upFov[eye] = m_eyeFov[eye];
            COTW_LOG("[vr] eye %d: upscale rectangle latched at %u,%u %ux%u (with its "
                     "own frustum) - DLSS keeps this frame of reference until the "
                     "render size changes", eye, boxIn.left, boxIn.top, w, h);
        }
    }
    if (!m_hold[0] || !m_hold[1]) {
        if (!m_width || !m_height) return false;
        if (!CreateHoldTextures()) return false;
        COTW_LOG("[vr] hold textures created on demand - full-rate was switched on "
                 "after startup");
    }

    const uint32_t srcW = box.right - box.left;
    const uint32_t srcH = box.bottom - box.top;
    uint32_t outW = srcW, outH = srcH;
    UpscaleTarget(srcW, srcH, &outW, &outH);

    if (outW != srcW && outW <= m_outW && outH <= m_outH) {
        ID3D11Resource* depth = TaaDepthForEye(eye);
        ID3D11Resource* motion = TaaMotionVectors();
        float jx = 0.0f, jy = 0.0f;
        // *** THE JITTER IS ASKED FOR IN FULL-FRAME PIXELS, NOT CROP PIXELS. ***
        //
        // It is stored as an NDC offset in the camera matrix, and NDC spans the
        // WHOLE render target - so turning it into pixels means multiplying by
        // the full width, not by the width of the piece we happen to be showing.
        // Passing the crop under-reported it by the crop ratio, about 12% here,
        // silently: an under-reported jitter is still a legal sub-pixel value,
        // so nothing rejected it and nothing logged it.
        //
        // The in-resolve DLAA path never had this - it asks with the resolve
        // target's own size, which is the full frame - which is why the fault
        // only ever showed with upscaling on.
        const bool haveJitter =
            TaaJitterForEye(eye, m_srcWidth ? m_srcWidth : srcW,
                            m_srcHeight ? m_srcHeight : srcH, &jx, &jy);
        // A hitch or a settings change means the history is about to lie; the
        // resolve already tracks that, and a reset is one soft frame against a
        // second of smear.
        static uint32_t lastW[2] = {0, 0}, lastH[2] = {0, 0};
        const bool reset = (lastW[eye] != srcW || lastH[eye] != srcH);
        if (depth && motion && haveJitter &&
            DlssUpscale(m_context, eye, back, depth, motion, m_hold[eye],
                        box.left, box.top, srcW, srcH, outW, outH, jx, jy, reset)) {
            lastW[eye] = srcW;
            lastH[eye] = srcH;
            m_holdW[eye] = outW;
            m_holdH[eye] = outH;
            g_upRan[eye]++;
            ReportUpscale(eye, srcW, srcH, outW, outH);
            return true;
        }
        // *** COUNT, DO NOT MOAN THREE TIMES AND GO QUIET. ***
        //
        // The first build logged the first three failures and then said nothing
        // ever again. Those three all landed in the menu, before the resolve had
        // ever run, so the log showed "upscale unavailable" and never showed the
        // success 70 seconds later - and the run was read as "no upscaling
        // happened" when it was in fact working. A once-a-second count of both
        // outcomes cannot mislead that way.
        g_upDeclined[eye]++;
        g_upWhy[eye] = !depth ? "no depth yet" : (!motion ? "no motion vectors yet"
                                                          : (!haveJitter ? "no jitter yet"
                                                                         : "NGX declined"));
    }
    ReportUpscale(eye, srcW, srcH, outW, outH);

    m_context->CopySubresourceRegion(m_hold[eye], 0, 0, 0, 0, back, 0, &box);
    m_holdW[eye] = srcW;
    m_holdH[eye] = srcH;
    return true;
}

bool VRSystem::StashEyeFromBackbuffer(int eye) {
    if (!Ready() || eye < 0 || eye > 1) return false;
    IDXGISwapChain* sc = CurrentSwapChain();
    if (!sc) return false;
    ID3D11Texture2D* back = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back)) || !back) {
        return false;
    }
    const bool ok = StashInto(eye, back, CropBox(eye));
    back->Release();
    return ok;
}

// Wall-clock milliseconds. QueryPerformanceCounter is the only clock with the
// resolution to tell a 0.3 ms blit from a 3 ms one.
static double NowMs() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) * 1000.0 / double(freq.QuadPart);
}

void VRSystem::SubmitFrame(IDXGISwapChain* swapChain) {
    if (!Ready()) return;
    ++m_frames;

    PollEvents();
    if (!Ready() || !m_sessionRunning) return;

    // Before anything is acquired: the quality level may have moved, or the
    // game's own video menu may have changed the resolution under us.
    EnsureOutputSize();
    if (!Ready()) return;

    const bool timing = Cfg().perf_log_every > 0;
    const double tStart = timing ? NowMs() : 0.0;

    XrFrameState frameState{XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(m_session, nullptr, &frameState);
    if (XR_FAILED(r)) {
        COTW_LOG("[vr] xrWaitFrame failed: %d %s", (int)r, XrStr(r));
        return;
    }
    // *** WHAT THE RUNTIME PREDICTS IS NOT ALWAYS SANE. *** (prior art lesson 9)
    //
    // The display period is used to walk the predicted time back for the
    // prediction dampening, so a garbage value there moves the pose by a
    // garbage amount - and garbage predictions ARE an observed failure mode:
    // the AC Black Flag framework checks for exactly this and names the values
    // it has seen. A period outside 2..40 ms is not a frame rate any headset
    // runs at, so it is replaced with 90 Hz rather than trusted.
    if (frameState.predictedDisplayPeriod < 2000000 ||     // 2 ms
        frameState.predictedDisplayPeriod > 40000000) {    // 40 ms
        static int moaned = 0;
        if (moaned < 3) {
            ++moaned;
            COTW_LOG("[vr] the runtime predicted a display period of %lld ns, which "
                     "is not a frame rate - using 11.1 ms instead. Anything that "
                     "paces itself from this would otherwise be wrong.",
                     (long long)frameState.predictedDisplayPeriod);
        }
        frameState.predictedDisplayPeriod = 11111111;      // 90 Hz
    }

    const double tWaited = timing ? NowMs() : 0.0;
    r = xrBeginFrame(m_session, nullptr);
    if (XR_FAILED(r) && r != XR_FRAME_DISCARDED) {
        COTW_LOG("[vr] xrBeginFrame failed: %d %s", (int)r, XrStr(r));
        return;
    }

    XrCompositionLayerProjectionView projViews[2]{};
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    bool haveLayer = false;
    // The flat screen (see MenuScreenReason). hideWorldForScreen is deliberately
    // separate from haveScreen: only the MAIN menu earns a blanked world.
    bool haveScreen = false;
    bool hideWorldForScreen = false;

    if (frameState.shouldRender) {
        XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
        locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        locate.displayTime = frameState.predictedDisplayTime;
        locate.space = m_space;

        // *** PREDICTION DAMPENING - the shake fix that is not a filter. ***
        //
        // The runtime does not report where the head IS, it EXTRAPOLATES where
        // it will be at display time. Extrapolation multiplies velocity, and a
        // pulse tremor is a tiny movement with a sharp velocity spike - so a
        // fraction of a degree of real tremor arrives as a visibly larger
        // swing, and the view beats with the heartbeat.
        //
        // Asking for a pose closer to NOW shrinks that amplification at its
        // source. It is what OpenXR Toolkit ships for exactly this complaint -
        // named "Prediction dampening", renamed "Shaking reduction", finally
        // "Over-prediction reduction" - and unlike smoothing it adds no lag to
        // a real head turn; it trades a little latency for the overshoot.
        //
        // 0 = the runtime's own prediction (stock). 100 = locate at the time
        // the frame began, i.e. no forward prediction at all.
        if (Cfg().prediction_damp_pct > 0 && frameState.predictedDisplayPeriod > 0) {
            long pct = Cfg().prediction_damp_pct;
            if (pct > 100) pct = 100;
            // The prediction we are shortening is the interval from now to the
            // predicted display instant; one display period is its natural
            // scale and is the only interval the runtime hands us.
            const XrDuration back =
                (frameState.predictedDisplayPeriod * pct) / 100;
            locate.displayTime = frameState.predictedDisplayTime - back;
        }

        XrViewState viewState{XR_TYPE_VIEW_STATE};
        XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t viewCount = 0;
        r = xrLocateViews(m_session, &locate, &viewState, 2, &viewCount, views);

        const bool posesValid =
            XR_SUCCEEDED(r) && viewCount == 2 &&
            (viewState.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) &&
            (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);

        if (posesValid) {
            // Per-eye rectangles, from the runtime's own tangents. Recomputed
            // every frame because the source size and the FOV lever can change
            // live, and a stale rectangle would be a fov that lies about its
            // pixels.
            if (Cfg().tier1_per_eye_crop) {
                ComputeEyeCrops(m_srcWidth ? m_srcWidth : m_width,
                                m_srcHeight ? m_srcHeight : m_height, views);
            }
            ID3D11Texture2D* back = nullptr;
            const bool haveBack =
                !Cfg().blank_layer_test &&
                SUCCEEDED(swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&back));

            // Alternate-eye: this frame was rendered from one eye's viewpoint
            // (the camera hook shifted the camera while the engine drew it), so
            // it belongs to that eye.  The other eye keeps its previous image.
            //
            // Two ways to make the other eye keep it:
            //   fast  - do not touch its swapchain at all; the runtime keeps
            //           compositing the image it was last handed. 1 blit/frame.
            //   hold  - stash every frame in a texture of our own and re-upload
            //           both eyes. 3 blits/frame. Needed by full-rate, which
            //           writes both eyes from the same instant.
            const bool aerFast = Cfg().stereo && haveBack && m_eyesPrimed &&
                                 Cfg().aer_reuse_swapchain && !FullRateActive();
            const bool stereo = Cfg().stereo && haveBack && !aerFast &&
                                m_hold[0] && m_hold[1];
            m_lastPathName = FullRateActive() ? "FULL-RATE (both eyes, one instant)"
                             : aerFast       ? "AER fast (1 blit)"
                             : stereo        ? "AER hold (3 blits)"
                                             : "mono (2 blits)";

            // The game renders ONE image through ONE symmetric frustum, so both
            // eyes must be shown it through the SAME symmetric frustum. Handing
            // each eye the runtime's own mirrored asymmetric FOV puts the image
            // at a different angular position per eye and it will not fuse.
            // The layer's frustum must be the one the image was RENDERED with,
            // derived from the game's vertical FOV and the image's own aspect
            // ratio. Showing a 4:3 image through the headset's near-square
            // frustum both squashes it and magnifies every angle - which is
            // harmless at distance (disparity ~0) but makes near objects
            // painful, because their disparity is magnified with it.
            XrFovf fov;
            if (Cfg().game_fov_deg > 1.0f) {
                const float kDeg = 0.01745329252f;
                // The game's FOV describes the FULL frame it rendered, so work
                // in tangents on the full frame first, then scale by how much
                // of it survived the crop. Deriving the FOV from the cropped
                // size directly would claim the game rendered that much world
                // into a smaller window, and everything would be the wrong size.
                const uint32_t srcW = m_srcWidth ? m_srcWidth : m_width;
                const uint32_t srcH = m_srcHeight ? m_srcHeight : m_height;
                const float fullAspect = float(srcW) / float(srcH);

                float tanH, tanV;
                if (Cfg().fov_is_horizontal) {
                    tanH = tanf(Cfg().game_fov_deg * 0.5f * kDeg);
                    tanV = tanH / fullAspect;
                } else {
                    tanV = tanf(Cfg().game_fov_deg * 0.5f * kDeg);
                    tanH = tanV * fullAspect;
                }
                // On the very first frame the crop has not been worked out yet
                // (it needs this FOV). Treat it as "keep everything" until then -
                // scaling by a zero width produced a zero FOV and then a NaN
                // frustum aspect.
                const uint32_t keptW = m_width ? m_width : srcW;
                const uint32_t keptH = m_height ? m_height : srcH;
                tanH *= float(keptW) / float(srcW);
                tanV *= float(keptH) / float(srcH);

                const float hHalf = atanf(tanH);
                const float vHalf = atanf(tanV);
                fov.angleUp = vHalf;
                fov.angleDown = -vHalf;
                fov.angleLeft = -hHalf;
                fov.angleRight = hHalf;
            } else if (Cfg().symmetric_fov) {
                fov = SymmetricFov(views[0].fov, views[1].fov);
            } else {
                fov = views[0].fov;
            }

            // The eye swapchains are sized to the CROPPED region, so they can
            // only be created once the headset's FOV is known - which is now.
            if (m_eyes[0].handle == XR_NULL_HANDLE) {
                DXGI_SWAP_CHAIN_DESC scd{};
                if (SUCCEEDED(swapChain->GetDesc(&scd))) {
                    ComputeCrop(scd.BufferDesc.Width, scd.BufferDesc.Height, fov);
                    if (!CreateSwapchains(m_width, m_height)) {
                        COTW_LOG("[vr] could not create eye swapchains - VR stopping");
                        if (back) back->Release();
                        m_failed = true;
                        xrEndFrame(m_session, nullptr);
                        Shutdown();
                        return;
                    }
                }
            }

            // The crop region is only known once the FOV above has been worked
            // out, so every backbuffer read happens from here down.
            const D3D11_BOX srcBox = CropBox(CurrentRenderEye());

            if (stereo) {
                // Full-rate: eye 0 was stashed by the scene hook between the two
                // draws, and the backbuffer now holds eye 1 - both from the same
                // instant.  Alternate-eye: this frame belongs to whichever eye
                // the camera was offset for, and the other keeps its last image.
                // Full-rate stashes BOTH eyes itself, around the second-eye
                // replay, so Present must not overwrite them with whatever
                // happens to be in the backbuffer at this moment.
                //
                // Cropping on the way IN means only the pixels that will be
                // displayed are ever moved: at 3360x3360 into a 3078x3360 eye,
                // holding the whole frame copied ~3.8 MB per frame that was
                // then discarded.
                if (!FullRateActive()) {
                    StashInto(CurrentRenderEye(), back, srcBox);
                }
            }

            static float loggedFovDeg = -1.0f;
            static bool loggedFovHoriz = false;
            if (fabsf(loggedFovDeg - Cfg().game_fov_deg) > 0.01f ||
                loggedFovHoriz != Cfg().fov_is_horizontal) {
                loggedFovDeg = Cfg().game_fov_deg;
                loggedFovHoriz = Cfg().fov_is_horizontal;
                const float deg = 57.2957795f;
                COTW_LOG("[vr] layer fov now +/-%.1f h, +/-%.1f v deg  (game fov %.1f %s, "
                         "image aspect %.3f)",
                         fov.angleRight * deg, fov.angleUp * deg, Cfg().game_fov_deg,
                         Cfg().fov_is_horizontal ? "HORIZONTAL" : "vertical",
                         float(m_width) / float(m_height));
            }

            // Likewise the pose: with identical content in both eyes there is no
            // disparity to carry, so separating the eye positions by the IPD only
            // shifts one copy sideways against the other. Both eyes get the
            // midpoint. (Once each eye is genuinely rendered from its own
            // viewpoint, per-eye positions come back.)
            // *** THE POSE THE IMAGE IN OUR HANDS WAS ACTUALLY RENDERED FROM. ***
            //
            // Order inside the Present hook is SubmitFrame (render_hook.cpp:109)
            // and then HeadTrackTick (render_hook.cpp:148). So the angles that
            // rotated the game camera for the frame being submitted right now
            // came from the pose located during the PREVIOUS present - the value
            // sitting in m_headPose at this instant, just before it is replaced.
            //
            // Submitting the freshly located pose instead tells the compositor
            // "this was rendered from here" about a pose the image never saw. Its
            // reprojection then starts from the wrong place, by exactly one frame
            // of head motion - nothing at all while the head is still, and worse
            // the faster it turns. That is judder that scales with head speed.
            //
            // Handing it the pose the frame really was rendered from lets it do
            // its job properly, and it corrects the staleness for free.
            const XrPosef renderedFrom = m_headPose;
            const bool haveRenderedFrom = InterlockedCompareExchange(&m_headValid, 1, 1) != 0;

            // Cache the head pose for the game thread. The camera hook runs
            // deep inside the engine where calling OpenXR would be unsafe, so
            // it reads this instead.
            m_headPose = views[0].pose;
            m_headPose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
            m_headPose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
            m_headPose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;
            InterlockedExchange(&m_headValid, 1);

            XrPosef monoPose = views[0].pose;
            monoPose.position.x = (views[0].pose.position.x + views[1].pose.position.x) * 0.5f;
            monoPose.position.y = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f;
            monoPose.position.z = (views[0].pose.position.z + views[1].pose.position.z) * 0.5f;

            static bool loggedFov = false;
            if (!loggedFov) {
                loggedFov = true;
                const float deg = 57.2957795f;
                COTW_LOG("[vr] runtime eye0 fov L%.1f R%.1f U%.1f D%.1f deg",
                         views[0].fov.angleLeft * deg, views[0].fov.angleRight * deg,
                         views[0].fov.angleUp * deg, views[0].fov.angleDown * deg);
                COTW_LOG("[vr] runtime eye1 fov L%.1f R%.1f U%.1f D%.1f deg",
                         views[1].fov.angleLeft * deg, views[1].fov.angleRight * deg,
                         views[1].fov.angleUp * deg, views[1].fov.angleDown * deg);
                COTW_LOG("[vr] submitting with symmetric fov +/-%.1f h, +/-%.1f v deg "
                         "(same for both eyes) - asymmetric per-eye frusta are what "
                         "made one image read as two",
                         fov.angleRight * deg, fov.angleUp * deg);
            }

            for (int eye = 0; eye < 2; ++eye) {
                // Fast path: the eye that was NOT rendered for this frame keeps
                // the image already sitting in its swapchain. Skipping it saves
                // a crop-sized blit AND an xrWaitSwapchainImage, which can block
                // on the runtime's own pipeline depth.
                const bool skipUpload = aerFast && eye != CurrentRenderEye();

                if (!skipUpload) {
                    uint32_t index = 0;
                    XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                    if (XR_FAILED(xrAcquireSwapchainImage(m_eyes[eye].handle, &acq, &index))) break;

                    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    wait.timeout = XR_INFINITE_DURATION;
                    if (XR_FAILED(xrWaitSwapchainImage(m_eyes[eye].handle, &wait))) break;

                    ID3D11Texture2D* dst = m_eyes[eye].images[index];
                    if (stereo) {
                        // The hold textures are already cropped, so this is a
                        // straight full-surface copy - no box, no per-pixel
                        // offset maths on the GPU.
                        //
                        // Unless sharpening or saturation is asked for, in which
                        // case the same move happens through a shader instead:
                        // it reads the hold and writes the swapchain image, so
                        // it REPLACES the copy rather than adding a pass. Any
                        // failure falls straight back to the copy.
                        const uint32_t pw = m_holdW[eye] ? m_holdW[eye] : m_outW;
                        const uint32_t ph = m_holdH[eye] ? m_holdH[eye] : m_outH;
                        // The swapchain images are TYPELESS, so the pass has to be
                        // told which concrete format we asked the runtime for -
                        // only this side knows.
                        if (!PostProcessApply(m_device, m_context, m_hold[eye], dst,
                                              pw, ph,
                                              (DXGI_FORMAT)m_swapchainFormat)) {
                            m_context->CopyResource(dst, m_hold[eye]);
                        }
                    } else if (haveBack) {
                        // Crop straight out of the backbuffer. Copying the whole
                        // thing is what turned a 2.667:1 render into a letterbox
                        // strip in the headset.
                        //
                        // Unless DLSS is upscaling, in which case even this mono
                        // path has to go via a hold texture: the swapchain image
                        // has no UNORDERED_ACCESS bind, and NGX writes its output
                        // through a UAV.
                        //
                        // *** OR SHARPENING/SATURATION IS ON. ***
                        //
                        // This branch is not only the mono path - it is also the
                        // one the FAST alternate-eye path takes, because that path
                        // deliberately skips the hold texture to save two blits.
                        // The post-process pass reads a whole texture and writes a
                        // whole texture, so with no hold there was nothing for it
                        // to read and it was simply never called: both sliders
                        // moved and nothing changed, in the mode the mod ships in.
                        // Asking for either one now buys the hold back, which is
                        // exactly the cost the fast path was avoiding - so it is
                        // only paid when a slider is actually off its default.
                        const uint32_t cropW = srcBox.right - srcBox.left;
                        const uint32_t cropH = srcBox.bottom - srcBox.top;
                        uint32_t mw = 0, mh = 0;
                        UpscaleTarget(cropW, cropH, &mw, &mh);
                        const bool wantPost = PostProcessActive();
                        if ((mw != cropW || wantPost) && StashInto(eye, back, srcBox)) {
                            const uint32_t pw = m_holdW[eye] ? m_holdW[eye] : cropW;
                            const uint32_t ph = m_holdH[eye] ? m_holdH[eye] : cropH;
                            if (!PostProcessApply(m_device, m_context, m_hold[eye], dst,
                                                  pw, ph,
                                                  (DXGI_FORMAT)m_swapchainFormat)) {
                                m_context->CopyResource(dst, m_hold[eye]);
                            }
                        } else {
                            m_context->CopySubresourceRegion(dst, 0, 0, 0, 0, back, 0, &srcBox);
                        }
                    } else {
                        // Checkpoint 3: solid colour, proving the runtime path
                        // with no game imagery involved.
                        ID3D11RenderTargetView* rtv = nullptr;
                        if (SUCCEEDED(m_device->CreateRenderTargetView(dst, nullptr, &rtv))) {
                            const float colour[4] = {eye == 0 ? 0.15f : 0.05f, 0.10f, 0.35f, 1.0f};
                            m_context->ClearRenderTargetView(rtv, colour);
                            rtv->Release();
                        }
                    }

                    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage(m_eyes[eye].handle, &rel);
                }

                projViews[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
                // *** EACH EYE STATES THE POSE ITS OWN PIXELS CAME FROM. ***
                //
                // The frustum stays shared - per-eye FOVs actively break fusion
                // when both eyes show one rendered image. The POSE is different,
                // and in alternate-eye mode it has to be.
                //
                // Under AER only one eye is redrawn each frame; the other keeps
                // the image it was last given, which is a frame older. Submitting
                // one pose for both told the compositor that the stale eye was
                // drawn from the CURRENT viewpoint. Its reprojection was then
                // right for the fresh eye and wrong by a frame of head motion for
                // the other - and because the eyes alternate, that error swapped
                // sides every frame. That is the wobble, and it grows with how
                // fast the head turns.
                //
                // OpenXR gives every projection view its own pose for exactly
                // this. Remembering what each eye was drawn with lets the runtime
                // reproject each one from its true origin, which is what corrects
                // the stale eye instead of smearing it.
                if (!skipUpload) {
                    m_eyeRenderPose[eye] = haveRenderedFrom ? renderedFrom : monoPose;
                    m_haveEyePose[eye] = true;
                }
                const bool perEye = Cfg().aer_per_eye_pose && m_haveEyePose[eye];
                projViews[eye].pose =
                    perEye ? m_eyeRenderPose[eye]
                           : ((Cfg().submit_rendered_pose && haveRenderedFrom)
                                  ? renderedFrom : monoPose);
                // *** THE DECLARED POSITION MUST MATCH THE PIXELS. ***
                //
                // The engine is rotated by the head but never MOVED by it, so
                // a live position here asks the compositor to reproject for
                // translation the image does not contain - tracker noise
                // becomes a visible shimmer of the whole picture, in the
                // headset only and in both eyes, while the desktop mirror
                // stays clean. Freeze what we promise; rotation stays live so
                // rotational timewarp still works, and the eye separation is
                // untouched because it is baked into the images.
                if (Cfg().submit_frozen_position) {
                    if (!m_haveFrozenPos) {
                        m_frozenPos = projViews[eye].pose.position;
                        m_haveFrozenPos = true;
                    }
                    projViews[eye].pose.position = m_frozenPos;
                }
                // The diagnostic half: freeze the orientation too, so the
                // compositor stops warping and the head response comes ONLY
                // from the engine. See config.h - a test, not a setting.
                if (Cfg().submit_frozen_orientation) {
                    if (!m_haveFrozenOri) {
                        m_frozenOri = projViews[eye].pose.orientation;
                        m_haveFrozenOri = true;
                    }
                    projViews[eye].pose.orientation = m_frozenOri;
                }
                // The fov that matches the pixels this eye is actually being
                // given. Falls back to the shared symmetric one whenever the
                // per-eye crop is off or has not been computed yet.
                // *** THE ANGLES MUST DESCRIBE THE PIXELS THAT WERE SENT. ***
                //
                // When the upscale has latched a rectangle, the pixels in this
                // swapchain are that rectangle - so the frustum submitted has to
                // be the one latched with it, not whatever the crop has drifted
                // to since. Mixing them presents each eye at a slightly wrong
                // angle, by a different amount per eye, which is double vision.
                const bool latched =
                    Cfg().dlss_enable && Cfg().dlss_upscale_rect_latch &&
                    m_upRectW[eye] && m_haveUpFov[eye];
                projViews[eye].fov =
                    latched ? m_upFov[eye]
                            : ((Cfg().tier1_per_eye_crop && m_haveEyeCrop)
                                   ? m_eyeFov[eye] : fov);
                projViews[eye].subImage.swapchain = m_eyes[eye].handle;
                projViews[eye].subImage.imageRect.offset = {0, 0};
                // The eye's own rectangle, copied to (0,0) of a full-size
                // swapchain. Submitting the extent - rather than resizing the
                // swapchain - is what lets the crop follow the measured FOV.
                const bool perEyeRect = Cfg().tier1_per_eye_crop && m_haveEyeCrop;
                // *** THE EXTENT IS WHAT WAS WRITTEN, NOT WHAT WAS INTENDED. ***
                //
                // m_holdW/m_holdH are set by the capture and are the upscaled
                // size only when the upscale actually ran. On a frame where DLSS
                // declined they are the render size, and this submits the render
                // size - so a fallback looks like a slightly softer frame rather
                // than a magnified corner.
                const bool haveWritten = m_holdW[eye] && m_holdH[eye];
                projViews[eye].subImage.imageRect.extent =
                    haveWritten
                        ? XrExtent2Di{(int32_t)m_holdW[eye], (int32_t)m_holdH[eye]}
                        : (perEyeRect
                               ? XrExtent2Di{(int32_t)m_eyeCropW[eye], (int32_t)m_eyeCropH[eye]}
                               : XrExtent2Di{(int32_t)m_width, (int32_t)m_height});
                projViews[eye].subImage.imageArrayIndex = 0;
                haveLayer = true;
            }

            // The flat screen takes the WHOLE backbuffer, not the crop - seeing
            // the parts of the menu the crop cuts off is the entire point.
            //
            // THIS PLACEMENT IS THE FIX FOR LAST TIME. There are two
            // `if (back) back->Release();` sites in this function; the other one
            // is inside the "could not create eye swapchains" branch, which only
            // runs while VR is shutting down. The first attempt was attached to
            // that one and therefore never executed once - the log had no
            // [screen] line at all while the feature looked switched on. The
            // anchor is the comment below, not the release on its own.
            LogMenuSignals();
            const ScreenReason reason = MenuScreenReason();
            if (reason != ScreenReason::None && haveBack && back &&
                EnsureMenuScreen(back)) {
                uint32_t si = 0;
                XrSwapchainImageAcquireInfo sa{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
                if (XR_SUCCEEDED(xrAcquireSwapchainImage(m_screenSwapchain, &sa, &si))) {
                    XrSwapchainImageWaitInfo sw{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
                    sw.timeout = XR_INFINITE_DURATION;
                    if (XR_SUCCEEDED(xrWaitSwapchainImage(m_screenSwapchain, &sw)) &&
                        si < m_screenImages.size()) {
                        m_context->CopyResource(m_screenImages[si], back);
                        haveScreen = true;
                        // ALWAYS hide the world behind the panel, whatever put
                        // the panel up.
                        //
                        // The first version only hid it for the main menu, on
                        // the theory that keeping the world visible during an
                        // in-game menu was the safer choice. It is not. What is
                        // behind the panel IS the same image the panel shows,
                        // wrapped across the entire field of view - so you look
                        // at the game twice, at two sizes, one inside the other.
                        // That is exactly what it looked like, and nobody wants
                        // it. One panel, nothing behind it. That is the feature.
                        hideWorldForScreen = Cfg().menu_screen_hide_world;
                    }
                    XrSwapchainImageReleaseInfo sr{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage(m_screenSwapchain, &sr);
                }
            }
            // Say what happened, once per change of reason. A feature that
            // silently does nothing cost two whole test sessions to tell apart
            // from one that ran and did not help.
            if (reason != m_lastScreenReason) {
                m_lastScreenReason = reason;
                static const char* kWhy[] = {"off", "the hotkey/toggle",
                                             "the main menu (no player camera)",
                                             "an in-game menu (mouse cursor showing)"};
                COTW_LOG("[screen] flat panel %s - reason: %s",
                         reason == ScreenReason::None ? "hidden"
                             : (haveScreen ? "SHOWING" : "wanted but NOT prepared"),
                         kWhy[(int)reason]);
            }

            if (back) back->Release();

            // Alternate-eye flips here so the eye the engine drew for and the
            // eye we deposited it into stay in step. Full-rate sets the eye
            // explicitly around each render pass, so it must NOT flip.
            if ((aerFast || stereo) && !FullRateActive()) AdvanceRenderEye();
        }

        if (haveLayer) {
            layer.space = m_space;
            layer.viewCount = 2;
            layer.views = projViews;
            ++m_submitted;
        }
    }

    // --- the flat screen, between the world and the settings panel ---
    XrCompositionLayerQuad screenQuad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    if (haveScreen && m_screenW && m_screenH) {
        const float wM = Cfg().menu_screen_width_m;
        const float hM = wM * float(m_screenH) / float(m_screenW);
        screenQuad.layerFlags = 0;      // opaque - the whole panel is the image
        screenQuad.space = m_viewSpace; // head-locked, so it cannot be lost
        screenQuad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        screenQuad.subImage.swapchain = m_screenSwapchain;
        screenQuad.subImage.imageRect.offset = {0, 0};
        screenQuad.subImage.imageRect.extent = {(int32_t)m_screenW, (int32_t)m_screenH};
        screenQuad.subImage.imageArrayIndex = 0;
        screenQuad.pose.orientation = {0, 0, 0, 1};
        screenQuad.pose.position = {0.0f, 0.0f, -Cfg().menu_screen_distance_m};
        screenQuad.size = {wM, hM};
    } else {
        haveScreen = false;
        hideWorldForScreen = false;     // never blank the world with nothing to show
    }

    // --- the settings panel, composited over the world ---
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    bool haveQuad = false;

    if (frameState.shouldRender && g_overlay.Visible() &&
        m_overlaySwapchain != XR_NULL_HANDLE && !m_overlayImages.empty()) {
        uint32_t index = 0;
        XrSwapchainImageAcquireInfo acq{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(m_overlaySwapchain, &acq, &index))) {
            XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
            wait.timeout = XR_INFINITE_DURATION;
            if (XR_SUCCEEDED(xrWaitSwapchainImage(m_overlaySwapchain, &wait))) {
                // Only re-upload when the bitmap actually changed - most frames
                // it has not, and a 2.7 MB upload every frame for a static panel
                // is pure waste.
                if (g_overlay.TakeDirty() && m_overlayStaging) {
                    m_context->UpdateSubresource(m_overlayStaging, 0, nullptr,
                                                 g_overlay.Pixels(),
                                                 g_overlay.Width() * 4, 0);
                }
                if (m_overlayStaging) {
                    m_context->CopyResource(m_overlayImages[index], m_overlayStaging);
                }
            }
            XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            xrReleaseSwapchainImage(m_overlaySwapchain, &rel);

            const float wM = g_overlay.WidthMetres();
            const float hM = wM * float(g_overlay.Height()) / float(g_overlay.Width());

            quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.space = m_viewSpace;          // head-locked
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = m_overlaySwapchain;
            quad.subImage.imageRect.offset = {0, 0};
            quad.subImage.imageRect.extent = {g_overlay.Width(), g_overlay.Height()};
            quad.subImage.imageArrayIndex = 0;
            quad.pose.orientation = {0, 0, 0, 1};
            quad.pose.position = {0.0f, 0.0f, -g_overlay.DistanceMetres()};
            quad.size = {wM, hM};
            haveQuad = true;
        }
    }

    // Back to front: the world, then the flat screen, then the settings panel.
    // A quad only sits on top of what was submitted before it.
    const XrCompositionLayerBaseHeader* layers[3];
    uint32_t layerCount = 0;
    if (haveLayer && !hideWorldForScreen) {
        layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer);
    }
    if (haveScreen) {
        layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&screenQuad);
    }
    if (haveQuad) {
        layers[layerCount++] = reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad);
    }

    const double tWorked = timing ? NowMs() : 0.0;

    // Begin and End must balance exactly, every frame, whether or not anything
    // was rendered - an unbalanced pair desynchronises the runtime for good.
    XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO};
    end.displayTime = frameState.predictedDisplayTime;
    end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    end.layerCount = layerCount;
    end.layers = layerCount ? layers : nullptr;
    r = xrEndFrame(m_session, &end);
    if (XR_FAILED(r)) {
        static int complained = 0;
        if (complained++ < 10) COTW_LOG("[vr] xrEndFrame failed: %d %s", (int)r, XrStr(r));
    }

    if (timing) {
        // Accumulated, not instantaneous: a single frame tells you nothing, and
        // averaging in the log costs one add per frame.
        const double now = NowMs();
        m_perfWaitMs += tWaited - tStart;      // runtime pacing us to the headset
        m_perfWorkMs += tWorked - tWaited;     // OUR cost: locate + the blits
        m_perfEndMs  += now - tWorked;         // handing the layers over
        if (m_perfPrevFrameMs > 0.0) m_perfFrameMs += tStart - m_perfPrevFrameMs;
        m_perfPrevFrameMs = tStart;
        ++m_perfSamples;

        if (m_perfSamples >= (uint64_t)Cfg().perf_log_every) {
            const double n = double(m_perfSamples);
            const double frame = m_perfFrameMs / n;
            // The engine's own idea of how long a frame took, next to how long
            // it ACTUALLY took. These must agree. If the engine's number is
            // half of reality then every time-based thing in the game - walking
            // speed, animations, reload timers - runs at half speed, and the
            // game's fps counter reads double for the same reason.
            const float engineDt = EngineFrameDelta();
            char clock[128] = "";
            if (engineDt > 0.0f) {
                const double engineMs = double(engineDt) * 1000.0;
                snprintf(clock, sizeof(clock),
                         " | engine thinks %.2f ms (%.0f fps) = %.2fx reality%s",
                         engineMs, 1000.0 / engineMs, engineMs / frame,
                         (engineMs < frame * 0.75 || engineMs > frame * 1.33)
                             ? "  <-- WRONG, the game clock is off" : "");
            }
            COTW_LOG("[perf] %.0f fps | frame %.2f ms = wait %.2f + OUR WORK %.2f + "
                     "submit %.2f + game %.2f | %s, eye %ux%u from %ux%u%s",
                     frame > 0.0 ? 1000.0 / frame : 0.0, frame,
                     m_perfWaitMs / n, m_perfWorkMs / n, m_perfEndMs / n,
                     frame - (m_perfWaitMs + m_perfWorkMs + m_perfEndMs) / n,
                     m_lastPathName, m_width, m_height, m_srcWidth, m_srcHeight,
                     clock);
            m_perfWaitMs = m_perfWorkMs = m_perfEndMs = m_perfFrameMs = 0.0;
            m_perfSamples = 0;
        }
    } else {
        m_perfPrevFrameMs = 0.0;
    }

    const int every = Cfg().log_frame_every;
    if (every > 0 && (m_frames % (uint64_t)every) == 0) {
        COTW_LOG("[vr] frame %llu state=%s submitted=%llu",
                 (unsigned long long)m_frames, StateName(),
                 (unsigned long long)m_submitted);
    }
}

}  // namespace cotwvr
