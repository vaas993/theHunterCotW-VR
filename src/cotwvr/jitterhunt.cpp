#include "jitterhunt.h"

#include "apex.h"
#include "config.h"
#include "hwbp.h"
#include "log.h"
#include "stereo.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <MinHook.h>
#include <string.h>

// *** FIND WHO COMPOSES THE VIEW-PROJECTION - the jitter hunt, stage 1. ***
//
// The 40x amplification test proved the geometry raster does not consume the
// shared camera block the mod can patch - the per-object WVPs are baked on the
// CPU from a V*P that already carries the engine's 2-phase jitter
// (DLSS_IMPLEMENTATION.md 7e). To take over the jitter, the composer has to be
// found: the CPU code that builds VP each frame.
//
// Method, two stages, both automatic in one run:
//   1. SCAN: the resolve hands this file the exact 64-byte VP the GPU is using
//      this frame. Walk every committed private read-write region and find
//      every copy of its first 48 bytes (m[0..11] - stable while the player
//      stands STILL; m[12..15] carry the per-frame jitter and position, so
//      they are excluded from the match). Those addresses are the CPU-side
//      homes of the matrix: source struct, staging copies, our own snapshots.
//   2. WATCH: arm a write-only hardware breakpoint on m[12] of the first two
//      finds. The report then names the WRITER instructions - an engine
//      matrix-compose (movss/movups from math code) or a memcpy (staging /
//      our own pool), distinguishable at a glance from the RVAs.
//
// The player must STAND STILL during the scan: head on the table, hands off.
// Movement changes m[0..11] between the sample and the memory being walked,
// and the scan honestly finds nothing until stillness returns.

namespace cotwvr {
namespace {

constexpr SIZE_T kChunk = 64u * 1024u * 1024u;   // per-frame scan budget
constexpr int    kMaxFinds = 8;

uintptr_t g_cursor = 0x10000;
uint64_t  g_scanned = 0;
int       g_pass = 0;
void*     g_finds[kMaxFinds];
int       g_findCount = 0;
bool      g_armed = false;
bool      g_inited = false;

bool OwnModuleContains(const void* p) {
    static HMODULE self = nullptr;
    if (!self) {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&OwnModuleContains), &self);
        if (!self) return false;
    }
    // Cheap containment: VirtualQuery names the allocation the address is in.
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    return mbi.AllocationBase == reinterpret_cast<void*>(self);
}

