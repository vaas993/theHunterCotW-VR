#include "taa.h"

#include "cbscan.h"
#include "config.h"
#include "dlss.h"
#include "jitterhunt.h"
#include "headtrack.h"
#include "frame_hook.h"
#include "log.h"
#include "stereo.h"
#include "vr.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace cotwvr {
namespace {

// ---------------------------------------------------------------------------
// WHAT THIS IS LOOKING FOR
//
// From two RenderDoc captures (2.4 GB and 2.8 GB, at 2880x3112 and 4130x2124),
// the engine's TAA resolve is ONE draw in the whole frame and it is identifiable
// by resource topology alone - no shader hash, no guessed state:
//
//     Draw(VertexCount = 3)                 a full-screen triangle
//     exactly one RTV bound, no DSV
//     RTV0        W x H   R11G11B10_FLOAT   the resolved image
//     PS slot 0   W x H   R11G11B10_FLOAT   this frame's scene colour
//     PS slot 1   W x H   R11G11B10_FLOAT   THE HISTORY, and not the RTV
//     PS slot 2   W x H   R16G16_FLOAT      motion vectors
//     viewport == (0, 0, W, H)
//
// 1 match in 785 draws, and 1 in 1225. The rule is SELF-REFERENTIAL - every size
// is compared against the RTV's own, never against a render resolution we think
// we know - so it survives a resolution change, and it was validated at two.
//
// Uniqueness is already reached at the slot-1 clause; slot 2, the viewport, the
// vertex count and the no-DSV test are margin. They are kept because they cost
// nothing and they make the rule fail LOUDLY after a game patch rather than
// drift quietly onto a neighbouring pass.
//
// This is a description of RESOURCES, not a learned pointer and not a guessed
// state fingerprint - the discipline THE_FLICKER_POSTMORTEM.md:139-152 demands.
//
// THE THREE QUESTIONS IT EXISTS TO ANSWER
//
//   1. Does the resolve run TWICE per real frame under full-rate stereo?
//      If it runs once, the whole per-eye model is wrong and nothing should be
//      built on it.
//   2. Do the two runs carry eye tags 0 and 1? This settles the attribution
//      question the previous attempt failed on - and that attempt failed
//      SILENTLY, because its counter was never printed by any of 113 logs.
//   3. Do both runs read the SAME slot-1 pointer? Same => each eye is blending
//      against the other eye, which is the ghost. Alternating => the engine's
//      ping-pong parity flips between eyes and the artefact is asymmetric.
//
// Either answer to 3 is fixed by the same per-eye duplication. The log just says
// which shape it has - and per-eye history is also the prerequisite for ever
// hanging FSR 2 / DLSS off this renderer, because those accumulate 8-16 frames
// where TAA keeps about 2.
// ---------------------------------------------------------------------------

constexpr DXGI_FORMAT kColourFmt = DXGI_FORMAT_R11G11B10_FLOAT;
constexpr DXGI_FORMAT kMotionFmt = DXGI_FORMAT_R16G16_FLOAT;

enum TaaExit {
    kNotThreeVerts = 0,
    kRtvCount,
    kDsvBound,
    kRtvNotTex2D,
    kRtvFormat,
    kSrv0,
    kSrv1,
    kSrv1IsRtv,
    kSrv2,
    kViewport,
    kTableFull,
    kMatched,
    kExitCount
};

const char* const kExitName[kExitCount] = {
    "not a 3-vertex Draw",
    "not exactly one render target bound",
    "a depth-stencil view is bound",
    "the render target is not a 2D texture",
    "the render target is not R11G11B10_FLOAT",
    "PS slot 0 is not a same-size R11G11B10_FLOAT texture",
    "PS slot 1 is not a same-size R11G11B10_FLOAT texture",
    "PS slot 1 IS the render target (so it is not history)",
    "PS slot 2 is not a same-size R16G16_FLOAT texture",
    "the viewport does not cover the render target exactly",
    "match table full - more than 8 resolves in one frame",
    "MATCHED - this is the TAA resolve",
};

// --- COM lifetime ----------------------------------------------------------
//
// The fingerprint queries up to sixteen objects and has ten ways out. Counting
// releases by hand across ten branches is how a per-draw hook starts leaking,
// and a leak here takes the game down within minutes rather than failing
// visibly. So the counting is done by the compiler.

template <class T>
struct Rel {
    T* p = nullptr;
    Rel() = default;
    explicit Rel(T* raw) : p(raw) {}
    Rel(const Rel&) = delete;
    Rel& operator=(const Rel&) = delete;
    ~Rel() { if (p) p->Release(); }
    T** Put() { return &p; }
    T* Get() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct RtvPair {
    ID3D11RenderTargetView* v[2] = { nullptr, nullptr };
    ~RtvPair() { for (auto* r : v) if (r) r->Release(); }
};

struct SrvSet {
    ID3D11ShaderResourceView* v[4] = {};
    ~SrvSet() { for (auto* s : v) if (s) s->Release(); }
};

// --- what a bound view is actually pointing at ------------------------------

struct TexInfo {
    const void* res = nullptr;   // IDENTITY TOKEN ONLY - never dereferenced
    UINT w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    UINT mips = 0, arr = 0, samples = 0;
    UINT bind = 0;
    UINT usage = 0;
    bool valid = false;          // true only if it is a 2D texture we described
};

// The resource pointer survives this function while the reference does not. That
// is deliberate and it is safe ONLY because the pointer is compared, printed and
// nothing else - the same rule HudRow's shader pointers live under. Dereference
// it and it is a use-after-free.
template <class View>
TexInfo DescribeView(View* v) {
    TexInfo t;
    if (!v) return t;
    Rel<ID3D11Resource> res;
    v->GetResource(res.Put());
    if (!res) return t;
    t.res = res.Get();
    Rel<ID3D11Texture2D> tex;
    if (FAILED(res.Get()->QueryInterface(__uuidof(ID3D11Texture2D),
                                         (void**)tex.Put())) || !tex)
        return t;                       // a buffer or a 3D texture: res set, valid false
    D3D11_TEXTURE2D_DESC d{};
    tex.Get()->GetDesc(&d);
    t.w = d.Width;  t.h = d.Height;  t.fmt = d.Format;
    t.mips = d.MipLevels;  t.arr = d.ArraySize;  t.samples = d.SampleDesc.Count;
    t.bind = d.BindFlags;  t.usage = (UINT)d.Usage;
    t.valid = true;
    return t;
}

bool SameSize(const TexInfo& a, const TexInfo& b) {
    return a.valid && b.valid && a.w == b.w && a.h == b.h;
}

const char* FmtName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R11G11B10_FLOAT:    return "R11G11B10_FLOAT";
        case DXGI_FORMAT_R16G16_FLOAT:       return "R16G16_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
        case DXGI_FORMAT_R32G8X24_TYPELESS:  return "R32G8X24_TYPELESS";
        case DXGI_FORMAT_R24G8_TYPELESS:     return "R24G8_TYPELESS";
        case DXGI_FORMAT_R32_FLOAT:          return "R32_FLOAT";
        case DXGI_FORMAT_R16_FLOAT:          return "R16_FLOAT";
        case DXGI_FORMAT_R8G8B8A8_UNORM:     return "R8G8B8A8_UNORM";
        case DXGI_FORMAT_UNKNOWN:            return "none";
        default:                             return "other";
    }
}

// ---------------------------------------------------------------------------
// WHAT THE PROBE MEASURED, 2026-08-10, ~1350 frames at 90 fps in the headset
//
// Every second, without exception: TWO resolves per real frame, eye tags 0 and
// 1, both on the SAME immediate context and the SAME thread, reading DIFFERENT
// history textures and writing DIFFERENT targets. Frame to frame, eye 0's
// history pointer never changed.
//
// With two textures A and B that pins the whole thing down. Measured pointers:
//
//     eye 0:  reads B (…A52E0)   writes A (…A4520)
//     eye 1:  reads A (…A4520)   writes B (…A52E0)
//
// So it is not "both eyes share one history" as section 3 of the plan supposed.
// It is a strict CROSS-FEED, and it is worse:
//
//     eye 0's history = what eye 1 wrote LAST frame   (other eye + one frame stale)
//     eye 1's history = what eye 0 wrote THIS frame   (other eye, same instant)
//
// NEITHER EYE EVER SEES ITSELF. And the two are contaminated differently, which
// is why the artefact is asymmetric - eye 1 gets a clean 64 mm parallax double,
// eye 0 gets parallax plus a frame of motion.
//
// It also means each eye ALREADY has a dedicated output texture (eye 0 always
// resolves into A, eye 1 always into B). That is tempting - it looks like the
// fix is just swapping which of the two each eye reads - but it is not
// reachable: eye 0's own previous image is in A, and A is the texture it is
// about to overwrite in the same draw. Reading and writing one texture in one
// draw is not legal D3D and the fingerprint rejects it by construction. So each
// eye needs a texture of its own that nothing else writes, and the resolved
// image has to be copied into it. Two full-res copies a frame is the price, and
// section 7 of the plan already records the zero-copy variant as later work.
// ---------------------------------------------------------------------------

// --- state ------------------------------------------------------------------

constexpr int kMaxMatches = 8;

struct Match {
    int   eye;                   // CurrentRenderEye() AT THE DRAW
    DWORD tid;
    const void* ctx;
    int   deferred;
    DXGI_FORMAT rtvViewFmt;      // the VIEW's format, in case it ever differs
    TexInfo rtv0;
    TexInfo srv[4];              // 3 is the depth read - not a gate, but FSR 2
                                 // and DLSS both need it, so it is recorded
    D3D11_VIEWPORT vp;
    // *** WHICH CONSTANT BUFFERS THIS DRAW ACTUALLY READS. ***
    //
    // The reprojection matrix is patched into the shared 704-byte camera block
    // at Unmap. That patch is worth nothing to THIS pass unless this pass reads
    // that block - and nobody has ever checked. If no slot here is 704 bytes,
    // the resolve reprojects with something else entirely and the matrix half
    // of the fix cannot reach it, however many blocks it patches.
    const void* cb[6];
    UINT cbBytes[6];
};

Match g_match[kMaxMatches];
volatile LONG g_matchCount = 0;      // slots claimed this real frame
volatile LONG g_drawsSeen  = 0;      // Draw() calls since the last report
LONG g_lastFrameMatches = 0;         // what the table printed below actually holds
volatile LONG g_exit[kExitCount] = {};

// Armed state. The probe stops by ITSELF - a diagnostic left on overnight fills
// the log and is indistinguishable from one that is broken. Re-arm by turning
// taa_probe off and on again, exactly like hud_probe.
volatile LONG g_armed = 0;
// Set when the probe is wanted but the game is not rendering the world yet. The
// fingerprint still runs while waiting - it is the only reliable signal that the
// world is on screen - but nothing is logged and the clock has not started.
volatile LONG g_waiting = 0;
ULONGLONG g_armedAt = 0;
ULONGLONG g_lastReport = 0;

// Aggregates over the report window. Only ever touched at the frame boundary,
// which is one thread, so plain longs are correct here.
long g_frames = 0;
long g_histo[4] = {};            // 0, 1, 2, 3-or-more matches in a real frame
long g_pairEyes01 = 0, g_pairEyesSame = 0;
long g_pairSameHistory = 0, g_pairDiffHistory = 0;
long g_pairSameTarget = 0, g_pairDiffTarget = 0;
long g_pingPongFlips = 0, g_pingPongStuck = 0;
const void* g_prevFirstHistory = nullptr;

// --- per-eye history: the private textures ----------------------------------
//
// Two textures, created from the ENGINE'S OWN DESC, unmodified. Never a
// hand-written one - the format-family rule, ARCHITECTURE.md:277-279.

ID3D11Texture2D*          g_ourTex[2] = { nullptr, nullptr };
ID3D11ShaderResourceView* g_ourSrv[2] = { nullptr, nullptr };
UINT        g_texW = 0, g_texH = 0;
DXGI_FORMAT g_texFmt = DXGI_FORMAT_UNKNOWN;
bool        g_ready = false;
bool        g_allocFailed = false;      // do not retry a failing create every draw

enum SubExit { kSubNoDevice = 0, kSubCreateFailed, kSubNoHistory, kSubExitCount };
const char* const kSubExitName[kSubExitCount] = {
    "no device from the context",
    "creating the private textures failed",
    "the history slot held nothing to copy from",
};
volatile LONG g_subApplied[2] = {};
volatile LONG g_subExit[kSubExitCount] = {};
volatile LONG g_subFrames = 0;

// Per THREAD. The resolve is issued on one thread (measured: the immediate
// context, thread 22500, both eyes), but the hook is shared with every deferred
// worker in the engine, and a shared flag between them is the bug class this
// file has already paid for twice.
struct Pending {
    bool active = false;
    bool replace = false;        // stage 1: the engine's resolve must not run
    int  eye = 0;
    ID3D11ShaderResourceView* saved = nullptr;
};
thread_local Pending t_pending;

// Belt and braces on top of using the original function pointer: if this file's
// own draw ever reaches the hook by any route, it is ignored rather than
// resolved again. Re-entrancy here is a stack overflow, not a glitch.
thread_local bool t_inOurDraw = false;

void ClearPending() {
    if (t_pending.saved) { t_pending.saved->Release(); t_pending.saved = nullptr; }
    t_pending.active = false;
    t_pending.replace = false;
}

// Stage 1 accounting. A pass that has been REPLACED and says nothing is the
// worst of both worlds: the picture changes and nothing explains it.
volatile LONG g_replaced[2] = {};
volatile LONG g_replaceFailed = 0;

// ---------------------------------------------------------------------------
// STAGE 2 - OUR OWN TEMPORAL RESOLVE
//
// A full-screen triangle of our own, drawn into the target the engine's resolve
// would have written, sampling THIS EYE's history instead of the shared one.
// That is the whole fix: the cross-eye contamination cannot happen, because the
// engine's ping-pong pair is never read.
//
// Almost no state has to be set up. The engine was about to issue exactly this
// kind of draw, so the render target, the viewport, the blend and depth state
// and the topology are already what a full-screen pass wants. We change the two
// shaders, one SRV slot, one sampler and one constant buffer - and put all five
// back afterwards, because our constants applied to whatever the engine draws
// next is the failure mode that made the weapon shift leak into the world.
// ---------------------------------------------------------------------------

const char kResolveVS[] = R"(
// No vertex buffer and no input layout: the triangle comes from the id alone.
void main(uint id : SV_VertexID, out float4 pos : SV_Position,
          out float2 uv : TEXCOORD0) {
    uv  = float2((id << 1) & 2, id & 2);
    pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
)";

const char kResolvePS[] = R"(
Texture2D<float3> CurColour : register(t0);   // the engine's, already bound
Texture2D<float3> History   : register(t1);   // OURS - this eye's own
Texture2D<float2> Motion    : register(t2);   // the engine's, already bound
Texture2D<float>  Depth     : register(t3);   // the engine's, already bound
Texture2D<uint2>  Stencil   : register(t8);   // OURS - the same depth texture,
                                              // read as stencil instead
SamplerState      Lin       : register(s0);

// Catmull-Rom history, 9 taps. A history resampled with plain bilinear every
// frame loses a little sharpness each time and the loss compounds - which is the
// "world is less sharp" the owner reported. This costs eight extra samples and
// gets it back.
float3 SampleHistorySharp(float2 uv, float2 size, float2 invSize) {
    float2 samplePos = uv * size;
    float2 texPos1 = floor(samplePos - 0.5) + 0.5;
    float2 f  = samplePos - texPos1;
    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);
    float2 w12 = w1 + w2;
    float2 off12 = w2 / w12;
    float2 p0 = (texPos1 - 1.0) * invSize;
    float2 p3 = (texPos1 + 2.0) * invSize;
    float2 p12 = (texPos1 + off12) * invSize;
    float3 r = 0.0;
    r += History.SampleLevel(Lin, float2(p0.x,  p0.y),  0) * (w0.x  * w0.y);
    r += History.SampleLevel(Lin, float2(p12.x, p0.y),  0) * (w12.x * w0.y);
    r += History.SampleLevel(Lin, float2(p3.x,  p0.y),  0) * (w3.x  * w0.y);
    r += History.SampleLevel(Lin, float2(p0.x,  p12.y), 0) * (w0.x  * w12.y);
    r += History.SampleLevel(Lin, float2(p12.x, p12.y), 0) * (w12.x * w12.y);
    r += History.SampleLevel(Lin, float2(p3.x,  p12.y), 0) * (w3.x  * w12.y);
    r += History.SampleLevel(Lin, float2(p0.x,  p3.y),  0) * (w0.x  * w3.y);
    r += History.SampleLevel(Lin, float2(p12.x, p3.y),  0) * (w12.x * w3.y);
    r += History.SampleLevel(Lin, float2(p3.x,  p3.y),  0) * (w3.x  * w3.y);
    return max(r, 0.0);
}

cbuffer Params : register(b13) {
    float  Blend;      // how much of the current frame survives each frame
    float  MvScale;    // the engine reads history at uv - mv, so this is -1
    float  UseMv;      // 0 disables reprojection entirely
    float  UseClamp;   // neighbourhood clamp on/off
    float2 InvSize;
    float  UseOwnMv;   // reproject from depth and OUR matrices instead
    float  UseStencil; // leave first-person geometry where it is
    float2 Size;       // pixels, for the sharp history filter
    float  SharpHist;  // Catmull-Rom history instead of bilinear
    float  ClampGamma; // how many standard deviations the history may stray
    float  Sharpen;      // 0 = off
    float  ObjectBlend;  // how much of the engine's per-object motion to take
    float2 HeadShift;    // the head rotation the matrices never carried, in UV
    float2 JitterUV;     // THIS frame's raster jitter in UV (y down); 0 = off
    float  PadJ;         // >0.5 = probe mode: write depth instead of the vector
    float  DilateDepth;  // take the closest depth of the 3x3 neighbourhood
    float  DeadZone;     // pixels: below this the fetch snaps to the texel
    float3 PadK;
    row_major float4x4 Reproj;   // this eye: NDC now -> NDC last frame
};

