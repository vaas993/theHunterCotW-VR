#include "dlss.h"

#include "config.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <string>

#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>

namespace cotwvr {
namespace {

// The application id NGX is initialised with. 0x0 is not valid; NVIDIA hand out
// per-title ids, and unregistered projects use the generic path with their own
// number. Any stable non-zero value works for local use.
constexpr unsigned long long kAppId = 0x434F5457ull;   // 'COTW'

bool g_inited = false;
NVSDK_NGX_Parameter* g_caps = nullptr;
// Kept for shutdown: this SDK's plain Shutdown() is behind
// NGX_ENABLE_DEPRECATED_SHUTDOWN, and Shutdown1 wants the device it was
// initialised with. Holding a reference means it cannot vanish underneath us.
ID3D11Device* g_dev = nullptr;

template <class T>
struct Rel {
    T* p = nullptr;
    ~Rel() { if (p) p->Release(); }
    T** Put() { return &p; }
    T* Get() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

const char* ResultName(NVSDK_NGX_Result r) {
    switch (r) {
        case NVSDK_NGX_Result_Success:                       return "Success";
        case NVSDK_NGX_Result_Fail:                          return "Fail";
        case NVSDK_NGX_Result_FAIL_FeatureNotSupported:      return "FeatureNotSupported";
        case NVSDK_NGX_Result_FAIL_PlatformError:            return "PlatformError";
        case NVSDK_NGX_Result_FAIL_FeatureAlreadyExists:     return "FeatureAlreadyExists";
        case NVSDK_NGX_Result_FAIL_FeatureNotFound:          return "FeatureNotFound";
        case NVSDK_NGX_Result_FAIL_InvalidParameter:         return "InvalidParameter";
        case NVSDK_NGX_Result_FAIL_ScratchBufferTooSmall:    return "ScratchBufferTooSmall";
        case NVSDK_NGX_Result_FAIL_NotInitialized:           return "NotInitialized";
        case NVSDK_NGX_Result_FAIL_UnsupportedInputFormat:   return "UnsupportedInputFormat";
        case NVSDK_NGX_Result_FAIL_RWFlagMissing:            return "RWFlagMissing";
        case NVSDK_NGX_Result_FAIL_MissingInput:             return "MissingInput";
        case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:return "UnableToInitializeFeature";
        case NVSDK_NGX_Result_FAIL_OutOfDate:                return "OutOfDate";
        case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:           return "OutOfGPUMemory";
        case NVSDK_NGX_Result_FAIL_UnsupportedFormat:        return "UnsupportedFormat";
        case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath: return "UnableToWriteToAppDataPath";
        case NVSDK_NGX_Result_FAIL_UnsupportedParameter:     return "UnsupportedParameter";
        case NVSDK_NGX_Result_FAIL_Denied:                   return "Denied";
        case NVSDK_NGX_Result_FAIL_NotImplemented:           return "NotImplemented";
        default:                                             return "unknown";
    }
}

// NGX looks for nvngx_dlss.dll beside the executable by default. The mod ships
// its own copy next to the game, so the game folder is the right place to point
// it at - and saying which path was used makes a "runtime not found" failure
// readable instead of mysterious.
std::wstring GameFolder() {
    wchar_t buf[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, buf, MAX_PATH)) return L".";
    std::wstring p(buf);
    const size_t cut = p.find_last_of(L'\\');
    return (cut == std::wstring::npos) ? L"." : p.substr(0, cut);
}

// --- one feature per eye ----------------------------------------------------

bool g_available = false;
NVSDK_NGX_Handle*    g_feature[2] = {nullptr, nullptr};
NVSDK_NGX_Parameter* g_params[2]  = {nullptr, nullptr};
unsigned g_featW = 0, g_featH = 0;
bool g_createFailed = false;

// DLSS writes through a UAV, and the engine's colour target is bound
// SRV|RENDER_TARGET with no unordered access - so it cannot be written into
// directly. One texture of our own with UAV, then a straight copy into the
// engine's target, which is what Luma does for Just Cause 3 for the same
// reason. The copy is full-res per eye; if it ever shows in frametimes the
// alternative is redirecting the seven downstream consumers, which is a far
// larger surface.
ID3D11Texture2D* g_out[2] = {nullptr, nullptr};

bool EnsureOutput(ID3D11Device* dev, int eye, unsigned w, unsigned h,
                  DXGI_FORMAT fmt) {
    if (g_out[eye]) return true;
    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE |
                  D3D11_BIND_RENDER_TARGET;
    if (FAILED(dev->CreateTexture2D(&d, nullptr, &g_out[eye])) || !g_out[eye]) {
        g_out[eye] = nullptr;
        COTW_LOG("[dlss] could not create the UAV output texture for eye %d "
                 "(%ux%u fmt %u) - DLSS cannot write anywhere.", eye, w, h,
                 (unsigned)fmt);
        return false;
    }
    return true;
}

// The panel's preset choice (0..4) mapped to the NGX enum. Non-contiguous on
// purpose: A-D are removed from this SDK, E/F deprecated, G-I reserved - the
// live values in 310.x are J=10, K=11, L=12, M=13.
constexpr unsigned kPresetValue[5] = {0, 10, 11, 12, 13};
constexpr const char* kPresetName[5] = {"default(K)", "J", "K", "L", "M"};

unsigned CreateFlagsNow() {
    // HDR because the target is R11G11B10_FLOAT and runs well past 1.0.
    // MVLowRes because our motion vectors are at render resolution (for DLAA
    // also the output resolution) - stated explicitly rather than left to a
    // default. The rest are the research sweep's corrections, each with its own
    // switch so a misdiagnosis is a toggle rather than a build:
    //  - AutoExposure: the first build declared HDR and supplied no exposure at
    //    all; the guide lists "ghosting of moving objects" as symptom #1 of
    //    exactly that. Luma runs DLSS with auto-exposure on this engine.
    //  - MVJittered: our vectors are built from the engine's jittered matrices,
    //    so they carry the frame-to-frame jitter difference; declaring it lets
    //    DLSS subtract the reported offsets itself (guide 3.6.2).
    //  - DepthInverted: Apex is reversed-Z, and Luma sets this for Just Cause 3.
    //    Wrong depth direction corrupts the closest-depth dilation of the
    //    motion vectors, which is moving-edge quality.
    unsigned f = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                 NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (Cfg().dlss_auto_exposure)  f |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    // With the resolve's de-jitter on, the motion vectors are CLEAN - built
    // from jitter-stripped matrices at an un-jittered sample position - so
    // declaring them jittered would make DLSS subtract offsets that are no
    // longer in them.
    if (Cfg().dlss_mv_jittered && !Cfg().taa_dejitter)
        f |= NVSDK_NGX_DLSS_Feature_Flags_MVJittered;
    if (Cfg().dlss_depth_inverted) f |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    return f;
}

unsigned g_createSig = 0xFFFFFFFFu;

bool EnsureFeature(ID3D11DeviceContext* ctx, int eye, unsigned w, unsigned h) {
    const int presetIdx =
        (Cfg().dlss_preset < 0) ? 0 : (Cfg().dlss_preset > 4 ? 4 : Cfg().dlss_preset);
    // The create-time inputs, folded into one signature. Flags and preset are
    // baked into a feature at creation ("the Preset chosen ... will persist
    // across the lifetime of the feature"), so changing any of them in the
    // panel must rebuild BOTH eyes - and a previous create failure is worth
    // retrying once the settings that caused it have changed.
    const unsigned sig = CreateFlagsNow() | ((unsigned)presetIdx << 24);
    if (g_createSig != sig) {
        for (int e = 0; e < 2; ++e) {
            if (g_feature[e]) { NVSDK_NGX_D3D11_ReleaseFeature(g_feature[e]); g_feature[e] = nullptr; }
            if (g_params[e])  { NVSDK_NGX_D3D11_DestroyParameters(g_params[e]); g_params[e] = nullptr; }
        }
        g_createSig = sig;
        g_createFailed = false;
    }

    if (g_feature[eye] && g_featW == w && g_featH == h) return true;
    if (g_createFailed) return false;

    // A resolution change invalidates both features, not just this eye's.
    if (g_featW != w || g_featH != h) {
        for (int e = 0; e < 2; ++e) {
            if (g_feature[e]) { NVSDK_NGX_D3D11_ReleaseFeature(g_feature[e]); g_feature[e] = nullptr; }
            if (g_params[e])  { NVSDK_NGX_D3D11_DestroyParameters(g_params[e]); g_params[e] = nullptr; }
            if (g_out[e])     { g_out[e]->Release(); g_out[e] = nullptr; }
        }
        g_featW = w; g_featH = h;
    }

    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_AllocateParameters(&g_params[eye]);
    if (NVSDK_NGX_FAILED(r) || !g_params[eye]) {
        COTW_LOG("[dlss] eye %d: could not allocate parameters: %s", eye,
                 ResultName(r));
        g_createFailed = true;
        return false;
    }

    // The render preset is a hint on the PARAMETER object, read at feature
    // creation and fixed for the feature's lifetime. The DLAA slot is the one
    // that applies (in==out size); the Quality slot is set too in case a future
    // build creates at a ratio.
    if (kPresetValue[presetIdx] != 0) {
        NVSDK_NGX_Parameter_SetUI(g_params[eye],
                                  NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                                  kPresetValue[presetIdx]);
        NVSDK_NGX_Parameter_SetUI(g_params[eye],
                                  NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                  kPresetValue[presetIdx]);
    }

    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth        = w;
    cp.Feature.InHeight       = h;
    cp.Feature.InTargetWidth  = w;    // DLAA: render size IS the output size
    cp.Feature.InTargetHeight = h;
    cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
    cp.InFeatureCreateFlags = CreateFlagsNow();

    r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_feature[eye], g_params[eye], &cp);
    if (NVSDK_NGX_FAILED(r) || !g_feature[eye]) {
        COTW_LOG("[dlss] eye %d: CreateFeature FAILED: %s (0x%08X)", eye,
                 ResultName(r), (unsigned)r);
        g_feature[eye] = nullptr;
        g_createFailed = true;
        return false;
    }
    COTW_LOG("[dlss] eye %d: DLAA feature created at %ux%u, preset %s, flags "
             "HDR|MVLowRes%s%s%s.", eye, w, h, kPresetName[presetIdx],
             Cfg().dlss_auto_exposure ? "|AutoExposure" : "",
             Cfg().dlss_mv_jittered ? "|MVJittered" : "",
             Cfg().dlss_depth_inverted ? "|DepthInverted" : "");
    return true;
}

}  // namespace