void ScanChunk(const float* vp) {
    SIZE_T budget = kChunk;
    const uint32_t want0 = *reinterpret_cast<const uint32_t*>(&vp[0]);

    while (budget > 0) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery(reinterpret_cast<void*>(g_cursor), &mbi, sizeof(mbi))) {
            // Past the end of user space: one full pass done.
            ++g_pass;
            g_cursor = 0x10000;
            return;
        }
        const uintptr_t regBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regEnd = regBase + mbi.RegionSize;

        const bool scannable =
            mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            (mbi.Protect == PAGE_READWRITE || mbi.Protect == PAGE_EXECUTE_READWRITE);

        if (!scannable) {
            g_cursor = regEnd;
            continue;
        }

        uintptr_t from = (g_cursor > regBase) ? g_cursor : regBase;
        from = (from + 3) & ~(uintptr_t)3;
        uintptr_t to = regEnd;
        if (to - from > budget) to = from + budget;

        __try {
            // 48 bytes must fit; the last 44 bytes of the window are covered by
            // the next chunk overlapping backwards would complicate the cursor,
            // so a find sitting exactly on a chunk boundary can be missed - a
            // second pass catches it, and passes repeat until stillness.
            for (uintptr_t p = from; p + 48 <= to; p += 4) {
                if (*reinterpret_cast<const uint32_t*>(p) != want0) continue;
                if (memcmp(reinterpret_cast<const void*>(p), vp, 48) != 0) continue;
                void* addr = reinterpret_cast<void*>(p);
                bool dup = false;
                for (int i = 0; i < g_findCount; ++i)
                    if (g_finds[i] == addr) { dup = true; break; }
                if (!dup && g_findCount < kMaxFinds) {
                    g_finds[g_findCount++] = addr;
                    COTW_LOG("[hunt] VP copy #%d at %p (region %p +0x%llX, prot "
                             "0x%X)%s",
                             g_findCount, addr, mbi.BaseAddress,
                             (unsigned long long)(p - regBase), mbi.Protect,
                             OwnModuleContains(addr) ? "  [inside our own module]"
                                                     : "");
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            // A region that faults mid-walk is simply skipped.
        }

        g_scanned += (to - from);
        budget -= (SIZE_T)(to - from);
        g_cursor = to;
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// *** THE TAKEOVER: our 8-phase Halton, generated where the engine generates
//     its 2-phase - upstream of everything. ***
//
// The generator at apex::kJitterGenerator is the single source every consumer
// derives from (per-object WVPs, the shared block, the compensating passes).
// The detour replaces its output with our own sequence in the engine's own
// units and matrix shape, indexed by the engine's own frame counter - so the
// per-render phase semantics (each eye pass advances the counter, exactly as
// stock) are preserved, and the value each eye rendered with is recorded for
// exact reporting to DLSS. No estimator anywhere in the loop.
//
// Halton(2,3), 8 entries, centred - the full +/-0.5 px budget, because unlike
// the failed shared-block overlay this is applied and compensated everywhere.
constexpr float kTakeX[8] = { 0.0000f, -0.2500f,  0.2500f, -0.3750f,
                              0.1250f, -0.1250f,  0.3750f, -0.4375f};
constexpr float kTakeY[8] = {-0.1667f,  0.1667f, -0.3889f, -0.0556f,
                              0.2778f, -0.2778f,  0.0556f,  0.3889f};

using PFN_JitterGen = bool(__fastcall*)(void*, float*, uint32_t, uint32_t);
PFN_JitterGen o_JitterGen = nullptr;
volatile LONG g_takeCalls = 0;
float g_takeLast[2][2] = {};

bool __fastcall Hook_JitterGen(void* obj, float* m, uint32_t w, uint32_t h) {
    if (!Cfg().jitter_take || !m || w < 64 || h < 64)
        return o_JitterGen(obj, m, w, h);

    // The engine's own counter, so phase advances per RENDER exactly like the
    // stock modes (both eyes of a frame land on consecutive phases).
    uint32_t ctr = 0;
    __try {
        ctr = *reinterpret_cast<volatile uint32_t*>(
            static_cast<uint8_t*>(apex::Rva(0)) + apex::kJitterFrameCounter);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return o_JitterGen(obj, m, w, h);
    }
    const uint32_t k = ctr & 7u;
    const float s = Cfg().jitter_take_scale;
    const float px = s * kTakeX[k];
    const float py = s * kTakeY[k];

    // The exact matrix shape the stock generator emits: identity with the NDC
    // offset in row3. Pixel -> NDC: x scales by 2/w; y by 2/h with the sign
    // flipped (NDC y is up, our pixel convention's y is down).
    memset(m, 0, 64);
    m[0] = m[5] = m[10] = m[15] = 1.0f;
    m[12] = 2.0f * px / (float)w;
    m[13] = -2.0f * py / (float)h;

    const int eye = (CurrentRenderEye() == 0) ? 0 : 1;
    g_takeLast[eye][0] = px;
    g_takeLast[eye][1] = py;
    InterlockedIncrement(&g_takeCalls);
    return true;                         // jitter active
}

// *** THE HOT PATH IS INLINED - so the takeover detour catches only a cold
//     caller (~33 calls/s against 90+ fps, measured). These two replace it,
//     both DATA-level, no code patched: ***
//
// 1. Read the jitter straight out of the composed projection. P_jittered =
//    P * J, and with P's row2 = (0,0,0,1), the jittered row2 becomes exactly
//    (jx, jy, 0, 1) - so [Q+0x2B4] and [Q+0x2B8] ARE this camera's jitter in
//    NDC, whatever mode generated it. Q = [[0x02718A78]+0x5C8], maintained by
//    the engine per compose.
bool JitterReadCurrent(unsigned w, unsigned h, float* px, float* py) {
    __try {
        uint8_t* base = static_cast<uint8_t*>(apex::Rva(0));
        if (!base) return false;
        uint8_t* p1 = *reinterpret_cast<uint8_t**>(base + 0x02718A78);
        if (!p1) return false;
        uint8_t* q = *reinterpret_cast<uint8_t**>(p1 + 0x5C8);
        if (!q) return false;
        const float jx = *reinterpret_cast<float*>(q + 0x2B4);
        const float jy = *reinterpret_cast<float*>(q + 0x2B8);
        // NDC -> pixels, MV convention (y down). Sanity: a jitter can never
        // legally exceed a pixel; more means this is not a jittered camera.
        const float ox = jx * 0.5f * (float)w;
        const float oy = -jy * 0.5f * (float)h;
        if (!(ox > -1.5f && ox < 1.5f && oy > -1.5f && oy < 1.5f)) return false;
        *px = ox;
        *py = oy;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 2. Force the engine's OWN 16-phase mode: the generator (inlined twin
//    included) branches on the AA mode at [obj+0x3D8], obj =
//    [[[0x02718A98]+0x1410]+0x1A8]. Mode 2 = the shipped 2-phase, mode 3 =
//    a 16-entry table the game's settings never enable. Written every frame,
//    because the engine may refresh it from settings.
void JitterMode3Tick() {
    if (!Cfg().jitter_mode3) return;
    __try {
        uint8_t* base = static_cast<uint8_t*>(apex::Rva(0));
        if (!base) return;
        uint8_t* p1 = *reinterpret_cast<uint8_t**>(base + 0x02718A98);
        if (!p1) return;
        uint8_t* p2 = *reinterpret_cast<uint8_t**>(p1 + 0x1410);
        if (!p2) return;
        uint8_t* obj = *reinterpret_cast<uint8_t**>(p2 + 0x1A8);
        if (!obj) return;
        volatile int* mode = reinterpret_cast<volatile int*>(obj + 0x3D8);
        static bool logged = false;
        if (!logged) {
            logged = true;
            COTW_LOG("[take] AA/jitter mode: %d -> 3 (the engine's own "
                     "16-phase table). Re-applied every frame.", *mode);
        }
        if (*mode != 3) *mode = 3;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        static bool moaned = false;
        if (!moaned) {
            moaned = true;
            COTW_LOG("[take] jitter_mode3: pointer chain faulted - leaving the "
                     "mode alone.");
        }
    }
}

bool JitterTakeNow(int eye, float* px, float* py) {
    if (!Cfg().jitter_take || !o_JitterGen || !g_takeCalls) return false;
    eye = (eye == 0) ? 0 : 1;
    *px = g_takeLast[eye][0];
    *py = g_takeLast[eye][1];
    return true;
}

long JitterTakeCallsLastSecond() {
    return (long)InterlockedExchange(&g_takeCalls, 0);
}

void JitterTakeInstall() {
    static bool tried = false;
    if (tried || !Cfg().jitter_take) return;
    tried = true;
    void* target = apex::Rva(apex::kJitterGenerator);
    if (!target) return;
    MH_STATUS st = MH_CreateHook(target, &Hook_JitterGen, (void**)&o_JitterGen);
    if (st != MH_OK) {
        COTW_LOG("[take] MH_CreateHook(JitterGen @0x%X) failed: %d",
                 apex::kJitterGenerator, (int)st);
        return;
    }
    if (MH_EnableHook(target) != MH_OK) {
        COTW_LOG("[take] MH_EnableHook(JitterGen) failed");
        return;
    }
    COTW_LOG("[take] jitter takeover installed at 0x%X - the engine now "
             "renders the mod's 8-phase Halton, applied and compensated "
             "everywhere the stock 2-phase was.", apex::kJitterGenerator);
}

void JitterHuntOnCapture(int eye, const float* vp16) {
    if (!Cfg().jitter_hunt || eye != 0 || !vp16) return;
    if (!g_inited) {
        g_inited = true;
        HwbpInit();
        if (Cfg().jitter_hunt == 1)
            COTW_LOG("[hunt] jitter hunt armed. STAND STILL - head on the "
                     "table, hands off - until the log says the watch is set. "
                     "The scan matches the matrix's stable 48 bytes, and "
                     "movement changes them faster than memory can be walked.");
    }
    if (g_armed) return;

    // Round 2: the true home is known from round 1's disassembly - the object
    // at [[0x02718A78]+0x5C8], VP at +0x184, its jitter row at +0x1C4 (what
    // the staging writer at +0x1308E5..+0x130903 copies). Verify the matrix is
    // really there (memcmp against the GPU truth we were just handed), then
    // watch who WRITES it - that is the composer.
    if (Cfg().jitter_hunt == 2) {
        __try {
            uint8_t* base = static_cast<uint8_t*>(apex::Rva(0));
            if (!base) return;
            uint8_t* p1 = *reinterpret_cast<uint8_t**>(base + 0x02718A78);
            if (!p1) return;
            uint8_t* q = *reinterpret_cast<uint8_t**>(p1 + 0x5C8);
            if (!q) return;
            // The home holds the CURRENT eye's matrix; compare the stable 48
            // bytes only (the jitter row differs between the copy moments).
            const bool match = memcmp(q + 0x194, vp16, 48) == 0;
            static int misses = 0;
            if (!match && ++misses < 300) return;   // wrong moment or wrong eye - retry
            g_armed = true;
            const int off = Cfg().jitter_hunt_offset;
            COTW_LOG("[hunt] camera-data object %s at %p (chain "
                     "[[0x02718A78]+0x5C8]) - watching +0x%X.",
                     match ? "VERIFIED" : "NOT verified after 300 tries, watching anyway",
                     q, off);
            HwbpWatch(q + off, "home+off");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            COTW_LOG("[hunt] pointer chain faulted - is the build the same?");
            g_armed = true;   // do not fault every frame
        }
        return;
    }

    // Two homes are the goal; after three full passes settle for one rather
    // than scan forever (the matrix may genuinely live in a single place).
    if (g_findCount >= 2 || (g_pass >= 3 && g_findCount >= 1)) {
        // Two homes found - watch m[12] (offset +0x30) of both for writers.
        g_armed = true;
        HwbpWatch(reinterpret_cast<uint8_t*>(g_finds[0]) + 0x30, "vp.m12 #1");
        if (g_findCount >= 2)
            HwbpWatch(reinterpret_cast<uint8_t*>(g_finds[1]) + 0x30, "vp.m12 #2");
        COTW_LOG("[hunt] WATCHING m[12] of finds #1 and #2. Keep still ~10 s, "
                 "then play normally; the [hwbp] report below names the "
                 "writers.");
        return;
    }
    ScanChunk(vp16);
}

void JitterHuntTick() {
    if (!Cfg().jitter_hunt) return;
    static ULONGLONG lastLog = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - lastLog >= 2000) {
        lastLog = now;
        if (!g_armed)
            COTW_LOG("[hunt] scanning... pass %d, %.1f GB walked, %d find(s)",
                     g_pass, g_scanned / 1073741824.0, g_findCount);
    }
    HwbpReport();
}

}  // namespace cotwvr
