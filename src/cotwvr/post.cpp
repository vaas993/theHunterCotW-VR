#include "post.h"

#include "config.h"
#include "log.h"

#include <cstring>

namespace cotwvr {
namespace {

// A full-screen triangle from the vertex id - no vertex buffer, no input
// layout, nothing to restore afterwards but the pipeline state itself.
const char kPostVS[] = R"(
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VSOut main(uint id : SV_VertexID) {
    VSOut o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.uv  = t;
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
)";

// *** RCAS, AND WHY IT RATHER THAN A PLAIN UNSHARP MASK. ***
//
// A naive sharpener adds a scaled Laplacian and rings: a bright halo on every
// trunk and antler against the sky, which in a headset reads as an artefact
// rather than as detail. RCAS (AMD's contrast-adaptive sharpener, the one
// designed to run after an upscaler) computes how much lobe each pixel can take
// WITHOUT pushing it outside the range of its own neighbours, so it cannot
// overshoot into a halo. Same reason Halo MCC VR uses it on its final image.
//
// The lobe is driven by the green channel, as RCAS does - green carries most of
// the luminance, and one shared lobe keeps the channels from separating into
// colour fringes.
const char kPostPS[] = R"(
Texture2D    Src : register(t0);
SamplerState Lin : register(s0);

cbuffer Params : register(b0) {
    float Sharpen;     // 0 = off
    float Saturation;  // 1 = untouched
    float2 InvSize;    // 1/width, 1/height OF THE VALID REGION
    float2 UvScale;    // valid region / whole texture - see below
    float2 PadA;
};

// *** THE VALID IMAGE IS NOT ALWAYS THE WHOLE TEXTURE. ***
//
// The hold texture is made at the LARGEST size either eye can need, and with a
// per-eye crop this eye's picture occupies only the top-left of it. Sampling
// 0..1 would drag the unused margin into the result and squeeze the picture.
// So the incoming 0..1 is scaled onto the part that actually holds an image.
// *** WORK IN LINEAR, HAND BACK LINEAR. ***
//
// The source holds display-encoded pixels and is read through a plain UNORM
// view, so a sample is gamma-encoded. The destination is an sRGB view, so the
// hardware encodes whatever we return. Decoding on the way in therefore does
// two jobs at once: it undoes the write conversion so the pass is invisible
// when it is off, and it puts the arithmetic in the space where sharpening and
// luma weights are actually defined.
float3 toLinear(float3 c) { return pow(saturate(c), 2.2); }

float3 tap(float2 uv, float2 o) {
    return toLinear(Src.SampleLevel(Lin, (uv + o * InvSize) * UvScale, 0).rgb);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 e = tap(uv, float2( 0,  0));

    if (Sharpen > 0.001) {
        float3 b = tap(uv, float2( 0, -1));
        float3 d = tap(uv, float2(-1,  0));
        float3 f = tap(uv, float2( 1,  0));
        float3 h = tap(uv, float2( 0,  1));

        // The window this pixel may move within, from its own neighbourhood.
        float mn = min(min(b.g, d.g), min(f.g, h.g));
        float mx = max(max(b.g, d.g), max(f.g, h.g));
        mn = min(mn, e.g);
        mx = max(mx, e.g);

        // How much lobe stays inside that window - RCAS's whole idea.
        //
        // The guards have to keep the sign. The first version wrote
        // max(4*mn - 4, -1e-5), and 4*mn-4 is NEGATIVE for every legal colour -
        // so the guard replaced the real denominator with -1e-5 on virtually
        // every pixel and threw the whole hitMax term away. Clamping a negative
        // quantity with max() against a tiny negative is not a divide-by-zero
        // guard, it is a deletion.
        float hitMin = mn / max(4.0 * mx, 1e-5);
        float hitMax = (1.0 - mx) / min(4.0 * mn - 4.0, -1e-5);
        float lobe = max(-hitMin, hitMax);
        lobe = clamp(lobe, -0.1875, 0.0) * Sharpen;

        float rcp = 1.0 / (4.0 * lobe + 1.0);
        e = saturate((lobe * (b + d + f + h) + e) * rcp);
    }

    if (abs(Saturation - 1.0) > 0.001) {
        // Rec.709 luma, so a grey stays grey at every setting.
        float l = dot(e, float3(0.2126, 0.7152, 0.0722));
        e = saturate(lerp(l.xxx, e, Saturation));
    }

    return float4(e, 1);
}
)";

struct PostParams {
    float sharpen;
    float saturation;
    float invW, invH;
    float uvScaleX, uvScaleY;
    float padA, padB;
};

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*,
                                        void*, LPCSTR, LPCSTR, UINT, UINT,
                                        ID3DBlob**, ID3DBlob**);

ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader*  g_ps = nullptr;
ID3D11SamplerState* g_samp = nullptr;
ID3D11Buffer*       g_cb = nullptr;
ID3D11BlendState*   g_blend = nullptr;
ID3D11RasterizerState* g_rast = nullptr;
ID3D11DepthStencilState* g_ds = nullptr;
bool g_ready = false;
bool g_failed = false;