bool DlssAvailable() { return g_available; }

bool DlssEvaluate(ID3D11DeviceContext* ctx, int eye,
                  ID3D11Resource* colour, ID3D11Resource* depth,
                  ID3D11Resource* motion, ID3D11Resource* target,
                  unsigned w, unsigned h, float jitterX, float jitterY,
                  bool reset) {
    if (!g_available || !ctx || !colour || !depth || !motion || !target) return false;
    eye = (eye == 0) ? 0 : 1;
    if (!EnsureFeature(ctx, eye, w, h)) return false;

    // The output has to match the target's format, since the result is copied
    // straight back into it.
    D3D11_TEXTURE2D_DESC td{};
    {
        ID3D11Texture2D* t = nullptr;
        if (FAILED(target->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t)
            return false;
        t->GetDesc(&td);
        t->Release();
    }
    Rel<ID3D11Device> dev;
    ctx->GetDevice(dev.Put());
    if (!dev || !EnsureOutput(dev.Get(), eye, w, h, td.Format)) return false;

    NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
    ep.Feature.pInColor  = colour;
    ep.Feature.pInOutput = g_out[eye];
    ep.pInDepth          = depth;
    ep.pInMotionVectors  = motion;
    ep.InJitterOffsetX   = jitterX;
    ep.InJitterOffsetY   = jitterY;
    // Our vectors are already in PIXELS, current -> previous, so no rescaling.
    ep.InMVScaleX        = 1.0f;
    ep.InMVScaleY        = 1.0f;
    ep.InRenderSubrectDimensions.Width  = w;
    ep.InRenderSubrectDimensions.Height = h;
    ep.InReset           = reset ? 1 : 0;

    const NVSDK_NGX_Result r =
        NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g_feature[eye], g_params[eye], &ep);
    if (NVSDK_NGX_FAILED(r)) {
        static int moaned = 0;
        if (moaned < 3) {
            ++moaned;
            COTW_LOG("[dlss] eye %d: Evaluate FAILED: %s (0x%08X) - falling back "
                     "to the mod's own resolve for this frame.", eye,
                     ResultName(r), (unsigned)r);
        }
        return false;
    }

    // Into the texture the engine's seven downstream consumers already read.
    ctx->CopyResource(target, g_out[eye]);
    return true;
}

