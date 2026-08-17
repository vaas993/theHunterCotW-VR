#include "cbscan.h"

#include "config.h"
#include "log.h"
#include "stereo.h"
#include "taa.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <MinHook.h>

// Finding the weapon's view matrix in the game's constant buffers.
//
// WHY THIS AND NOT A CALL SITE: the held weapon renders in its own pass with its
// own projection (which is why the game's FOV slider never moved it), and
// sweeping ALL SIX of the engine's transform-builder call sites, 25 cm each with
// a weapon equipped, never moved the viewmodel on its own. So its transform does
// not come through those helpers. What remains is the render view itself.
//
// HOW: not by reading shader code, but by looking for numbers we already know.
// The camera's world position and basis are captured every frame at the position
// lever, so any constant buffer the game uploads can be searched for them. A
// buffer that contains the camera position IS a view-related buffer, whatever it
// is called and whatever shader consumes it.
//
// Map/Unmap rather than UpdateSubresource because DISCARD-mapped constant
// buffers are how a D3D11 engine of this vintage streams per-draw constants.

namespace cotwvr {
namespace {

using PFN_Map = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT,
                                            D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
using PFN_Unmap = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Resource*, UINT);
using PFN_ClearDSV = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                              ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
using PFN_DrawIndexed = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, INT);
using PFN_Draw = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT);
using PFN_DrawInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
using PFN_DrawIndexedInstanced = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, UINT, UINT,
                                                          UINT, INT, UINT);
using PFN_VSSetShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11VertexShader*,
                                                 ID3D11ClassInstance* const*, UINT);
// Needed so the answer SURVIVES. Shader indices come from discovery order, which
// can differ run to run - an index found today may mean a different shader
// tomorrow. Hashing the bytecode at creation gives a stable name for the one we
// find, so the fix can be pinned to it permanently.
using PFN_CreateVertexShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*,
                                                           SIZE_T, ID3D11ClassLinkage*,
                                                           ID3D11VertexShader**);
// The OTHER way constants reach the GPU, and on this engine evidently the main
// one: only a single projection was ever seen through Map, and it stopped being
// written at all. A D3D11 renderer of this vintage typically pushes per-draw
// constants with UpdateSubresource instead.
using PFN_UpdateSubresource = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                       ID3D11Resource*, UINT,
                                                       const D3D11_BOX*, const void*,
                                                       UINT, UINT);

PFN_Map   o_Map = nullptr;
PFN_Unmap o_Unmap = nullptr;
PFN_ClearDSV o_ClearDSV = nullptr;
PFN_DrawIndexed o_DrawIndexed = nullptr;
PFN_Draw o_Draw = nullptr;
PFN_DrawInstanced o_DrawInstanced = nullptr;
PFN_UpdateSubresource o_UpdateSubresource = nullptr;
PFN_DrawIndexedInstanced o_DrawIndexedInstanced = nullptr;
PFN_VSSetShader o_VSSetShader = nullptr;
constexpr int kSlotVSSetShader = 11;
constexpr int kSlotDrawIndexedInstanced = 20;
// The non-indexed pair, hooked for the same reason the indexed pair is: a
// viewmodel drawn through Draw() rather than DrawIndexed() would capture its
// constants normally and then miss the viewport shift entirely, which looks
// exactly like "the shift does nothing".
constexpr int kSlotDraw          = 13;
constexpr int kSlotDrawInstanced = 21;

// --- FINDING THE WEAPON BY HIDING IT ---------------------------------------
//
// Every approach so far ran FORWARDS: find a matrix, hope it belongs to the
// weapon, poke it, see nothing. This runs backwards - skip a shader's draws
// until the weapon disappears, and then there is no guessing left about which
// draws are the weapon.
//
// That is what ReShade's Shader Toggler does, and it is a far better tool for
// this than anything I built today. Doing it inside the mod rather than through
// the external tool avoids having to reproduce ReShade's shader hash to map its
// answer back to a pointer in our own process - here the answer IS the pointer.
//
// Once the weapon's vertex shader is known, its constant buffers are simply
// whatever is bound while it is active. No content matching, no layout
// guessing, no arm's-length heuristics.
constexpr int kMaxShaders = 512;
ID3D11VertexShader* g_shaders[kMaxShaders];
volatile LONG g_shaderCount = 0;
// Which shaders are bound, tracked PER CONTEXT.
//
// These were thread-local, which is the wrong model: in D3D11 the bound shader
// is state of the CONTEXT, not of the thread that touched it. Apex records its
// draws on a pool of worker threads across deferred contexts, so a thread that
// bound the weapon's shader while recording one context would then draw on
// another and see stale state. The result was that the weapon's own pixel
// shaders were recognised at a draw roughly 0.05 times per frame - the weapon
// is on screen every frame - so almost every weapon draw went unshifted while a
// few stragglers did move, which is what flickered.
struct ContextShaders {
    ID3D11DeviceContext* ctx;
    ID3D11VertexShader*  vs;
    ID3D11PixelShader*   ps;
};
constexpr int kMaxContexts = 32;
ContextShaders g_ctxState[kMaxContexts] = {};

// Slots are claimed and NEVER released, so a game that creates deferred
// contexts and drops them fills this table and then every later context gets
// nullptr - i.e. "no shader is bound" for every draw it records. That is a
// silent, permanent blindness, and it is the leading suspect for the weapon
// matching ~16 draws in 300 frames after working briefly at startup. Counted,
// because a table that quietly stops answering must not look like a weapon
// that is quietly not drawn.
volatile LONG g_ctxTableFull = 0;

ContextShaders* StateFor(ID3D11DeviceContext* ctx) {
    for (int i = 0; i < kMaxContexts; ++i) {
        if (g_ctxState[i].ctx == ctx) return &g_ctxState[i];
        if (g_ctxState[i].ctx == nullptr) {
            // Claim the slot. A lost race just means another context takes it
            // and this one tries the next, so the table stays consistent.
            void* prev = InterlockedCompareExchangePointer(
                (void* volatile*)&g_ctxState[i].ctx, ctx, nullptr);
            if (prev == nullptr || prev == ctx) return &g_ctxState[i];
        }
    }
    InterlockedIncrement(&g_ctxTableFull);
    return nullptr;
}

ID3D11PixelShader* BoundPS(ID3D11DeviceContext* ctx) {
    ContextShaders* st = StateFor(ctx);
    return st ? st->ps : nullptr;
}
ID3D11VertexShader* BoundVS(ID3D11DeviceContext* ctx) {
    ContextShaders* st = StateFor(ctx);
    return st ? st->vs : nullptr;
}


PFN_CreateVertexShader o_CreateVertexShader = nullptr;
constexpr int kSlotCreateVertexShader = 12;      // ID3D11Device vtable

struct ShaderId { void* obj; uint32_t hash; uint32_t crc; uint32_t len; bool pixel; };
constexpr int kMaxIds = 1024;
ShaderId g_ids[kMaxIds];
volatile LONG g_idCount = 0;

// ReShade (and therefore Shader Toggler) identifies a shader by a CRC32 of its
// bytecode, so computing the SAME thing here means the number shown in its UI
// can be matched directly against ours - no guessing, no translation.
uint32_t Crc32(const void* data, size_t len) {
    static uint32_t table[256];
    static bool built = false;
    if (!built) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            table[i] = c;
        }
        built = true;
    }
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

// The lens materials, identified by number at creation rather than guessed at
// draw time. Four slots because a game may build binoculars, a rifle scope and a
// pistol scope from different glass; in this game one number covers all of them.
constexpr int kLensVS = 4;
ID3D11VertexShader* g_lensVS[kLensVS] = {};
volatile LONG g_lensVSCount = 0;

void AddLensVS(ID3D11VertexShader* vs) {
    if (!vs) return;
    const LONG n = g_lensVSCount;
    for (LONG i = 0; i < n && i < kLensVS; ++i) {
        if (g_lensVS[i] == vs) return;
    }
    const LONG slot = InterlockedIncrement(&g_lensVSCount) - 1;
    if (slot >= kLensVS) return;
    vs->AddRef();               // held for the life of the process
    g_lensVS[slot] = vs;
}

uint32_t Fnv1a(const void* data, size_t len) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}

void NoteShaderId(void* obj, const void* code, SIZE_T len, bool pixel) {
    if (!obj || !code) return;
    const LONG idx = InterlockedIncrement(&g_idCount) - 1;
    if (idx < 0 || idx >= kMaxIds) return;
    g_ids[idx].obj = obj;
    g_ids[idx].hash = Fnv1a(code, len);
    g_ids[idx].crc = Crc32(code, len);
    g_ids[idx].len = (uint32_t)len;
    g_ids[idx].pixel = pixel;
}

bool ShaderFingerprint(void* obj, uint32_t* hash, uint32_t* len) {
    const LONG n = g_idCount;
    for (LONG i = 0; i < n && i < kMaxIds; ++i) {
        if (g_ids[i].obj == obj) { *hash = g_ids[i].hash; *len = g_ids[i].len; return true; }
    }
    return false;
}

// The crc32 on its own, because that is the number the owner reads out of
// Shader Toggler and the only one his hunted HUD list can be compared against.
// 0 means "this object was created before we hooked, or the table is full" -
// which is a different statement from "crc 0" and has to stay distinguishable.
uint32_t ShaderCrc(void* obj) {
    if (!obj) return 0;
    const LONG n = g_idCount;
    for (LONG i = 0; i < n && i < kMaxIds; ++i) {
        if (g_ids[i].obj == obj) return g_ids[i].crc;
    }
    return 0;
}

// Every shader the game has created, with the SAME crc32 Shader Toggler shows.
// Reported once so a hash noted in its UI can be looked up here directly.
void DumpShaderTable() {
    const LONG n = g_idCount;
    COTW_LOG("[shader] %ld shaders created. crc32 is the number Shader Toggler shows:",
             (long)n);
    for (LONG i = 0; i < n && i < kMaxIds; ++i) {
        COTW_LOG("[shader]   %-6s crc32 %08X  (%u bytes)  fnv %08X",
                 g_ids[i].pixel ? "PIXEL" : "VERTEX", g_ids[i].crc, g_ids[i].len,
                 g_ids[i].hash);
    }
}

// *** THE WEAPON, IDENTIFIED FOR CERTAIN. ***
//
// Found with ReShade's Shader Toggler: three PIXEL shaders which between them
// hide the gun, the scope and the hands. Shader Toggler identifies shaders with
// ReShade's compute_crc32 over the bytecode - the standard CRC-32 - so the same
// numbers can be recognised here directly:
//
//   0xFBECF3C0   0xF616CE07   0x7A86206F
//
// This is what every earlier approach was missing. Instead of searching for a
// matrix that might be the weapon's, a draw using one of these shaders IS the
// weapon, and its constant buffers are simply whatever is bound at that moment.
constexpr int kWeaponPS = 3;

// ALL the shader objects matching those CRCs, not one per CRC.
//
// The engine creates several ID3D11PixelShader objects from the same bytecode -
// the log shows 7A86206F identified twice within a millisecond. A single slot
// per CRC meant each new creation threw away the previous pointer, so only the
// draws using the most recently created object ever matched: 13 out of roughly
// 100,000 draws. The weapon was being drawn the whole time, just through
// objects that had been overwritten out of the table.
constexpr int kMaxWeaponPS = 64;
ID3D11PixelShader* g_weaponPS[kMaxWeaponPS] = {};
volatile LONG g_weaponPSCount = 0;

void AddWeaponPS(ID3D11PixelShader* ps) {
    if (!ps) return;
    const LONG n = g_weaponPSCount;
    for (LONG i = 0; i < n && i < kMaxWeaponPS; ++i) if (g_weaponPS[i] == ps) return;
    if (n >= kMaxWeaponPS) return;
    const LONG idx = InterlockedIncrement(&g_weaponPSCount) - 1;
    if (idx < 0 || idx >= kMaxWeaponPS) return;
    g_weaponPS[idx] = ps;
}

// The weapon's VERTEX shaders, learned from the draws where its pixel shaders
// are bound.
//
// Needed because the first-person pass draws the weapon MORE THAN ONCE: a depth
// pre-pass and then colour. Depth-only draws usually bind no pixel shader at
// all, so matching on the pixel shader alone shifted the colour and left the
// depth where it was - and mismatched depth is precisely what flickers.
constexpr int kMaxWeaponVS = 16;
ID3D11VertexShader* g_weaponVS[kMaxWeaponVS] = {};
volatile LONG g_weaponVSCount = 0;

void NoteWeaponVS(ID3D11VertexShader* vs) {
    if (!vs) return;
    const LONG n = g_weaponVSCount;
    for (LONG i = 0; i < n && i < kMaxWeaponVS; ++i) if (g_weaponVS[i] == vs) return;
    if (n >= kMaxWeaponVS) return;
    const LONG idx = InterlockedIncrement(&g_weaponVSCount) - 1;
    if (idx >= kMaxWeaponVS) return;
    g_weaponVS[idx] = vs;
    COTW_LOG("[weapon] vertex shader #%ld learned - its depth pass will be shifted too",
             (long)idx);
}

bool IsWeaponVertexShader(ID3D11VertexShader* vs) {
    if (!Cfg().weapon_shift_use_vs) return false;
    if (!vs) return false;
    const LONG n = g_weaponVSCount;
    for (LONG i = 0; i < n && i < kMaxWeaponVS; ++i) if (g_weaponVS[i] == vs) return true;
    return false;
}

// The positive control for the whole weapon feature: if this is zero, nothing
// downstream can possibly fire, and every other symptom is a consequence.
int IdentifiedWeaponShaderCount() {
    return (int)g_weaponPSCount;
}

bool IsWeaponPixelShader(ID3D11PixelShader* ps) {
    if (!ps) return false;
    const LONG n = g_weaponPSCount;
    for (LONG i = 0; i < n && i < kMaxWeaponPS; ++i) if (g_weaponPS[i] == ps) return true;
    return false;
}

// *** THE VIEWMODEL PASS, AS THE RENDERDOC CAPTURE ACTUALLY SHOWS IT. ***
//
// Ground truth from cotw_frame18616.rdc (analysed 2026-08-02; scripts in
// tools/rdc_*.py). The weapon is rendered TWICE per frame and the two copies
// are nothing alike:
//
//  1. Inside the GPU-driven world pipeline, as instanced geometry, through
//     DrawIndexedInstancedIndirect with the same uber-shaders the vegetation
//     uses. Nothing here can identify the weapon.
//  2. As a DEDICATED FORWARD PASS AFTER POST-PROCESSING - ten meshes, each
//     drawn three times through plain DrawIndexed: two depth-only rounds and
//     then colour, straight into scene colour. A snapshot either side proved
//     the scene has no weapon in it before this pass, so these thirty draws
//     ARE the visible gun, with nothing underneath to leave behind.
//
// The colour round binds exactly the three pixel shaders found with Shader
// Toggler, and a sweep of all 2,162 draws in the frame found them NOWHERE
// else - so that match is exact. The depth rounds bind no pixel shader at
// all, and their vertex shaders are shared with other dynamic objects, so
// they are matched by INDEX COUNT instead: the ten meshes are
//   {77484, 5007, 162, 6087, 3252, 2484, 4992, 5748, 5958, 9594}
// and those counts collide with nothing else in the frame.
//
// Both rounds have to move together. Shifting the colour while the depth
// stays put makes the gun fail its own depth test, which is the flicker.
// Ten meshes per weapon, and the counts change when the player swaps guns, so
// this holds several weapons' worth. It only ever fills from the colour round,
// which is an exact match, so a stale entry is a real weapon mesh - not a
// guess that could start shifting scenery.
// The weapon mesh whose count is NOT unique - 14 world draws share it - so it is
// matched by its position next to a confirmed weapon draw instead. See the
// neighbour rule in Hook_DrawIndexed.
constexpr UINT kWeaponNeighbourIdx = 162;

// *** COUNTS THAT ARE ONLY THE WEAPON WHEN THEY ARE IN THE WEAPON'S PASS. ***
//
// Seven of the seeded counts were measured unique across all 2,214 draws of a
// frame. These were not:
//   162    - 14 world draws share it, known from the start
//   10272  - the scope; also seen outside the weapon block in a frame tail
//   4992   - absent from the reference frame, so never checked
//   5748   - likewise
// and one of them is a wooden fence, which duly picked up the weapon's 3D shift
// and moved with the strength slider.
//
// Position settles it. The viewmodel pass is a contiguous run, so an ambiguous
// count sitting immediately after a CONFIRMED weapon draw is part of that run,
// while the same count out in the world has no weapon in front of it. Verified
// counts still match on their own - they have to, or nothing would ever start
// the run.
bool IsAmbiguousWeaponIdx(UINT idx) {
    return idx == 162u || idx == 10272u || idx == 4992u || idx == 5748u;
}

// *** THE PASS IS ANCHORED, AND THAT IS WHAT MAKES A COUNT MEAN ANYTHING. ***
//
// The seven "verified unique" counts were only ever verified against ONE
// captured frame in ONE place. Uniqueness there says nothing about a fence in
// some other part of the map carrying the same count - and one duly does, which
// is why a fence picked up the weapon's 3D shift and slid with the strength
// slider.
//
// The pass itself is the discriminator. Its order is fixed:
//     77484  5007  162  6087  5958  10272  3252  2484  9594
// three times over, and it ALWAYS starts with 77484 - the hands and arms, which
// are drawn whatever you are holding, which is why they were the one mesh still
// present with only the binoculars out.
//
// So 77484 arms the run and everything else is believed only while the run is
// live. A fence out in the world has no hands drawn in front of it.
constexpr UINT kWeaponPassAnchor = 77484u;

// *** THE ENGINE TAGS ITS OWN FIRST-PERSON GEOMETRY. ASK IT. ***
//
// Every heuristic before this - index counts, the pass anchor, distance from a
// depth clear - was an attempt to infer something the renderer already states
// outright. It reserves STENCIL BIT 6 as the first-person tag:
//
//   every viewmodel draw   StencilReadMask == 0x40, ref has 0x40 set
//   all 2187 world draws   never read or write bit 6. Read masks seen: 0xFF,
//                          0x04, 0x08, 0x80, 0x03. Refs seen: 0x00, 0x04, 0x06,
//                          0x08, 0x80, 0x10, 0x16, 0x12, 0x02.
//
// Zero false positives, zero false negatives, and confirmed on a SECOND capture
// (2,162 draws) so it is not an artifact of one frame. All three rounds of the
// pass carry it - depth rounds included - so meshes cannot half-vanish.
//
// It also solves the problems the index table never could: it needs no per-
// weapon seeding, it cannot be imitated by a fence that happens to draw 3252
// indices at close LOD, and it costs one virtual call with no AddRef/Release -
// cheaper than the PSGetShader this hook already makes per draw.
constexpr UINT kFirstPersonStencilBit = 0x40u;

// *** THE REF ALONE IS NOT ENOUGH - IT PERSISTS. ***
//
// OMGetDepthStencilState returns the ref CURRENTLY SET on the context, and that
// survives until something sets another. So a world draw that does not test
// stencil at all still reports whatever the viewmodel pass left behind, and
// testing the ref by itself matched 14,088 draws where the viewmodel is ~9,700 -
// dragging in scenery, the fence among it.
//
// The state object itself is what carries the intent: StencilEnable and
// StencilReadMask == 0x40. Both must hold, along with the ref bit.
//
// Cached by state pointer, so GetDesc runs once per distinct state rather than
// once per draw - engines bind a handful of these and reuse them all frame.
// *** BIT 6 HAS TWO ROLES, AND ONLY ONE OF THEM WAS EVER IMPLEMENTED. ***
//
// The gun READS the bit: ReadMask == 0x40, func NOT_EQUAL, ref & 0x40 - "draw
// only where the bit is clear". That is what this file has matched all along.
//
// The scope's lens mask WRITES it: WriteMask == 0x40, func ALWAYS, ref & 0x40,
// PassOp == REPLACE. It stamps a circle into the stencil buffer and paints
// nothing at all - no pixel shader, no render target, DepthEnable FALSE. A
// RenderDoc stencil readback of the sniper capture found it as one draw:
//
//     eid 17030 - after it, bit 0x40 is SET from screen centre out to NDC
//     0.6167 and CLEAR beyond: a circle, dead centre, radius 1192px on a
//     3854x2800 target. PixelHistory then shows the shroud (eid 17627) failing
//     the stencil test INSIDE that circle and passing outside it.
//
// So the disc is not glass, and no amount of refining the glass rules could ever
// have found it: it has no colour, no blending, no depth and no target. It is
// the stamp the lens picture is cut out of. Move the gun and leave the stamp
// where it is, and the gun gets clipped against a circle that did not move -
// which is precisely the crescent eaten out of the shroud.
//
// Both roles now count as first-person, so the stamp is shifted with the thing
// it cuts.
struct DssCache {
    ID3D11DepthStencilState* dss;
    bool reads;     // the gun
    bool writes;    // the lens mask
};
constexpr int kMaxDss = 64;
DssCache g_dssCache[kMaxDss] = {};
volatile LONG g_dssCount = 0;

void FirstPersonStencilRoles(ID3D11DepthStencilState* dss, bool* reads, bool* writes) {
    const LONG n = g_dssCount;
    for (LONG i = 0; i < n && i < kMaxDss; ++i) {
        if (g_dssCache[i].dss == dss) {
            *reads = g_dssCache[i].reads;
            *writes = g_dssCache[i].writes;
            return;
        }
    }
    D3D11_DEPTH_STENCIL_DESC d{};
    dss->GetDesc(&d);
    const bool r = d.StencilEnable &&
                   d.StencilReadMask == (UINT8)kFirstPersonStencilBit;
    // The mask's ReadMask is 0xFF, not 0x40 - it tests nothing, it only stamps -
    // so the write role has to be recognised by the WRITE mask and the REPLACE,
    // never by reusing the read test.
    const bool w = d.StencilEnable &&
                   d.StencilWriteMask == (UINT8)kFirstPersonStencilBit &&
                   d.FrontFace.StencilPassOp == D3D11_STENCIL_OP_REPLACE;
    if (n < kMaxDss) {
        const LONG idx = InterlockedIncrement(&g_dssCount) - 1;
        if (idx >= 0 && idx < kMaxDss) {
            g_dssCache[idx].reads = r;
            g_dssCache[idx].writes = w;
            g_dssCache[idx].dss = dss;      // published last
        }
    }
    *reads = r;
    *writes = w;
}

bool StateReadsFirstPersonBit(ID3D11DepthStencilState* dss) {
    bool r = false, w = false;
    FirstPersonStencilRoles(dss, &r, &w);
    return r;
}

// *** THE GLASS. ***
//
// Scope and binocular glass is drawn as a separate alpha-blended pass and does
// NOT carry the first-person stencil bit - so it stays flat while the body of
// the gun gains depth, and it does not move with the strength slider. Neither
// capture the discriminator was derived from had glass in it, so this was a
// known gap rather than a surprise.
//
// Extending from the stencil tag is safe in a way the old index-count runs never
// were, because the anchor is now exact: a blended draw within a few draws of a
// CONFIRMED first-person draw is part of the same pass. Blending is required as
// well as proximity - solid geometry immediately after the pass is world, and
// this is the one place a mistake would put scenery back on the slider.
// Cached by blend-state pointer: this now runs for EVERY DrawIndexed rather
// than only near the pass, and GetDesc per draw would be wasteful. Engines
// reuse a handful of blend states all frame.
struct BlendCache { ID3D11BlendState* bs; bool blended; };
constexpr int kMaxBlend = 64;
BlendCache g_blendCache[kMaxBlend] = {};
volatile LONG g_blendCount = 0;

bool DrawIsBlended(ID3D11DeviceContext* ctx) {
    ID3D11BlendState* bs = nullptr;
    FLOAT factor[4]{};
    UINT mask = 0;
    ctx->OMGetBlendState(&bs, factor, &mask);
    if (!bs) return false;
    const LONG n = g_blendCount;
    for (LONG i = 0; i < n && i < kMaxBlend; ++i) {
        if (g_blendCache[i].bs == bs) {
            const bool r = g_blendCache[i].blended;
            bs->Release();
            return r;
        }
    }
    D3D11_BLEND_DESC d{};
    bs->GetDesc(&d);
    const bool blended = d.RenderTarget[0].BlendEnable != FALSE;
    if (n < kMaxBlend) {
        const LONG idx = InterlockedIncrement(&g_blendCount) - 1;
        if (idx >= 0 && idx < kMaxBlend) {
            g_blendCache[idx].blended = blended;
            g_blendCache[idx].bs = bs;      // published last
        }
    }
    bs->Release();
    return blended;
}

// outWrites tells the caller this draw is the lens MASK rather than the gun, so
// the shift can fall back to the rasterizer if the mask carries no matrix we
// recognise. Both roles are first-person; only the delivery differs.
bool DrawIsFirstPerson(ID3D11DeviceContext* ctx, bool* outWrites = nullptr) {
    if (outWrites) *outWrites = false;
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&dss, &ref);
    if (!dss) return false;
    bool reads = false, writes = false;
    FirstPersonStencilRoles(dss, &reads, &writes);
    dss->Release();
    if ((ref & kFirstPersonStencilBit) == 0) return false;
    if (writes && !Cfg().weapon_match_stencil_write) writes = false;
    if (outWrites) *outWrites = writes;
    return reads || writes;
}

constexpr int kMaxWeaponIdx = 64;
UINT g_weaponIdxCounts[kMaxWeaponIdx] = {};
volatile LONG g_weaponIdxCount = 0;

// Learned from the colour round, which is identified exactly. The counts are
// per weapon, so they are re-learned continuously and a weapon swap simply
// adds its own; the table is small and only ever holds real colour-round
// counts, so a stale entry cannot match anything the gun does not draw.
void NoteWeaponIndexCount(UINT idx) {
    if (!idx) return;
    const LONG n = g_weaponIdxCount;
    for (LONG i = 0; i < n && i < kMaxWeaponIdx; ++i) {
        if (g_weaponIdxCounts[i] == idx) return;
    }
    if (n >= kMaxWeaponIdx) return;
    const LONG slot = InterlockedIncrement(&g_weaponIdxCount) - 1;
    if (slot < 0 || slot >= kMaxWeaponIdx) return;
    g_weaponIdxCounts[slot] = idx;
    COTW_LOG("[weapon] mesh learned: %u indices - its depth rounds will shift too", idx);
}

bool IsWeaponIndexCount(UINT idx) {
    if (!idx) return false;
    const LONG n = g_weaponIdxCount;
    for (LONG i = 0; i < n && i < kMaxWeaponIdx; ++i) {
        if (g_weaponIdxCounts[i] == idx) return true;
    }
    return false;
}

// *** SEEDED, BECAUSE LEARNING NEVER RAN. ***
//
// The table above only ever fills from the colour round, and the colour round is
// matched by pixel-shader CRC - a match that has never fired once, in any
// session. The CRCs are right; a capture of this exact build finds all three,
// current. The problem is that D3D11 gives no way to recover bytecode from an
// ID3D11PixelShader after the fact, so shaders must be fingerprinted AT
// CREATION, and the game builds its library before our hook is installed. The
// mod's own census says 101 shaders seen against 2,039 used in a single frame.
// So "[weapon] mesh learned" never printed, the table stayed empty, and every
// feature downstream of it was inert.
//
// These counts come from RenderDoc instead, and were checked against all 2,214
// draws in the frame: each appears EXACTLY THREE TIMES, all three inside the
// viewmodel block, and nowhere else. Three because every mesh is drawn twice
// depth-only then once in colour - the structure this file documents, confirmed
// from the data without reference to any shader.
//
// DELIBERATELY EXCLUDED, so the omissions are not silent:
//   162          - a real weapon mesh, but 14 world draws share the count. One
//                  small part of the gun will not shift. Better than dragging
//                  scenery with it.
//   4992, 5748   - real, from the August 2 capture, but that session held a
//                  different gun and neither appears in this frame, so there is
//                  no collision evidence either way. The colour round can still
//                  learn them if it is ever repaired.
// *** WHAT THE GUN'S MESHES ACTUALLY ARE, IN THIS SESSION. ***
//
// The seed comes from one captured frame. A different weapon - or the same
// weapon with different attachments - draws different meshes, so some of its
// counts are missing from the table. That shows up as FLICKER rather than as
// nothing happening: the meshes we match are hidden or shifted while the ones we
// miss are not, and a gun whose parts disagree about where they are fails its
// own depth test.
//
// The viewmodel pass is the last cluster of DrawIndexed calls in the frame, so
// the tail of the frame IS the gun. This records it and the frame report prints
// it - which turns "some parts flicker" into an exact list of the counts to add.
constexpr int kTailN = 64;
struct TailDraw {
    UINT idx;
    bool matched;
};
TailDraw g_tail[kTailN] = {};
volatile LONG g_tailPos = 0;

// No driver calls here - PSGetShader already costs one per draw via
// weapon_pass_live_ps, and at ~718 DrawIndexed a frame a second one is not worth
// it when the index count alone is what we need.
void NoteTailDraw(UINT idx, bool matched) {
    const LONG p = InterlockedIncrement(&g_tailPos) - 1;
    TailDraw& e = g_tail[(p & (kTailN - 1))];
    e.idx = idx;
    e.matched = matched;
}

void ReportFrameTail() {
    const LONG end = g_tailPos;
    if (end <= 0) return;
    LONG start = end - kTailN;
    if (start < 0) start = 0;
    COTW_LOG("[tail] the last %ld DrawIndexed of the frame - the viewmodel pass "
             "is in here. '*' = matched and acted on, plain = MISSED:",
             (long)(end - start));
    char line[512];
    int n = 0;
    line[0] = 0;
    for (LONG p = start; p < end; ++p) {
        const TailDraw& e = g_tail[(p & (kTailN - 1))];
        if (!e.idx) continue;
        const int len = (int)strlen(line);
        _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, "%s%u%s",
                    len ? "  " : "", e.idx, e.matched ? "*" : "");
        if (++n % 10 == 0) {
            COTW_LOG("[tail]   %s", line);
            line[0] = 0;
        }
    }
    if (line[0]) COTW_LOG("[tail]   %s", line);
}

// *** IS THE MATCH INTERMITTENT, AND DOES IT FOLLOW THE EYE? ***
//
// A mesh we never match draws steadily - the scope does exactly that. A mesh
// that FLICKERS is one we catch on some frames and miss on others, which means
// the weapon reaches the GPU by more than one path and we are only on one of
// them. Alternate-eye rendering is the obvious suspect: one eye per frame, and
// if the two eye passes use different contexts we would match every other frame.
//
// So count matches per eye, and count frames where nothing matched at all. Even
// coverage across both eyes rules the eye out; a lopsided split convicts it.
volatile LONG g_weaponHitsEye[2] = {};
volatile LONG g_weaponHitsThisFrame = 0;
volatile LONG g_framesWithWeapon = 0;
volatile LONG g_framesWithoutWeapon = 0;

void NoteWeaponFrameBoundary() {
    if (g_weaponHitsThisFrame > 0) InterlockedIncrement(&g_framesWithWeapon);
    else InterlockedIncrement(&g_framesWithoutWeapon);
    InterlockedExchange(&g_weaponHitsThisFrame, 0);
}

void ReportWeaponIntermittency() {
    const LONG e0 = g_weaponHitsEye[0], e1 = g_weaponHitsEye[1];
    const LONG with = g_framesWithWeapon, without = g_framesWithoutWeapon;
    const LONG total = with + without;
    if (!total) return;
    COTW_LOG("[wflick] weapon draws matched: eye0 %ld, eye1 %ld | frames with a "
             "match %ld, without %ld (%ld%% of frames see the gun)%s",
             (long)e0, (long)e1, (long)with, (long)without,
             (long)(total ? (with * 100 / total) : 0),
             (without > with / 4)
                 ? "   <-- INTERMITTENT: that is the flicker"
                 : "");
    if ((e0 == 0) != (e1 == 0)) {
        COTW_LOG("[wflick] one eye sees the gun and the other does not - the two "
                 "eye passes are NOT going through the same context");
    }
    InterlockedExchange(&g_weaponHitsEye[0], 0);
    InterlockedExchange(&g_weaponHitsEye[1], 0);
    InterlockedExchange(&g_framesWithWeapon, 0);
    InterlockedExchange(&g_framesWithoutWeapon, 0);
}

void SeedWeaponIndexCounts() {
    // 4992 and 5748 are back in. They were left out as unverifiable - absent
    // from the reference frame, so no collision evidence either way - but the
    // scope is the one part of the sniper that never flickers, i.e. the one part
    // never matched, and a scope is exactly what differs between two captures of
    // "the sniper". Worth the risk: if scenery starts vanishing, these two are
    // the first thing to remove.
    // 10272 is the SCOPE, and it was found by reading the pass rather than a
    // capture. With every other mesh hidden it was the one draw left standing,
    // and the frame tail showed it in the middle of the pass, once per round:
    //     77484* 162* 5007* 6087* 2484* 10272 5958* 3252* 9594*
    // three times over, the two depth rounds and the colour one.
    static const UINT kVerified[] = {77484, 5007, 6087, 3252, 2484, 5958, 9594,
                                     4992, 5748, 10272};
    for (UINT c : kVerified) NoteWeaponIndexCount(c);
    COTW_LOG("[weapon] seeded %d index counts verified unique across a whole "
             "frame - the gun is now identifiable without any shader",
             (int)(sizeof(kVerified) / sizeof(kVerified[0])));
    COTW_LOG("[weapon] excluded: 162 (shared with 14 world draws) - matched by "
             "its position next to a confirmed weapon draw instead");
}

// Ask the DRIVER what is bound, instead of trusting our own shadow copy.
//
// The shadow table is per-context and capped, and a game that cycles deferred
// contexts exhausts it - after which every draw looks like "no shader bound"
// and the weapon becomes permanently invisible to us. The context itself
// always knows the truth, and PSGetShader is the one call that cannot be
// wrong. It costs an AddRef/Release per draw, so it is only asked on
// DrawIndexed - the entry point the viewmodel actually uses - and never on
// the indirect path that carries the other ~99% of the frame.
bool IsWeaponPixelShaderLive(ID3D11DeviceContext* ctx) {
    if (!ctx) return false;
    if (g_weaponPSCount == 0) return false;
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    if (!ps) return false;
    const bool hit = IsWeaponPixelShader(ps);
    ps->Release();
    return hit;
}

// Is there a pixel shader bound at all? A depth-only round binds none, and
// that is half of what distinguishes it from ordinary geometry.
bool HasNoPixelShader(ID3D11DeviceContext* ctx) {
    if (!ctx) return false;
    ID3D11PixelShader* ps = nullptr;
    ctx->PSGetShader(&ps, nullptr, nullptr);
    if (!ps) return true;
    ps->Release();
    return false;
}

// --- the diagnostic that decides how this feature is implemented -----------
//
// Every number below exists to separate causes that look identical from
// outside. "The weapon does not move" has at least four different meanings and
// the previous session spent seven launches failing to tell them apart.
volatile LONG g_diTotal      = 0;   // DrawIndexed calls seen this frame
volatile LONG g_diDeferred   = 0;   // ...of which were on a deferred context
volatile LONG g_matchShadow  = 0;   // trio matched via our shadow table
volatile LONG g_matchDriver  = 0;   // trio matched via PSGetShader (the truth)
volatile LONG g_matchDepth   = 0;   // depth round matched by index count
volatile LONG g_execCmdList  = 0;   // ExecuteCommandList calls this frame
volatile LONG g_finishCmdList = 0;  // FinishCommandList calls this frame

// *** PER ENTRY POINT, AGAINST GROUND TRUTH. ***
//
// Hooking the deferred contexts took DrawIndexed from 0.9 to 2.9 a frame. The
// capture says 718. So we are STILL blind, and one more theory about why is
// worth less than knowing which entry points we can and cannot see. The
// capture counted, for one frame of this game:
//     DrawIndexedInstanced 785, DrawIndexed 718,
//     DrawIndexedInstancedIndirect 433, DrawInstancedIndirect 132, Draw 94
// Every one of those is counted here and printed next to its expected value,
// so the next line of the log says plainly whether this DLL sits on the
// renderer at all - instead of leaving it to be inferred from a weapon that
// does not move.
volatile LONG g_epDrawIndexed = 0;
volatile LONG g_epDraw = 0;
volatile LONG g_epDrawIndexedInstanced = 0;
volatile LONG g_epDrawInstanced = 0;
volatile LONG g_epDrawIndexedInstancedIndirect = 0;
volatile LONG g_epDrawInstancedIndirect = 0;

// Which constant buffers the weapon's draws actually read. Recorded by POINTER,
// so the patch can key on buffer identity instead of guessing from content.
struct WeaponCB { ID3D11Buffer* buf; UINT slot; UINT size; volatile LONG seen; };
constexpr int kMaxWeaponCB = 16;
WeaponCB g_weaponCBs[kMaxWeaponCB];
volatile LONG g_weaponCBCount = 0;

void NoteWeaponCB(ID3D11Buffer* b, UINT slot, UINT size) {
    const LONG n = g_weaponCBCount;
    for (LONG i = 0; i < n && i < kMaxWeaponCB; ++i) {
        if (g_weaponCBs[i].buf == b && g_weaponCBs[i].slot == slot) {
            InterlockedIncrement(&g_weaponCBs[i].seen);
            return;
        }
    }
    if (n >= kMaxWeaponCB) return;
    const LONG idx = InterlockedIncrement(&g_weaponCBCount) - 1;
    if (idx >= kMaxWeaponCB) return;
    g_weaponCBs[idx].buf = b;
    g_weaponCBs[idx].slot = slot;
    g_weaponCBs[idx].size = size;
    g_weaponCBs[idx].seen = 1;
}

// Dump what the weapon's own buffers actually contain.
//
// Four buffers is a small enough set to simply LOOK at, instead of guessing
// which of thousands of matrices might be the weapon's - which is what every
// attempt before the shader identification was reduced to.
void DumpWeaponBuffer(ID3D11DeviceContext* ctx, ID3D11Buffer* b, const float* f,
                      UINT bytes) {
    // Throttled PER BUFFER, not globally. A single shared timer meant one busy
    // buffer monopolised every window and the other three were never printed at
    // all - so the one most likely to hold an arm's-length transform went
    // unseen.
    const DWORD now = GetTickCount();
    UINT slot = 0;
    LONG which = -1;
    const LONG n = g_weaponCBCount;
    for (LONG i = 0; i < n && i < kMaxWeaponCB; ++i) {
        if (g_weaponCBs[i].buf == b) { slot = g_weaponCBs[i].slot; which = i; break; }
    }
    if (which < 0) return;

    static DWORD lastPer[kMaxWeaponCB] = {};
    if (now - lastPer[which] < 4000) return;
    lastPer[which] = now;

    // Whether this write happened while the weapon's own shader was bound. The
    // buffers are shared, so the same buffer carries other objects' data most of
    // the time; this marks the writes that are actually the weapon's.
    const bool weaponBound = IsWeaponPixelShader(BoundPS(ctx));

    COTW_LOG("[wbuf] slot b%u, %u bytes%s:", slot, bytes,
             weaponBound ? "   <<< WEAPON SHADER BOUND" : "");
    const UINT count = bytes / 4;
    for (UINT i = 0; i + 4 <= count && i < 64; i += 4) {
        // Call out anything sitting at arm's length - that is what a held
        // weapon's transform looks like, and it saves reading 176 numbers.
        const float d3 = sqrtf(f[i] * f[i] + f[i + 1] * f[i + 1] + f[i + 2] * f[i + 2]);
        const bool armsLength = isfinite(d3) && d3 > 0.05f && d3 < 2.0f &&
                                fabsf(f[i + 3] - 1.0f) < 0.01f;
        COTW_LOG("[wbuf]   +0x%03X  %9.4f %9.4f %9.4f %9.4f%s", i * 4,
                 f[i], f[i + 1], f[i + 2], f[i + 3],
                 armsLength ? "   <-- ARM'S LENGTH" : "");
    }
}

bool IsWeaponBufferPtr(ID3D11Buffer* b) {
    const LONG n = g_weaponCBCount;
    for (LONG i = 0; i < n && i < kMaxWeaponCB; ++i) {
        if (g_weaponCBs[i].buf == b) return true;
    }
    return false;
}

using PFN_PSSetShader = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11PixelShader*,
                                                 ID3D11ClassInstance* const*, UINT);
PFN_PSSetShader o_PSSetShader = nullptr;
constexpr int kSlotPSSetShader = 9;

// *** THE DEFERRED CONTEXT IS WHERE THE FRAME ACTUALLY IS. ***
//
// Measured 2026-08-03: our DrawIndexed hook saw 0.9 calls per frame. The
// RenderDoc capture of the same game shows 718. We were watching one draw in
// eight hundred - and every conclusion drawn from "the weapon is not drawn
// through here" was really "almost NOTHING is drawn through here".
//
// Apex records its frame on DEFERRED contexts and replays the command lists on
// the immediate one. In D3D11 an immediate context and a deferred context are
// different implementation classes with DIFFERENT function addresses, and
// MinHook patches a function, not a vtable slot - so hooking the immediate
// context never touched the deferred path. The log proves it exactly:
// ExecuteCommandList counted 2.00/frame (legal only on the immediate context,
// so we see it) while FinishCommandList counted 0.00 (called on the deferred
// context, so we do not). Same for every draw.
//
// Getting at that implementation without an ordering problem - the game
// creates its contexts long before our first Present - is done by creating a
// THROWAWAY deferred context of our own and reading its vtable: all deferred
// contexts of a device share one class, so hooking those addresses hooks the
// game's.
//
// Each hooked function therefore needs TWO originals. Calling the immediate
// context's trampoline for a call that arrived on a deferred context would
// dispatch into the wrong implementation, so every detour picks its original
// by asking the context which kind it is.
// Defined with the camera-selection machinery further down; the
// UpdateSubresource path needs it before that point.
void NoteCameraCandidateImpl(ID3D11DeviceContext* ctx, const uint8_t* data,
                             uint32_t bytes);

inline bool IsDeferredCtx(ID3D11DeviceContext* c) {
    return c && c->GetType() == D3D11_DEVICE_CONTEXT_DEFERRED;
}

// *** ONE DETOUR, MANY IMPLEMENTATIONS. ***
//
// The census settled it: 0.3% of the frame was reaching this DLL, and the
// device log said why - we hooked the contexts of a device the game barely
// uses, while shaders were being created on two OTHER devices. Hooking one
// "correct" context was the wrong idea from the start. There is no single
// context class: immediate differs from deferred, and a device created with
// different flags or feature level can hand out different implementations
// again.
//
// So hook them ALL, and let each detour work out which implementation it was
// reached through. MinHook patches an ADDRESS, and the context's own vtable
// still holds that address - so `(*(void***)ctx)[slot]` names the target, and
// the matching original can be looked up. Calling the wrong original would
// dispatch into another class's method, which is why this cannot just keep one
// pointer per function.
constexpr int kMaxImpl = 8;

struct MethodOrigs {
    int   slot = 0;
    void* target[kMaxImpl] = {};
    void* orig[kMaxImpl] = {};
    volatile LONG n = 0;

    // *** VTABLE MODE. ***
    //
    // Patching the function body is what crashes here: MinHook rewrites
    // DrawIndexed's prologue while the renderer is calling it, and a thread
    // sitting inside the relocated bytes corrupts. Writing the VTABLE ENTRY is a
    // single aligned pointer store - atomic, no thread suspension, no
    // relocation - so it cannot catch a renderer mid-draw.
    //
    // But it breaks the lookup above: once vt[slot] holds our detour, the
    // original can no longer be found by reading it. So vtable mode keys on the
    // VTABLE POINTER instead, which does not change.
    // *** ITS OWN ORIGINALS ARRAY, AND THAT IS NOT A DETAIL. ***
    //
    // vtOrig, not orig. Both paths can be live in one run - contexts hooked
    // atomically at creation, everything else through MinHook - and they index
    // by DIFFERENT counters (nvt and n). Sharing one array means each overwrites
    // the other's saved originals, so calls land on the wrong function. That is
    // a black screen at startup, and it is what shipped in the previous build.
    void** vtKey[kMaxImpl] = {};
    void* vtOrig[kMaxImpl] = {};
    volatile LONG nvt = 0;
    void* detour = nullptr;

    bool KnownVT(void** v) const {
        const LONG cur = nvt;
        for (LONG i = 0; i < cur && i < kMaxImpl; ++i) if (vtKey[i] == v) return true;
        return false;
    }
    void AddVT(void** v, void* o, void* d) {
        if (KnownVT(v)) return;
        const LONG idx = InterlockedIncrement(&nvt) - 1;
        if (idx < 0 || idx >= kMaxImpl) return;
        vtOrig[idx] = o;
        detour = d;
        vtKey[idx] = v;      // published last: the reader keys off this
    }

    bool Known(void* t) const {
        const LONG cur = n;
        for (LONG i = 0; i < cur && i < kMaxImpl; ++i) if (target[i] == t) return true;
        return false;
    }
    void Add(void* t, void* o) {
        if (Known(t)) return;
        const LONG idx = InterlockedIncrement(&n) - 1;
        if (idx < 0 || idx >= kMaxImpl) return;
        target[idx] = t;
        orig[idx] = o;
    }
    // Look the original up by the vtable entry the call must have come through.
    //
    // *** NEVER GUESS AN ORIGINAL ACROSS IMPLEMENTATIONS. ***
    //
    // This used to end `return cur > 0 ? orig[0] : nullptr;` - commented "best
    // effort, never null". That is harmless with ONE d3d11 context
    // implementation and fatal with two: it calls implementation A's trampoline
    // with implementation B's `this`, a type-confused call into unrelated code.
    //
    // The game creates five devices exposing two context implementations, so the
    // moment hook_device_creation made the second one reachable, the next draw
    // through an unmapped context took the game down at startup - one line after
    // "present-path device: 14 method(s) newly hooked ... DrawIndexed at
    // 00007FFDFC56FB40", a different address from the present-path context's.
    //
    // An unknown target means WE NEVER PATCHED IT, so its vtable entry still
    // holds the real function: calling that is both correct and safe. No
    // guessing, and no dropped draw either - returning null here would make the
    // callers swallow the call, which is its own kind of broken frame.
    void* For(void* obj) const {
        if (!obj) return nullptr;
        void** v = *reinterpret_cast<void***>(obj);

        // Vtable mode first: keyed on the vtable pointer, which our patch does
        // not change.
        const LONG cvt = nvt;
        for (LONG i = 0; i < cvt && i < kMaxImpl; ++i) if (vtKey[i] == v) return vtOrig[i];

        void* t = v[slot];

        // *** THE ONE CASE THAT MUST NOT FALL THROUGH. ***
        // If the entry already holds OUR detour but the vtable is not one we
        // recorded, returning t would call ourselves forever. That happens when
        // a patched vtable is copied. Every copy shares the original we already
        // recorded, so vtOrig[0] is right - but only if every recorded original
        // agrees, otherwise we would be guessing across implementations again,
        // which is the type-confused call that crashed the game earlier.
        if (cvt > 0 && t == detour) {
            for (LONG i = 1; i < cvt && i < kMaxImpl; ++i) {
                if (vtOrig[i] != vtOrig[0]) return nullptr;
            }
            return vtOrig[0];
        }

        const LONG cur = n;
        for (LONG i = 0; i < cur && i < kMaxImpl; ++i) if (target[i] == t) return orig[i];
        return t;   // unhooked implementation: the vtable entry IS the original
    }
};

MethodOrigs g_oClearDSV, g_oDrawIndexed, g_oDraw, g_oMap, g_oUnmap;
MethodOrigs g_oDrawIndexedInstanced, g_oDrawInstanced;
MethodOrigs g_oDrawIndexedInstancedIndirect, g_oDrawInstancedIndirect;
MethodOrigs g_oPSSetShader, g_oVSSetShader;
MethodOrigs g_oExecuteCommandList, g_oFinishCommandList, g_oUpdateSubresource;

// How many distinct context implementations we ended up hooking, and how many
// contexts were offered to us. Printed, because "we hooked everything" is a
// claim that has now been wrong twice.
volatile LONG g_implsHooked = 0;
volatile LONG g_ctxOffered = 0;

void HookContextVTable(ID3D11DeviceContext* c, const char* what);
void HookContextVTableRaw(void** vt, const char* what);
// Always by atomic vtable write, whatever hook_context_vtable says. Used at the
// one moment it is provably safe AND correct: inside CreateDeferredContext,
// before the engine has touched the context it just made.
void HookContextVTableAtomic(void** vt, const char* what);
bool PatchVTableSlot(void** vt, int slot, void* detour, void** prevOut);
void HookAllContextsOf(ID3D11Device* dev, const char* why);

// *** CATCH CONTEXTS WHERE THEY ARE HANDED OUT, NOT WHERE WE GUESS THEY ARE. ***
//
// Two rounds of "hook the right context" have now failed while the DEVICE
// hooks caught 1223 of 1226 pixel shaders - proof that patching a function
// address works fine and that the problem is only ever WHICH vtable. So stop
// guessing: hook the device methods that RETURN contexts, and hook whatever
// comes back. Every context the game can possibly draw on passes through one
// of them.
//
// Only the VTABLE POINTER is remembered, never the context itself: a vtable
// lives as long as d3d11.dll, so there is no lifetime or refcount problem, and
// it is the only part we need. The hooking itself is deferred to the Present
// thread because MH_EnableHook suspends every other thread.
constexpr int kMaxPendingVT = 12;
void** g_pendingVT[kMaxPendingVT] = {};
const char* g_pendingWhy[kMaxPendingVT] = {};
volatile LONG g_pendingVTCount = 0;

void NoteContextVTable(void* ctx, const char* why) {
    if (!ctx) return;
    void** vt = *reinterpret_cast<void***>(ctx);
    const LONG n = g_pendingVTCount;
    for (LONG i = 0; i < n && i < kMaxPendingVT; ++i) if (g_pendingVT[i] == vt) return;
    if (n >= kMaxPendingVT) return;
    const LONG idx = InterlockedIncrement(&g_pendingVTCount) - 1;
    if (idx < 0 || idx >= kMaxPendingVT) return;
    g_pendingWhy[idx] = why;
    g_pendingVT[idx] = vt;      // published last: the reader keys off this
}

// The two INDIRECT entry points. They draw 565 of the capture's 2,162 calls and
// were never hooked, so they are a hole in every draw count this DLL has ever
// printed. Counted only - the visible weapon does not use them - but a census
// with a known hole in it cannot answer "do we see the frame".
using PFN_DrawIndirect = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, ID3D11Buffer*, UINT);
constexpr int kSlotDrawIndexedInstancedIndirect = 39;
constexpr int kSlotDrawInstancedIndirect        = 40;

// Forward-declared, and one of very few in this file, for a reason worth
// stating: the HUD probe's "draw index within the frame" is only a frame
// POSITION if every draw advances it. These two are 565 of the capture's 2,162
// calls, so leaving them out would make every ordinal in the probe's listing a
// position in a subset - which is exactly the kind of quietly-wrong number this
// file has been burned by. The probe is defined far below, next to the draw
// hooks it belongs with.
void HudProbeNoteIndirect();

void STDMETHODCALLTYPE Hook_DrawIndexedInstancedIndirect(ID3D11DeviceContext* ctx,
                                                         ID3D11Buffer* args, UINT off) {
    InterlockedIncrement(&g_epDrawIndexedInstancedIndirect);
    HudProbeNoteIndirect();
    if (auto orig = (PFN_DrawIndirect)g_oDrawIndexedInstancedIndirect.For(ctx)) {
        orig(ctx, args, off);
    }
}

void STDMETHODCALLTYPE Hook_DrawInstancedIndirect(ID3D11DeviceContext* ctx,
                                                  ID3D11Buffer* args, UINT off) {
    InterlockedIncrement(&g_epDrawInstancedIndirect);
    HudProbeNoteIndirect();
    if (auto orig = (PFN_DrawIndirect)g_oDrawInstancedIndirect.For(ctx)) {
        orig(ctx, args, off);
    }
}

void STDMETHODCALLTYPE Hook_PSSetShader(ID3D11DeviceContext* ctx, ID3D11PixelShader* ps,
                                        ID3D11ClassInstance* const* inst, UINT n) {
    if (ContextShaders* st = StateFor(ctx)) st->ps = ps;
    if (auto orig = (PFN_PSSetShader)g_oPSSetShader.For(ctx)) orig(ctx, ps, inst, n);
}

// At a weapon draw, ask the context what the VERTEX shader is reading. That is
// where a transform lives; the pixel shader only told us WHICH draw this is.
void CaptureWeaponConstants(ID3D11DeviceContext* ctx) {
    ID3D11Buffer* bufs[8] = {};
    ctx->VSGetConstantBuffers(0, 8, bufs);
    for (UINT i = 0; i < 8; ++i) {
        if (!bufs[i]) continue;
        D3D11_BUFFER_DESC bd{};
        bufs[i]->GetDesc(&bd);
        NoteWeaponCB(bufs[i], i, bd.ByteWidth);
        bufs[i]->Release();
    }
}

using PFN_CreatePixelShader = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, const void*,
                                                          SIZE_T, ID3D11ClassLinkage*,
                                                          ID3D11PixelShader**);
PFN_CreatePixelShader o_CreatePixelShader = nullptr;
constexpr int kSlotCreatePixelShader = 15;   // ID3D11Device vtable

// Shader Toggler works on PIXEL shaders as often as vertex ones, so both are
// fingerprinted - otherwise a hash reported from its UI might have no match here
// at all.
// *** HOW MANY DEVICES IS THIS GAME USING? ***
//
// Shader creation is a DEVICE call, so it cannot be missed by hooking the
// wrong context - yet after 32 seconds only 16 vertex shaders had been seen
// bound and just ONE of the three weapon pixel shaders was ever caught being
// created, against 684 vertex and 1,226 pixel shaders in the capture. A device
// hook that blind means the game is creating them somewhere we are not: a
// second ID3D11Device. Every distinct device pointer that reaches these hooks
// is recorded, and the one we hooked is logged beside them.
constexpr int kMaxDevices = 8;
ID3D11Device* g_devicesSeen[kMaxDevices] = {};
volatile LONG g_deviceCount = 0;
volatile LONG g_psCreated = 0;
volatile LONG g_vsCreated = 0;
ID3D11Device* g_hookedDevice = nullptr;

void NoteDevice(ID3D11Device* dev) {
    if (!dev) return;
    const LONG n = g_deviceCount;
    for (LONG i = 0; i < n && i < kMaxDevices; ++i) if (g_devicesSeen[i] == dev) return;
    if (n >= kMaxDevices) return;
    const LONG idx = InterlockedIncrement(&g_deviceCount) - 1;
    if (idx < 0 || idx >= kMaxDevices) return;
    g_devicesSeen[idx] = dev;
    COTW_LOG("[dev] device #%ld seen creating shaders: %p%s", (long)idx, (void*)dev,
             dev == g_hookedDevice ? "   <-- the one we hooked"
                                   : "   (its contexts will be hooked too)");
}

// Hook the contexts of any device we have not covered yet.
//
// Deliberately NOT done inside NoteDevice: that runs on whatever thread is
// creating a shader, and MH_EnableHook suspends every other thread to patch
// code. Doing that from inside a hooked call on a worker thread invites a
// deadlock. This runs from Present instead, one frame later at worst.
volatile LONG g_devicesHooked = 0;

volatile LONG g_pendingVTHooked = 0;

void HookPendingDevices() {
    if (!Cfg().hook_deferred_context) return;
    while (g_devicesHooked < g_deviceCount) {
        const LONG i = g_devicesHooked;
        if (i < 0 || i >= kMaxDevices) break;
        // Shader creation too, not just contexts. Devices caught at birth are
        // recorded and nothing more - all their MinHook work belongs here, on
        // the Present thread, and not in the middle of D3D11CreateDevice.
        InstallShaderCreationHooks(g_devicesSeen[i]);
        HookAllContextsOf(g_devicesSeen[i], "shader-creating device");
        InterlockedIncrement(&g_devicesHooked);
    }
    // Contexts the game asked the device for, caught at the source.
    while (g_pendingVTHooked < g_pendingVTCount) {
        const LONG i = g_pendingVTHooked;
        if (i < 0 || i >= kMaxPendingVT) break;
        if (g_pendingVT[i]) HookContextVTableRaw(g_pendingVT[i], g_pendingWhy[i]);
        InterlockedIncrement(&g_pendingVTHooked);
    }
}

HRESULT STDMETHODCALLTYPE Hook_CreatePixelShader(ID3D11Device* dev, const void* code,
                                                 SIZE_T len, ID3D11ClassLinkage* link,
                                                 ID3D11PixelShader** out) {
    const HRESULT hr = o_CreatePixelShader(dev, code, len, link, out);
    if (SUCCEEDED(hr) && out && *out) {
        InterlockedIncrement(&g_psCreated);
        NoteDevice(dev);
        NoteShaderId(*out, code, len, true);
        // Recognise the three the player marked in Shader Toggler.
        const uint32_t crc = Crc32(code, len);
        const uint32_t wanted[kWeaponPS] = {
            (uint32_t)Cfg().weapon_ps_crc0, (uint32_t)Cfg().weapon_ps_crc1,
            (uint32_t)Cfg().weapon_ps_crc2};
        for (int i = 0; i < kWeaponPS; ++i) {
            if (crc == wanted[i] && wanted[i] != 0) {
                AddWeaponPS(*out);
                COTW_LOG("[weapon] pixel shader %08X identified - its draws ARE the "
                         "weapon", crc);
            }
        }
    }
    return hr;
}

HRESULT STDMETHODCALLTYPE Hook_CreateVertexShader(ID3D11Device* dev, const void* code,
                                                  SIZE_T len, ID3D11ClassLinkage* link,
                                                  ID3D11VertexShader** out) {
    const HRESULT hr = o_CreateVertexShader(dev, code, len, link, out);
    if (SUCCEEDED(hr) && out && *out) {
        InterlockedIncrement(&g_vsCreated);
        NoteDevice(dev);
        NoteShaderId(*out, code, len, false);

    }
    return hr;
}

int ShaderIndex(ID3D11VertexShader* vs) {
    if (!vs) return -1;
    const LONG n = g_shaderCount;
    for (LONG i = 0; i < n && i < kMaxShaders; ++i) {
        if (g_shaders[i] == vs) return (int)i;
    }
    if (n >= kMaxShaders) return -1;
    const LONG idx = InterlockedIncrement(&g_shaderCount) - 1;
    if (idx >= kMaxShaders) return -1;
    g_shaders[idx] = vs;
    return (int)idx;
}

// Hides a RANGE, so the weapon can be bisected out in ~9 steps instead of
// stepping through hundreds of shaders one at a time. Set the range to the first
// half: if the gun vanishes it is in there, otherwise it is in the other half.
// Halve and repeat.
bool ShouldSkipCurrentDraw(ID3D11DeviceContext* ctx) {
    ID3D11VertexShader* vs = BoundVS(ctx);
    if (!vs) return false;
    const int lo = Cfg().shader_hide_index;
    if (lo < 0) return false;
    const int count = Cfg().shader_hide_count < 1 ? 1 : Cfg().shader_hide_count;
    const LONG n = g_shaderCount;
    for (int i = lo; i < lo + count && i < n && i < kMaxShaders; ++i) {
        if (g_shaders[i] == vs) return true;
    }
    return false;
}
bool g_installed = false;
constexpr int kSlotUpdateSubresource = 48;

// ID3D11DeviceContext vtable: 12 = DrawIndexed, 14 = Map, 15 = Unmap,
// 53 = ClearDepthStencilView.
constexpr int kSlotDrawIndexed = 12;
constexpr int kSlotMap = 14;
constexpr int kSlotUnmap = 15;
constexpr int kSlotClearDSV = 53;

// THE VIEWMODEL'S BOUNDARY.
//
// Searching the constant buffers for the camera position found NOTHING, which is
// itself the answer: this engine renders CAMERA-RELATIVE (vertex positions are
// passed relative to the camera, the usual way to keep float precision in a big
// world). So there is no world-space camera position in the buffers to look for,
// and the basis alone matches 95 places a frame - far too promiscuous to act on.
//
// The render-pass enum names a Z_FIRST_PERSON_CLEAR_PASS, so the engine clears
// depth immediately before drawing the held weapon - exactly so the weapon can
// never be occluded by the world. That clear is a precise, cheap marker for
// "everything after this is the viewmodel", which is the boundary the whole
// problem needs.
volatile LONG g_clearsThisFrame = 0;
volatile LONG g_drawsThisFrame = 0;
volatile LONG g_drawsSinceLastClear = 0;

// *** PER THREAD, AND THAT IS THE WHOLE POINT. ***
//
// g_drawsSinceLastClear above is one counter incremented by every thread, and
// the engine records the frame on several deferred contexts at once - so a draw
// on one thread reads a count inflated by draws on another. As a per-draw
// measure of "how far into this pass are we" it is noise.
//
// It shows in the numbers: with the shared counter, 2484's impostor draws were
// rejected cleanly (1080 shifted of 1440 seen) while 3252's were not (1800 of
// 2520), and its measured range straddled the threshold at 224..1041. Same rule,
// two different answers, because each context has its own position in its own
// recording and the shared counter belongs to none of them.
__declspec(thread) LONG t_drawsSinceClear = 0;

// The high-water mark of the above, per thread. The viewmodel is drawn at the
// END of its recording, so its draws sit near this maximum while world geometry
// sits far below it:
//
//     real weapon meshes    clear = 897 .. 929
//     fence impostor (3252) clear = 200
//
// A FIXED threshold would work in this scene and fail in a sparser one, where
// the whole band moves down and the gun would be excluded - which is exactly how
// the previous attempt made half the weapon disappear. Judging against the
// high-water mark scales with the scene instead.
__declspec(thread) LONG t_maxSinceClear = 0;
LONG g_lastClearAtDraw = 0;

struct ClearMark { LONG atDraw; LONG drawsAfter; };
constexpr int kMaxClears = 16;
ClearMark g_clearMarks[kMaxClears];

// The buffer currently mapped on this thread. Constant-buffer maps are short and
// not nested in practice, and a wrong guess here only costs a missed sample.
__declspec(thread) ID3D11Resource* t_mapped = nullptr;
__declspec(thread) void* t_data = nullptr;
__declspec(thread) UINT t_size = 0;

// *** THE VIEWMODEL'S CONSTANTS, CAUGHT ON THE WAY PAST. ***
//
// The engine writes InstanceConsts immediately before each draw that uses it, so
// the last thing unmapped before a weapon draw IS that draw's constants. We keep
// a copy: at the draw we cannot read the game's buffer back cheaply, but we can
// rebuild it from this, patched, in a buffer of our own.
//
// 256 bytes is the whole block:
//   +0x00 WorldViewProjection  +0x40 TextureMatrix  +0x70 World / PrevWVP
//   +0xB0 Mirrored  +0xB8 WindConst  +0xBC Alpha  +0xC0 RotWorld
//   +0xF0 LocalWindDir  +0xFC PrevSkinningBoneOffset
//
// Thread-local because the engine records on several deferred contexts at once,
// each on its own thread, and each has its own map/unmap/draw sequence.
constexpr UINT kInstanceConstsSize = 256;
// *** THE SMALLEST CONSTANT BUFFER WORTH SNAPSHOTTING. ***
//
// It used to be kInstanceConstsSize - 256 bytes - and that single number is why
// the scope and binocular lenses never moved. THE LENS BUFFER IS 128 BYTES.
// It is DYNAMIC, written through Map/Unmap, seen by our hook every frame, and
// silently dropped by a size test written for a different block.
//
// Three theories were built on top of that filter and all were wrong: snapshot
// table capacity (skips unchanged at 64 vs 256 slots), UpdateSubresource (zero
// constant buffers arrive that way), and an unhooked context (it is hooked).
// The buffer was in front of us the whole time, being measured out.
//
// 64 bytes is one 4x4 matrix, which is the smallest thing that can carry a
// transform worth rewriting.
constexpr UINT kMinSnapshotBytes = 64;
// The engine writes into a large constant POOL and binds a RANGE of it per draw,
// so the block this draw wants is rarely at offset 0. Keep enough of the head of
// the pool to cover the offsets actually used; 8 KB costs one memcpy per unmap
// and covers everything seen so far.
constexpr UINT kPoolSnapshot = 8192;

// *** ONE SNAPSHOT PER BUFFER, NOT ONE "MOST RECENT". ***
//
// Keeping only the last unmapped buffer made the patch a lottery: material and
// skinning constants are unmapped in between, so whether the right data was to
// hand varied from draw to draw AND from frame to frame. With alternate-eye
// rendering, consecutive frames are opposite eyes - so a mesh could be shifted
// in the left eye and skipped in the right, giving it a disparity that means
// nothing. The brain refuses to fuse that and drops one eye's copy, which is
// exactly the reported "I see half the barrel with one eye and the other half
// with the other".
//
// Keyed by resource, so a draw always finds the snapshot belonging to the buffer
// it actually reads, and gets the same answer every frame.
// 16, not 4. Four entries are evicted faster than the engine cycles its
// constant buffers, so a draw could arrive to find its buffer's snapshot already
// pushed out - which showed up as the hands staying flat while the gun and even
// the glove on the same hand had depth. 16 x 8 KB per thread is cheap.
// 64, not 16. The counters showed 29,920 blended draws a report skipped for
// "no snapshot of the buffer this draw reads" - the table was cycling faster
// than the engine cycles its constant buffers, so a buffer's snapshot was
// routinely gone by the time its draw arrived.
// *** SIZED SO A BUFFER SURVIVES FROM ITS UPLOAD TO ITS DRAW. ***
//
// 64 slots with round-robin eviction, against an engine that maps far more
// constant buffers than that per frame. A buffer snapshotted early in the frame
// was being evicted before the draw that reads it arrived - which is exactly
// what 'our snapshot is of a different buffer than this draw reads' means, and
// the log counts it at 742 a window, about four per frame: the four lens draws.
//
// That is why the constants path never once reached a lens, and why the lens was
// left on the rasterizer - the one route that cannot work for it, because the
// lens shader samples the scene in SCREEN SPACE and a viewport shift drags that
// sampling along with the geometry.
//
// 256 slots is ~2 MB of static memory, which is nothing next to re-rendering the
// world twice.
// 512 now, and the reason changed. At a 256-byte floor the table was never full
// and capacity was irrelevant - proven by the skip count not moving when it went
// from 64 slots to 256. Admitting 64-byte buffers admits far more of them, so
// eviction is real for the first time, and the lens losing its snapshot is worth
// 4 MB of static memory to avoid.
// *** 2048, AND THE REASON IS THE FLICKER ITSELF. ***
//
// The report carries a line that has been visible since the first build and read
// as noise every time: '~20 skipped (our snapshot is of a different buffer than
// this draw reads)'. Twenty per 180-frame window is about ONE EVERY NINE SECONDS,
// and a skip means that piece of the weapon is drawn UNSHIFTED for a single
// frame - a one-frame jump on whatever lost its snapshot.
//
// That is the owner's description exactly: random, ten quiet seconds between
// events, and coming from any part - the glass, the pieces behind it, or the
// turret. It was never specific to lenses; lenses were just where it was easiest
// to see.
//
// Dropping the snapshot floor to 64 bytes (to reach the 128-byte lens buffer)
// admitted far more buffers into this table, so eviction pressure is real.
// 2048 slots is 16 MB of static memory - cheap next to a visible artefact.
// 2048 was tried and the game did not survive startup - it hung at probe-device
// creation, before a frame was drawn. 2048 entries is ~17 MB of static data in
// this DLL; 512 has run all day. The skip counter this was meant to reduce is
// worth chasing, but not at the cost of a build that will not launch.
// 2048. The skip counter is now known to BE the flicker: the pieces that lose
// their snapshot are weapon meshes - 77484, 9594, 162, 5007, 10272 - each one a
// single frame of that piece drawn unshifted, about one every eight seconds,
// which is the owner's 'random, and this time it was the turret'.
//
// Tried once before and blamed for a startup hang; the hang was a launch that
// had been interrupted, and the owner confirmed it does not crash.
// *** 512, AND THE BIG TABLE IS A DEAD END - PROVEN TWICE OVER. ***
//
// Raising this was meant to remove the ~20 draws a window that cannot find
// their buffer's snapshot, on the theory that each was one frame of a weapon
// piece drawn unshifted - the random flicker. At 2048 the misses reached ZERO
// ('9740 shifted, 0 skipped') and the flicker got WORSE, spreading to the
// weapon and the hands.
//
// So those misses were never the flicker. They were protecting the player:
// some of those draws are claimed by the looser identification rules and
// should not be moved at all, and failing to reach their constants was the
// accident that kept them still. Exactly what the stencil-tag fallback
// experiment showed, which I had already been told and did not carry over.
constexpr int kMaxCBSnapshots = 512;
struct CBSnapshot {
    ID3D11Resource* res;
    UINT bytes;
    uint8_t data[kPoolSnapshot];
};
// GLOBAL, not thread_local - the third instance of the same bug. The constants
// are WRITTEN (mapped/unmapped) on one deferred context's thread and the glass
// is DRAWN on another; a per-thread table means the lens's thread never has the
// snapshot and every lens draw is skipped. Same failure as the clip-matrix ring
// and the sinceStencil gap counter before it.
//
// Torn reads are tolerated by design: D3D forbids mapping one buffer on two
// threads at once, so a given entry has one writer at a time, and a reader that
// races it gets bytes that fail the bit-identity check - a skipped draw for one
// frame, never a wrong shift.
CBSnapshot g_snaps[kMaxCBSnapshots] = {};
volatile LONG g_snapNext = 0;

CBSnapshot* FindSnapshot(ID3D11Resource* res) {
    if (!res) return nullptr;
    for (int i = 0; i < kMaxCBSnapshots; ++i) {
        if (g_snaps[i].res == res && g_snaps[i].bytes) return &g_snaps[i];
    }
    return nullptr;
}

CBSnapshot* SlotForSnapshot(ID3D11Resource* res) {
    if (CBSnapshot* s = FindSnapshot(res)) return s;
    const LONG n = (InterlockedIncrement(&g_snapNext) - 1) % kMaxCBSnapshots;
    return &g_snaps[n];
}

// *** WHO NEEDS THIS TABLE - ASKED IN ONE PLACE, FOR THE FOURTH TIME. ***
//
// The snapshot table is written on two hooks (Unmap and UpdateSubresource) and
// read by four things. Each writer carried its OWN hand-written list of who
// might want it, and both lists named the weapon and the probes and stopped
// there - so with weapon depth switched off the table was empty, and
// NoteVelocityPass and ScanResolveConstants, which taa.cpp calls EVERY FRAME,
// found nothing to read. The resolve was left identifying its own pass without
// the constants that describe it, and the picture went soft and shimmery the
// moment an unrelated switch was turned off. Reported exactly that way:
// "disabling weapon 3d is causing shimmering and blurriness".
//
// This is the same failure Hook_Map's gate has now described three times, and
// the reason it came back is that the fix was applied to the GATE and not to
// the pattern: three lists of dependants, maintained by hand, agreeing until
// they did not. One predicate, named for the resource rather than for any
// feature, is the only version of this that stays fixed - a new reader adds
// itself here and every writer follows.
bool SnapshotsWanted() {
    const Config& c = Cfg();
    return c.weapon_3d ||          // reads them to give the gun depth
           c.hud_probe ||          // reports the constants a HUD draw uses
           c.taa_probe ||          // looks inside the resolve's own buffers
           c.taa_replace_pass ||   // NoteVelocityPass / ScanResolveConstants
           c.dlss_enable;          // same two, on the reconstruction path
}

struct Hit {
    UINT bufferSize;
    UINT offset;        // byte offset of the match within the buffer
    int  kind;          // 0 = camera position, 1 = camera basis row 0
    volatile LONG countThisFrame;
    volatile LONG countTotal;
};
constexpr int kMaxHits = 32;
Hit g_hits[kMaxHits];
volatile LONG g_hitCount = 0;

// What sizes of constant buffer does this engine actually use?
//
// Asked because the scan has been silently skipping anything over 4096 bytes,
// and a large ring buffer sub-allocated per draw is exactly how a renderer of
// this vintage streams constants - which would put the weapon's view somewhere
// the search has never looked. Cheap to answer, and it decides whether the
// filter or the theory is wrong.
struct SizeBin { UINT size; volatile LONG count; };
constexpr int kMaxSizes = 24;
SizeBin g_sizes[kMaxSizes];
volatile LONG g_sizeCount = 0;

void NoteBufferSize(UINT size) {
    const LONG n = g_sizeCount;
    for (LONG i = 0; i < n && i < kMaxSizes; ++i) {
        if (g_sizes[i].size == size) { InterlockedIncrement(&g_sizes[i].count); return; }
    }
    if (n >= kMaxSizes) return;
    const LONG idx = InterlockedIncrement(&g_sizeCount) - 1;
    if (idx >= kMaxSizes) return;
    g_sizes[idx].size = size;
    g_sizes[idx].count = 1;
}

void NoteHit(UINT size, UINT offset, int kind) {
    const LONG n = g_hitCount;
    for (LONG i = 0; i < n && i < kMaxHits; ++i) {
        if (g_hits[i].bufferSize == size && g_hits[i].offset == offset &&
            g_hits[i].kind == kind) {
            InterlockedIncrement(&g_hits[i].countThisFrame);
            InterlockedIncrement(&g_hits[i].countTotal);
            return;
        }
    }
    if (n >= kMaxHits) return;
    const LONG idx = InterlockedIncrement(&g_hitCount) - 1;
    if (idx >= kMaxHits) return;
    g_hits[idx].bufferSize = size;
    g_hits[idx].offset = offset;
    g_hits[idx].kind = kind;
    g_hits[idx].countThisFrame = 1;
    g_hits[idx].countTotal = 1;
}

bool Close(float a, float b) {
    const float d = fabsf(a - b);
    return d < 0.002f + fabsf(b) * 0.001f;
}

// A PROJECTION MATRIX has a shape nothing else has: two scale terms on the
// diagonal, a ±1 in the w column, a zero in the bottom-right, and zeros almost
// everywhere else. That makes it findable without knowing a single thing about
// the engine's shader layout.
//
// And it is the right thing to look for here. The player observed early on that
// the game's FOV slider moves the world but NOT the held weapon - so the weapon
// is rendered through its OWN projection, and there must be two distinct ones in
// every frame. The narrower is the viewmodel's, and whichever buffer carries it
// is the buffer whose view has to be shifted per eye.
struct ProjHit {
    float vfovMin, vfovMax;     // a zooming scope animates its FOV; one entry, a range
    UINT  bufferSize;
    UINT  offset;
    volatile LONG countThisFrame;
    volatile LONG countTotal;
};
constexpr int kMaxProj = 48;
ProjHit g_proj[kMaxProj];
volatile LONG g_projCount = 0;

// Keyed on WHERE, not on the value.
//
// Keying on the FOV as well filled all 16 slots with one zooming scope - the
// same buffer at the same offset, recorded eleven times as its FOV animated
// 60 -> 70 degrees. Once full the table silently dropped everything else, so a
// missing weapon projection was indistinguishable from a table with no room for
// it. Same trap as the probes earlier today: a silent cap that turns "not
// recorded" into "not found".
// The frame's projection TIMELINE, in order.
//
// The 192-byte buffer at +0x0000 is written twice per frame - one per eye under
// full-rate - and its FOV ranges 60 to 90, which is what a buffer shared between
// the world pass and the weapon pass looks like. Aggregates cannot tell those
// apart; the ORDER can. If a frame reads 90, 55, 90, 55 then the narrow ones are
// the viewmodel and there is the target.
struct ProjEvent { UINT size; UINT offset; float fov; };
constexpr int kMaxEvents = 24;
ProjEvent g_events[kMaxEvents];
volatile LONG g_eventCount = 0;

void NoteProjectionEvent(float vfovDeg, UINT size, UINT offset) {
    const LONG idx = InterlockedIncrement(&g_eventCount) - 1;
    if (idx < 0 || idx >= kMaxEvents) return;
    g_events[idx].size = size;
    g_events[idx].offset = offset;
    g_events[idx].fov = vfovDeg;
}

void NoteProjection(float vfovDeg, UINT size, UINT offset) {
    NoteProjectionEvent(vfovDeg, size, offset);
    const LONG n = g_projCount;
    for (LONG i = 0; i < n && i < kMaxProj; ++i) {
        if (g_proj[i].bufferSize == size && g_proj[i].offset == offset) {
            if (vfovDeg < g_proj[i].vfovMin) g_proj[i].vfovMin = vfovDeg;
            if (vfovDeg > g_proj[i].vfovMax) g_proj[i].vfovMax = vfovDeg;
            InterlockedIncrement(&g_proj[i].countThisFrame);
            InterlockedIncrement(&g_proj[i].countTotal);
            return;
        }
    }
    if (n >= kMaxProj) return;
    const LONG idx = InterlockedIncrement(&g_projCount) - 1;
    if (idx >= kMaxProj) return;
    g_proj[idx].vfovMin = vfovDeg;
    g_proj[idx].vfovMax = vfovDeg;
    g_proj[idx].bufferSize = size;
    g_proj[idx].offset = offset;
    g_proj[idx].countThisFrame = 1;
    g_proj[idx].countTotal = 1;
}

bool LooksLikeProjection(const float* m, float* vfovDegOut) {
    auto zero = [](float v) { return fabsf(v) < 1e-4f; };
    if (!zero(m[1]) || !zero(m[2]) || !zero(m[3])) return false;      // row 0
    if (!zero(m[4]) || !zero(m[6]) || !zero(m[7])) return false;      // row 1
    if (!zero(m[12]) || !zero(m[13])) return false;                   // row 3 x,y
    if (!zero(m[15])) return false;                                   // w must be 0
    if (fabsf(fabsf(m[11]) - 1.0f) > 1e-3f) return false;             // +/-1 in w column
    if (m[0] < 0.1f || m[0] > 20.0f) return false;
    if (m[5] < 0.1f || m[5] > 20.0f) return false;
    if (!isfinite(m[5])) return false;
    *vfovDegOut = 2.0f * atanf(1.0f / m[5]) * 57.2957795f;
    return *vfovDegOut > 5.0f && *vfovDegOut < 175.0f;
}

// THE VIEWMODEL'S MODEL MATRIX.
//
// There is no second projection - measured: exactly two projection writes a
// frame, both 90 degrees, one per eye. So the weapon is drawn through the SAME
// view-projection as the world, and its apparent FOV comes from its MODEL
// transform instead. That also explains something in the notes that should have
// carried more weight: the Nexus "Increased Weapon FOV" mod is an ADF ASSET
// edit, and a data file cannot change a projection matrix - but it can change a
// model transform.
//
// So look for an affine 4x4 whose translation is small. The engine renders
// camera-relative, so a matrix sitting 0.05-2 m from the origin is an object
// held at arm's length - and almost nothing else in a hunting map is that close
// to the camera every single frame.
struct ModelHit {
    UINT  bufferSize;
    UINT  offset;
    float distMin, distMax;
    volatile LONG countThisFrame;
    volatile LONG countTotal;
};
constexpr int kMaxModels = 48;
ModelHit g_models[kMaxModels];
volatile LONG g_modelCount = 0;

void NoteModel(UINT size, UINT offset, float dist) {
    const LONG n = g_modelCount;
    for (LONG i = 0; i < n && i < kMaxModels; ++i) {
        if (g_models[i].bufferSize == size && g_models[i].offset == offset) {
            if (dist < g_models[i].distMin) g_models[i].distMin = dist;
            if (dist > g_models[i].distMax) g_models[i].distMax = dist;
            InterlockedIncrement(&g_models[i].countThisFrame);
            InterlockedIncrement(&g_models[i].countTotal);
            return;
        }
    }
    if (n >= kMaxModels) return;
    const LONG idx = InterlockedIncrement(&g_modelCount) - 1;
    if (idx >= kMaxModels) return;
    g_models[idx].bufferSize = size;
    g_models[idx].offset = offset;
    g_models[idx].distMin = dist;
    g_models[idx].distMax = dist;
    g_models[idx].countThisFrame = 1;
    g_models[idx].countTotal = 1;
}

bool LooksLikeCloseModel(const float* m, float* distOut) {
    auto zero = [](float v) { return fabsf(v) < 1e-4f; };
    if (!zero(m[3]) || !zero(m[7]) || !zero(m[11])) return false;   // affine
    if (fabsf(m[15] - 1.0f) > 1e-3f) return false;
    for (int r = 0; r < 3; ++r) {                                    // sane basis
        const float* v = &m[r * 4];
        const float len2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
        if (!isfinite(len2) || len2 < 0.04f || len2 > 25.0f) return false;
    }
    const float* t = &m[12];
    const float d = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    if (!isfinite(d) || d < 0.05f || d > 2.0f) return false;         // arm's length
    *distOut = d;
    return true;
}

// *** THE FIX ***
//
// CONFIRMED by holstering: the 192-byte buffer at +0x0000 holds an affine matrix
// 0.27-0.52 m from the camera, written twice per frame (once per eye), and it
// stops being written ENTIRELY while the weapon is holstered. That is the
// viewmodel's transform.
//
// It is camera-relative, so it inherits the stereo eye shift automatically and
// lands on identical pixels in both eyes - zero disparity, which the brain reads
// as infinitely far away. Subtracting the shift here leaves the weapon standing
// still in world space while the eyes move around it, which is exactly the
// disparity a real object at arm's length produces.
volatile LONG g_patchCount = 0;
volatile LONG g_patchViaMap = 0;
volatile LONG g_patchViaUpdate = 0;

// Is there anything to do to the weapon's matrix at all? Position nudging works
// independently of the stereo shift - it is useful on its own for placing the
// gun where it sits comfortably in a headset, and it doubles as the decisive
// test: if the weapon MOVES, this matrix is confirmed to be what places it.
bool WeaponPatchEnabled() {
    const Config& c = Cfg();
    return c.weapon_cb_stereo || c.weapon_pos_x != 0.0f || c.weapon_pos_y != 0.0f ||
           c.weapon_pos_z != 0.0f;
}

void PatchWeaponMatrix(float* m) {
    const Config& c = Cfg();

    // THE UNAMBIGUOUS TEST.
    //
    // The patch demonstrably runs - 360 writes a report, all through Map - and a
    // 0.3 m nudge moves nothing. So either this buffer does not feed the weapon's
    // draw, or it is not consumed the way a row-major transform would be.
    // Nudging cannot tell those apart, but WRECKING it can: if the weapon is
    // drawn from this matrix, collapsing it must make the gun vanish, stretch or
    // fly off. If the gun carries on regardless, this buffer is not what draws
    // it and the whole target is wrong.
    if (c.weapon_cb_test == 1) {
        for (int i = 0; i < 12; ++i) m[i] = 0.0f;      // basis to nothing
        InterlockedIncrement(&g_patchCount);
        return;
    }
    if (c.weapon_cb_test == 2) {
        m[12] += 5.0f; m[13] += 5.0f; m[14] += 5.0f;   // 5 m away, unmissable
        InterlockedIncrement(&g_patchCount);
        return;
    }
    if (c.weapon_cb_test == 3) {
        // The other storage convention: translation in the last COLUMN rather
        // than the last row. Standard HLSL is column-major, so if the engine
        // uploads transposed, this is where a shift has to go.
        m[3] += c.weapon_pos_x; m[7] += c.weapon_pos_y; m[11] += c.weapon_pos_z;
        InterlockedIncrement(&g_patchCount);
        return;
    }

    // Manual placement, in the weapon's own space. Applied whether or not the
    // stereo shift is on.
    m[12] += c.weapon_pos_x;
    m[13] += c.weapon_pos_y;
    m[14] += c.weapon_pos_z;

    if (!c.weapon_cb_stereo || !c.stereo) { InterlockedIncrement(&g_patchCount); return; }
    int axis = Cfg().weapon_cb_axis;
    if (axis < 0 || axis > 2) axis = 0;

    const float half = Cfg().ipd_mm * 0.001f * 0.5f;
    float sign = (CurrentRenderEye() == 0) ? -1.0f : 1.0f;
    if (Cfg().eye_swap) sign = -sign;

    // MINUS: cancel what the weapon inherited from the camera.
    m[12 + axis] -= half * sign * Cfg().weapon_cb_depth;
    InterlockedIncrement(&g_patchCount);
}

// *** GIVING THE GUN DEPTH: A CLIP-SPACE SHIFT ON WorldViewProjection. ***
//
// The weapon is flat because it rides the camera: the engine re-places it every
// frame relative to wherever the view is, so when the mod moves the eye the gun
// moves with it and both eyes land on identical pixels. Zero disparity is what
// the brain reads as infinitely far away, which is why it looks pasted to your
// face rather than held in your hands.
//
// The fix is to put the disparity back. Shifting clip-space X by k*w is exactly
// a horizontal parallax, opposite per eye, and it needs no camera basis - which
// matters, because we do not have the world-space right vector down here.
//
//     clip = v . M          (row-vector convention: m[2][3] == -1 confirms it)
//     want clip.x += k * clip.w
//     => column0 += k * column3
//
// LAYOUT IS A GUESS UNTIL IT IS SEEN. HLSL's default column_major storage packs
// COLUMNS into the rows of the buffer, so the bytes may be the transpose of what
// RenderDoc's reflection displays. weapon_3d_layout picks; if one shears the gun
// instead of moving it sideways, it is the other one.
// *** WHERE THE MATRIX IS DEPENDS ON WHICH BLOCK THIS SHADER USES. ***
//
// Two layouts are in play at the same slot, and they disagree about +0x00:
//
//   InstanceConsts (256 B)   +0x00 WorldViewProjection   <- shift this
//   LocalConstants (6944 B)  +0x00 World
//                            +0x40 WorldViewProj         <- shift this
//
// Shifting +0x00 blindly meant the hands - the only mesh on LocalConstants - had
// their WORLD matrix perturbed instead of their projection. No depth, and the
// faint sliding ghost that showed up when the strength changed.
//
// They identify themselves from their contents rather than needing a per-mesh
// table: World holds a world-space position, thousands of units from the origin,
// while a WorldViewProjection's last row never is. Measured: World's translation
// was (-5291, 1031, 5997), the InstanceConsts matrix's was (0.32, -0.87, 0.01).
// *** STRUCTURAL, NOT MAGNITUDE. ***
//
// The first version asked "is the translation at +0x00 huge? then it is World,
// so use +0x40". That works for the two layouts it was derived from and fails on
// a third:
//
//   solid  InstanceConsts (256 B)   +0x00 WorldViewProjection  +0x40 TextureMatrix
//   hands  LocalConstants (6944 B)  +0x00 World                +0x40 WorldViewProj
//   GLASS  InstanceConsts (128 B)   +0x00 World                +0x40 WorldViewProjection
//
// The glass block shares a NAME with the 256-byte one and reverses its order, so
// name is no guide; and its World is CAMERA-RELATIVE - translation 0.65 units -
// so magnitude called it a projection. The mod then shifted World, whose 4th
// column is exactly (0,0,0,1), which reduces the shift to m[12] += k: a rigid
// slide of the lens along world X with no clip-space shift at all. That is the
// lenses detaching and moving up-and-right.
//
// What actually distinguishes them is a property no scale can disguise: an
// object transform's 4th COLUMN is a literal (0,0,0,1); a matrix that projects
// to clip space never is, because w has to come from the vertex. Exact
// comparison is right - the bits really are 0,0,0,0x3F800000.
//
// Verified 10/10 on every tagged draw plus the glass draw in the binocular
// capture. Note it must test +0x00 and stop: "scan for the most projective
// matrix" lands on TextureMatrix at +0x40 in the 256-byte layout and shears the
// textures instead of moving the mesh.
bool MatrixIsAffine(const float* m) {
    return m[3] == 0.0f && m[7] == 0.0f && m[11] == 0.0f && m[15] == 1.0f;
}

bool MatrixIsFinite(const float* m) {
    for (int i = 0; i < 16; ++i) if (!isfinite(m[i])) return false;
    return true;
}

UINT WvpOffsetIn(const uint8_t* block, UINT bytes) {
    if (bytes < 0x80) return 0;
    const float* m0 = reinterpret_cast<const float*>(block);
    if (!MatrixIsFinite(m0)) return 0;
    if (!MatrixIsAffine(m0)) return 0;          // +0x00 IS the clip matrix
    const float* m1 = reinterpret_cast<const float*>(block + 0x40);
    // Refuse rather than guess: if +0x40 is also affine it is not a projection
    // either, and shifting it would move geometry the way the glass moved.
    if (!MatrixIsFinite(m1) || MatrixIsAffine(m1)) return 0;
    return 0x40;
}

void ShiftWeaponClipX(float* m, float k) {
    if (Cfg().weapon_3d_layout == 0) {
        m[0]  += k * m[3];
        m[4]  += k * m[7];
        m[8]  += k * m[11];
        m[12] += k * m[15];
    } else {
        m[0] += k * m[12];
        m[1] += k * m[13];
        m[2] += k * m[14];
        m[3] += k * m[15];
    }
}

// Which SIDE actually received the weapon field-of-view scale. If the body's
// count is zero while the lens's is not, the lens is scaling alone about the
// screen centre - which looks exactly like the lens flying off the optic.
volatile LONG g_poseOnBody = 0;
volatile LONG g_scaleOnLens = 0;
// *** DRAWS OF A KNOWN LENS MESH THAT WE DID NOT CATCH. ***
//
// The per-frame glass counter counts draws we CAUGHT, so a missed one never
// appears in it - 'min 2, max 3' was never evidence of coverage, and reading it
// as such cost several rounds. This counts the other side: a draw of a mesh we
// KNOW is a lens, that no rule claimed. Every one of those renders the lens
// unmoved for that frame, which is the owner's 'it detaches when it flickers'.
volatile LONG g_lensMeshSeen = 0;
volatile LONG g_lensMeshMissed = 0;
volatile LONG g_lensMissedGapMin = 999999;
volatile LONG g_lensMissedGapMax = 0;
// *** WHAT ELSE IS DRAWN ON THE WEAPON THAT NO RULE CLAIMS? ***
//
// Every draw of a KNOWN lens mesh is caught (missed 0), yet a second copy of
// the glass is visibly there - bigger, untransformed, behind the one we move.
// So it is a mesh we have never learned. This tallies the index counts of
// unclaimed draws sitting inside the weapon's window, most frequent first: the
// missing lens piece has to be in that list.
constexpr int kUnclaimed = 24;
UINT g_unclaimedIdx[kUnclaimed] = {};
volatile LONG g_unclaimedHits[kUnclaimed] = {};
volatile LONG g_unclaimedCount = 0;

void NoteUnclaimedDraw(UINT idx) {
    const LONG n = g_unclaimedCount;
    for (LONG i = 0; i < n && i < kUnclaimed; ++i) {
        if (g_unclaimedIdx[i] == idx) { InterlockedIncrement(&g_unclaimedHits[i]); return; }
    }
    if (n >= kUnclaimed) return;
    const LONG slot = InterlockedIncrement(&g_unclaimedCount) - 1;
    if (slot < 0 || slot >= kUnclaimed) return;
    g_unclaimedHits[slot] = 1;
    g_unclaimedIdx[slot] = idx;      // published last
}

// *** WHERE THE WEAPON SITS, AND HOW WIDE ITS OWN LENS IS. ***
//
// This engine gives the viewmodel a projection of its own - the gun is drawn
// with a different field of view from the world, which is why a weapon can look
// right on a monitor and far too large in a headset. Both live in the same
// matrix this file already rewrites per eye, so both are reachable from here.
//
// Columns are addressed exactly as ShiftWeaponClipX does. In layout 0 they are
// strided by 4 (clip.x from m[0],m[4],m[8],m[12]) and column 3 is the w row;
// layout 1 is the transpose. Adding o * column3 to a column is a constant offset
// in normalised device coordinates - the same trick the eye shift uses - and
// scaling columns 0 and 1 scales clip x and y, which IS the weapon's field of
// view: scale below 1 and the gun shrinks as though seen through a wider lens.
//
// Z is offered with a caveat worth stating: it moves the gun through the DEPTH
// BUFFER, so pushed far enough, world geometry will cut into it. It is for small
// corrections, not for moving the gun across the room.
void ApplyWeaponPose(float* m) {
    const Config& c = Cfg();
    const float s2 = c.weapon_view_scale;
    const float ox = c.weapon_view_offset_x;
    const float oy = c.weapon_view_offset_y;
    if (s2 == 1.0f && ox == 0.0f && oy == 0.0f) return;
    InterlockedIncrement(&g_poseOnBody);

    if (c.weapon_3d_layout == 0) {
        if (s2 != 1.0f) {
            m[0] *= s2; m[4] *= s2; m[8]  *= s2; m[12] *= s2;   // clip x
            m[1] *= s2; m[5] *= s2; m[9]  *= s2; m[13] *= s2;   // clip y
        }
        if (ox != 0.0f) {
            m[0]  += ox * m[3];  m[4]  += ox * m[7];
            m[8]  += ox * m[11]; m[12] += ox * m[15];
        }
        if (oy != 0.0f) {
            m[1]  += oy * m[3];  m[5]  += oy * m[7];
            m[9]  += oy * m[11]; m[13] += oy * m[15];
        }
    } else {
        if (s2 != 1.0f) {
            m[0] *= s2; m[1] *= s2; m[2] *= s2; m[3] *= s2;
            m[4] *= s2; m[5] *= s2; m[6] *= s2; m[7] *= s2;
        }
        if (ox != 0.0f) {
            m[0] += ox * m[12]; m[1] += ox * m[13];
            m[2] += ox * m[14]; m[3] += ox * m[15];
        }
        if (oy != 0.0f) {
            m[4] += oy * m[12]; m[5] += oy * m[13];
            m[6] += oy * m[14]; m[7] += oy * m[15];
        }
    }
}

// One substitute buffer PER CONTEXT. The engine records on several deferred
// contexts at once and mapping one resource from two contexts at the same time
// is invalid, so they cannot share.
struct CtxCB {
    ID3D11DeviceContext* ctx;
    ID3D11Buffer* buf;
};
constexpr int kMaxCtxCB = 16;
CtxCB g_ctxCB[kMaxCtxCB] = {};
volatile LONG g_ctxCBCount = 0;

ID3D11Buffer* SubstituteCBFor(ID3D11DeviceContext* ctx) {
    const LONG n = g_ctxCBCount;
    for (LONG i = 0; i < n && i < kMaxCtxCB; ++i) {
        if (g_ctxCB[i].ctx == ctx) return g_ctxCB[i].buf;
    }
    if (n >= kMaxCtxCB) return nullptr;

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) return nullptr;
    D3D11_BUFFER_DESC bd{};
    // As large as the snapshot, NOT 256 bytes. The engine binds the whole pool
    // with no range - "block sizes ... 65536B" - so a shader declaring a bigger
    // block than InstanceConsts (the big mesh wants LocalConstants, 6944 bytes)
    // reads straight off the end of a 256-byte stand-in and the mesh disappears.
    // That is what left only the scope on screen.
    bd.ByteWidth = kPoolSnapshot;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Buffer* buf = nullptr;
    const HRESULT hr = dev->CreateBuffer(&bd, nullptr, &buf);
    dev->Release();
    if (FAILED(hr) || !buf) {
        COTW_LOG("[weapon3d] could not create the substitute constant buffer "
                 "(0x%08X) - the gun stays flat", (unsigned)hr);
        return nullptr;
    }
    const LONG idx = InterlockedIncrement(&g_ctxCBCount) - 1;
    if (idx < 0 || idx >= kMaxCtxCB) { buf->Release(); return nullptr; }
    g_ctxCB[idx].buf = buf;
    g_ctxCB[idx].ctx = ctx;          // published last
    COTW_LOG("[weapon3d] substitute constant buffer ready for context %p (%ld in "
             "use)", (void*)ctx, (long)(idx + 1));
    return buf;
}

volatile LONG g_matchStencil = 0;
volatile LONG g_matchMask = 0;
// Draws claimed ONLY by the loose rules - no stencil tag, no glass rule. These
// are the ones weapon_stencil_only stops moving.
volatile LONG g_looseOnly = 0;
// *** THE SCOPE'S MAGNIFIED PICTURE MUST NOT BE MOVED. ***
//
// The owner found it by holding free-look while aimed: a blue square carrying
// the crosshair, which follows the HEAD rather than the scope, slides out of the
// scope view and vanishes past its edge. The flicker happens ONLY inside that
// square, and only where a scope part sits behind it - move the square off the
// turret and that part stops flickering.
//
// It is a screen-space quad: its shader samples a full-size COPY of the scene
// instead of showing real geometry. The capture said so - 'the magnification is
// screen-space, done inside the lens shader at full resolution' - and the frame
// analysis listed render-to-texture passes as ones that must be left alone.
//
// So inside the square the scope parts sit at their UNSHIFTED positions while
// the same parts outside are shifted, and the overlap alternates. Moving it can
// only ever be wrong: the picture is drawn from the camera, not from the weapon.
//
// Recognised by what it does rather than by what it is: a draw whose pixel
// shader reads a texture the size of the whole scene is showing a copy of the
// scene, and nothing on a weapon does that except a magnifying optic.
volatile LONG g_sceneW = 0;
volatile LONG g_sceneH = 0;
volatile LONG g_skippedPicture = 0;
// *** THE OPTIC GLASS, BY ITS EXACT PIPELINE STATE. ***
//
// Measured across six captures at four resolutions: the scope/binocular lens is
// always the SAME material - two DrawIndexed calls, 1584 indices, one 289-vertex
// disc, VS 2205 + PS 1320, sampling a full-size CopyResource of the scene.
// Its state is a fingerprint no other draw in those frames shares:
//
//   depth  enable=TRUE, writes=FALSE, GREATER_EQUAL   (reverse-Z, equal passes)
//   stencil enable=TRUE, func=ALWAYS, ref=0x12, read=0xFF, write=0x12, REPLACE
//   two render targets, the second R16G16 (motion vectors)
//   blend SrcAlpha / InvSrcAlpha
//
// Two measurements here overturn earlier work:
//
// 1. The optic draws test stencil with func=ALWAYS, compareMask=0xFF. They do
//    NOT read bit 6, so the mask disc cannot gate them - and two captures show
//    the optic drawing in frames with NO 0x40 stamper anywhere. Every theory
//    that made the mask the engine of the flicker is dead.
// 2. The lens disc's screen footprint TRACKS THE WEAPON across captures (443px
//    centred with the optic raised, 286x391 at the corner with it lowered). The
//    glass is weapon geometry, so leaving it unshifted while the body around it
//    moves guarantees the two disagree.
//
// So it gets the SAME treatment as the body: the identical matrix, for the same
// eye, through the constants - not a rasterizer approximation of it.
volatile LONG g_opticGlass = 0;
volatile LONG g_opticThisFrame = 0;
volatile LONG g_opticOddFrames = 0;
// The trade this makes: a mask whose constants cannot be resolved no longer
// moves at the rasterizer, it does not move at all. A one-frame miss is
// measurable; two rival positions are not. But if this is ever nonzero the
// fallback must come back for GLASS AND MASK TOGETHER - never one alone, or the
// split reopens by a different door.
volatile LONG g_maskNoConstants = 0;

bool DrawIsOpticGlass(ID3D11DeviceContext* ctx) {
    ID3D11DepthStencilState* dss = nullptr;
    UINT ref = 0;
    ctx->OMGetDepthStencilState(&dss, &ref);
    if (!dss) return false;
    D3D11_DEPTH_STENCIL_DESC d{};
    dss->GetDesc(&d);
    dss->Release();
    if (ref != 0x12) return false;
    if (!d.DepthEnable || d.DepthWriteMask != D3D11_DEPTH_WRITE_MASK_ZERO) return false;
    if (d.DepthFunc != D3D11_COMPARISON_GREATER_EQUAL) return false;
    if (!d.StencilEnable) return false;
    if (d.StencilReadMask != 0xFF || d.StencilWriteMask != 0x12) return false;
    if (d.FrontFace.StencilFunc != D3D11_COMPARISON_ALWAYS) return false;
    if (d.FrontFace.StencilPassOp != D3D11_STENCIL_OP_REPLACE) return false;

    // Two targets, the second one motion vectors. Cheap last, because it costs
    // an AddRef per view.
    ID3D11RenderTargetView* rtv[2] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(2, rtv, &dsv);
    bool ok = (rtv[0] != nullptr && rtv[1] != nullptr);
    if (ok) {
        D3D11_RENDER_TARGET_VIEW_DESC rvd{};
        rtv[1]->GetDesc(&rvd);
        ok = (rvd.Format == DXGI_FORMAT_R16G16_TYPELESS ||
              rvd.Format == DXGI_FORMAT_R16G16_FLOAT ||
              rvd.Format == DXGI_FORMAT_R16G16_UNORM ||
              rvd.Format == DXGI_FORMAT_R16G16_SNORM);
    }
    if (rtv[0]) rtv[0]->Release();
    if (rtv[1]) rtv[1]->Release();
    if (dsv) dsv->Release();
    return ok;
}

bool DrawSamplesWholeScene(ID3D11DeviceContext* ctx) {
    if (g_sceneW <= 0 || g_sceneH <= 0) return false;
    ID3D11ShaderResourceView* srv[4] = {};
    ctx->PSGetShaderResources(0, 4, srv);
    bool whole = false;
    for (int i = 0; i < 4; ++i) {
        if (!srv[i]) continue;
        if (!whole) {
            ID3D11Resource* res = nullptr;
            srv[i]->GetResource(&res);
            if (res) {
                ID3D11Texture2D* tex = nullptr;
                if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                                                  (void**)&tex)) && tex) {
                    D3D11_TEXTURE2D_DESC td{};
                    tex->GetDesc(&td);
                    if ((LONG)td.Width == g_sceneW && (LONG)td.Height == g_sceneH) {
                        whole = true;
                    }
                    tex->Release();
                }
                res->Release();
            }
        }
        srv[i]->Release();
    }
    return whole;
}

// *** ARE WE LOOKING THROUGH AN OPTIC RIGHT NOW? ***
//
// The lens mask answers this for nothing. That draw - the one that stamps the
// circle into stencil bit 6 - exists ONLY while a scope or binoculars are
// raised: no optic, no circle to cut, no stamp. So it is a free, exact,
// weapon-independent "player is scoped" signal, with no address to scan for, no
// game-state pointer to chase and nothing to re-find when the game updates.
//
// g_scopedNow is the answer for the frame being drawn; g_maskSeenThisFrame
// accumulates evidence for the next one. Latching it a frame behind keeps the
// value STABLE for every draw of a frame - the stamp happens early (eid 17030
// against the shroud's 17627), but not before literally every viewmodel draw,
// and a strength that changed halfway through a frame would tear the gun in two.
volatile LONG g_scopedNow = 0;
volatile LONG g_maskSeenThisFrame = 0;

// PlayerIsScoped() is defined just below, OUTSIDE the anonymous namespace - it is
// declared in cbscan.h now, and headtrack.cpp needs to be able to link to it.

// The per-eye shift, in NDC. One place, so the body, the glass and the mask can
// never disagree about how far to move - which they would the moment a second
// copy of this rule existed.
float Weapon3dAmount() {
    const Config& c = Cfg();
    return (c.weapon_3d_scoped_separate && PlayerIsScoped()) ? c.weapon_3d_amount_scoped
                                                             : c.weapon_3d_amount;
}
volatile LONG g_matchGlass = 0;
volatile LONG g_weapon3dGlassNoMatch = 0;
volatile LONG g_weapon3dPatched = 0;
volatile LONG g_weapon3dAltOffset = 0;
volatile LONG g_weapon3dSkippedOffset = 0;
// WHICH pieces lose their snapshot. Each one is a part of the weapon drawn
// unshifted for a single frame, so this names what the player sees jump.
constexpr int kSkipMesh = 16;
UINT g_skipMeshIdx[kSkipMesh] = {};
volatile LONG g_skipMeshHits[kSkipMesh] = {};
volatile LONG g_skipMeshCount = 0;

void NoteSkippedMesh(UINT idx) {
    const LONG n = g_skipMeshCount;
    for (LONG i = 0; i < n && i < kSkipMesh; ++i) {
        if (g_skipMeshIdx[i] == idx) { InterlockedIncrement(&g_skipMeshHits[i]); return; }
    }
    if (n >= kSkipMesh) return;
    const LONG slot = InterlockedIncrement(&g_skipMeshCount) - 1;
    if (slot < 0 || slot >= kSkipMesh) return;
    g_skipMeshHits[slot] = 1;
    g_skipMeshIdx[slot] = idx;      // published last
}
volatile LONG g_weapon3dSkippedSize = 0;
volatile LONG g_weapon3dNoCtx1 = 0;

// What the weapon's draws actually ask for at that slot. Guessing the block size
// is what made most of the gun vanish; this prints the truth instead. Sizes are
// in BYTES so they can be read against the shader reflection directly.
struct CBRange { UINT bytes; LONG hits; };
constexpr int kMaxCBRanges = 12;
CBRange g_cbRanges[kMaxCBRanges] = {};
volatile LONG g_cbRangeCount = 0;

void NoteWeaponCBRange(UINT first, UINT count) {
    (void)first;
    const UINT bytes = count * 16u;
    const LONG n = g_cbRangeCount;
    for (LONG i = 0; i < n && i < kMaxCBRanges; ++i) {
        if (g_cbRanges[i].bytes == bytes) {
            InterlockedIncrement(&g_cbRanges[i].hits);
            return;
        }
    }
    if (n >= kMaxCBRanges) return;
    const LONG idx = InterlockedIncrement(&g_cbRangeCount) - 1;
    if (idx < 0 || idx >= kMaxCBRanges) return;
    g_cbRanges[idx].hits = 1;
    g_cbRanges[idx].bytes = bytes;      // published last
}

// *** WHICH MESHES ARE WE MISSING? ***
//
// A mesh whose draws are only PARTLY shifted is worse than one left alone: the
// shifted copy fights the unshifted depth and shows up as a faint ghost that
// slides when the strength changes, which is exactly what a doubled hand looks
// like. Counting shifted-vs-skipped PER INDEX COUNT names the meshes instead of
// leaving it to inference - the index count is how we identify them everywhere
// else in this file.
// offset: which layout this mesh's block uses, decided ONCE and then kept.
//
// Deciding per draw let the answer flip between a mesh's own rounds - two depth
// draws and a colour draw - so parts of one mesh got their projection shifted
// and parts got their world matrix mangled instead. That is half a hand
// disappearing. Whatever the first draw of a mesh says, every later draw of the
// same mesh follows, in both eyes.
constexpr UINT kOffsetUndecided = 0xFFFFFFFFu;
struct MeshTally {
    UINT idx; LONG shifted; LONG skipped; UINT offset;
    // How far into a pass this count is drawn, measured from the last depth
    // clear. THE question: are the weapon's draws and the impostor's separable
    // by this at all? If a count's range is 0..40 it is only ever the weapon; if
    // it spans 0..1500 the same count is drawn in both places and no threshold
    // can tell them apart, so the discriminator has to be something else.
    LONG minClear; LONG maxClear; LONG seen;
};
constexpr int kMaxMeshTally = 24;
MeshTally g_meshTally[kMaxMeshTally] = {};
volatile LONG g_meshTallyCount = 0;

MeshTally* MeshEntry(UINT idx) {
    if (!idx) return nullptr;
    const LONG n = g_meshTallyCount;
    for (LONG i = 0; i < n && i < kMaxMeshTally; ++i) {
        if (g_meshTally[i].idx == idx) return &g_meshTally[i];
    }
    if (n >= kMaxMeshTally) return nullptr;
    const LONG slot = InterlockedIncrement(&g_meshTallyCount) - 1;
    if (slot < 0 || slot >= kMaxMeshTally) return nullptr;
    g_meshTally[slot].offset = kOffsetUndecided;
    g_meshTally[slot].minClear = 0x7FFFFFFF;
    g_meshTally[slot].maxClear = -1;
    g_meshTally[slot].idx = idx;      // published last
    return &g_meshTally[slot];
}

// Recorded for EVERY draw carrying a seeded count, matched or not - the point is
// to see where the impostors are drawn, and they are the ones we refuse.
void NoteSeededCountClear(UINT idx, LONG sinceClear) {
    MeshTally* e = MeshEntry(idx);
    if (!e) return;
    InterlockedIncrement(&e->seen);
    if (sinceClear < e->minClear) e->minClear = sinceClear;
    if (sinceClear > e->maxClear) e->maxClear = sinceClear;
}

void NoteMeshOutcome(UINT idx, bool shifted) {
    if (MeshTally* e = MeshEntry(idx)) {
        InterlockedIncrement(shifted ? &e->shifted : &e->skipped);
    }
}

// The sticky decision. The first draw of a mesh works the layout out from the
// block's contents; every draw of that mesh afterwards is told the same answer.
UINT OffsetForMesh(UINT idx, const uint8_t* block, UINT bytes) {
    MeshTally* e = MeshEntry(idx);
    if (!e) return WvpOffsetIn(block, bytes);
    if (e->offset == kOffsetUndecided) e->offset = WvpOffsetIn(block, bytes);
    return e->offset;
}

// Which draws the glass path catches, named by index count, with how far past
// the last tagged draw they were and which offset was used on them.
struct GlassTally { UINT idx; LONG hits; int minGap; int maxGap; };
constexpr int kMaxGlassTally = 16;
GlassTally g_glassTally[kMaxGlassTally] = {};
volatile LONG g_glassTallyCount = 0;

void NoteGlassDraw(UINT idx, int gap) {
    const LONG n = g_glassTallyCount;
    for (LONG i = 0; i < n && i < kMaxGlassTally; ++i) {
        if (g_glassTally[i].idx == idx) {
            InterlockedIncrement(&g_glassTally[i].hits);
            if (gap < g_glassTally[i].minGap) g_glassTally[i].minGap = gap;
            if (gap > g_glassTally[i].maxGap) g_glassTally[i].maxGap = gap;
            return;
        }
    }
    if (n >= kMaxGlassTally) return;
    const LONG s = InterlockedIncrement(&g_glassTallyCount) - 1;
    if (s < 0 || s >= kMaxGlassTally) return;
    g_glassTally[s].hits = 1;
    g_glassTally[s].minGap = gap;
    g_glassTally[s].maxGap = gap;
    g_glassTally[s].idx = idx;      // published last
}

void ReportGlassTally() {
    const LONG n = g_glassTallyCount;
    if (n <= 0) {
        COTW_LOG("[weapon3d]   glass path caught NOTHING - the lens is not even "
                 "reaching it (blended? within %d draws of a tagged one?)",
                 Cfg().weapon_3d_glass);
        return;
    }
    char line[420];
    line[0] = 0;
    for (LONG i = 0; i < n && i < kMaxGlassTally; ++i) {
        const int len = (int)strlen(line);
        _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE,
                    "%s%u x%ld gap%d-%d", len ? "   " : "", g_glassTally[i].idx,
                    (long)g_glassTally[i].hits, g_glassTally[i].minGap,
                    g_glassTally[i].maxGap);
        InterlockedExchange(&g_glassTally[i].hits, 0);
        g_glassTally[i].minGap = 9999;
        g_glassTally[i].maxGap = -1;
    }
    COTW_LOG("[weapon3d]   glass path caught (index count x hits, gap from the "
             "last tagged draw): %s   -- the binocular lens is 2208 in the "
             "capture", line);
}

void ReportMeshTally() {
    const LONG n = g_meshTallyCount;
    if (n <= 0) return;
    COTW_LOG("[weapon3d] per mesh (index count: shifted / skipped) - a mesh with "
             "BOTH is the ghosting one:");
    char line[420];
    line[0] = 0;
    for (LONG i = 0; i < n && i < kMaxMeshTally; ++i) {
        const LONG sh = g_meshTally[i].shifted, sk = g_meshTally[i].skipped;
        const int len = (int)strlen(line);
        const UINT off = g_meshTally[i].offset;
        const LONG lo = g_meshTally[i].minClear, hi = g_meshTally[i].maxClear;
        _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE,
                    "%s%u: %ld/%ld @%s clear=%ld..%ld n=%ld%s", len ? "   " : "",
                    g_meshTally[i].idx, (long)sh, (long)sk,
                    off == kOffsetUndecided ? "?" : (off ? "0x40" : "0x00"),
                    (long)(hi < 0 ? -1 : lo), (long)hi, (long)g_meshTally[i].seen,
                    (sh && sk) ? " <<" : "");
        if (((i + 1) % 3) == 0) { COTW_LOG("[weapon3d]   %s", line); line[0] = 0; }
        InterlockedExchange(&g_meshTally[i].shifted, 0);
        InterlockedExchange(&g_meshTally[i].skipped, 0);
        InterlockedExchange(&g_meshTally[i].seen, 0);
        g_meshTally[i].minClear = 0x7FFFFFFF;
        g_meshTally[i].maxClear = -1;
    }
    if (line[0]) COTW_LOG("[weapon3d]   %s", line);
}

void ReportWeaponCBRanges() {
    const LONG n = g_cbRangeCount;
    if (n <= 0) return;
    char line[384];
    line[0] = 0;
    for (LONG i = 0; i < n && i < kMaxCBRanges; ++i) {
        const int len = (int)strlen(line);
        _snprintf_s(line + len, sizeof(line) - len, _TRUNCATE, "%s%uB x%ld",
                    len ? ", " : "", g_cbRanges[i].bytes,
                    (long)g_cbRanges[i].hits);
    }
    COTW_LOG("[weapon3d] block sizes the weapon's draws bind at that slot: %s "
             "(0B means the whole buffer - no range given)", line);
}

// What the game had bound, so it can be put back EXACTLY - buffer and range.
struct SavedCB {
    ID3D11Buffer* buf = nullptr;
    UINT first = 0;      // in 16-byte constants, not bytes
    UINT count = 0;
    bool valid = false;
};

// *** THE OFFSET IS THE WHOLE STORY. ***
//
// The engine keeps one large constant pool and binds a RANGE of it per draw
// through VSSetConstantBuffers1. Binding a replacement with the plain
// VSSetConstantBuffers throws the range away: the shader then reads from offset
// 0 of our little buffer instead of from its own block, gets nonsense, and the
// mesh drops out. That is exactly what was seen - every part of the gun vanished
// EXCEPT the scope, whose draw happens to bind at offset 0, which is the one
// case where losing the offset changes nothing.
//
// So: read the range, copy the block from that offset in our pool snapshot,
// shift it, and bind ours as a full 16-constant range. If the block sits beyond
// what we snapshotted, do nothing at all - an unpatched mesh is flat, a
// mis-patched one is missing.
ID3D11DeviceContext1* AsCtx1(ID3D11DeviceContext* ctx) {
    ID3D11DeviceContext1* c1 = nullptr;
    if (SUCCEEDED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1),
                                      (void**)&c1))) {
        return c1;   // caller releases
    }
    return nullptr;
}

// *** GLASS, IDENTIFIED RATHER THAN GUESSED. ***
//
// The lens draw's clip matrix is BIT-IDENTICAL to the tagged binocular body's -
// same object, same instant - so a blended draw can be checked against the
// matrices we have just shifted rather than merely being near them in the frame.
//
// That matters twice over. It picks the right offset with certainty instead of
// inferring it, and it rejects the blended draws that are NOT glass: the
// proximity rule catches 8 a frame and only one of them is a lens. Anything that
// does not match a tagged matrix is left completely alone, so a world
// transparency wandering into the window can never be dragged along.
// GLOBAL, not thread_local. The engine records on several deferred contexts, so
// the glass draw routinely lands on a different thread from the tagged draws
// that would have filled a per-thread ring - which leaves it empty and rejects
// every lens. That is exactly what happened: 812 blended draws entered the glass
// path and 822 skips came back out.
//
// A torn read here costs one unshifted lens for one frame, so no lock: the worst
// case is a miss, never a wrong shift.
constexpr int kClipRing = 32;
float g_clipRing[kClipRing][16] = {};
volatile LONG g_clipRingNext = 0;
volatile LONG g_clipRingCount = 0;

// First 8 bytes of each ring entry, as a cheap prefilter for the write-time
// scan below: a full 64-byte compare at every 16-byte position of every
// constant-buffer upload would be far too expensive.
uint64_t g_clipRingHead[kClipRing] = {};
// The signed shift applied to each ring entry. A glass draw belongs to the BODY
// whose matrix it carries, not to whichever body happened to be shifted most
// recently - the transparency pass is recorded on its own deferred context, so
// 'most recent' crosses threads and lands on the other eye at random. Carrying
// k with the matrix identity removes time from the question entirely.
float g_clipRingK[kClipRing] = {};

LONG NoteTaggedClipMatrix(const float* m) {
    const LONG slot = (InterlockedIncrement(&g_clipRingNext) - 1) % kClipRing;
    if (slot < 0 || slot >= kClipRing) return -1;
    memcpy(g_clipRing[slot], m, 16 * sizeof(float));
    memcpy(&g_clipRingHead[slot], m, sizeof(uint64_t));
    if (g_clipRingCount < kClipRing) InterlockedIncrement(&g_clipRingCount);
    return slot;
}

bool ClipMatrixWasTagged(const float* m) {
    const LONG n = g_clipRingCount;
    for (LONG i = 0; i < n && i < kClipRing; ++i) {
        if (memcmp(g_clipRing[i], m, 16 * sizeof(float)) == 0) return true;
    }
    return false;
}

// *** OPTION A: shift the glass's own copy, in the game's buffer, at write time.
//
// Walks a constant-buffer upload looking for a 4x4 bit-identical to one the
// tagged weapon draws used this frame, and applies the same per-eye clip shift
// in place. The lens then reads an already-shifted matrix and moves with the
// body - no draw-time identification, no substitute buffer, nothing that cares
// which thread or context the lens is drawn on.
//
// Bit-identity is the safety: 64 bytes of camera-dependent floats cannot
// collide by accident, so only geometry genuinely sharing the weapon's
// transform is touched. The body's own copy may also be hit here, which is
// harmless - the draw-time path skips anything already shifted because its
// pre-shift value no longer matches the ring.
volatile LONG g_glassInPlace = 0;
volatile LONG g_glassInPlaceScans = 0;

// *** THE UPLOAD, NOT THE BUFFER. ***
//
// Identifying glass by which BUFFER it binds was far too coarse: that buffer is
// a shared constant pool, so every blended draw binding it matched - house
// window glass and the grass-trample decal both started moving with the weapon.
//
// The engine map-discards and rewrites that pool PER DRAW, so an upload
// containing a copy of the weapon's matrix belongs to the draw that immediately
// follows it, on that same recording thread. That is a per-upload signal rather
// than a per-buffer one, and it cannot be shared with unrelated geometry.
// A COUNTDOWN, not a single-shot flag. Consuming it on the very next draw
// whatever that draw was meant an intervening opaque draw ate the mark and the
// lens went static again. It now survives a few draws and is spent only by a
// BLENDED one - the first transparent thing after the weapon's transform was
// written is the lens.
// *** THE EXACT BLOCK, NOT THE BUFFER AND NOT THE MOMENT. ***
//
// Two coarser signals both failed, each in its own way:
//   - "which BUFFER" was far too wide: that buffer is a shared pool, so window
//     glass and the grass-trample decal matched it too.
//   - "the draw right after the upload" was too narrow: the lens is drawn well
//     after its constants are written, so the mark was spent on something else
//     and only 6 unrelated draws a frame moved.
//
// What is exactly right is the ADDRESS: the write-time scan knows the byte
// offset where the weapon's matrix landed, and a draw declares the offset it
// reads its constants from (firstConstant). Same buffer AND same offset is the
// same block - no contents needed at draw time, so none of the snapshot
// problems apply.
// Two signals, because neither is sufficient alone and each covers the other's
// blind spot:
//
//   WHICH BUFFER - reaches the lens (this is the version where the scope was
//   correct in both eyes) but is far too wide on its own: the buffer is a shared
//   pool, so window glass and the grass-trample decal matched it too.
//
//   WHERE IN THE FRAME - the viewmodel is drawn at the very end, long after the
//   world pass. This is the same discriminator that separated the gun from a
//   fence sharing its index count, and it is what makes the buffer match safe.
//
// Address matching was tried in between and does not work: the lens reads a copy
// of the matrix that sits beyond the 4 KB scanned of a 64 KB pool, so the sites
// we know are never the ones it reads (7 sites known, 0 draws matched).
// *** SIZED FOR A ROTATING POOL, AND IT EVICTS. ***
//
// This was 16 entries that FROZE once full - NoteGlassSite returned early and no
// buffer could ever be learned again. The report said "16 distinct block site(s)"
// against a capacity of 16, which is what a saturated table looks like and what
// nobody read as one.
//
// D3D11 engines cycle dynamic constant buffers so the GPU is never stalled
// writing into one it is still reading. Once the rotation moved past the 16
// recorded objects, the lens read a buffer nothing knew about, was not
// recognised, and stood still for those frames - the binocular lenses and the
// scope glass flickering every couple of seconds as the rotation drifted.
//
// 128 entries with round-robin eviction: large enough for any plausible pool,
// and if it ever does fill, the oldest entry goes rather than the table sealing
// itself shut. A stale entry costs at worst one wrong shift on a recycled
// pointer; a sealed table costs a permanently flickering lens.
constexpr int kMaxGlassRes = 128;
ID3D11Resource* g_glassRes[kMaxGlassRes] = {};
volatile LONG g_glassResKnown = 0;
volatile LONG g_glassResNext = 0;       // round-robin cursor once full
// The shift that belongs to each recorded buffer, copied from the ring entry
// whose matrix was found inside it. This is what lets a lens drawn on the
// transparency thread use its OWN body's shift instead of the last one written
// by any thread.
float g_glassResK[kMaxGlassRes] = {};
volatile LONG g_glassResCount = 0;      // uploads flagged, for the report
volatile LONG g_glassSiteCount = 0;     // kept for the report line
// Blended draws inside the viewmodel reach whose constant buffer was NOT one
// we had recorded. Each is a frame where that lens did not move - the
// flicker, counted rather than watched for.
volatile LONG g_glassBufUnknown = 0;
// The eye the most recent WEAPON BODY was shifted for, published so the glass
// can copy it instead of deciding for itself. -1 until a body has been shifted.
volatile LONG g_bodyShiftEye = -1;
// The SIGNED SHIFT the most recent weapon body actually received, as raw float
// bits so it can live in an interlocked LONG. The glass copies this instead of
// deriving a sign of its own, because the eye is not trustworthy on the draw
// path - the mod believes it is rendering eye 0 for every weapon draw in the
// frame, so a sign computed there is right in one eye and inverted in the other.
constexpr LONG kNoBodyShift = 0x7FFFFFFF;
volatile LONG g_bodyShiftKBits = kNoBodyShift;
// Where each glass shift came from. If these are mostly 'global', the buffer
// lookup is not reaching the lens and the race is still live.
volatile LONG g_glassKFromBuffer = 0;
volatile LONG g_glassKFromGlobal = 0;
// Which route each lens draw actually took. If 'via constants' stays at zero,
// the lens's buffer is not reachable and the rasterizer is still doing the work.
volatile LONG g_glassViaConstants = 0;
volatile LONG g_glassViaViewport = 0;
// *** NAME THE EXTRA DRAW. ***
//
// Two lens draws a frame is right; the count reaches three on some frames, and
// a thing that moves only on some frames IS the flicker. The per-index tally
// cannot show it - it aggregates a whole window, so a third draw of the same
// mesh just makes the total 361 instead of 360. This records what the extra
// draw actually was, the moment it happens.
struct ExtraGlass { UINT idx; LONG gap; LONG nth; LONG blended; LONG tagged; };
constexpr int kMaxExtraGlass = 12;
ExtraGlass g_extraGlass[kMaxExtraGlass] = {};
volatile LONG g_extraGlassCount = 0;

// *** THE MESH THE MASK IS MADE OF, LEARNED FROM THE MASK ITSELF. ***
//
// The intermittent draw is 1584 indices, blended, WITHOUT the first-person tag
// - the scope's lens geometry, which the capture found shares its mesh with the
// mask (byte-identical post-VS bounds). It was being caught only when its
// constant buffer happened to be one of the handful we had recorded, and the
// engine rotates those buffers - so it moved on some frames and not others.
//
// The mask is identified from pipeline state and is never in doubt, so it can
// name the mesh. A blended draw of that same mesh, sitting on the viewmodel, is
// the lens - decided by nothing that rotates, changes or has to be learned from
// a lucky upload.
constexpr int kMaskIdx = 8;
UINT g_maskIdx[kMaskIdx] = {};
volatile LONG g_maskIdxCount = 0;
volatile LONG g_glassByMaskMesh = 0;
// The same idea as the mask mesh, for optics that have NO mask - binoculars.
// Learned from a lens the strict buffer rule has already confirmed, then used
// to catch that mesh's OTHER draws, including the ones that are not blended.
// Without this a binocular lens has blended copies moving and depth-only copies
// standing still, which is the same fight the scope had.
constexpr int kGlassMesh = 8;
UINT g_glassMesh[kGlassMesh] = {};
volatile LONG g_glassMeshCount = 0;
volatile LONG g_glassByGlassMesh = 0;

void NoteGlassMeshIndexCount(UINT idx) {
    if (!idx) return;
    const LONG n = g_glassMeshCount;
    for (LONG i = 0; i < n && i < kGlassMesh; ++i) if (g_glassMesh[i] == idx) return;
    if (n >= kGlassMesh) return;
    const LONG slot = InterlockedIncrement(&g_glassMeshCount) - 1;
    if (slot < 0 || slot >= kGlassMesh) return;
    g_glassMesh[slot] = idx;
    COTW_LOG("[weapon3d] lens mesh learned from a confirmed lens: %u indices - "
             "its other draws are caught now too, blended or not", idx);
}

bool IsGlassMeshIndexCount(UINT idx) {
    const LONG n = g_glassMeshCount;
    for (LONG i = 0; i < n && i < kGlassMesh; ++i) if (g_glassMesh[i] == idx) return true;
    return false;
}
// Constant buffers snapshotted from UpdateSubresource rather than Unmap. If
// this stays at zero the engine does not write constants that way and the lens
// buffer is reaching us by some third route.
volatile LONG g_snapViaUpdate = 0;
// *** WHAT IS THE LENS'S CONSTANT BUFFER, ACTUALLY? ***
//
// Every theory about why the constants path cannot reach it has been a guess:
// table capacity (wrong), UpdateSubresource (wrong), an unhooked context
// (unverified). The buffer itself answers the question - its Usage says how it
// is written, and its ByteWidth says whether the snapshot code even considers
// it, since BOTH snapshot paths require >= 256 bytes.
volatile LONG g_lensCBWidth = 0;
volatile LONG g_lensCBUsage = -1;
volatile LONG g_lensCBCpuFlags = 0;
volatile LONG g_lensCBBind = 0;
volatile LONG g_lensCBHaveSnap = -1;
volatile LONG g_lensCBSeen = 0;

void NoteMaskIndexCount(UINT idx) {
    if (!idx) return;
    const LONG n = g_maskIdxCount;
    for (LONG i = 0; i < n && i < kMaskIdx; ++i) if (g_maskIdx[i] == idx) return;
    if (n >= kMaskIdx) return;
    const LONG slot = InterlockedIncrement(&g_maskIdxCount) - 1;
    if (slot < 0 || slot >= kMaskIdx) return;
    g_maskIdx[slot] = idx;
    COTW_LOG("[weapon3d] lens mesh learned from the mask: %u indices - blended "
             "draws of it on the viewmodel are the scope glass", idx);
}

bool IsMaskMeshIndexCount(UINT idx) {
    const LONG n = g_maskIdxCount;
    for (LONG i = 0; i < n && i < kMaskIdx; ++i) if (g_maskIdx[i] == idx) return true;
    return false;
}

inline uint32_t FloatBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
inline float BitsToFloat(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
// Times the glass would have chosen a different eye than the body it belongs
// to. Every one of these was a lens rendered 2k away from its shroud for one
// frame - the flicker, which the owner confirmed IS the displacement: the
// lenses are out of place only on the frames they flicker.
volatile LONG g_glassEyeDisagree = 0;
// PER FRAME, not per report. A window total of 360 against an expected 360
// hides a frame that got 0 and a frame that got 2 perfectly - and a lens that
// is missed for ONE frame every few seconds is exactly what a flicker is. The
// aggregate cannot see the thing being complained about; this can.
volatile LONG g_glassHitsThisFrame = 0;
volatile LONG g_glassFrameMin = 999999;
volatile LONG g_glassFrameMax = 0;
volatile LONG g_glassFramesZero = 0;
volatile LONG g_glassFramesSeen = 0;
// Draws since the last first-person stencil-tagged draw, across all contexts.
// Small for anything in or just after the viewmodel pass, large out in the world.
volatile LONG g_drawsSinceTagged = 999999;

// *** THE ONE DISTINCTION THAT COVERS EVERY CASE. ***
//
// Picking the viewmodel apart piece by piece never converged: the binoculars
// wanted a reach of 1, the scope 2, the phone 0, and anything larger dragged in
// the world. Each item is not a different puzzle - the mistake was asking WHAT
// a draw is instead of WHERE IT IS BEING PAINTED.
//
// A scope's picture and the phone's screen are rendered into their own small
// textures and then pasted onto geometry. Shifting those moves the image INSIDE
// the screen, which is the phone's scrambled icons and part of the scope's
// trouble. The shroud, the lens and the invisible mask disc are all painted
// straight into the scene, so they must move together.
//
// So: shift only draws going to the same render target the tagged first-person
// draws use. Learned, not assumed - the tagged draws are exact.
// *** THE LENS MATERIAL, LEARNED AT RUNTIME. ***
//
// The owner found with ReShade's Shader Toggler that the scope's glass AND the
// invisible disc that cuts into the shroud are drawn by ONE shader pair
// (VS 604431419, PS 284683960) - hiding it removes both. That is the lens
// material, and it is the identification fifteen property-based attempts were
// groping for.
//
// Those hash numbers cannot be used directly: matching a shader by hash needs it
// fingerprinted at creation, and the mod only ever sees ~200 of the 1,226
// shaders a frame uses. But the SHADER OBJECT can be learned instead - take it
// from a lens draw already identified by the older rules, then match by pointer.
// The mask disc shares the pair, so it comes along for free without ever being
// identified in its own right.
// Global, not thread_local. The shader is registered on the device thread at
// creation; the match happens on whichever deferred context records the draw.
// That is the same bug that silently emptied the clip-matrix ring, the snapshot
// table and the gap counter - three times now - so it is stated here rather than
// rediscovered a fourth.
bool DrawUsesLensShader(ID3D11DeviceContext* ctx) {
    const LONG n = g_lensVSCount;
    if (n <= 0) return false;
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return false;
    bool same = false;
    for (LONG i = 0; i < n && i < kLensVS; ++i) {
        if (g_lensVS[i] == vs) { same = true; break; }
    }
    vs->Release();
    return same;
}

// *** THE SECOND LENS. ***
//
// Binoculars have two, and only one of them is ever caught: the buffer rule
// finds it every frame (2208 x180 over 180 frames), while its twin reads a
// constant buffer that was never recorded and is therefore never matched, never
// moved, and left sitting at its unshifted position. 450 blended draws a window
// near the gun read an unrecorded buffer against 4 buffers ever learned - a
// buffer is only recorded when an upload into it is caught carrying a
// recognisable weapon matrix, and the second lens's never is.
//
// The missing signal is what the two lenses obviously share: their material.
// Learn it from the lens the STRICT rule already confirmed, then accept its
// twin.
//
// *** AND THE REACH IS WHY THIS IS SAFE THIS TIME. *** An earlier build learned
// a shader the same way and moved 10,527 draws a frame against 2,637 genuinely
// first-person - it had picked up a common world shader. The difference is not
// the learning, it is the leash: that version applied the match out to 512 draws
// past the viewmodel, this one only within weapon_3d_glass (24), the same tight
// window the strict rule uses, and only to blended draws. A world shader learned
// by mistake can then only affect the handful of draws sitting on top of the gun.
constexpr int kLearnedGlassVS = 4;
ID3D11VertexShader* g_learnedGlassVS[kLearnedGlassVS] = {};
volatile LONG g_learnedGlassVSCount = 0;
volatile LONG g_glassBySecondLens = 0;   // draws the twin rule rescued

bool DrawUsesLearnedGlassShader(ID3D11DeviceContext* ctx) {
    const LONG n = g_learnedGlassVSCount;
    if (n <= 0) return false;
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return false;
    bool same = false;
    for (LONG i = 0; i < n && i < kLearnedGlassVS; ++i) {
        if (g_learnedGlassVS[i] == vs) { same = true; break; }
    }
    vs->Release();
    return same;
}

void LearnGlassShaderFromStrictHit(ID3D11DeviceContext* ctx) {
    if (g_learnedGlassVSCount >= kLearnedGlassVS) return;
    ID3D11VertexShader* vs = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    if (!vs) return;
    for (LONG i = 0; i < g_learnedGlassVSCount && i < kLearnedGlassVS; ++i) {
        if (g_learnedGlassVS[i] == vs) { vs->Release(); return; }
    }
    const LONG slot = InterlockedIncrement(&g_learnedGlassVSCount) - 1;
    if (slot >= kLearnedGlassVS) { vs->Release(); return; }
    g_learnedGlassVS[slot] = vs;    // reference held for the process
    COTW_LOG("[weapon3d] glass material %ld learned from a confirmed lens: VS %p "
             "- its twin on the other side of the binoculars can be matched now",
             (long)slot, (void*)vs);
}

ID3D11RenderTargetView* g_mainSceneRTV = nullptr;

void NoteMainSceneRTV(ID3D11DeviceContext* ctx) {
    if (g_mainSceneRTV) return;
    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (rtv) {
        g_mainSceneRTV = rtv;      // one reference held for the process
        // The scene's pixel size, so a draw that SAMPLES something this big can be
        // recognised as showing a copy of the scene.
        ID3D11Resource* res = nullptr;
        rtv->GetResource(&res);
        if (res) {
            ID3D11Texture2D* tex = nullptr;
            if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D),
                                              (void**)&tex)) && tex) {
                D3D11_TEXTURE2D_DESC td{};
                tex->GetDesc(&td);
                InterlockedExchange(&g_sceneW, (LONG)td.Width);
                InterlockedExchange(&g_sceneH, (LONG)td.Height);
                COTW_LOG("[weapon3d] scene is %ux%u - draws sampling a texture that "
                         "size are showing a COPY of it, and must not be moved",
                         td.Width, td.Height);
                tex->Release();
            }
            res->Release();
        }
    }
}

bool DrawGoesToMainScene(ID3D11DeviceContext* ctx) {
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);

    // *** NO RENDER TARGET IS NOT "SOMEWHERE ELSE" - IT IS DEPTH ONLY. ***
    //
    // The viewmodel's depth rounds bind ZERO render targets and a depth buffer;
    // the RenderDoc sweep recorded exactly that. The invisible disc that carves
    // out the scope's lens is one of these - it writes depth and nothing else,
    // which is why it can cut into a shroud that moved without it.
    //
    // Asking "is your target the scene target" therefore excluded the one draw
    // that most needed including. A draw with no colour target but a depth
    // target is scene geometry, and must move with everything else.
    const bool depthOnly = (rtv == nullptr && dsv != nullptr);
    const bool sceneColour = (rtv != nullptr && rtv == g_mainSceneRTV);

    if (rtv) rtv->Release();
    if (dsv) dsv->Release();
    return depthOnly || sceneColour;
}

bool IsKnownGlassResource(ID3D11Resource* res) {
    if (!res) return false;
    const LONG n = g_glassResKnown;
    for (LONG i = 0; i < n && i < kMaxGlassRes; ++i) {
        if (g_glassRes[i] == res) return true;
    }
    return false;
}

// The shift recorded for this buffer, if any. false when the buffer is unknown.
bool GlassShiftForResource(ID3D11Resource* res, float* out) {
    const LONG n = g_glassResKnown;
    for (LONG i = 0; i < n && i < kMaxGlassRes; ++i) {
        if (g_glassRes[i] == res) { *out = g_glassResK[i]; return true; }
    }
    return false;
}

void NoteGlassSite(ID3D11Resource* res, UINT off, float k) {
    (void)off;
    // Refresh the shift even for a buffer already known: the value changes every
    // eye and the identity does not.
    for (LONG i = 0; i < g_glassResKnown && i < kMaxGlassRes; ++i) {
        if (g_glassRes[i] == res) { g_glassResK[i] = k; return; }
    }
    const LONG n = g_glassResKnown;
    LONG s;
    if (n < kMaxGlassRes) {
        s = InterlockedIncrement(&g_glassResKnown) - 1;
    } else {
        // Full: evict the oldest rather than refusing to learn. A pool that
        // rotates more buffers than this table holds must not be able to lock
        // the lens out permanently.
        s = (InterlockedIncrement(&g_glassResNext) - 1) % kMaxGlassRes;
    }
    if (s < 0 || s >= kMaxGlassRes) return;
    g_glassResK[s] = k;
    g_glassRes[s] = res;      // published last
    InterlockedIncrement(&g_glassSiteCount);
}

// *** IDENTIFY HERE, SHIFT AT THE RASTERIZER. ***
//
// This used to shift the matrix in place, which fixed the binoculars and left
// the scope wrong in one eye: the lens's constants are written ONCE and read by
// BOTH eye passes, so a single stored value cannot carry two disparities. The
// eye that wrote them looked perfect, the other was displaced by twice the
// shift - exactly what the left/right screenshots showed.
//
// Moving the shift to draw time via a substitute buffer failed for a different
// reason: it needs the buffer's contents at draw time, and ~23,000 blended
// draws a report never have a snapshot. Neither a 256-entry table nor pinning
// changed that.
//
// So the shift no longer touches constants at all. This records WHICH BUFFER
// carries a weapon matrix, and the draw-time side moves those draws with a
// VIEWPORT offset instead - decided per eye where the eye is unambiguous,
// needing no buffer contents, and immune to which thread or context drew it.
void PatchMatchingClipMatrices(uint8_t* data, UINT bytes, ID3D11Resource* res) {
    // Capped: the blocks in play are a few hundred bytes, and an unbounded walk
    // of a 64 KB pool on every upload would cost more than the feature is worth.
    UINT limit = bytes < 4096u ? bytes : 4096u;
    if (limit < 64) return;
    limit -= 64;

    const LONG n = g_clipRingCount;
    InterlockedIncrement(&g_glassInPlaceScans);

    for (UINT off = 0; off <= limit; off += 16) {
        uint64_t head;
        memcpy(&head, data + off, sizeof(head));
        for (LONG i = 0; i < n && i < kClipRing; ++i) {
            if (head != g_clipRingHead[i]) continue;              // cheap reject
            if (memcmp(data + off, g_clipRing[i], 64) != 0) continue;
            // Record WHERE it landed. A draw reading its constants from this
            // exact buffer and offset is drawing with the weapon's transform.
            NoteGlassSite(res, off, g_clipRingK[i]);
            InterlockedIncrement(&g_glassResCount);
            InterlockedIncrement(&g_glassInPlace);
            return;                       // one hit is enough to flag the upload
        }
    }
}

// The same displacement the body gets, expressed in pixels instead of clip
// units: a clip-space shift of k moves NDC x by k, and NDC spans 2 across the
// viewport, so k * width/2 pixels. Sign comes from the eye RENDERING NOW.
struct GlassViewportShift {
    ID3D11DeviceContext* ctx = nullptr;
    D3D11_VIEWPORT saved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT count = 0;
    bool active = false;

    void Begin(ID3D11DeviceContext* c) {
        const Config& cf = Cfg();
        if (!cf.weapon_3d) return;

        // *** THE SHIFT THAT BELONGS TO THIS DRAW. ***
        //
        // Prefer the value the lens's OWN body received, looked up by the buffer
        // this draw reads: the transparency pass runs on its own deferred context,
        // so 'the last shift any thread made' can be the other eye's.
        float k = 0.0f;
        bool haveK = false;
        if (cf.weapon_3d_glass_follow_body_eye) {
            ID3D11Buffer* cb = nullptr;
            c->VSGetConstantBuffers(
                (UINT)(cf.weapon_3d_slot < 0 ? 1 : cf.weapon_3d_slot), 1, &cb);
            if (cb) {
                haveK = GlassShiftForResource(cb, &k);
                cb->Release();
            }
            if (haveK) {
                InterlockedIncrement(&g_glassKFromBuffer);
            } else {
                const LONG bits = g_bodyShiftKBits;
                if (bits != kNoBodyShift) {
                    k = BitsToFloat((uint32_t)bits);
                    haveK = true;
                    InterlockedIncrement(&g_glassKFromGlobal);
                }
            }
        }
        if (!haveK) {
            k = Weapon3dAmount();
            if (cf.stereo) {
                float sign = (CurrentRenderEye() == 0) ? -1.0f : 1.0f;
                if (cf.eye_swap) sign = -sign;
                k *= sign;
            }
        }

        // *** THE WHOLE TRANSFORM, IN ONE PLACE. ***
        //
        // The rasterizer is the right route for a lens because its shader samples
        // the scene in SCREEN SPACE: moving the viewport moves the geometry and its
        // sampling together, while moving the constants moves the geometry and
        // leaves the picture behind.
        //
        // But it has to carry EVERYTHING the body gets, or the lens comes off its
        // optic the moment the weapon is repositioned or resized. Units:
        //   - an NDC offset of o is o * half the viewport
        //   - a field-of-view scale of s is the viewport scaled by s about its
        //     centre, matching what scaling clip x and y does to the body
        const float sc = cf.weapon_view_scale;
        const float ox = cf.weapon_view_offset_x;
        const float oy = cf.weapon_view_offset_y;
        // Nothing to do at all: no eye shift, no move, no resize.
        if (k == 0.0f && ox == 0.0f && oy == 0.0f && sc == 1.0f) return;

        count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        c->RSGetViewports(&count, saved);
        if (!count || saved[0].Width <= 0.0f) { count = 0; return; }

        const float halfW = saved[0].Width * 0.5f;
        const float halfH = saved[0].Height * 0.5f;
        // *** THE EYE SHIFT IS SCALED BY THE FIELD OF VIEW, BECAUSE THE BODY'S IS.
        //
        // The body applies the eye shift FIRST and the field-of-view scale after,
        // so its shift comes out multiplied:  s*ndc + s*k + offset.  A viewport
        // that scales and then translates gives  s*ndc + k + offset.  The two
        // differ by (s-1)*k - exactly zero at 1.00, growing as the field of view
        // moves away from it, and proportional to the strength.
        //
        // That is the lens sitting off-centre inside its own shroud the moment the
        // weapon field of view is touched, and why the disc stopped responding to
        // strength once it was. Same transform, same order, both routes.
        const float px = k * sc * halfW * cf.weapon_3d_glass_scale + ox * halfW;
        // Screen y runs DOWN while NDC y runs UP, so this sign is flipped. Getting
        // it wrong moves the lens the opposite way from its own scope.
        const float py = -oy * halfH;   // no eye shift vertically

        D3D11_VIEWPORT moved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        for (UINT i = 0; i < count; ++i) {
            moved[i] = saved[i];
            if (sc != 1.0f) {
                if (i == 0) InterlockedIncrement(&g_scaleOnLens);
                moved[i].Width  = saved[i].Width * sc;
                moved[i].Height = saved[i].Height * sc;
                moved[i].TopLeftX = saved[i].TopLeftX +
                                    (saved[i].Width - moved[i].Width) * 0.5f;
                moved[i].TopLeftY = saved[i].TopLeftY +
                                    (saved[i].Height - moved[i].Height) * 0.5f;
            }
            moved[i].TopLeftX += px;
            moved[i].TopLeftY += py;
        }
        c->RSSetViewports(count, moved);
        ctx = c;
        active = true;
        InterlockedIncrement(&g_matchGlass);
    }

    void End() {
        if (active && ctx && count) ctx->RSSetViewports(count, saved);
        active = false;
    }
};

// Which offset in THIS block holds a matrix we just shifted on a tagged draw?
// Returns ~0u when none does, i.e. this blended draw is not part of the weapon.
UINT GlassOffsetIn(const uint8_t* block, UINT bytes) {
    if (bytes >= 0x40 &&
        ClipMatrixWasTagged(reinterpret_cast<const float*>(block))) {
        return 0;
    }
    if (bytes >= 0x80 &&
        ClipMatrixWasTagged(reinterpret_cast<const float*>(block + 0x40))) {
        return 0x40;
    }
    return ~0u;
}

// *** EVERY WAY THE CONSTANTS PATH CAN REFUSE A DRAW, COUNTED SEPARATELY. ***
//
// The report has only ever shown one of these ('our snapshot is of a different
// buffer'), around 20 a window. Routing every failure to the rasterizer
// flickered the entire weapon, which one-in-500 draws cannot do - so the other
// exits are firing far more often and nothing was measuring them.
//
// Named in order of the checks, so the report reads as a funnel.
volatile LONG g_bindExit[7] = {};
const char* const kBindExitName[7] = {
    "slot out of range",
    "no ID3D11DeviceContext1",
    "no snapshot of this draw's buffer",
    "draw's range lies past the snapshot",
    "no substitute buffer for this context",
    "Map/Unmap unavailable or Map failed",
    "no weapon matrix found in the block",
};

bool BindShiftedWeaponCB(ID3D11DeviceContext* ctx, SavedCB* saved, UINT meshIdx,
                         bool tagged) {
    saved->valid = false;
    struct Tally {
        UINT idx; bool done = false;
        ~Tally() { NoteMeshOutcome(idx, done); }
    } tally{meshIdx};

    const int slot = Cfg().weapon_3d_slot;
    if (slot < 0 || slot > 13) { InterlockedIncrement(&g_bindExit[0]); return false; }

    ID3D11DeviceContext1* c1 = AsCtx1(ctx);
    if (!c1) { InterlockedIncrement(&g_weapon3dNoCtx1); InterlockedIncrement(&g_bindExit[1]); return false; }

    ID3D11Buffer* prev = nullptr;
    UINT first = 0, count = 0;
    c1->VSGetConstantBuffers1((UINT)slot, 1, &prev, &first, &count);

    // *** THE SNAPSHOT MUST BE OF THIS EXACT BUFFER. ***
    //
    // Unmap snapshots every constant buffer big enough to be a candidate, so the
    // most recent one is frequently NOT the transform pool - material or
    // skinning constants land in between. Substituting those as a mesh's
    // transform sends it somewhere off screen, which is why most of the gun
    // disappeared while the scope (whose draw happened to follow the right
    // unmap) came through with correct depth.
    //
    // If we have no snapshot of the buffer this draw actually reads, we have
    // nothing valid to offer and must leave the draw alone.
    CBSnapshot* snap = FindSnapshot(prev);
    if (!snap || !snap->bytes) {
        InterlockedIncrement(&g_weapon3dSkippedSize);
        InterlockedIncrement(&g_bindExit[2]);
        NoteSkippedMesh(meshIdx);
        if (prev) prev->Release();
        c1->Release();
        return false;
    }

    // Where this draw's block actually lives. first is in 16-byte constants.
    //
    // THE RANGE IS NOT OPTIONAL. Ignoring it and checking the pool head made
    // ~30,000 particle draws per report "match" the viewmodel's matrices -
    // every draw bound deeper into the shared pool was being judged by the
    // leftover bytes at offset 0. Everything below is relative to the range
    // this draw actually binds; a range beyond the snapshot is a skip.
    const UINT offset = first * 16u;
    NoteWeaponCBRange(first, count);
    const UINT snapBytes = snap->bytes;

    // *** ONLY SUBSTITUTE A BLOCK OF THE SIZE THE SHADER EXPECTS. ***
    //
    // The viewmodel's shaders do not agree on what sits at b1. Most declare
    // InstanceConsts, 256 bytes. The big mesh declares LocalConstants, 6944.
    // Handing a 6944-byte shader our 256-byte buffer makes it read past the end,
    // and the mesh disappears - which is what "only the scope is left" was.
    //
    // count is in 16-byte constants; 0 means the whole buffer, which we also
    // cannot safely stand in for. Anything that is not exactly our block size is
    // left completely alone: flat but present beats absent.
    // *** DO NOT GATE ON THE BINDING SIZE. ***
    //
    // The engine binds the whole 65536-byte pool every time, so count*16 is
    // always 65536 - a check of "is the binding bigger than our buffer" is
    // therefore always true and skips every single draw. It did: "0 shifted,
    // 3608 skipped".
    //
    // The binding size describes the POOL, not what the shader reads. What
    // matters is the largest block any of these shaders declares at this slot -
    // LocalConstants at 6944 bytes - and our 8 KB stand-in already covers it.
    // Demands only a MATRIX, not a whole 256-byte InstanceConsts block. The lens
    // reads a 128-byte buffer, so requiring 256 rejected it on every single draw
    // even once the snapshot existed - the second half of the same mistake.
    if (offset + kMinSnapshotBytes > snapBytes) {
        InterlockedIncrement(&g_weapon3dSkippedOffset);
        InterlockedIncrement(&g_bindExit[3]);
        if (prev) prev->Release();
        c1->Release();
        return false;
    }

    ID3D11Buffer* sub = SubstituteCBFor(ctx);
    if (!sub) { InterlockedIncrement(&g_bindExit[4]); if (prev) prev->Release(); c1->Release(); return false; }

    D3D11_MAPPED_SUBRESOURCE ms{};
    auto origMap = (PFN_Map)g_oMap.For(ctx);
    auto origUnmap = (PFN_Unmap)g_oUnmap.For(ctx);
    if (!origMap || !origUnmap ||
        FAILED(origMap(ctx, sub, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms)) ||
        !ms.pData) {
        InterlockedIncrement(&g_bindExit[5]);
        if (prev) prev->Release();
        c1->Release();
        return false;
    }
    // *** COPY FROM THIS DRAW'S RANGE, NOT FROM THE POOL HEAD. ***
    //
    // We rebind at firstConstant = 0, so the shader's constant 0 is byte 0 of
    // OUR buffer. The game bound it at firstConstant = first, so its constant 0
    // is byte first*16 of the pool. Copying from the pool head therefore handed
    // every draw with a non-zero range somebody else's constants - and since
    // the weapon binds at 0 and writes there, that "somebody else" was the
    // weapon. 32,000 blended draws a report duly matched the weapon's matrices
    // and were shifted; the lens, which has its own range, never did.
    const UINT blockBytes = snapBytes - offset;
    memcpy(ms.pData, snap->data + offset, blockBytes);

    UINT wvpOff;
    if (Cfg().weapon_3d_wvp_offset >= 0) {
        wvpOff = (UINT)Cfg().weapon_3d_wvp_offset;
    } else if (tagged) {
        wvpOff = OffsetForMesh(meshIdx, static_cast<const uint8_t*>(ms.pData),
                               blockBytes);
    } else {
        // Blended, untagged: only shift it if its matrix is one we just shifted
        // on a tagged draw. No match means it is not part of the weapon.
        wvpOff = GlassOffsetIn(static_cast<const uint8_t*>(ms.pData), blockBytes);
        if (wvpOff == ~0u) {
            InterlockedIncrement(&g_bindExit[6]);
            origUnmap(ctx, sub, 0);
            if (prev) prev->Release();
            c1->Release();
            // Its OWN counter. Sharing one with "no snapshot for this buffer"
            // meant 822 skips could not be attributed, and the two want opposite
            // fixes.
            InterlockedIncrement(&g_weapon3dGlassNoMatch);
            return false;
        }
    }
    if (wvpOff) InterlockedIncrement(&g_weapon3dAltOffset);
    float* m = reinterpret_cast<float*>(static_cast<uint8_t*>(ms.pData) + wvpOff);
    // Recorded BEFORE the shift, so a following glass draw can recognise it.
    const LONG ringSlot = tagged ? NoteTaggedClipMatrix(m) : -1;
    const Config& c = Cfg();
    float k = Weapon3dAmount();
    if (c.stereo) {
        // THE EYE THIS BODY WAS SHIFTED FOR, PUBLISHED FOR THE GLASS TO COPY.
        //
        // The body is shifted HERE, when the game writes its constants. The lens
        // is shifted later, at draw time, and used to ask CurrentRenderEye()
        // again for itself. Two questions, two moments, and when the answer
        // changed in between the lens went one way while its shroud went the
        // other - displaced by 2k, in a direction set by the sign of the
        // strength, on just the frames where the flip landed.
        InterlockedExchange(&g_bodyShiftEye, (LONG)CurrentRenderEye());
        float sign = (CurrentRenderEye() == 0) ? -1.0f : 1.0f;
        if (c.eye_swap) sign = -sign;
        k *= sign;
    }
    ShiftWeaponClipX(m, k);
    ApplyWeaponPose(m);
    // *** PUBLISH THE SHIFT ITSELF, NOT THE EYE IT CAME FROM. ***
    //
    // Publishing the eye was not enough, and the reason is worth keeping: at
    // draw time the mod always believes it is rendering eye 0 (its own
    // diagnostic says so - "eye0 1801, eye1 0, the two eye passes are NOT going
    // through the same context"). So the glass always shifted with the left-eye
    // sign, while the body - shifted here, at constant-buffer write time - got
    // the correct sign for each eye. They agree in one eye and are 2k apart in
    // the other, which is exactly what is seen: the binocular lenses sit outside
    // the eyepieces in the left Quest 3 lens and correct in the right, and
    // nothing is wrong at all without a headset.
    //
    // Comparing the two eye values reported no disagreement because BOTH were
    // reading the same broken source. Publishing the resulting k removes the
    // question entirely: the glass cannot compute a different sign because it no
    // longer computes one.
    InterlockedExchange(&g_bodyShiftKBits, (LONG)FloatBits(k));
    // The shift now travels WITH this matrix, so a lens carrying a copy of it can
    // find the exact value its own body received - whichever eye that was,
    // whichever thread recorded it, however long ago.
    if (ringSlot >= 0 && ringSlot < kClipRing) g_clipRingK[ringSlot] = k;
    origUnmap(ctx, sub, 0);

    // Bound as a whole buffer, matching how the engine binds the pool.
    const UINT zero = 0, all = kPoolSnapshot / 16u;
    c1->VSSetConstantBuffers1((UINT)slot, 1, &sub, &zero, &all);
    c1->Release();

    saved->buf = prev;          // released by the restore
    saved->first = first;
    saved->count = count;
    saved->valid = true;
    InterlockedIncrement(&g_weapon3dPatched);
    tally.done = true;
    return true;
}

void RestoreWeaponCB(ID3D11DeviceContext* ctx, SavedCB* saved) {
    if (!saved->valid) return;
    const int slot = Cfg().weapon_3d_slot;
    if (slot >= 0 && slot <= 13) {
        if (ID3D11DeviceContext1* c1 = AsCtx1(ctx)) {
            // Range and all. Restoring the buffer but not its range would leave
            // the next draw reading the right pool from the wrong place.
            c1->VSSetConstantBuffers1((UINT)slot, 1, &saved->buf, &saved->first,
                                      &saved->count);
            c1->Release();
        }
    }
    if (saved->buf) saved->buf->Release();
    saved->valid = false;
}

bool IsWeaponBuffer(UINT bytes, UINT offset) {
    return bytes == (UINT)Cfg().weapon_cb_size && offset == (UINT)Cfg().weapon_cb_offset;
}

// THE OTHER STORAGE ORDER - and a blind spot in everything above.
//
// LooksLikeCloseModel demands m[3]/m[7]/m[11] be zero, which is true of a
// ROW-major transform and false of every COLUMN-major one. HLSL's default is
// column-major, so a transposed upload - the norm for a D3D11 engine - could
// never have been a candidate. The whole close-model hunt has been looking at
// half the possibilities.
//
// Column-major puts the translation in the last COLUMN:
//     row0 = (r00 r01 r02 tx)
//     row1 = (r10 r11 r12 ty)
//     row2 = (r20 r21 r22 tz)
//     row3 = ( 0   0   0   1)
bool LooksLikeCloseModelT(const float* m, float* distOut) {
    auto zero = [](float v) { return fabsf(v) < 1e-4f; };
    if (!zero(m[12]) || !zero(m[13]) || !zero(m[14])) return false;
    if (fabsf(m[15] - 1.0f) > 1e-3f) return false;
    // The basis is now the top-left 3x3 read down the columns; its column
    // lengths are what must be sane.
    for (int c = 0; c < 3; ++c) {
        const float len2 = m[c] * m[c] + m[c + 4] * m[c + 4] + m[c + 8] * m[c + 8];
        if (!isfinite(len2) || len2 < 0.04f || len2 > 25.0f) return false;
    }
    const float tx = m[3], ty = m[7], tz = m[11];
    const float d = sqrtf(tx * tx + ty * ty + tz * tz);
    if (!isfinite(d) || d < 0.05f || d > 2.0f) return false;
    *distOut = d;
    return true;
}

void ScanCloseModels(const float* f, UINT bytes) {
    const UINT count = bytes / 4;
    for (UINT i = 0; i + 16 <= count; i += 4) {
        float d = 0.0f;
        if (LooksLikeCloseModel(f + i, &d)) NoteModel(bytes, i * 4, d);
        // Recorded with the offset flagged so the report tells the two layouts
        // apart - they need opposite patches.
        if (LooksLikeCloseModelT(f + i, &d)) NoteModel(bytes, (i * 4) | 0x80000000u, d);
    }
}

void ScanProjections(const float* f, UINT bytes) {
    const UINT count = bytes / 4;
    for (UINT i = 0; i + 16 <= count; i += 4) {     // 16-byte aligned candidates
        float vfov = 0.0f;
        if (LooksLikeProjection(f + i, &vfov)) NoteProjection(vfov, bytes, i * 4);
    }
}

void ScanBuffer(const float* f, UINT bytes) {
    float cam[16];
    if (!CurrentCameraMatrix(cam)) return;

    const float* pos = &cam[12];
    const float* row0 = &cam[0];
    // A position of exactly zero matches far too much; wait until the player has
    // moved away from the origin.
    const bool posUsable = fabsf(pos[0]) + fabsf(pos[1]) + fabsf(pos[2]) > 1.0f;

    const UINT count = bytes / 4;
    for (UINT i = 0; i + 2 < count; ++i) {
        if (posUsable && Close(f[i], pos[0]) && Close(f[i + 1], pos[1]) &&
            Close(f[i + 2], pos[2])) {
            NoteHit(bytes, i * 4, 0);
        }
        if (Close(f[i], row0[0]) && Close(f[i + 1], row0[1]) && Close(f[i + 2], row0[2])) {
            NoteHit(bytes, i * 4, 1);
        }
    }
}

// *** GIVING THE WEAPON DEPTH, WITHOUT NEEDING ITS MATRIX. ***
//
// The matrix hunt is unnecessary. For an object at a FIXED distance from the
// eye, stereo disparity is just a horizontal shift on screen - that is what
// disparity IS. And now that the weapon's own draws can be identified exactly,
// they can be drawn through a shifted VIEWPORT and nothing else is touched.
//
// The shift in pixels comes straight from the geometry:
//     disparity = (ipd/2) * focal_px / distance
//     focal_px  = (width/2) / tan(hfov/2)
// At 2574 px wide, 94 deg across and a weapon ~0.5 m away that is about 75 px
// per eye - which is exactly the disparity a real object at arm's length makes.
//
// It also degrades gracefully: if the distance guess is off, the weapon simply
// sits nearer or further than life, which is a slider, not a bug.
float WeaponPixelShift() {
    if (!Cfg().weapon_screen_stereo || !Cfg().stereo) return 0.0f;
    const float half = Cfg().ipd_mm * 0.001f * 0.5f;
    const float dist = Cfg().weapon_distance_m > 0.05f ? Cfg().weapon_distance_m : 0.5f;
    const float hfovRad = Cfg().game_fov_deg * 0.01745329252f;
    const float widthPx = (float)Cfg().weapon_shift_width;
    const float focal = (widthPx * 0.5f) / tanf(hfovRad * 0.5f);
    float shift = half * focal / dist;

    float sign = (CurrentRenderEye() == 0) ? 1.0f : -1.0f;
    if (Cfg().eye_swap) sign = -sign;
    if (Cfg().weapon_shift_invert) sign = -sign;
    return shift * sign * Cfg().weapon_shift_scale;
}

// Draws the weapon through a horizontally shifted viewport, then puts the
// viewport straight back so nothing else in the frame is affected.
// Counted per frame and reported, because "it flickers" cannot distinguish
// "the shift reached two draws" from "it reached two hundred". If the vertex
// match fires on a flood of draws, that shader is shared with other geometry
// and matching on it is wrong.
volatile LONG g_shiftByPS = 0;
volatile LONG g_shiftByVS = 0;

// Every exit path out of Begin(), counted. "0 draws shifted" with a healthy
// 82.4 px shift means it is leaving through one of the early returns, and
// guessing which one is exactly the habit that has burned three test cycles.
volatile LONG g_vpEntered   = 0;   // the weapon's draw reached Begin()
volatile LONG g_vpZeroShift = 0;   // ...but the computed shift was zero
volatile LONG g_vpZeroCount = 0;   // ...but RSGetViewports reported no viewport
volatile LONG g_vpApplied   = 0;   // ...and the viewport was actually moved

struct WeaponViewportShift {
    ID3D11DeviceContext* ctx = nullptr;
    D3D11_VIEWPORT saved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT count = 0;
    bool active = false;

    void Begin(ID3D11DeviceContext* c) {
        InterlockedIncrement(&g_vpEntered);
        const float shift = WeaponPixelShift();
        if (shift == 0.0f) { InterlockedIncrement(&g_vpZeroShift); return; }
        count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        c->RSGetViewports(&count, saved);
        if (!count) { InterlockedIncrement(&g_vpZeroCount); return; }
        D3D11_VIEWPORT moved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        for (UINT i = 0; i < count; ++i) {
            moved[i] = saved[i];
            moved[i].TopLeftX += shift;
        }
        c->RSSetViewports(count, moved);
        ctx = c;
        active = true;
        InterlockedIncrement(&g_vpApplied);
        if (IsWeaponPixelShader(BoundPS(ctx))) InterlockedIncrement(&g_shiftByPS);
        else                                  InterlockedIncrement(&g_shiftByVS);
    }
    void End() {
        if (active && ctx && count) ctx->RSSetViewports(count, saved);
        active = false;
    }
};

void STDMETHODCALLTYPE Hook_VSSetShader(ID3D11DeviceContext* ctx, ID3D11VertexShader* vs,
                                        ID3D11ClassInstance* const* inst, UINT n) {
    if (ContextShaders* st = StateFor(ctx)) st->vs = vs;
    ShaderIndex(vs);   // the table must be built even while hiding nothing
    if (auto orig = (PFN_VSSetShader)g_oVSSetShader.For(ctx)) orig(ctx, vs, inst, n);
}

// Command lists, counted.
//
// FinishCommandList is what says whether the frame is RE-RECORDED each time.
// It has to be: the world moves, and a command list is immutable once closed.
// If it is recorded once per eye pass then the eye is known at record time and
// a per-eye viewport shift recorded into the list is correct. If one list were
// recorded and executed twice, both eyes would get the same shift and this
// whole approach would be dead - so this counter decides it.
using PFN_ExecuteCommandList = void(STDMETHODCALLTYPE*)(ID3D11DeviceContext*,
                                                        ID3D11CommandList*, BOOL);
using PFN_FinishCommandList = HRESULT(STDMETHODCALLTYPE*)(ID3D11DeviceContext*, BOOL,
                                                          ID3D11CommandList**);
PFN_ExecuteCommandList o_ExecuteCommandList = nullptr;
PFN_FinishCommandList  o_FinishCommandList = nullptr;
PFN_FinishCommandList  o_FinishCommandListDef = nullptr;
constexpr int kSlotExecuteCommandList = 58;
constexpr int kSlotFinishCommandList  = 114;

void STDMETHODCALLTYPE Hook_ExecuteCommandList(ID3D11DeviceContext* ctx,
                                               ID3D11CommandList* list, BOOL restore) {
    InterlockedIncrement(&g_execCmdList);
    if (auto orig = (PFN_ExecuteCommandList)g_oExecuteCommandList.For(ctx)) {
        orig(ctx, list, restore);
    }
}

HRESULT STDMETHODCALLTYPE Hook_FinishCommandList(ID3D11DeviceContext* ctx, BOOL restore,
                                                 ID3D11CommandList** out) {
    InterlockedIncrement(&g_finishCmdList);
    auto orig = (PFN_FinishCommandList)g_oFinishCommandList.For(ctx);
    return orig ? orig(ctx, restore, out) : E_FAIL;
}

// *** THE VIEWMODEL TEST, FOR EVERY KIND OF DRAW CALL. ***
//
// This lived inside Hook_DrawIndexed, and that single fact is why the scope's
// invisible disc survived fifteen different identification rules. D3D11 has six
// ways to issue geometry, and this frame uses at least five of them:
//
//     DrawIndexedInstanced 785, DrawIndexed 718,
//     DrawIndexedInstancedIndirect 433, DrawInstancedIndirect 132, Draw 94
//
// MORE draws arrive through DrawIndexedInstanced than through DrawIndexed. Every
// rule tried so far - blend state, buffer identity, byte address, distance from
// the tagged draw, render target, and finally the material the owner found in
// ReShade - was only ever asked about DrawIndexed calls. The other three hooks
// did nothing but a shader-fingerprint check that misses ~85% of the frame's
// shaders, then passed the draw through untouched.
//
// So the rules were not wrong. They were never consulted. The tell was there in
// plain sight: weapon_3d_glass_whole_pass moves EVERY draw near the gun without
// identifying anything, and the disc still did not move. Nothing that reaches
// the hook can survive that, so the disc was not reaching the hook.
//
// Everything below is now shared, and the shift is applied identically wherever
// the geometry comes from.
struct ViewmodelHit {
    bool weapon  = false;
    bool stencil = false;
    bool glass   = false;
    bool mask    = false;   // stamps the lens circle rather than painting anything
};

ViewmodelHit ClassifyViewmodelDraw(ID3D11DeviceContext* ctx, UINT meshIdx) {
    ViewmodelHit h;

    // The engine's own first-person tag - stencil bit 6, in BOTH its roles: the
    // gun that reads it and the lens mask that writes it. Independent of
    // shaders, index counts and weapon choice.
    h.stencil = Cfg().weapon_match_stencil && DrawIsFirstPerson(ctx, &h.mask);
    // The optic glass, matched outright. Deliberately NOT h.glass: that routes a
    // draw to the rasterizer, and the measurement says this one wants the body's
    // own matrix, for the body's own eye.
    if (!h.stencil && Cfg().weapon_3d_optic_signature && DrawIsOpticGlass(ctx)) {
        h.stencil = true;
        InterlockedIncrement(&g_opticGlass);
        InterlockedIncrement(&g_opticThisFrame);
    }
    if (h.mask) {
        InterlockedIncrement(&g_matchMask);
        // Counted from EVERY draw path, not just DrawIndexed: the stamp is
        // the only thing that says an optic is raised, and missing it would
        // silently drop the player back to the hip-fire strength.
        InterlockedExchange(&g_maskSeenThisFrame, 1);
        // The mask names the mesh the lens is drawn from.
        NoteMaskIndexCount(meshIdx);
    }

    // Frame position, maintained for EVERY draw call regardless of type. It used
    // to advance only on DrawIndexed, which made "draws since the viewmodel" a
    // distance measured in a stream that skipped most of the frame - and the
    // reach values tuned against it (1 for binoculars, 2 for the scope) were
    // fitted to that skewed count.
    if (h.stencil) {
        InterlockedExchange(&g_drawsSinceTagged, 0);
        NoteMainSceneRTV(ctx);
    } else if (g_drawsSinceTagged < 999999) {
        InterlockedIncrement(&g_drawsSinceTagged);
    }

    // *** THE LENS, BY THE MESH THE MASK NAMED. ***
    //
    // Blended, on the viewmodel, drawn from the mesh the mask is drawn from.
    // Nothing here rotates or has to be learned from a lucky upload, so the lens
    // is caught on EVERY frame instead of on the frames its constant buffer
    // happens to be one we recorded. That intermittency was the flicker: the
    // measurement showed this exact draw - 1584 indices, blended, untagged -
    // present on some frames and missing on others.
    // Every draw of a lens mesh learned from a confirmed lens - the binocular
    // case, which has no mask to name its mesh.
    if (!h.glass && !h.stencil && !h.mask && Cfg().weapon_3d &&
        Cfg().weapon_3d_glass_mask_mesh && Cfg().weapon_3d_glass > 0 &&
        g_drawsSinceTagged <= (LONG)Cfg().weapon_3d_glass &&
        IsGlassMeshIndexCount(meshIdx) &&
        (!Cfg().weapon_stencil_only || DrawIsBlended(ctx))) {
        h.glass = true;
        InterlockedIncrement(&g_glassByGlassMesh);
    }

    // *** THE BLEND TEST IS BACK, under 'only move what is certain'. ***
    //
    // It was dropped on the theory that unblended copies of the mesh were being
    // missed and fighting the ones that moved. That theory is dead: narrowing the
    // rules fixed the weapon and the hands, and every widening made things worse.
    //
    // These two rules match by INDEX COUNT, which is the same kind of rule that
    // just proved unsafe - index counts are not unique across a map. Without the
    // blend test they claim any draw of that size sitting near the gun.
    //
    // Kept OFF the old path so the previous behaviour is still one switch away.
    // *** EVERY DRAW OF THE MESH, BLENDED OR NOT. ***
    //
    // The blend test was the last thing keeping this broken, and the owner's
    // test named it: with 3D strength at 0 AND weapon field of view at 1.00 the
    // scope is clean, but move the FIELD OF VIEW - with the strength still zero -
    // and it flickers. That rules out the eye shift, the per-eye sign and stereo
    // entirely. What is left is ANY transform at all.
    //
    // A transform can only flicker if some copies of the mesh get it and others
    // do not: at identity every copy lands in the same place and nothing shows,
    // which is exactly why 1.00 looked fine. The engine draws this mesh several
    // times a frame - the log has been saying so all along as 'min 4, max 5' -
    // and the copies that are NOT blended (a depth-only or stencil-only pass)
    // failed this test, stayed put, and fought the ones that moved.
    if (!h.glass && !h.stencil && !h.mask && Cfg().weapon_3d &&
        Cfg().weapon_3d_glass_mask_mesh && Cfg().weapon_3d_glass > 0 &&
        g_drawsSinceTagged <= (LONG)Cfg().weapon_3d_glass &&
        IsMaskMeshIndexCount(meshIdx) &&
        (!Cfg().weapon_stencil_only || DrawIsBlended(ctx))) {
        h.glass = true;
        InterlockedIncrement(&g_glassByMaskMesh);
    }

    // NOT the mask. It is already handled above by the stencil rule, and letting
    // it match here as well is how one object ended up with two mechanisms.
    if (!h.glass && !h.stencil && !h.mask && Cfg().weapon_3d &&
        Cfg().weapon_3d_glass > 0 &&
        g_drawsSinceTagged <= (LONG)Cfg().weapon_3d_glass) {
        if (Cfg().weapon_3d_glass_whole_pass) {
            h.glass = DrawGoesToMainScene(ctx);
        } else if (g_glassResKnown > 0 &&
                   (!Cfg().weapon_3d_glass_blended_only || DrawIsBlended(ctx))) {
            ID3D11Buffer* cb = nullptr;
            ctx->VSGetConstantBuffers(
                (UINT)(Cfg().weapon_3d_slot < 0 ? 1 : Cfg().weapon_3d_slot), 1, &cb);
            if (cb) {
                h.glass = IsKnownGlassResource(cb);
                // Counted: a blended draw sitting right on the gun whose buffer
                // we had never recorded is a lens that does not move this frame.
                // That is the flicker, made countable instead of watched for.
                if (!h.glass) InterlockedIncrement(&g_glassBufUnknown);
                // A confirmed lens names its mesh, so its unblended copies can
                // be caught as well.
                if (h.glass) NoteGlassMeshIndexCount(meshIdx);
                cb->Release();
            }

        }

    }

    // *** BINOCULARS COUNT AS AN OPTIC TOO. ***
    //
    // 'Scoped' is detected from the mask stamp, and only scopes stamp one - so
    // binoculars never triggered it and never got the scoped strength. A lens
    // draw is the general signal: glass appears on the viewmodel only while an
    // optic is raised.
    //
    // This is the same avoidance the scope now uses, not a cure: the lens still
    // cannot be moved through its constants, so the honest option is to move
    // nothing while an optic is up rather than move the lens the one way that
    // makes it flicker.
    // Describe the buffer a lens draw reads, once. This is the fact every
    // hypothesis about the constants path has been missing.
    if (h.glass && !g_lensCBSeen) {
        ID3D11Buffer* cb = nullptr;
        ctx->VSGetConstantBuffers(
            (UINT)(Cfg().weapon_3d_slot < 0 ? 1 : Cfg().weapon_3d_slot), 1, &cb);
        if (cb) {
            D3D11_BUFFER_DESC bd{};
            cb->GetDesc(&bd);
            InterlockedExchange(&g_lensCBWidth, (LONG)bd.ByteWidth);
            InterlockedExchange(&g_lensCBUsage, (LONG)bd.Usage);
            InterlockedExchange(&g_lensCBCpuFlags, (LONG)bd.CPUAccessFlags);
            InterlockedExchange(&g_lensCBBind, (LONG)bd.BindFlags);
            InterlockedExchange(&g_lensCBHaveSnap, FindSnapshot(cb) ? 1 : 0);
            InterlockedExchange(&g_lensCBSeen, 1);
            cb->Release();
        }
    }

    // Anything drawn right on the weapon that nothing claimed. The second glass
    // copy is in here, and naming it is the whole point.
    if (meshIdx && !h.glass && !h.mask && !h.stencil &&
        g_drawsSinceTagged <= (LONG)Cfg().weapon_3d_glass) {
        NoteUnclaimedDraw(meshIdx);
    }

    // Coverage of the meshes we know to be lenses, measured from BOTH sides.
    if (meshIdx && (IsGlassMeshIndexCount(meshIdx) || IsMaskMeshIndexCount(meshIdx))) {
        InterlockedIncrement(&g_lensMeshSeen);
        if (!h.glass && !h.mask && !h.stencil) {
            InterlockedIncrement(&g_lensMeshMissed);
            const LONG g = g_drawsSinceTagged;
            if (g < g_lensMissedGapMin) InterlockedExchange(&g_lensMissedGapMin, g);
            if (g > g_lensMissedGapMax) InterlockedExchange(&g_lensMissedGapMax, g);
        }
    }

    // A draw showing a copy of the scene is the magnified picture. Leave it where
    // the game put it - moving it slides the picture against the world it is a
    // picture OF, which is the flicker the owner traced to the blue square.
    if ((h.glass || h.mask) && Cfg().weapon_3d_skip_scene_picture &&
        DrawSamplesWholeScene(ctx)) {
        h.glass = false;
        h.mask = false;
        InterlockedIncrement(&g_skippedPicture);
    }

    h.weapon = h.stencil || h.glass;
    return h;
}

// The shift itself, identical for every draw type. The body moves through its
// constants; the glass moves at the rasterizer - same displacement, no constant
// buffer involved.
template <typename Issue>
void IssueShiftedViewmodel(ID3D11DeviceContext* ctx, const ViewmodelHit& h,
                           UINT meshIdx, Issue&& issue) {
    // *** GLASS THROUGH THE SAME ROUTE AS THE BODY. ***
    //
    // Everything that flickers is a lens: both binocular lenses, and a part of
    // the scope. Lens parts are also the only things shifted at the RASTERIZER
    // while the body is shifted through its CONSTANTS. One mechanism flickers and
    // the other does not, and the true-zero test proved it is the shift doing it.
    //
    // The two routes are not equivalent for a lens, and the capture says why: the
    // lens's pixel shader SAMPLES a full-size copy of the scene and magnifies it
    // in screen space. Moving the viewport moves the geometry and its
    // screen-space sample position together, so the picture inside the circle is
    // taken from somewhere other than where the circle now is. Moving the matrix
    // moves only the geometry, which is what the body gets and what a lens needs.
    //
    // The clip matrix a lens reads is bit-identical to its body's - stated at the
    // top of this file and relied on by GlassOffsetIn - so the constants path can
    // find and shift it exactly as it does for the gun. The rasterizer stays as
    // the fallback for a lens whose constants we cannot reach.
    // *** THE MASK TAKES ONE ROUTE, ALWAYS. ***
    //
    // It used to try the constants first and fall back to the rasterizer when
    // the snapshot did not match the buffer the draw reads - and that fallback
    // fires intermittently (13 skips a window). So the SAME disc moved by one
    // mechanism on some frames and the other on the rest. A circle alternating
    // between two positions IS the flicker, and because the scope's glass is
    // clipped against that circle, the glass flickers with it. The extra-draw
    // diagnostic named it outright: 1584 indices at gap 1 and 2 - the mask and
    // its sibling - arriving through the glass path as well as the stencil one.
    //
    // The rasterizer is the right single choice for it. The mask paints nothing:
    // no pixel shader, no render target, depth off - so nothing about it needs
    // its constants touched, and unlike the constants path the rasterizer never
    // declines. One object, one mechanism, every frame.
    // *** THE MASK GOES THROUGH THE CONSTANTS AGAIN. ***
    //
    // It was pinned to the rasterizer when the constants path could not hold its
    // buffer, to at least be consistent. That reason is gone: the 128-byte lens
    // and mask buffers are snapshotted now.
    //
    // And pinning it is actively wrong once the weapon can be REPOSITIONED: the
    // rasterizer route applies only the per-eye shift, knowing nothing about the
    // position offsets or the weapon field of view. So the scope moved and its
    // stencil circle stayed - the crescent eating the shroud, back again by a
    // different door.
    SavedCB saved;
    bool cbShifted = false;
    // Lens and mask do NOT go through the constants. Their shader samples the
    // scene in screen space, so the rasterizer is the only route that moves the
    // picture with the glass; the constants route moves the glass and leaves the
    // picture, which alternates per eye and reads as flicker. The rasterizer now
    // carries the position offsets and the weapon field of view as well, which is
    // the only thing it ever lacked.
    // *** THE MASK FOLLOWS THE GLASS, BECAUSE THEY ARE THE SAME DISC. ***
    //
    // Measured: the mask draw and the glass draw share index and vertex buffers
    // and their post-VS clip positions are bit-identical to six decimals
    // (15960 = 16691 at z/w 0.036561-0.036745; 15963 = 16700 at 0.219812-0.223026).
    // One object, issued twice.
    //
    // This line sent those two copies down DIFFERENT transforms - the glass
    // through the constants, the mask through the rasterizer - and the two are
    // asymmetric in the worst way: the rasterizer never declines, while the
    // constants can. On a frame where the constants fail, the glass does not move
    // at all and the mask still does. Same disc, two positions, alternating.
    //
    // Binoculars have NO stamper, so the split cannot happen there - which is
    // exactly why they came out fixed and scopes did not.
    const bool atRasterizer =
        (h.glass || (h.mask && !Cfg().weapon_3d_mask_follows_glass)) &&
        Cfg().weapon_3d_glass_via_rasterizer;
    if (Cfg().weapon_3d && !atRasterizer &&
        (!h.glass || Cfg().weapon_3d_glass_via_constants)) {
        cbShifted = BindShiftedWeaponCB(ctx, &saved, meshIdx, h.stencil);
        if (h.glass && cbShifted) InterlockedIncrement(&g_glassViaConstants);
    }

    // The mask stamps a screen region, so if its constants carry no matrix we
    // recognise, the rasterizer moves it just as exactly - and unlike the gun
    // body, there is no lighting or depth to get wrong. Without this fallback a
    // mask whose matrix we miss would silently not move, which is the failure
    // that has been mistaken for "the rule did not match" all day.
    //
    // *** GATED ON weapon_3d, and leaving that out inverted the very bug this
    // was written to fix. *** With 3D off nothing else moves, but the mask does
    // not reach BindShiftedWeaponCB at all (it is skipped when weapon_3d is 0),
    // so cbShifted stayed false, the fallback fired, and the stamp travelled
    // while the gun stood still - the same crescent, mirrored, in the one mode
    // that is supposed to touch nothing. An "off" switch that still moves
    // something is not off.
    // *** NOTHING IDENTIFIED AS THE VIEWMODEL MAY SILENTLY NOT MOVE. ***
    //
    // The constant-buffer shift fails occasionally - "our snapshot is of a
    // different buffer than this draw reads", 11 times per 180 frames in the
    // log, split across 77484 (6) and 26178 (5). Each failure leaves that piece
    // at its unshifted position for one frame while everything around it moves.
    // Occasional, brief, affecting individual pieces: that is a flicker, and it
    // was sitting in the diagnostics being read as a rounding error.
    //
    // The rasterizer reaches what the constants could not, and reaches it
    // identically: a clip-space shift of k is a constant NDC offset, and
    // k * width/2 pixels is that same offset.
    const bool needFallback =
        Cfg().weapon_3d && !cbShifted && h.mask &&
        !Cfg().weapon_3d_optic_signature;
    // *** ONE ROUTE FOR THE MASK, NOW THAT THE BINOCULARS ARE PROOF. ***
    //
    // The binoculars are fixed and the scope is not, and the ONLY thing that
    // separates them is the mask disc: the measurement found the optic drawing in
    // frames with no 0x40 stamper at all, and binocular frames are exactly those.
    //
    // The disc still had two possible mechanisms - the constants, with a
    // rasterizer fallback whenever the constants could not be reached. Same
    // object, two transforms, chosen by whether an unrelated lookup happened to
    // succeed that frame. Every artefact in this project has come from that shape:
    // the crescent, the detached lenses, the lens that moved by 2k in one eye.
    //
    // With the exact signature on, the mask takes the constants or it is left
    // alone - never a third position. If it is occasionally left alone, that is a
    // one-frame miss we can measure; two rival positions is a flicker we cannot.
    // *** TRIED AND REVERTED: falling back for EVERY tagged draw. ***
    //
    // The reasoning looked sound - ~20 draws a window cannot reach their
    // constants, each one a frame of that piece unmoved, and the stencil tag is
    // exact (57/57 viewmodel, 0/4319 world), so moving what it claims cannot move
    // scenery. In practice it flickered the whole weapon, the hands and the scope
    // at once - far more than 20 draws' worth.
    //
    // Which means cbShifted is false FAR more often than the skip counter reports:
    // BindShiftedWeaponCB has several other silent false returns, and every one of
    // them started taking the rasterizer instead. The counter measures one exit
    // path, not the failure rate, and I read it as the failure rate.
    //
    // Only the mask falls back, as before.
    if (h.mask && !cbShifted && Cfg().weapon_3d_mask_follows_glass) {
        InterlockedIncrement(&g_maskNoConstants);
    }
    GlassViewportShift gvp;
    // Only when the constants could not carry it. Doing both would move the lens
    // twice.
    const bool maskToRaster = (h.mask && !cbShifted) && !Cfg().weapon_3d_optic_signature;
    if (atRasterizer || maskToRaster || (h.glass && !cbShifted) || needFallback) {
        if (h.glass) InterlockedIncrement(&g_glassViaViewport);
        gvp.Begin(ctx);
    }

    WeaponViewportShift vp;
    vp.Begin(ctx);
    issue();
    vp.End();

    gvp.End();
    RestoreWeaponCB(ctx, &saved);
}

// =========================================================================
// *** THE HUD COMPONENT PROBE. ***
//
// The HUD sits at the EDGES of the screen, which is the worst place for it in a
// headset. Moving each component inward needs each component IDENTIFIED, and
// shader identity cannot do that here: the owner has confirmed several HUD
// components share one shader, and his hunted list of HUD shaders is partial.
//
// So the handle has to be WHERE THE QUAD LANDS ON SCREEN, and this prints
// exactly that. It answers one question - "what does the frame's HUD look like,
// measured" - and it answers it in numbers that can be quoted back at a
// hypothesis later. Nothing here is bucketed, named or interpreted: rects are
// printed at %.2f because a report that says "top-centre" cannot be used to test
// a claim about a boundary, and a %.2f readout on a coarse control has already
// voided four eliminations in this project once.
//
// FIVE PROPERTIES IT WAS BUILT TO HAVE:
//
//  1. READ-ONLY. Get* calls only; every object queried is released on every exit
//     path; no state is set, saved or restored; no draw is changed, skipped or
//     reissued. Two diagnostic builds in this project were themselves broken and
//     reported working code as broken, so this one cannot touch the picture.
//  2. FREE WHEN OFF. hud_probe = 0 costs one global read and one branch per
//     draw. Every driver call below sits behind that read.
//  3. IT STOPS BY ITSELF. hud_probe_frames Presents and it disarms. A probe that
//     logs forever fills the disk and tanks the framerate, and this one logs
//     several hundred lines per frame.
//  4. IT ACCOUNTS FOR EVERY DRAW IT SAW. accepted + each named rejection = the
//     draws it classified, printed and reconciled. BindShiftedWeaponCB had seven
//     failure exits and its log counted one, which produced three consecutive
//     wrong theories; a probe with one "rejected" counter would do it again.
//  5. IT REPORTS ITS OWN LIMITS. Row capacity is printed, saturation is printed
//     in words, and the two INDIRECT draw entry points - which this file counts
//     but never classifies - are printed as a known hole. A number equal to a
//     capacity is a limit until proven otherwise; that has been read as a
//     discovery here before.
//
// AND THE MEASUREMENT IT IS MEANT FOR: run it twice, once with the game's own
// HUD on and once with it off (settings.json GameDisplayHUD), and diff the two
// logs. Everything that disappears is HUD; everything that survives is not.
// That is a differential with no guessed fingerprint in it anywhere, and it also
// settles - for every cluster at once - whether a shader that draws HUD also
// draws world.
// =========================================================================

// The owner's hunted list, read out of ReShade's Shader Toggler. ReShade
// identifies a shader by a CRC32 of its bytecode and so does this file (see
// Crc32 above), so these numbers are DIRECTLY comparable - no translation, no
// "if a comparable hash can be computed". They are used only as a net test: if a
// known HUD shader shows up only among REJECTED draws, the coarse filter is
// wrong and every cluster below it is meaningless until it is fixed. They are
// never used to classify anything, because the list is partial by the owner's
// own account and a partial list used as a rule is a rule with holes in it.
constexpr uint32_t kKnownHudVS[] = { 1787392644u, 461146230u, 1145290386u, 2910689842u };
constexpr uint32_t kKnownHudPS[] = { 422461458u, 2727569164u, 2611165027u,
                                     893034202u, 304038693u, 198363578u };
constexpr int kKnownHudVSCount = (int)(sizeof(kKnownHudVS) / sizeof(kKnownHudVS[0]));
constexpr int kKnownHudPSCount = (int)(sizeof(kKnownHudPS) / sizeof(kKnownHudPS[0]));

int KnownHudVSIndex(uint32_t crc) {
    if (!crc) return -1;
    for (int i = 0; i < kKnownHudVSCount; ++i) if (kKnownHudVS[i] == crc) return i;
    return -1;
}
int KnownHudPSIndex(uint32_t crc) {
    if (!crc) return -1;
    for (int i = 0; i < kKnownHudPSCount; ++i) if (kKnownHudPS[i] == crc) return i;
    return -1;
}

// Why a draw did not make it into the table. Named, not counted as one lump.
enum HudReason {
    kHudAccepted = 0,
    kHudViewmodel,      // already claimed by the viewmodel / optic-glass rules
    kHudDepthOn,        // the depth test is live, so this is world geometry
    kHudNoBlend,        // opaque, so it is not a composited UI quad
    kHudNoViewport,     // no rasterizer viewport - nothing to describe
    kHudTableFull,      // passed every test, but the row table was saturated
    kHudReasonCount
};
const char* const kHudReasonName[kHudReasonCount] = {
    "accepted", "viewmodel", "depth-on", "no-blend", "no-viewport", "TABLE-FULL"
};

// A texture, DESCRIBED. The pointer alone is useless - three theories about the
// lens buffer died to one line that printed its descriptor - so a 2048x2048
// atlas, a 64x64 icon and a 1x1 white are told apart here rather than guessed at
// later. The pointers are identity tokens only and are never dereferenced: they
// are recorded after the object has been released, which is safe for comparison
// and would not be safe for anything else.
struct HudTexInfo {
    void* view;
    void* res;
    UINT  w, h, mips;
    UINT  resFmt;    // the RESOURCE's format
    UINT  viewFmt;   // the VIEW's format, which may be a typed member of the same family
};

// A constant buffer, described AND sampled. ByteWidth is printed next to the
// bytes deliberately: a size filter written for a different block silently
// dropped a 128-byte buffer for an entire investigation here, and a line that
// shows the size beside the contents makes that visible in one line instead of
// two days.
struct HudCbInfo {
    void* res;
    UINT  bytes;
    UINT  usage;
    UINT  cpu;
    UINT  bind;
    UINT  first;     // in 16-byte constants, from the RANGE-AWARE bind
    int   snap;      // 1 = contents below are real, 0 = no snapshot, -1 = block past its end
    float f[16];
};

struct HudRow {
    volatile LONG valid;      // published LAST, so a racing reader never sees a half row
    LONG  ordinal;            // position of this draw within the frame
    int   eye;
    void* ctx;
    int   deferred;
    int   path;               // which entry point issued it
    UINT  vtx, idx, inst, topo;
    UINT  vpCount;
    D3D11_VIEWPORT vp;
    UINT  scCount;
    D3D11_RECT sc;
    // The screen rectangle this draw is CONFINED to: the viewport, cut down by
    // the scissor when there is one. If the UI system sets a scissor per
    // element, this rect IS the component and the hunt is over on line one.
    float rl, rt, rr, rb;
    void* vsObj; void* psObj;
    uint32_t vsCrc, psCrc;
    int   knownVS, knownPS;
    // Depth-stencil and blend, AS MEASURED. The filter above is coarse on
    // purpose; the real fingerprint is meant to be read back off these columns
    // afterwards rather than guessed at in advance.
    int   dEnable, sEnable;
    UINT  dWrite, dFunc, sRef, sRead, sWrite;
    int   bEnable;
    UINT  bSrc, bDst, bOp, bMask;
    int   hasDsv;
    HudTexInfo rt2;
    HudTexInfo srv[2];
    HudCbInfo  vscb, pscb;
};

// Per frame, not per run: the table is printed and emptied at every Present, so
// this is "how many HUD-shaped draws can one frame hold".
constexpr int kHudRows = 512;
// A probe that logs forever fills the disk. Even asked for more, it stops here.
constexpr int kHudMaxFrames = 30;

HudRow g_hudRow[kHudRows] = {};
volatile LONG g_hudRows = 0;          // claimed slots; may run past kHudRows
volatile LONG g_hudSeen = 0;          // every draw this frame, all six entry points
volatile LONG g_hudIndirect = 0;      // of which indirect - counted, never classified
volatile LONG g_hudFunnel[kHudReasonCount] = {};
volatile LONG g_hudEyeAccepted[2] = {};
// The net test, accumulated across the whole run: for each hunted hash, at how
// many draws was it bound, split by what the filter decided about that draw.
volatile LONG g_hudKnownVS[kKnownHudVSCount][kHudReasonCount] = {};
volatile LONG g_hudKnownPS[kKnownHudPSCount][kHudReasonCount] = {};

// THE HOT-PATH GATE. One volatile read per draw when the probe is off.
volatile LONG g_hudCapture = 0;
LONG g_hudFrameNo = 0;        // touched only from the Present thread
LONG g_hudFramesLeft = 0;

const char* HudPathName(int path) {
    switch (path) {
        case 0: return "DrawIndexed";
        case 1: return "DrawIndexedInstanced";
        case 2: return "Draw";
        case 3: return "DrawInstanced";
        default: return "?";
    }
}

void HudDescribeRes(ID3D11Resource* res, HudTexInfo* out) {
    if (!res || !out) return;
    out->res = res;
    ID3D11Texture2D* tex = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex)) && tex) {
        D3D11_TEXTURE2D_DESC td{};
        tex->GetDesc(&td);
        out->w = td.Width;
        out->h = td.Height;
        out->mips = td.MipLevels;
        out->resFmt = (UINT)td.Format;
        tex->Release();
    }
}

// The constant buffer this draw ACTUALLY reads.
//
// Through the RANGE-AWARE bind, always. The engine binds the whole pool and
// sub-allocates, so the block belonging to this draw starts at first*16 bytes
// inside it; ignoring the range once made ~30,000 particle draws look like the
// viewmodel. If the offset lands past the head of the pool we kept, that is
// reported as -1 rather than as zeros, because "we did not see it" and "it is
// zero" are different answers.
void HudReadCb(ID3D11DeviceContext* ctx, ID3D11DeviceContext1* c1, bool pixelStage,
               HudCbInfo* out) {
    ID3D11Buffer* cb = nullptr;
    UINT first = 0, count = 0;
    if (c1) {
        if (pixelStage) c1->PSGetConstantBuffers1(0, 1, &cb, &first, &count);
        else            c1->VSGetConstantBuffers1(0, 1, &cb, &first, &count);
    } else {
        if (pixelStage) ctx->PSGetConstantBuffers(0, 1, &cb);
        else            ctx->VSGetConstantBuffers(0, 1, &cb);
    }
    if (!cb) return;
    D3D11_BUFFER_DESC bd{};
    cb->GetDesc(&bd);
    out->res   = cb;
    out->bytes = bd.ByteWidth;
    out->usage = (UINT)bd.Usage;
    out->cpu   = bd.CPUAccessFlags;
    out->bind  = bd.BindFlags;
    out->first = first;
    const UINT off = first * 16u;
    if (CBSnapshot* s = FindSnapshot(cb)) {
        const UINT have = s->bytes;
        if (have > off && (have - off) >= sizeof(out->f)) {
            memcpy(out->f, s->data + off, sizeof(out->f));
            out->snap = 1;
        } else {
            out->snap = -1;
        }
    }
    cb->Release();
}

// Called from every real draw entry point. vh is what the mod's own classifier
// already decided about this draw, so the probe can exclude what is already
// claimed instead of re-deriving it and disagreeing.
// *** WHERE EACH HUD DRAW ACTUALLY LANDS ON SCREEN. ***
//
// The sweep proved the texture is not the handle. Four elements have their own
// (phone map, phone icons, compass ring, wind arrow) and everything after index
// 9 is a shared fill or atlas serving both the compass and the bottom-right
// cluster at once. A texture that draws in two places cannot tell them apart.
//
// Position can. Each of those is still a separate DRAW with its own quad, so
// reading where the quad lands separates them by construction - and position is
// also the thing we want to change, so the discriminator and the lever are the
// same quantity.
//
// This reads the VERTEX BUFFER, because the viewport turned out to be the render
// target (every rect at 0,0) and the constant snapshot did not obviously carry a
// screen rectangle (82 signatures, leading floats that look like camera data).
// The vertices are where a quad's corners are by definition.
//
// A staging copy is needed because a vertex buffer the game draws from is not
// CPU-readable. That is expensive, so this runs ONLY while the probe is armed
// and stops after a fixed number of lines.
ID3D11Buffer* g_quadStage = nullptr;
UINT g_quadStageSize = 0;
LONG g_quadLines = 0;
constexpr LONG kQuadMaxLines = 400;

void HudQuadReset() { InterlockedExchange(&g_quadLines, 0); }

void HudProbeQuad(ID3D11DeviceContext* ctx, const ViewmodelHit& vh, UINT idxCount) {
    // GATED ON THE CAPTURE WINDOW, NOT ON THE SWITCH.
    //
    // It was gated on Cfg().hud_probe, and that switch was still 1 in the ini
    // from an earlier session - so it armed at frame one and spent its whole
    // 400-line budget on the main menu, on fullscreen post-process triangles in
    // clip space, (-1,1)(3,1)(-1,-3). The probe's own capture flag is raised
    // when the player arms it from the panel, in gameplay, which is the only
    // moment worth sampling.
    if (!g_hudCapture || !ctx) return;
    if (vh.weapon || vh.stencil || vh.glass || vh.mask) return;
    if (InterlockedCompareExchange(&g_quadLines, 0, 0) >= kQuadMaxLines) return;

    ID3D11Buffer* vb = nullptr;
    UINT stride = 0, offset = 0;
    ctx->IAGetVertexBuffers(0, 1, &vb, &stride, &offset);
    if (!vb) return;
    if (stride < 8 || stride > 256) { vb->Release(); return; }

    D3D11_BUFFER_DESC bd{};
    vb->GetDesc(&bd);
    // Only the first few vertices are needed - a quad has four.
    const UINT want = stride * 6;
    const UINT copyBytes = bd.ByteWidth < want ? bd.ByteWidth : want;
    if (copyBytes < stride) { vb->Release(); return; }

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (!dev) { vb->Release(); return; }

    if (!g_quadStage || g_quadStageSize < bd.ByteWidth) {
        if (g_quadStage) { g_quadStage->Release(); g_quadStage = nullptr; }
        D3D11_BUFFER_DESC sd{};
        sd.ByteWidth = bd.ByteWidth;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev->CreateBuffer(&sd, nullptr, &g_quadStage))) {
            g_quadStage = nullptr;
            dev->Release(); vb->Release(); return;
        }
        g_quadStageSize = bd.ByteWidth;
    }
    dev->Release();

    ctx->CopyResource(g_quadStage, vb);
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (SUCCEEDED(ctx->Map(g_quadStage, 0, D3D11_MAP_READ, 0, &ms)) && ms.pData) {
        const char* base = static_cast<const char*>(ms.pData) + offset;
        const UINT verts = copyBytes / stride;
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        char buf[256];
        int n = 0;
        for (UINT v = 0; v < verts && v < 4; ++v) {
            const float* f = reinterpret_cast<const float*>(base + size_t(v) * stride);
            if (f[0] < x0) x0 = f[0];
            if (f[0] > x1) x1 = f[0];
            if (f[1] < y0) y0 = f[1];
            if (f[1] > y1) y1 = f[1];
            n += _snprintf_s(buf + n, sizeof(buf) - n, _TRUNCATE,
                             "(%.1f,%.1f)", f[0], f[1]);
        }
        ctx->Unmap(g_quadStage, 0);
        InterlockedIncrement(&g_quadLines);
        COTW_LOG("[hud-xy] stride%u idx%u verts %s  bbox %.1f,%.1f -> %.1f,%.1f",
                 stride, idxCount, buf, x0, y0, x1, y1);
    }
    vb->Release();
}

// Blended, depth-less, and not something the mod already claims. The same two
// facts the probe's funnel uses, in one place so the mover and the isolator
// cannot drift apart.
bool HudLooksLikeHud(ID3D11DeviceContext* ctx, const ViewmodelHit& vh) {
    if (!ctx) return false;
    if (vh.weapon || vh.stencil || vh.glass || vh.mask) return false;

    ID3D11DepthStencilState* dss = nullptr;
    UINT sref = 0;
    ctx->OMGetDepthStencilState(&dss, &sref);
    bool depthOff = false;
    if (dss) {
        D3D11_DEPTH_STENCIL_DESC dd{};
        dss->GetDesc(&dd);
        depthOff = !dd.DepthEnable ||
                   (dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO &&
                    dd.DepthFunc == D3D11_COMPARISON_ALWAYS);
        dss->Release();
    }
    if (!depthOff) return false;

    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4]; UINT mask = 0;
    ctx->OMGetBlendState(&bs, bf, &mask);
    bool blended = false;
    if (bs) {
        D3D11_BLEND_DESC bd{};
        bs->GetDesc(&bd);
        blended = bd.RenderTarget[0].BlendEnable != FALSE;
        bs->Release();
    }
    return blended;
}

// *** THE GLARE / DIRTY-LENS OVERLAY. ***
//
// Found by sweep: hiding one texture removed a haze the owner had been looking
// through the whole time and had stopped noticing. The log named it, and its
// neighbours gave it away as a BLOOM PYRAMID:
//
//     index 25   180x194   R11G11B10_FLOAT
//     index 26   360x389   R11G11B10_FLOAT
//     index 27   720x778   R11G11B10_FLOAT   <- the one that was hidden
//
// Each level double the last, and 720x778 is exactly a quarter of the 2880x3112
// render. A bloom chain composited back over the scene.
//
// Matched by FORMAT, not by index, because the index is only stable within one
// run and the sizes move with the render preset. Real HUD art is 8-bit -
// measured, fmt28 / fmt77 / fmt87 - so a floating-point HDR texture bound to a
// blended, depth-less draw is the glare and nothing else. That rule holds at any
// resolution.
bool HudIsGlareDraw(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    srv->Release();
    if (!res) return false;
    bool glare = false;
    ID3D11Texture2D* t2 = nullptr;
    if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2)) && t2) {
        D3D11_TEXTURE2D_DESC td{};
        t2->GetDesc(&td);
        glare = td.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
                td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        t2->Release();
    }
    res->Release();
    return glare;
}

// *** THE COLOURED SUN FLARE. ***
//
// Found by sweep, and it is NOT a lens-dirt texture - it is one rung of the
// bloom pyramid. The log laid the whole chain out:
//
//   down  2880x3112 -> 1440x1556 -> 720x778 -> 360x389 -> 180x194
//   up      360x389 ->  720x778  -> 2880x3112
//
// The step the owner hid is the FIRST UPSAMPLE, and hiding it takes the
// coloured fringing off the sun while leaving bloom everywhere else intact -
// verified by looking at bright sky and lit surfaces with it both ways.
//
// It cannot be matched by description: the downsample rung at the same size is
// byte-identical - 360x389, fmt26, one mip, both of them. Nor by format: 21 of
// the registered textures are fmt26, including the full-size scene buffers.
//
// What separates them is DIRECTION. An upsample draws into something LARGER
// than its source; a downsample into something smaller. Combined with the
// source being about an eighth of the render, that picks this rung and no
// other, at any resolution and in any run.
UINT g_hdrFullW = 0;   // widest HDR float texture seen = the render width

bool HudIsSunFlareDraw(ID3D11DeviceContext* ctx) {
    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    ID3D11Resource* sres = nullptr;
    srv->GetResource(&sres);
    srv->Release();
    if (!sres) return false;

    UINT srcW = 0;
    bool hdr = false;
    ID3D11Texture2D* st = nullptr;
    if (SUCCEEDED(sres->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&st)) && st) {
        D3D11_TEXTURE2D_DESC td{};
        st->GetDesc(&td);
        hdr = td.Format == DXGI_FORMAT_R11G11B10_FLOAT ||
              td.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;
        srcW = td.Width;
        if (hdr && td.Width > g_hdrFullW) g_hdrFullW = td.Width;
        st->Release();
    }
    sres->Release();
    if (!hdr || !srcW || !g_hdrFullW) return false;

    ID3D11RenderTargetView* rtv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return false;
    ID3D11Resource* rres = nullptr;
    rtv->GetResource(&rres);
    rtv->Release();
    if (!rres) return false;
    UINT rtW = 0;
    ID3D11Texture2D* rt = nullptr;
    if (SUCCEEDED(rres->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&rt)) && rt) {
        D3D11_TEXTURE2D_DESC td{};
        rt->GetDesc(&td);
        rtW = td.Width;
        rt->Release();
    }
    rres->Release();
    if (!rtW) return false;

    // *** NO DIRECTION TEST. ***
    //
    // It had one - upsample only - and the toggle did nothing in the headset.
    // The sweep that DID work hid every draw sourcing that texture, and the
    // pyramid reads it twice: once going down, once coming back up. Matching
    // only the upsample half left the effective one running.
    //
    // So match what the sweep matched: any draw whose source is the HDR float
    // texture at an eighth of the render. rtW is still read, because a draw
    // with no render target is not a pass at all.
    (void)rtW;
    const float frac = float(srcW) * 8.0f / float(g_hdrFullW);
    return frac > 0.85f && frac < 1.15f;
}

// *** MOVING THE HUD IN FROM THE EDGES. ***
//
// The search for a per-element handle ended honestly: the shader is too coarse
// (one carries 216 of 408 draws), the texture works for four elements and then
// hits shared fills and atlases, the viewport is the render TARGET (every rect
// at 0,0), there are NO scissor rects on any of the 508 accepted draws, and the
// vertex buffers cannot be read back - deferred contexts, and position is not at
// offset 0 anyway, with strides of 8, 20 and 28 all appearing.
//
// None of that is needed, because the owner's HUD is all in ONE corner. Every
// composite draw goes through a full-screen viewport, and a viewport is the map
// from clip space to pixels: move it and everything drawn through it moves,
// without knowing where any individual piece is.
//
// TWO CONTROLS, BECAUSE THEY ARE DIFFERENT OPERATIONS:
//   hud_move_x / hud_move_y  shift it. Moves without resizing.
//   hud_scale                shrink it about its centre. Pulls everything toward
//                            the middle from wherever it is, and makes it
//                            smaller by the same factor - the two cannot be
//                            separated, which is exactly why the offset exists.
//
// The WORLD is untouched. It is drawn earlier, through its own viewport, with
// depth on, and never passes the filter above.
//
// RAII, so every early return in the draw hooks restores the viewport. The lens
// shift at the top of this file learned that one the hard way.
struct HudMove {
    ID3D11DeviceContext* ctx = nullptr;
    UINT count = 0;
    D3D11_VIEWPORT saved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    bool active = false;

    HudMove(ID3D11DeviceContext* c, const ViewmodelHit& vh) {
        const float sc = Cfg().hud_scale;
        const float ox = (float)Cfg().hud_move_x;
        const float oy = (float)Cfg().hud_move_y;
        if (!c) return;
        if (sc == 1.0f && ox == 0.0f && oy == 0.0f) return;
        if (!HudLooksLikeHud(c, vh)) return;

        // *** FULL-SCREEN EFFECTS ARE NOT HUD, AND MUST NOT BE MOVED. ***
        //
        // The bloom / glare composite is blended and depth-less, so it passes the
        // HUD filter - and it covers the whole screen. Scaling it to 0.7 turns a
        // full-screen haze into a rectangle floating in the middle of the view,
        // which is exactly what the owner reported: "it will look like a small
        // window in my view".
        //
        // Recognised the same way the glare toggle recognises it, by its HDR
        // float source, so the two can never disagree about what the glare is.
        // This exclusion applies whether or not the player is hiding the glare -
        // it is about not MOVING a thing that has no position to move.
        if (HudIsGlareDraw(c)) return;

        count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        c->RSGetViewports(&count, saved);
        if (!count) return;

        D3D11_VIEWPORT moved[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
        for (UINT i = 0; i < count; ++i) {
            moved[i] = saved[i];
            if (sc != 1.0f && sc > 0.05f) {
                moved[i].Width  = saved[i].Width * sc;
                moved[i].Height = saved[i].Height * sc;
                moved[i].TopLeftX = saved[i].TopLeftX +
                                    (saved[i].Width - moved[i].Width) * 0.5f;
                moved[i].TopLeftY = saved[i].TopLeftY +
                                    (saved[i].Height - moved[i].Height) * 0.5f;
            }
            moved[i].TopLeftX += ox;
            moved[i].TopLeftY += oy;
        }
        c->RSSetViewports(count, moved);
        ctx = c;
        active = true;
    }
    ~HudMove() { if (active && ctx && count) ctx->RSSetViewports(count, saved); }
};

// *** NAMING THE HUD, ONE PIECE AT A TIME. ***
//
// The probe answered which handle separates HUD components: not the shader - six
// of the owner's seven hunted pixel shaders land in the accepted set and ONE of
// them carries 216 of the 408 on-screen draws - but the BOUND TEXTURE. There are
// 28 distinct ones, so the texture splits the HUD 28 ways where the shader would
// have merged half of it into a single lever.
//
// What is still missing is the map from a texture to the thing the player SEES.
// No amount of reading finds that; it needs eyes. So: hide one texture group at
// a time and let the owner say what vanished.
//
// The index is a position in the order the textures first appear in a frame,
// filled ONCE and never cleared, because rebuilding it every frame is what made
// indices 1-8 remove things cleanly while 9 upwards only FLICKERED. Per-frame
// rebuilding leaves the numbering only as stable as the order textures happen to
// be bound in, and past the first handful that order varies between frames - so
// one index selected a different texture each frame and a different piece of HUD
// vanished each frame. That reads as flicker, and it was the instrument's fault
// rather than the game's.
//
// The resource pointer is stable for the life of the run, so first-seen order is
// fixed the moment each texture is first seen. It is NOT stable across runs, and
// does not need to be: the whole exercise is one session of cycling through it.
//
// hud_isolate = 0 off, 1..N hides the Nth group, -1 hides everything accepted
// (which is the sanity check: if -1 does not blank the HUD, the filter is wrong
// and every number below it is meaningless).
// 64 was nowhere near enough once the table stopped being rebuilt every frame.
// Measured: the main menu registered 58 textures before gameplay began, and the
// real HUD arrived at index 59 with five slots left - so most of it could never
// be numbered at all. Sized for a whole session now; it only fills while a sweep
// is actually running.
constexpr int kIsoMax = 512;
ID3D11Resource* g_isoSeen[kIsoMax] = {};
int g_isoCount = 0;
LONG g_isoHidden = 0;

// Only the per-frame counter is cleared. The TABLE persists - see above.
void HudIsolateNewFrame() { g_isoHidden = 0; }

// true = swallow this draw. Deliberately its own filter rather than a hook into
// the probe: this runs in ordinary play, with capture off, and must not be able
// to change what the probe measures.
bool HudIsolateSkip(ID3D11DeviceContext* ctx, const ViewmodelHit& vh) {
    const int want = Cfg().hud_isolate;

    // START A SWEEP CLEAN. Going from off to on empties the table, so numbering
    // begins where the player is standing when they start it - not at whatever
    // the main menu happened to draw first. Measured: the menu alone registered
    // 58 textures and the real HUD did not arrive until index 59.
    // Stepping 1 -> 2 -> 3 does NOT clear it, so the numbers hold still during a
    // sweep; only returning to 0 and starting again renumbers.
    static int prevWant = 0;
    if (want != 0 && prevWant == 0) {
        g_isoCount = 0;
        COTW_LOG("[hud-iso] sweep started - texture numbering reset");
    }
    prevWant = want;

    if (want == 0 || !ctx) return false;
    if (vh.weapon || vh.stencil || vh.glass || vh.mask) return false;

    // Blended and depth-less, the same two facts the probe's funnel uses.
    ID3D11DepthStencilState* dss = nullptr;
    UINT sref = 0;
    ctx->OMGetDepthStencilState(&dss, &sref);
    bool depthOff = false;
    if (dss) {
        D3D11_DEPTH_STENCIL_DESC dd{};
        dss->GetDesc(&dd);
        depthOff = !dd.DepthEnable ||
                   (dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO &&
                    dd.DepthFunc == D3D11_COMPARISON_ALWAYS);
        dss->Release();
    }
    if (!depthOff) return false;

    ID3D11BlendState* bs = nullptr;
    FLOAT bf[4]; UINT mask = 0;
    ctx->OMGetBlendState(&bs, bf, &mask);
    bool blended = false;
    if (bs) {
        D3D11_BLEND_DESC bd{};
        bs->GetDesc(&bd);
        blended = bd.RenderTarget[0].BlendEnable != FALSE;
        bs->Release();
    }
    // *** TWO FAMILIES, BECAUSE THE FIRST CANNOT SEE POST-PROCESSING. ***
    //
    // This filter required BLENDING, and the lens-dirt / flare layer is not a
    // blended overlay. Proven the only way that settles it: hud_isolate = -1
    // hides every draw this filter accepts, and the dirt survived it. So it is
    // not in this family at all, and no index within it could ever have found
    // it - the sweep was searching a set that never contained the thing.
    //
    // A full-screen post-process pass WRITES rather than blends: the same
    // depth-less state, the opposite blend state. hud_isolate_post switches the
    // sweep to that family so it can be searched exactly the same way.
    if (blended == (Cfg().hud_isolate_post != 0)) return false;

    if (want < 0) { InterlockedIncrement(&g_isoHidden); return true; }

    ID3D11ShaderResourceView* srv = nullptr;
    ctx->PSGetShaderResources(0, 1, &srv);
    if (!srv) return false;
    ID3D11Resource* res = nullptr;
    srv->GetResource(&res);
    srv->Release();
    if (!res) return false;

    int idx = -1;
    for (int i = 0; i < g_isoCount; ++i) {
        if (g_isoSeen[i] == res) { idx = i; break; }
    }
    if (idx < 0 && g_isoCount < kIsoMax) {
        idx = g_isoCount;
        g_isoSeen[g_isoCount++] = res;
        // Named once, when it is first seen, so the numbers the owner reports
        // can be matched afterwards to real resources rather than to a position
        // in a list that no longer exists.
        ID3D11Texture2D* t2 = nullptr;
        if (SUCCEEDED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t2)) && t2) {
            D3D11_TEXTURE2D_DESC td{};
            t2->GetDesc(&td);
            COTW_LOG("[hud-iso] index %d = %ux%u fmt%d mips%u", idx + 1,
                     td.Width, td.Height, (int)td.Format, td.MipLevels);
            t2->Release();
        }
    }
    res->Release();
    if (idx < 0) return false;

    if (idx + 1 == want) { InterlockedIncrement(&g_isoHidden); return true; }
    return false;
}

void HudProbeDraw(ID3D11DeviceContext* ctx, const ViewmodelHit& vh, int path,
                  UINT vtx, UINT idx, UINT inst) {
    if (!g_hudCapture) return;
    const LONG ordinal = InterlockedIncrement(&g_hudSeen) - 1;

    // The shaders are read FIRST, before any filter can reject the draw, because
    // the net test needs to know where a hunted hash showed up - including at a
    // draw this filter threw away. That is the whole point of having a known-good
    // sample: it validates the net, it is not the net.
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader*  ps = nullptr;
    ctx->VSGetShader(&vs, nullptr, nullptr);
    ctx->PSGetShader(&ps, nullptr, nullptr);
    const uint32_t vsCrc = ShaderCrc(vs);
    const uint32_t psCrc = ShaderCrc(ps);
    if (vs) vs->Release();
    if (ps) ps->Release();
    const int kVS = KnownHudVSIndex(vsCrc);
    const int kPS = KnownHudPSIndex(psCrc);

    // One place that records the verdict, so no exit can forget to.
    struct Verdict {
        int kvs, kps;
        void operator()(HudReason r) const {
            InterlockedIncrement(&g_hudFunnel[r]);
            if (kvs >= 0) InterlockedIncrement(&g_hudKnownVS[kvs][r]);
            if (kps >= 0) InterlockedIncrement(&g_hudKnownPS[kps][r]);
        }
    } verdict{ kVS, kPS };

    if (vh.weapon || vh.stencil || vh.glass || vh.mask) {
        verdict(kHudViewmodel);
        return;
    }

    // *** DEPTH TEST DISABLED - STATED TWO WAYS ON PURPOSE. ***
    //
    // A state object can express "this draw does not care about depth" either by
    // turning the test off or by leaving it on with func ALWAYS and no write. A
    // filter that only knew the first would drop half the HUD and report the
    // half it kept as the whole of it. Both count, and the measured columns are
    // printed on every accepted line so the engine's real fingerprint can be
    // read back off the log instead of assumed here.
    //
    // A context with no depth-stencil state bound is at D3D's default, which has
    // the depth test ENABLED - so that is world, and it is rejected.
    ID3D11DepthStencilState* dss = nullptr;
    UINT sref = 0;
    ctx->OMGetDepthStencilState(&dss, &sref);
    D3D11_DEPTH_STENCIL_DESC dd{};
    bool hadDss = false;
    if (dss) { dss->GetDesc(&dd); dss->Release(); hadDss = true; }
    const bool depthOff = hadDss &&
                          (dd.DepthEnable == FALSE ||
                           (dd.DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ZERO &&
                            dd.DepthFunc == D3D11_COMPARISON_ALWAYS));
    if (!depthOff) { verdict(kHudDepthOn); return; }

    ID3D11BlendState* bs = nullptr;
    FLOAT bfactor[4]{};
    UINT bmask = 0;
    ctx->OMGetBlendState(&bs, bfactor, &bmask);
    D3D11_BLEND_DESC bdsc{};
    bool hadBs = false;
    if (bs) { bs->GetDesc(&bdsc); bs->Release(); hadBs = true; }
    if (!hadBs || !bdsc.RenderTarget[0].BlendEnable) { verdict(kHudNoBlend); return; }

    D3D11_VIEWPORT vps[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT vpCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetViewports(&vpCount, vps);
    if (!vpCount || vps[0].Width <= 0.0f) { verdict(kHudNoViewport); return; }

    const LONG slot = InterlockedIncrement(&g_hudRows) - 1;
    if (slot < 0 || slot >= kHudRows) { verdict(kHudTableFull); return; }
    HudRow& r = g_hudRow[slot];
    r.valid = 0;

    D3D11_RECT scs[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT scCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ctx->RSGetScissorRects(&scCount, scs);

    D3D11_PRIMITIVE_TOPOLOGY topo = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ctx->IAGetPrimitiveTopology(&topo);

    r.ordinal  = ordinal;
    r.eye      = (CurrentRenderEye() == 0) ? 0 : 1;
    r.ctx      = ctx;
    r.deferred = IsDeferredCtx(ctx) ? 1 : 0;
    r.path     = path;
    r.vtx = vtx; r.idx = idx; r.inst = inst; r.topo = (UINT)topo;
    r.vpCount = vpCount;
    r.vp = vps[0];
    r.scCount = scCount;
    r.sc = scCount ? scs[0] : D3D11_RECT{ 0, 0, 0, 0 };
    r.rl = vps[0].TopLeftX;
    r.rt = vps[0].TopLeftY;
    r.rr = vps[0].TopLeftX + vps[0].Width;
    r.rb = vps[0].TopLeftY + vps[0].Height;
    if (scCount) {
        if ((float)scs[0].left   > r.rl) r.rl = (float)scs[0].left;
        if ((float)scs[0].top    > r.rt) r.rt = (float)scs[0].top;
        if ((float)scs[0].right  < r.rr) r.rr = (float)scs[0].right;
        if ((float)scs[0].bottom < r.rb) r.rb = (float)scs[0].bottom;
    }
    r.vsObj = vs; r.psObj = ps;      // identity tokens, never dereferenced
    r.vsCrc = vsCrc; r.psCrc = psCrc;
    r.knownVS = kVS; r.knownPS = kPS;
    r.dEnable = dd.DepthEnable ? 1 : 0;
    r.dWrite  = (UINT)dd.DepthWriteMask;
    r.dFunc   = (UINT)dd.DepthFunc;
    r.sEnable = dd.StencilEnable ? 1 : 0;
    r.sRef    = sref;
    r.sRead   = dd.StencilReadMask;
    r.sWrite  = dd.StencilWriteMask;
    r.bEnable = 1;
    r.bSrc    = (UINT)bdsc.RenderTarget[0].SrcBlend;
    r.bDst    = (UINT)bdsc.RenderTarget[0].DestBlend;
    r.bOp     = (UINT)bdsc.RenderTarget[0].BlendOp;
    r.bMask   = bmask;
    r.rt2 = HudTexInfo{};
    r.srv[0] = HudTexInfo{};
    r.srv[1] = HudTexInfo{};
    r.vscb = HudCbInfo{};
    r.pscb = HudCbInfo{};

    // *** THE FIELD THAT CAN CHANGE THE WHOLE PLAN, FOR ONE CALL. ***
    //
    // If the HUD is drawn into an offscreen UI target that is composited later,
    // the lever is ONE composite quad and not N elements. If it goes straight
    // into the full-size backbuffer, it is per-element or nothing. Neither should
    // be built before this column has been read.
    {
        ID3D11RenderTargetView* rtv = nullptr;
        ID3D11DepthStencilView* dsv = nullptr;
        ctx->OMGetRenderTargets(1, &rtv, &dsv);
        if (rtv) {
            r.rt2.view = rtv;
            D3D11_RENDER_TARGET_VIEW_DESC rvd{};
            rtv->GetDesc(&rvd);
            r.rt2.viewFmt = (UINT)rvd.Format;
            ID3D11Resource* res = nullptr;
            rtv->GetResource(&res);
            HudDescribeRes(res, &r.rt2);
            if (res) res->Release();
            rtv->Release();
        }
        r.hasDsv = dsv ? 1 : 0;
        if (dsv) dsv->Release();
    }

    {
        ID3D11ShaderResourceView* srv[2] = {};
        ctx->PSGetShaderResources(0, 2, srv);
        for (int i = 0; i < 2; ++i) {
            if (!srv[i]) continue;
            r.srv[i].view = srv[i];
            D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
            srv[i]->GetDesc(&svd);
            r.srv[i].viewFmt = (UINT)svd.Format;
            ID3D11Resource* res = nullptr;
            srv[i]->GetResource(&res);
            HudDescribeRes(res, &r.srv[i]);
            if (res) res->Release();
            srv[i]->Release();
        }
    }

    {
        ID3D11DeviceContext1* c1 = AsCtx1(ctx);
        HudReadCb(ctx, c1, false, &r.vscb);
        HudReadCb(ctx, c1, true,  &r.pscb);
        if (c1) c1->Release();       // AsCtx1 hands back an AddRef'd pointer
    }

    InterlockedExchange(&r.valid, 1);      // published LAST
    verdict(kHudAccepted);
    InterlockedIncrement(&g_hudEyeAccepted[r.eye]);
}

// The two INDIRECT entry points draw 565 of the capture's 2,162 calls and are
// counted but never classified anywhere in this file. Feeding them into the
// probe's ordinal keeps "draw index within the frame" a true frame position
// rather than a position in the subset we happen to inspect - and the report
// says how many of them there were, so the hole is stated instead of hidden.
void HudProbeNoteIndirect() {
    if (!g_hudCapture) return;
    InterlockedIncrement(&g_hudSeen);
    InterlockedIncrement(&g_hudIndirect);
}

// A line built in pieces, because one draw carries more columns than a single
// format string should. Truncation is impossible to miss: the buffer is far
// larger than the longest line this can produce, and Logf truncates safely.
struct HudLine {
    char buf[1900];
    int  n;
    HudLine() : n(0) { buf[0] = '\0'; }
    void Add(const char* fmt, ...) {
        const int cap = (int)sizeof(buf);
        if (n >= cap - 1) return;
        va_list a;
        va_start(a, fmt);
        const int w = _vsnprintf_s(buf + n, (size_t)(cap - n), _TRUNCATE, fmt, a);
        va_end(a);
        n = (w > 0) ? (n + w) : (int)strlen(buf);
    }
};

void HudFormatTex(HudLine* L, const char* tag, const HudTexInfo& t) {
    if (!t.view) { L->Add(" %s -", tag); return; }
    L->Add(" %s v%p r%p %ux%u m%u resfmt%u viewfmt%u", tag, t.view, t.res,
           t.w, t.h, t.mips, t.resFmt, t.viewFmt);
}

void HudFormatCb(HudLine* L, const char* tag, const HudCbInfo& c, bool withFloats) {
    if (!c.res) { L->Add(" | %s -", tag); return; }
    L->Add(" | %s %p %uB usage%u cpu0x%X bind0x%X first%u %s", tag, c.res, c.bytes,
           c.usage, c.cpu, c.bind, c.first,
           c.snap == 1 ? "snap" : (c.snap == -1 ? "snap-PAST-POOL-HEAD" : "NO-SNAP"));
    if (withFloats && c.snap == 1) {
        L->Add(" f[");
        for (int i = 0; i < 16; ++i) L->Add("%s%.4f", i ? " " : "", c.f[i]);
        L->Add("]");
    }
}

void HudResetFrame() {
    const LONG used = g_hudRows;
    const LONG n = (used < kHudRows) ? used : kHudRows;
    for (LONG i = 0; i < n; ++i) g_hudRow[i].valid = 0;
    InterlockedExchange(&g_hudRows, 0);
    InterlockedExchange(&g_hudSeen, 0);
    InterlockedExchange(&g_hudIndirect, 0);
    for (int i = 0; i < kHudReasonCount; ++i) InterlockedExchange(&g_hudFunnel[i], 0);
    InterlockedExchange(&g_hudEyeAccepted[0], 0);
    InterlockedExchange(&g_hudEyeAccepted[1], 0);
}

void HudPrintNetTest() {
    // How many of the hunted shaders were even CREATED in this run - the first
    // thing to know before reading anything into where they were bound.
    int createdVS = 0, createdPS = 0;
    const LONG ids = g_idCount;
    for (int k = 0; k < kKnownHudVSCount; ++k) {
        for (LONG i = 0; i < ids && i < kMaxIds; ++i) {
            if (!g_ids[i].pixel && g_ids[i].crc == kKnownHudVS[k]) { ++createdVS; break; }
        }
    }
    for (int k = 0; k < kKnownHudPSCount; ++k) {
        for (LONG i = 0; i < ids && i < kMaxIds; ++i) {
            if (g_ids[i].pixel && g_ids[i].crc == kKnownHudPS[k]) { ++createdPS; break; }
        }
    }
    COTW_LOG("[hud] NET TEST - the owner's Shader Toggler list against this filter.");
    COTW_LOG("[hud]   of the hunted shaders, seen CREATED this run: %d/%d vertex, "
             "%d/%d pixel  (a 0 here means the list is from another build, or "
             "creation happened before the hook went in - not that the net is wrong)",
             createdVS, kKnownHudVSCount, createdPS, kKnownHudPSCount);
    for (int k = 0; k < kKnownHudVSCount; ++k) {
        HudLine L;
        L.Add("[hud]   VS %10u (0x%08X):", kKnownHudVS[k], kKnownHudVS[k]);
        for (int r = 0; r < kHudReasonCount; ++r) {
            L.Add(" %s %ld", kHudReasonName[r], (long)g_hudKnownVS[k][r]);
        }
        COTW_LOG("%s", L.buf);
    }
    for (int k = 0; k < kKnownHudPSCount; ++k) {
        HudLine L;
        L.Add("[hud]   PS %10u (0x%08X):", kKnownHudPS[k], kKnownHudPS[k]);
        for (int r = 0; r < kHudReasonCount; ++r) {
            L.Add(" %s %ld", kHudReasonName[r], (long)g_hudKnownPS[k][r]);
        }
        COTW_LOG("%s", L.buf);
    }
    COTW_LOG("[hud]   READ IT THIS WAY: a hunted shader with hits ONLY under a "
             "rejection reason means the coarse filter is WRONG and every rect "
             "below it is meaningless until the filter is fixed. All zeros means "
             "that shader was not drawn during the probe, which says nothing "
             "either way.");
}

void HudPrintFrame() {
    const LONG claimed = g_hudRows;
    const LONG used = (claimed < kHudRows) ? claimed : kHudRows;
    const LONG seen = g_hudSeen;
    const LONG indirect = g_hudIndirect;

    LONG sum = 0;
    for (int i = 0; i < kHudReasonCount; ++i) sum += g_hudFunnel[i];

    COTW_LOG("[hud] ---- frame %ld of %ld ----", (long)g_hudFrameNo,
             (long)(g_hudFrameNo + g_hudFramesLeft));
    {
        HudLine L;
        L.Add("[hud]   funnel:");
        for (int i = 0; i < kHudReasonCount; ++i) {
            L.Add(" %s %ld%s", kHudReasonName[i], (long)g_hudFunnel[i],
                  (i + 1 < kHudReasonCount) ? " +" : "");
        }
        L.Add(" = %ld classified, of %ld draws seen (%ld indirect, never classified "
              "by this file at all) -> %s",
              (long)sum, (long)seen, (long)indirect,
              (sum + indirect == seen) ? "RECONCILES"
                                       : "DOES NOT RECONCILE - a draw path is unaccounted for");
        COTW_LOG("%s", L.buf);
    }
    COTW_LOG("[hud]   accepted per eye: eye0 %ld, eye1 %ld   (one Present spans BOTH "
             "eye passes under full-rate stereo, so a per-frame total that looks "
             "doubled is correct)",
             (long)g_hudEyeAccepted[0], (long)g_hudEyeAccepted[1]);
    COTW_LOG("[hud]   rows: %ld used of %d capacity%s", (long)used, kHudRows,
             (claimed > kHudRows)
                 ? "  *** TABLE FULL - CLUSTERS ARE BEING DROPPED, this count is a "
                   "LIMIT and not a measurement ***"
                 : "");

    // Sorted by screen position - top to bottom, then left to right - so the
    // listing reads as a HUD layout rather than as a draw log. Insertion sort:
    // at most 512 rows, once per frame, three frames.
    static int order[kHudRows];
    int n = 0;
    for (LONG i = 0; i < used; ++i) {
        if (!g_hudRow[i].valid) continue;
        order[n++] = (int)i;
    }
    for (int i = 1; i < n; ++i) {
        const int key = order[i];
        int j = i - 1;
        while (j >= 0) {
            const HudRow& a = g_hudRow[order[j]];
            const HudRow& b = g_hudRow[key];
            const bool after = (a.rt > b.rt) || (a.rt == b.rt && a.rl > b.rl) ||
                               (a.rt == b.rt && a.rl == b.rl && a.ordinal > b.ordinal);
            if (!after) break;
            order[j + 1] = order[j];
            --j;
        }
        order[j + 1] = key;
    }

    int tightScissor = 0;
    for (int k = 0; k < n; ++k) {
        const HudRow& r = g_hudRow[order[k]];
        if (r.scCount && (r.rr - r.rl < r.vp.Width - 0.5f ||
                          r.rb - r.rt < r.vp.Height - 0.5f)) {
            ++tightScissor;
        }
        HudLine L;
        L.Add("[hud] #%03d ord%ld eye%d %s ctx%p %s", k, (long)r.ordinal, r.eye,
              r.deferred ? "def" : "imm", r.ctx, HudPathName(r.path));
        L.Add(" vtx%u idx%u inst%u topo%u", r.vtx, r.idx, r.inst, r.topo);
        L.Add(" | rect %.2f,%.2f -> %.2f,%.2f (%.2fx%.2f)", r.rl, r.rt, r.rr, r.rb,
              r.rr - r.rl, r.rb - r.rt);
        L.Add(" vp[%u] %.2f,%.2f %.2fx%.2f d%.3f-%.3f", r.vpCount, r.vp.TopLeftX,
              r.vp.TopLeftY, r.vp.Width, r.vp.Height, r.vp.MinDepth, r.vp.MaxDepth);
        if (r.scCount) {
            L.Add(" sc[%u] %ld,%ld -> %ld,%ld", r.scCount, (long)r.sc.left,
                  (long)r.sc.top, (long)r.sc.right, (long)r.sc.bottom);
        } else {
            L.Add(" sc none");
        }
        L.Add(" | vs %u(0x%08X)%s ps %u(0x%08X)%s", r.vsCrc, r.vsCrc,
              r.knownVS >= 0 ? " HUNTED" : "", r.psCrc, r.psCrc,
              r.knownPS >= 0 ? " HUNTED" : "");
        L.Add(" | depth en%d wr%u fn%u sten en%u ref0x%02X rd0x%02X wr0x%02X",
              r.dEnable, r.dWrite, r.dFunc, r.sEnable, r.sRef, r.sRead, r.sWrite);
        L.Add(" | blend src%u dst%u op%u wmask0x%X", r.bSrc, r.bDst, r.bOp, r.bMask);
        HudFormatTex(&L, "| rt", r.rt2);
        L.Add(" dsv%s", r.hasDsv ? "yes" : "no");
        HudFormatTex(&L, "| t0", r.srv[0]);
        HudFormatTex(&L, "t1", r.srv[1]);
        HudFormatCb(&L, "vscb", r.vscb, true);
        COTW_LOG("%s", L.buf);
        if (r.pscb.res) {
            HudLine P;
            P.Add("[hud]      #%03d", k);
            HudFormatCb(&P, "pscb", r.pscb, true);
            COTW_LOG("%s", P.buf);
        }
    }

    // What the listing itself says about WHICH handle carries position. This is
    // the question the whole probe exists to answer, so it is answered in words
    // rather than left to be inferred from 200 lines.
    if (n > 0) {
        int sameRect = 0;
        const HudRow& first = g_hudRow[order[0]];
        for (int k = 0; k < n; ++k) {
            const HudRow& r = g_hudRow[order[k]];
            if (r.rl == first.rl && r.rt == first.rt && r.rr == first.rr &&
                r.rb == first.rb) {
                ++sameRect;
            }
        }
        if (tightScissor > 0) {
            COTW_LOG("[hud]   HANDLE: %d of %d accepted draws are confined by a "
                     "SCISSOR tighter than the viewport. That rect IS the "
                     "component's screen rectangle, for free, with no constant "
                     "buffer to decode.", tightScissor, n);
        } else if (sameRect == n) {
            COTW_LOG("[hud]   HANDLE: all %d accepted draws share ONE rect "
                     "(%.2f,%.2f -> %.2f,%.2f) and no scissor narrows it - so the "
                     "rasterizer does NOT carry position here and the quad's place "
                     "is inside the constants. Read the f[...] floats above: the "
                     "component's rectangle is in there.",
                     n, first.rl, first.rt, first.rr, first.rb);
        } else {
            COTW_LOG("[hud]   HANDLE: the accepted draws use %s viewport rects "
                     "(%d of %d share the first one), so position is partly at the "
                     "rasterizer. Check the f[...] floats for the rest.",
                     "several", sameRect, n);
        }
    } else {
        COTW_LOG("[hud]   NOTHING ACCEPTED THIS FRAME. Before concluding the HUD "
                 "is not drawn here, read the funnel above: if depth-on swallowed "
                 "everything the filter is wrong, not the HUD.");
    }
}

// Armed, run and disarmed from the Present hook, which is the only true frame
// boundary this file gets.
void HudProbeReport() {
    HudIsolateNewFrame();
    static bool prevOn = false;
    const bool on = Cfg().hud_probe;

    if (on && !prevOn) {
        prevOn = true;
        int frames = Cfg().hud_probe_frames;
        if (frames < 1) frames = 1;
        if (frames > kHudMaxFrames) frames = kHudMaxFrames;
        HudQuadReset();
        g_hudFrameNo = 0;
        g_hudFramesLeft = frames;
        for (int k = 0; k < kKnownHudVSCount; ++k)
            for (int r = 0; r < kHudReasonCount; ++r)
                InterlockedExchange(&g_hudKnownVS[k][r], 0);
        for (int k = 0; k < kKnownHudPSCount; ++k)
            for (int r = 0; r < kHudReasonCount; ++r)
                InterlockedExchange(&g_hudKnownPS[k][r], 0);
        HudResetFrame();
        COTW_LOG("[hud] ==================== HUD COMPONENT PROBE ARMED "
                 "====================");
        COTW_LOG("[hud] %d frame(s) will be recorded, then it stops by itself. "
                 "Re-arm by setting hud_probe back to 0 and to 1 again.", frames);
        COTW_LOG("[hud] Read-only: Get* calls only, everything released, no draw "
                 "changed. Accepting a draw as HUD-shaped means: depth test off "
                 "(or ALWAYS with no write), blending on, and not already claimed "
                 "as viewmodel or optic glass.");
        COTW_LOG("[hud] RUN IT TWICE - once with the game's HUD on, once with it "
                 "off (settings.json GameDisplayHUD) - and diff the two logs. What "
                 "disappears is HUD; what survives is not.");
        InterlockedExchange(&g_hudCapture, 1);
        return;      // capture starts now, so the first table printed is a whole frame
    }

    if (!on) {
        if (prevOn) {
            prevOn = false;
            if (g_hudCapture) {
                InterlockedExchange(&g_hudCapture, 0);
                COTW_LOG("[hud] probe switched off part way through - stopping.");
            }
        }
        return;
    }
    prevOn = true;
    if (!g_hudCapture) return;      // this arming has already run its course

    ++g_hudFrameNo;
    --g_hudFramesLeft;
    // Stop capturing BEFORE printing, ALWAYS - not only on the last frame. The
    // engine records draws on worker threads, and a table being read out while it
    // is still being written to is the same race that has bitten this file three
    // times already. Printing costs a frame; it is a diagnostic.
    InterlockedExchange(&g_hudCapture, 0);
    HudPrintFrame();
    if (g_hudFramesLeft > 0) {
        HudResetFrame();
        InterlockedExchange(&g_hudCapture, 1);
        return;
    }
    HudPrintNetTest();
    COTW_LOG("[hud] ==================== PROBE FINISHED ====================");
}

void STDMETHODCALLTYPE Hook_DrawIndexed(ID3D11DeviceContext* ctx, UINT idx, UINT sidx,
                                        INT bvtx) {
    InterlockedIncrement(&g_drawsThisFrame);
    InterlockedIncrement(&g_drawsSinceLastClear);
    ++t_drawsSinceClear;
    InterlockedIncrement(&g_diTotal);
    InterlockedIncrement(&g_epDrawIndexed);
    const bool deferred = IsDeferredCtx(ctx);
    if (deferred) InterlockedIncrement(&g_diDeferred);

    // The COLOUR round. Exact - those three pixel shaders draw nothing else in
    // the entire frame - and it is what teaches us every other round.
    const bool shadowHit = IsWeaponPixelShader(BoundPS(ctx));
    const bool colourHit = Cfg().weapon_pass_live_ps ? IsWeaponPixelShaderLive(ctx)
                                                     : shadowHit;
    if (shadowHit) InterlockedIncrement(&g_matchShadow);
    if (colourHit) {
        InterlockedIncrement(&g_matchDriver);
        NoteWeaponIndexCount(idx);
        CaptureWeaponConstants(ctx);
        NoteWeaponVS(BoundVS(ctx));
    }

    // The two DEPTH rounds. No pixel shader at all, drawing a mesh the colour
    // round has already claimed. Cheap test first: the index count rules out
    // everything else in the frame before the driver is asked anything.
    // GATED ON weapon_match_index AS WELL, and that omission is what kept the
    // fence moving after the stencil tag was in.
    //
    // This path has its own flag (weapon_pass_depth_rounds, default on) but
    // consults the same index-count table, so switching weapon_match_index off
    // did not switch it off: the fence's depth-prepass draws are 3252 indices
    // with no pixel shader, which is exactly what it looks for. The stencil tag
    // matched 9,740 draws - the viewmodel, correctly - while 10,440 were being
    // shifted, and the surplus 700 came through here.
    //
    // With the engine's own tag available, this is redundant anyway: the depth
    // rounds carry the first-person stencil bit like every other round.
    bool depthHit = false;
    if (!colourHit && Cfg().weapon_match_index && Cfg().weapon_pass_depth_rounds &&
        IsWeaponIndexCount(idx)) {
        depthHit = HasNoPixelShader(ctx);
        if (depthHit) InterlockedIncrement(&g_matchDepth);
    }

    // THE MATCH THAT DOES NOT NEED A SHADER.
    //
    // Everything above depends on the colour round, which depends on a CRC that
    // never fires because the shaders were created before we could fingerprint
    // them. This asks the one question the draw call answers by itself.
    //
    // No pixel-shader check here, unlike depthHit: all three draws of a mesh -
    // both depth rounds and the colour one - carry the same index count, so this
    // single test covers the whole pass. Safe because these seven counts were
    // measured to appear nowhere outside the viewmodel block in an entire frame.
    // *** BOUNDED FROM THE ANCHOR, NOT EXTENDED BY ITS OWN HITS. ***
    //
    // The previous version reset this on every match, so the run sustained
    // itself: a row of fences sharing an index count kept re-arming it and the
    // whole row picked up the shift. Counting from the ANCHOR alone makes the
    // window absolute - one round of the pass is nine draws, so a dozen is
    // ample, and nothing beyond that can chain.
    static thread_local int sinceAnchor = 99999;
    static thread_local bool prevWasWeapon = false;

    // *** THE DEPTH CLEAR IS THE ONE THING SCENERY CANNOT IMITATE. ***
    //
    // Index counts are not unique across the map. 3252 matched 2520 times a
    // report against 1080 for every real weapon mesh - the surplus was a row of
    // fences - and anchoring on 77484 did not help, which means 77484 is drawn
    // out in the world too. No table of numbers is going to settle this.
    //
    // MEASURED, not assumed. The guess was that the viewmodel is drawn just
    // after a depth clear; it is the opposite - the pass sits ~1000 draws after
    // one, in a tight band, and the impostors are the EARLY draws:
    //
    //     real weapon meshes   clear = 1003 .. 1033   (30-draw band)
    //     2484 impostor        clear =  710
    //     3252 impostor        clear =  228
    //
    // Gating on "close to a clear" therefore threw away the whole pass and kept
    // nothing, which is half a gun disappearing. The correct test is a MINIMUM,
    // and the 300-draw gap either side of it leaves plenty of margin.
    // PER THREAD. The shared counter is incremented by every context at once, so
    // as a per-draw position it is meaningless - see t_drawsSinceClear.
    const LONG sinceClear = t_drawsSinceClear;
    if (sinceClear > t_maxSinceClear) t_maxSinceClear = sinceClear;
    if (Cfg().weapon_pass_diag && IsWeaponIndexCount(idx)) {
        NoteSeededCountClear(idx, sinceClear);
    }

    // Relative to this thread's high-water mark, so it survives scenes of
    // different density. Measured here: weapon 897..929 against a mark of ~929,
    // i.e. 96%; the fence at 200 is 22%. The default cut of 60% sits in the
    // enormous gap between them. weapon_min_since_clear still applies as an
    // absolute floor for anyone who wants to pin it.
    const LONG tailCut = (t_maxSinceClear * Cfg().weapon_pass_tail_pct) / 100;
    const bool freshClear = sinceClear >= tailCut &&
                            sinceClear >= (LONG)Cfg().weapon_min_since_clear &&
                            sinceClear <= (LONG)Cfg().weapon_max_since_clear;

    const bool isAnchor = (idx == kWeaponPassAnchor) && freshClear;
    if (isAnchor) sinceAnchor = 0;
    else if (sinceAnchor < 99999) ++sinceAnchor;
    const bool inRun = freshClear && sinceAnchor <= Cfg().weapon_pass_gap;

    // The anchor stands alone; everything else needs the run to be live.
    const bool idxHit = Cfg().weapon_match_index && IsWeaponIndexCount(idx) &&
                        (isAnchor || inRun);
    if (idxHit && !colourHit && !depthHit) InterlockedIncrement(&g_matchDepth);

    // *** THE 162-INDEX MESH, MATCHED BY WHERE IT IS RATHER THAN WHAT IT IS. ***
    //
    // 162 is a real weapon mesh, but 14 world draws in the frame carry the same
    // count, so the seed table cannot claim it - and leaving it out is visible:
    // the rest of the gun moves and that piece does not, which is the flicker.
    //
    // The frame tail shows it sitting IMMEDIATELY after a matched weapon draw,
    // every time, three times a frame:
    //     3  1440  3  36  3  5007*  162  3  3  1059
    //     3  77484*  5007*  162  3  3  159  3  6  1470
    //     3  3  498  3  3  5007*  162  3  204  3
    // while the colliding world draws never neighbour one. Position in the pass
    // separates them when the count alone cannot.
    //
    // Deliberately only the IMMEDIATELY following draw: a wider window would
    // start swallowing the ordinary geometry that follows the pass.
    const bool neighbourHit =
        Cfg().weapon_match_index && prevWasWeapon && idx == kWeaponNeighbourIdx;

    // The stencil tag stands alone and outranks everything else. When it is on,
    // the index-count machinery is not consulted at all - it exists only as a
    // fallback if some future build stops tagging.
    const ViewmodelHit vh = ClassifyViewmodelDraw(ctx, idx);
    // The HUD probe. Placed AFTER the classifier so it can exclude what the mod
    // already claims rather than re-deriving it and disagreeing, and BEFORE any
    // of the shift machinery so it only ever sees the game's own state. Costs one
    // global read when hud_probe is 0. Counts are the D3D ones: DrawIndexed has
    // an index count, one instance and no separate vertex count.
    HudProbeDraw(ctx, vh, 0, 0, idx, 1);
    // THE HUD CONTROLS. Re-added WITHOUT the two diagnostics that went in
    // beside them - HudProbeQuad and HudIsolateSkip - because removing all six
    // of these lines is what brought the weapon shift back, and putting all six
    // in again would forfeit that. Three lines, not six, so if the weapon breaks
    // a second time the search is already halved.
    if (Cfg().hud_hide_sunflare && HudIsSunFlareDraw(ctx)) return;
    if (Cfg().hud_hide_glare && HudLooksLikeHud(ctx, vh) && HudIsGlareDraw(ctx)) return;
    HudMove hudMove_(ctx, vh);
    // No depth test needed here - an HDR float source, upsampling, at an
    // eighth of the render is specific enough by itself.
    const bool stencilHit = vh.stencil;
    if (stencilHit) InterlockedIncrement(&g_matchStencil);

    // Glass and other blended parts of the viewmodel, which the engine does not
    // tag.
    //
    // NO PROXIMITY GATE, and that is a lesson paid for twice. The original rule
    // was "a blended draw within N draws of a tagged one" - but the engine
    // records the transparency pass on its OWN deferred context, and the gap
    // counter was thread-local, so on the lens's thread no tagged draw ever
    // happened and the gap sat at infinity. Widening N from 4 to 12 changed
    // nothing; the lens (2205 indices live, 2208 in the capture) never appeared
    // in the tally at all. Same bug class as the matrix ring being thread_local.
    //
    // What actually identifies viewmodel glass is the MATRIX IDENTITY check in
    // BindShiftedWeaponCB: the lens's clip matrix is bit-identical to the
    // tagged body's (same object, same instant), and 64 bytes of camera-
    // dependent floats cannot collide by accident. So every blended draw is
    // OFFERED to the shift while the ring has entries, and the ring decides.
    // A blended draw that matches nothing is left completely alone.
    // Glass: a BLENDED draw reading a buffer we have seen carrying a copy of the
    // weapon's clip matrix. Both tests are cheap - a cached blend-state lookup
    // and a pointer compare against at most 16 resources - and neither needs the
    // buffer's contents, which is what every previous attempt foundered on.
    static thread_local int sinceStencil = 99999;   // kept for the diagnostics gap column
    if (stencilHit) sinceStencil = 0;
    else if (sinceStencil < 99999) ++sinceStencil;
    const bool glassHit = vh.glass;
    // (The stencil test, the frame-position counter, the material match and the
    // older glass rules all now live in ClassifyViewmodelDraw above, so that the
    // instanced and non-indexed draw paths get exactly the same answers. Keeping
    // a second copy here is what let the scope's mask disc slip through.)

    // WHICH draws are being taken as glass, by index count.
    //
    // The binocular lens is 2208 indices in the capture, and it is the very next
    // draw after the last tagged one. The lenses currently do not move while the
    // body does, yet the glass path reports 8 hits a frame with none rejected -
    // so it is catching something, and that something is not the lens. Naming
    // the counts settles it instead of another round of inference.
    //
    // Also records how far past the last tagged draw each one was, because if
    // the lens sits further out than weapon_3d_glass allows it is being filtered
    // before any of the matrix logic is reached.
    if (glassHit && Cfg().weapon_pass_diag) NoteGlassDraw(idx, sinceStencil);
    if (glassHit) {
        const LONG nth = InterlockedIncrement(&g_glassHitsThisFrame);
        // The third and beyond, with what it was and how far it sat from the
        // viewmodel - enough to say whether it is a real lens drawn again or
        // something else that wandered into the window.
        // FROM THE FIFTH. Four glass draws happen every frame (two lens, two
        // untagged copies of the mask mesh); the count reaches five only
        // sometimes, so the FIFTH is the intermittent one and therefore the
        // flicker. Recording from the third just reprinted the two constant
        // ones and buried it.
        if (nth >= 5) {
            const LONG e2 = InterlockedIncrement(&g_extraGlassCount) - 1;
            if (e2 >= 0 && e2 < kMaxExtraGlass) {
                g_extraGlass[e2].idx = idx;
                g_extraGlass[e2].gap = g_drawsSinceTagged;
                g_extraGlass[e2].nth = nth;
                g_extraGlass[e2].blended = DrawIsBlended(ctx) ? 1 : 0;
                g_extraGlass[e2].tagged = stencilHit ? 1 : 0;
            }
        }
    }

    // *** THE EXACT RULES ONLY, WHEN ASKED. ***
    //
    // Every widening of coverage today made the flicker WORSE, in three separate
    // ways: the stencil-tag fallback, then routing all constants failures to the
    // rasterizer, then removing the snapshot misses entirely (misses hit zero and
    // the weapon and hands began to flicker). The consistent reading is that some
    // draws are being moved that should not be, and the failures were the accident
    // keeping them still.
    //
    // So this narrows instead. Only the rules that have never been wrong survive:
    // the engine's own first-person stencil tag - 57/57 viewmodel draws, 0/4319
    // world draws across two captures - and the glass rules built on top of it,
    // which are anchored to the mask it stamps.
    //
    // Dropped here: the colour and depth rounds (a shader fingerprint that misses
    // ~85% of the frame's shaders), the index-count table (index counts are not
    // unique across a map - a fence shared 3252 with a gun mesh), the neighbour
    // rule, and the vertex-shader match.
    //
    // If the flicker stops and pieces of the gun go flat, this is the diagnosis and
    // those pieces need identifying properly. If it does not, the loose rules were
    // never the problem.
    const bool looseHit = colourHit || depthHit || idxHit || neighbourHit ||
                          IsWeaponVertexShader(BoundVS(ctx));
    if (looseHit && !stencilHit && !glassHit) InterlockedIncrement(&g_looseOnly);
    const bool isWeapon = stencilHit || glassHit ||
                          (!Cfg().weapon_stencil_only && looseHit);

    prevWasWeapon = colourHit || depthHit ||
                    (idxHit && !IsAmbiguousWeaponIdx(idx));

    if (Cfg().weapon_pass_diag) {
        NoteTailDraw(idx, isWeapon);
        if (isWeapon) {
            const int eye = (CurrentRenderEye() == 0) ? 0 : 1;
            InterlockedIncrement(&g_weaponHitsEye[eye]);
            InterlockedIncrement(&g_weaponHitsThisFrame);
        }
    }

    const PFN_DrawIndexed orig = (PFN_DrawIndexed)g_oDrawIndexed.For(ctx);
    if (!orig) return;

    // A flat gun welded to your face reads worse than no gun at all, which is
    // why several shipped VR mods simply remove it. Now that all thirty draws
    // are identified, that is one line - and it is the fallback if per-eye
    // depth turns out not to be reachable.
    if (isWeapon && Cfg().weapon_hide) return;

    if (ShouldSkipCurrentDraw(ctx)) return;          // swallow the draw entirely
    if (isWeapon) {
        // The gun in 3D: bind a copy of its constants with WorldViewProjection
        // shifted per eye, draw, then put the game's buffer straight back. The
        // restore is not optional - our constants would otherwise be applied to
        // whatever the engine draws next.
        ViewmodelHit h = vh;
        h.weapon = true;   // the index-count and neighbour rules can promote a draw
        IssueShiftedViewmodel(ctx, h, idx, [&] { orig(ctx, idx, sidx, bvtx); });
        return;
    }
    orig(ctx, idx, sidx, bvtx);
}

void STDMETHODCALLTYPE Hook_DrawIndexedInstanced(ID3D11DeviceContext* ctx, UINT ipi,
                                                 UINT ic, UINT sil, INT bvl, UINT sii) {
    InterlockedIncrement(&g_drawsThisFrame);
    InterlockedIncrement(&g_drawsSinceLastClear);
    ++t_drawsSinceClear;
    InterlockedIncrement(&g_epDrawIndexedInstanced);
    if (IsWeaponPixelShader(BoundPS(ctx))) {
        CaptureWeaponConstants(ctx);
        NoteWeaponVS(BoundVS(ctx));
    }
    // THE BUSIEST DRAW PATH IN THE FRAME - 785 calls against DrawIndexed's 718 -
    // and until now the only question it asked was a shader fingerprint that
    // misses most of the frame. It gets the full test now.
    const ViewmodelHit vh = Cfg().weapon_all_draw_types ? ClassifyViewmodelDraw(ctx, ic)
                                                       : ViewmodelHit{};
    // ipi is IndexCountPerInstance and ic is InstanceCount, per D3D's own
    // parameter order - which is what the probe records. (The classifier above
    // is handed ic; that is existing behaviour and is deliberately left alone
    // here, but it is why the probe's idx column can disagree with the mesh
    // number the viewmodel diagnostics print for the same draw.)
    HudProbeDraw(ctx, vh, 1, 0, ipi, ic);
    // THE HUD CONTROLS. Re-added WITHOUT the two diagnostics that went in
    // beside them - HudProbeQuad and HudIsolateSkip - because removing all six
    // of these lines is what brought the weapon shift back, and putting all six
    // in again would forfeit that. Three lines, not six, so if the weapon breaks
    // a second time the search is already halved.
    if (Cfg().hud_hide_sunflare && HudIsSunFlareDraw(ctx)) return;
    if (Cfg().hud_hide_glare && HudLooksLikeHud(ctx, vh) && HudIsGlareDraw(ctx)) return;
    HudMove hudMove_(ctx, vh);
    // No depth test needed here - an HDR float source, upsampling, at an
    // eighth of the render is specific enough by itself.
    if (ShouldSkipCurrentDraw(ctx)) return;
    const PFN_DrawIndexedInstanced orig =
        (PFN_DrawIndexedInstanced)g_oDrawIndexedInstanced.For(ctx);
    if (!orig) return;
    const bool isWeapon = vh.weapon || IsWeaponPixelShader(BoundPS(ctx)) ||
                          IsWeaponVertexShader(BoundVS(ctx));
    if (isWeapon && Cfg().weapon_hide) return;
    if (isWeapon) {
        ViewmodelHit h = vh;
        h.weapon = true;
        IssueShiftedViewmodel(ctx, h, ic, [&] { orig(ctx, ipi, ic, sil, bvl, sii); });
        return;
    }
    orig(ctx, ipi, ic, sil, bvl, sii);
}

void STDMETHODCALLTYPE Hook_Draw(ID3D11DeviceContext* ctx, UINT vcount, UINT start) {
    InterlockedIncrement(&g_drawsThisFrame);
    InterlockedIncrement(&g_drawsSinceLastClear);
    ++t_drawsSinceClear;
    InterlockedIncrement(&g_epDraw);
    // THE TAA RESOLVE PROBE, AND IT LIVES IN THIS HOOK ALONE.
    //
    // The resolve is a Draw(VertexCount=3) - a full-screen triangle - so only
    // the plain Draw path can ever see it. That is worth stating because the
    // regression that cost a day put a probe in all FOUR draw hooks; one hook is
    // a quarter of the surface, on the least busy path in the frame (94 calls
    // against DrawIndexedInstanced's 785). Read-only: Get*/GetDesc only, no Map,
    // no CopyResource - a read-map on a deferred context is what broke it.
    if (TaaWantsDraws()) TaaOnDraw(ctx, vcount);
    if (IsWeaponPixelShader(BoundPS(ctx))) {
        CaptureWeaponConstants(ctx);
        NoteWeaponVS(BoundVS(ctx));
    }
    const ViewmodelHit vh = Cfg().weapon_all_draw_types ? ClassifyViewmodelDraw(ctx, vcount)
                                                       : ViewmodelHit{};
    HudProbeDraw(ctx, vh, 2, vcount, 0, 1);
    // THE HUD CONTROLS. Re-added WITHOUT the two diagnostics that went in
    // beside them - HudProbeQuad and HudIsolateSkip - because removing all six
    // of these lines is what brought the weapon shift back, and putting all six
    // in again would forfeit that. Three lines, not six, so if the weapon breaks
    // a second time the search is already halved.
    if (Cfg().hud_hide_sunflare && HudIsSunFlareDraw(ctx)) return;
    if (Cfg().hud_hide_glare && HudLooksLikeHud(ctx, vh) && HudIsGlareDraw(ctx)) return;
    HudMove hudMove_(ctx, vh);
    // No depth test needed here - an HDR float source, upsampling, at an
    // eighth of the render is specific enough by itself.
    if (ShouldSkipCurrentDraw(ctx)) return;
    const PFN_Draw orig = (PFN_Draw)g_oDraw.For(ctx);
    if (!orig) return;
    const bool isWeapon = vh.weapon || IsWeaponPixelShader(BoundPS(ctx)) ||
                          IsWeaponVertexShader(BoundVS(ctx));
    if (isWeapon && Cfg().weapon_hide) return;
    if (isWeapon) {
        ViewmodelHit h = vh;
        h.weapon = true;
        IssueShiftedViewmodel(ctx, h, vcount, [&] { orig(ctx, vcount, start); });
        return;
    }
    // PER-EYE TEMPORAL HISTORY, wrapped around the real draw and nothing else.
    //
    // TaaOnDraw at the top of this function decided; this only issues. It sits
    // BELOW every early return above it on purpose - if another feature swallows
    // the resolve, the substitution simply does not happen, and the flag it
    // would have used is cleared on the next Draw before anything can read it.
    if (TaaResolvePending()) {
        // Stage 1 first: if the pass has been replaced outright, the engine's
        // draw must not run at all.
        if (TaaReplacePass(ctx, orig)) return;
        TaaSubstituteBegin(ctx);
        orig(ctx, vcount, start);
        TaaSubstituteEnd(ctx);
        return;
    }
    orig(ctx, vcount, start);
}

void STDMETHODCALLTYPE Hook_DrawInstanced(ID3D11DeviceContext* ctx, UINT vpi, UINT ic,
                                          UINT svl, UINT sii) {
    InterlockedIncrement(&g_drawsThisFrame);
    InterlockedIncrement(&g_drawsSinceLastClear);
    ++t_drawsSinceClear;
    InterlockedIncrement(&g_epDrawInstanced);
    if (IsWeaponPixelShader(BoundPS(ctx))) {
        CaptureWeaponConstants(ctx);
        NoteWeaponVS(BoundVS(ctx));
    }
    const ViewmodelHit vh = Cfg().weapon_all_draw_types ? ClassifyViewmodelDraw(ctx, ic)
                                                       : ViewmodelHit{};
    // vpi is VertexCountPerInstance, ic is InstanceCount - see the note on
    // DrawIndexedInstanced above.
    HudProbeDraw(ctx, vh, 3, vpi, 0, ic);
    // THE HUD CONTROLS. Re-added WITHOUT the two diagnostics that went in
    // beside them - HudProbeQuad and HudIsolateSkip - because removing all six
    // of these lines is what brought the weapon shift back, and putting all six
    // in again would forfeit that. Three lines, not six, so if the weapon breaks
    // a second time the search is already halved.
    if (Cfg().hud_hide_sunflare && HudIsSunFlareDraw(ctx)) return;
    if (Cfg().hud_hide_glare && HudLooksLikeHud(ctx, vh) && HudIsGlareDraw(ctx)) return;
    HudMove hudMove_(ctx, vh);
    // No depth test needed here - an HDR float source, upsampling, at an
    // eighth of the render is specific enough by itself.
    if (ShouldSkipCurrentDraw(ctx)) return;
    const PFN_DrawInstanced orig = (PFN_DrawInstanced)g_oDrawInstanced.For(ctx);
    if (!orig) return;
    const bool isWeapon = vh.weapon || IsWeaponPixelShader(BoundPS(ctx)) ||
                          IsWeaponVertexShader(BoundVS(ctx));
    if (isWeapon && Cfg().weapon_hide) return;
    if (isWeapon) {
        ViewmodelHit h = vh;
        h.weapon = true;
        IssueShiftedViewmodel(ctx, h, ic, [&] { orig(ctx, vpi, ic, svl, sii); });
        return;
    }
    orig(ctx, vpi, ic, svl, sii);
}

void STDMETHODCALLTYPE Hook_ClearDSV(ID3D11DeviceContext* ctx, ID3D11DepthStencilView* dsv,
                                     UINT flags, FLOAT depth, UINT8 stencil) {
    const LONG n = InterlockedIncrement(&g_clearsThisFrame) - 1;
    if (n >= 0 && n < kMaxClears) {
        g_clearMarks[n].atDraw = g_drawsThisFrame;
        g_clearMarks[n].drawsAfter = 0;
    }
    if (n > 0 && (n - 1) < kMaxClears) {
        g_clearMarks[n - 1].drawsAfter = g_drawsSinceLastClear;
    }
    g_lastClearAtDraw = g_drawsThisFrame;
    InterlockedExchange(&g_drawsSinceLastClear, 0);
    t_drawsSinceClear = 0;
    if (auto orig = (PFN_ClearDSV)g_oClearDSV.For(ctx)) {
        orig(ctx, dsv, flags, depth, stencil);
    }
}

void STDMETHODCALLTYPE Hook_UpdateSubresource(ID3D11DeviceContext* ctx,
                                              ID3D11Resource* res, UINT sub,
                                              const D3D11_BOX* box, const void* src,
                                              UINT rowPitch, UINT depthPitch) {
    // UpdateSubresource hands us a CONST pointer to the caller's data, so
    // patching means substituting a modified copy. Without this the fix only
    // ever ran for buffers written through Map - and if the weapon goes the
    // other way, it silently did nothing at all.
    const void* srcOut = src;
    static __declspec(thread) unsigned char t_copy[4096];

    // *** SNAPSHOT HERE TOO. ***
    //
    // The snapshot table was filled ONLY from Unmap, so every constant buffer the
    // engine writes this way was permanently invisible to the constants path -
    // and a draw whose buffer has no snapshot is refused. That is the lens: about
    // four refusals a frame, 742 a window, UNCHANGED when the table went from 64
    // slots to 256. A number that does not move when you quadruple the capacity
    // is not an eviction problem; the buffer was never in the table at all.
    //
    // Being refused meant falling back to the rasterizer, which is the one route
    // a lens cannot survive: its shader samples the scene in SCREEN SPACE, so
    // moving the viewport moves the sampled picture along with the geometry.
    if (src && res && SnapshotsWanted()) {
        D3D11_RESOURCE_DIMENSION dS{};
        res->GetType(&dS);
        if (dS == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC bS{};
            static_cast<ID3D11Buffer*>(res)->GetDesc(&bS);
            if ((bS.BindFlags & D3D11_BIND_CONSTANT_BUFFER) &&
                bS.ByteWidth >= kMinSnapshotBytes) {
                const UINT want = bS.ByteWidth < kPoolSnapshot ? bS.ByteWidth
                                                               : kPoolSnapshot;
                CBSnapshot* sn = SlotForSnapshot(res);
                __try {
                    memcpy(sn->data, src, want);
                    sn->bytes = want;
                    sn->res = res;          // published last
                    InterlockedIncrement(&g_snapViaUpdate);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    sn->bytes = 0;
                    sn->res = nullptr;
                }
            }
        }
    }

    if (src && WeaponPatchEnabled() && res) {
        D3D11_RESOURCE_DIMENSION d0{};
        res->GetType(&d0);
        if (d0 == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC b0{};
            static_cast<ID3D11Buffer*>(res)->GetDesc(&b0);
            if ((b0.BindFlags & D3D11_BIND_CONSTANT_BUFFER) &&
                IsWeaponBuffer(b0.ByteWidth, (UINT)Cfg().weapon_cb_offset) &&
                b0.ByteWidth <= sizeof(t_copy)) {
                __try {
                    memcpy(t_copy, src, b0.ByteWidth);
                    float* m = reinterpret_cast<float*>(t_copy) +
                               (Cfg().weapon_cb_offset / 4);
                    float d = 0.0f;
                    if (LooksLikeCloseModel(m, &d)) {
                        PatchWeaponMatrix(m);
                        InterlockedIncrement(&g_patchViaUpdate);
                        srcOut = t_copy;
                    }
                } __except (EXCEPTION_EXECUTE_HANDLER) { srcOut = src; }
            }
        }
    }

    // THE OTHER UPLOAD PATH, for the 3D shift.
    //
    // Snapshots are taken at Unmap, so anything written with UpdateSubresource
    // instead has no snapshot and its draws are skipped - flat while everything
    // around them has depth. That is a candidate for the hands staying flat
    // while the glove on the same hand did not.
    //
    // *** AND IT MUST NOT BE GATED ON weapon_3d. ***
    //
    // The temporal resolve finds its camera through these same snapshots, so
    // gating them on the weapon feature meant that turning weapon depth OFF
    // starved the resolve of every camera written this way - which is exactly
    // the owner's report that DLSS "loses a lot of its cleanness and the game
    // becomes more shimmery" with weapon depth off (2026-08-14). A rendering
    // feature was quietly feeding a different subsystem's data.
    if (src && res && !box &&
        (Cfg().weapon_3d || Cfg().taa_replace_pass || Cfg().taa_probe)) {
        D3D11_RESOURCE_DIMENSION d1{};
        res->GetType(&d1);
        if (d1 == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC b1{};
            static_cast<ID3D11Buffer*>(res)->GetDesc(&b1);
            if ((b1.BindFlags & D3D11_BIND_CONSTANT_BUFFER) &&
                b1.ByteWidth >= kMinSnapshotBytes) {
                const UINT want =
                    b1.ByteWidth < kPoolSnapshot ? b1.ByteWidth : kPoolSnapshot;
                CBSnapshot* s = SlotForSnapshot(res);
                __try {
                    memcpy(s->data, src, want);
                    s->bytes = want;
                    s->res = res;          // published last
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    s->bytes = 0;
                    s->res = nullptr;
                }
                // Cameras written THIS way were invisible to the resolve's
                // candidate collection, which only ever saw Map/Unmap. Offer
                // them too, so the choice is made from every camera the frame
                // actually used rather than from a subset.
                __try {
                    NoteCameraCandidateImpl(ctx, static_cast<const uint8_t*>(src),
                                            b1.ByteWidth);
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
    }

    if (src && Cfg().weapon_cb_scan) {
        D3D11_RESOURCE_DIMENSION dim{};
        res->GetType(&dim);
        if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC bd{};
            static_cast<ID3D11Buffer*>(res)->GetDesc(&bd);
            if (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) NoteBufferSize(bd.ByteWidth);
            if ((bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && bd.ByteWidth >= 64 &&
                bd.ByteWidth <= 262144) {
                __try {
                    ScanProjections(static_cast<const float*>(src), bd.ByteWidth);
                    ScanCloseModels(static_cast<const float*>(src), bd.ByteWidth);
                    ScanBuffer(static_cast<const float*>(src), bd.ByteWidth);
                } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
        }
    }
    if (auto orig = (PFN_UpdateSubresource)g_oUpdateSubresource.For(ctx)) {
        orig(ctx, res, sub, box, srcOut, rowPitch, depthPitch);
    }
}

HRESULT STDMETHODCALLTYPE Hook_Map(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub,
                                   D3D11_MAP type, UINT flags,
                                   D3D11_MAPPED_SUBRESOURCE* mapped) {
    auto origMap = (PFN_Map)g_oMap.For(ctx);
    if (!origMap) return E_FAIL;
    const HRESULT hr = origMap(ctx, res, sub, type, flags, mapped);
    t_mapped = nullptr;
    if (FAILED(hr) || !mapped || !mapped->pData) return hr;
    // Watch when the content scan is on OR when the weapon's own buffers are
    // known - the latter costs nothing and is the whole point now.
    //
    // weapon_3d BELONGS IN THIS GATE. Without it Map returns here, t_mapped is
    // never set, Unmap never snapshots InstanceConsts, and the 3D patch has
    // nothing to copy - it reported "0 draws ... NEVER RAN" while coverage was a
    // healthy 190% and every weapon draw was being matched. A feature whose data
    // source is switched off by an unrelated flag is precisely the failure this
    // file keeps paying for.
    // hud_probe belongs in this gate for exactly the same reason weapon_3d did:
    // the probe reports the first sixteen floats of the constant buffer each HUD
    // draw reads, and those floats come from the snapshot table this hook feeds.
    // Without it named here the probe would print "no snapshot" for every draw
    // and read as "the HUD has no constants" - a wrong answer produced by an
    // unrelated switch, which is the failure this file keeps paying for.
    // AND THE THIRD TIME, 2026-08-14: the TEMPORAL RESOLVE lives on this hook
    // too. Everything Unmap does - the snapshot table, the camera candidates
    // the resolve selects from, the jitter overlay, the previous-frame patch -
    // needs t_mapped, and t_mapped is set only if we get past this line. With
    // weapon depth off and nothing else on, Unmap did NOTHING, the resolve was
    // left reprojecting on scraps, and the owner reported exactly that: "with
    // weapon depth off the game is still shimmery, enabling the depth brings
    // back the clean look". A rendering feature was switched off and a
    // different subsystem quietly lost its data source - the same failure this
    // comment has now described three times.
    if (!Cfg().weapon_cb_scan && !Cfg().weapon_3d && !Cfg().hud_probe &&
        !Cfg().taa_replace_pass && !Cfg().taa_probe && !Cfg().dlss_enable &&
        g_weaponCBCount == 0) {
        return hr;
    }

    D3D11_RESOURCE_DIMENSION dim{};
    res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return hr;

    D3D11_BUFFER_DESC bd{};
    static_cast<ID3D11Buffer*>(res)->GetDesc(&bd);
    if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return hr;
    NoteBufferSize(bd.ByteWidth);
    // Cap raised well past 4096: the old limit silently hid any large ring
    // buffer, which is exactly where a renderer of this vintage would put the
    // per-draw constants we cannot find.
    if (bd.ByteWidth < 48 || bd.ByteWidth > 262144) return hr;

    t_mapped = res;
    t_data = mapped->pData;
    t_size = bd.ByteWidth;
    return hr;
}

// *** TIER 1 FOV, STAGE 0: MEASURE THE CAMERA'S PROJECTION. ***
//
// Read-only. Nothing here writes, patches or moves anything - it exists so that
// every later stage can be judged against a number the GPU itself reports.
//
// There is no projection matrix in this renderer to read directly: every 4x4 in
// all 26,397 constant buffers was tested and none is a pure projection. World
// draws read a premultiplied world->clip matrix, so the projection is recovered
// from its STRUCTURE instead:
//
//     M = V * P, row-vector convention, row-major storage
//     |col0| = a = 1/tanH     (the view basis rows are unit length)
//     |col1| = b = 1/tanV
//     |col2| = 0              P's row2 is (0,0,0,+1), so z reaches only w
//     |col3| = 1              the camera forward; w = view depth
//     m[14]  = n              the near plane
//
// Identified BY CONTENT, never by resource id: the engine cycles a small pool
// and the same buffer holds a shadow matrix at one event and the camera matrix
// at another. Shadow matrices fail structurally (col3 = (0,0,0,1), col2 non-zero)
// and per-object baked WVPs fail on |col3| != 1.
//
// THE ENGINE IS REVERSE-Z WITH AN INFINITE FAR PLANE - row2 (0,0,0,+1), row3
// (0,0,n,0). There is no far term to read here and none may ever be written; the
// textbook far/(near-far) form would break depth outright.
volatile LONG g_projSeen = 0;        // camera-clip windows seen this second
volatile LONG g_projAgreed = 0;      // ...where two offsets agreed
float g_projA = 0.0f, g_projB = 0.0f, g_projN = 0.0f;

inline float Len3(float x, float y, float z) { return sqrtf(x * x + y * y + z * z); }

bool IsSharedCameraClip(const float* m, float* aOut, float* bOut, float* nOut) {
    const float c0 = Len3(m[0], m[4], m[8]);
    const float c1 = Len3(m[1], m[5], m[9]);
    const float c2 = Len3(m[2], m[6], m[10]);
    const float c3 = Len3(m[3], m[7], m[11]);
    const float n = m[14];
    const float big = (c0 > c1 ? (c0 > c3 ? c0 : c3) : (c1 > c3 ? c1 : c3));
    if (!(c2 < 1e-4f * big)) return false;              // z must reach only w
    if (fabsf(c3 - 1.0f) > 1e-3f) return false;         // shared, not baked
    if (!(n > 0.005f && n < 0.02f)) return false;       // the camera's near
    if (!(c0 > 0.1f && c0 < 40.0f)) return false;
    if (!(c1 > 0.1f && c1 < 40.0f)) return false;
    *aOut = c0; *bOut = c1; *nOut = n;
    return true;
}

// *** OWN THE JITTER: an 8-position overlay on top of the engine's 2. ***
//
// The engine jitters its projection between exactly TWO sub-pixel positions;
// DLSS's requirement is at least EIGHT for DLAA (more when upscaling), and the
// shortfall is the measured cause of the residual edge-shake (see
// DLSS_IMPLEMENTATION.md 7d). We own every camera-block upload right here in
// Hook_Unmap, so the fix is to ADD a small offset of our own to m[12]/m[13] of
// each main-view camera before the engine consumes it - the same slot, the
// same m[15]-scaled form, the engine's own jitter left untouched underneath.
//
// The overlay is an 8-entry Halton(2,3) sequence scaled to +/-0.22 px, so the
// combined positions stay inside the spec's +/-0.5 px and cover 8+ distinct
// spots. It advances ONCE PER REAL FRAME (both eyes of a frame see the same
// overlay - draws within a frame must agree), and it is CONSTANT within the
// frame for the same reason. Shadow/reflection cameras never receive it: the
// tangent test that guards the reprojection capture guards this too, so shadow
// rasterisation keeps its stock 2-phase and cannot start shimmering (the trap
// Luma's Just Cause 3 notes warn about).
//
// The exact value applied each frame is published (JitterOverlayNow) so the
// resolve can subtract it before estimating the ENGINE's part (the third
// difference assumes pure alternation) and can report the exact sum to DLSS.
float g_jitRenderW = 0.0f, g_jitRenderH = 0.0f;
LONG  g_jitPhase = 0;
volatile LONG g_jitInjectedBlocks = 0;
// The owner found a HEADING where the picture is stable and headings where it
// flickers - and the injection is scaled by m[15], which sweeps through zero
// as the view yaws. These record the evidence: the range of m[15] seen and how
// many cameras were SKIPPED by the |m[15]| guard (skipped = rendered without
// the overlay while DLSS was told it was there = misregistration).
float g_jitM15Min = 1e30f, g_jitM15Max = 0.0f;
volatile LONG g_jitSkippedSmallM15 = 0;
// Halton(2,3) entries 1..8, centred and halved: max |x| 0.219, |y| 0.194 px.
constexpr float kJitOvlX[8] = { 0.0000f, -0.1250f,  0.1250f, -0.1875f,
                                0.0625f, -0.0625f,  0.1875f, -0.2188f};
constexpr float kJitOvlY[8] = {-0.0833f,  0.0833f, -0.1944f, -0.0278f,
                                0.1389f, -0.1389f,  0.0278f,  0.1944f};

void InjectJitterOverlay(uint8_t* data, UINT bytes);

// *** CAMERA SELECTION, NOT CAMERA REJECTION (taa_camera_lock v2). ***
//
// Measured 2026-08-12: standing perfectly still, the matrix the resolve
// captured jumped by up to ~3 world units in its translation row on 87 of 90
// frames. The resolve reads whatever camera block happens to be BOUND at its
// draw - and the resolve provably does not read a camera at all, so that
// binding is incidental. Several cameras share the player's tangents (the
// main eye, water reflections meters away) and the engine rewrites this block
// ~90 times a frame through a small buffer pool, so the capture was a
// per-frame lottery. That wander - not the jitter - is the universal object
// shake: it grows with history weight, survives with DLSS off, and no jitter
// work could ever have touched it.
//
// v1 tried REJECTION at the resolve (hold off anything far from last frame).
// With one candidate to judge it starved: 15-frame stale cycles, tree flicker
// on look movement. v2 collects EVERY main-view camera written during the
// eye's pass here in Hook_Unmap - where every constant-buffer write is
// already seen - and the resolve then SELECTS the one continuous with last
// frame's choice. Selection cannot starve: there is always a nearest.
struct CamCandidate {
    float m[16];
    int   hits;        // writes that carried this same camera this pass
    float wobble;      // how much the cluster's members differed from each other
};
constexpr int kMaxCamCand = 24;
CamCandidate g_camCand[2][kMaxCamCand];
int g_camCandN[2] = {0, 0};
volatile LONG g_camCandOverflow = 0;
volatile LONG g_camCandSeen[2] = {};

void NoteCameraCandidateImpl(ID3D11DeviceContext* ctx, const uint8_t* data,
                             uint32_t bytes) {
    if (!Cfg().taa_camera_lock || bytes != 704 || !data) return;
    // Deferred contexts record now and execute later, so a camera written
    // there belongs to no particular pass at this instant. The main view's
    // blocks were measured on the immediate context (94 of 94).
    if (ctx && IsDeferredCtx(ctx)) return;

    const float* m = reinterpret_cast<const float*>(data);
    float a = 0.0f, b = 0.0f, n = 0.0f;
    if (!IsSharedCameraClip(m, &a, &b, &n)) return;
    // The same main-view test the resolve uses, against the GPU-measured
    // tangents - never against a learned first sample.
    float tanH = 0.0f, tanV = 0.0f;
    if (!MeasuredCameraTangents(&tanH, &tanV) || tanH <= 1e-4f || tanV <= 1e-4f)
        return;
    const float wantA = 1.0f / tanH, wantB = 1.0f / tanV;
    if (fabsf(a - wantA) > 0.10f * wantA || fabsf(b - wantB) > 0.10f * wantB)
        return;

    const int eye = (CurrentRenderEye() == 0) ? 0 : 1;
    InterlockedIncrement(&g_camCandSeen[eye]);
    for (int i = 0; i < g_camCandN[eye]; ++i) {
        const float* c = g_camCand[eye][i].m;
        float d = 0.0f;
        for (int k = 12; k < 16; ++k) {
            const float e = fabsf(c[k] - m[k]);
            if (e > d) d = e;
        }
        if (d < 0.01f) {
            // *** KEEP THE LATEST, NOT THE FIRST. ***
            //
            // The cluster tolerance is 10 mm and the engine refines this
            // camera through the pass, so its members differ by a few mm -
            // measured as a ~5 mm error, which is invisible at 90 m and
            // several pixels at 1.8 m. Keeping the pass's FIRST write meant
            // reprojecting through a camera the GPU had already moved on
            // from; the last write before the resolve is the one the frame
            // was actually drawn with.
            ++g_camCand[eye][i].hits;
            if (d > g_camCand[eye][i].wobble) g_camCand[eye][i].wobble = d;
            memcpy(g_camCand[eye][i].m, m, 64);
            return;
        }
    }
    if (g_camCandN[eye] >= kMaxCamCand) {
        InterlockedIncrement(&g_camCandOverflow);
        return;
    }
    memcpy(g_camCand[eye][g_camCandN[eye]].m, m, 64);
    g_camCand[eye][g_camCandN[eye]].hits = 1;
    g_camCand[eye][g_camCandN[eye]].wobble = 0.0f;
    ++g_camCandN[eye];
}

bool PickCameraCandidateImpl(int eye, const float* prev, bool hasPrev, float* out,
                             CamPick* info) {
    eye = (eye == 0) ? 0 : 1;
    const int n = g_camCandN[eye];
    if (n <= 0) return false;

    // *** WEIGHT OF USE FIRST, CONTINUITY ONLY TO BREAK TIES. ***
    //
    // Continuity alone was measured to fail, and to fail permanently: given a
    // candidate that merely sits near last frame's choice, it locks on and
    // re-elects it forever. One run picked a camera written 4 times a pass
    // (and carrying no jitter at all - the main view always carries jitter)
    // while the real one, written 83 times, stood right there. The engine
    // uses the player's camera for essentially every main-pass draw, so the
    // write count separates it from the impostors by an order of magnitude.
    // Continuity then decides only among the genuinely popular ones.
    int maxHits = 0;
    for (int i = 0; i < n; ++i)
        if (g_camCand[eye][i].hits > maxHits) maxHits = g_camCand[eye][i].hits;
    const int need = (maxHits > 1) ? (maxHits / 2) : 1;

    int best = -1;
    float bestD = 0.0f;
    for (int i = 0; i < n; ++i) {
        if (g_camCand[eye][i].hits < need) continue;      // an impostor
        float d = 0.0f;
        if (hasPrev && prev) {
            for (int k = 12; k < 16; ++k) {
                const float e = fabsf(g_camCand[eye][i].m[k] - prev[k]);
                if (e > d) d = e;
            }
        }
        if (best < 0 || d < bestD - 1e-6f ||
            (fabsf(d - bestD) <= 1e-6f &&
             g_camCand[eye][i].hits > g_camCand[eye][best].hits)) {
            best = i;
            bestD = d;
        }
    }
    if (best < 0) return false;

    // How far apart the candidates were this frame - the evidence that the
    // lottery is real, and that dodging it is doing something.
    float spread = 0.0f;
    for (int i = 0; i < n; ++i) {
        float d = 0.0f;
        for (int k = 12; k < 16; ++k) {
            const float e = fabsf(g_camCand[eye][i].m[k] - g_camCand[eye][best].m[k]);
            if (e > d) d = e;
        }
        if (d > spread) spread = d;
    }

    memcpy(out, g_camCand[eye][best].m, 64);
    if (info) {
        info->candidates = n;
        info->hits = g_camCand[eye][best].hits;
        info->maxHits = maxHits;      // chosen < max means an impostor won
        info->wobble = g_camCand[eye][best].wobble;
        info->moved = hasPrev ? bestD : 0.0f;
        info->spread = spread;
    }
    g_camCandN[eye] = 0;          // consumed; the next pass starts clean
    return true;
}

void CameraCandidatesResetImpl() {
    g_camCandN[0] = 0;
    g_camCandN[1] = 0;
}

// The four offsets the shared 704-byte block is known to carry a camera clip
// matrix at. Two must agree before anything is published: +0x210 and +0x260 hold
// the PREVIOUS frame's view-projection (the TAA / motion-vector pair), so a lone
// reading can be a frame stale.
// *** TAA GHOSTING UNDER FULL-RATE STEREO. ***
//
// The engine keeps the PREVIOUS FRAME's view-projection at +0x210 and +0x260 of
// the shared 704-byte block - measured, and confirmed to be a different camera
// basis from the current pair (translation 2383.69 against 2387.14). That is
// what TAA and the motion-vector pass reproject against.
//
// Under full-rate stereo it is the wrong matrix. Both eyes render every frame,
// so the image sitting in TAA's history is not the previous FRAME - it is the
// OTHER EYE, rendered moments ago from a camera 64 mm to the side. TAA is handed
// a matrix that describes neither.
//
// The pixels are not the problem; the reprojection is. An IPD shift IS camera
// motion, which TAA already knows how to undo - so giving it the matrix of the
// render that actually produced the history should collapse the ghost to
// ordinary disocclusion.
//
// Off by default: this rewrites what every temporal and velocity pass in the
// frame reprojects against, which is a wide blast radius for a hypothesis.
float g_eyeVP[2][16] = {};
bool g_haveEyeVP[2] = {false, false};
volatile LONG g_taaPrevPatched = 0;

// *** THE SECOND HALF OF THE PER-EYE FIX, AND THE HALF THE PIXELS ALONE CANNOT
//     DELIVER. ***
//
// Owner's verdict on the pixels-only build, 2026-08-10: "it does fix the
// ghosting but it makes the world blurry and smearing and shaky a little."
// That is not a disappointment, it is the diagnosis. TAA does not simply mix the
// history in - it REPROJECTS it, fetching each pixel from where that pixel was
// in the render the history came from. The matrix it reprojects with lives at
// +0x210 / +0x260 of this block, and the engine keeps exactly ONE of them, set
// from the last render.
//
// So after the pixel substitution the two halves disagree:
//
//   eye 0's history is now eye 0 one frame ago;  the matrix still describes
//                                                eye 1 one frame ago  -> off by the IPD
//   eye 1's history is now eye 1 one frame ago;  the matrix still describes
//                                                eye 0 THIS frame     -> off by a WHOLE
//                                                                        FRAME of motion
//
// Fetching a history sample from the wrong place is blur when still and smear
// when moving, and it should be markedly worse in eye 1 than eye 0 - which is
// what "blurry and shaky" is.
//
// The fix is routing, not mathematics: keep each eye's matrix, roll it once per
// REAL FRAME, and hand each eye back its OWN previous frame. Then the pixels and
// the reprojection describe the same render.
float g_eyeVPCur[2][16] = {};      // rewritten ~45x per pass; the live one
float g_eyeVPPrev[2][16] = {};     // the SAME eye, one real frame ago
bool g_haveEyeVPCur[2] = {false, false};
bool g_haveEyeVPPrev[2] = {false, false};
volatile LONG g_taaEyePatched[2] = {};
volatile LONG g_taaEyeSeen[2] = {};
volatile LONG g_taaEyeDeferred = 0;
float g_taaShift[2] = {0.0f, 0.0f};   // how far our matrix moved the reprojection

// TWO ASSUMPTIONS THIS PATCH RESTS ON, NEITHER EVER MEASURED. They are inherited
// from the older taa_prev_from_last_render lever and they are exactly the sort
// of thing that makes a build "change nothing" for a reason nobody can see:
//
//  1. That the ~94 camera-block writes in one eye pass all carry the SAME
//     matrix. If they do not, the one we happen to store last is a lottery
//     ticket, not the main view.
//  2. That +0x210 really is a copy of an earlier +0x000. If the engine's own
//     +0x210 is already equal to the CURRENT matrix, it is not a previous frame
//     at all and writing one there is meaningless.
// *** BITWISE IDENTITY, NOT A TOLERANCE. ***
//
// The tolerance-based search matched all four references against everything: a
// relative 1e-3 on matrix elements in the thousands is looser than the distance
// between two cameras 11 ms and 64 mm apart, so it could not discriminate at all.
// These buffers hold COPIES of the same floats, so memcmp answers exactly which
// render a buffer's matrix came from - and that is the question.
struct CamSample {
    float    m[16];
    uint64_t frame;
    int      eye;
    int      seq;
};
// 384, not 12. The block is rewritten about 94 times a frame, so a twelve-entry
// ring covered a fraction of ONE frame and could never have held a matrix a whole
// frame old - which is what made every "unrecognised camera" line unreadable.
// 384 covers roughly four frames, for 24 KB.
constexpr int kCamRing = 384;
CamSample g_camRing[kCamRing] = {};
int g_camRingCount = 0;
int g_camRingNext = 0;
int g_camSeq = 0;
uint64_t g_taaFrameNo = 0;

void NoteCameraSample(const float* m, int eye) {
    if (g_camRingCount) {
        const CamSample& last = g_camRing[(g_camRingNext + kCamRing - 1) % kCamRing];
        if (memcmp(last.m, m, 64) == 0) return;      // the same write repeated
    }
    CamSample& s = g_camRing[g_camRingNext];
    memcpy(s.m, m, 64);
    s.frame = g_taaFrameNo;
    s.eye = eye;
    s.seq = ++g_camSeq;
    g_camRingNext = (g_camRingNext + 1) % kCamRing;
    if (g_camRingCount < kCamRing) ++g_camRingCount;
}

float g_passFirstVP[2][16] = {};
bool  g_havePassFirst[2] = {false, false};
volatile LONG g_camBlockDistinct[2] = {};
float g_engPrevVsCur[2] = {0.0f, 0.0f};

void PatchPreviousViewProjection(ID3D11DeviceContext* ctx, uint8_t* data, UINT bytes) {
    const bool perEye   = Cfg().per_eye_temporal_history && Cfg().per_eye_temporal_matrix;
    const bool oldLever = Cfg().taa_prev_from_last_render && !perEye;
    // taa_probe HAS to be in this gate, and leaving it out voided a second
    // search: the inner tracking was corrected to run for the probe, and this
    // line above it still returned before the tracking could be reached. Fixing
    // the second gate and not the first is not a fix.
    if ((!perEye && !oldLever && !Cfg().taa_probe) || bytes != 704) return;
    float* cur = reinterpret_cast<float*>(data + 0x000);
    float a = 0.0f, b = 0.0f, n = 0.0f;
    // Only when this really is the camera block - the same structural test the
    // measurement uses, so a shadow matrix can never be written here.
    if (!IsSharedCameraClip(cur, &a, &b, &n)) return;
    // *** PER EYE, NOT PER WRITE. ***
    //
    // The first version stored 'the previous write' and it fired 16,740 times a
    // second - about ninety times a frame, because this block is rewritten by
    // many passes. So the matrix it restored was the SAME eye from moments
    // earlier, a near-copy of the current one, and nothing changed on screen.
    // Keeping one per eye is what made it mean anything.
    const int eye = (CurrentRenderEye() == 0) ? 0 : 1;
    const int other = 1 - eye;

    // *** TRACK WHENEVER EITHER IS ON, PATCH ONLY WHEN THE FEATURE IS. ***
    //
    // The first version collected these matrices inside the feature's own branch,
    // so with the probe armed and the feature OFF there was nothing stored - and
    // the buffer search then compared every candidate against four null pointers
    // and reported "nothing recognised" fifteen times. A search with no reference
    // cannot fail to find nothing. Exactly the null result the postmortem is
    // about, one round after writing that warning down.
    const bool track = perEye || Cfg().taa_probe;
    if (track) {
        InterlockedIncrement(&g_taaEyeSeen[eye]);
        if (ctx && IsDeferredCtx(ctx)) InterlockedIncrement(&g_taaEyeDeferred);
        // Assumption 1: do all of this pass's camera blocks agree?
        if (!g_havePassFirst[eye]) {
            memcpy(g_passFirstVP[eye], cur, 64);
            g_havePassFirst[eye] = true;
        } else {
            float d = 0.0f;
            for (int i = 0; i < 16; ++i) {
                const float e = fabsf(cur[i] - g_passFirstVP[eye][i]);
                if (e > d) d = e;
            }
            if (d > 1e-4f) InterlockedIncrement(&g_camBlockDistinct[eye]);
        }
        // Assumption 2: is the engine's own +0x210 actually a PREVIOUS matrix,
        // or is it the current one? Read before we overwrite it.
        if (0x210 + 64 <= bytes) {
            const float* was = reinterpret_cast<const float*>(data + 0x210);
            float d = 0.0f;
            for (int i = 12; i < 15; ++i) {
                const float e = fabsf(was[i] - cur[i]);
                if (e > d) d = e;
            }
            g_engPrevVsCur[eye] = d;
        }
        if (perEye && g_haveEyeVPPrev[eye] && 0x210 + 64 <= bytes) {
            // Measure the size of the change BEFORE making it. A patch that
            // writes a matrix indistinguishable from the one already there
            // cannot be the cause of anything, and saying so costs three floats.
            const float* was = reinterpret_cast<const float*>(data + 0x210);
            float d = 0.0f;
            for (int i = 12; i < 15; ++i) {
                const float e = fabsf(was[i] - g_eyeVPPrev[eye][i]);
                if (e > d) d = e;
            }
            g_taaShift[eye] = d;
            memcpy(data + 0x210, g_eyeVPPrev[eye], 64);
            if (0x260 + 64 <= bytes) memcpy(data + 0x260, g_eyeVPPrev[eye], 64);
            InterlockedIncrement(&g_taaEyePatched[eye]);
        }
        memcpy(g_eyeVPCur[eye], cur, 64);
        g_haveEyeVPCur[eye] = true;
        NoteCameraSample(cur, eye);
        return;
    }

    // The older lever, unchanged: hand back the OTHER eye's matrix, which is
    // what TAA had in its history before the pixels were substituted. It is
    // mutually exclusive with the per-eye route above and is superseded by it.
    if (g_haveEyeVP[other]) {
        if (0x210 + 64 <= bytes) memcpy(data + 0x210, g_eyeVP[other], 64);
        if (0x260 + 64 <= bytes) memcpy(data + 0x260, g_eyeVP[other], 64);
        InterlockedIncrement(&g_taaPrevPatched);
    }
    memcpy(g_eyeVP[eye], cur, 64);
    g_haveEyeVP[eye] = true;
}

void InjectJitterOverlay(uint8_t* data, UINT bytes) {
    if (!Cfg().taa_jitter_inject || bytes != 704) return;
    if (g_jitRenderW < 64.0f || g_jitRenderH < 64.0f) return;   // resolve not seen yet
    float a = 0.0f, b = 0.0f, n = 0.0f;
    float* m0 = reinterpret_cast<float*>(data + 0x000);
    if (!IsSharedCameraClip(m0, &a, &b, &n)) return;
    // MAIN VIEW ONLY - the tangent agreement that guards the reprojection
    // capture. A shadow or reflection camera passing the structural test still
    // fails this, so those passes keep the engine's stock jitter and cannot
    // start shimmering because of us.
    float tanH = 0.0f, tanV = 0.0f;
    if (!MeasuredCameraTangents(&tanH, &tanV) || tanH <= 1e-4f || tanV <= 1e-4f)
        return;
    const float wantA = 1.0f / tanH, wantB = 1.0f / tanV;
    if (fabsf(a - wantA) > 0.10f * wantA || fabsf(b - wantB) > 0.10f * wantB)
        return;

    const int k = (int)(g_jitPhase & 7);
    // Pixel offsets (MV convention, y down) to NDC: x scales by 2/w, y by 2/h
    // with the sign flipped. Then into the matrix exactly the way the engine's
    // own jitter sits there: m[12] += j_ndc * m[15] (measured model, validated
    // by the stick-phase probe run - 0.33 deg -> 8.0 px against 7.5 predicted).
    const float s = Cfg().taa_jitter_inject_scale;
    const float oxNdc =  s * kJitOvlX[k] / (0.5f * g_jitRenderW);
    const float oyNdc = -s * kJitOvlY[k] / (0.5f * g_jitRenderH);
    // *** THE EXACT FORM, VALID FOR EVERY CONVENTION IN THE BLOCK. ***
    //
    // A screen-space offset j is a post-multiply by an NDC translation:
    // clip.x' = clip.x + j * clip.w. In matrix terms that is column0 += j *
    // column3 - ALL four rows, not just the translation one. The first build
    // used the world-scale shorthand (m[12] += j*m[15]) and SKIPPED any camera
    // with a small m[15] - and the skip counter then showed exactly TWO of the
    // four cameras in every block skipped: the block stores two world-scale
    // and two CAMERA-RELATIVE cameras (m[15] ~ 0 - this project measured both
    // conventions long ago), and for the camera-relative pair the jitter lives
    // in rows 0..2, which the shorthand never touched. Passes reading those
    // copies rendered WITHOUT the overlay while DLSS was told it was there -
    // the measured 24,440 skips/s against 12,220 blocks/s, and the prime
    // suspect for the vegetation flicker. The exact form needs no convention,
    // no m[15] model, and no skips.
    static const UINT kOff[4] = {0x000, 0x1D0, 0x210, 0x260};
    for (int i = 0; i < 4; ++i) {
        if (kOff[i] + 64 > bytes) continue;
        float* m = reinterpret_cast<float*>(data + kOff[i]);
        float aa = 0.0f, bb = 0.0f, nn = 0.0f;
        if (i != 0 && !IsSharedCameraClip(m, &aa, &bb, &nn)) continue;
        const float am15 = fabsf(m[15]);
        if (i == 0) {
            if (am15 < g_jitM15Min) g_jitM15Min = am15;
            if (am15 > g_jitM15Max) g_jitM15Max = am15;
        }
        for (int r = 0; r < 4; ++r) {
            m[r * 4 + 0] += oxNdc * m[r * 4 + 3];
            m[r * 4 + 1] += oyNdc * m[r * 4 + 3];
        }
    }
    InterlockedIncrement(&g_jitInjectedBlocks);
}

// *** WHERE THE RESOLVE'S OWN REPROJECTION LIVES. ***
//
// Measured 2026-08-11: the TAA resolve binds five pixel-shader constant buffers -
// 1584, 384, 192, 160 and 64 bytes - and NOT ONE of them is the shared 704-byte
// camera block. So the matrix patched at Unmap cannot reach this pass, however
// many blocks it patches, and the owner's verdict ("re-aim only sharpens some
// objects") is exactly what a patch landing on the velocity pass and nothing
// else looks like.
//
// This searches those buffers for a 4x4 we ALREADY KNOW - the same technique
// that cracked the weapon constants: do not read shader code, look for numbers
// you can recognise. Transposed as well as straight, because HLSL is column-major
// by default and engines routinely transpose on upload; not checking that would
// produce a confident "not found" for a matrix sitting right there.
bool LooksLikeReprojection(const float* m) {
    float maxOff = 0.0f, maxDiagErr = 0.0f;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float v = m[r * 4 + c];
            if (r == c) {
                const float e = fabsf(v - 1.0f);
                if (e > maxDiagErr) maxDiagErr = e;
            } else {
                const float a = fabsf(v);
                if (a > maxOff) maxOff = a;
            }
        }
    }
    // Exact identity is padding, and there is a lot of it. Near identity with
    // real off-diagonal terms is a camera that moved slightly.
    return maxDiagErr < 0.05f && maxOff < 0.10f && maxOff > 1e-6f;
}

bool MatrixMatches(const float* a, const float* b, bool transposed) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const float x = a[r * 4 + c];
            const float y = transposed ? b[c * 4 + r] : b[r * 4 + c];
            const float scale = fabsf(x) > 1.0f ? fabsf(x) : 1.0f;
            if (fabsf(x - y) > 1e-3f * scale) return false;
        }
    }
    return true;
}

// *** IS +0x210 THE PREVIOUS FRAME'S CAMERA, OR SOMETHING ELSE ENTIRELY? ***
//
// The whole matrix half of this feature rests on a sentence in the doc: that
// +0x210 and +0x260 hold "the PREVIOUS frame's view-projection". Nobody has ever
// tested it. The scan at the velocity pass says those three slots hold cameras
// that match NOTHING in the sample ring - but the ring holds the last twelve
// distinct writes of +0x000, which at ~94 writes a frame is a fraction of one
// frame, so it could never have contained a matrix a whole frame old. That is a
// limitation of the instrument, not an answer.
//
// This is the apples-to-apples test: at the VELOCITY DRAW - one known point in
// the frame, where the bound block is certainly the main view's - remember
// +0x000, and next frame ask whether +0x1D0 / +0x210 / +0x260 are bitwise equal
// to it. Same offset, same pass, one frame apart.
float g_velCur[2][16] = {};
float g_velPrev[2][16] = {};
bool  g_haveVelCur[2] = {false, false};
bool  g_haveVelPrev[2] = {false, false};
volatile LONG g_velSeen[2] = {};
volatile LONG g_velHit[3][2] = {};      // [which offset][eye]
volatile LONG g_velRingHit[3][2] = {};  // matched ANY sampled camera, any frame
long g_velRingAge[3][2] = {};           // how old that one was, in frames
const UINT kVelOffsets[3] = {0x1D0, 0x210, 0x260};

void NoteVelocityPass(ID3D11DeviceContext* ctx, int eye) {
    if (!ctx) return;
    ID3D11Buffer* vs0 = nullptr;
    ctx->VSGetConstantBuffers(0, 1, &vs0);
    if (!vs0) return;
    D3D11_BUFFER_DESC bd{};
    vs0->GetDesc(&bd);
    if (bd.ByteWidth == 704) {
        const CBSnapshot* sn = FindSnapshot(vs0);
        if (sn && sn->res == vs0 && sn->bytes >= 0x2A0) {
            InterlockedIncrement(&g_velSeen[eye]);
            for (int k = 0; k < 3; ++k) {
                // First the exact question: this pass's own +0x000, one frame ago.
                if (g_haveVelPrev[eye] &&
                    memcmp(sn->data + kVelOffsets[k], g_velPrev[eye], 64) == 0) {
                    InterlockedIncrement(&g_velHit[k][eye]);
                    continue;
                }
                // Then the wider one: ANY main-view matrix written in the last few
                // frames, at any point in the frame. The camera is rewritten ~94
                // times a frame, so "the previous frame's camera" need not be the
                // one this pass happened to see last time round.
                for (int j = 0; j < g_camRingCount; ++j) {
                    if (memcmp(sn->data + kVelOffsets[k], g_camRing[j].m, 64) != 0)
                        continue;
                    InterlockedIncrement(&g_velRingHit[k][eye]);
                    g_velRingAge[k][eye] =
                        (long)(g_taaFrameNo - g_camRing[j].frame);
                    break;
                }
            }
            memcpy(g_velCur[eye], sn->data, 64);
            g_haveVelCur[eye] = true;
        }
    }
    vs0->Release();
}

void ScanResolveConstants(ID3D11DeviceContext* ctx, int eye, const char* what) {
    if (!ctx) return;
    struct Known { const char* name; const float* m; };
    const Known known[4] = {
        {"THIS eye's CURRENT matrix",       g_haveEyeVPCur[eye]      ? g_eyeVPCur[eye]      : nullptr},
        {"THIS eye's PREVIOUS frame",       g_haveEyeVPPrev[eye]     ? g_eyeVPPrev[eye]     : nullptr},
        {"the OTHER eye's CURRENT matrix",  g_haveEyeVPCur[1 - eye]  ? g_eyeVPCur[1 - eye]  : nullptr},
        {"the OTHER eye's PREVIOUS frame",  g_haveEyeVPPrev[1 - eye] ? g_eyeVPPrev[1 - eye] : nullptr},
    };

    // *** A SEARCH WITH NO REFERENCE MUST SAY SO, NOT REPORT ZERO. ***
    //
    // Twice now this has run with all four references null and printed "nothing
    // recognised", which reads exactly like a finding and is not one. The
    // instrument now refuses to produce that sentence.
    int refs = 0;
    for (int k = 0; k < 4; ++k) if (known[k].m) ++refs;
    if (refs == 0) {
        COTW_LOG("[taa] *** SEARCH VOID - no reference matrices captured yet. "
                 "Nothing was compared against anything, so a result of zero here "
                 "means NOTHING. The 704-byte camera block has not been seen "
                 "through Unmap since the probe was armed. ***");
        return;
    }
    for (int k = 0; k < 4; ++k) {
        if (!known[k].m) continue;
        COTW_LOG("[taa] reference: %-28s  row0 = %.4f %.4f %.4f %.4f",
                 known[k].name, known[k].m[0], known[k].m[1], known[k].m[2],
                 known[k].m[3]);
    }

    // *** ALL FOURTEEN SLOTS OF BOTH STAGES. ***
    //
    // The first version of this read PSGetConstantBuffers(0, 6) and concluded the
    // camera block was not bound to the resolve. Replaying the capture's chunk
    // stream showed the truth: the resolve has NINE pixel-shader constant buffers
    // and fourteen vertex ones, and the 704-byte camera block is VS cb0. Six of
    // twenty-three is not a survey.
    ID3D11Buffer* pscb[14] = {};
    ID3D11Buffer* vscb[14] = {};
    ctx->PSGetConstantBuffers(0, 14, pscb);
    ctx->VSGetConstantBuffers(0, 14, vscb);
    COTW_LOG("[taa] --- every constant buffer bound to the %s (eye %d) ---", what, eye);
    for (int stage = 0; stage < 2; ++stage) {
        ID3D11Buffer** arr = stage ? vscb : pscb;
        const char* tag = stage ? "VS" : "PS";
        for (int slot = 0; slot < 14; ++slot) {
            if (!arr[slot]) continue;
            D3D11_BUFFER_DESC bd{};
            arr[slot]->GetDesc(&bd);
            // Does this buffer hold a camera, and how old is it? Bitwise, against
            // the ring - the one question that names the target.
            const CBSnapshot* sn = FindSnapshot(arr[slot]);
            const bool have = sn && sn->res == arr[slot] && sn->bytes >= 64;
            COTW_LOG("[taa]   %s cb%-2d  %p  %5u bytes   %s", tag, slot,
                     (void*)arr[slot], bd.ByteWidth,
                     have ? "" : "NO SNAPSHOT - contents invisible");
            if (!have) continue;

            // *** EVERY CAMERA IN THE BUFFER, NOT THE FIRST ONE. ***
            //
            // The first version stopped at the first hit, so the 704-byte block
            // reported its CURRENT matrix at +0x000 and never reached +0x210 -
            // which is precisely where the previous one lives. It then printed
            // that as a finding. Stopping early and reporting the result as
            // complete is the same failure that has now voided three searches.
            UINT n = sn->bytes < bd.ByteWidth ? sn->bytes : bd.ByteWidth;
            int cams = 0;
            for (UINT off = 0; off + 64 <= n; off += 16) {
                const float* cand = reinterpret_cast<const float*>(sn->data + off);
                bool named = false;
                for (int j = 0; j < g_camRingCount; ++j) {
                    if (memcmp(cand, g_camRing[j].m, 64) != 0) continue;
                    const unsigned long long age =
                        (unsigned long long)(g_taaFrameNo - g_camRing[j].frame);
                    COTW_LOG("[taa]        +0x%03X  camera #%d, eye %d, %llu frame(s) "
                             "old%s", off, g_camRing[j].seq, g_camRing[j].eye, age,
                             age ? "   <<< A PREVIOUS RENDER" : "   (the current one)");
                    named = true; ++cams;
                    break;
                }
                if (named) continue;
                // Structurally a camera but matching nothing we have sampled -
                // e.g. a render this ring never saw. Still worth naming.
                float a = 0.0f, b = 0.0f, nr = 0.0f;
                if (IsSharedCameraClip(cand, &a, &b, &nr)) {
                    COTW_LOG("[taa]        +0x%03X  an UNRECOGNISED camera "
                             "(a=%.4f b=%.4f near=%.5f) - not one of the %d we "
                             "sampled, so it belongs to some other render",
                             off, a, b, nr, g_camRingCount);
                    ++cams;
                }
            }
            if (!cams) COTW_LOG("[taa]        (no camera matrix anywhere in it)");
        }
    }
    for (int i = 0; i < 14; ++i) {
        if (pscb[i]) pscb[i]->Release();
        if (vscb[i]) vscb[i]->Release();
    }
    COTW_LOG("[taa] --- a slot marked A PREVIOUS RENDER is the reprojection the "
             "velocity pass uses, and the one the per-eye fix must write ---");

    ID3D11Buffer* cbs[6] = {};
    ctx->PSGetConstantBuffers(0, 6, cbs);
    COTW_LOG("[taa] --- searching the resolve's own constant buffers (eye %d, %d "
             "reference matrices) ---", eye, refs);
    for (int s = 0; s < 6; ++s) {
        if (!cbs[s]) continue;
        D3D11_BUFFER_DESC bd{};
        cbs[s]->GetDesc(&bd);
        UINT bytes = 0;
        const CBSnapshot* sn = FindSnapshot(cbs[s]);
        const uint8_t* data = nullptr;
        if (sn && sn->res == cbs[s]) { data = sn->data; bytes = sn->bytes; }
        if (!data || bytes < 64) {
            COTW_LOG("[taa]   cb%d  %u bytes - NO SNAPSHOT, so its contents are "
                     "invisible. It is written by neither Map nor UpdateSubresource, "
                     "or it was evicted.", s, bd.ByteWidth);
            continue;
        }
        if (bytes > bd.ByteWidth) bytes = bd.ByteWidth;

        // *** JUST LOOK AT IT. ***
        //
        // Three rounds of heuristics have each answered "not found", and two of
        // those answers were void. The buffers are 64 to 1584 bytes; printing the
        // head of each one costs a few lines a second and lets the contents be
        // read directly instead of guessed at. cb4 is exactly 64 bytes - one 4x4 -
        // and is the strongest candidate in the list for a reprojection matrix.
        const UINT show = bytes < 128u ? bytes : 128u;   // up to 32 floats
        for (UINT off = 0; off + 16 <= show; off += 16) {
            const float* v = reinterpret_cast<const float*>(data + off);
            COTW_LOG("[taa]   cb%d +0x%03X  %12.5f %12.5f %12.5f %12.5f",
                     s, off, v[0], v[1], v[2], v[3]);
        }

        int hits = 0;
        for (UINT off = 0; off + 64 <= bytes; off += 16) {
            const float* cand = reinterpret_cast<const float*>(data + off);
            for (int k = 0; k < 4; ++k) {
                if (!known[k].m) continue;
                for (int t = 0; t < 2; ++t) {
                    if (!MatrixMatches(cand, known[k].m, t != 0)) continue;
                    COTW_LOG("[taa]   cb%d + 0x%03X  ==  %s%s", s, off, known[k].name,
                             t ? "  (TRANSPOSED)" : "");
                    ++hits;
                }
            }
            float a = 0.0f, b = 0.0f, n = 0.0f;
            if (IsSharedCameraClip(cand, &a, &b, &n))
                COTW_LOG("[taa]   cb%d + 0x%03X  looks like a clip matrix "
                         "(a=%.4f b=%.4f near=%.5f) - unrecognised", s, off, a, b, n);
            // A clip -> PREVIOUS-clip reprojection matrix is near identity: the
            // camera moved a little between two renders. That encoding would
            // match none of the four references and pass no clip test, which is
            // exactly how it could hide from all three previous searches.
            if (LooksLikeReprojection(cand))
                COTW_LOG("[taa]   cb%d + 0x%03X  NEAR-IDENTITY 4x4 - this is what a "
                         "frame-to-frame reprojection matrix looks like", s, off);
        }
        if (!hits)
            COTW_LOG("[taa]   cb%d  %u bytes - nothing recognised", s, bd.ByteWidth);
    }
    for (int s = 0; s < 6; ++s) if (cbs[s]) cbs[s]->Release();

    // *** AND NOW EVERY OTHER CONSTANT BUFFER IN THE FRAME. ***
    //
    // If the resolve carries no matrix it is reprojecting from the MOTION VECTOR
    // TEXTURE, and the thing to correct is whatever camera the velocity pass
    // used - which lives in some other buffer. Rather than hunt that pass draw
    // by draw, sweep the snapshot table: it already holds every constant buffer
    // the engine wrote this frame, and a camera matrix is recognisable on sight.
    //
    // 512 slots x 8 KB at 16-byte steps, once a second, with an early-exit
    // compare. Only while the probe is armed.
    COTW_LOG("[taa] --- sweeping every constant buffer for a camera matrix, BY "
             "BITWISE IDENTITY (eye %d, %d samples in the ring, frame %llu) ---",
             eye, g_camRingCount, (unsigned long long)g_taaFrameNo);
    int found = 0, scanned = 0;
    for (int i = 0; i < kMaxCBSnapshots && found < 48; ++i) {
        const CBSnapshot& sn = g_snaps[i];
        ID3D11Resource* r = sn.res;
        if (!r || sn.bytes < 64) continue;
        ++scanned;
        const UINT bytes = sn.bytes;
        for (UINT off = 0; off + 64 <= bytes && found < 48; off += 16) {
            const float* cand = reinterpret_cast<const float*>(sn.data + off);
            for (int j = 0; j < g_camRingCount; ++j) {
                const CamSample& cs = g_camRing[j];
                if (memcmp(cand, cs.m, 64) != 0) continue;
                const unsigned long long age =
                    (unsigned long long)(g_taaFrameNo - cs.frame);
                COTW_LOG("[taa]   buffer %p (%4u B) + 0x%03X  ==  camera #%d, eye %d, "
                         "%llu frame(s) old%s", (void*)r, bytes, off, cs.seq, cs.eye,
                         age, age > 0 ? "   <-- A PREVIOUS RENDER" : "");
                ++found;
            }
        }
    }
    COTW_LOG("[taa] --- swept %d buffer(s), %d exact hit(s). Any buffer holding a "
             "camera that is 1+ frames old, or the OTHER eye's, is a 'previous "
             "render' slot - that is what the velocity pass reprojects against and "
             "what has to become per-eye. ---", scanned, found);
}

// Once per REAL frame, from CBScanReport - the only true frame boundary this
// file has. Each eye's live matrix becomes its previous-frame matrix, so next
// frame it reprojects against the render that actually produced the pixels now
// sitting in its private history texture.
void TaaRollEyeMatrices() {
    if (!((Cfg().per_eye_temporal_history && Cfg().per_eye_temporal_matrix) ||
          Cfg().taa_probe)) return;
    ++g_taaFrameNo;
    for (int e = 0; e < 2; ++e) {
        if (g_haveVelCur[e]) {
            memcpy(g_velPrev[e], g_velCur[e], 64);
            g_haveVelPrev[e] = true;
        }
    }
    for (int e = 0; e < 2; ++e) {
        if (!g_haveEyeVPCur[e]) continue;
        memcpy(g_eyeVPPrev[e], g_eyeVPCur[e], 64);
        g_haveEyeVPPrev[e] = true;
        g_havePassFirst[e] = false;   // each pass is compared against its own first block
    }
}

void ScanForCameraProjection(const uint8_t* data, UINT bytes) {
    if (!Cfg().tier1_measure_fov || bytes != 704) return;
    static const UINT kOff[4] = {0x000, 0x1d0, 0x210, 0x260};
    float a[4], b[4], n[4];
    int hits = 0;
    for (int i = 0; i < 4; ++i) {
        if (kOff[i] + 64 > bytes) continue;
        if (IsSharedCameraClip((const float*)(data + kOff[i]), &a[hits], &b[hits],
                               &n[hits])) {
            ++hits;
        }
    }
    if (!hits) return;
    InterlockedIncrement(&g_projSeen);
    // Agreement between two offsets, not a single reading.
    for (int i = 0; i < hits; ++i) {
        for (int j = i + 1; j < hits; ++j) {
            if (fabsf(a[i] - a[j]) < 1e-3f && fabsf(b[i] - b[j]) < 1e-3f) {
                g_projA = a[i];
                g_projB = b[i];
                g_projN = n[i];
                InterlockedIncrement(&g_projAgreed);
                return;
            }
        }
    }
}

void STDMETHODCALLTYPE Hook_Unmap(ID3D11DeviceContext* ctx, ID3D11Resource* res, UINT sub) {
    // Read what the game just wrote, BEFORE handing the buffer back.
    if (t_mapped == res && t_data && t_size) {
        // Snapshot the head of every constant buffer big enough to be
        // InstanceConsts. Cheap - one 256-byte memcpy - and it means that when a
        // weapon draw arrives we already hold the constants it is about to use.
        // Stage 0 measurement, before anything else touches the data.
        ScanForCameraProjection(static_cast<const uint8_t*>(t_data), t_size);
        // The jitter overlay goes in BEFORE the trackers and the snapshot table,
        // so everything downstream - the engine's raster, the reprojection
        // capture, the probes - sees one consistent, jittered truth.
        InjectJitterOverlay(static_cast<uint8_t*>(t_data), t_size);
        // Every main-view camera the engine writes this pass becomes a
        // candidate for the resolve to choose from (taa_camera_lock v2).
        NoteCameraCandidateImpl(ctx, static_cast<const uint8_t*>(t_data), t_size);
        PatchPreviousViewProjection(ctx, static_cast<uint8_t*>(t_data), t_size);

        // hud_probe reads this table too - see the note in Hook_Map. So does the
        // TAA probe: the resolve's own constant buffers have to be in this table
        // before anything can be looked for inside them.
        if (SnapshotsWanted() && t_size >= kMinSnapshotBytes) {
            const UINT want = t_size < kPoolSnapshot ? t_size : kPoolSnapshot;
            CBSnapshot* s = SlotForSnapshot(res);
            __try {
                memcpy(s->data, t_data, want);
                s->bytes = want;
                s->res = res;          // published last
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                s->bytes = 0;
                s->res = nullptr;
            }
        }

        // *** SHIFT THE GLASS WHERE IT IS WRITTEN. ***
        //
        // Scope and binocular lenses are blended, carry no first-person stencil
        // tag, and are drawn on a different deferred context from the body - so
        // no amount of work at DRAW time could reach them. Eight attempts said
        // so: a proximity window (their thread never sees a tagged draw), a
        // thread-local ring, thread-local snapshots, a bind-range fix.
        //
        // But the lens holds its OWN COPY of the body's clip matrix - a
        // different block, byte-for-byte the same value, because it is the same
        // object at the same instant. So it can be found by VALUE at the moment
        // the game writes it, with no draw-time identification at all.
        //
        // Cost is controlled: a uint64 prefilter on the first two floats, a
        // 16-byte stride, a capped scan length, and only while glass is on and
        // the ring has entries.
        if (Cfg().weapon_3d && Cfg().weapon_3d_glass > 0 && g_clipRingCount > 0 &&
            t_size >= 64) {
            PatchMatchingClipMatrices(static_cast<uint8_t*>(t_data), t_size, res);
        }
        __try {
            if (Cfg().weapon_cb_dump) {
                DumpWeaponBuffer(ctx, static_cast<ID3D11Buffer*>(t_mapped),
                                 static_cast<const float*>(t_data), t_size);
            }
            if (Cfg().weapon_cb_scan) {
                ScanProjections(static_cast<const float*>(t_data), t_size);
                ScanCloseModels(static_cast<const float*>(t_data), t_size);
                ScanBuffer(static_cast<const float*>(t_data), t_size);
            }

            // *** THE PATCH. ***
            // The buffer is still mapped and still writable until Unmap returns,
            // so this is the moment to shift the weapon. The counter says
            // whether it ran, because the previous build's version of this block
            // never made it into the file at all and reported as "no axis
            // works".
            if (WeaponPatchEnabled() &&
                IsWeaponBuffer(t_size, (UINT)Cfg().weapon_cb_offset)) {
                float* m = reinterpret_cast<float*>(static_cast<char*>(t_data) +
                                                    Cfg().weapon_cb_offset);
                float d = 0.0f;
                if (LooksLikeCloseModel(m, &d)) {
                    PatchWeaponMatrix(m);
                    InterlockedIncrement(&g_patchViaMap);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    t_mapped = nullptr;
    if (auto orig = (PFN_Unmap)g_oUnmap.For(ctx)) orig(ctx, res, sub);
}

}  // namespace

// Answered by the optic's own lens-mask draw, latched one frame behind so it is
// stable across every draw of a frame. Read by the aim-steadying code as well.
bool PlayerIsScoped() { return g_scopedNow != 0; }

// Installed as EARLY as possible - from the probe device inside
// InstallRenderHooks, not from the game's context at first Present.
//
// All D3D11 devices share one vtable, so the throwaway probe device is enough to
// hook shader creation for the whole process. That matters: hooking at first
// Present meant shaders created during the preceding startup were never
// fingerprinted, and the three the player marked in Shader Toggler were among
// the ones missed - 298 were captured, none of them the weapon's.
// The device methods that hand out contexts. Hooking these is airtight in a way
// that hooking "the context" never was: the device hooks are already proven to
// see everything (1223 of 1226 pixel shaders), and every context the game can
// draw on has to come out of one of these calls.
using PFN_GetImmediateContext = void(STDMETHODCALLTYPE*)(ID3D11Device*,
                                                         ID3D11DeviceContext**);
using PFN_CreateDeferredContext = HRESULT(STDMETHODCALLTYPE*)(ID3D11Device*, UINT,
                                                              ID3D11DeviceContext**);
PFN_GetImmediateContext   o_GetImmediateContext = nullptr;
PFN_CreateDeferredContext o_CreateDeferredContext = nullptr;
constexpr int kSlotGetImmediateContext   = 40;
constexpr int kSlotCreateDeferredContext = 27;

void STDMETHODCALLTYPE Hook_GetImmediateContext(ID3D11Device* dev,
                                                ID3D11DeviceContext** out) {
    o_GetImmediateContext(dev, out);
    if (out && *out) NoteContextVTable(*out, "immediate context (from the device)");
}

HRESULT STDMETHODCALLTYPE Hook_CreateDeferredContext(ID3D11Device* dev, UINT flags,
                                                     ID3D11DeviceContext** out) {
    const HRESULT hr = o_CreateDeferredContext(dev, flags, out);
    if (SUCCEEDED(hr) && out && *out) {
        // *** HOOK IT HERE, NOT AT PRESENT. ***
        //
        // Recording the vtable and hooking it later is the whole problem: by
        // Present the engine has been rendering on this context for a second,
        // and MinHook then rewrites DrawIndexed's prologue underneath a live
        // renderer. Right here there is nothing to race - the context was made
        // one instruction ago and the engine has not used it yet - and an atomic
        // vtable write cannot catch anyone mid-call regardless.
        //
        // And this patches the vtable of the ENGINE'S OWN context. The earlier
        // vtable attempt hooked a deferred context WE created, which the engine
        // may not share, and that is why it saw less of the frame rather than
        // more (0.4% -> 0.2%, weapon 11% -> 0%).
        if (Cfg().hook_contexts_early) {
            HookContextVTableAtomic(*reinterpret_cast<void***>(*out),
                                    "deferred context, at creation");
        }
        NoteContextVTable(*out, "deferred context (from the device)");
    }
    return hr;
}

bool InstallShaderCreationHooks(ID3D11Device* dev) {
    static bool done = false;
    if (done || !dev) return done;
    void** dvt = *reinterpret_cast<void***>(dev);
    g_hookedDevice = dev;
    COTW_LOG("[dev] hooking shader creation on device %p", (void*)dev);

    MH_STATUS cs = MH_CreateHook(dvt[kSlotGetImmediateContext],
                                 &Hook_GetImmediateContext,
                                 (void**)&o_GetImmediateContext);
    if (cs == MH_OK || cs == MH_ERROR_ALREADY_CREATED) {
        MH_EnableHook(dvt[kSlotGetImmediateContext]);
    } else {
        COTW_LOG("[dev] GetImmediateContext hook failed (%d)", (int)cs);
    }
    // *** NEVER MinHOOK OUR OWN DETOUR. ***
    //
    // When hook_contexts_early patched this slot at the device's birth, the
    // vtable entry IS Hook_CreateDeferredContext. MinHooking it with itself as
    // the detour makes o_CreateDeferredContext point back into our own hook -
    // calling it then recurses forever, which is the silent black-screen HANG
    // (not crash) at startup: the log's last line was the context hook directly
    // before this call.
    if (dvt[kSlotCreateDeferredContext] == (void*)&Hook_CreateDeferredContext) {
        COTW_LOG("[dev] CreateDeferredContext already vtable-patched at birth - "
                 "skipping the MinHook that would have hooked our own detour");
    } else {
        cs = MH_CreateHook(dvt[kSlotCreateDeferredContext],
                           &Hook_CreateDeferredContext,
                           (void**)&o_CreateDeferredContext);
        if (cs == MH_OK || cs == MH_ERROR_ALREADY_CREATED) {
            MH_EnableHook(dvt[kSlotCreateDeferredContext]);
        } else {
            COTW_LOG("[dev] CreateDeferredContext hook failed (%d)", (int)cs);
        }
    }

    // The 11.1 interface may be a separate object with its own vtable. If the
    // game asks for its contexts through ID3D11Device1 and that is a tear-off,
    // every hook above sits on an interface the renderer never touches - which
    // would look exactly like what we have been seeing.
    ID3D11Device1* d1 = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) && d1) {
        void** d1vt = *reinterpret_cast<void***>(d1);
        COTW_LOG("[dev] ID3D11Device1 is %s object (device %p / device1 %p, "
                 "vtable %p / %p)",
                 (void*)d1 == (void*)dev ? "the SAME" : "a SEPARATE",
                 (void*)dev, (void*)d1, (void*)dvt, (void*)d1vt);
        // ID3D11Device1 adds GetImmediateContext1 at 43 and
        // CreateDeferredContext1 at 44, after the base interface's 43 methods.
        constexpr int kSlotGetImmediateContext1   = 43;
        constexpr int kSlotCreateDeferredContext1 = 44;
        static PFN_GetImmediateContext   o_GetImm1 = nullptr;
        static PFN_CreateDeferredContext o_CreateDef1 = nullptr;
        struct Local {
            static void STDMETHODCALLTYPE GetImm1(ID3D11Device* d,
                                                  ID3D11DeviceContext** out) {
                o_GetImm1(d, out);
                if (out && *out) NoteContextVTable(*out, "immediate context (11.1)");
            }
            static HRESULT STDMETHODCALLTYPE CreateDef1(ID3D11Device* d, UINT f,
                                                        ID3D11DeviceContext** out) {
                const HRESULT h = o_CreateDef1(d, f, out);
                if (SUCCEEDED(h) && out && *out) {
                    NoteContextVTable(*out, "deferred context (11.1)");
                }
                return h;
            }
        };
        cs = MH_CreateHook(d1vt[kSlotGetImmediateContext1], &Local::GetImm1,
                           (void**)&o_GetImm1);
        if (cs == MH_OK || cs == MH_ERROR_ALREADY_CREATED) {
            MH_EnableHook(d1vt[kSlotGetImmediateContext1]);
        }
        cs = MH_CreateHook(d1vt[kSlotCreateDeferredContext1], &Local::CreateDef1,
                           (void**)&o_CreateDef1);
        if (cs == MH_OK || cs == MH_ERROR_ALREADY_CREATED) {
            MH_EnableHook(d1vt[kSlotCreateDeferredContext1]);
        }
        // And whatever context it is currently handing out.
        ID3D11DeviceContext1* c1 = nullptr;
        d1->GetImmediateContext1(&c1);
        if (c1) { NoteContextVTable(c1, "immediate context (11.1, at install)"); c1->Release(); }
        d1->Release();
    }

    MH_STATUS ds = MH_CreateHook(dvt[kSlotCreateVertexShader], &Hook_CreateVertexShader,
                                 (void**)&o_CreateVertexShader);
    if ((ds == MH_OK || ds == MH_ERROR_ALREADY_CREATED) &&
        MH_EnableHook(dvt[kSlotCreateVertexShader]) == MH_OK) {
        done = true;
    }
    ds = MH_CreateHook(dvt[kSlotCreatePixelShader], &Hook_CreatePixelShader,
                       (void**)&o_CreatePixelShader);
    if (ds == MH_OK || ds == MH_ERROR_ALREADY_CREATED) {
        MH_EnableHook(dvt[kSlotCreatePixelShader]);
    }
    if (done) {
        // "From now on" is the whole limitation, and it is why the viewmodel was
        // never matched: the game's shader library is already built by this
        // point, so the three weapon shaders are created before this hook exists
        // and can never be fingerprinted. 101 seen against 2,039 used per frame.
        // The index-count seed below is what makes the gun findable regardless.
        COTW_LOG("[cb] shader fingerprinting installed EARLY - every shader the game "
                 "creates FROM NOW ON is seen (shaders made before this are "
                 "unfingerprintable: D3D11 cannot hand back bytecode)");
        SeedWeaponIndexCounts();
    }
    return done;
}

bool MeasuredCameraTangents(float* tanH, float* tanV) {
    const float a = g_projA, b = g_projB;
    if (!(a > 0.01f) || !(b > 0.01f)) return false;
    if (tanH) *tanH = 1.0f / a;
    if (tanV) *tanV = 1.0f / b;
    return true;
}

void NoteCameraCandidate(ID3D11DeviceContext* ctx, const uint8_t* data,
                         uint32_t bytes) {
    NoteCameraCandidateImpl(ctx, data, bytes);
}

bool PickCameraCandidate(int eye, const float* prev, bool hasPrev, float* out,
                         CamPick* info) {
    return PickCameraCandidateImpl(eye, prev, hasPrev, out, info);
}

void CameraCandidatesReset() { CameraCandidatesResetImpl(); }

long CameraCandidateOverflows() {
    return (long)InterlockedExchange(&g_camCandOverflow, 0);
}

void TaaPublishRenderSize(unsigned w, unsigned h) {
    g_jitRenderW = (float)w;
    g_jitRenderH = (float)h;
}

void JitterOverlayAdvance() {
    InterlockedIncrement(&g_jitPhase);
}

bool JitterOverlayNow(float* px, float* py) {
    if (!Cfg().taa_jitter_inject) { *px = 0.0f; *py = 0.0f; return false; }
    const int k = (int)(g_jitPhase & 7);
    const float s = Cfg().taa_jitter_inject_scale;
    *px = s * kJitOvlX[k];
    *py = s * kJitOvlY[k];
    return true;
}

long JitterOverlayInjectedLastSecond() {
    return (long)InterlockedExchange(&g_jitInjectedBlocks, 0);
}

void JitterOverlayM15Stats(float* mn, float* mx, long* skipped) {
    *mn = (g_jitM15Min > 1e29f) ? 0.0f : g_jitM15Min;
    *mx = g_jitM15Max;
    *skipped = (long)InterlockedExchange(&g_jitSkippedSmallM15, 0);
    g_jitM15Min = 1e30f;
    g_jitM15Max = 0.0f;
}

void CBScanResolveConstants(ID3D11DeviceContext* ctx, int eye, const char* what) {
    ScanResolveConstants(ctx, eye, what);
}

void CBNoteVelocityPass(ID3D11DeviceContext* ctx, int eye) {
    NoteVelocityPass(ctx, eye);
}

bool CBIsCameraClip(const float* m, float* aOut, float* bOut, float* nOut) {
    float a = 0.0f, b = 0.0f, n = 0.0f;
    if (!IsSharedCameraClip(m, &a, &b, &n)) return false;
    if (aOut) *aOut = a;
    if (bOut) *bOut = b;
    if (nOut) *nOut = n;
    return true;
}

bool CBSnapshotFor(ID3D11Resource* res, const uint8_t** dataOut,
                   uint32_t* bytesOut) {
    const CBSnapshot* sn = FindSnapshot(res);
    if (!sn || sn->res != res || sn->bytes < 64) return false;
    *dataOut = sn->data;
    *bytesOut = sn->bytes;
    return true;
}

void CBReportVelocityMatch() {
    const LONG s0 = InterlockedExchange(&g_velSeen[0], 0);
    const LONG s1 = InterlockedExchange(&g_velSeen[1], 0);
    if (!s0 && !s1) return;
    COTW_LOG("[taa] IS +0x210 THE PREVIOUS FRAME? velocity pass seen %ld/%ld times "
             "(eye 0 / eye 1) since the last line. Of those, bitwise equal to the "
             "SAME slot's +0x000 one frame earlier:", s0, s1);
    bool any = false, anyRing = false;
    for (int k = 0; k < 3; ++k) {
        const LONG h0 = InterlockedExchange(&g_velHit[k][0], 0);
        const LONG h1 = InterlockedExchange(&g_velHit[k][1], 0);
        const LONG r0 = InterlockedExchange(&g_velRingHit[k][0], 0);
        const LONG r1 = InterlockedExchange(&g_velRingHit[k][1], 0);
        if (h0 || h1) any = true;
        if (r0 || r1) anyRing = true;
        COTW_LOG("[taa]   +0x%03X : this pass one frame back %ld/%ld | ANY sampled "
                 "camera %ld/%ld (age %ld/%ld frames)  %s",
                 kVelOffsets[k], h0, h1, r0, r1,
                 g_velRingAge[k][0], g_velRingAge[k][1],
                 (h0 || h1) ? "<<< THE PREVIOUS FRAME'S CAMERA"
                            : ((r0 || r1) ? "<<< a camera we HAVE seen" : ""));
    }
    if (!any && !anyRing) {
        COTW_LOG("[taa]   NONE of them, against %d sampled cameras covering about "
                 "four frames. So +0x1D0 / +0x210 / +0x260 are not copies of any "
                 "main-view matrix this mod has ever seen written - the doc's "
                 "claim that they hold 'the previous frame's view-projection' is "
                 "WRONG, and patching them per-eye could never have fixed the "
                 "reprojection. Which is exactly what the headset reported.",
                 g_camRingCount);
    } else if (!any && anyRing) {
        COTW_LOG("[taa]   Not this pass's own previous matrix, but a camera we HAVE "
                 "seen written - see the age above. That is the slot to make "
                 "per-eye, and the earlier patch was aimed at the right offset for "
                 "the wrong reason.");
    }
}

void CBScanReport() {
    // *** TIER 1 STAGE 0 READOUT. ***
    //
    // Once a second, not once per report block: this is the instrument every
    // later stage is judged against, and a reading three seconds stale is worse
    // than none. It states what it EXPECTS, so a wrong reader is obvious without
    // anyone having to remember the target numbers.
    if (Cfg().tier1_measure_fov) {
        static ULONGLONG last = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - last >= 1000) {
            last = now;
            const float a = g_projA, b = g_projB, n = g_projN;
            if (a > 0.0f && b > 0.0f) {
                const float kRad = 57.2957795f;
                const float h = 2.0f * atanf(1.0f / a) * kRad;
                const float v = 2.0f * atanf(1.0f / b) * kRad;
                COTW_LOG("[tier1] the camera's OWN projection, read back from the "
                         "GPU: %.2f deg vertical, %.2f deg horizontal, aspect "
                         "%.4f, near %.6f   (%ld windows, %ld agreed)",
                         v, h, tanf(h * 0.5f / kRad) / tanf(v * 0.5f / kRad), n,
                         (long)g_projSeen, (long)g_projAgreed);
                if (Cfg().tier1_fov) {
                    // The loop the whole route depends on closing: the mod
                    // writes a float into the engine, then reads the
                    // consequence out of the GPU. Expect the readout above to
                    // equal 2*atan(k*tan(stock/2)).
                    const float want = 2.0f * atanf(Cfg().tier1_fov_k_override *
                                                    tanf(45.0f / 57.2957795f)) *
                                       57.2957795f;
                    COTW_LOG("[tier1]   lever k = %.3f -> vertical should read "
                             "%.2f deg; it reads %.2f  (%s)",
                             Cfg().tier1_fov_k_override, want, v,
                             (fabsf(want - v) < want * 0.005f)
                                 ? "LOOP CLOSED - the lever reaches the renderer"
                                 : "MISMATCH - lever and renderer disagree");
                }
            } else {
                COTW_LOG("[tier1] no camera projection read yet (%ld windows seen) "
                         "- if this stays at 0 the reader is wrong and nothing "
                         "built on it can be trusted", (long)g_projSeen);
            }
            InterlockedExchange(&g_projSeen, 0);
            InterlockedExchange(&g_projAgreed, 0);
            if (Cfg().taa_prev_from_last_render) {
                COTW_LOG("[taa] previous-frame matrix replaced with the LAST "
                         "RENDER's %ld time(s) - under full rate that render is "
                         "the other eye, which is what TAA has in history",
                         (long)g_taaPrevPatched);
                InterlockedExchange(&g_taaPrevPatched, 0);
            }
        }
    }
    // A device can appear at any time; cover its contexts as soon as it does.
    HookPendingDevices();

    // *** LATCH "SCOPED" - ONCE PER PRESENT. ***
    //
    // This has to sit ABOVE every diagnostic block below it: those are throttled
    // to one frame in 180, and a scoped flag updated three seconds late would be
    // worse than none at all. Present is the only true frame boundary this file
    // gets, and it is here.
    {
        const LONG n = InterlockedExchange(&g_glassHitsThisFrame, 0);
        InterlockedIncrement(&g_glassFramesSeen);
        if (n == 0) InterlockedIncrement(&g_glassFramesZero);
        if (n < g_glassFrameMin) InterlockedExchange(&g_glassFrameMin, n);
        if (n > g_glassFrameMax) InterlockedExchange(&g_glassFrameMax, n);
    }
    // Exactly two optic draws per frame in all six captures. Any other number
    // means the signature is catching something it should not, and it says so
    // rather than quietly moving scenery.
    {
        const LONG n = InterlockedExchange(&g_opticThisFrame, 0);
        // A PAIR per eye pass, and one Present spans both - so the honest test is
        // 'even', not 'exactly 2'. The old check tripped on every scoped frame and
        // reported the signature as broken when it was working perfectly.
        if (n != 0 && (n % 2) != 0) InterlockedIncrement(&g_opticOddFrames);
    }
    InterlockedExchange(&g_scopedNow, g_maskSeenThisFrame);
    InterlockedExchange(&g_maskSeenThisFrame, 0);

    // The HUD probe's frame boundary. Above every throttled block below, and
    // NOT throttled itself: it counts frames of its own and stops after the
    // number it was asked for, so a "once every 180 frames" wrapper would make
    // three frames take nine minutes to collect.
    HudProbeReport();

    // The TAA probe's frame boundary, and it has to be HERE rather than in the
    // Present hook itself: CBScanReport runs inside `if (ownFrame)`, which under
    // full-rate stereo is true on the SECOND pass only. So one call spans both
    // eye passes, which is the only window in which "how many resolves per
    // frame" means what it says. Not throttled - it counts its own seconds.
    TaaOnFrame();

    // Each eye's reprojection matrix ages by exactly one real frame here, in
    // step with the private history texture it belongs to. Above the throttled
    // blocks below, for the same reason the scoped latch is.
    TaaRollEyeMatrices();
    if (Cfg().per_eye_temporal_history && Cfg().per_eye_temporal_matrix) {
        static ULONGLONG last = 0;
        static uint64_t frames = 0;
        ++frames;
        const ULONGLONG now = GetTickCount64();
        if (now - last >= 1000) {
            const double f = frames ? (double)frames : 1.0;
            const LONG p0 = InterlockedExchange(&g_taaEyePatched[0], 0);
            const LONG p1 = InterlockedExchange(&g_taaEyePatched[1], 0);
            const LONG s0 = InterlockedExchange(&g_taaEyeSeen[0], 0);
            const LONG s1 = InterlockedExchange(&g_taaEyeSeen[1], 0);
            const LONG def = InterlockedExchange(&g_taaEyeDeferred, 0);
            COTW_LOG("[taa] reprojection matrix, per frame: eye 0 patched %.1f of "
                     "%.1f camera blocks, eye 1 %.1f of %.1f  (%.1f of them on a "
                     "deferred context)", (double)p0 / f, (double)s0 / f,
                     (double)p1 / f, (double)s1 / f, (double)def / f);
            COTW_LOG("[taa]   it MOVED the reprojection by %.4f / %.4f world units "
                     "(eye 0 / eye 1). Two zeros mean the matrix we wrote was the "
                     "one already there, and the picture cannot have changed.",
                     g_taaShift[0], g_taaShift[1]);
            const LONG d0 = InterlockedExchange(&g_camBlockDistinct[0], 0);
            const LONG d1 = InterlockedExchange(&g_camBlockDistinct[1], 0);
            COTW_LOG("[taa]   ASSUMPTION 1 - of the camera blocks in one pass, "
                     "%.1f (eye 0) and %.1f (eye 1) per frame DIFFER from the "
                     "first one. Anything but ~0 means the matrix we store is "
                     "whichever pass wrote last, not the main view.",
                     (double)d0 / f, (double)d1 / f);
            COTW_LOG("[taa]   ASSUMPTION 2 - the engine's OWN +0x210 differs from "
                     "its current matrix by %.4f / %.4f. Near zero means +0x210 is "
                     "NOT a previous frame at all and this whole patch site is the "
                     "wrong one.", g_engPrevVsCur[0], g_engPrevVsCur[1]);
            if (p0 == 0 && p1 == 0) {
                COTW_LOG("[taa]   NOTHING was patched. Either the camera block "
                         "never arrives through Unmap on this build, or the "
                         "structural test rejected it - judge nothing from the "
                         "picture until this line is non-zero.");
            } else if ((p0 == 0) != (p1 == 0)) {
                COTW_LOG("[taa]   ONE EYE ONLY - the eye tag is not being read "
                         "correctly at this write site. Turn the matrix half off.");
            }
            last = now;
            frames = 0;
        }
    }

    // *** THE ONE DIAGNOSTIC THAT DECIDES HOW THIS FEATURE IS BUILT. ***
    //
    // The capture says the visible weapon is thirty plain DrawIndexed calls per
    // frame. Everything below is here to answer, in a single run, which of the
    // ways that could fail is the one actually happening - because from outside
    // they all look like "the gun does not move".
    if (Cfg().weapon_pass_diag) {
        static uint64_t frames = 0;
        if ((++frames % 300) == 0) {
            const LONG di    = InterlockedExchange(&g_diTotal, 0);
            const LONG def   = InterlockedExchange(&g_diDeferred, 0);
            const LONG shad  = InterlockedExchange(&g_matchShadow, 0);
            const LONG drv   = InterlockedExchange(&g_matchDriver, 0);
            const LONG dep   = InterlockedExchange(&g_matchDepth, 0);
            const LONG exec  = InterlockedExchange(&g_execCmdList, 0);
            const LONG fin   = InterlockedExchange(&g_finishCmdList, 0);
            const LONG full  = InterlockedExchange(&g_ctxTableFull, 0);

            COTW_LOG("[wdiag] over 300 frames, per frame: DrawIndexed %.1f "
                     "(deferred %.1f) | colour round: shadow %.2f, DRIVER %.2f | "
                     "depth rounds %.2f",
                     (double)di / 300.0, (double)def / 300.0, (double)shad / 300.0,
                     (double)drv / 300.0, (double)dep / 300.0);
            COTW_LOG("[wdiag] command lists: executed %.2f/frame, recorded %.2f/frame | "
                     "context table exhausted %ld times | %ld meshes learned",
                     (double)exec / 300.0, (double)fin / 300.0, (long)full,
                     (long)g_weaponIdxCount);

            // The census. Each entry point next to what the capture counted for
            // one frame of this same game, so "do we see the renderer at all"
            // is answered by reading, not by inference.
            struct { const char* name; volatile LONG* counter; int expected; } census[] = {
                {"DrawIndexed",                  &g_epDrawIndexed,                  718},
                {"DrawIndexedInstanced",         &g_epDrawIndexedInstanced,         785},
                {"DrawIndexedInstancedIndirect", &g_epDrawIndexedInstancedIndirect, 433},
                {"DrawInstancedIndirect",        &g_epDrawInstancedIndirect,        132},
                {"Draw",                         &g_epDraw,                          94},
            };
            double seen = 0.0, want = 0.0;
            COTW_LOG("[wdiag] draw census - per frame seen vs the capture's frame:");
            for (auto& c : census) {
                const double v = (double)InterlockedExchange(c.counter, 0) / 300.0;
                seen += v;
                want += c.expected;
                COTW_LOG("[wdiag]   %-30s %8.1f   (capture: %4d)  %s",
                         c.name, v, c.expected,
                         v < c.expected * 0.5 ? "<-- MISSING" : "");
            }
            COTW_LOG("[wdiag]   TOTAL %.1f of ~%.0f expected = %.1f%% of the frame is "
                     "visible to this DLL", seen, want,
                     want > 0 ? 100.0 * seen / want : 0.0);

            // Shader creation is a DEVICE call. If this is short too, the
            // problem is not which CONTEXT we hooked - it is which DEVICE.
            COTW_LOG("[wdiag] device side: %ld pixel + %ld vertex shaders created "
                     "through our hooks (capture: 1226 + 684) | %ld distinct device(s) "
                     "seen | %ld weapon PS identified of 3",
                     (long)g_psCreated, (long)g_vsCreated, (long)g_deviceCount,
                     (long)IdentifiedWeaponShaderCount());
            COTW_LOG("[wdiag] hooking: %ld context implementation(s) hooked from %ld "
                     "context(s) offered, %ld/%ld device(s) covered, %ld context "
                     "vtable(s) caught at the device",
                     (long)g_implsHooked, (long)g_ctxOffered, (long)g_devicesHooked,
                     (long)g_deviceCount, (long)g_pendingVTCount);

            // The capture counted TEN colour-round draws per frame. Say what
            // each possible reading means, so the next step is decided by this
            // log and not by another theory.
            const double perFrame = (double)drv / 300.0;
            if (IdentifiedWeaponShaderCount() == 0) {
                COTW_LOG("[wdiag] VERDICT: no weapon shader identified at all - the "
                         "CRCs never matched a CreatePixelShader call. Nothing "
                         "downstream can fire. Check weapon_ps_crc0/1/2.");
            } else if (perFrame >= 5.0) {
                COTW_LOG("[wdiag] VERDICT: the hooks SEE the viewmodel pass "
                         "(%.1f colour draws/frame, capture says 10). Per-draw "
                         "shifting is reachable - this is now a tuning problem.",
                         perFrame);
                if (full > 0) {
                    COTW_LOG("[wdiag]   and the shadow table WAS exhausted (%ld) - that "
                             "is why the old build matched almost nothing. The driver "
                             "query is what fixed it.", (long)full);
                }
            } else if (exec > 0) {
                COTW_LOG("[wdiag] VERDICT: almost no colour draws seen (%.2f/frame) AND "
                         "%.2f command lists are executed per frame - the pass is "
                         "recorded once and replayed, so the shift has to be applied "
                         "at RECORD time, not at draw time.", perFrame,
                         (double)exec / 300.0);
            } else {
                COTW_LOG("[wdiag] VERDICT: almost no colour draws seen (%.2f/frame) and "
                         "NO command lists are in play - so the pass reaches the GPU by "
                         "some path this DLL does not sit on. Next: check whether "
                         "another module (ReShade) has the vtable.", perFrame);
            }
        }
    }

    // Weapon shift accounting, every 300 frames. A healthy line is a SMALL,
    // steady number on both counters; hundreds on the vertex counter means the
    // weapon's vertex shader is shared with the rest of the world.
    if (Cfg().weapon_screen_stereo) {
        static uint64_t frames = 0;
        if ((++frames % 300) == 0) {
            const LONG ps = InterlockedExchange(&g_shiftByPS, 0);
            const LONG vs = InterlockedExchange(&g_shiftByVS, 0);
            const LONG ent = InterlockedExchange(&g_vpEntered, 0);
            const LONG zs  = InterlockedExchange(&g_vpZeroShift, 0);
            const LONG zc  = InterlockedExchange(&g_vpZeroCount, 0);
            const LONG app = InterlockedExchange(&g_vpApplied, 0);
            COTW_LOG("[weapon] shifted %.1f draws/frame over 300 frames "
                     "(pixel-matched %.1f, vertex-matched %.1f) shift=%.1f px eye=%d",
                     (double)(ps + vs) / 300.0, (double)ps / 300.0, (double)vs / 300.0,
                     (double)WeaponPixelShift(), CurrentRenderEye());
            COTW_LOG("[weapon]   Begin() reached %ld times: applied %ld, "
                     "bailed on zero shift %ld, bailed on no viewport %ld",
                     (long)ent, (long)app, (long)zs, (long)zc);
            if (ent == 0) {
                // Say what is actually known, and check the prerequisite rather
                // than assuming it. The previous wording asserted the shader
                // match was working - it was not, the three CRCs had loaded as
                // zero - and that false claim sent a whole test cycle looking at
                // draw entry points instead.
                const int ids = IdentifiedWeaponShaderCount();
                if (ids == 0) {
                    COTW_LOG("[weapon]   *** NO weapon shader was ever identified this "
                             "run, so no draw can match. Check weapon_ps_crc0/1/2 in "
                             "the ini - they are %08X %08X %08X. ***",
                             (unsigned)Cfg().weapon_ps_crc0, (unsigned)Cfg().weapon_ps_crc1,
                             (unsigned)Cfg().weapon_ps_crc2);
                } else {
                    COTW_LOG("[weapon]   *** %d shader(s) identified, but no draw reached "
                             "Begin() - the weapon draws through an entry point that is "
                             "not hooked. ***", ids);
                }
            }
        }
    }

    // The shader table is what the weapon hunt needs, and it fills from the
    // first frame after the hooks install - so report it even when the (costly)
    // constant-buffer content scan is switched off.
    {
        static uint64_t f = 0;
        const LONG wn = g_weaponCBCount;
        if (wn && (++f % 180) == 1) {
            COTW_LOG("[weapon] constant buffers read by the weapon's own draws:");
            for (LONG i = 0; i < wn && i < kMaxWeaponCB; ++i) {
                COTW_LOG("[weapon]   VS slot b%u : %5u bytes, seen %ld times  (buffer %p)",
                         g_weaponCBs[i].slot, g_weaponCBs[i].size,
                         (long)g_weaponCBs[i].seen, (void*)g_weaponCBs[i].buf);
            }
        }
    }

    if (Cfg().shader_dump) {
        static bool dumped = false;
        static DWORD armedAt = 0;
        if (!armedAt) armedAt = GetTickCount();
        // Wait a little so the game has created its shaders before listing them.
        if (!dumped && GetTickCount() - armedAt > 20000) { dumped = true; DumpShaderTable(); }
    }
    // ABOVE the weapon_cb_scan gate on purpose. The tail is the cheapest useful
    // diagnostic there is - a ring buffer and one log line - and it answers the
    // question that actually blocks the viewmodel work. Leaving it below the gate
    // meant it never printed, because weapon_cb_scan turns on a great deal of
    // heavy scanning that nobody wants running just to read ten numbers.
    if (Cfg().weapon_pass_diag) {
        NoteWeaponFrameBoundary();      // every frame - it is counting frames
        static uint64_t ft = 0;
        if ((++ft % 180) == 1) {
            ReportFrameTail();
            ReportWeaponIntermittency();
            if (Cfg().weapon_3d) {
                const LONG p = g_weapon3dPatched;
                const LONG so = g_weapon3dSkippedOffset;
                const LONG nc = g_weapon3dNoCtx1;
                const LONG ss = g_weapon3dSkippedSize;
                COTW_LOG("[weapon3d] %ld shifted, %ld skipped (our snapshot is of "
                         "a different buffer than this draw reads), %ld skipped "
                         "(past our %uB snapshot), %ld skipped (no ctx1)  "
                         "[amount %.4f, slot %d, layout %d]%s",
                         (long)p, (long)ss, (long)so, kPoolSnapshot, (long)nc,
                         Cfg().weapon_3d_amount, Cfg().weapon_3d_slot,
                         Cfg().weapon_3d_layout,
                         p == 0 ? "   <-- NEVER RAN" : "");
                {
                    LONG tot = 0;
                    for (int i2 = 0; i2 < 7; ++i2) tot += g_bindExit[i2];
                    COTW_LOG("[weapon3d]   the constants path REFUSED %ld draw(s) - "
                             "every reason, not just the one:", (long)tot);
                    for (int i2 = 0; i2 < 7; ++i2) {
                        if (!g_bindExit[i2]) continue;
                        COTW_LOG("[weapon3d]       %6ld  %s",
                                 (long)g_bindExit[i2], kBindExitName[i2]);
                        InterlockedExchange(&g_bindExit[i2], 0);
                    }
                }
                {
                    char sl[512]; int at = 0;
                    const LONG ns = g_skipMeshCount < kSkipMesh ? g_skipMeshCount
                                                                : kSkipMesh;
                    for (LONG i = 0; i < ns && at < 460; ++i) {
                        at += snprintf(sl + at, sizeof(sl) - at, "%u x%ld  ",
                                       g_skipMeshIdx[i], (long)g_skipMeshHits[i]);
                        InterlockedExchange(&g_skipMeshHits[i], 0);
                    }
                    sl[at] = 0;
                    COTW_LOG("[weapon3d]   pieces that LOST their snapshot (index "
                             "count x times): %s  - each is one frame of that piece "
                             "drawn unshifted", sl);
                }
                COTW_LOG("[weapon3d]   %ld draw(s) matched by the engine's own "
                         "first-person stencil tag (bit 6)%s",
                         (long)g_matchStencil,
                         Cfg().weapon_match_stencil && g_matchStencil == 0
                             ? "   <-- NONE: this build may not tag; turn "
                               "weapon_match_index back on"
                             : "");
                InterlockedExchange(&g_matchStencil, 0);
                COTW_LOG("[weapon3d]   %ld of those STAMP the scope lens circle "
                         "into the stencil buffer (WriteMask 0x40, REPLACE) - the "
                         "invisible disc the shroud is clipped against%s",
                         (long)g_matchMask,
                         g_matchMask == 0
                             ? "   <-- none seen: no optic raised, or this build "
                               "stamps the circle another way"
                             : "");
                InterlockedExchange(&g_matchMask, 0);
                COTW_LOG("[weapon3d]   %ld draw(s) were claimed ONLY by the loose "
                         "rules (no stencil tag, no glass rule) - %s",
                         (long)g_looseOnly,
                         Cfg().weapon_stencil_only
                             ? "NOT moved: exact rules only"
                             : "moved, and any wrong claim among them flickers");
                InterlockedExchange(&g_looseOnly, 0);
                COTW_LOG("[weapon3d]   %ld draw(s) left alone because they show a "
                         "COPY of the scene (the scope's magnified picture)",
                         (long)g_skippedPicture);
                InterlockedExchange(&g_skippedPicture, 0);
                COTW_LOG("[weapon3d]   optic glass matched by its exact state %ld "
                         "time(s)%s",
                         (long)g_opticGlass,
                         g_opticOddFrames
                             ? "   <-- SOME FRAMES DID NOT SEE EXACTLY 2: the "
                               "signature is matching something else"
                             : "");
                InterlockedExchange(&g_opticGlass, 0);
                InterlockedExchange(&g_opticOddFrames, 0);
                COTW_LOG("[weapon3d]   %ld mask draw(s) could not be reached through "
                         "their constants and were left unmoved%s",
                         (long)g_maskNoConstants,
                         g_maskNoConstants
                             ? "   <-- if this is not zero, restore the fallback for "
                               "GLASS AND MASK TOGETHER, never one alone"
                             : "");
                InterlockedExchange(&g_maskNoConstants, 0);
                COTW_LOG("[weapon3d]   %ld blended draw(s) near the gun read a "
                         "constant buffer we had NOT recorded - each is a lens that "
                         "did not move that frame (table holds %d, %ld learned)%s",
                         (long)g_glassBufUnknown, (int)kMaxGlassRes,
                         (long)g_glassResKnown,
                         g_glassResKnown >= kMaxGlassRes
                             ? "   <-- TABLE FULL: evicting oldest, not refusing"
                             : "");
                InterlockedExchange(&g_glassBufUnknown, 0);
                COTW_LOG("[weapon3d]   %ld glass draw(s) would have picked a "
                         "DIFFERENT eye than the body they belong to - each one is "
                         "a lens thrown 2x the strength away from its shroud for a "
                         "frame%s",
                         (long)g_glassEyeDisagree,
                         g_glassEyeDisagree > 0
                             ? (Cfg().weapon_3d_glass_follow_body_eye
                                    ? "   (corrected: the glass copied the body)"
                                    : "   <-- NOT corrected: follow-body-eye is off")
                             : "");
                InterlockedExchange(&g_glassEyeDisagree, 0);
                COTW_LOG("[weapon3d]   glass shift taken from its OWN buffer %ld "
                         "time(s), from the last-written global %ld time(s)%s",
                         (long)g_glassKFromBuffer, (long)g_glassKFromGlobal,
                         g_glassKFromGlobal > g_glassKFromBuffer
                             ? "   <-- mostly the global: the lens is still able "
                               "to pick up the other eye's shift"
                             : "");
                InterlockedExchange(&g_glassKFromBuffer, 0);
                InterlockedExchange(&g_glassKFromGlobal, 0);
                COTW_LOG("[weapon3d]   lens moved through its CONSTANTS %ld "
                         "time(s), at the RASTERIZER %ld time(s)%s",
                         (long)g_glassViaConstants, (long)g_glassViaViewport,
                         (Cfg().weapon_3d_glass_via_constants &&
                          g_glassViaConstants == 0 && g_glassViaViewport > 0)
                             ? "   <-- the constants path never reached a lens"
                             : "");
                InterlockedExchange(&g_glassViaConstants, 0);
                InterlockedExchange(&g_glassViaViewport, 0);
                COTW_LOG("[weapon3d]   %ld lens draw(s) caught by the MASK MESH "
                         "rule (%ld mesh size(s) learned from the mask) - these no "
                         "longer depend on which buffer the engine happened to use%s",
                         (long)g_glassByMaskMesh, (long)g_maskIdxCount,
                         (g_maskIdxCount == 0)
                             ? "   <-- no mask seen yet, so nothing learned"
                             : "");
                InterlockedExchange(&g_glassByMaskMesh, 0);
                COTW_LOG("[weapon3d]   %ld lens draw(s) caught by the LENS MESH rule "
                         "(%ld mesh size(s) learned from confirmed lenses) - these are "
                         "the unblended copies that used to stay put and fight",
                         (long)g_glassByGlassMesh, (long)g_glassMeshCount);
                InterlockedExchange(&g_glassByGlassMesh, 0);
                COTW_LOG("[weapon3d]   weapon field of view %.3f applied to the BODY "
                         "%ld time(s), to the LENS %ld time(s)%s",
                         Cfg().weapon_view_scale, (long)g_poseOnBody,
                         (long)g_scaleOnLens,
                         (Cfg().weapon_view_scale != 1.0f && g_poseOnBody == 0 &&
                          g_scaleOnLens > 0)
                             ? "   <-- LENS ONLY: it is scaling about the screen "
                               "centre while the weapon is not, which is the lens "
                               "flying off the optic"
                             : "");
                InterlockedExchange(&g_poseOnBody, 0);
                InterlockedExchange(&g_scaleOnLens, 0);
                COTW_LOG("[weapon3d]   lens-mesh draws seen %ld, MISSED %ld (their "
                         "distance from the weapon ran %ld..%ld, the window is %d)%s",
                         (long)g_lensMeshSeen, (long)g_lensMeshMissed,
                         (long)(g_lensMissedGapMin == 999999 ? 0 : g_lensMissedGapMin),
                         (long)g_lensMissedGapMax, Cfg().weapon_3d_glass,
                         g_lensMeshMissed > 0
                             ? "   <-- THESE render the lens unmoved: the detached "
                               "frames"
                             : "");
                InterlockedExchange(&g_lensMeshSeen, 0);
                InterlockedExchange(&g_lensMeshMissed, 0);
                InterlockedExchange(&g_lensMissedGapMin, 999999);
                InterlockedExchange(&g_lensMissedGapMax, 0);
                {
                    char line[512]; int at = 0;
                    const LONG nu = g_unclaimedCount < kUnclaimed ? g_unclaimedCount
                                                                  : kUnclaimed;
                    for (LONG i = 0; i < nu && at < 460; ++i) {
                        at += snprintf(line + at, sizeof(line) - at, "%u x%ld  ",
                                       g_unclaimedIdx[i], (long)g_unclaimedHits[i]);
                        InterlockedExchange(&g_unclaimedHits[i], 0);
                    }
                    line[at] = 0;
                    COTW_LOG("[weapon3d]   UNCLAIMED draws on the weapon (index count "
                             "x hits): %s", line);
                }
                COTW_LOG("[weapon3d]   %ld constant buffer(s) snapshotted from "
                         "UpdateSubresource (previously none were)%s",
                         (long)g_snapViaUpdate,
                         g_snapViaUpdate == 0
                             ? "   <-- none: the lens buffer arrives some other way"
                             : "");
                InterlockedExchange(&g_snapViaUpdate, 0);
                if (g_lensCBSeen) {
                    static const char* kUsage[] = {"DEFAULT", "IMMUTABLE",
                                                   "DYNAMIC", "STAGING"};
                    const LONG u = g_lensCBUsage;
                    COTW_LOG("[weapon3d]   THE LENS BUFFER: %ld bytes, usage %s, "
                             "cpuAccess 0x%lX, bind 0x%lX, snapshot held: %s%s",
                             (long)g_lensCBWidth,
                             (u >= 0 && u < 4) ? kUsage[u] : "?",
                             (long)g_lensCBCpuFlags, (long)g_lensCBBind,
                             g_lensCBHaveSnap > 0 ? "yes" : "NO",
                             (g_lensCBWidth > 0 && g_lensCBWidth < 256)
                                 ? "   <-- UNDER 256 BYTES: both snapshot paths "
                                   "skip it, which is why it is never held"
                                 : "");
                }
                {
                    const LONG ne = g_extraGlassCount < kMaxExtraGlass
                                        ? g_extraGlassCount : kMaxExtraGlass;
                    for (LONG i = 0; i < ne; ++i) {
                        COTW_LOG("[weapon3d]   INTERMITTENT glass draw #%ld: %u "
                                 "indices, gap %ld, blended=%ld, carries the "
                                 "first-person tag=%ld - present on some frames "
                                 "and not others, i.e. the flicker",
                                 (long)g_extraGlass[i].nth, g_extraGlass[i].idx,
                                 (long)g_extraGlass[i].gap,
                                 (long)g_extraGlass[i].blended,
                                 (long)g_extraGlass[i].tagged);
                    }
                    InterlockedExchange(&g_extraGlassCount, 0);
                }
                COTW_LOG("[weapon3d]   glass PER FRAME over this window: min %ld, "
                         "max %ld, and %ld of %ld frames got NONE%s",
                         (long)(g_glassFrameMin == 999999 ? 0 : g_glassFrameMin),
                         (long)g_glassFrameMax, (long)g_glassFramesZero,
                         (long)g_glassFramesSeen,
                         g_glassFramesZero > 0
                             ? "   <-- THE FLICKER: those frames drew the lens "
                               "unshifted"
                             : (g_glassFrameMax > g_glassFrameMin
                                    ? "   <-- uneven: some frames shift it more "
                                      "times than others"
                                    : ""));
                InterlockedExchange(&g_glassFrameMin, 999999);
                InterlockedExchange(&g_glassFrameMax, 0);
                InterlockedExchange(&g_glassFramesZero, 0);
                InterlockedExchange(&g_glassFramesSeen, 0);
                COTW_LOG("[weapon3d]   scoped right now: %s -> strength %.4f "
                         "(hip %.4f, scoped %.4f, separate %s)",
                         PlayerIsScoped() ? "YES" : "no", Weapon3dAmount(),
                         Cfg().weapon_3d_amount, Cfg().weapon_3d_amount_scoped,
                         Cfg().weapon_3d_scoped_separate ? "on" : "off");
                COTW_LOG("[weapon3d]   %ld blended draw(s) considered as glass, "
                         "%ld of them rejected because their matrix is not one we "
                         "shifted on a tagged draw (%ld matrices in the ring)%s",
                         (long)g_matchGlass, (long)g_weapon3dGlassNoMatch,
                         (long)g_clipRingCount,
                         (g_matchGlass > 0 && g_weapon3dGlassNoMatch >= g_matchGlass)
                             ? "   <-- ALL rejected: the lens is never recognised"
                             : "");
                InterlockedExchange(&g_weapon3dGlassNoMatch, 0);
                COTW_LOG("[weapon3d]   glass: %ld upload(s) carried a weapon matrix "
                         "(of %ld scanned), %ld distinct block site(s), %ld draw(s) "
                         "moved at the rasterizer%s",
                         (long)g_glassResCount, (long)g_glassInPlaceScans,
                         (long)g_glassSiteCount, (long)g_matchGlass,
                         (g_glassResCount == 0)
                             ? "   <-- no weapon matrix copy seen in any upload"
                             : (g_matchGlass == 0
                                    ? "   <-- sites known, but no blended draw reads "
                                      "its constants from one of them"
                                    : ""));
                InterlockedExchange(&g_matchGlass, 0);
                InterlockedExchange(&g_glassResCount, 0);
                InterlockedExchange(&g_glassInPlace, 0);
                InterlockedExchange(&g_glassInPlaceScans, 0);
                ReportGlassTally();
                COTW_LOG("[weapon3d]   %ld of those used the +0x40 layout "
                         "(LocalConstants - the hands); the rest used +0x00 "
                         "(InstanceConsts)", (long)g_weapon3dAltOffset);
                InterlockedExchange(&g_weapon3dAltOffset, 0);
                ReportMeshTally();
                InterlockedExchange(&g_weapon3dPatched, 0);
                InterlockedExchange(&g_weapon3dSkippedOffset, 0);
                InterlockedExchange(&g_weapon3dSkippedSize, 0);
                InterlockedExchange(&g_weapon3dNoCtx1, 0);
            }
        }
    }

    if (!Cfg().weapon_cb_scan) {
        static uint64_t f = 0;
        if ((++f % 180) == 1) {
            COTW_LOG("[cb] %ld shaders seen so far%s", (long)g_shaderCount,
                     Cfg().shader_hide_index >= 0 ? "" : "  (hide one to find the gun)");
        }
        return;
    }
    static uint64_t frame = 0;
    if ((++frame % 180) != 1) {
        InterlockedExchange(&g_eventCount, 0);   // one frame's worth, not 180
        const LONG n = g_hitCount;
        for (LONG i = 0; i < n && i < kMaxHits; ++i) {
            InterlockedExchange(&g_hits[i].countThisFrame, 0);
        }
        return;
    }

    // The depth-clear map. The engine clears depth before the first-person pass
    // so the weapon cannot be occluded, so the LAST clear with only a handful of
    // draws after it is the viewmodel - and those draws are the ones whose view
    // has to be shifted per eye.
    COTW_LOG("[cb] frame: %ld draws, %ld depth clears. Draws after each clear:",
             (long)g_drawsThisFrame, (long)g_clearsThisFrame);
    const LONG clears = g_clearsThisFrame < kMaxClears ? g_clearsThisFrame : kMaxClears;
    for (LONG i = 0; i < clears; ++i) {
        const LONG after = (i + 1 < clears) ? g_clearMarks[i].drawsAfter
                                            : (g_drawsThisFrame - g_clearMarks[i].atDraw);
        COTW_LOG("[cb]   clear #%ld at draw %ld -> %ld draws follow it%s",
                 (long)i, (long)g_clearMarks[i].atDraw, (long)after,
                 (after > 0 && after < 60) ? "   <-- small: the VIEWMODEL pass?" : "");
    }
    InterlockedExchange(&g_clearsThisFrame, 0);
    InterlockedExchange(&g_drawsThisFrame, 0);
    InterlockedExchange(&g_drawsSinceLastClear, 0);
    t_drawsSinceClear = 0;

    COTW_LOG("[cb] weapon patch: %ld applied since last report (%ld via Map, %ld via "
             "UpdateSubresource)%s", (long)g_patchCount, (long)g_patchViaMap,
             (long)g_patchViaUpdate,
             g_patchCount == 0 ? "   <-- NEVER RAN: wrong buffer, or the write goes a "
                                 "third way" : "");
    InterlockedExchange(&g_patchCount, 0);
    InterlockedExchange(&g_patchViaMap, 0);
    InterlockedExchange(&g_patchViaUpdate, 0);

    {
        const int lo = Cfg().shader_hide_index;
        if (lo >= 0) {
            const int cnt = Cfg().shader_hide_count < 1 ? 1 : Cfg().shader_hide_count;
            COTW_LOG("[cb] hiding shaders %d..%d of %ld known:", lo, lo + cnt - 1,
                     (long)g_shaderCount);
            for (int i = lo; i < lo + cnt && i < g_shaderCount && i < kMaxShaders; ++i) {
                uint32_t h = 0, L = 0;
                if (ShaderFingerprint(g_shaders[i], &h, &L)) {
                    COTW_LOG("[cb]   #%d  fingerprint %08X  (%u bytes) - stable across "
                             "runs, unlike the index", i, h, L);
                } else {
                    COTW_LOG("[cb]   #%d  (no fingerprint - created before we hooked)", i);
                }
            }
        }
    }

    const LONG mn = g_modelCount;
    COTW_LOG("[cb] CLOSE model matrices (0.05-2 m from the camera) - %ld distinct:",
             (long)mn);
    for (LONG i = 0; i < mn && i < kMaxModels; ++i) {
        const bool transposed = (g_models[i].offset & 0x80000000u) != 0;
        COTW_LOG("[cb]   %6u-byte +0x%04X %s | %.2f-%.2f m | %ld this frame, %ld total%s",
                 g_models[i].bufferSize, g_models[i].offset & 0x7FFFFFFFu,
                 transposed ? "COLUMN-major" : "row-major   ", g_models[i].distMin,
                 g_models[i].distMax, (long)g_models[i].countThisFrame,
                 (long)g_models[i].countTotal,
                 (g_models[i].countThisFrame > 0 && g_models[i].countThisFrame <= 40)
                     ? "   <-- steady and close: the WEAPON?" : "");
        InterlockedExchange(&g_models[i].countThisFrame, 0);
    }
    if (mn >= kMaxModels) COTW_LOG("[cb]   *** TABLE FULL - list incomplete ***");

    const LONG en = g_eventCount < kMaxEvents ? g_eventCount : kMaxEvents;
    COTW_LOG("[cb] projection writes IN ORDER (first %ld of this frame):", (long)en);
    for (LONG i = 0; i < en; ++i) {
        COTW_LOG("[cb]   %2ld: %6.1f deg | %6u-byte +0x%04X", (long)i, g_events[i].fov,
                 g_events[i].size, g_events[i].offset);
    }
    InterlockedExchange(&g_eventCount, 0);

    const LONG sn = g_sizeCount;
    COTW_LOG("[cb] constant-buffer SIZES in use (%ld distinct):", (long)sn);
    for (LONG i = 0; i < sn && i < kMaxSizes; ++i) {
        COTW_LOG("[cb]   %6u bytes : %ld writes%s", g_sizes[i].size,
                 (long)g_sizes[i].count,
                 g_sizes[i].size > 4096 ? "   <-- LARGE: was hidden by the old filter"
                                        : "");
    }

    const LONG pn = g_projCount;
    COTW_LOG("[cb] PROJECTIONS seen (%ld distinct). Two different FOVs = world + weapon:",
             (long)pn);
    for (LONG i = 0; i < pn && i < kMaxProj; ++i) {
        char fov[48];
        if (g_proj[i].vfovMax - g_proj[i].vfovMin > 1.0f) {
            snprintf(fov, sizeof(fov), "%.1f-%.1f deg (zooms)", g_proj[i].vfovMin,
                     g_proj[i].vfovMax);
        } else {
            snprintf(fov, sizeof(fov), "%.1f deg", g_proj[i].vfovMin);
        }
        COTW_LOG("[cb]   %-22s vertical | %6u-byte buffer +0x%04X | %ld this frame, "
                 "%ld total", fov, g_proj[i].bufferSize, g_proj[i].offset,
                 (long)g_proj[i].countThisFrame, (long)g_proj[i].countTotal);
        InterlockedExchange(&g_proj[i].countThisFrame, 0);
    }
    if (!pn) COTW_LOG("[cb]   none found");
    if (pn >= kMaxProj) COTW_LOG("[cb]   *** TABLE FULL - entries were dropped, this "
                                 "list is not complete ***");

    const LONG n = g_hitCount;
    COTW_LOG("[cb] constant buffers carrying the camera (%ld distinct places):", (long)n);
    for (LONG i = 0; i < n && i < kMaxHits; ++i) {
        COTW_LOG("[cb]   %4u-byte buffer, +0x%03X : %s, %ld this frame, %ld total",
                 g_hits[i].bufferSize, g_hits[i].offset,
                 g_hits[i].kind == 0 ? "camera POSITION" : "camera basis row0",
                 (long)g_hits[i].countThisFrame, (long)g_hits[i].countTotal);
    }
    if (!n) {
        COTW_LOG("[cb]   none - the view is not uploaded through a DISCARD-mapped "
                 "constant buffer, so UpdateSubresource is the next thing to watch.");
    }
    for (LONG i = 0; i < n && i < kMaxHits; ++i) {
        InterlockedExchange(&g_hits[i].countThisFrame, 0);
    }
}

namespace {

// Hook every method we care about on ONE context implementation.
//
// Safe to call with the same class twice - each method is skipped if its
// address is already in the table - so it can simply be pointed at every
// context the game hands us without any bookkeeping about which is which.
// One aligned pointer store, and that is the whole point: no thread suspension,
// no prologue relocation, nothing a renderer can be standing in the middle of.
// Returns false when the slot is already ours, so a second pass cannot record
// our own detour as the "original" and spin forever.
bool PatchVTableSlot(void** vt, int slot, void* detour, void** prevOut) {
    void* cur = vt[slot];
    if (!cur || cur == detour) return false;
    DWORD oldProt = 0;
    if (!VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &oldProt)) {
        return false;
    }
    *prevOut = cur;
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(&vt[slot]), detour);
    DWORD tmp = 0;
    VirtualProtect(&vt[slot], sizeof(void*), oldProt, &tmp);
    return true;
}

void HookContextVTableImpl(void** vt, const char* what, bool forceAtomic);

void HookContextVTableRaw(void** vt, const char* what) {
    HookContextVTableImpl(vt, what, false);
}
void HookContextVTableAtomic(void** vt, const char* what) {
    HookContextVTableImpl(vt, what, true);
}

void HookContextVTableImpl(void** vt, const char* what, bool forceAtomic) {
    if (!vt) return;
    InterlockedIncrement(&g_ctxOffered);

    struct Entry { MethodOrigs* m; int slot; void* detour; const char* name; };
    const Entry table[] = {
        {&g_oDrawIndexed,       kSlotDrawIndexed,       (void*)&Hook_DrawIndexed,       "DrawIndexed"},
        {&g_oDraw,              kSlotDraw,              (void*)&Hook_Draw,              "Draw"},
        {&g_oDrawIndexedInstanced, kSlotDrawIndexedInstanced, (void*)&Hook_DrawIndexedInstanced, "DrawIndexedInstanced"},
        {&g_oDrawInstanced,     kSlotDrawInstanced,     (void*)&Hook_DrawInstanced,     "DrawInstanced"},
        {&g_oDrawIndexedInstancedIndirect, kSlotDrawIndexedInstancedIndirect, (void*)&Hook_DrawIndexedInstancedIndirect, "DrawIndexedInstancedIndirect"},
        {&g_oDrawInstancedIndirect, kSlotDrawInstancedIndirect, (void*)&Hook_DrawInstancedIndirect, "DrawInstancedIndirect"},
        {&g_oPSSetShader,       kSlotPSSetShader,       (void*)&Hook_PSSetShader,       "PSSetShader"},
        {&g_oVSSetShader,       kSlotVSSetShader,       (void*)&Hook_VSSetShader,       "VSSetShader"},
        {&g_oClearDSV,          kSlotClearDSV,          (void*)&Hook_ClearDSV,          "ClearDepthStencilView"},
        {&g_oUpdateSubresource, kSlotUpdateSubresource, (void*)&Hook_UpdateSubresource, "UpdateSubresource"},
        {&g_oMap,               kSlotMap,               (void*)&Hook_Map,               "Map"},
        {&g_oUnmap,             kSlotUnmap,             (void*)&Hook_Unmap,             "Unmap"},
        {&g_oExecuteCommandList, kSlotExecuteCommandList, (void*)&Hook_ExecuteCommandList, "ExecuteCommandList"},
        {&g_oFinishCommandList, kSlotFinishCommandList, (void*)&Hook_FinishCommandList, "FinishCommandList"},
    };

    int added = 0, already = 0, failed = 0;
    const bool useVTable = forceAtomic || Cfg().hook_context_vtable;

    for (const Entry& e : table) {
        e.m->slot = e.slot;

        if (useVTable) {
            if (e.m->KnownVT(vt)) { ++already; continue; }
            void* prev = nullptr;
            if (PatchVTableSlot(vt, e.slot, e.detour, &prev) && prev) {
                e.m->AddVT(vt, prev, e.detour);
                ++added;
            } else {
                // Already ours, or the page would not turn writable. Neither is
                // an error worth a line per method per vtable.
                ++already;
            }
            continue;
        }

        void* target = vt[e.slot];
        if (e.m->Known(target)) { ++already; continue; }
        void* orig = nullptr;
        MH_STATUS st = MH_CreateHook(target, e.detour, &orig);
        if ((st == MH_OK || st == MH_ERROR_ALREADY_CREATED) &&
            MH_EnableHook(target) == MH_OK && orig) {
            e.m->Add(target, orig);
            ++added;
        } else {
            ++failed;
            COTW_LOG("[cb]   %s: %s hook FAILED (%d)", what, e.name, (int)st);
        }
    }
    if (added) {
        InterlockedIncrement(&g_implsHooked);
        COTW_LOG("[cb] %s: %d method(s) newly hooked, %d already known, %d failed "
                 "(vtable %p, %s)", what, added, already, failed, (void*)vt,
                 useVTable ? "vtable entries patched - atomic, cannot race a "
                             "live draw"
                           : "function bodies patched via MinHook");
    }
}

void HookContextVTable(ID3D11DeviceContext* c, const char* what) {
    if (c) HookContextVTableRaw(*reinterpret_cast<void***>(c), what);
}

// Every context a device can give us: its immediate one, and the class its
// deferred ones belong to. Called for EVERY device, because the census showed
// the game creating shaders on devices we had never hooked a context of.
void HookAllContextsOf(ID3D11Device* dev, const char* why) {
    if (!dev) return;
    ID3D11DeviceContext* imm = nullptr;
    dev->GetImmediateContext(&imm);
    if (imm) {
        HookContextVTable(imm, why);
        imm->Release();
    }
    ID3D11DeviceContext* def = nullptr;
    if (SUCCEEDED(dev->CreateDeferredContext(0, &def)) && def) {
        HookContextVTable(def, why);
        def->Release();
    }
}

// *** CATCH THE DEVICE AT BIRTH. ***
//
// Everything else here finds the device by asking the SWAPCHAIN at Present time,
// and that is far too late: by then the game has created its device, its
// contexts and its entire shader library. The census is what that costs -
//
//     201 pixel + 106 vertex shaders created through our hooks (capture: 1226 + 684)
//     TOTAL 7.9 of ~2162 expected = 0.4% of the frame is visible to this DLL
//
// and it is why the weapon could never be found. Not the CRCs, not the index
// counts: 99.6% of the frame simply never reaches this file.
//
// D3D11CreateDevice is the one door every device comes through. Hooking the
// EXPORT rather than a vtable catches every device the game makes - including
// any that never present, which are invisible from the swapchain no matter how
// carefully its contexts are walked.
using PFN_D3D11CreateDevice = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
    UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
using PFN_D3D11CreateDeviceAndSwapChain = HRESULT(WINAPI*)(
    IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT,
    UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**,
    D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);

PFN_D3D11CreateDevice o_D3D11CreateDevice = nullptr;
PFN_D3D11CreateDeviceAndSwapChain o_D3D11CreateDeviceAndSwapChain = nullptr;
volatile LONG g_devicesBorn = 0;

// *** RECORDS ONLY. NO MinHook CALLS HERE, EVER. ***
//
// We are INSIDE the game's call to D3D11CreateDevice, on its loading thread,
// while the D3D runtime holds its own construction locks. MH_EnableHook suspends
// every other thread in the process to patch code - doing that from here crashed
// the game at startup twice, at exactly this point.
//
// NoteDevice's own comment says the same thing about a far safer place ("that
// runs on whatever thread is creating a shader ... invites a deadlock"), and an
// earlier version of this function called InstallShaderCreationHooks inline
// anyway while claiming in its comment to have deferred the risky work. It had
// deferred the contexts and left the hooks.
//
// Everything is handed to HookPendingDevices, which runs on the Present thread
// where suspending the others is safe.
// *** DO NOT ADOPT OUR OWN DEVICE. ***
//
// The mod creates a throwaway device to read vtables from, and render_hook's own
// log line predicted what happens when something hooks that: "creating the probe
// device - if the log stops HERE, a second D3D hooking layer is fighting us."
// Once D3D11CreateDeviceAndSwapChain is hooked, WE are that second layer - the
// probe call re-enters us and we patch vtables in the middle of building the
// very device we are building. The log stopped on that exact line.
//
// Thread-local because only the thread doing the probe creation must be blind;
// the game can legitimately create devices on other threads at the same time.
thread_local int g_creatingOwnDevice = 0;

void AdoptDevice(ID3D11Device* dev, ID3D11DeviceContext* ctx, const char* how) {
    if (!dev) return;
    if (g_creatingOwnDevice > 0) {
        COTW_LOG("[dev] ignoring our own probe device (%s) - adopting it means "
                 "hooking ourselves mid-creation", how);
        return;
    }
    const LONG n = InterlockedIncrement(&g_devicesBorn);
    NoteDevice(dev);

    // *** THE ONE HOOK THAT HAS TO GO IN RIGHT NOW. ***
    //
    // Everything else waits for Present, and rightly so - MinHook suspends
    // threads and must not run from inside D3D11CreateDevice. But this is a
    // single atomic pointer write into the device's vtable, which suspends
    // nothing, and it MUST happen here: the engine builds its deferred contexts
    // moments after the device exists, and every one it builds before we are
    // watching is a context we never see. That is the race, and it is what
    // decides 0.4% against 194% of the frame.
    //
    // Hooking CreateDeferredContext instead of chasing the contexts afterwards
    // turns the race into a certainty: we do not have to be faster than the
    // engine, we only have to be in place before it asks.
    if (Cfg().hook_contexts_early) {
        void** dvt = *reinterpret_cast<void***>(dev);
        void* prev = nullptr;
        if (PatchVTableSlot(dvt, kSlotCreateDeferredContext,
                            (void*)&Hook_CreateDeferredContext, &prev) && prev) {
            if (!o_CreateDeferredContext) {
                o_CreateDeferredContext = (PFN_CreateDeferredContext)prev;
            }
            COTW_LOG("[dev] device #%ld born via %s - CreateDeferredContext hooked "
                     "AT BIRTH, so every context it builds is ours", (long)n, how);
        } else {
            COTW_LOG("[dev] device #%ld born via %s - CreateDeferredContext was "
                     "already hooked", (long)n, how);
        }
    } else {
        COTW_LOG("[dev] device #%ld born via %s - recorded, hooks deferred to "
                 "Present", (long)n, how);
    }

    if (ctx) NoteContextVTable(ctx, "immediate context at birth");
}

// Entry AND exit are logged, deliberately. Tonight's hangs stop the log at
// "creating the probe device" and everything after that line is inference. With
// these, one hung run says which side of the real create call it is stuck on,
// and whether the game was creating a device on another thread at that moment.
HRESULT WINAPI Hook_D3D11CreateDevice(
    IDXGIAdapter* a, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags,
    const D3D_FEATURE_LEVEL* fl, UINT nfl, UINT sdk, ID3D11Device** dev,
    D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
    COTW_LOG("[dev] > D3D11CreateDevice entered (thread %lu)",
             GetCurrentThreadId());
    const HRESULT hr =
        o_D3D11CreateDevice(a, dt, sw, flags, fl, nfl, sdk, dev, got, ctx);
    COTW_LOG("[dev] < D3D11CreateDevice returned 0x%08X", (unsigned)hr);
    if (SUCCEEDED(hr) && dev && *dev) {
        AdoptDevice(*dev, ctx ? *ctx : nullptr, "D3D11CreateDevice");
    }
    return hr;
}

HRESULT WINAPI Hook_D3D11CreateDeviceAndSwapChain(
    IDXGIAdapter* a, D3D_DRIVER_TYPE dt, HMODULE sw, UINT flags,
    const D3D_FEATURE_LEVEL* fl, UINT nfl, UINT sdk,
    const DXGI_SWAP_CHAIN_DESC* scd, IDXGISwapChain** scOut, ID3D11Device** dev,
    D3D_FEATURE_LEVEL* got, ID3D11DeviceContext** ctx) {
    COTW_LOG("[dev] > D3D11CreateDeviceAndSwapChain entered (thread %lu)",
             GetCurrentThreadId());
    const HRESULT hr = o_D3D11CreateDeviceAndSwapChain(
        a, dt, sw, flags, fl, nfl, sdk, scd, scOut, dev, got, ctx);
    COTW_LOG("[dev] < D3D11CreateDeviceAndSwapChain returned 0x%08X",
             (unsigned)hr);
    if (SUCCEEDED(hr) && dev && *dev) {
        AdoptDevice(*dev, ctx ? *ctx : nullptr, "D3D11CreateDeviceAndSwapChain");
    }
    return hr;
}

}  // namespace

void BeginOwnDeviceCreation() { ++g_creatingOwnDevice; }
void EndOwnDeviceCreation()   { if (g_creatingOwnDevice > 0) --g_creatingOwnDevice; }

// *** HOOK THE CONTEXTS BEFORE THE RENDERER IS HOT. ***
//
// Two problems, one cause, and the window resize is what exposed both.
//
// COVERAGE. HookPendingDevices only runs from Present - about 2.6 s in - by
// which time the engine has long since built the deferred contexts it records
// the frame on. We never see them, which is the 0.4%. Resizing the window makes
// the engine rebuild those contexts, our by-then-installed CreateDeferredContext
// hook catches the new ones, and coverage jumps to 194% - the whole frame, twice
// over for stereo. That is the entire difference between a run that works and
// one that does not; the hooking counters are identical in both.
//
// CRASHES. Hooking at first Present means MinHook patches DrawIndexed's prologue
// while the renderer is already calling it. It suspends threads first, but a
// thread sitting inside the bytes being relocated corrupts. Same line, same
// build: one run died at 2.586 s, the next ran 142 s.
//
// Draining from a worker thread from process start fixes both: contexts get
// hooked around 1.3 s, before the renderer is hot and before the engine's own
// contexts have recorded anything. Off the D3D call stack, so it is not the
// deadlock that killed AdoptDevice either.
DWORD WINAPI ContextHookWorker(LPVOID) {
    // Devices appear around 1.25-1.7 s and the first Present lands near 2.2 s,
    // so the window that matters is short. Poll it tightly, then stop: after
    // startup HookPendingDevices at Present is enough, and a thread spinning for
    // the life of the process to catch nothing is just overhead.
    const DWORD start = GetTickCount();
    for (;;) {
        HookPendingDevices();
        if (GetTickCount() - start > 20000) break;
        Sleep(4);
    }
    COTW_LOG("[dev] context-hook worker done - anything created later is caught "
             "by the CreateDeferredContext hook or at Present");
    return 0;
}

bool InstallDeviceCreationHooks() {
    // *** MinHook, EXACTLY AS IN THE BUILD THAT WORKED. ***
    //
    // An IAT patch replaced this for a while, on the theory that the moving
    // startup deaths were a code-patch race. They were not: with the IAT version
    // the game hung INSIDE the driver's own D3D11CreateDeviceAndSwapChain, with
    // none of our code on the stack - the hook's own entry log never printed.
    // The real causes were found and fixed separately (the probe device
    // re-entering us, MethodOrigs sharing one originals array between two index
    // counters, and the Present-time MinHook hooking our own birth-patched
    // detour and recursing forever).
    //
    // So this is back to what demonstrably worked: two MinHook'd exports, and
    // AdoptDevice recording only. Do not swap the mechanism again to fix a bug
    // that lives somewhere else.
    HMODULE d3d11 = GetModuleHandleW(L"d3d11.dll");
    if (!d3d11) {
        COTW_LOG("[dev] d3d11.dll is not mapped yet - device creation NOT hooked");
        return false;
    }
    struct Entry { const char* name; void* detour; void** orig; };
    const Entry table[] = {
        {"D3D11CreateDevice", (void*)&Hook_D3D11CreateDevice,
         (void**)&o_D3D11CreateDevice},
        {"D3D11CreateDeviceAndSwapChain", (void*)&Hook_D3D11CreateDeviceAndSwapChain,
         (void**)&o_D3D11CreateDeviceAndSwapChain},
    };
    int ok = 0;
    for (const Entry& e : table) {
        void* target = (void*)GetProcAddress(d3d11, e.name);
        if (!target) {
            COTW_LOG("[dev] %s not found in d3d11.dll", e.name);
            continue;
        }
        const MH_STATUS s = MH_CreateHook(target, e.detour, e.orig);
        if (s == MH_OK || s == MH_ERROR_ALREADY_CREATED) {
            if (MH_EnableHook(target) == MH_OK) { ++ok; continue; }
        }
        COTW_LOG("[dev] %s hook failed (%d)", e.name, (int)s);
    }
    COTW_LOG("[dev] device creation: %d/2 entry point(s) hooked%s", ok,
             ok ? " - every device from here on is seen at birth"
                : " - devices will only be found late, via the swapchain");
    return ok > 0;
}

// *** MAKE THE ENGINE REBUILD ITS CONTEXTS, ON PURPOSE. ***
//
// Our context hooks go in at first Present, and by then the engine has long
// since built the deferred contexts it records the frame on - so we never see
// them, and only 0.4% of the frame reaches this file. Contexts created AFTER the
// hook exists are caught normally.
//
// The owner found the trigger by accident: resizing the game window took
// coverage from 0.4% to 194% (194% because stereo draws the frame twice and the
// reference capture was flat - i.e. the WHOLE frame), and the gun went from
// flickering to vanishing cleanly. The hooking counters were IDENTICAL either
// side of it - 2 implementations, 15 contexts, 2/2 devices - so nothing about
// our hooking changed. The engine simply rebuilt its contexts and we caught the
// new ones.
//
// So we do the same thing deliberately: nudge the window one pixel and put it
// back. Deliberately NOT ResizeBuffers - that fights the mod's own VR swapchains
// - and deliberately not hooking earlier, which is what crashed startup four
// times tonight. This goes through the game's OWN resize path, exactly like
// doing it by hand, which is the one route already proven to work.
BOOL CALLBACK PickBiggestWindow(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    RECT r{};
    if (!GetClientRect(h, &r)) return TRUE;
    const long area = (r.right - r.left) * (long)(r.bottom - r.top);
    auto* best = reinterpret_cast<std::pair<HWND, long>*>(lp);
    if (area > best->second) { best->first = h; best->second = area; }
    return TRUE;
}

DWORD WINAPI ContextRebuildNudge(LPVOID) {
    // Let the first frames settle. Nudging while the renderer is still standing
    // itself up is exactly the kind of timing we have been punished for.
    Sleep(2500);

    std::pair<HWND, long> best{nullptr, 0};
    EnumWindows(&PickBiggestWindow, reinterpret_cast<LPARAM>(&best));
    HWND w = best.first;
    if (!w) {
        COTW_LOG("[rebuild] no game window found - contexts not rebuilt, coverage "
                 "stays at whatever the engine built before we hooked");
        return 0;
    }
    RECT r{};
    GetWindowRect(w, &r);
    const int ww = r.right - r.left, hh = r.bottom - r.top;
    if (ww <= 1 || hh <= 1) return 0;

    COTW_LOG("[rebuild] nudging the game window %dx%d by one pixel to make the "
             "engine rebuild its deferred contexts", ww, hh);
    SetWindowPos(w, nullptr, 0, 0, ww + 1, hh,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    Sleep(400);
    SetWindowPos(w, nullptr, 0, 0, ww, hh,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    COTW_LOG("[rebuild] window restored to %dx%d - watch the next census: if this "
             "worked, coverage jumps from ~0.4%% to ~194%%", ww, hh);
    return 0;
}

bool InstallCBScan(ID3D11DeviceContext* ctx) {
    if (g_installed || !ctx) return g_installed;

    // Hook the context we were handed - whatever it is - and its device's
    // deferred class. Then, crucially, do the same for EVERY device seen
    // creating shaders: the census showed the game rendering on devices whose
    // contexts we had never touched, which is why 0.3% of the frame reached us.
    HookContextVTable(ctx, "present-path context");

    ID3D11Device* dev = nullptr;
    ctx->GetDevice(&dev);
    if (dev) {
        InstallShaderCreationHooks(dev);
        if (Cfg().hook_deferred_context) HookAllContextsOf(dev, "present-path device");
        dev->Release();
    }
    for (LONG i = 0; i < g_deviceCount && i < kMaxDevices; ++i) {
        if (Cfg().hook_deferred_context) HookAllContextsOf(g_devicesSeen[i], "shader device");
    }

    g_installed = true;
    COTW_LOG("[cb] context hooks installed across %ld implementation(s) from %ld "
             "context(s) offered", (long)g_implsHooked, (long)g_ctxOffered);

    // Now that the hooks exist, make the engine build its contexts again so we
    // actually see the frame. One shot, on its own thread - Present must not
    // block on a window message.
    if (Cfg().rebuild_contexts_on_start) {
        if (HANDLE t = CreateThread(nullptr, 0, &ContextRebuildNudge, nullptr, 0,
                                    nullptr)) {
            CloseHandle(t);
        }
    }
    return true;
}

}  // namespace cotwvr