// Two outputs. o1 is the motion vector DLSS will need - written by the pass we
// already own rather than by an extra one, since this shader has already worked
// out where the pixel was. In PIXELS, current -> previous, which is what NGX
// expects with a unit MVScale.
void main(float4 pos : SV_Position, float2 uv : TEXCOORD0,
          out float4 o0 : SV_Target0, out float2 o1 : SV_Target1) {
    float3 cur = CurColour.SampleLevel(Lin, uv, 0);

    // WHERE WAS THIS PIXEL LAST FRAME, FOR THIS EYE?
    //
    // Two routes. The engine's own motion vectors describe the previous RENDER,
    // which under full-rate stereo is the OTHER EYE - measured, and the owner
    // confirmed they smear either way round. So by default we work it out from
    // depth and this eye's own two matrices instead.
    //
    // Conventions are not guessed: they are read off the game's decompiled
    // GenMotionVectors and SMAA_Temporal shaders (Luma's Just Cause 3 dump, same
    // engine). The matrix is ROW-major used as a ROW VECTOR - clip = world * M,
    // with w coming from column 3 - and the resolve samples history at
    // `uv - mv`, not `uv + mv`.
    // *** THE THING IN YOUR HANDS IS NOT PART OF THE WORLD. ***
    //
    // The weapon and the phone are drawn in first person: they do not move when
    // the camera turns, so reprojecting them as world geometry at their depth
    // drags them about, which is the shake the owner saw. The engine tags that
    // geometry with STENCIL BIT 6 (0x40) - the same tag the weapon 3D feature
    // already relies on - and the stencil is in the very depth texture we are
    // sampling. For those pixels the correct answer is that they did not move.
    bool firstPerson = false;
    if (UseStencil > 0.5)
        firstPerson = (Stencil.Load(int3(pos.xy, 0)).g & 0x40u) != 0u;

    // *** DE-JITTER: the textbook step this resolve was missing. *** The
    // raster put this pixel's sample at uv + JitterUV; motion and history
    // live in UN-jittered space, so all reprojection starts from the clean
    // position. Zero when the feature is off - identical to the old code.
    float2 baseUv = uv - JitterUV;

    float2 huv;
    if (firstPerson) {
        huv = baseUv;
    } else if (UseOwnMv > 0.5) {
        // Point-sampled: a depth filtered across an edge is a depth that exists
        // nowhere, and it reprojects to a place that exists nowhere either.
        float depth = Depth.Load(int3(pos.xy, 0));
        // *** CLOSEST-DEPTH DILATION - the fix for the edge buzz. ***
        //
        // Measured 2026-08-13: with a static camera the reprojection is right
        // to 0.3 px most of the time and then spikes to 4, 10, 25 px. The
        // engine rasterises with a 16-phase jitter, so a pixel on ANY
        // silhouette - a log's rim, a branch, a rock's outline - catches the
        // near surface on some frames and the far one on others. The depth
        // flips, the reconstructed world point flips with it, and the history
        // is fetched from somewhere else entirely: that pixel buzzes. Every
        // object has edges, which is why "most objects shake".
        //
        // The engine's own temporal pass never had this because it reprojects
        // from a VELOCITY BUFFER, where a pixel's motion belongs to whichever
        // surface was actually drawn there and is exactly zero for static
        // geometry. Reconstructing from depth cannot have that property, so
        // this does what every shipped TAA does instead: take the CLOSEST
        // depth in the neighbourhood, which is stable under sub-pixel
        // wobble because the near surface wins consistently.
        if (DilateDepth > 0.5) {
            [unroll] for (int dy = -1; dy <= 1; ++dy) {
                [unroll] for (int dx = -1; dx <= 1; ++dx) {
                    float d = Depth.Load(int3(pos.xy + int2(dx, dy), 0));
                    // Reverse-Z: nearer is LARGER.
                    depth = max(depth, d);
                }
            }
        }
        // uv -> NDC, then straight back out to the world through the inverse of
        // the matrix that produced this depth, and forward again through the
        // same eye's matrix from last frame. Reproj is those two combined
        // (both stripped of their jitter when de-jitter is on).
        float4 ndc  = float4(baseUv.x * 2.0 - 1.0, 1.0 - baseUv.y * 2.0, depth, 1.0);
        float4 prev = mul(ndc, Reproj);
        if (abs(prev.w) < 1e-9) { o0 = float4(cur, 1); o1 = 0; return; }
        float2 pndc = prev.xy / prev.w;
        huv = pndc * float2(0.5, -0.5) + 0.5;
    } else {
        // MvScale is -1 by default: the engine's own resolve reads its history
        // at uv MINUS the vector, and the first build had this backwards.
        float2 mv = Motion.SampleLevel(Lin, uv, 0) * MvScale * UseMv;
        huv = baseUv + mv;
    }
    // *** WHAT MOVES ON ITS OWN, NOT JUST WHAT THE CAMERA DID. ***
    //
    // Our reprojection is camera-only: right for rocks and houses, silent about
    // wind-blown foliage. DLSS accumulates 8-16 frames along whatever vector it
    // is given, so leaves smear while the bed does not - which is exactly what
    // the headset showed.
    //
    // The engine's own vectors (slot 2, UV space) DO contain per-object motion,
    // merged in by its GenMotionVectors pass. Their temporal reference is wrong
    // for us - "the previous render", which under full-rate is the other eye -
    // so they cannot simply replace ours. The difference between the two is
    // therefore part object motion and part reference mismatch, and this blends
    // rather than pretends to separate them.
    //
    // ObjectBlend 0 = ours alone (camera only, foliage smears under DLSS).
    // ObjectBlend 1 = the engine's alone (object motion right, reference wrong).
    // The head turned between these two frames and neither matrix knows it, so
    // the reprojection lands short by exactly that. Add it back.
    huv += HeadShift;

    // *** THE HISTORY IS INDEXED BY PIXEL, NOT BY WORLD POSITION. ***
    //
    // Everything above works in UN-JITTERED space, which is right for
    // reconstructing where a world point was. But the history buffer holds
    // the previous OUTPUT at pixel centres, so the fetch must be this
    // PIXEL's position displaced by the motion - not the un-jittered
    // position displaced by the motion. The difference is exactly this
    // frame's jitter, it changes every frame, and it lands on EVERY pixel:
    // rocks, logs, trunks, and the weapon and hands, which are excluded from
    // reprojection and still fetched off-texel by it. That is the shimmer
    // this project has been chasing, and it is why turning the reprojection
    // off looked like a cure - that test zeroed the jitter shift too.
    float2 ourDelta = huv - baseUv;   // jitter-free motion of this pixel
    // *** A STILL SCENE MUST BE FETCHED FROM THE EXACT TEXEL. ***
    //
    // Any fractional offset means the history is RESAMPLED, and a different
    // fraction each frame means a different blur each frame - which reads as
    // the texture breathing even when the offset itself is tiny. Below a
    // fifth of a pixel there is no motion worth tracking, so snap to zero and
    // let the fetch land dead on the texel it was written to.
    if (DeadZone > 0.0 && dot(ourDelta * Size, ourDelta * Size) < DeadZone * DeadZone) {
        ourDelta = 0.0;
    }
    // Apply the motion to THIS PIXEL. Zero motion now means the history is
    // read from the very texel it was written to - no resampling, nothing to
    // shimmer - which is what a still scene, and anything hand-held, needs.
    float2 finalDelta = ourDelta;
    if (ObjectBlend > 0.001) {
        // The engine reads its history at uv - mv, so its delta is -mv.
        float2 engDelta = -Motion.SampleLevel(Lin, uv, 0);
        finalDelta = lerp(ourDelta, engDelta, ObjectBlend);
    }
    huv = uv + finalDelta;
    o1 = finalDelta * Size;
    // PROBE MODE: hand the CPU the two numbers it cannot see - the depth this
    // pixel was reconstructed from, and the w the reprojection produced. The
    // CPU check and the shader disagreed by 3-5x with depth unknown, and
    // D3D11 refuses a boxed copy out of a depth-stencil resource, so the
    // shader reports it instead. Scaled by 1000 to sit in half-float's good
    // range. Costs the MV target for the duration - a diagnostic, not a mode
    // to play in.
    if (PadJ > 0.5) {
        float dbg = Depth.Load(int3(pos.xy, 0));
        o1 = float2(dbg * 1000.0, ourDelta.x * Size.x);
    }

    // History off the edge of the frame has nothing to say about this pixel.
    if (huv.x < 0.0 || huv.y < 0.0 || huv.x > 1.0 || huv.y > 1.0) {
        o0 = float4(cur, 1);
        return;
    }

    float3 hist = (SharpHist > 0.5) ? SampleHistorySharp(huv, Size, InvSize)
                                    : History.SampleLevel(Lin, huv, 0);

    // Clamp the history into the colours actually present around this pixel.
    // Without it, a history fetched from the wrong place is accepted whole and
    // smears; with it, a wrong fetch is rejected and the pixel just aliases.
    // That is the difference between a soft picture and a shimmery one, and a
    // shimmery one is honest about being wrong.
    if (UseClamp > 0.5) {
        // *** VARIANCE, NOT MIN/MAX. ***
        //
        // A min/max box over the neighbourhood is the blunt version: one bright
        // speck in the nine widens the box for every pixel, so wrong history
        // gets let through and reads as the "medium blurness when moving" the
        // owner reported. Clamping to mean +/- gamma*sigma tracks what the
        // neighbourhood actually contains, which is both tighter where the
        // picture is flat and honest where it is busy.
        float3 m1 = 0.0, m2 = 0.0;
        [unroll] for (int y = -1; y <= 1; ++y) {
            [unroll] for (int x = -1; x <= 1; ++x) {
                float3 s = CurColour.SampleLevel(Lin, uv + float2(x, y) * InvSize, 0);
                m1 += s;
                m2 += s * s;
            }
        }
        m1 /= 9.0;
        m2 /= 9.0;
        float3 sigma = sqrt(max(m2 - m1 * m1, 0.0));
        float3 lo = m1 - ClampGamma * sigma;
        float3 hi = m1 + ClampGamma * sigma;
        // Never exclude the pixel we actually have - a box that does not contain
        // the current colour would fight the resolve rather than steady it.
        lo = min(lo, cur);
        hi = max(hi, cur);
        hist = clamp(hist, lo, hi);
    }
    float3 outColour = lerp(hist, cur, Blend);

    // *** SHARPEN THE OUTPUT, NOT THE RENDER. ***
    //
    // Prior-art lesson 12: every VR mod surveyed sharpens the finished image
    // rather than fighting the renderer. Any temporal resolve costs a little
    // acutance - it is an average, and averages are soft - and a clamped unsharp
    // mask gives it back for four taps.
    //
    // Clamped to the local neighbourhood on purpose: an unclamped sharpen
    // overshoots into halos around trunks and antlers, which in a headset reads
    // as edge crawl. Bounded by what is actually there, it cannot ring. And it
    // is written for HDR - the R11G11B10 target runs well past 1.0, so the usual
    // 0..1 RCAS limiter would clip the sky.
    if (Sharpen > 0.001) {
        float3 n = CurColour.SampleLevel(Lin, uv + float2(0, -InvSize.y), 0);
        float3 s = CurColour.SampleLevel(Lin, uv + float2(0,  InvSize.y), 0);
        float3 e = CurColour.SampleLevel(Lin, uv + float2( InvSize.x, 0), 0);
        float3 w = CurColour.SampleLevel(Lin, uv + float2(-InvSize.x, 0), 0);
        float3 blur = (n + s + e + w) * 0.25;
        float3 sharpened = outColour + (outColour - blur) * Sharpen;
        float3 mn = min(min(min(n, s), min(e, w)), outColour);
        float3 mx = max(max(max(n, s), max(e, w)), outColour);
        outColour = clamp(sharpened, mn, mx);
    }
    o0 = float4(outColour, 1);
}
)";

float g_headYawCur[2] = {}, g_headPitchCur[2] = {};
float g_headYawPrev[2] = {}, g_headPitchPrev[2] = {};
float g_headYawPrev2[2] = {}, g_headPitchPrev2[2] = {};
bool  g_haveHeadCur[2] = {false, false}, g_haveHeadPrev[2] = {false, false};
bool  g_haveHeadPrev2[2] = {false, false};
float g_headShift[2] = {0.0f, 0.0f};
// The 10x amplification test read "no change", so the shift is not reaching
// the picture and one of the gates above it is failing silently - the fifth
// silent-gate incident if so. Each exit of the head-fix block is counted, so
// one run of the log NAMES the dead gate instead of guessing at it.
volatile LONG g_headFixApplied = 0;
volatile LONG g_headFixNoAngles = 0;
volatile LONG g_headFixNoTangents = 0;
volatile LONG g_headFixNoPrev = 0;
float g_headFixLastX = 0.0f, g_headFixLastY = 0.0f;

struct ResolveParams {
    float blend, mvScale, useMv, useClamp;
    float invW, invH, useOwnMv, useStencil;
    float sizeW, sizeH, sharpHist, clampGamma;
    float sharpen, objectBlend, headShiftX, headShiftY;
    float jitterUVx, jitterUVy, padJ0, dilateDepth;
    float deadZone, padK0, padK1, padK2;
    float reproj[16];        // this eye's previous clip, from this eye's current
};

// ---------------------------------------------------------------------------
// STAGE 3 - OUR OWN MOTION VECTORS
//
// The engine's velocities describe "since the previous RENDER", which under
// full-rate stereo is THE OTHER EYE - eye 0's point at eye 1 a frame ago, eye
// 1's at eye 0 moments ago. Neither points at that eye's own previous frame,
// which is what a per-eye history needs. Owner's verdict on using them: ON is
// worse than off, and reversed is better than ON but still mostly smeared.
// Exactly what a systematically wrong velocity looks like.
//
// So we compute our own. Depth is bound to that very pass, and the engine's
// world->clip matrix for THIS eye sits at +0x000 of the 704-byte block, which
// the snapshot table already holds. Rebuild the world position from depth,
// project it with the SAME EYE's matrix from the previous frame, and the
// difference is the motion vector we actually want. Static geometry only -
// animals carry no per-object history - but static geometry is the whole
// landscape, which is what smears.
//
// One matrix goes to the shader: prevVP * inverse(curVP). Its own sanity check
// is free: when prev and cur are the same frame it MUST be the identity.

void Mat4Mul(const float* a, const float* b, float* out) {   // out = a * b
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

bool Mat4Inverse(const float* m, float* out) {
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-20f) return false;
    det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return true;
}

float g_vpCur[2][16] = {};
float g_vpPrev[2][16] = {};
float g_vpPrev2[2][16] = {};      // two frames back - the jitter hunt needs it
// A copy of the three, taken at the frame boundary BEFORE the roll, so the
// jitter report can compare honestly without the roll having to move.
float g_vpPrev3[2][16] = {};      // a fourth level: the third difference needs it
bool  g_haveVpPrev3[2] = {false, false};
float g_jitCur[2][16] = {}, g_jitPrev[2][16] = {}, g_jitPrev2[2][16] = {};
float g_jitPrev3[2][16] = {};
bool  g_haveJit[2] = {false, false}, g_haveJit2[2] = {false, false};
bool  g_haveJit3[2] = {false, false};
float g_jitAccum[2] = {0.0f, 0.0f};
int   g_jitCount[2] = {0, 0};

// --- THE JITTER DLSS IS TOLD ABOUT, ESTIMATED LIVE -------------------------
//
// Per eye, per axis (x from m[12], y from m[13]): a running mean of the
// magnitude in PIXELS, the last signed value handed out, and how many sane
// samples the mean has seen. The estimator itself is EstimateJitterPixels
// below; this state is what lets it hand out a LOCKED magnitude with a
// per-frame sign instead of a raw sample that spikes under head acceleration.
float g_jitMagAvg[2][2] = {};
int   g_jitMagN[2][2] = {};
float g_jitOutPx[2][2] = {};
bool  g_jitOutValid[2] = {false, false};
// The overlay (taa_jitter_inject) value that was IN each captured matrix, per
// eye per history level, in pixels - recorded at capture, rolled with the
// matrices. The estimator subtracts these before measuring the engine's part:
// the third difference assumes pure two-phase alternation, and the overlay
// would otherwise leak straight into the estimate.
float g_ovlCur[2][2] = {}, g_ovlPrev[2][2] = {};
float g_ovlPrev2[2][2] = {}, g_ovlPrev3[2][2] = {};
float g_turnAccum[2] = {0.0f, 0.0f};
float g_turnMax[2] = {0.0f, 0.0f};
int   g_turnCount[2] = {0, 0};

// *** FOUR CAMERAS, AND ONE OF THEM MAY BE THE ONE THAT FOLLOWS THE HEAD. ***
//
// The 704-byte block carries a camera at +0x000, +0x1D0, +0x210 and +0x260 -
// same field of view, different near planes. We reproject with +0x000, and it
// was just measured NOT to rotate when the head does. Rather than derive a
// correction for it, ask whether one of the other three already carries what we
// need: the view the frame was actually rendered with.
//
// Cheap, decisive, and it needs no maths - the same forward-axis angle, applied
// to all four.
constexpr UINT kCandOffsets[4] = {0x000, 0x1D0, 0x210, 0x260};
float g_candCur[2][4][16] = {};
float g_candPrev[2][4][16] = {};
bool  g_haveCand[2] = {false, false};
bool  g_haveCandPrev[2] = {false, false};
float g_candTurn[2][4] = {};
int   g_candCount[2] = {0, 0};
bool  g_haveVpCur[2] = {false, false};
bool  g_haveVpPrev[2] = {false, false};
bool  g_haveVpPrev2[2] = {false, false};
volatile LONG g_ownMvReady[2] = {};
float g_projA[2] = {}, g_projB[2] = {};
bool  g_haveProj[2] = {false, false};
float g_rejA = 0.0f, g_rejB = 0.0f, g_wantA = 0.0f, g_wantB = 0.0f;
volatile LONG g_vpAccepted[2] = {};
volatile LONG g_vpRejected[2] = {};
// The consistency gate: candidates too far from last frame's accepted camera
// (impostor cameras / snapshot rename races), and the re-seeds after a run of
// rejections (real teleports).
// Selection stats (taa_camera_lock v2) - the pre-headset verdict lives here:
// standing still, "moved" must read ~0.000 while "spread" stays large, which
// says impostor cameras were on offer and the continuous one was taken.
volatile LONG g_pickUsed[2] = {};
volatile LONG g_pickEmpty[2] = {};
int   g_pickCand[2] = {0, 0};
int   g_pickHits[2] = {0, 0};
int   g_pickMaxHits[2] = {0, 0};
float g_pickWobble[2] = {0.0f, 0.0f};
float g_pickMoved[2] = {0.0f, 0.0f};
float g_pickMovedDejit[2] = {0.0f, 0.0f};
float g_pickSpread[2] = {0.0f, 0.0f};

// The camera's own world position, which the renderer's matrices are relative
// to. Read from GlobalConstants +0x40 - the same slot the engine's own motion
// vector shader calls Globals[4].xyz, and the dump of this game's 1584-byte
// block shows a world-scale position sitting exactly there.
constexpr UINT kCamPosOffset = 0x40;
float g_camCur[2][3] = {};
float g_camPrev[2][3] = {};
bool  g_haveCamCur[2] = {false, false};
bool  g_haveCamPrev[2] = {false, false};
float g_camDelta[2] = {0.0f, 0.0f};
// *** THE CONVENTION IS READ, NOT GUESSED. ***
//
// From the engine's own decompiled GenMotionVectors shader (Luma's Just Cause 3
// dump, same Avalanche engine):
//
//     r2.xyz = r1.y * PrevViewProjMatrix._m10_m11_m13;
//     r1.xyw = r1.x * PrevViewProjMatrix._m00_m01_m03 + r2.xyz;
//     r1.xyz = r1.z * PrevViewProjMatrix._m20_m21_m23 + r1.xyw;
//     r1.xyz = PrevViewProjMatrix._m30_m31_m33 + r1.xyz;
//     float2 prevNDC = r1.xy / r1.z;
//
// declared there as `row_major float4x4`. That is a ROW VECTOR times the matrix -
// clip = world * M - with w taken from column 3 (_m03/_m13/_m23/_m33). It also
// matches this mod's own structural test, which looks for the w terms at indices
// 3/7/11 and expects index 15 to be ~0 (the renderer is camera-relative).
//
// So: row-major storage, row-vector multiply, and the combined matrix is
// inverse(cur) * prev in that order - NOT prev * inverse(cur), which is the
// column-vector form and was what the first draft of this function had.