// --- UPSCALING: a second feature set, in != out -----------------------------
//
// Kept entirely separate from the DLAA features above. They are created at
// different sizes, they own different histories, and mixing them would mean
// releasing and recreating a feature every time the caller changed its mind -
// which is the one thing NGX is expensive at.
namespace {
NVSDK_NGX_Handle*    g_upFeature[2] = {nullptr, nullptr};
NVSDK_NGX_Parameter* g_upParams[2]  = {nullptr, nullptr};
// *** SIZES ARE PER EYE, AND THAT IS NOT PEDANTRY. ***
//
// With tier1_per_eye_crop on, the two eyes' crops differ by a few dozen pixels
// because they are cut to each eye's own measured frustum. A single shared size
// here would see "the size changed" on every single eye swap, tear both
// features down and rebuild them - twice a frame, at NGX's build cost. Each eye
// remembers its own.
unsigned g_upInW[2] = {0, 0}, g_upInH[2] = {0, 0};
unsigned g_upOutW[2] = {0, 0}, g_upOutH[2] = {0, 0};
int  g_upPreset[2] = {-1, -1};
bool g_upFailed = false;

bool EnsureUpscaleFeature(ID3D11DeviceContext* ctx, int eye, unsigned inW,
                          unsigned inH, unsigned outW, unsigned outH) {
    const int presetNow =
        (Cfg().dlss_preset < 0) ? 0 : (Cfg().dlss_preset > 4 ? 4 : Cfg().dlss_preset);
    if (g_upInW[eye] != inW || g_upInH[eye] != inH || g_upOutW[eye] != outW ||
        g_upOutH[eye] != outH || g_upPreset[eye] != presetNow) {
        if (g_upFeature[eye]) { NVSDK_NGX_D3D11_ReleaseFeature(g_upFeature[eye]); g_upFeature[eye] = nullptr; }
        if (g_upParams[eye])  { NVSDK_NGX_D3D11_DestroyParameters(g_upParams[eye]); g_upParams[eye] = nullptr; }
        g_upInW[eye] = inW; g_upInH[eye] = inH;
        g_upOutW[eye] = outW; g_upOutH[eye] = outH;
        g_upPreset[eye] = presetNow;
        g_upFailed = false;
    }
    if (g_upFeature[eye]) return true;
    if (g_upFailed) return false;

    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_AllocateParameters(&g_upParams[eye]);
    if (NVSDK_NGX_FAILED(r) || !g_upParams[eye]) { g_upFailed = true; return false; }

    // Let NGX name the quality level for this ratio rather than assuming one:
    // the preset tables are keyed on it, and a mismatch silently costs quality.
    const float ratio = (outW > 0) ? (float)inW / (float)outW : 1.0f;
    NVSDK_NGX_PerfQuality_Value q = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    if (ratio <= 0.40f)      q = NVSDK_NGX_PerfQuality_Value_UltraPerformance;
    else if (ratio <= 0.52f) q = NVSDK_NGX_PerfQuality_Value_MaxPerf;
    else if (ratio <= 0.60f) q = NVSDK_NGX_PerfQuality_Value_Balanced;
    else if (ratio <= 0.70f) q = NVSDK_NGX_PerfQuality_Value_MaxQuality;
    else if (ratio >= 0.99f) q = NVSDK_NGX_PerfQuality_Value_DLAA;

    const int presetIdx = presetNow;
    if (kPresetValue[presetIdx] != 0) {
        // *** EVERY QUALITY LEVEL, NOT THREE OF THEM. ***
        //
        // The hint is per PerfQuality value, and only Quality/Balanced/
        // Performance were being set - so on DLAA or Ultra Performance the
        // preset row silently did nothing, because the level in force had no
        // hint attached. Five levels exist; all five get told.
        static const char* const kHintKeys[] = {
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
        };
        for (const char* key : kHintKeys) {
            NVSDK_NGX_Parameter_SetUI(g_upParams[eye], key, kPresetValue[presetIdx]);
        }
    }

    NVSDK_NGX_DLSS_Create_Params cp{};
    cp.Feature.InWidth        = inW;
    cp.Feature.InHeight       = inH;
    cp.Feature.InTargetWidth  = outW;
    cp.Feature.InTargetHeight = outH;
    cp.Feature.InPerfQualityValue = q;
    // NOT IsHDR here: this input is the FINAL backbuffer, already tonemapped
    // to 8-bit - the opposite of the resolve's R11G11B10 float. Declaring HDR
    // on an LDR image is the same class of mistake as declaring nothing on an
    // HDR one, which cost this project a week.
    cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
                              (Cfg().dlss_auto_exposure
                                   ? NVSDK_NGX_DLSS_Feature_Flags_AutoExposure : 0) |
                              (Cfg().dlss_depth_inverted
                                   ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);

    r = NGX_D3D11_CREATE_DLSS_EXT(ctx, &g_upFeature[eye], g_upParams[eye], &cp);
    if (NVSDK_NGX_FAILED(r) || !g_upFeature[eye]) {
        COTW_LOG("[dlss] eye %d: UPSCALE feature FAILED at %ux%u -> %ux%u: %s",
                 eye, inW, inH, outW, outH, ResultName(r));
        g_upFeature[eye] = nullptr;
        g_upFailed = true;
        return false;
    }
    COTW_LOG("[dlss] eye %d: UPSCALE feature %ux%u -> %ux%u (%.0f%% of each axis, "
             "%.0f%% of the pixels), preset %s.", eye, inW, inH, outW, outH,
             ratio * 100.0f, ratio * ratio * 100.0f, kPresetName[presetIdx]);
    return true;
}
}  // namespace