bool CompileOne(PFN_D3DCompile fn, const char* src, size_t len, const char* target,
                ID3DBlob** out) {
    ID3DBlob* err = nullptr;
    const HRESULT hr = fn(src, len, nullptr, nullptr, nullptr, "main", target,
                          0, 0, out, &err);
    if (FAILED(hr) || !*out) {
        COTW_LOG("[post] %s failed to compile (0x%08X): %s", target, (unsigned)hr,
                 err ? (const char*)err->GetBufferPointer() : "-");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    return true;
}

bool EnsureShaders(ID3D11Device* dev) {
    if (g_ready) return true;
    if (g_failed || !dev) return false;

    // By name, never a hard import: a d3dcompiler that cannot resolve would
    // take the whole DLL down at load time, and this is one optional feature.
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dll) dll = LoadLibraryW(L"d3dcompiler_43.dll");
    PFN_D3DCompile compile =
        dll ? (PFN_D3DCompile)GetProcAddress(dll, "D3DCompile") : nullptr;
    if (!compile) {
        COTW_LOG("[post] no d3dcompiler available - sharpening and saturation "
                 "cannot run, the eye is copied through unchanged.");
        g_failed = true;
        return false;
    }

    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    bool ok = CompileOne(compile, kPostVS, sizeof(kPostVS) - 1, "vs_5_0", &vsb) &&
              CompileOne(compile, kPostPS, sizeof(kPostPS) - 1, "ps_5_0", &psb);
    if (ok) {
        ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(),
                                               vsb->GetBufferSize(), nullptr, &g_vs)) &&
             SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(),
                                              psb->GetBufferSize(), nullptr, &g_ps));
    }
    if (vsb) vsb->Release();
    if (psb) psb->Release();

    if (ok) {
        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        ok = SUCCEEDED(dev->CreateSamplerState(&sd, &g_samp));
    }
    if (ok) {
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = sizeof(PostParams);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_cb));
    }
    if (ok) {
        // Our own state for all three, because whatever the game left bound is
        // not ours to assume. A blend state with the alpha channel masked off,
        // or a depth test against a buffer we are not using, silently discards
        // the draw - which is exactly how the motion-vector target came out
        // black for a week.
        D3D11_BLEND_DESC bl{};
        bl.RenderTarget[0].BlendEnable = FALSE;
        bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        ok = SUCCEEDED(dev->CreateBlendState(&bl, &g_blend));
    }
    if (ok) {
        D3D11_RASTERIZER_DESC rs{};
        rs.FillMode = D3D11_FILL_SOLID;
        rs.CullMode = D3D11_CULL_NONE;
        rs.DepthClipEnable = TRUE;
        ok = SUCCEEDED(dev->CreateRasterizerState(&rs, &g_rast));
    }
    if (ok) {
        D3D11_DEPTH_STENCIL_DESC ds{};
        ds.DepthEnable = FALSE;
        ds.StencilEnable = FALSE;
        ok = SUCCEEDED(dev->CreateDepthStencilState(&ds, &g_ds));
    }

    if (!ok) {
        COTW_LOG("[post] could not create the sharpening pass - the eye is copied "
                 "through unchanged.");
        PostProcessShutdown();
        g_failed = true;
        return false;
    }
    g_ready = true;
    COTW_LOG("[post] sharpening and saturation pass ready (RCAS, contrast-adaptive "
             "so it cannot ring).");
    return true;
}

}  // namespace

bool PostProcessActive() {
    const Config& c = Cfg();
    return c.post_sharpen > 0.001f || fabsf(c.post_saturation - 1.0f) > 0.001f;
}