// Strip a matrix's own jitter, exactly: extract it with the col0.col3
// identity (see JitterFromMatrix) and remove it as column0 -= j*column3 -
// the algebraic inverse of how a post-multiplied NDC translation lands.
// Each matrix carries ITS OWN frame's jitter, so each is stripped separately.
void StripJitter(float* m) {
    const float jx = m[0] * m[3] + m[4] * m[7] + m[8] * m[11];
    const float jy = m[1] * m[3] + m[5] * m[7] + m[9] * m[11];
    for (int r = 0; r < 4; ++r) {
        m[r * 4 + 0] -= jx * m[r * 4 + 3];
        m[r * 4 + 1] -= jy * m[r * 4 + 3];
    }
}

// *** THE REPROJECTION MUST BE BUILT IN DOUBLE PRECISION. ***
//
// These matrices carry world-scale translations - m[15] alone measures ~8500 -
// alongside a near plane of 0.0094, and inverting that in 32-bit float then
// multiplying by another one is catastrophic cancellation by the textbook.
// What survives is a few TENTHS OF A PIXEL of noise that changes every frame,
// which is precisely what the probe measured with a completely static camera,
// and precisely what the owner sees: silhouettes hold still (the variance
// clamp pins high-contrast edges) while the low-contrast texture INSIDE
// objects swims. Rocks, logs, trunks, foliage - every surface at once.
//
// The engine never meets this because it reprojects from a velocity buffer
// and never inverts anything. We do, so the intermediate work has to be
// double; the RESULT is near-identity and perfectly happy in float.
bool Mat4InverseD(const double* m, double* out) {
    double inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
    inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];
    double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det > -1e-30 && det < 1e-30) return false;
    det = 1.0 / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * det;
    return true;
}

void Mat4MulD(const double* a, const double* b, double* out) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double s = 0.0;
            for (int k = 0; k < 4; ++k) s += a[r * 4 + k] * b[k * 4 + c];
            out[r * 4 + c] = s;
        }
}

bool BuildReprojection(int eye, float* out) {
    if (!g_haveVpCur[eye] || !g_haveVpPrev[eye]) return false;
    float curM[16], prevM[16];
    memcpy(curM, g_vpCur[eye], 64);
    memcpy(prevM, g_vpPrev[eye], 64);
    if (Cfg().taa_dejitter) {
        StripJitter(curM);
        StripJitter(prevM);
    }
    if (Cfg().taa_double_reproj) {
        double curD[16], prevD[16], invD[16], outD[16];
        for (int i = 0; i < 16; ++i) { curD[i] = curM[i]; prevD[i] = prevM[i]; }
        if (!Mat4InverseD(curD, invD)) return false;
        Mat4MulD(invD, prevD, outD);
        for (int i = 0; i < 16; ++i) out[i] = (float)outD[i];
        // The camera-relative translation term stays available, in double too.
        if (Cfg().taa_camera_relative && g_haveCamCur[eye] && g_haveCamPrev[eye]) {
            double t[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
            t[12] = (double)g_camCur[eye][0] - g_camPrev[eye][0];
            t[13] = (double)g_camCur[eye][1] - g_camPrev[eye][1];
            t[14] = (double)g_camCur[eye][2] - g_camPrev[eye][2];
            double tmp[16];
            Mat4MulD(invD, t, tmp);
            Mat4MulD(tmp, prevD, outD);
            for (int i = 0; i < 16; ++i) out[i] = (float)outD[i];
        }
        return true;
    }
    float invCur[16];
    if (!Mat4Inverse(curM, invCur)) return false;

    // *** THE RENDERER IS CAMERA-RELATIVE, AND THAT IS THE WHOLE OF THE
    //     TRANSLATION BUG. ***
    //
    // These matrices take positions relative to THE CAMERA, not absolute world
    // positions - which is why this file's own structural test expects m[15] to
    // be ~0, and why the game's GenMotionVectors shader has to ADD
    // Globals[4].xyz (the camera position) to get a world position before
    // projecting with the previous matrix.
    //
    // So the origin itself moves between frames. A point reconstructed in this
    // frame's camera space and fed into last frame's matrix is wrong by exactly
    // the distance walked in between. Rotation does not move that origin, which
    // is why turning looked perfect while walking trailed on houses and rocks.
    //
    // The correction is a translation by (camNow - camThen), folded straight
    // into the matrix: relative-to-now -> relative-to-then -> clip-then.
    float t[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    if (Cfg().taa_camera_relative && g_haveCamCur[eye] && g_haveCamPrev[eye]) {
        t[12] = g_camCur[eye][0] - g_camPrev[eye][0];
        t[13] = g_camCur[eye][1] - g_camPrev[eye][1];
        t[14] = g_camCur[eye][2] - g_camPrev[eye][2];
        g_camDelta[eye] = sqrtf(t[12]*t[12] + t[13]*t[13] + t[14]*t[14]);
    }
    float tmp[16];
    Mat4Mul(invCur, t, tmp);
    Mat4Mul(tmp, g_vpPrev[eye], out);
    const float* cur = g_vpCur[eye];

    // The free sanity check: prev*inverse(cur) with prev == cur is the identity.
    // Logged once, because an inverse that is wrong produces a picture that is
    // merely odd rather than obviously broken.
    static bool checked = false;
    if (!checked) {
        checked = true;
        float t[16];
        Mat4Mul(cur, invCur, t);
        float worst = 0.0f;
        for (int i = 0; i < 16; ++i) {
            const float want = (i % 5 == 0) ? 1.0f : 0.0f;
            const float e = fabsf(t[i] - want);
            if (e > worst) worst = e;
        }
        COTW_LOG("[taa] reprojection self-check: inverse(VP)*VP differs from the "
                 "identity by at most %.6f. %s", worst,
                 worst < 1e-3f ? "Good - the inverse and the layout agree."
                               : "WRONG - the maths or the layout is off, and any "
                                 "motion vector built from it is meaningless.");
    }
    return true;
}

// *** THE JITTER, IN PIXELS, EVERY FRAME - what the guide requires and the
//     first integration never supplied. ***
//
// Rule 3.7.3(3): the per-frame sub-pixel offset must be reported whether or
// not the motion vectors also carry it; zero is only legal when no jitter was
// applied. This engine jitters every frame.
//
// The model, settled at last: the engine adds an NDC offset j into the
// projection, and in the combined world->clip matrix that lands as
// m[12] += j * m[15] (m[15] = the view translation's w coefficient, ~-8487
// here, NOT ~0 - these matrices are world-scale). So j_ndc = delta / m[15],
// and the screen effect is depth-INdependent after the perspective divide -
// the "divides by w" worry that stalled the first attempt was about the matrix
// element, not the pixel. NDC -> pixels: x scales by w/2, y by h/2 with the
// sign flipped (NDC y is up, the MV/jitter convention's y is down).
//
// The per-frame signed sample comes from the same third difference the probe
// proved out (x0 - 3x1 + 3x2 - x3 = 8j - annihilates motion up to
// acceleration). One sample can still be poisoned by a jerk, so the magnitude
// is locked to a running mean once eight sane samples agree, and only the SIGN
// is taken per frame; a spiked frame falls back to alternating the previous
// value, which is exactly what a two-phase jitter does anyway.
// *** THE EXACT EXTRACTION - jitter read out of the captured matrix itself. ***
//
// VP = V * (P*J). P*J's rows are (a,0,0,0),(0,b,0,0),(jx,jy,0,1),(0,0,n,0),
// so VP[i][0] = a*V_i0 + jx*V_i2 and VP[i][3] = V_i2. The view's rotation
// columns are orthonormal (col0 . col2 = 0, |col2| = 1), so summing
// VP[i][0]*VP[i][3] over the three rotation rows annihilates the a-term and
// leaves EXACTLY jx. No estimator, no timing, no thread: the value comes from
// the very matrix the GPU is using for this eye's draw. This is what finally
// replaced the third-difference estimator, whose 2-phase model turned out to
// be fiction - the engine runs its 16-phase table (AA mode 3) and the
// estimator's confident ~±0.26 was averaged noise, the measured root of the
// residual shake.
bool JitterFromMatrix(const float* m, unsigned w, unsigned h,
                      float* px, float* py) {
    if (!m) return false;
    const float jxNdc = m[0] * m[3] + m[4] * m[7] + m[8] * m[11];
    const float jyNdc = m[1] * m[3] + m[5] * m[7] + m[9] * m[11];
    const float ox = jxNdc * 0.5f * (float)w;
    const float oy = -jyNdc * 0.5f * (float)h;   // NDC y up -> pixel y down
    // A legal jitter is sub-pixel; anything larger means this matrix is not
    // the jittered main camera and the caller must fall back.
    if (!(ox > -1.5f && ox < 1.5f && oy > -1.5f && oy < 1.5f)) return false;
    *px = ox;
    *py = oy;
    return true;
}

bool EstimateJitterPixels(int eye, unsigned w, unsigned h, float* jx, float* jy) {
    *jx = *jy = 0.0f;
    if (!g_haveVpCur[eye] || !g_haveVpPrev[eye] || !g_haveVpPrev2[eye] ||
        !g_haveVpPrev3[eye])
        return false;
    const float m15 = g_vpCur[eye][15];
    if (fabsf(m15) < 1.0f) return false;     // not the world-scale matrix we know

    const float half[2] = {0.5f * (float)w, 0.5f * (float)h};
    float out[2] = {0.0f, 0.0f};
    bool any = false;
    for (int a = 0; a < 2; ++a) {
        const int i = 12 + a;
        // Remove the overlay WE injected from each sample first (pixels -> the
        // same m[15]-scaled clip form it was added in; y's sign flips), so the
        // third difference sees the engine's pure two-phase alternation.
        const float ySign = (a == 1) ? -1.0f : 1.0f;
        const float x0 = g_vpCur[eye][i]
            - ySign * g_ovlCur[eye][a] / half[a] * g_vpCur[eye][15];
        const float x1 = g_vpPrev[eye][i]
            - ySign * g_ovlPrev[eye][a] / half[a] * g_vpPrev[eye][15];
        const float x2 = g_vpPrev2[eye][i]
            - ySign * g_ovlPrev2[eye][a] / half[a] * g_vpPrev2[eye][15];
        const float x3 = g_vpPrev3[eye][i]
            - ySign * g_ovlPrev3[eye][a] / half[a] * g_vpPrev3[eye][15];
        const float jClip = (x0 - 3.0f * x1 + 3.0f * x2 - x3) * 0.125f;
        float px = (jClip / m15) * half[a];
        if (a == 1) px = -px;                // NDC y up -> pixel y down

        const float mag = fabsf(px);
        const bool sane = (mag > 0.02f && mag < 0.6f);
        if (sane) {
            // Running mean, capped so it stays adaptive if the engine ever
            // changes amplitude (a resolution change scales it, for one).
            if (g_jitMagN[eye][a] < 300) ++g_jitMagN[eye][a];
            g_jitMagAvg[eye][a] +=
                (mag - g_jitMagAvg[eye][a]) / (float)g_jitMagN[eye][a];
        }
        if (g_jitMagN[eye][a] < 8) {
            // This axis has not locked. Zero is the honest value for it - an
            // axis the engine genuinely does not jitter must never report a
            // fabricated offset.
            out[a] = 0.0f;
            continue;
        }
        const float locked = g_jitMagAvg[eye][a];
        if (sane && mag < 3.0f * locked) {
            out[a] = copysignf(locked, px);          // measured sign, locked size
        } else if (g_jitOutValid[eye]) {
            // Two-phase: alternate the ENGINE part of the previous output.
            // The stored value is the TOTAL (engine + overlay); negating that
            // wholesale also flipped the overlay, which is not alternating -
            // one of the ways red out-of-range dots got onto the scatter plot.
            out[a] = -(g_jitOutPx[eye][a] - g_ovlPrev[eye][a]);
        } else {
            out[a] = copysignf(locked, px);
        }
        any = true;
    }
    // The overlay is OURS and therefore exact - it is added to the report even
    // on frames where the engine part has not locked yet. What DLSS must be
    // told is the TOTAL offset the pixels were rendered with.
    out[0] += g_ovlCur[eye][0];
    out[1] += g_ovlCur[eye][1];
    // The guide's hard range applies to the SUM - clamping the engine part
    // alone and then adding the overlay is how totals of ~0.7 px reached the
    // scatter plot as red out-of-range dots.
    for (int a = 0; a < 2; ++a) {
        if (out[a] > 0.5f) out[a] = 0.5f;
        if (out[a] < -0.5f) out[a] = -0.5f;
    }
    if (Cfg().taa_jitter_inject) any = true;
    g_jitOutPx[eye][0] = out[0];
    g_jitOutPx[eye][1] = out[1];
    g_jitOutValid[eye] = any;
    *jx = out[0];
    *jy = out[1];
    return any;
}

ID3D11VertexShader* g_vs = nullptr;
ID3D11PixelShader*  g_ps = nullptr;
ID3D11SamplerState* g_samp = nullptr;
ID3D11Buffer*       g_params = nullptr;
// *** WRITES TO THE SECOND TARGET GO THROUGH THE BLEND STATE, AND WE NEVER
//     OWNED IT. ***
//
// The resolve draw ran with the ENGINE's blend state. The engine's own resolve
// writes ONE target, so whatever that state says about a second render target
// was never exercised by the engine - and the dev-DLL overlay showed the
// consequence: the motion-vector view is a field of unmoving dots, i.e. the
// texture DLSS receives is ZERO even in full motion, while our "written 83/83"
// counter cheerfully counted bind operations. A masked or blended RT1 is
// discarded silently; D3D11 has no error for it. Our draw now sets its own
// state: blending off, all channels writable, on BOTH targets.
ID3D11BlendState*   g_mrtBlend = nullptr;
// The depth each eye was rendered with, held for the headset-side upscale.
ID3D11Resource* g_upDepth[2] = {nullptr, nullptr};

// *** READ THE VECTOR BACK, DO NOT SQUINT AT IT. ***
//
// The dev overlay's dot grid distinguishes "vectors alive" from "vectors dead",
// but it cannot distinguish "head shift missing" from "head sweep was slow" -
// a slow turn's per-frame delta rounds to a dot honestly. This probe copies the
// CENTRE TEXEL of the motion-vector texture to a staging surface and logs it in
// pixels next to the head shift that was uploaded the same frame. If the two
// disagree, the loss is between the constant buffer and the render target; if
// they agree, the shift is in DLSS's input and the model itself is what's next.
// Three-deep ring: under headset load a single staging texture was almost
// never ready when asked (DO_NOT_WAIT failed for ~30 s at a stretch), so the
// copy goes into slot N and the map reads slot N-2, two frames retired.
ID3D11Texture2D* g_mvStage[3] = {nullptr, nullptr, nullptr};
int  g_mvStageIdx = 0;
int  g_mvStageFilled = 0;
// The depth the SHADER read at that same texel. Without it the CPU check and
// the GPU output are not computing the same thing, and they were measured to
// disagree by 10x - which is the whole question.
ID3D11Texture2D* g_depthStage[3] = {nullptr, nullptr, nullptr};
float g_depthAtCentre = 0.0f;
float ForwardTurnDegrees(const float* a, const float* b);

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h & 0x8000u) << 16;
    uint32_t exp = (h >> 10) & 0x1Fu;
    uint32_t man = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
        if (!man) { bits = sign; }
        else {                       // subnormal
            exp = 127 - 15 + 1;
            while (!(man & 0x400u)) { man <<= 1; --exp; }
            man &= 0x3FFu;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000u | (man << 13);
    } else {
        bits = sign | ((exp - 15 + 127) << 23) | (man << 13);
    }
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

// A SECOND VIEW ONTO THE ENGINE'S DEPTH TEXTURE, reading the stencil half.
// R32G8X24_TYPELESS carries depth and stencil together; the engine binds the
// depth view, and the stencil needs its own. Cached against the resource, so a
// resolution change or a new depth buffer rebuilds it rather than reading a
// stale one.
ID3D11ShaderResourceView* g_stencilSrv = nullptr;
ID3D11Resource*           g_stencilFor = nullptr;   // identity token only

// THE MOTION VECTOR TARGET, for DLSS.
//
// One texture, not one per eye: it is written by an eye's resolve and consumed
// by that same eye's DLSS evaluate immediately afterwards, before the other eye
// runs. Nothing survives across the pair, so nothing needs duplicating.
ID3D11Texture2D*          g_mvTex = nullptr;
ID3D11RenderTargetView*   g_mvRtv = nullptr;
ID3D11ShaderResourceView* g_mvSrv = nullptr;
UINT g_mvW = 0, g_mvH = 0;
bool g_mvFailed = false;

bool EnsureMotionVectors(ID3D11Device* dev, UINT w, UINT h) {
    if (g_mvTex && g_mvW == w && g_mvH == h) return true;
    if (g_mvFailed || !dev || !w || !h) return false;
    if (g_mvSrv) { g_mvSrv->Release(); g_mvSrv = nullptr; }
    if (g_mvRtv) { g_mvRtv->Release(); g_mvRtv = nullptr; }
    if (g_mvTex) { g_mvTex->Release(); g_mvTex = nullptr; }

    D3D11_TEXTURE2D_DESC d{};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R16G16_FLOAT;   // the same format the engine's own
                                           // velocity target uses
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_mvTex)) && g_mvTex;
    if (ok) ok = SUCCEEDED(dev->CreateRenderTargetView(g_mvTex, nullptr, &g_mvRtv));
    if (ok) ok = SUCCEEDED(dev->CreateShaderResourceView(g_mvTex, nullptr, &g_mvSrv));
    if (!ok) {
        if (g_mvSrv) { g_mvSrv->Release(); g_mvSrv = nullptr; }
        if (g_mvRtv) { g_mvRtv->Release(); g_mvRtv = nullptr; }
        if (g_mvTex) { g_mvTex->Release(); g_mvTex = nullptr; }
        g_mvFailed = true;
        COTW_LOG("[taa] could not create the %ux%u motion-vector target - DLSS "
                 "cannot be fed without it.", w, h);
        return false;
    }
    g_mvW = w; g_mvH = h;
    COTW_LOG("[taa] motion-vector target created: %ux%u R16G16_FLOAT, %.1f MB. "
             "Written by the resolve we already own, in pixels, current -> "
             "previous - which is what NGX expects.",
             w, h, (double)w * h * 4.0 / (1024.0 * 1024.0));
    return true;
}

ID3D11ShaderResourceView* StencilViewFor(ID3D11Device* dev, ID3D11Resource* depth) {
    if (!dev || !depth) return nullptr;
    if (g_stencilSrv && g_stencilFor == depth) return g_stencilSrv;
    if (g_stencilSrv) { g_stencilSrv->Release(); g_stencilSrv = nullptr; }
    g_stencilFor = nullptr;
    // *** THE STENCIL VIEW MUST BE IN THE DEPTH TEXTURE'S OWN FORMAT FAMILY. ***
    //
    // The first attempt used X24_TYPELESS_G8_UINT, which belongs to R24G8. This
    // depth buffer is R32G8X24_TYPELESS, whose stencil view is
    // X32_TYPELESS_G8X24_UINT - so creation failed silently, the flag stayed 0,
    // and the weapon went on shaking with nothing in the log to say why.
    D3D11_TEXTURE2D_DESC td{};
    {
        Rel<ID3D11Texture2D> t2;
        if (FAILED(depth->QueryInterface(__uuidof(ID3D11Texture2D),
                                         (void**)t2.Put())) || !t2)
            return nullptr;
        t2.Get()->GetDesc(&td);
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
    switch (td.Format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
            sd.Format = DXGI_FORMAT_X32_TYPELESS_G8X24_UINT; break;
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
            sd.Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT; break;
        default:
            COTW_LOG("[taa] depth texture is format %u - no stencil in it, so "
                     "first-person geometry cannot be told apart this way.",
                     (unsigned)td.Format);
            return nullptr;
    }
    sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    const HRESULT hr = dev->CreateShaderResourceView(depth, &sd, &g_stencilSrv);
    if (FAILED(hr)) {
        COTW_LOG("[taa] stencil view failed (0x%08X) on depth format %u.",
                 (unsigned)hr, (unsigned)td.Format);
        g_stencilSrv = nullptr;
        return nullptr;
    }
    g_stencilFor = depth;
    COTW_LOG("[taa] stencil view created on the engine's depth texture - "
             "first-person geometry (bit 6) will be left where it is instead of "
             "being reprojected as if it were scenery.");
    return g_stencilSrv;
}
bool g_shaderReady = false;
bool g_shaderFailed = false;
volatile LONG g_resolved[2] = {};
volatile LONG g_mvWritten[2] = {};
volatile LONG g_dlssRan[2] = {};

using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*,
                                        void*, LPCSTR, LPCSTR, UINT, UINT,
                                        ID3DBlob**, ID3DBlob**);