// *** THROW THE HISTORY AWAY AND BUILD THE FEATURE AGAIN. ***
//
// Called when the output the upscaler writes into has been torn down and
// remade - switching DLSS off and on does exactly that, and the owner reported
// that the DLSS look did not come back afterwards while every counter insisted
// it was running. A feature carrying history that was accumulated against
// destroyed textures is the one explanation those two facts share, and it costs
// a single rebuild to remove from the list of possibilities.
void DlssUpscaleInvalidate() {
    for (int e = 0; e < 2; ++e) {
        if (g_upFeature[e]) { NVSDK_NGX_D3D11_ReleaseFeature(g_upFeature[e]); g_upFeature[e] = nullptr; }
        if (g_upParams[e])  { NVSDK_NGX_D3D11_DestroyParameters(g_upParams[e]); g_upParams[e] = nullptr; }
        g_upInW[e] = g_upInH[e] = g_upOutW[e] = g_upOutH[e] = 0;
        g_upPreset[e] = -1;
    }
    g_upFailed = false;
}

bool DlssUpscale(ID3D11DeviceContext* ctx, int eye,
                 ID3D11Resource* colour, ID3D11Resource* depth,
                 ID3D11Resource* motion, ID3D11Resource* out,
                 unsigned srcX, unsigned srcY, unsigned srcW, unsigned srcH,
                 unsigned outW, unsigned outH,
                 float jitterX, float jitterY, bool reset) {
    if (!g_available || !ctx || !colour || !depth || !motion || !out) return false;
    if (srcW < 64 || srcH < 64 || outW < srcW || outH < srcH) return false;
    eye = (eye == 0) ? 0 : 1;

    // *** THE THREE INPUTS MUST SHARE ONE COORDINATE SYSTEM. ***
    //
    // One subrect addresses colour, depth and motion vectors alike, which is
    // only meaningful if all three are the same size. They should be - the
    // depth and the vector target both belong to the render the backbuffer
    // came from - but "should be" is how the last four faults in this project
    // started, and a silent half-size depth buffer would look like a mystery
    // smear rather than a mistake. Checked once per size, then trusted.
    {
        auto dims = [](ID3D11Resource* r, unsigned* w, unsigned* h) {
            ID3D11Texture2D* t = nullptr;
            if (FAILED(r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) || !t)
                return false;
            D3D11_TEXTURE2D_DESC d{};
            t->GetDesc(&d);
            t->Release();
            *w = d.Width; *h = d.Height;
            return true;
        };
        unsigned cw = 0, ch = 0, dw = 0, dh = 0, mw = 0, mh = 0;
        if (!dims(colour, &cw, &ch) || !dims(depth, &dw, &dh) || !dims(motion, &mw, &mh))
            return false;
        if (dw != cw || dh != ch || mw != cw || mh != ch ||
            srcX + srcW > cw || srcY + srcH > ch) {
            static unsigned moanedW = 0;
            if (moanedW != cw) {
                moanedW = cw;
                COTW_LOG("[dlss] upscale declined: colour %ux%u, depth %ux%u, vectors "
                         "%ux%u, crop %u,%u %ux%u - one subrect cannot address three "
                         "different grids. The headset gets the plain copy.",
                         cw, ch, dw, dh, mw, mh, srcX, srcY, srcW, srcH);
            }
            return false;
        }
    }

    if (!EnsureUpscaleFeature(ctx, eye, srcW, srcH, outW, outH)) return false;

    NVSDK_NGX_D3D11_DLSS_Eval_Params ep{};
    ep.Feature.pInColor  = colour;
    ep.Feature.pInOutput = out;
    ep.pInDepth          = depth;
    ep.pInMotionVectors  = motion;
    ep.InJitterOffsetX   = jitterX;
    ep.InJitterOffsetY   = jitterY;
    ep.InMVScaleX        = 1.0f;      // ours are already pixels, cur -> prev
    ep.InMVScaleY        = 1.0f;
    // ONE subrect for every input: the eye's crop inside the full-size frame.
    // The output is written from its own origin, because the swapchain image
    // holds nothing else.
    ep.InColorSubrectBase = {srcX, srcY};
    ep.InDepthSubrectBase = {srcX, srcY};
    ep.InMVSubrectBase    = {srcX, srcY};
    ep.InRenderSubrectDimensions = {srcW, srcH};
    ep.InReset = reset ? 1 : 0;

    const NVSDK_NGX_Result r =
        NGX_D3D11_EVALUATE_DLSS_EXT(ctx, g_upFeature[eye], g_upParams[eye], &ep);
    if (NVSDK_NGX_FAILED(r)) {
        static int moaned = 0;
        if (moaned < 3) {
            ++moaned;
            COTW_LOG("[dlss] eye %d: UPSCALE evaluate FAILED: %s - the headset gets "
                     "the plain cropped copy for this frame.", eye, ResultName(r));
        }
        return false;
    }
    return true;
}