bool PostProcessApply(ID3D11Device* dev, ID3D11DeviceContext* ctx,
                      ID3D11Texture2D* src, ID3D11Texture2D* dst,
                      unsigned w, unsigned h, DXGI_FORMAT dstViewFormat) {
    if (!PostProcessActive() || !dev || !ctx || !src || !dst || !w || !h) return false;
    if (!EnsureShaders(dev)) return false;

    D3D11_TEXTURE2D_DESC sd{}, dd{};
    src->GetDesc(&sd);
    dst->GetDesc(&dd);

    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    if (FAILED(dev->CreateShaderResourceView(src, nullptr, &srv)) || !srv) {
        static bool moaned = false;
        if (!moaned) {
            moaned = true;
            COTW_LOG("[post] the hold texture cannot be read by a shader (format %d, "
                     "bind flags %u) - sharpening and colour cannot run.",
                     (int)sd.Format, sd.BindFlags);
        }
        return false;
    }

    // *** THE SWAPCHAIN IMAGE IS TYPELESS, AND THAT IS THE WHOLE STORY. ***
    //
    // The runtime hands out R8G8B8A8_TYPELESS (27) - it is the application that
    // decides how those bits are read, which is exactly why we asked it for an
    // sRGB format when the swapchain was created. A typeless resource has NO
    // default view format, so `CreateRenderTargetView(dst, nullptr, ...)` can
    // never succeed on one, and an explicit view whose format is also TYPELESS
    // is just as invalid. Two attempts failed for that one reason, both of them
    // silently: built, initialised, reported ready, never drew.
    //
    // So the concrete format comes from the caller - the one place that knows
    // what it asked the runtime for. The view is sRGB, the hardware encodes on
    // write, and the shader hands back LINEAR to be encoded: it decodes each
    // sample on the way in, does its arithmetic where sharpening and luma
    // weights are actually defined, and the round trip returns the original
    // pixel exactly when sharpening is 0 and colour is 1.00.
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = dstViewFormat;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (rd.Format == DXGI_FORMAT_UNKNOWN ||
        rd.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS) {
        rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;   // what we ask XR for
    } else if (rd.Format == DXGI_FORMAT_B8G8R8A8_TYPELESS) {
        rd.Format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    }
    if (FAILED(dev->CreateRenderTargetView(dst, &rd, &rtv)) || !rtv) {
        static bool moaned = false;
        if (!moaned) {
            moaned = true;
            COTW_LOG("[post] could not make a render target view of the swapchain "
                     "image (resource format %d, view format %d) - sharpening and "
                     "colour cannot run, the eye is copied through unchanged.",
                     (int)dd.Format, (int)rd.Format);
        }
        srv->Release();
        return false;
    }

    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) && m.pData) {
        PostParams p{};
        p.sharpen = Cfg().post_sharpen;
        p.saturation = Cfg().post_saturation;
        p.invW = 1.0f / (float)w;
        p.invH = 1.0f / (float)h;
        // The part of the source that actually holds this eye's picture.
        p.uvScaleX = sd.Width ? (float)w / (float)sd.Width : 1.0f;
        p.uvScaleY = sd.Height ? (float)h / (float)sd.Height : 1.0f;
        memcpy(m.pData, &p, sizeof(p));
        ctx->Unmap(g_cb, 0);
    }

    // Once a second, what this pass is actually doing - because "the sharpness
    // slider does nothing" needed three guesses to answer and one line of log
    // would have answered it outright.
    static ULONGLONG lastLog = 0;
    static long ran = 0;
    ++ran;
    const ULONGLONG now = GetTickCount64();
    if (now - lastLog >= 5000) {
        lastLog = now;
        COTW_LOG("[post] %ld draws in the last 5 s | sharpen %.2f saturation %.2f | "
                 "source %ux%u using %ux%u | target %ux%u fmt %d",
                 ran, Cfg().post_sharpen, Cfg().post_saturation,
                 sd.Width, sd.Height, w, h, dd.Width, dd.Height, (int)dd.Format);
        ran = 0;
    }

    // *** THIS RUNS ON THE MOD'S OWN FRAME, NOT INSIDE THE GAME'S. ***
    //
    // The submit path owns the context here - the game's frame has already
    // presented - so the state is set outright rather than saved and restored.
    // Everything the draw depends on is bound explicitly for the same reason
    // the resolve had to: an inherited scissor, blend or depth state is a
    // silent black frame.
    const float blendFactor[4] = {0, 0, 0, 0};
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0.0f, 0.0f, (float)w, (float)h, 0.0f, 1.0f};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(g_rast);
    ctx->RSSetScissorRects(0, nullptr);
    ctx->OMSetBlendState(g_blend, blendFactor, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(g_ds, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &g_samp);
    ctx->PSSetConstantBuffers(0, 1, &g_cb);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);
    ctx->Draw(3, 0);

    // Unbind the source before anything else can bind it as a target.
    ID3D11ShaderResourceView* none = nullptr;
    ctx->PSSetShaderResources(0, 1, &none);
    ID3D11RenderTargetView* noRtv = nullptr;
    ctx->OMSetRenderTargets(1, &noRtv, nullptr);

    rtv->Release();
    srv->Release();
    return true;
}

void PostProcessShutdown() {
    if (g_ds)    { g_ds->Release();    g_ds = nullptr; }
    if (g_rast)  { g_rast->Release();  g_rast = nullptr; }
    if (g_blend) { g_blend->Release(); g_blend = nullptr; }
    if (g_cb)    { g_cb->Release();    g_cb = nullptr; }
    if (g_samp)  { g_samp->Release();  g_samp = nullptr; }
    if (g_ps)    { g_ps->Release();    g_ps = nullptr; }
    if (g_vs)    { g_vs->Release();    g_vs = nullptr; }
    g_ready = false;
}

}  // namespace cotwvr