bool CompileOne(PFN_D3DCompile fn, const char* src, size_t len, const char* target,
                ID3DBlob** out) {
    ID3DBlob* err = nullptr;
    const HRESULT hr = fn(src, len, nullptr, nullptr, nullptr, "main", target,
                          0, 0, out, &err);
    if (FAILED(hr) || !*out) {
        COTW_LOG("[taa] shader %s failed to compile (0x%08X): %s", target,
                 (unsigned)hr, err ? (const char*)err->GetBufferPointer() : "-");
        if (err) err->Release();
        return false;
    }
    if (err) err->Release();
    return true;
}

bool EnsureShaders(ID3D11Device* dev) {
    if (g_shaderReady) return true;
    if (g_shaderFailed || !dev) return false;

    // Loaded by name rather than imported: a hard import that cannot resolve
    // takes the whole DLL down at load time, and this is one optional feature.
    HMODULE dll = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!dll) dll = LoadLibraryW(L"d3dcompiler_43.dll");
    PFN_D3DCompile compile =
        dll ? (PFN_D3DCompile)GetProcAddress(dll, "D3DCompile") : nullptr;
    if (!compile) {
        COTW_LOG("[taa] no d3dcompiler available - the mod cannot build its own "
                 "resolve, so the pass stays a straight pass-through.");
        g_shaderFailed = true;
        return false;
    }

    ID3DBlob* vsb = nullptr;
    ID3DBlob* psb = nullptr;
    bool ok = CompileOne(compile, kResolveVS, sizeof(kResolveVS) - 1, "vs_5_0", &vsb) &&
              CompileOne(compile, kResolvePS, sizeof(kResolvePS) - 1, "ps_5_0", &psb);
    if (ok)
        ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(),
                                               vsb->GetBufferSize(), nullptr, &g_vs)) &&
             SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(),
                                              psb->GetBufferSize(), nullptr, &g_ps));
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
        bd.ByteWidth = sizeof(ResolveParams);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ok = SUCCEEDED(dev->CreateBuffer(&bd, nullptr, &g_params));
    }
    if (ok) {
        D3D11_BLEND_DESC bl{};
        bl.AlphaToCoverageEnable = FALSE;
        bl.IndependentBlendEnable = TRUE;
        for (int i = 0; i < 2; ++i) {
            bl.RenderTarget[i].BlendEnable = FALSE;
            bl.RenderTarget[i].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        }
        ok = SUCCEEDED(dev->CreateBlendState(&bl, &g_mrtBlend));
    }

    if (!ok) {
        if (g_vs) { g_vs->Release(); g_vs = nullptr; }
        if (g_ps) { g_ps->Release(); g_ps = nullptr; }
        if (g_samp) { g_samp->Release(); g_samp = nullptr; }
        if (g_params) { g_params->Release(); g_params = nullptr; }
        if (g_mrtBlend) { g_mrtBlend->Release(); g_mrtBlend = nullptr; }
        g_shaderFailed = true;
        COTW_LOG("[taa] could not build the resolve shaders - passing through.");
        return false;
    }
    g_shaderReady = true;
    COTW_LOG("[taa] resolve shaders built. The mod now does the temporal pass "
             "itself, with one history per eye.");
    return true;
}

// Issues our resolve into whatever render target is currently bound - which is
// the engine's own, so all seven downstream consumers see what they expect.
bool RunOurResolve(ID3D11DeviceContext* ctx, int eye, UINT w, UINT h,
                   TaaDrawFn origDraw) {
    if (!g_shaderReady || !g_ourSrv[eye] || !origDraw) return false;

    // The injection needs the render size for its pixel->NDC conversion, and
    // this is the one place that reliably knows it every frame.
    TaaPublishRenderSize(w, h);

    // --- save exactly what we are about to change ---------------------------
    ID3D11VertexShader* oVS = nullptr;
    ID3D11PixelShader*  oPS = nullptr;
    ID3D11InputLayout*  oIL = nullptr;
    ID3D11ShaderResourceView* oSRV1 = nullptr;
    ID3D11ShaderResourceView* oSRV8 = nullptr;
    ID3D11SamplerState* oSamp = nullptr;
    ID3D11Buffer* oCB = nullptr;
    D3D11_PRIMITIVE_TOPOLOGY oTopo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->VSGetShader(&oVS, nullptr, nullptr);
    ctx->PSGetShader(&oPS, nullptr, nullptr);
    ctx->IAGetInputLayout(&oIL);
    ctx->IAGetPrimitiveTopology(&oTopo);
    ctx->PSGetShaderResources(1, 1, &oSRV1);
    ctx->PSGetShaderResources(8, 1, &oSRV8);
    ctx->PSGetSamplers(0, 1, &oSamp);
    ctx->PSGetConstantBuffers(13, 1, &oCB);

    // The stencil half of the depth texture the engine already has bound at 3.
    ID3D11ShaderResourceView* stencil = nullptr;
    {
        ID3D11ShaderResourceView* depthSrv = nullptr;
        ctx->PSGetShaderResources(3, 1, &depthSrv);
        if (depthSrv) {
            Rel<ID3D11Resource> depthRes;
            depthSrv->GetResource(depthRes.Put());
            Rel<ID3D11Device> dev;
            ctx->GetDevice(dev.Put());
            if (depthRes && dev) stencil = StencilViewFor(dev.Get(), depthRes.Get());
            depthSrv->Release();
        }
    }

    // --- parameters ---------------------------------------------------------
    D3D11_MAPPED_SUBRESOURCE m{};
    if (SUCCEEDED(ctx->Map(g_params, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)) && m.pData) {
        // ONE DIAL. Higher strength keeps more history (a lower blend weight)
        // and lets slightly more of it through the clamp, because tightening
        // the clamp while smoothing harder just trades smear for crawl. At 1.00
        // this is exactly the pair tuned in the headset.
        const float strength = Cfg().taa_ghosting_fix > 0.05f
                                   ? Cfg().taa_ghosting_fix : 0.05f;
        ResolveParams p{};
        p.blend    = Cfg().taa_blend / strength;
        if (p.blend > 1.0f) p.blend = 1.0f;
        if (p.blend < 0.02f) p.blend = 0.02f;
        // *** ONE ACCUMULATOR, NOT TWO. ***
        //
        // With the headset-side upscale running, this pass and DLSS BOTH average
        // over time: ours blends a history here, then DLSS accumulates 8-16
        // frames of that. Two accumulations chained means any misalignment in
        // the first is fed to the second as if it were signal - and the models
        // differ in how hard they lean on their history, which is exactly the
        // shape of "K and J shimmer while L and M do not".
        //
        // Luma does the same thing the other way round on this engine family: it
        // REPLACES the engine's temporal pass outright and lets DLSS keep the
        // only history. Blend 1.0 is that arrangement here - this pass still
        // reprojects and still writes the motion vectors DLSS needs, but its
        // colour output is simply the current frame, so DLSS is the only thing
        // averaging.
        if (Cfg().taa_single_accumulation && Cfg().dlss_enable && VRUpscalingNow()) {
            p.blend = 1.0f;
        }
        p.mvScale  = Cfg().taa_mv_invert ? -1.0f : 1.0f;
        p.useMv    = Cfg().taa_use_motion_vectors ? 1.0f : 0.0f;
        p.useClamp = Cfg().taa_clamp ? 1.0f : 0.0f;
        p.invW     = w ? 1.0f / (float)w : 0.0f;
        p.invH     = h ? 1.0f / (float)h : 0.0f;
        p.sizeW    = (float)w;
        p.sizeH    = (float)h;
        p.sharpHist  = Cfg().taa_sharp_history ? 1.0f : 0.0f;
        p.clampGamma = Cfg().taa_clamp_strength * (0.5f + 0.5f * strength);
        p.sharpen     = Cfg().taa_sharpen;
        p.objectBlend = Cfg().dlss_mv_object_blend;

        // *** THE HEAD ROTATION THE MATRICES DO NOT CARRY. ***
        //
        // Measured: neither +0x000 nor any of the block's other three cameras
        // rotates when the head does - 0.0000 deg across 60 samples of head-only
        // movement, against 0.47..1.52 for the stick. The head is applied
        // downstream of this buffer, at the view commit, from angles.
        //
        // Why the reprojection works at all despite that: it is
        // inverse(VP_now) * VP_prev, and when BOTH lack the head equally the
        // error cancels - a wrong world position projected back through an
        // equally wrong matrix lands in the right place. The cancellation only
        // fails by the amount the head rotation CHANGED between the two frames.
        //
        // So the correction is that delta, and nothing more. In NDC a small view
        // rotation is a shift of angle/tan(fov/2); halved for UV. Applied to the
        // history lookup, not to the matrix, because it is a screen-space
        // consequence of a rotation the matrix never saw.
        p.headShiftX = 0.0f;
        p.headShiftY = 0.0f;
        if (Cfg().taa_head_rotation_fix) {
            float yaw = 0.0f, pitch = 0.0f, tanH = 0.0f, tanV = 0.0f;
            if (!HeadYawRadians(&yaw) || !HeadPitchRadians(&pitch)) {
                InterlockedIncrement(&g_headFixNoAngles);
            } else if (!MeasuredCameraTangents(&tanH, &tanV) ||
                       tanH <= 1e-4f || tanV <= 1e-4f) {
                InterlockedIncrement(&g_headFixNoTangents);
            } else {
                if (!g_haveHeadPrev[eye]) InterlockedIncrement(&g_headFixNoPrev);
                if (g_haveHeadPrev[eye]) {
                    // Phase option: the mod WRITES the head angles into the
                    // engine's fields, but nothing proves the engine consumes
                    // them the same frame. If it renders them one frame late,
                    // the correct delta is the PREVIOUS frame's, and using the
                    // current one is out of phase - worst exactly at the head's
                    // reversals, which reads as shake.
                    float dYaw, dPitch;
                    if (Cfg().taa_head_rotation_delay && g_haveHeadPrev2[eye]) {
                        dYaw   = g_headYawPrev[eye]   - g_headYawPrev2[eye];
                        dPitch = g_headPitchPrev[eye] - g_headPitchPrev2[eye];
                    } else {
                        dYaw   = yaw   - g_headYawPrev[eye];
                        dPitch = pitch - g_headPitchPrev[eye];
                    }
                    // SEPARATE scales: yaw and pitch reach the screen through
                    // independently unknown sign conventions (the head invert
                    // flags calibrate them to the ENGINE's fields, not to UV),
                    // so one shared sign could fix one axis only by breaking
                    // the other - which is a candidate for why the first
                    // headset test read "no improvement at any scale".
                    p.headShiftX = Cfg().taa_head_rotation_scale   * 0.5f * dYaw   / tanH;
                    p.headShiftY = Cfg().taa_head_rotation_scale_y * 0.5f * dPitch / tanV;
                    g_headShift[eye] = fabsf(p.headShiftX) + fabsf(p.headShiftY);
                    g_headFixLastX = p.headShiftX;
                    g_headFixLastY = p.headShiftY;
                    InterlockedIncrement(&g_headFixApplied);
                }
                g_headYawCur[eye] = yaw;
                g_headPitchCur[eye] = pitch;
                g_haveHeadCur[eye] = true;
            }
        }
        p.useStencil = (Cfg().taa_exclude_viewmodel && stencil) ? 1.0f : 0.0f;
        // This frame's raster jitter for the de-jitter step, in UV (y down).
        // Extracted from the very matrix the GPU is using; zero when off or
        // when this eye's matrix has not been captured yet.
        p.jitterUVx = 0.0f;
        p.jitterUVy = 0.0f;
        // padJ0 doubles as the probe-depth switch (taa_mv_probe = 2).
        p.padJ0 = (Cfg().taa_mv_probe >= 2) ? 1.0f : 0.0f;
        p.dilateDepth = Cfg().taa_dilate_depth ? 1.0f : 0.0f;
        p.deadZone = Cfg().taa_mv_deadzone_px;
        p.padK0 = p.padK1 = p.padK2 = 0.0f;
        if (Cfg().taa_dejitter && g_haveVpCur[eye] && w && h) {
            float jpx = 0.0f, jpy = 0.0f;
            if (JitterFromMatrix(g_vpCur[eye], w, h, &jpx, &jpy)) {
                p.jitterUVx = jpx / (float)w;
                p.jitterUVy = jpy / (float)h;
            }
        }
        // Identity until both of this eye's matrices exist, which costs one
        // frame at startup and after every resolution change.
        float reproj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
        const bool haveReproj = BuildReprojection(eye, reproj);
        p.useOwnMv = (Cfg().taa_own_motion_vectors && haveReproj) ? 1.0f : 0.0f;
        memcpy(p.reproj, reproj, sizeof(reproj));
        if (haveReproj) InterlockedIncrement(&g_ownMvReady[eye]);
        memcpy(m.pData, &p, sizeof(p));
        ctx->Unmap(g_params, 0);
    }

    // --- the second render target -------------------------------------------
    //
    // The engine's colour target stays at slot 0 exactly as it was; ours goes
    // beside it at slot 1. Saved and put back including the null slot, because
    // leaving a stray target bound would have the engine's next draw writing
    // into our motion vectors.
    ID3D11RenderTargetView* oRtv[2] = { nullptr, nullptr };
    ID3D11DepthStencilView* oDsv = nullptr;
    bool mrt = false;
    if (Cfg().taa_write_motion_vectors) {
        Rel<ID3D11Device> dev;
        ctx->GetDevice(dev.Put());
        if (dev && EnsureMotionVectors(dev.Get(), w, h)) {
            ctx->OMGetRenderTargets(2, oRtv, &oDsv);
            if (oRtv[0]) {
                ID3D11RenderTargetView* both[2] = { oRtv[0], g_mvRtv };
                ctx->OMSetRenderTargets(2, both, oDsv);
                mrt = true;
                InterlockedIncrement(&g_mvWritten[eye]);
            }
        }
    }

    // --- our state ----------------------------------------------------------
    ctx->VSSetShader(g_vs, nullptr, 0);
    ctx->PSSetShader(g_ps, nullptr, 0);
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->PSSetShaderResources(1, 1, &g_ourSrv[eye]);
    if (stencil) ctx->PSSetShaderResources(8, 1, &stencil);
    ctx->PSSetSamplers(0, 1, &g_samp);
    ctx->PSSetConstantBuffers(13, 1, &g_params);
    // Our own blend state, so the second target's writes are not at the mercy
    // of whatever the engine left bound - which is exactly where the motion
    // vectors were dying. Saved and restored like everything else here.
    ID3D11BlendState* oBlend = nullptr;
    FLOAT oBlendFactor[4] = {};
    UINT oSampleMask = 0xFFFFFFFF;
    ctx->OMGetBlendState(&oBlend, oBlendFactor, &oSampleMask);
    if (g_mrtBlend) ctx->OMSetBlendState(g_mrtBlend, nullptr, 0xFFFFFFFF);
    // THE ORIGINAL, never ctx->Draw. See the note on TaaDrawFn in taa.h.
    t_inOurDraw = true;
    origDraw(ctx, 3, 0);
    t_inOurDraw = false;
    ctx->OMSetBlendState(oBlend, oBlendFactor, oSampleMask);
    if (oBlend) oBlend->Release();

    // --- exact restore, including the nulls ---------------------------------
    ctx->VSSetShader(oVS, nullptr, 0);
    ctx->PSSetShader(oPS, nullptr, 0);
    ctx->IASetInputLayout(oIL);
    ctx->IASetPrimitiveTopology(oTopo);
    ctx->PSSetShaderResources(1, 1, &oSRV1);
    ctx->PSSetShaderResources(8, 1, &oSRV8);
    ctx->PSSetSamplers(0, 1, &oSamp);
    ctx->PSSetConstantBuffers(13, 1, &oCB);
    if (oVS) oVS->Release();
    if (oPS) oPS->Release();
    if (oIL) oIL->Release();
    if (oSRV1) oSRV1->Release();
    if (oSRV8) oSRV8->Release();
    if (mrt) {
        ctx->OMSetRenderTargets(2, oRtv, oDsv);   // exact, nulls included
    }
    if (oRtv[0]) oRtv[0]->Release();
    if (oRtv[1]) oRtv[1]->Release();
    if (oDsv) oDsv->Release();
    if (oSamp) oSamp->Release();
    if (oCB) oCB->Release();

    // *** WHAT THE UPSCALER WILL NEED, KEPT ALIVE HERE. ***
    //
    // The headset-side upscale runs after the engine has finished this eye -
    // tonemap, HUD and all - but DLSS needs the depth and motion vectors that
    // eye was RENDERED with, and by then the engine may have moved on. This is
    // the moment both are provably current, so a reference is held per eye.
    {
        ID3D11ShaderResourceView* dsrv = nullptr;
        ctx->PSGetShaderResources(3, 1, &dsrv);
        if (dsrv) {
            ID3D11Resource* dres = nullptr;
            dsrv->GetResource(&dres);
            if (dres) {
                if (g_upDepth[eye]) g_upDepth[eye]->Release();
                g_upDepth[eye] = dres;              // reference transferred
            }
            dsrv->Release();
        }
    }

    // The centre-texel readback, eye 0 only, mapped two frames late so it never
    // stalls the pipeline. Logged against everything it could correlate with:
    // the head shift uploaded, how far the CAPTURED MATRIX turned this frame,
    // and how far the HEAD turned - so a spike names its source in one line.
    if (Cfg().taa_mv_probe && mrt && eye == 0 && g_mvTex) {
        Rel<ID3D11Device> dev2;
        ctx->GetDevice(dev2.Put());
        if (dev2 && !g_mvStage[0]) {
            D3D11_TEXTURE2D_DESC d{};
            d.Width = 1; d.Height = 1; d.MipLevels = 1; d.ArraySize = 1;
            d.Format = DXGI_FORMAT_R16G16_FLOAT;
            d.SampleDesc.Count = 1;
            d.Usage = D3D11_USAGE_STAGING;
            d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            for (int i = 0; i < 3; ++i)
                if (FAILED(dev2.Get()->CreateTexture2D(&d, nullptr, &g_mvStage[i]))) {
                    for (int j = 0; j < 3; ++j)
                        if (g_mvStage[j]) { g_mvStage[j]->Release(); g_mvStage[j] = nullptr; }
                    break;
                }
        }
        // Every-frame matrix-difference stats, accumulated between log lines -
        // the first fingerprint sampled at 4 Hz aliased over 90 fps and showed
        // "identical matrices" while frames in between could differ freely.
        {
            static float dRow3Max = 0.0f;
            static long dBigCount = 0, dFrames = 0;
            float cD[16], pD[16];
            memcpy(cD, g_vpCur[0], 64);
            memcpy(pD, g_vpPrev[0], 64);
            if (Cfg().taa_dejitter) { StripJitter(cD); StripJitter(pD); }
            float d3 = 0.0f;
            for (int i = 12; i < 16; ++i) {
                const float d = fabsf(cD[i] - pD[i]);
                if (d > d3) d3 = d;
            }
            ++dFrames;
            if (d3 > dRow3Max) dRow3Max = d3;
            if (d3 > 0.1f) ++dBigCount;
            static ULONGLONG lastD = 0;
            const ULONGLONG nowD = GetTickCount64();
            if (nowD - lastD >= 1000) {
                lastD = nowD;
                COTW_LOG("[mvprobe] per-frame dejit row3 diff: max %.3f, "
                         ">0.1 on %ld of %ld frames. Zero big frames while "
                         "still = matrices truly stable; many = the capture "
                         "alternates between cameras at frame rate (aliased "
                         "invisible at 4 Hz).",
                         dRow3Max, dBigCount, dFrames);
                dRow3Max = 0.0f;
                dBigCount = 0;
                dFrames = 0;
            }
        }
        // The depth staging ring, created from the engine's own depth desc so
        // the format always matches whatever it is using.
        if (g_mvStage[0] && !g_depthStage[0]) {
            ID3D11ShaderResourceView* dsrv = nullptr;
            ctx->PSGetShaderResources(3, 1, &dsrv);
            if (dsrv) {
                Rel<ID3D11Resource> dres;
                dsrv->GetResource(dres.Put());
                ID3D11Texture2D* dtex = nullptr;
                if (dres && SUCCEEDED(dres.Get()->QueryInterface(
                                __uuidof(ID3D11Texture2D), (void**)&dtex)) && dtex) {
                    D3D11_TEXTURE2D_DESC dd{};
                    dtex->GetDesc(&dd);
                    dtex->Release();
                    Rel<ID3D11Device> dv;
                    ctx->GetDevice(dv.Put());
                    if (dv) {
                        D3D11_TEXTURE2D_DESC sd{};
                        sd.Width = 1; sd.Height = 1; sd.MipLevels = 1;
                        sd.ArraySize = 1; sd.Format = dd.Format;
                        sd.SampleDesc.Count = 1;
                        sd.Usage = D3D11_USAGE_STAGING;
                        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                        for (int i = 0; i < 3; ++i)
                            if (FAILED(dv.Get()->CreateTexture2D(&sd, nullptr,
                                                                 &g_depthStage[i]))) {
                                for (int j = 0; j < 3; ++j)
                                    if (g_depthStage[j]) {
                                        g_depthStage[j]->Release();
                                        g_depthStage[j] = nullptr;
                                    }
                                break;
                            }
                    }
                }
                dsrv->Release();
            }
        }
        if (g_depthStage[0]) {
            if (g_mvStageFilled >= 2) {
                const int rd = (g_mvStageIdx + 1) % 3;
                D3D11_MAPPED_SUBRESOURCE dm{};
                if (SUCCEEDED(ctx->Map(g_depthStage[rd], 0, D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT, &dm)) && dm.pData) {
                    g_depthAtCentre = *reinterpret_cast<const float*>(dm.pData);
                    ctx->Unmap(g_depthStage[rd], 0);
                }
            }
            ID3D11ShaderResourceView* dsrv = nullptr;
            ctx->PSGetShaderResources(3, 1, &dsrv);
            if (dsrv) {
                Rel<ID3D11Resource> dres;
                dsrv->GetResource(dres.Put());
                if (dres) {
                    D3D11_BOX db{ w / 2, h / 2, 0, w / 2 + 1, h / 2 + 1, 1 };
                    ctx->CopySubresourceRegion(g_depthStage[g_mvStageIdx], 0, 0, 0, 0,
                                               dres.Get(), 0, &db);
                }
                dsrv->Release();
            }
        }
        if (g_mvStage[0]) {
            if (g_mvStageFilled >= 2) {
                const int readIdx = (g_mvStageIdx + 1) % 3;   // oldest slot
                D3D11_MAPPED_SUBRESOURCE mm{};
                if (SUCCEEDED(ctx->Map(g_mvStage[readIdx], 0, D3D11_MAP_READ,
                                       D3D11_MAP_FLAG_DO_NOT_WAIT, &mm)) && mm.pData) {
                    const uint16_t* hp = (const uint16_t*)mm.pData;
                    const float mx = HalfToFloat(hp[0]);
                    const float my = HalfToFloat(hp[1]);
                    ctx->Unmap(g_mvStage[readIdx], 0);
                    static ULONGLONG lastLog = 0;
                    static float maxAbs = 0.0f;
                    const float mag = fabsf(mx) > fabsf(my) ? fabsf(mx) : fabsf(my);
                    if (mag > maxAbs) maxAbs = mag;
                    const ULONGLONG nowT = GetTickCount64();
                    if (nowT - lastLog >= 250) {
                        lastLog = nowT;
                        const float turn = ForwardTurnDegrees(g_vpCur[0], g_vpPrev[0]);
                        // The frame-to-frame matrix difference, AFTER the
                        // de-jitter strip - the fingerprint that separates the
                        // wobble candidates. Translation row at eye-separation
                        // scale = eye mix-up; small x/y-column drift = jitter
                        // residue; smooth translation = camera bob.
                        float cD[16], pD[16];
                        memcpy(cD, g_vpCur[0], 64);
                        memcpy(pD, g_vpPrev[0], 64);
                        if (Cfg().taa_dejitter) { StripJitter(cD); StripJitter(pD); }
                        float dMax = 0.0f;
                        int dIdx = -1;
                        for (int i = 0; i < 16; ++i) {
                            const float d = fabsf(cD[i] - pD[i]);
                            if (d > dMax) { dMax = d; dIdx = i; }
                        }
                        // *** WHAT KIND OF ERROR IS IT? ***
                        //
                        // Push the screen centre through the reprojection at a
                        // NEAR and a FAR depth. A camera-ROTATION error moves
                        // both by the same amount; a camera-POSITION error
                        // moves the near one far more than the far one (the
                        // shift is offset/depth). Reverse-Z with infinite far,
                        // so z_ndc = near/depth: 0.001 is ~9 m, 0.0001 ~94 m.
                        float rp[16];
                        float nearPx = 0.0f, farPx = 0.0f, nearPy = 0.0f;
                        // Same matrix, same texel, THE SHADER'S OWN DEPTH -
                        // the only input the CPU check was not sharing.
                        float atDepthX = 0.0f, atDepthY = 0.0f;
                        if (BuildReprojection(0, rp)) {
                            const float zr[4] = {0.0f, 0.0f, g_depthAtCentre, 1.0f};
                            float pr[4] = {0, 0, 0, 0};
                            for (int c = 0; c < 4; ++c)
                                for (int r = 0; r < 4; ++r)
                                    pr[c] += zr[r] * rp[r * 4 + c];
                            if (fabsf(pr[3]) > 1e-9f) {
                                atDepthX = (pr[0] / pr[3]) * 0.5f * (float)w;
                                atDepthY = (pr[1] / pr[3]) * 0.5f * (float)h;
                            }
                            const float zs[2] = {0.001f, 0.0001f};
                            float outp[2] = {0.0f, 0.0f};
                            float outy = 0.0f;
                            for (int s = 0; s < 2; ++s) {
                                const float ndc[4] = {0.0f, 0.0f, zs[s], 1.0f};
                                float pr[4] = {0, 0, 0, 0};
                                for (int c = 0; c < 4; ++c)
                                    for (int r = 0; r < 4; ++r)
                                        pr[c] += ndc[r] * rp[r * 4 + c];
                                if (fabsf(pr[3]) > 1e-9f) {
                                    outp[s] = (pr[0] / pr[3]) * 0.5f * (float)w;
                                    if (s == 0) outy = (pr[1] / pr[3]) * 0.5f * (float)h;
                                }
                            }
                            nearPx = outp[0];
                            farPx = outp[1];
                            nearPy = outy;
                        }
                        if (Cfg().taa_mv_probe >= 2) {
                            // The shader is reporting: x = depth*1000,
                            // y = the delta it computed at that same pixel.
                            const float sz = mx * 0.001f;
                            const float metres = (sz > 1e-7f) ? (0.0094f / sz) : 1e9f;
                            // What the matrix says at exactly that depth.
                            float cpuX = 0.0f;
                            if (BuildReprojection(0, rp)) {
                                const float zr2[4] = {0.0f, 0.0f, sz, 1.0f};
                                float pr2[4] = {0, 0, 0, 0};
                                for (int c = 0; c < 4; ++c)
                                    for (int r = 0; r < 4; ++r)
                                        pr2[c] += zr2[r] * rp[r * 4 + c];
                                if (fabsf(pr2[3]) > 1e-9f)
                                    cpuX = (pr2[0] / pr2[3]) * 0.5f * (float)w;
                            }
                            COTW_LOG("[mvprobe] DEPTH MODE: centre z=%.6f "
                                     "(~%.1f m) | shader's own delta.x %+7.2f "
                                     "px | matrix at that depth %+7.2f px -> "
                                     "%s",
                                     sz, metres, my, cpuX,
                                     (fabsf(my - cpuX) > 0.5f + 0.3f * fabsf(cpuX))
                                         ? "STILL DISAGREE at the same depth - "
                                           "the shader's reconstruction is the bug"
                                         : "agree - depth explained it, the "
                                           "matrix carries the error");
                        } else {
                        const float dz = g_depthAtCentre;
                        COTW_LOG("[mvprobe] GPU MV (%+7.2f,%+7.2f) px | CPU at "
                                 "the SAME depth (z=%.6f, ~%.1f m) "
                                 "(%+7.2f,%+7.2f) px -> %s | near(9m) %+.2f, "
                                 "far(94m) %+.2f",
                                 mx, my, dz, (dz > 1e-7f) ? (0.0094f / dz) : 0.0f,
                                 atDepthX, atDepthY,
                                 (fabsf(mx - atDepthX) > 0.5f + 0.3f * fabsf(atDepthX))
                                     ? "THEY DISAGREE - the shader is not doing "
                                       "what the matrix says"
                                     : "they agree - the matrix IS the error",
                                 nearPx, farPx);
                        }
                        maxAbs = 0.0f;
                    }
                }
            }
            D3D11_BOX box{ w / 2, h / 2, 0, w / 2 + 1, h / 2 + 1, 1 };
            ctx->CopySubresourceRegion(g_mvStage[g_mvStageIdx], 0, 0, 0, 0,
                                       g_mvTex, 0, &box);
            g_mvStageIdx = (g_mvStageIdx + 1) % 3;
            if (g_mvStageFilled < 2) ++g_mvStageFilled;
        }
    }

    InterlockedIncrement(&g_resolved[eye]);
    return true;
}