void DlssProbe(ID3D11Device* dev) {
    if (g_inited || !dev) return;
    g_inited = true;      // one attempt, whatever happens - a retry loop in the
                          // frame path is a stutter machine

    const std::wstring folder = GameFolder();
    COTW_LOG("[dlss] initialising NGX against the game's own device, data path %S",
             folder.c_str());

    NVSDK_NGX_Result r = NVSDK_NGX_D3D11_Init(kAppId, folder.c_str(), dev);
    if (NVSDK_NGX_FAILED(r)) {
        COTW_LOG("[dlss] NGX init FAILED: %s (0x%08X).", ResultName(r), (unsigned)r);
        if (r == NVSDK_NGX_Result_FAIL_FeatureNotSupported)
            COTW_LOG("[dlss]   the GPU or driver does not offer NGX at all.");
        else
            COTW_LOG("[dlss]   most often this is nvngx_dlss.dll not being found "
                     "beside the game - check it is in %S", folder.c_str());
        return;
    }
    COTW_LOG("[dlss] NGX initialised.");
    g_dev = dev;
    g_dev->AddRef();

    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_caps);
    if (NVSDK_NGX_FAILED(r) || !g_caps) {
        COTW_LOG("[dlss] could not read capabilities: %s", ResultName(r));
        return;
    }

    int available = 0;
    g_caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
    if (!available) {
        int needsDriver = 0, major = 0, minor = 0;
        unsigned int why = 0;
        g_caps->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needsDriver);
        g_caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major);
        g_caps->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor);
        g_caps->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, (int*)&why);
        if (needsDriver)
            COTW_LOG("[dlss] DLSS is NOT available: the driver is too old, %d.%d "
                     "or newer is required.", major, minor);
        else
            COTW_LOG("[dlss] DLSS is NOT available on this setup: %s (0x%08X).",
                     ResultName((NVSDK_NGX_Result)why), why);
        return;
    }

    g_available = true;
    COTW_LOG("[dlss] *** DLSS IS AVAILABLE. *** NGX is up inside the mod's own "
             "process, alongside our D3D hooks, the OpenXR session and the "
             "overlay - which was the step most likely to fail outright.");
    COTW_LOG("[dlss] Next: a motion-vector texture (we already compute the "
             "reprojection, it needs a render target), then one DLSS feature per "
             "eye, swapped by CurrentRenderEye().");
}

void DlssShutdown() {
    if (!g_inited) return;
    for (int e = 0; e < 2; ++e) {
        if (g_upFeature[e]) { NVSDK_NGX_D3D11_ReleaseFeature(g_upFeature[e]); g_upFeature[e] = nullptr; }
        if (g_upParams[e])  { NVSDK_NGX_D3D11_DestroyParameters(g_upParams[e]); g_upParams[e] = nullptr; }
    }
    for (int e = 0; e < 2; ++e) {
        if (g_feature[e]) { NVSDK_NGX_D3D11_ReleaseFeature(g_feature[e]); g_feature[e] = nullptr; }
        if (g_params[e])  { NVSDK_NGX_D3D11_DestroyParameters(g_params[e]); g_params[e] = nullptr; }
        if (g_out[e])     { g_out[e]->Release(); g_out[e] = nullptr; }
    }
    g_available = false;
    if (g_caps) {
        NVSDK_NGX_D3D11_DestroyParameters(g_caps);
        g_caps = nullptr;
    }
    if (g_dev) {
        NVSDK_NGX_D3D11_Shutdown1(g_dev);
        g_dev->Release();
        g_dev = nullptr;
    }
    g_inited = false;
}

}  // namespace cotwvr