void ReleaseOurTextures() {
    for (int e = 0; e < 2; ++e) {
        if (g_ourSrv[e]) { g_ourSrv[e]->Release(); g_ourSrv[e] = nullptr; }
        if (g_ourTex[e]) { g_ourTex[e]->Release(); g_ourTex[e] = nullptr; }
    }
    g_ready = false;
}

// Called only from the matched draw, so allocation AND release both happen on
// the thread that draws. That is deliberate: freeing a texture from the frame
// boundary while another thread might still be binding it is a race, and this
// way the question never arises. The cost is that turning the feature off does
// not hand the memory back - it stops the substitution, which is what the off
// switch is for.
bool EnsureTexturesFor(ID3D11DeviceContext* ctx, ID3D11Resource* source) {
    if (!source) { InterlockedIncrement(&g_subExit[kSubNoHistory]); return false; }
    Rel<ID3D11Resource> res;
    source->AddRef();
    res.p = source;
    Rel<ID3D11Texture2D> tex;
    if (FAILED(res.Get()->QueryInterface(__uuidof(ID3D11Texture2D),
                                         (void**)tex.Put())) || !tex) {
        InterlockedIncrement(&g_subExit[kSubNoHistory]);
        return false;
    }
    D3D11_TEXTURE2D_DESC d{};
    tex.Get()->GetDesc(&d);

    if (g_ready && d.Width == g_texW && d.Height == g_texH && d.Format == g_texFmt)
        return true;

    if (g_ready) {
        COTW_LOG("[taa] the engine's history changed shape (%ux%u -> %ux%u) - "
                 "rebuilding both private textures", g_texW, g_texH, d.Width, d.Height);
        ReleaseOurTextures();
        g_allocFailed = false;
    }
    if (g_allocFailed) return false;

    Rel<ID3D11Device> dev;
    ctx->GetDevice(dev.Put());
    if (!dev) {
        InterlockedIncrement(&g_subExit[kSubNoDevice]);
        g_allocFailed = true;
        return false;
    }

    bool ok = true;
    for (int e = 0; e < 2 && ok; ++e) {
        // d VERBATIM. Not a desc we wrote out by hand from remembered fields.
        ok = SUCCEEDED(dev.Get()->CreateTexture2D(&d, nullptr, &g_ourTex[e])) &&
             g_ourTex[e] != nullptr;
        if (ok)
            ok = SUCCEEDED(dev.Get()->CreateShaderResourceView(g_ourTex[e], nullptr,
                                                               &g_ourSrv[e])) &&
                 g_ourSrv[e] != nullptr;
    }
    if (!ok) {
        COTW_LOG("[taa] could not create the per-eye history textures (%ux%u fmt %u) "
                 "- the feature stays OFF and the game renders exactly as before",
                 d.Width, d.Height, (unsigned)d.Format);
        ReleaseOurTextures();
        InterlockedIncrement(&g_subExit[kSubCreateFailed]);
        g_allocFailed = true;
        return false;
    }

    // Seed both from the engine's current history so the first frame of each eye
    // is a picture rather than black.
    for (int e = 0; e < 2; ++e) ctx->CopyResource(g_ourTex[e], res.Get());

    g_texW = d.Width; g_texH = d.Height; g_texFmt = d.Format;
    g_ready = true;
    const double mb = (double)d.Width * d.Height * 4.0 / (1024.0 * 1024.0);
    COTW_LOG("[taa] per-eye history ON: two private %ux%u %s textures created from "
             "the engine's own description, %.1f MB each, seeded from the engine's "
             "current history.", d.Width, d.Height, FmtName(d.Format), mb);
    return true;
}

bool EnsureTextures(ID3D11DeviceContext* ctx, ID3D11ShaderResourceView* historySrv) {
    if (!historySrv) { InterlockedIncrement(&g_subExit[kSubNoHistory]); return false; }
    Rel<ID3D11Resource> res;
    historySrv->GetResource(res.Put());
    if (!res) { InterlockedIncrement(&g_subExit[kSubNoHistory]); return false; }
    return EnsureTexturesFor(ctx, res.Get());
}

void ResetWindow() {
    g_frames = 0;
    for (auto& h : g_histo) h = 0;
    g_pairEyes01 = g_pairEyesSame = 0;
    g_pairSameHistory = g_pairDiffHistory = 0;
    g_pairSameTarget = g_pairDiffTarget = 0;
    g_pingPongFlips = g_pingPongStuck = 0;
    for (int i = 0; i < kExitCount; ++i) InterlockedExchange(&g_exit[i], 0);
}

void PrintMatch(int n, const Match& m) {
    COTW_LOG("[taa]   #%d  eye %d  thread %lu  ctx %p  %s", n, m.eye,
             (unsigned long)m.tid, m.ctx, m.deferred ? "DEFERRED" : "immediate");
    COTW_LOG("[taa]        target   res %p  %ux%u  %s(%u)  mips %u arr %u samples %u "
             "bind 0x%02X usage %u  [view fmt %s]",
             m.rtv0.res, m.rtv0.w, m.rtv0.h, FmtName(m.rtv0.fmt), (unsigned)m.rtv0.fmt,
             m.rtv0.mips, m.rtv0.arr, m.rtv0.samples, m.rtv0.bind, m.rtv0.usage,
             FmtName(m.rtvViewFmt));
    COTW_LOG("[taa]        colour   res %p  %ux%u  %s",
             m.srv[0].res, m.srv[0].w, m.srv[0].h, FmtName(m.srv[0].fmt));
    COTW_LOG("[taa]        HISTORY  res %p  %ux%u  %s   <-- the texture the fix "
             "has to duplicate per eye",
             m.srv[1].res, m.srv[1].w, m.srv[1].h, FmtName(m.srv[1].fmt));
    COTW_LOG("[taa]        motion   res %p  %ux%u  %s",
             m.srv[2].res, m.srv[2].w, m.srv[2].h, FmtName(m.srv[2].fmt));
    COTW_LOG("[taa]        depth    res %p  %ux%u  %s   (not part of the rule - "
             "recorded because FSR 2 / DLSS need it)",
             m.srv[3].res, m.srv[3].w, m.srv[3].h, FmtName(m.srv[3].fmt));
    COTW_LOG("[taa]        viewport %.0f,%.0f  %.0fx%.0f  depth %.2f..%.2f",
             m.vp.TopLeftX, m.vp.TopLeftY, m.vp.Width, m.vp.Height,
             m.vp.MinDepth, m.vp.MaxDepth);
    bool found704 = false;
    for (int i = 0; i < 6; ++i) {
        if (!m.cb[i]) continue;
        COTW_LOG("[taa]        PS cb%d   %p  %u bytes%s", i, m.cb[i], m.cbBytes[i],
                 m.cbBytes[i] == 704 ? "   <-- THE SHARED CAMERA BLOCK" : "");
        if (m.cbBytes[i] == 704) found704 = true;
    }
    if (!found704)
        COTW_LOG("[taa]        *** no 704-byte block bound to this draw - the "
                 "reprojection matrix patched at Unmap CANNOT be reaching it ***");
}

// *** THE JITTER HUNT, STEP ONE. ***
//
// Just Cause 3 - same engine - jitters with TWO alternating sub-pixel offsets
// (-0.25 +0.25 and +0.25 -0.25), driven from CPU code and applied in the vertex
// shader. Applied in the vertex shader means it lands in the world->clip matrix,
// and we already capture that per eye per frame.
//
// So the measurement needs no byte patching and no AOB scan: HOLD THE CAMERA
// STILL and diff consecutive matrices. With a static camera, whatever differs IS
// the jitter. Diffing against two frames back as well settles whether it is the
// same 2-phase pattern - if N matches N-2 but not N-1, that is a two-entry
// sequence, and the elements that carry it are the ones to read (and later, to
// take over).
//
// This is also the measurement that decides whether DLSS is worth building. Under
// full-rate stereo the engine advances that sequence PER RENDER, so each eye may
// well get the same offset every frame - which is no sub-pixel variation at all,
// and would mean DLSS lands at denoiser quality until we drive the jitter
// ourselves.
// *** DOES THE HEAD'S ROTATION REACH THE MATRIX WE REPROJECT WITH? ***
//
// The owner's observation: with the gamepad stick, trees behave; with head
// movement they displace and drag, as though glued to the head. The stick turns
// the GAME's camera, which lands in this matrix. The head's rotation is injected
// by the mod at the camera lever - and if it does not land here identically from
// frame to frame, the reprojection under-compensates and content follows the
// head. Small under our own two-frame resolve, glaring under DLSS's long
// accumulation, which is exactly the pattern reported.
//
// No judgement of the picture is needed. The w row (m[3], m[7], m[11]) is the
// view FORWARD direction and is unit length, so the angle between consecutive
// frames IS how far the camera turned. Turn only the head, then only the stick,
// and compare the two numbers.
float ForwardTurnDegrees(const float* a, const float* b) {
    const float ax = a[3], ay = a[7], az = a[11];
    const float bx = b[3], by = b[7], bz = b[11];
    const float la = sqrtf(ax*ax + ay*ay + az*az);
    const float lb = sqrtf(bx*bx + by*by + bz*bz);
    if (la < 1e-6f || lb < 1e-6f) return -1.0f;
    float d = (ax*bx + ay*by + az*bz) / (la * lb);
    if (d > 1.0f) d = 1.0f;
    if (d < -1.0f) d = -1.0f;
    return acosf(d) * 57.2957795f;
}

void JitterReport() {
    for (int e = 0; e < 2; ++e) {
        if (!g_haveJit[e]) continue;
        if (g_haveCandPrev[e]) {
            ++g_candCount[e];
            for (int k = 0; k < 4; ++k) {
                const float t = ForwardTurnDegrees(g_candCur[e][k], g_candPrev[e][k]);
                if (t > 0.0f) g_candTurn[e][k] += t;
            }
            if (e == 0)
                COTW_LOG("[head] which camera follows you? +0x000 %.4f | +0x1D0 "
                         "%.4f | +0x210 %.4f | +0x260 %.4f  (mean deg/frame over "
                         "%d). Turn ONLY your head: the one that is NOT ~0 is the "
                         "view the frame was really rendered with.",
                         g_candTurn[e][0] / g_candCount[e],
                         g_candTurn[e][1] / g_candCount[e],
                         g_candTurn[e][2] / g_candCount[e],
                         g_candTurn[e][3] / g_candCount[e], g_candCount[e]);
        }
        const float turn = ForwardTurnDegrees(g_jitCur[e], g_jitPrev[e]);
        if (turn >= 0.0f) {
            g_turnAccum[e] += turn;
            if (turn > g_turnMax[e]) g_turnMax[e] = turn;
            g_turnCount[e] += 1;
            COTW_LOG("[head] eye %d: the camera matrix turned %.4f deg this frame "
                     "(mean %.4f, max %.4f over %d samples)", e, turn,
                     g_turnCount[e] ? g_turnAccum[e] / g_turnCount[e] : 0.0f,
                     g_turnMax[e], g_turnCount[e]);
        }
    }
    for (int e = 0; e < 2; ++e) {
        if (!g_haveJit[e]) continue;
        float d1 = 0.0f, d2 = 0.0f;
        int i1 = -1, i2 = -1;
        for (int i = 0; i < 16; ++i) {
            const float a = fabsf(g_jitCur[e][i] - g_jitPrev[e][i]);
            if (a > d1) { d1 = a; i1 = i; }
            if (g_haveJit2[e]) {
                const float b = fabsf(g_jitCur[e][i] - g_jitPrev2[e][i]);
                if (b > d2) { d2 = b; i2 = i; }
            }
        }
        COTW_LOG("[jitter] eye %d: vs LAST frame max diff %.7f at element %d | "
                 "vs TWO frames back %.7f at element %d", e, d1, i1, d2, i2);

        // *** SEPARATE THE JITTER FROM THE MOTION, ARITHMETICALLY. ***
        //
        // With a drift d per frame and a jitter alternating +/-j:
        //     now - two-back = 2d          (the alternation cancels)
        //     now - last     = d +/- 2j
        // so d = (now - twoback)/2 and |j| = |(now - last) - d| / 2.
        //
        // That holds however fast the head is moving, as long as the motion is
        // roughly steady across three frames - which is why it works in the
        // headset where reading the numbers by eye does not. Flat, this came out
        // at 1.478 on m[12]: a quarter of a pixel, exactly Just Cause 3's.
        //
        // THE ANSWER WE NEED: ~1.478 means this eye's jitter alternates and DLSS
        // has real sub-pixel variation to work with. ~0 means this eye gets the
        // SAME offset every time it renders - the degenerate case - and we would
        // have to drive the jitter ourselves before DLSS is worth anything.
        if (g_haveJit3[e]) {
            // *** THIRD DIFFERENCE - MOTION-PROOF UP TO ACCELERATION. ***
            //
            // The three-frame estimator assumed the head moved at a STEADY
            // speed. Real head motion accelerates, and that acceleration landed
            // straight in the jitter figure: readings scattered 0.07 to 0.92
            // against a flat-screen truth of 1.478.
            //
            // For a signal x = p(n) + (-1)^n * j, the third difference
            //     x[n] - 3x[n-1] + 3x[n-2] - x[n-3]
            // annihilates ANY p up to quadratic - position, velocity AND
            // acceleration all cancel - and leaves exactly 8j from the
            // alternating part. So the jitter falls out of a moving head
            // without asking anyone to hold still.
            const float x0 = g_jitCur[e][12];
            const float x1 = g_jitPrev[e][12];
            const float x2 = g_jitPrev2[e][12];
            const float x3 = g_jitPrev3[e][12];
            const float j = fabsf(x0 - 3.0f * x1 + 3.0f * x2 - x3) / 8.0f;
            const float d = (x0 - x2) * 0.5f;
            g_jitAccum[e] += j;
            g_jitCount[e] += 1;
            COTW_LOG("[jitter]   eye %d m[12]: drift %.4f/frame, JITTER %.4f  "
                     "(mean %.4f over %d samples)  [third difference]", e, d, j,
                     g_jitCount[e] ? g_jitAccum[e] / g_jitCount[e] : 0.0f,
                     g_jitCount[e]);
        }
        // Name the elements that move, with their values, so the pattern is
        // readable rather than inferred from one magnitude. A two-entry jitter
        // shows up as now and two-back matching while last differs.
        for (int i = 0; i < 16; ++i) {
            const float a = fabsf(g_jitCur[e][i] - g_jitPrev[e][i]);
            if (a < 1e-9f || a < d1 * 0.01f) continue;
            COTW_LOG("[jitter]    m[%2d]  now %.7f  last %.7f  two-back %.7f  "
                     "(now-last %.7f, now-twoback %.7f)",
                     i, g_jitCur[e][i], g_jitPrev[e][i], g_jitPrev2[e][i],
                     g_jitCur[e][i] - g_jitPrev[e][i],
                     g_jitCur[e][i] - g_jitPrev2[e][i]);
        }
    }
    COTW_LOG("[jitter] Read this with the camera COMPLETELY STILL - no mouse, no "
             "head movement. Then: differs from last frame but MATCHES two frames "
             "back = a two-entry jitter, and those elements carry it. All zero = "
             "the jitter is not in this matrix and it is applied further down.");
}

void Report() {
    const long f = g_frames ? g_frames : 1;
    const long draws = (long)InterlockedExchange(&g_drawsSeen, 0);
    COTW_LOG("[taa] ---- %ld real frame(s), %ld Draw() calls looked at ----",
             g_frames, draws);
    COTW_LOG("[taa] resolves per real frame:  0 -> %ld   1 -> %ld   2 -> %ld   "
             "3+ -> %ld", g_histo[0], g_histo[1], g_histo[2], g_histo[3]);

    const bool fullRate = Cfg().full_rate_stereo;
    if (g_histo[0] == g_frames) {
        COTW_LOG("[taa] NOTHING MATCHED. Either the game's anti-aliasing does not "
                 "include TAA (GraphicsAA must be 2 or 3), or the fingerprint has "
                 "stopped being true for this build. The funnel below says which - "
                 "read the first line that is NOT 'not a 3-vertex Draw'.");
    } else if (fullRate && g_histo[2] == 0 && g_histo[1] > 0) {
        COTW_LOG("[taa] ONE resolve per frame under full-rate stereo. That "
                 "CONTRADICTS the model in TAA_PER_EYE_PLAN.md section 3 - the "
                 "second eye's replay is not reaching this pass. Do not build the "
                 "per-eye feature on this reading; find out why first.");
    } else if (!fullRate && g_histo[1] > 0) {
        COTW_LOG("[taa] One resolve per frame, and full-rate stereo is off - that "
                 "is the expected shape for alternate-eye rendering. Each frame "
                 "resolves one eye against a history holding the other.");
    }

    if (g_pairEyes01 || g_pairEyesSame) {
        COTW_LOG("[taa] eye tags on the pair:   {0,1} %ld frame(s)   both the same "
                 "eye %ld frame(s)%s", g_pairEyes01, g_pairEyesSame,
                 g_pairEyesSame ? "   <-- attribution is WRONG on those frames" : "");
    }
    if (g_pairSameHistory || g_pairDiffHistory) {
        COTW_LOG("[taa] the two resolves read the SAME history texture in %ld "
                 "frame(s), DIFFERENT in %ld", g_pairSameHistory, g_pairDiffHistory);
        COTW_LOG("[taa] and they WRITE the same target in %ld, different in %ld",
                 g_pairSameTarget, g_pairDiffTarget);
        if (g_pairSameHistory > g_pairDiffHistory) {
            COTW_LOG("[taa]   ANSWER: both eyes blend against ONE history. Each "
                     "eye's 'previous frame' is the other eye, 64 mm away. That is "
                     "the ghost, and per-eye duplication is the fix.");
        } else {
            COTW_LOG("[taa]   ANSWER: the two eyes read DIFFERENT history textures - "
                     "the engine's ping-pong parity flips between the eye passes. "
                     "The same duplication fixes it; expect the artefact to be "
                     "ASYMMETRIC, which the owner can confirm by closing one eye.");
        }
    }
    if (g_pingPongFlips || g_pingPongStuck) {
        COTW_LOG("[taa] frame to frame the history texture changed %ld time(s) and "
                 "stayed the same %ld - the engine's two-texture ping-pong is %s",
                 g_pingPongFlips, g_pingPongStuck,
                 g_pingPongFlips > g_pingPongStuck ? "alive, as captured"
                                                   : "NOT alternating, which the "
                                                     "captures say it should be");
    }

    long total = 0;
    for (int i = 0; i < kExitCount; ++i) total += g_exit[i];
    if (total > 0) {
        COTW_LOG("[taa] funnel, per real frame (every Draw() call accounted for):");
        for (int i = 0; i < kExitCount; ++i) {
            const long n = g_exit[i];
            if (!n) continue;
            COTW_LOG("[taa]   %8.2f  %s", (double)n / (double)f, kExitName[i]);
        }
    }

    // The table holds the frame that has just finished, not the whole window -
    // g_matchCount is already back to zero by the time this runs, which is why
    // the count is carried in its own variable rather than read from it.
    const LONG shown = g_lastFrameMatches < kMaxMatches ? g_lastFrameMatches
                                                        : kMaxMatches;
    if (shown > 0) {
        COTW_LOG("[taa] the last frame's resolves in full:");
        for (LONG i = 0; i < shown; ++i) PrintMatch((int)i, g_match[i]);
    }
}

// A FEATURE THAT CANNOT SAY WHETHER IT IS WORKING IS THE ONE THAT WASTED TWO
// HEADSET SESSIONS. This runs whether or not the probe is armed, once a second,
// for as long as the feature is on.
void ReplaceReport() {
    static bool prevOn = false;
    static ULONGLONG last = 0;
    static uint64_t frames = 0;
    const bool on = Cfg().taa_replace_pass;
    if (!on) {
        if (prevOn) {
            prevOn = false;
            COTW_LOG("[taa] the engine's temporal pass is running again.");
        }
        return;
    }
    if (!prevOn) {
        prevOn = true;
        last = GetTickCount64();
        frames = 0;
        COTW_LOG("[taa] STAGE 1: intercepting the engine's temporal resolve and "
                 "passing the frame through untouched. Expect a sharp, aliased, "
                 "ghost-free picture - shimmer on foliage is the point, not a "
                 "fault. If it looks like anything else, the interception is wrong.");
    }
    ++frames;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    const long r0 = InterlockedExchange(&g_replaced[0], 0);
    const long r1 = InterlockedExchange(&g_replaced[1], 0);
    const long bad = InterlockedExchange(&g_replaceFailed, 0);
    const long q0 = InterlockedExchange(&g_resolved[0], 0);
    const long q1 = InterlockedExchange(&g_resolved[1], 0);
    const long m0 = InterlockedExchange(&g_mvWritten[0], 0);
    const long m1 = InterlockedExchange(&g_mvWritten[1], 0);
    if (Cfg().taa_write_motion_vectors)
        COTW_LOG("[taa]   motion vectors written: %ld / %ld  (%s)", m0, m1,
                 g_mvTex ? "target ready" : "NO TARGET - DLSS cannot be fed");
    if (Cfg().dlss_enable) {
        const long d0 = InterlockedExchange(&g_dlssRan[0], 0);
        const long d1 = InterlockedExchange(&g_dlssRan[1], 0);
        COTW_LOG("[taa]   DLSS evaluated: %ld / %ld  (%s). Zero on both with the "
                 "switch on means it declined every frame and you are looking at "
                 "the mod's own resolve - see the [dlss] lines for why.",
                 d0, d1, DlssAvailable() ? "NGX up" : "NGX NOT AVAILABLE");
        if (Cfg().taa_head_rotation_fix) {
            const long ap = InterlockedExchange(&g_headFixApplied, 0);
            const long na = InterlockedExchange(&g_headFixNoAngles, 0);
            const long nt = InterlockedExchange(&g_headFixNoTangents, 0);
            const long np = InterlockedExchange(&g_headFixNoPrev, 0);
            COTW_LOG("[taa]   head fix gates this second: APPLIED %ld | angles "
                     "unavailable %ld | tangents unavailable %ld | no previous "
                     "sample %ld | last shift (%.5f, %.5f) UV. Applied ~0 while "
                     "the head is clearly turning = the named gate is the dead "
                     "one. Applied high but last shift ~0.00000 = the delta "
                     "itself is zero and the angle source is not what renders "
                     "the view.", ap, na, nt, np, g_headFixLastX, g_headFixLastY);
        }
        if (Cfg().dlss_jitter_mode == 1)
            COTW_LOG("[taa]   jitter told to DLSS: eye0 (%+.3f, %+.3f) px%s | "
                     "eye1 (%+.3f, %+.3f) px%s  (locked after %d/%d sane "
                     "samples; expect ~0.26 on x. All zero = not locked yet, "
                     "and DLSS is being told zero - honest but the old, wrong "
                     "state.)",
                     g_jitOutPx[0][0], g_jitOutPx[0][1],
                     g_jitOutValid[0] ? "" : " [NOT LOCKED]",
                     g_jitOutPx[1][0], g_jitOutPx[1][1],
                     g_jitOutValid[1] ? "" : " [NOT LOCKED]",
                     g_jitMagN[0][0], g_jitMagN[1][0]);
        if (Cfg().jitter_take) {
            float tx0 = 0, ty0 = 0, tx1 = 0, ty1 = 0;
            const bool live0 = JitterTakeNow(0, &tx0, &ty0);
            JitterTakeNow(1, &tx1, &ty1);
            COTW_LOG("[taa]   jitter takeover: %ld generator calls/s%s, eye0 "
                     "(%+.3f, %+.3f) px, eye1 (%+.3f, %+.3f) px. ~2 calls per "
                     "frame = both eyes covered; 0 = the generator is not "
                     "running (menu?) or the hook failed - see [take] lines.",
                     JitterTakeCallsLastSecond(),
                     live0 ? "" : " [NOT LIVE]", tx0, ty0, tx1, ty1);
        }
        if (Cfg().taa_jitter_inject) {
            float m15lo = 0.0f, m15hi = 0.0f;
            long skipped = 0;
            JitterOverlayM15Stats(&m15lo, &m15hi, &skipped);
            COTW_LOG("[taa]   jitter overlay: %ld camera blocks/s injected, "
                     "this frame's overlay (%+.3f, %+.3f) px | |m15| %.1f .. "
                     "%.1f, %ld cameras SKIPPED for small m15. Skips or a tiny "
                     "m15 minimum while facing the FLICKERY heading = the "
                     "injection scale is collapsing there and the report no "
                     "longer matches the render.",
                     JitterOverlayInjectedLastSecond(),
                     g_ovlCur[0][0], g_ovlCur[0][1], m15lo, m15hi, skipped);
        }
    }
    COTW_LOG("[taa] pass replaced: eye 0 %ld, eye 1 %ld, over %llu frame(s)  |  "
             "resolved by our own shader: %ld / %ld  (%s)",
             r0, r1, (unsigned long long)frames, q0, q1,
             Cfg().taa_resolve ? (g_shaderReady ? "stage 2"
                                                : "stage 2 asked for, shaders NOT ready")
                               : "stage 1, pass-through");
    if (bad)
        COTW_LOG("[taa]   %ld fail-closed exit(s) - the engine resolved those "
                 "itself, so the picture is mixed and proves nothing.", bad);
    // The VALUE, not just the delta. The delta read 0.0000 while walking and the
    // "not read yet" marker was absent - so the number is arriving and simply
    // not changing, which the delta alone could never distinguish from a number
    // that was never read. Print what it actually is.
    COTW_LOG("[taa]   camera eye0 (%.2f, %.2f, %.2f)  eye1 (%.2f, %.2f, %.2f)  "
             "moved %.4f / %.4f between frames%s",
             g_camCur[0][0], g_camCur[0][1], g_camCur[0][2],
             g_camCur[1][0], g_camCur[1][1], g_camCur[1][2],
             g_camDelta[0], g_camDelta[1],
             (g_haveCamPrev[0] || g_haveCamPrev[1]) ? "" : "  [NOT READ YET]");
    COTW_LOG("[taa]   (world-scale numbers that change as you walk = the right "
             "position. Near-zero or constant = the position lever is not the "
             "origin these matrices use, and walking will trail.)");
    const long va = InterlockedExchange(&g_vpAccepted[0], 0);
    const long vr = InterlockedExchange(&g_vpRejected[0], 0);
    const long va1 = InterlockedExchange(&g_vpAccepted[1], 0);
    const long vr1 = InterlockedExchange(&g_vpRejected[1], 0);
    COTW_LOG("[taa]   camera matrix: accepted %ld/%ld, REJECTED %ld/%ld as "
             "belonging to another render (shadow, reflection, viewmodel).",
             va, va1, vr, vr1);
    if (Cfg().taa_camera_lock) {
        const long used0 = InterlockedExchange(&g_pickUsed[0], 0);
        const long used1 = InterlockedExchange(&g_pickUsed[1], 0);
        const long empty0 = InterlockedExchange(&g_pickEmpty[0], 0);
        const long empty1 = InterlockedExchange(&g_pickEmpty[1], 0);
        COTW_LOG("[taa]   camera SELECT: used %ld/%ld per s (none %ld/%ld) | "
                 "eye0: %d offered, chosen written %dx of best %dx, others up "
                 "to %.0f away, moved %.3f raw / %.4f WITHOUT JITTER | eye1: "
                 "chosen %dx of %dx, moved %.3f / %.4f. Chosen must equal "
                 "best (the main view is written for nearly every draw); "
                 "standing still the without-jitter figure must be ~0.000.",
                 used0, used1, empty0, empty1,
                 g_pickCand[0], g_pickHits[0], g_pickMaxHits[0], g_pickSpread[0],
                 g_pickMoved[0], g_pickMovedDejit[0],
                 g_pickHits[1], g_pickMaxHits[1],
                 g_pickMoved[1], g_pickMovedDejit[1]);
        COTW_LOG("[taa]   ...the chosen camera moved %.4f / %.4f units WITHIN "
                 "the pass (eye0/eye1) - the engine refining it between "
                 "writes. That is the few-mm error that reads as several "
                 "pixels on close geometry; the last write is now the one "
                 "kept.", g_pickWobble[0], g_pickWobble[1]);
    }
    if (vr || vr1)
        COTW_LOG("[taa]     last reject had scales %.4f / %.4f; the player's "
                 "camera measures %.4f / %.4f. If those are close, the test is "
                 "too strict and it is throwing away good frames.",
                 g_rejA, g_rejB, g_wantA, g_wantB);
    if (!r0 && !r1)
        COTW_LOG("[taa]   NOTHING was replaced. The switch is on and the pass is "
                 "not being reached; the picture you are looking at is stock. "
                 "FIRST THING TO CHECK: the GAME's own Anti-aliasing setting. "
                 "This feature works by taking over the engine's temporal "
                 "resolve, and with the game's AA off there is no such pass to "
                 "take over - so this, and DLSS with it, can do nothing. It "
                 "wants a TAA mode (FXAA+TAA).");
    frames = 0;
}

void FeatureReport() {
    static bool prevOn = false;
    static ULONGLONG last = 0;
    const bool on = Cfg().per_eye_temporal_history;

    if (!on) {
        if (prevOn) {
            prevOn = false;
            COTW_LOG("[taa] per-eye history OFF - the engine's own history is in "
                     "use again, exactly as before. (The two textures stay "
                     "allocated; freeing them from this thread would be a race.)");
        }
        return;
    }
    if (!prevOn) {
        prevOn = true;
        last = GetTickCount64();
        InterlockedExchange(&g_subFrames, 0);
        InterlockedExchange(&g_subApplied[0], 0);
        InterlockedExchange(&g_subApplied[1], 0);
        COTW_LOG("[taa] per-eye history requested. Expect ~1 substitution per eye "
                 "per frame. If the two counts below are not roughly equal, the "
                 "feature is doing something WORSE than the bug and should go off.");
    }

    InterlockedIncrement(&g_subFrames);
    const ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;

    const long fr = InterlockedExchange(&g_subFrames, 0);
    const long a0 = InterlockedExchange(&g_subApplied[0], 0);
    const long a1 = InterlockedExchange(&g_subApplied[1], 0);
    COTW_LOG("[taa] per-eye history: eye 0 substituted %ld time(s), eye 1 %ld, over "
             "%ld frame(s)  [%s]", a0, a1, fr,
             g_ready ? "textures ready" : "NOT READY - running stock");
    if (a0 == 0 && a1 == 0) {
        COTW_LOG("[taa]   NOTHING was substituted. The switch is on and the pass is "
                 "not being reached - so any judgement of the picture right now "
                 "says nothing about the idea. Check the funnel with taa_probe.");
    } else if (a0 == 0 || a1 == 0) {
        COTW_LOG("[taa]   ONE EYE ONLY. That is worse than the bug - turn the "
                 "feature off until this line reads two.");
    }
    for (int i = 0; i < kSubExitCount; ++i) {
        const long n = InterlockedExchange(&g_subExit[i], 0);
        if (n) COTW_LOG("[taa]   %ld fail-closed exit(s): %s", n, kSubExitName[i]);
    }
    if (Cfg().per_eye_temporal_eye_swap)
        COTW_LOG("[taa]   *** eye_swap is ON - each eye is being fed the WRONG "
                 "eye's history on purpose. The picture SHOULD look bad.");
    if (Cfg().per_eye_temporal_starve)
        COTW_LOG("[taa]   *** starve is ON - this frame's own colour is being fed "
                 "back as history, which should visibly kill all smoothing. If it "
                 "does not, the substitution is not reaching the shader and a null "
                 "result from the feature means nothing.");
}

}  // namespace

// ---------------------------------------------------------------------------

bool TaaWantsDraws() {
    return Cfg().taa_probe || Cfg().per_eye_temporal_history ||
           Cfg().taa_replace_pass;
}

void TaaOnDraw(ID3D11DeviceContext* ctx, UINT vertexCount) {
    // FIRST, ALWAYS. A resolve that some other feature swallows between here and
    // the issue point must not leave a substitution standing for the next draw.
    ClearPending();
    if (t_inOurDraw) return;        // our own resolve, arriving back at the hook

    const bool probing = (g_armed != 0) || (g_waiting != 0);
    const bool feature = Cfg().per_eye_temporal_history;
    const bool replacing = Cfg().taa_replace_pass;
    if ((!probing && !feature && !replacing) || !ctx) return;
    InterlockedIncrement(&g_drawsSeen);

    // Clause order is the capture's funnel order, cheapest first. 94 of a
    // frame's ~2100 draws arrive here at all, and most die on this first line.
    if (vertexCount != 3) { InterlockedIncrement(&g_exit[kNotThreeVerts]); return; }

    RtvPair rtv;
    Rel<ID3D11DepthStencilView> dsv;
    ctx->OMGetRenderTargets(2, rtv.v, dsv.Put());

    // *** THE VELOCITY PASS, ON THE WAY PAST. ***
    //
    // The resolve carries no reprojection matrix - all 28 of its constant-buffer
    // slots were enumerated and the only camera among them is the CURRENT one,
    // on VS cb0. So it reprojects from the motion-vector texture, and the camera
    // that matters belongs to whatever WRITES those vectors.
    //
    // The capture says that is a single full-screen draw: Draw(3), TWO render
    // targets, no depth, the second one full-res R16G16_FLOAT. Same call, same
    // hook, one extra branch - no new surface in the busy draw paths.
    if (probing && rtv.v[0] && rtv.v[1] && !dsv) {
        const TexInfo mv = DescribeView(rtv.v[1]);
        if (mv.valid && mv.fmt == kMotionFmt) {
            const int veye = (CurrentRenderEye() == 0) ? 0 : 1;
            CBNoteVelocityPass(ctx, veye);      // every frame - it is the test
            static ULONGLONG lastVel = 0;
            const ULONGLONG now = GetTickCount64();
            if (now - lastVel >= 1000) {
                lastVel = now;
                CBScanResolveConstants(ctx, veye,
                                       "VELOCITY PASS (writes the motion vectors)");
            }
        }
    }

    if (!rtv.v[0] || rtv.v[1]) { InterlockedIncrement(&g_exit[kRtvCount]); return; }
    if (dsv) { InterlockedIncrement(&g_exit[kDsvBound]); return; }

    const TexInfo rt = DescribeView(rtv.v[0]);
    if (!rt.valid) { InterlockedIncrement(&g_exit[kRtvNotTex2D]); return; }
    if (rt.fmt != kColourFmt) { InterlockedIncrement(&g_exit[kRtvFormat]); return; }

    SrvSet srv;
    ctx->PSGetShaderResources(0, 4, srv.v);
    const TexInfo s0 = DescribeView(srv.v[0]);
    const TexInfo s1 = DescribeView(srv.v[1]);
    const TexInfo s2 = DescribeView(srv.v[2]);
    const TexInfo s3 = DescribeView(srv.v[3]);

    if (!SameSize(s0, rt) || s0.fmt != kColourFmt) {
        InterlockedIncrement(&g_exit[kSrv0]); return;
    }
    if (!SameSize(s1, rt) || s1.fmt != kColourFmt) {
        InterlockedIncrement(&g_exit[kSrv1]); return;
    }
    // The clause that makes it history rather than a second read of the output.
    if (s1.res == rt.res) { InterlockedIncrement(&g_exit[kSrv1IsRtv]); return; }
    if (!SameSize(s2, rt) || s2.fmt != kMotionFmt) {
        InterlockedIncrement(&g_exit[kSrv2]); return;
    }

    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&vpCount, vps);
    if (!vpCount || vps[0].TopLeftX != 0.0f || vps[0].TopLeftY != 0.0f ||
        (UINT)vps[0].Width != rt.w || (UINT)vps[0].Height != rt.h) {
        InterlockedIncrement(&g_exit[kViewport]); return;
    }

    InterlockedIncrement(&g_exit[kMatched]);
    const int eye = (CurrentRenderEye() == 0) ? 0 : 1;

    // THE WORLD IS ON SCREEN - the resolve only exists when it is. Start here,
    // not at launch, and not at whatever moment a switch happened to be flipped.
    if (g_waiting) {
        InterlockedExchange(&g_waiting, 0);
        InterlockedExchange(&g_armed, 1);
        ResetWindow();
        InterlockedExchange(&g_drawsSeen, 0);
        g_armedAt = GetTickCount64();
        g_lastReport = g_armedAt;
        COTW_LOG("[taa] ============ PROBE ARMED - the world is rendering "
                 "============");
        COTW_LOG("[taa] Armed on the first TAA resolve seen, not at launch: a probe "
                 "that starts with the process records the main menu, where this "
                 "pass does not exist. Runs for %d s from now.",
                 Cfg().taa_probe_seconds);
    }

    // The probe's record. Claiming a slot can fail on a frame with more resolves
    // than the table holds; the SUBSTITUTION below must not fail with it, so the
    // two are independent.
    const LONG slot = InterlockedIncrement(&g_matchCount) - 1;
    if (slot >= 0 && slot < kMaxMatches) {
        D3D11_RENDER_TARGET_VIEW_DESC rvd{};
        rtv.v[0]->GetDesc(&rvd);
        Match& m = g_match[slot];
        m.eye      = eye;
        m.tid      = GetCurrentThreadId();
        m.ctx      = ctx;
        m.deferred = (ctx->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED) ? 1 : 0;
        m.rtvViewFmt = rvd.Format;
        m.rtv0     = rt;
        m.srv[0] = s0; m.srv[1] = s1; m.srv[2] = s2; m.srv[3] = s3;
        m.vp       = vps[0];
        // Descriptions only - the buffer's SIZE, never its contents. Reading the
        // contents would mean a Map, and that is the call that broke the weapon.
        ID3D11Buffer* cbs[6] = {};
        ctx->PSGetConstantBuffers(0, 6, cbs);
        for (int i = 0; i < 6; ++i) {
            m.cb[i] = cbs[i];
            m.cbBytes[i] = 0;
            if (cbs[i]) {
                D3D11_BUFFER_DESC bd{};
                cbs[i]->GetDesc(&bd);
                m.cbBytes[i] = bd.ByteWidth;
                cbs[i]->Release();
            }
        }
    } else {
        InterlockedIncrement(&g_exit[kTableFull]);
    }

    // WHERE DOES THIS PASS GET ITS REPROJECTION FROM? Measured once a second
    // while the probe is armed, never in normal play: it walks up to 8 KB per
    // bound buffer and prints several lines, which is a diagnostic's budget, not
    // a per-draw one.
    if (probing) {
        static ULONGLONG lastScan = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastScan >= 1000) {
            lastScan = now;
            CBScanResolveConstants(ctx, eye, "TAA RESOLVE");
        }
    }

    // --- STAGE 1: REPLACE THE PASS -------------------------------------------
    //
    // Luma does exactly this for Just Cause 3 on the same engine family: it does
    // not correct the game's temporal pass, it intercepts the draw, skips it, and
    // resolves the frame itself. Whoever owns the pass owns the history, and a
    // history we own is per-eye by construction - no ping-pong to outwit and no
    // previous-frame matrix to find, which matters because that matrix is not in
    // any constant buffer. It lives on the CPU.
    //
    // This stage does the interception and nothing else: the current colour is
    // copied into the target the engine would have resolved into. Sharp, aliased,
    // ghost-free, and worth exactly one thing - proof that the interception is
    // correct before a line of shader is written.
    if (replacing) {
        t_pending.active = true;
        t_pending.replace = true;
        t_pending.eye = eye;
        return;
    }

    // --- and now the feature -------------------------------------------------
    //
    // FAIL CLOSED. If the textures are not there, nothing is substituted and the
    // frame renders exactly as it does today. Never one eye substituted and the
    // other not - that is worse than the bug it is fixing.
    if (!feature) return;
    if (!EnsureTextures(ctx, srv.v[1])) return;
    t_pending.active = true;
    t_pending.eye    = eye;
}

bool TaaResolvePending() { return t_pending.active; }

bool TaaReplacePass(ID3D11DeviceContext* ctx, TaaDrawFn origDraw) {
    Pending& p = t_pending;
    if (!p.replace) return false;
    p.replace = false;
    p.active = false;
    if (!ctx) return false;

    // Read both ends off the pipeline rather than remembering them: this runs
    // immediately before the draw would have, so what is bound IS what the draw
    // would have used.
    ID3D11RenderTargetView* rtvRaw = nullptr;
    ctx->OMGetRenderTargets(1, &rtvRaw, nullptr);
    Rel<ID3D11RenderTargetView> rtv{ rtvRaw };
    ID3D11ShaderResourceView* srvRaw = nullptr;
    ctx->PSGetShaderResources(0, 1, &srvRaw);
    Rel<ID3D11ShaderResourceView> srv{ srvRaw };
    if (!rtv || !srv) { InterlockedIncrement(&g_replaceFailed); return false; }

    Rel<ID3D11Resource> dst, src;
    rtv.Get()->GetResource(dst.Put());
    srv.Get()->GetResource(src.Put());
    if (!dst || !src || dst.Get() == src.Get()) {
        InterlockedIncrement(&g_replaceFailed);
        return false;                       // fail closed: let the engine resolve
    }

    // This eye's world->clip matrix. Two routes, and the second one exists
    // because the first was measured to be a lottery (see below).
    {
        // *** ROUTE A: SELECT FROM THE PASS'S OWN CANDIDATES. ***
        // Runs first and independently of what is bound, because the binding
        // is exactly what could not be trusted.
        bool captured = false;
        if (Cfg().taa_camera_lock) {
            float picked[16];
            CamPick info{};
            if (PickCameraCandidate(p.eye, g_vpPrev[p.eye], g_haveVpPrev[p.eye],
                                    picked, &info)) {
                memcpy(g_vpCur[p.eye], picked, 64);
                g_haveVpCur[p.eye] = true;
                captured = true;
                float pa = 0.0f, pb = 0.0f, pn = 0.0f;
                if (CBIsCameraClip(picked, &pa, &pb, &pn)) {
                    g_projA[p.eye] = pa;
                    g_projB[p.eye] = pb;
                    g_haveProj[p.eye] = true;
                }
                g_pickCand[p.eye] = info.candidates;
                g_pickMoved[p.eye] = info.moved;
                g_pickSpread[p.eye] = info.spread;
                g_pickHits[p.eye] = info.hits;
                g_pickMaxHits[p.eye] = info.maxHits;
                g_pickWobble[p.eye] = info.wobble;
                // The same distance with the JITTER TAKEN OUT of both. The
                // engine folds the jitter into the translation row as
                // j * m[15], and with m[15] ~ 8500 a half-pixel jitter is
                // several units - which is the whole of the raw figure above
                // at rest. This one is the honest "did the camera move".
                if (g_haveVpPrev[p.eye]) {
                    float pc[16], pp[16];
                    memcpy(pc, picked, 64);
                    memcpy(pp, g_vpPrev[p.eye], 64);
                    StripJitter(pc);
                    StripJitter(pp);
                    float d = 0.0f;
                    for (int k = 12; k < 16; ++k) {
                        const float e = fabsf(pc[k] - pp[k]);
                        if (e > d) d = e;
                    }
                    g_pickMovedDejit[p.eye] = d;
                }
                InterlockedIncrement(&g_pickUsed[p.eye]);
                InterlockedIncrement(&g_vpAccepted[p.eye]);
                JitterOverlayNow(&g_ovlCur[p.eye][0], &g_ovlCur[p.eye][1]);
                JitterHuntOnCapture(p.eye, g_vpCur[p.eye]);
            } else {
                InterlockedIncrement(&g_pickEmpty[p.eye]);
            }
        }

        // *** ROUTE B: the bound block, as before. *** Still the path when the
        // selection is off, and the fallback when a pass offered no candidate
        // at all (a menu frame, or the very first frames after a load).
        ID3D11Buffer* vs0 = nullptr;
        if (!captured) ctx->VSGetConstantBuffers(0, 1, &vs0);
        if (vs0) {
            D3D11_BUFFER_DESC bd{};
            vs0->GetDesc(&bd);
            const uint8_t* data = nullptr;
            UINT bytes = 0;
            if (bd.ByteWidth == 704 && CBSnapshotFor(vs0, &data, &bytes) &&
                bytes >= 64) {
                // *** CHECK IT IS THE MAIN VIEW BEFORE BELIEVING IT. ***
                //
                // Measured while standing perfectly still: m[12] swung
                // -14311 -> -11967 -> +3328 -> +14153 across four frames, and
                // m[15] - which must be ~0 for this camera - came back as
                // -12600. Those are OTHER CAMERAS. The engine rewrites this
                // block about ninety times a frame for shadow cascades,
                // reflections and the viewmodel, and taking whatever the
                // snapshot happens to hold means reprojecting some frames
                // through a shadow camera. That trails, and it trails
                // intermittently, which is the worst kind to chase.
                const float* cand = reinterpret_cast<const float*>(data);
                float a = 0.0f, b = 0.0f, n = 0.0f;
                bool ok = CBIsCameraClip(cand, &a, &b, &n);
                // And it must be THE PLAYER'S camera, not merely a valid clip
                // matrix - a shadow projection is one of those too.
                //
                // Judged against the mod's own Stage 0 measurement of the
                // camera's projection, which is read back from the GPU and
                // agrees with itself 21338 times out of 21338. NOT against the
                // first matrix that happened to pass: that latched the baseline
                // onto whatever arrived first, and if it was a shadow camera it
                // then rejected every real one - accepted 0, rejected 227 a
                // second. Learning a reference from an unverified sample is the
                // trap this project has paid for repeatedly.
                float tanH = 0.0f, tanV = 0.0f;
                if (ok && MeasuredCameraTangents(&tanH, &tanV) &&
                    tanH > 1e-4f && tanV > 1e-4f) {
                    const float wantA = 1.0f / tanH;
                    const float wantB = 1.0f / tanV;
                    if (fabsf(a - wantA) > 0.10f * wantA ||
                        fabsf(b - wantB) > 0.10f * wantB) {
                        ok = false;
                        g_rejA = a; g_rejB = b;
                        g_wantA = wantA; g_wantB = wantB;
                    }
                }
                // *** SELECT THE CONTINUOUS CAMERA (taa_camera_lock v2). ***
                //
                // What is bound here is incidental - this pass does not read a
                // camera at all - and the block is rewritten ~90 times a frame
                // by cameras that share the player's tangents. Measured: the
                // captured matrix wandered by metres on 87 of 90 frames at
                // rest. So do not judge the bound one: take the whole set of
                // main-view cameras collected during this eye's pass and pick
                // the one continuous with last frame's choice. See
                // cbscan.cpp's NoteCameraCandidate for the full story; v1's
                // rejection starved and is gone.
                if (ok) {
                    memcpy(g_vpCur[p.eye], data, 64);
                    g_haveVpCur[p.eye] = true;
                    // The overlay these very bytes were rendered with - the
                    // injection is constant across a frame, so reading it now
                    // is reading what was applied.
                    JitterOverlayNow(&g_ovlCur[p.eye][0], &g_ovlCur[p.eye][1]);
                    // The hunt gets the freshest possible truth: the matrix
                    // the GPU is provably using for this very draw.
                    JitterHuntOnCapture(p.eye, g_vpCur[p.eye]);
                    // All four cameras in the block, for the head hunt.
                    if (bytes >= 0x2A0) {
                        for (int k = 0; k < 4; ++k)
                            memcpy(g_candCur[p.eye][k], data + kCandOffsets[k], 64);
                        g_haveCand[p.eye] = true;
                    }
                    g_projA[p.eye] = a;
                    g_projB[p.eye] = b;
                    g_haveProj[p.eye] = true;
                    InterlockedIncrement(&g_vpAccepted[p.eye]);
                } else {
                    // Keep the last good one rather than reproject through a
                    // camera that belongs to something else.
                    InterlockedIncrement(&g_vpRejected[p.eye]);
                }
            }
            vs0->Release();
        }
        // And the camera position those matrices are relative to - from the
        // position lever, not from a constant buffer. GlobalConstants +0x40 was
        // tried first because that is where the engine's own motion-vector
        // shader reads it, and it moved 0.0000 between frames while walking.
        float cam[3];
        if (EyeWorldPosition(p.eye, cam)) {
            memcpy(g_camCur[p.eye], cam, 12);
            g_haveCamCur[p.eye] = true;
        }
    }

    // --- STAGE 4: DLSS, if it is wanted and everything it needs is here ------
    //
    // Runs BEFORE our own resolve and returns false for any reason at all, in
    // which case the resolve below runs exactly as it does today. One feature
    // per eye, so each eye owns its own history and the cross-eye problem this
    // whole effort has been about cannot occur.
    //
    // It still needs our motion vectors, which our own shader writes - so the
    // resolve runs FIRST to produce them, and DLSS then supersedes its colour
    // output. Slightly wasteful; correct, and one thing at a time.
    // *** WITH UPSCALING ON, DLSS MUST RUN ONCE - NOT TWICE. ***
    //
    // The capture-time upscale is a full DLSS evaluate with its own history. If
    // this DLAA evaluate also runs, every eye goes through TWO temporal
    // accumulations per frame: DLAA at render size, then reconstruction on top
    // of its output. That costs twice and looks worse than either alone -
    // accumulating an already-accumulated image is how detail is smoothed away,
    // and it is the first suspect for the owner's "the image was so low" on the
    // first upscaling run. So when the upscale is doing the work, this stage
    // steps aside and our own cheap shader resolve below runs instead - which is
    // also what writes the motion vectors the upscale needs.
    // *** ASK WHAT IS HAPPENING, NOT WHAT IS CONFIGURED. ***
    //
    // This read the quality percentage, which is a statement about the NEXT
    // launch - so moving the quality row mid-game switched this DLAA pass on
    // and off underneath a headset-side upscale that had not changed at all.
    // Switching to DLAA stacked two temporal passes and went blurry; switching
    // back removed one and read as shimmer. Neither had anything to do with the
    // level chosen. The only thing that matters here is whether the mod is
    // ACTUALLY reconstructing at the capture, which VRUpscalingNow() answers
    // from the sizes in force this frame.
    const bool upscaling = VRUpscalingNow();
    if (Cfg().dlss_enable && DlssAvailable() && Cfg().taa_write_motion_vectors &&
        !(upscaling && Cfg().dlss_upscale_solo)) {
        const TexInfo t = DescribeView(rtv.Get());
        ID3D11ShaderResourceView* depthSrv = nullptr;
        ctx->PSGetShaderResources(3, 1, &depthSrv);
        Rel<ID3D11ShaderResourceView> depthKeep{ depthSrv };
        Rel<ID3D11Resource> depthRes;
        if (depthSrv) depthSrv->GetResource(depthRes.Put());
        if (t.valid && depthRes && g_mvTex) {
            Rel<ID3D11Device> dev;
            ctx->GetDevice(dev.Put());
            if (dev && EnsureShaders(dev.Get()) &&
                EnsureTexturesFor(ctx, dst.Get()) &&
                RunOurResolve(ctx, p.eye, t.w, t.h, origDraw)) {
                static bool firstFrame[2] = {true, true};
                static ULONGLONG lastEval[2] = {0, 0};

                // The jitter, per the mode: 0 reports zero (the old behaviour,
                // kept as the control), 1 reads it from the matrices we already
                // capture, 2 hands through the ini values verbatim.
                float jx = 0.0f, jy = 0.0f;
                if (Cfg().dlss_jitter_mode == 1) {
                    // TWO SOURCES, and the default is the one the owner's
                    // "clean, almost perfect" verdict was given on.
                    //
                    // dlss_jitter_exact = 1: extract it from THIS eye's
                    // captured matrix - mathematically exact and the honest
                    // answer now that the engine is known to run 16 phases
                    // (the estimator's two-phase model is fiction). It was
                    // introduced in the same evening that DLSS lost its clean
                    // look, so it is unvalidated and OFF until it has been
                    // A/B'd alone from a good baseline. Note it is only as
                    // good as the captured matrix, which is why the camera
                    // selection above had to come first.
                    // dlss_jitter_exact = 0: the estimator, exactly as the
                    // known-good build ran it.
                    bool got = false;
                    if (Cfg().dlss_jitter_exact && g_haveVpCur[p.eye])
                        got = JitterFromMatrix(g_vpCur[p.eye], t.w, t.h, &jx, &jy);
                    if (!got)
                        EstimateJitterPixels(p.eye, t.w, t.h, &jx, &jy);
                    jx *= Cfg().dlss_jitter_scale;
                    jy *= Cfg().dlss_jitter_scale;
                    // Keep the once-a-second log truthful: it prints these.
                    g_jitOutPx[p.eye][0] = jx;
                    g_jitOutPx[p.eye][1] = jy;
                    g_jitOutValid[p.eye] = true;
                } else if (Cfg().dlss_jitter_mode == 2) {
                    jx = Cfg().dlss_jitter_x;
                    jy = Cfg().dlss_jitter_y;
                }

                // Reset discipline (guide 3.13, and Luma resets on any frame
                // with no 3D scene): if this eye has not evaluated for half a
                // second the world was not being drawn - a menu, a loading
                // screen - and history must not bridge that gap.
                const ULONGLONG nowTick = GetTickCount64();
                const bool gap =
                    lastEval[p.eye] && (nowTick - lastEval[p.eye] > 500);
                if (DlssEvaluate(ctx, p.eye, src.Get(), depthRes.Get(),
                                 g_mvTex, dst.Get(), t.w, t.h,
                                 jx, jy,
                                 firstFrame[p.eye] || gap)) {
                    firstFrame[p.eye] = false;
                    lastEval[p.eye] = nowTick;
                    if (g_ourTex[p.eye]) ctx->CopyResource(g_ourTex[p.eye], dst.Get());
                    InterlockedIncrement(&g_replaced[p.eye]);
                    InterlockedIncrement(&g_dlssRan[p.eye]);
                    return true;
                }
                // DLSS declined - our resolve already wrote the frame, so keep
                // it rather than resolving twice.
                if (g_ourTex[p.eye]) ctx->CopyResource(g_ourTex[p.eye], dst.Get());
                InterlockedIncrement(&g_replaced[p.eye]);
                return true;
            }
        }
    }

    // --- STAGE 2: resolve it ourselves, against THIS EYE's own history -------
    if (Cfg().taa_resolve) {
        const TexInfo t = DescribeView(rtv.Get());
        Rel<ID3D11Device> dev;
        ctx->GetDevice(dev.Put());
        if (t.valid && dev && EnsureShaders(dev.Get()) &&
            EnsureTexturesFor(ctx, dst.Get()) &&
            RunOurResolve(ctx, p.eye, t.w, t.h, origDraw)) {
            // This eye keeps what we just resolved, and nothing else can read it.
            if (g_ourTex[p.eye]) ctx->CopyResource(g_ourTex[p.eye], dst.Get());
            InterlockedIncrement(&g_replaced[p.eye]);
            return true;
        }
        // Anything missing and we fall through to the pass-through below rather
        // than leaving the frame half-done.
    }

    // --- STAGE 1: pass through -----------------------------------------------
    // Both are the same W x H R11G11B10_FLOAT texture - the fingerprint that got
    // us here checked exactly that - so a whole-resource copy is legal and needs
    // no shader.
    ctx->CopyResource(dst.Get(), src.Get());
    InterlockedIncrement(&g_replaced[p.eye]);
    return true;                            // the engine's resolve does NOT run
}

void TaaSubstituteBegin(ID3D11DeviceContext* ctx) {
    Pending& p = t_pending;
    if (!p.active || !ctx) return;

    // Both slots in one call: 1 to save and restore, 0 because the starve
    // control feeds THIS FRAME'S COLOUR back in as history, which must visibly
    // kill all temporal accumulation if the substitution is reaching the shader.
    ID3D11ShaderResourceView* cur[2] = { nullptr, nullptr };
    ctx->PSGetShaderResources(0, 2, cur);
    p.saved = cur[1];                       // reference kept; released in End

    ID3D11ShaderResourceView* use = g_ourSrv[p.eye];
    if (Cfg().per_eye_temporal_eye_swap) use = g_ourSrv[p.eye ? 0 : 1];
    if (Cfg().per_eye_temporal_starve && cur[0]) use = cur[0];
    ctx->PSSetShaderResources(1, 1, &use);

    if (cur[0]) cur[0]->Release();          // the context holds its own reference
}

void TaaSubstituteEnd(ID3D11DeviceContext* ctx) {
    Pending& p = t_pending;
    if (!p.active) return;
    p.active = false;

    if (ctx) {
        // EXACT restore, including the case where the game had nothing bound -
        // our constants applied to whatever the engine draws next is the failure
        // mode that made the weapon shift leak into the world.
        ID3D11ShaderResourceView* restore = p.saved;
        ctx->PSSetShaderResources(1, 1, &restore);

        // This eye keeps its own resolved image. Read back off the pipeline
        // rather than remembered from Begin: it is the target the draw actually
        // wrote, and it costs two calls on two draws a frame.
        ID3D11RenderTargetView* rtv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, nullptr);
        if (rtv) {
            ID3D11Resource* res = nullptr;
            rtv->GetResource(&res);
            if (res) {
                if (g_ourTex[p.eye]) ctx->CopyResource(g_ourTex[p.eye], res);
                res->Release();
            }
            rtv->Release();
        }
        InterlockedIncrement(&g_subApplied[p.eye]);
    }

    if (p.saved) { p.saved->Release(); p.saved = nullptr; }
}

// What the headset-side upscale needs, published from the resolve. Null until
// the resolve has run for that eye, which is exactly when the upscale must
// fall back to a plain copy.
ID3D11Resource* TaaMotionVectors() { return g_mvTex; }
ID3D11Resource* TaaDepthForEye(int eye) {
    return g_upDepth[(eye == 0) ? 0 : 1];
}
bool TaaJitterForEye(int eye, unsigned w, unsigned h, float* jx, float* jy) {
    const int e = (eye == 0) ? 0 : 1;
    if (!g_haveVpCur[e]) return false;
    return JitterFromMatrix(g_vpCur[e], w, h, jx, jy);
}

void TaaOnFrame() {
    // The jitter overlay steps to its next Halton entry here, at the frame
    // boundary - never mid-frame, because every draw of a frame must agree on
    // where the sub-pixel sample sits.
    JitterOverlayAdvance();
    // Candidates never outlive their frame: a pass that did not resolve must
    // not offer its cameras to the next one.
    CameraCandidatesReset();
    JitterHuntTick();
    JitterTakeInstall();     // lazy, once, only when the switch is on
    JitterMode3Tick();       // force the engine's 16-phase table, when asked

    // *** THE ROLL RUNS EVERY FRAME, UNCONDITIONALLY, AND IT GOES FIRST. ***
    //
    // It was briefly moved to the end of this function so the jitter report
    // could see a real previous frame - and everything below here returns early
    // when the probe is off, so the previous matrix stopped ageing the moment
    // the probe disarmed. Frozen for the rest of the session: the reprojection
    // silently reads a matrix minutes old, and the camera delta sticks at
    // whatever it last was. A diagnostic must never sit upstream of the feature.
    //
    // The jitter report gets what it needs from a copy taken here instead,
    // BEFORE the roll, which is the same three frames without moving the roll.
    for (int e = 0; e < 2; ++e) {
        if (g_haveVpCur[e]) {
            memcpy(g_jitCur[e], g_vpCur[e], 64);
            memcpy(g_jitPrev[e], g_vpPrev[e], 64);
            memcpy(g_jitPrev2[e], g_vpPrev2[e], 64);
            memcpy(g_jitPrev3[e], g_vpPrev3[e], 64);
            if (g_haveCand[e]) {
                // Compared BEFORE this roll, in the report below, so it is one
                // frame apart and not a value against a copy of itself - the
                // mistake that voided the first jitter reading.
                for (int k = 0; k < 4; ++k)
                    memcpy(g_candPrev[e][k], g_candCur[e][k], 64);
                g_haveCandPrev[e] = true;
            }
            g_haveJit[e] = g_haveVpPrev[e];
            g_haveJit2[e] = g_haveVpPrev2[e];
            g_haveJit3[e] = g_haveVpPrev3[e];

            if (g_haveVpPrev2[e]) {
                memcpy(g_vpPrev3[e], g_vpPrev2[e], 64);
                g_haveVpPrev3[e] = true;
                g_ovlPrev3[e][0] = g_ovlPrev2[e][0];
                g_ovlPrev3[e][1] = g_ovlPrev2[e][1];
            }
            if (g_haveVpPrev[e]) {
                memcpy(g_vpPrev2[e], g_vpPrev[e], 64);
                g_haveVpPrev2[e] = true;
                g_ovlPrev2[e][0] = g_ovlPrev[e][0];
                g_ovlPrev2[e][1] = g_ovlPrev[e][1];
            }
            memcpy(g_vpPrev[e], g_vpCur[e], 64);
            g_haveVpPrev[e] = true;
            g_ovlPrev[e][0] = g_ovlCur[e][0];
            g_ovlPrev[e][1] = g_ovlCur[e][1];
        }
        if (g_haveCamCur[e]) {
            memcpy(g_camPrev[e], g_camCur[e], 12);
            g_haveCamPrev[e] = true;
        }
        if (g_haveHeadCur[e]) {
            if (g_haveHeadPrev[e]) {
                g_headYawPrev2[e] = g_headYawPrev[e];
                g_headPitchPrev2[e] = g_headPitchPrev[e];
                g_haveHeadPrev2[e] = true;
            }
            g_headYawPrev[e] = g_headYawCur[e];
            g_headPitchPrev[e] = g_headPitchCur[e];
            g_haveHeadPrev[e] = true;
        }
    }

    ReplaceReport();      // stage 1 - never silent
    FeatureReport();      // independent of the probe, and never throttled away

    static bool prevOn = false;
    const bool on = Cfg().taa_probe;

    // *** NEVER ARM FROM THE INI. ***
    //
    // The probe is edge-triggered, the game SAVES its config on exit, and so a
    // probe left on wrote taa_probe = 1 into the ini and armed itself 1.5 seconds
    // into the next launch - burning its fifteen seconds on the main menu, where
    // there is no TAA resolve to find, and reporting "nothing matched" as though
    // that meant something. The HUD probe's own help text warns about exactly
    // this and I built the same trap anyway.
    static bool firstCall = true;
    if (firstCall) {
        firstCall = false;
        prevOn = on;                    // adopt the ini value without acting on it
        if (on) {
            InterlockedExchange(&g_waiting, 1);
            COTW_LOG("[taa] taa_probe is ON in the ini. NOT arming yet - it will "
                     "arm by itself on the first TAA resolve, i.e. when the world "
                     "is actually being rendered. Just play; nothing to toggle.");
        }
        return;
    }

    if (!on && g_waiting) {
        InterlockedExchange(&g_waiting, 0);
        COTW_LOG("[taa] probe switched off before it armed.");
    }

    if (on && !prevOn) {
        prevOn = true;
        ResetWindow();
        InterlockedExchange(&g_matchCount, 0);
        InterlockedExchange(&g_drawsSeen, 0);
        g_prevFirstHistory = nullptr;
        g_armedAt = GetTickCount64();
        g_lastReport = g_armedAt;
        InterlockedExchange(&g_armed, 1);
        COTW_LOG("[taa] ============ TAA RESOLVE PROBE ARMED ============");
        COTW_LOG("[taa] Read-only: Get*/GetDesc and pointer identity only. No Map, "
                 "no CopyResource, no readback, no draw changed. It stops by itself "
                 "after %d s; re-arm by turning it off and on again.",
                 Cfg().taa_probe_seconds);
        COTW_LOG("[taa] full_rate_stereo = %d (driving right now: %s) -> expect %d "
                 "resolve(s) per real frame.", Cfg().full_rate_stereo ? 1 : 0,
                 FullRateDriving() ? "yes" : "no", Cfg().full_rate_stereo ? 2 : 1);
        COTW_LOG("[taa] Counted per REAL FRAME, not per Present: under full-rate the "
                 "Present hook runs twice and this report only runs on the second "
                 "pass, so one window covers both eye passes.");
        COTW_LOG("[taa] If nothing matches at all, check the game's anti-aliasing is "
                 "TAA or FXAA+TAA (GraphicsAA 2 or 3) before anything else.");
        return;                      // start counting from a clean frame
    }

    if (!on) {
        if (prevOn) {
            prevOn = false;
            if (g_armed) {
                InterlockedExchange(&g_armed, 0);
                COTW_LOG("[taa] probe switched off.");
            }
        }
        return;
    }
    if (!g_armed) return;            // this arming has already run its course

    // --- the frame boundary. Fold this frame's matches into the window. ------
    const LONG n = InterlockedExchange(&g_matchCount, 0);
    g_lastFrameMatches = n;
    ++g_frames;
    g_histo[n <= 0 ? 0 : (n == 1 ? 1 : (n == 2 ? 2 : 3))]++;

    if (n >= 2) {
        const Match& a = g_match[0];
        const Match& b = g_match[1];
        if (a.eye != b.eye) ++g_pairEyes01; else ++g_pairEyesSame;
        if (a.srv[1].res == b.srv[1].res) ++g_pairSameHistory; else ++g_pairDiffHistory;
        if (a.rtv0.res == b.rtv0.res) ++g_pairSameTarget; else ++g_pairDiffTarget;
    }
    if (n >= 1) {
        const void* h = g_match[0].srv[1].res;
        if (g_prevFirstHistory) {
            if (h != g_prevFirstHistory) ++g_pingPongFlips; else ++g_pingPongStuck;
        }
        g_prevFirstHistory = h;
    }

    const ULONGLONG now = GetTickCount64();
    if (now - g_lastReport >= 1000) {
        g_lastReport = now;
        Report();
        JitterReport();
        CBReportVelocityMatch();
        // The counts reset, the match table does not: it is overwritten by the
        // next frame's resolves and only ever read for the frame just finished.
        ResetWindow();
    }

    const int secs = Cfg().taa_probe_seconds;
    if (secs > 0 && now - g_armedAt >= (ULONGLONG)secs * 1000ull) {
        InterlockedExchange(&g_armed, 0);
        COTW_LOG("[taa] ============ PROBE FINISHED (%d s) ============", secs);
        COTW_LOG("[taa] Turn taa_probe off and on again to run it once more.");
    }
}

}  // namespace cotwvr
