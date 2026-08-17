#include "frame_hook.h"

#include "apex.h"
#include "camera_probe.h"
#include "config.h"
#include "log.h"
#include "render_hook.h"
#include "stereo.h"
#include "vr.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <emmintrin.h>

#include <cmath>
#include <intrin.h>          // _ReturnAddress - identifies WHICH caller is asking

#include <MinHook.h>

namespace cotwvr {
namespace {

// The engine's per-frame sequence, read off the frame driver at 0x007BA6CF:
//
//     lea  rdx, [rsp+0x40]        ; a caller-owned description buffer
//     mov  rcx, rdi               ; this
//     call 0x1407C0DE0            ; BUILD  - the camera lever runs inside here
//     ...
//     lea  rcx, [rsp+0x40]
//     call 0x14080C780            ; SUBMIT - actually draws
//     mov  rcx, rdi
//     call 0x140797420            ; PRESENT
//
// So rendering a second eye is NOT "call the build again" - the build only
// fills a description that the submit consumes, and calling it twice would just
// overwrite that description. The second eye is the BUILD+SUBMIT PAIR replayed,
// in the engine's own order, with the engine's own arguments.
//
// void __fastcall DrawScene(void* this, void* desc, ...)
//   TWO arguments - the prologue reads both:
//       7C0DF9  mov rdi, rdx
//       7C0DFC  mov rsi, rcx
//   Declaring it with one let the compiler clobber RDX before the pass-through,
//   handing the original garbage on EVERY call. Four register args are declared
//   and forwarded so nothing is guessed.
//
// void __fastcall SubmitScene(void* desc)
//   One argument: `push rbx; sub rsp,0x20; mov rbx,rcx`.
// void __fastcall PresentPath(void* this, float arg)
//
// Post-processing + resolve to the backbuffer + present. A COMPLETE eye image
// only exists after this has run.
//
// The second argument arrives in XMM1 - proven by the prologue:
//     797432  mov    rbx, rcx      ; this
//     797442  movaps xmm13, xmm1   ; the argument, saved off
//
// It MUST be declared `float`, not `__m128`. MSVC's x64 ABI passes __m128 by
// POINTER, so declaring it that way silently changes the calling convention and
// corrupts the call on every frame - which crashed the game the moment the hook
// was installed, before full-rate had even engaged.
// The engine's elapsed-time helper at 0x000EB040. ONE argument - both call
// sites read set only rcx:
//     503283  mov rcx, [rip+0x219CD66]   -> the global time object
//     50328A  call 0x1400EB040
// It reads QueryPerformanceCounter, subtracts [rcx+0x70], and stores the new
// timestamp back there - so EVERY object it is called on has its clock advanced.
// Restoring one global was not enough (the world came right but the player and
// UI stayed slow), so this catches whichever objects actually tick, whatever
// they are, instead of guessing at more addresses.
using PFN_TimerTick   = uint64_t(__fastcall*)(void*);
using PFN_ClockUpdate = void(__fastcall*)(void*);
// (this, bool) -> float in xmm0. Both call sites set rcx and dl only.
using PFN_TimeGetter  = float(__fastcall*)(void*, unsigned char);
using PFN_DrawScene   = void(__fastcall*)(void*, void*, void*, void*);
using PFN_SubmitScene = void(__fastcall*)(void*);
using PFN_PresentPath = void(__fastcall*)(void*, float);

PFN_TimerTick   o_TimerTick = nullptr;
PFN_ClockUpdate o_ClockUpdate = nullptr;
PFN_TimeGetter  o_TimeGetter = nullptr;
PFN_TimeGetter  o_TimeGetter2 = nullptr;
float g_realFrameDelta = 0.0f;   // measured between consecutive real frames
PFN_DrawScene   o_DrawScene = nullptr;
PFN_SubmitScene o_SubmitScene = nullptr;
PFN_PresentPath o_PresentPath = nullptr;

bool g_installed = false;
bool g_disabledByFault = false;

volatile LONG g_pass = 0;
volatile LONG g_inDouble = 0;
uint64_t g_frames = 0;
uint64_t g_sceneCalls = 0;
uint64_t g_stashFailures = 0;

// Captured from the build hook so the replay uses exactly what the engine did.
void* g_lastThis = nullptr;
void* g_lastDesc = nullptr;
void* g_lastA3 = nullptr;
void* g_lastA4 = nullptr;

// True for the whole of a full-rate frame, from just before the first eye's
// present until the second eye's has been consumed. The DXGI Present hook reads
// this to decide whether it owns the OpenXR frame or the replay does.
volatile LONG g_driving = 0;

// True ONLY while the second eye is being replayed - not during the first eye's
// own present path.
//
// g_inDouble covers both, because as a re-entrancy guard it must. Reusing it for
// the clock policy meant the getter returned ZERO for the whole of eye 0's
// normal present path as well, so a large part of every real frame was told no
// time had passed. That is why no threshold could separate the world from the
// player: they were both being starved in the same window, and the threshold was
// only trading which of them got compensated.
volatile LONG g_inReplay = 0;

// Full-rate must not engage during startup. The first crash engaged it 0.712 s
// in - the instant the OpenXR session went live - while the engine and Steam
// were still coming up.
bool WarmedUp() {
    return g_sceneCalls > (uint64_t)Cfg().full_rate_warmup_frames;
}

// How much of the engine's time object to snapshot. The layout is not fully
// known - only that the delta lives at +0x14 and the scaled one at +0x2C - so
// the whole head of the object is taken rather than guessing where it ends.
// 0x80, not 0x40. The old window stopped at 0x40 and therefore MISSED the tick's
// baseline timestamp at +0x70 - the one piece of state that actually mattered.
// With it included, the engine's own next tick measures the full real frame and
// every delta it feeds (+0x14, +0x20, +0x2C) comes out right together.
constexpr uint32_t kTimeSnapshotBytes = 0x80;

// The object itself, through the global pointer. Null until the engine has
// built it, and it is read fresh every frame rather than cached: a pointer
// captured during startup and later reallocated would have us restoring bytes
// into whatever moved in.
// Wall-clock seconds, for measuring what a frame REALLY took.
double NowSeconds() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart) / double(freq.QuadPart);
}

double g_prevFrameSeconds = 0.0;
float  g_engineDtAtFrameStart = -1.0f;

// The engine keeps TWO deltas: raw at +0x14 and scaled at +0x2C, the scaled one
// being raw times whatever time dilation is in effect. Restoring the time object
// fixes the raw one - the world runs correctly - but the scaled one still comes
// out at half, and the player and the UI are what read it. Measured on one
// frame: raw 0.01010 s, scaled 0.00556 s.
//
// The ratio between them is the game's own dilation and is nothing to do with
// us, so it is SAMPLED while full-rate is not driving (during the 20-second
// warm-up, and any time it disengages) rather than assumed to be 1.0 - the game
// uses slow motion for its own effects and forcing 1.0 would break those.
float g_timeScaleRatio = 1.0f;

// Every timer object touched during the replay, snapshotted on first touch and
// put back afterwards. One global was not enough; this makes no assumption about
// how many there are or where they live.
constexpr int kMaxTimers = 12;
struct TimerSnap {
    void* obj;
    uint8_t bytes[kTimeSnapshotBytes];
};
TimerSnap g_timers[kMaxTimers];
int  g_timerCount = 0;
bool g_collecting = false;

// --- freezing time for the duration of the replay --------------------------
//
// The engine reads the performance counter from several places per frame, so
// repairing any one stored delta afterwards always left another consumer wrong.
// Holding the counter still for the replay removes the problem instead of
// chasing it: every elapsed-time computation anywhere in the engine yields zero,
// which is exactly the truth - the second eye is the same instant as the first.
//
// Scoped tightly: only while the replay is running, and only on the thread
// running it, so nothing else in the process sees a stopped clock.
using PFN_QPC = BOOL(WINAPI*)(LARGE_INTEGER*);
PFN_QPC o_QueryPerformanceCounter = nullptr;

volatile LONG g_freezeThread = 0;
LARGE_INTEGER g_frozenCounter{};
volatile LONG g_frozenHits = 0;

BOOL WINAPI Hook_QPC(LARGE_INTEGER* out) {
    const LONG owner = g_freezeThread;
    if (owner && out && (DWORD)owner == GetCurrentThreadId()) {
        *out = g_frozenCounter;
        InterlockedIncrement(&g_frozenHits);
        return TRUE;
    }
    return o_QueryPerformanceCounter(out);
}

// Hand every consumer the true frame interval while full-rate is driving.
//
// The getter returns the fixed 1/30 timestep on one branch and the frame delta
// on the other; only the second is ours to correct. Everything else - paused
// state, the fixed step - passes through untouched.
uint8_t* TimeObjectFast();
uint8_t* TimeObject();

// Both getters answer the same two questions, so they share one policy:
//
//   during the REPLAY  -> ZERO. No time passes between one eye and the other,
//                         and handing out a full frame here is what made the
//                         world advance twice per frame and run fast.
//   during a real frame -> the true measured interval, whatever the stored
//                          field happens to hold at this point in the frame.
float PatchedDelta(void* self, unsigned char flag, float engine) {
    if (!Cfg().full_rate_patch_delta_getter || !FullRateActive()) return engine;
    if (g_realFrameDelta <= 0.0f) return engine;

    // ONLY the one global time object we have actually identified.
    //
    // This getter is a generic method - it can be called on any instance of its
    // class. Reading +0x30/+0x31, and especially WRITING +0x20, on whatever
    // object happened to be passed was an unguarded stray write into unrelated
    // memory: it corrupts quietly and crashes somewhere else later, which is
    // exactly the pattern of the last two crashes.
    uint8_t* obj = static_cast<uint8_t*>(self);
    if (!obj || obj != TimeObjectFast()) return engine;

    // Leave the paused branch and the fixed-timestep branch exactly as they are.
    if (obj[apex::kTimePausedOffset] != 0 && flag == 0) return engine;
    if (obj[apex::kTimeUseScaledOffset] != 0) return engine;

    // During the REPLAY only: zero. No time passes between one eye and the
    // other. Eye 0's own present path is a normal part of the frame and must
    // keep seeing normal time.
    if (g_inReplay) return 0.0f;

    // WHICH CALLER, not which value.
    //
    // A threshold cannot work here, and the measurement that proved it is a
    // clean switch at exactly 0.5 with the two subsystems always on opposite
    // sides of it. That means they read the SAME value and need OPPOSITE
    // treatment:
    //
    //   the world  reads twice per real frame and SUMS  -> two halves are
    //              already correct, so correcting it gives it two full frames
    //   the player reads once                            -> one half, so it
    //                                                       needs correcting
    //
    // The one thing that distinguishes the callers is this flag. The engine asks
    // both ways - `xor edx,edx` at one site, `mov dl,1` at another - and the
    // difference is whether the caller wants the clock to respect the paused
    // state. Different subsystems care about that differently, so it is a real
    // discriminator where the value was not.
    const int mode = Cfg().full_rate_patch_flag;
    if (mode == 0 && flag != 0) return engine;   // only the pause-respecting reads
    if (mode == 1 && flag == 0) return engine;   // only the pause-ignoring reads

    const float frag = Cfg().full_rate_fragment_threshold;
    if (engine < g_realFrameDelta * frag) return g_realFrameDelta;
    return engine;
}

// WHO is asking. The exact discriminator, and the one that should have been used
// several attempts ago: value does not separate the world from the player, and
// neither does the flag, but they are different code and therefore call from
// different addresses.
constexpr int kMaxCallers = 24;
struct GetterCaller {
    uint32_t rva;
    volatile LONG callsThisFrame;
    volatile LONG callsTotal;
    float lastEngineValue;
};
GetterCaller g_callers[kMaxCallers];
volatile LONG g_callerCount = 0;

GetterCaller* NoteCaller(uint32_t rva, float engineValue) {
    const LONG n = g_callerCount;
    for (LONG i = 0; i < n && i < kMaxCallers; ++i) {
        if (g_callers[i].rva == rva) {
            InterlockedIncrement(&g_callers[i].callsThisFrame);
            InterlockedIncrement(&g_callers[i].callsTotal);
            g_callers[i].lastEngineValue = engineValue;
            return &g_callers[i];
        }
    }
    if (n >= kMaxCallers) return nullptr;
    const LONG idx = InterlockedIncrement(&g_callerCount) - 1;
    if (idx >= kMaxCallers) return nullptr;
    g_callers[idx].rva = rva;
    g_callers[idx].callsThisFrame = 1;
    g_callers[idx].callsTotal = 1;
    g_callers[idx].lastEngineValue = engineValue;
    return &g_callers[idx];
}

void ReportGetterCallers() {
    const LONG n = g_callerCount;
    COTW_LOG("[clock] delta getter callers this frame (real frame %.2f ms):",
             g_realFrameDelta * 1000.0f);
    for (LONG i = 0; i < n && i < kMaxCallers; ++i) {
        COTW_LOG("[clock]   call site +0x%06X : %ld this frame, %ld total, last value "
                 "%.2f ms", g_callers[i].rva, (long)g_callers[i].callsThisFrame,
                 (long)g_callers[i].callsTotal, g_callers[i].lastEngineValue * 1000.0f);
    }
    COTW_LOG("[clock]   ^ a site called TWICE a frame is summing halves and is already "
             "right; a site called ONCE is short and is the one to correct.");
}

void ResetGetterCallerFrame() {
    const LONG n = g_callerCount;
    for (LONG i = 0; i < n && i < kMaxCallers; ++i) {
        InterlockedExchange(&g_callers[i].callsThisFrame, 0);
    }
}

float __fastcall Hook_TimeGetter(void* self, unsigned char flag) {
    const float engine = o_TimeGetter(self, flag);

    uint32_t callerRva = 0;
    {
        const uintptr_t base = apex::Base();
        const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
        if (base && ret >= base && (ret - base) < apex::ImageSize()) {
            callerRva = static_cast<uint32_t>(ret - base);
            NoteCaller(callerRva, engine);
        }
    }

    float out = PatchedDelta(self, flag, engine);

    // Exactly TWO callers exist, both in the frame driver, each asking once per
    // frame and each getting half the real frame:
    //     +0x7B9E5B  the dl=0 read, fed to build / submit / present
    //     +0x7B9E76  the dl=1 companion read
    // The world and the player must consume different ones, so correcting the
    // right one alone should fix the player without inflating the world.
    switch (Cfg().full_rate_patch_callsite) {
        case 1: if (callerRva != apex::kDeltaReadA) out = engine; break;
        case 2: if (callerRva != apex::kDeltaReadB) out = engine; break;
        case 3: out = engine; break;   // correct nothing - the control
        default: break;                // both
    }

    // Keep the OTHER delta field in step from here, rather than hooking its
    // getter - that hook crashed the game. Guarded to the one identified global:
    // an unguarded write here was itself a crash source.
    if (out != engine && self && static_cast<uint8_t*>(self) == TimeObjectFast()) {
        *reinterpret_cast<float*>(static_cast<uint8_t*>(self) +
                                  apex::kTimeDelta3Offset) = out;
    }

    // What a CONSUMER actually receives - which is the only number worth
    // reporting now that the correction happens at the read rather than in
    // storage. The perf line was still sampling the stored field and calling it
    // "what the engine thinks", which is no longer what anything reads.
    if (!g_inReplay) g_engineDtAtFrameStart = out;
    static uint64_t n = 0;
    const int every = Cfg().stereo_log_every;
    if (out != engine && every > 0 && (++n % ((uint64_t)every * 20)) == 1) {
        COTW_LOG("[clock] delta getter: %.2f ms -> %.2f ms (%s)", engine * 1000.0f,
                 out * 1000.0f, InDoubleRender() ? "REPLAY, no time passes"
                                                 : "real frame");
    }
    return out;
}

// The engine's OTHER delta getter, returning +0x20. Different subsystems read
// different ones - the in-game UI stayed slow while the player came right
// precisely because only the first getter was corrected.
float __fastcall Hook_TimeGetter2(void* self, unsigned char flag) {
    const float engine = o_TimeGetter2(self, flag);
    return PatchedDelta(self, flag, engine);
}

// The replay's clock update, suppressed at source. Nothing downstream of it
// then needs repairing - not the raw delta, not the scaled one, not the
// baseline, and not whatever else it touches that has not been found yet.
void __fastcall Hook_ClockUpdate(void* self) {
    if (g_inDouble && Cfg().full_rate_skip_clock_update) {
        static int logged = 0;
        if (logged < 3) {
            ++logged;
            COTW_LOG("[clock] clock update SKIPPED for the second eye - the replay no "
                     "longer advances the engine's time at all");
        }
        return;
    }
    o_ClockUpdate(self);
}

uint64_t __fastcall Hook_TimerTick(void* self) {
    // Only while replaying the second eye. Outside that this is a pure
    // pass-through and costs one predictable branch.
    if (g_collecting && self && g_timerCount < kMaxTimers) {
        bool known = false;
        for (int i = 0; i < g_timerCount; ++i) {
            if (g_timers[i].obj == self) { known = true; break; }
        }
        // Only the one identified object - this is a generic method and `self`
        // can be anything. Copying 0x80 bytes out of an arbitrary object, and
        // later writing them back, was another stray-memory hazard.
        if (!known && self == TimeObject()) {
            g_timers[g_timerCount].obj = self;
            memcpy(g_timers[g_timerCount].bytes, self, kTimeSnapshotBytes);
            ++g_timerCount;
        }
    }
    return o_TimerTick(self);
}

void RestoreTickedTimers() {
    for (int i = 0; i < g_timerCount; ++i) {
        __try { memcpy(g_timers[i].obj, g_timers[i].bytes, kTimeSnapshotBytes); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    static int logged = 0;
    if (logged < 3) {
        ++logged;
        COTW_LOG("[clock] the replay ticked %d timer object(s); all restored. "
                 "More than one is why fixing the global alone left the player "
                 "and UI slow while the world came right.", g_timerCount);
    }
}

// Just the pointer, no validation. The delta getter runs several times per
// frame and only needs to compare identity - putting IsBadReadPtr on that path
// would be both slow and pointless.
uint8_t* TimeObjectFast() {
    void** slot = static_cast<void**>(apex::Rva(apex::kTimeObjectPtr));
    return slot ? static_cast<uint8_t*>(*slot) : nullptr;
}

// Validated, and NOT with IsBadReadPtr.
//
// That call was on a per-frame path here, and Microsoft documents it as able to
// crash the process: it probes the memory, which can trip the guard pages
// Windows uses to grow thread stacks. Non-deterministic crashes with a useless
// dump, sometimes minutes apart, is precisely its signature - and that is what
// this session spent four launches chasing.
//
// VirtualQuery asks the kernel about the mapping instead of touching it, and
// the answer is cached because the object does not move.
uint8_t* TimeObject() {
    uint8_t* obj = TimeObjectFast();
    if (!obj) return nullptr;

    static uint8_t* validated = nullptr;
    if (obj == validated) return obj;

    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(obj, &mbi, sizeof(mbi))) return nullptr;
    if (mbi.State != MEM_COMMIT) return nullptr;
    const DWORD writable = PAGE_READWRITE | PAGE_EXECUTE_READWRITE |
                           PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
    if (!(mbi.Protect & writable)) return nullptr;
    const uintptr_t end = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    if (end < reinterpret_cast<uintptr_t>(obj) + kTimeSnapshotBytes) return nullptr;

    validated = obj;
    return obj;
}

// Replay BUILD + SUBMIT + PRESENT-PATH for the second eye.
//
// The present path has to be included: it is where post-processing resolves
// into the backbuffer, so without it there is no complete second image - which
// is exactly why the previous attempt produced no depth despite the eye offset
// demonstrably being applied twice per frame.
bool ReplaySecondEye(void* self, float arg) {
    if (!g_lastDesc || !o_DrawScene || !o_SubmitScene || !o_PresentPath) return false;

    // The float argument is the FRAME DELTA TIME in seconds. That was read off
    // the frame driver, not guessed:
    //
    //     7B9E56  call 0x1400EB270          ; a timer accessor, returns xmm0
    //     7B9E5B  movaps xmm9, xmm0
    //     7B9E5F  minss  xmm9, [0x19387E0]  ; = 0.5f, the classic dt clamp
    //     ...
    //     7BA642  movss  [rsp+0x40], xmm9   ; dt is also field 0 of the DESC
    //     7BA6FA  movaps xmm1, xmm9         ; and the present path's argument
    //
    // So the world is told how much time passed by two routes, and the second
    // eye must be told ZERO on both. That is the whole "slow motion" problem
    // stated properly: the replay was reporting a second real frame's worth of
    // elapsed time, so the engine advanced the world twice per pair and then
    // divided its own rate to compensate. Zero is not a hack - it is the
    // truthful answer, because no time passes between one eye and the other.
    // --- the world's SECOND advance ---------------------------------------
    //
    // The replay re-runs BUILD with the engine's own description buffer, and the
    // driver writes the frame delta into that buffer:
    //     7BA642  movss [rsp+0x40], xmm9
    // So the world is advanced once by the real frame and once more by the
    // replay, using the same delta both times. With an uncorrected half-frame
    // that happens to come out right, which is why the world looked normal while
    // the player was slow - and why correcting the read fixed the player and
    // made the world run at double speed.
    //
    // The old guard only ever looked at FIELD 0, found a fixed 1/30 there and
    // correctly declined to zero it - and then looked no further. Scan the
    // buffer for whichever fields actually hold this frame's delta, zero those
    // for the replay, and put them back afterwards.
    constexpr int kDescWords = 64;                 // 0x00 .. 0x100
    float* descWords = static_cast<float*>(g_lastDesc);
    float savedDesc[kDescWords];
    int   descHits[kDescWords];
    int   descHitCount = 0;
    if (Cfg().full_rate_zero_desc_dt && descWords && arg > 1e-6f) {
        __try {
            for (int i = 0; i < kDescWords; ++i) {
                const float v = descWords[i];
                if (isfinite(v) && fabsf(v - arg) < arg * 0.02f) {
                    savedDesc[descHitCount] = v;
                    descHits[descHitCount] = i;
                    ++descHitCount;
                    descWords[i] = 0.0f;           // no time passes for the replay
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            descHitCount = 0;
        }
        static int logged = 0;
        if (logged < 3) {
            ++logged;
            char list[128] = "";
            int n = 0;
            for (int i = 0; i < descHitCount && n < (int)sizeof(list) - 8; ++i) {
                n += snprintf(list + n, sizeof(list) - n, " +0x%X", descHits[i] * 4);
            }
            COTW_LOG("[fullrate] description fields holding this frame's delta (%.2f ms):"
                     "%s - zeroed for the replay so the world advances ONCE",
                     arg * 1000.0f, descHitCount ? list : " (none found)");
        }
    }

    const bool zeroDt = Cfg().full_rate_zero_dt;
    float* descDt = static_cast<float*>(g_lastDesc);
    float savedDt = 0.0f;

    static bool loggedDt = false;
    if (!loggedDt) {
        loggedDt = true;
        COTW_LOG("[fullrate] delta time this frame: present-path argument %.5f s, "
                 "description field 0 = %.5f s %s",
                 arg, *descDt,
                 fabsf(arg - *descDt) < 1e-6f
                     ? "- they MATCH, so field 0 is confirmed to be dt"
                     : "- they DIFFER, so field 0 is something else; not zeroing it");
    }
    // Only touch field 0 if it really is the same dt the caller was handed;
    // blindly zeroing the first word of a structure we have not identified is
    // how a renderer gets corrupted.
    const bool descIsDt = fabsf(arg - *descDt) < 1e-6f;
    if (zeroDt && descIsDt) { savedDt = *descDt; *descDt = 0.0f; }

    // Belt and braces: the frame-counter globals, restored around the replay in
    // case anything else in the present path treats a call as a frame.
    volatile uint32_t* clockA =
        static_cast<volatile uint32_t*>(apex::Rva(apex::kFrameClockA));
    volatile uint32_t* clockB =
        static_cast<volatile uint32_t*>(apex::Rva(apex::kFrameClockB));
    const bool freeze = Cfg().full_rate_freeze_clock && clockA && clockB;
    uint32_t savedA = 0, savedB = 0;
    if (freeze) { savedA = *clockA; savedB = *clockB; }

    // The engine's global time object. Whatever computes the frame delta parks
    // it in here, and that tick lives inside the region we are about to replay -
    // so without this the real frame's elapsed time gets split across two ticks
    // and the world runs at half speed. Snapshot it, replay, put it back: the
    // next real tick then measures from the right moment.
    uint8_t* timeObj = TimeObject();
    uint8_t savedTime[kTimeSnapshotBytes];
    const bool freezeTime = Cfg().full_rate_freeze_time && timeObj;
    if (freezeTime) memcpy(savedTime, timeObj, kTimeSnapshotBytes);

    // Collect every timer the replay advances, so they can all be put back -
    // not just the one global whose address we happen to know.
    g_timerCount = 0;
    g_collecting = true;
    InterlockedExchange(&g_inReplay, 1);   // the replay, and only the replay

    // Stop the clock. From here until the replay returns, every
    // QueryPerformanceCounter on this thread reports the same instant.
    const bool freezeQpc = Cfg().full_rate_freeze_qpc && o_QueryPerformanceCounter;
    if (freezeQpc) {
        o_QueryPerformanceCounter(&g_frozenCounter);
        InterlockedExchange(&g_freezeThread, (LONG)GetCurrentThreadId());
    }

    bool ok;
    __try {
        SetRenderEye(1);
        o_DrawScene(self, g_lastDesc, g_lastA3, g_lastA4);
        o_SubmitScene(g_lastDesc);
        o_PresentPath(self, zeroDt ? 0.0f : arg);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }

    if (freezeQpc) {
        InterlockedExchange(&g_freezeThread, 0);
        static int logged = 0;
        if (logged < 3) {
            ++logged;
            COTW_LOG("[clock] time frozen for the replay - %ld counter reads served the "
                     "same instant. Zero elapsed everywhere means nothing to repair.",
                     (long)g_frozenHits);
        }
        InterlockedExchange(&g_frozenHits, 0);
    }

    InterlockedExchange(&g_inReplay, 0);

    // One report per second or so, then the per-frame counts reset. The counts
    // are the whole point: they say which caller is summing and which is not.
    if (Cfg().full_rate_log_callsites) {
        static uint64_t f = 0;
        if ((++f % 90) == 1) ReportGetterCallers();
        ResetGetterCallerFrame();
    }

    g_collecting = false;
    if (o_TimerTick) RestoreTickedTimers();

    // Report WHICH bytes the replay disturbed before putting them back. That
    // turns "the world runs at half speed" from a guess into a list of offsets,
    // and if nothing changed it says so - which would mean the delta is ticked
    // somewhere else entirely and this lever is the wrong one.
    if (freezeTime && ok) {
        static int reported = 0;
        if (reported < 3) {
            char diff[256];
            int n = 0;
            for (uint32_t i = 0; i < kTimeSnapshotBytes && n < (int)sizeof(diff) - 8; i += 4) {
                if (memcmp(savedTime + i, timeObj + i, 4) != 0) {
                    n += snprintf(diff + n, sizeof(diff) - n, " +0x%X", i);
                }
            }
            if (n == 0) snprintf(diff, sizeof(diff), " (none)");
            ++reported;
            COTW_LOG("[fullrate] the replay changed these time-object fields:%s "
                     "(delta before %.5f s, after %.5f s)",
                     diff,
                     *reinterpret_cast<const float*>(savedTime + apex::kTimeDeltaOffset),
                     *reinterpret_cast<const float*>(timeObj + apex::kTimeDeltaOffset));
        }
        memcpy(timeObj, savedTime, kTimeSnapshotBytes);
    }

    if (freeze) { *clockA = savedA; *clockB = savedB; }
    if (zeroDt && descIsDt) *descDt = savedDt;

    // Put the description's delta fields back - the engine owns that buffer.
    if (descHitCount && descWords) {
        __try {
            for (int i = 0; i < descHitCount; ++i) descWords[descHits[i]] = savedDesc[i];
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // *** LAST, after every restore above. ***
    //
    // This was previously written BEFORE the full-object memcpy a few lines up,
    // which promptly copied the old halved value straight back over it - so the
    // scaled-delta fix has never actually taken effect, and the run that
    // "proved" it did not work proved nothing of the sort. Ordering, not the
    // idea, was wrong.
    //
    // The raw delta at +0x14 comes out correct (the world runs at the right
    // speed); the scaled one at +0x2C stays at half, and the player and the UI
    // are what read it. Rebuild it from the raw delta and the game's own
    // dilation, sampled while we were not interfering.
    if (Cfg().full_rate_fix_scaled_delta) {
        if (uint8_t* obj = TimeObject()) {
            float* raw = reinterpret_cast<float*>(obj + apex::kTimeDeltaOffset);
            float* sc = reinterpret_cast<float*>(obj + apex::kTimeScaledDeltaOffset);
            if (*raw > 1e-6f) {
                static uint64_t n = 0;
                const int every = Cfg().stereo_log_every;
                if (every > 0 && (++n % (uint64_t)every) == 1) {
                    COTW_LOG("[clock] scaled delta %.2f ms -> %.2f ms (raw %.2f ms, "
                             "game dilation %.3f) - applied AFTER the restores this time",
                             *sc * 1000.0f, *raw * g_timeScaleRatio * 1000.0f,
                             *raw * 1000.0f, g_timeScaleRatio);
                }
                *sc = *raw * g_timeScaleRatio;
            }
        }
    }
    return ok;
}

void __fastcall Hook_DrawScene(void* self, void* desc, void* a3, void* a4) {
    ++g_sceneCalls;
    // Remember the engine's own arguments for the replay.
    g_lastThis = self;
    g_lastDesc = desc;
    g_lastA3 = a3;
    g_lastA4 = a4;
    o_DrawScene(self, desc, a3, a4);
}

void __fastcall Hook_SubmitScene(void* desc) {
    g_lastDesc = desc;      // the buffer the engine filled, for the replay
    o_SubmitScene(desc);
}

void __fastcall Hook_PresentPath(void* self, float arg) {
    // How long the last REAL frame took, measured by us. This is what the delta
    // getter hands the engine while full-rate is driving, so it has to be
    // maintained every frame - including the ones where full-rate is not
    // engaged, or the first frame after it engages would use a stale value.
    {
        const double now = NowSeconds();
        if (g_prevFrameSeconds > 0.0) {
            const double d = now - g_prevFrameSeconds;
            if (d > 0.001 && d < 0.05) {
                // Lightly smoothed: a single long frame should not make the
                // whole world lurch.
                g_realFrameDelta = g_realFrameDelta > 0.0f
                                       ? g_realFrameDelta * 0.8f + float(d) * 0.2f
                                       : float(d);
            }
        }
        g_prevFrameSeconds = now;
    }

    // (The delta a consumer receives is recorded in the getter hook itself; the
    // stored field is no longer what anything reads.)

    // PlayerCameraActive() keeps this out of the main menu. The menu has no
    // player camera and runs on its own timing, and replaying its frame - plus
    // rewriting the engine's frame delta to match a world that is not being
    // simulated - is what put the menu into slow motion.
    const bool want = Cfg().full_rate_stereo && Cfg().stereo && VR().Ready() &&
                      !g_disabledByFault && !g_inDouble && WarmedUp() &&
                      PlayerCameraActive();

    if (!want) {
        SetFullRateActive(false);
        o_PresentPath(self, arg);
        return;
    }

    static bool announced = false;
    if (!announced) {
        announced = true;
        COTW_LOG("[fullrate] engaging after %llu scene builds (warm-up %d)",
                 (unsigned long long)g_sceneCalls, Cfg().full_rate_warmup_frames);
        COTW_LOG("[fullrate] replaying BUILD + SUBMIT + PRESENT-PATH for the second eye, "
                 "so both eyes are complete images from the same instant");
    }

    InterlockedExchange(&g_inDouble, 1);
    InterlockedExchange(&g_driving, 1);
    // Set BEFORE the first eye presents, not after the replay succeeds. The
    // Present hook runs twice inside this function and asks whether full-rate is
    // active; if the answer only became true at the end, the first frame would
    // take the alternate-eye path and upload one eye.
    SetFullRateActive(true);
    ++g_frames;

    // The present path ENDS in DXGI Present, and the DXGI hook is where an eye
    // is captured - the backbuffer is only guaranteed to hold the finished image
    // *before* the flip, not after it. So each eye is grabbed by the Present
    // hook on its own pass, and the OpenXR frame is submitted once, on pass 1,
    // when both eyes exist.
    //
    // This is what was wrong before. The replay ran the whole present path a
    // second time, which reached DXGI Present a second time, which ran the
    // ENTIRE OpenXR frame loop again - two xrWaitFrame/xrBeginFrame/xrEndFrame
    // triples per real frame. That is what made the world run at double speed
    // and then in slow motion, and an unbalanced Begin/End pair desynchronises
    // the runtime permanently.
    InterlockedExchange(&g_pass, 0);
    o_PresentPath(self, arg);

    InterlockedExchange(&g_pass, 1);
    const bool ok = ReplaySecondEye(self, arg);

    // --- put the engine's clock back on real time --------------------------
    //
    // Restoring the time object across the replay was not enough, and the log
    // said why: at snapshot time the delta was ALREADY too small. So the tick's
    // baseline timestamp lives outside the object we snapshot, and the two ticks
    // per frame each measure their own half of it - the replay's tick times the
    // replay, and the next frame's tick times only what is left. Measured at
    // 0.47x reality, which is that split exactly.
    //
    // Rather than hunt for the baseline, state the truth directly: a frame took
    // as long as it took, and we are the ones holding a stopwatch on it. This is
    // the LAST write to the delta before the next frame's game logic reads it.
    // The old "measure the frame and write it into +0x14" correction lived here.
    // Removed: it shared g_prevFrameSeconds with the measurement above, and the
    // getter hook now delivers the same number at the point of use without
    // writing any engine state at all.

    InterlockedExchange(&g_pass, 0);
    InterlockedExchange(&g_inDouble, 0);
    InterlockedExchange(&g_driving, 0);
    SetRenderEye(0);   // the engine builds eye 0 next frame

    if (!ok) {
        g_disabledByFault = true;
        SetFullRateActive(false);
        COTW_LOG("[fullrate] *** the second-eye replay FAULTED - full-rate is OFF for "
                 "this session; the game continues in alternate-eye. Survived %llu "
                 "frames. ***", (unsigned long long)g_frames);
        return;
    }

    const int every = Cfg().stereo_log_every;
    if (every > 0 && (g_frames % (uint64_t)every) == 0) {
        COTW_LOG("[fullrate] %llu frames double-rendered, %llu stash failures",
                 (unsigned long long)g_frames, (unsigned long long)g_stashFailures);
    }
}

}  // namespace

int CurrentRenderPass() { return (int)g_pass; }
bool InDoubleRender() { return g_inDouble != 0; }
bool FullRateDriving() { return g_driving != 0; }
void NoteStashResult(bool ok) { if (!ok) ++g_stashFailures; }

float EngineFrameDelta() {
    // The value sampled at the TOP of the frame - what the game's own logic
    // actually ran on. Reading it live was the mistake: the perf log calls this
    // from inside SubmitFrame, which happens DURING the second-eye replay, so it
    // was reporting the replay's own half-frame and flagging it as "the clock is
    // off" even on frames where the clock was fine. A measurement taken at the
    // wrong instant is worse than none, because it looks authoritative.
    return g_engineDtAtFrameStart;
}

bool InstallFrameHook() {
    if (g_installed) return true;
    if (!apex::FingerprintMatches()) {
        COTW_LOG("[fullrate] not installed - build fingerprint mismatch");
        return false;
    }

    void* build = apex::Rva(apex::kRenderScene);
    void* submit = apex::Rva(apex::kSubmitScene);
    void* present = apex::Rva(apex::kPresentPath);
    if (!build || !submit || !present) {
        COTW_LOG("[fullrate] could not resolve the build/submit/present RVAs");
        return false;
    }

    // The engine's delta GETTER. Read-only substitution at the point of use -
    // no engine state is written, and nothing about the passage of time is
    // interfered with, which is what made the counter freeze fatal.
    if (void* g = apex::Rva(apex::kTimeGetter)) {
        MH_STATUS gs = MH_CreateHook(g, &Hook_TimeGetter, (void**)&o_TimeGetter);
        if ((gs == MH_OK || gs == MH_ERROR_ALREADY_CREATED) &&
            MH_EnableHook(g) == MH_OK) {
            COTW_LOG("[clock] delta getter hooked at +0x%X - consumers will be handed the "
                     "true frame interval while full-rate is driving",
                     apex::kTimeGetter);
        } else {
            o_TimeGetter = nullptr;
            COTW_LOG("[clock] could not hook the delta getter at +0x%X (%d)",
                     apex::kTimeGetter, (int)gs);
        }
    }

    // OFF by default: hooking this crashed the game on startup. It is 0x18
    // bytes long and sits 32 bytes from the tick at 0x0EB040, which is also
    // hooked - two trampolines that close together in functions that short is a
    // good way to corrupt one. The UI is corrected by keeping its field in step
    // from the other getter instead. Kept switchable so the theory can be
    // retested on its own rather than guessed at.
    if (Cfg().full_rate_hook_second_getter)
    if (void* g2 = apex::Rva(apex::kTimeGetter2)) {
        MH_STATUS gs = MH_CreateHook(g2, &Hook_TimeGetter2, (void**)&o_TimeGetter2);
        if ((gs == MH_OK || gs == MH_ERROR_ALREADY_CREATED) &&
            MH_EnableHook(g2) == MH_OK) {
            COTW_LOG("[clock] second delta getter hooked at +0x%X - this is the one the "
                     "in-game UI reads", apex::kTimeGetter2);
        } else {
            o_TimeGetter2 = nullptr;
        }
    }

    // The performance counter itself. Only installed when explicitly asked for:
    // it crashed the game, because the OpenXR runtime and DXGI run inside the
    // replay and cannot survive a stopped clock.
    if (Cfg().full_rate_freeze_qpc) {
        const wchar_t* mods[] = {L"ntdll.dll", L"kernel32.dll"};
        for (const wchar_t* m : mods) {
            HMODULE h = GetModuleHandleW(m);
            if (!h) continue;
            void* fn = (void*)GetProcAddress(
                h, wcscmp(m, L"ntdll.dll") == 0 ? "RtlQueryPerformanceCounter"
                                                : "QueryPerformanceCounter");
            if (!fn) continue;
            PFN_QPC orig = nullptr;
            MH_STATUS qs = MH_CreateHook(fn, &Hook_QPC, (void**)&orig);
            if ((qs == MH_OK || qs == MH_ERROR_ALREADY_CREATED) &&
                MH_EnableHook(fn) == MH_OK) {
                if (!o_QueryPerformanceCounter) o_QueryPerformanceCounter = orig;
                COTW_LOG("[clock] performance counter hooked in %S", m);
            }
        }
        if (!o_QueryPerformanceCounter) {
            COTW_LOG("[clock] *** could not hook the performance counter - the replay "
                     "will keep advancing the engine's time ***");
        }
    }

    // Only installed if actually used. Every hook is a risk surface, and this
    // one is only ever a pass-through unless full_rate_skip_clock_update is on -
    // which it is not, because skipping it made the world flicker black.
    if (Cfg().full_rate_skip_clock_update)
    if (void* cu = apex::Rva(apex::kClockUpdate)) {
        MH_STATUS cs = MH_CreateHook(cu, &Hook_ClockUpdate, (void**)&o_ClockUpdate);
        if ((cs == MH_OK || cs == MH_ERROR_ALREADY_CREATED) &&
            MH_EnableHook(cu) == MH_OK) {
            COTW_LOG("[clock] clock-update hook installed at +0x%X - the replay will not "
                     "advance the engine's time", apex::kClockUpdate);
        } else {
            o_ClockUpdate = nullptr;
            COTW_LOG("[clock] could not hook the clock update at +0x%X (%d)",
                     apex::kClockUpdate, (int)cs);
        }
    }

    // Likewise the tick. Superseded by correcting the value at the getter, so
    // it is off unless asked for.
    if (Cfg().full_rate_hook_timer_tick)
    if (void* tick = apex::Rva(apex::kTimerTick)) {
        MH_STATUS ts = MH_CreateHook(tick, &Hook_TimerTick, (void**)&o_TimerTick);
        if ((ts == MH_OK || ts == MH_ERROR_ALREADY_CREATED) &&
            MH_EnableHook(tick) == MH_OK) {
            COTW_LOG("[clock] tick hook installed at +0x%X - every timer the replay "
                     "advances will be restored", apex::kTimerTick);
        } else {
            o_TimerTick = nullptr;
            COTW_LOG("[clock] could not hook the tick at +0x%X (%d)", apex::kTimerTick,
                     (int)ts);
        }
    }

    MH_STATUS s = MH_CreateHook(build, &Hook_DrawScene, (void**)&o_DrawScene);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
        COTW_LOG("[fullrate] MH_CreateHook(DrawScene @0x%X) failed: %d",
                 apex::kRenderScene, (int)s);
        return false;
    }
    s = MH_CreateHook(submit, &Hook_SubmitScene, (void**)&o_SubmitScene);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
        COTW_LOG("[fullrate] MH_CreateHook(SubmitScene @0x%X) failed: %d",
                 apex::kSubmitScene, (int)s);
        return false;
    }
    s = MH_CreateHook(present, &Hook_PresentPath, (void**)&o_PresentPath);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) {
        COTW_LOG("[fullrate] MH_CreateHook(PresentPath @0x%X) failed: %d",
                 apex::kPresentPath, (int)s);
        return false;
    }
    if (MH_EnableHook(build) != MH_OK || MH_EnableHook(submit) != MH_OK ||
        MH_EnableHook(present) != MH_OK) {
        COTW_LOG("[fullrate] MH_EnableHook failed");
        return false;
    }

    g_installed = true;
    COTW_LOG("[fullrate] hooks installed: build +0x%X, submit +0x%X, present +0x%X",
             apex::kRenderScene, apex::kSubmitScene, apex::kPresentPath);
    COTW_LOG("[fullrate] a second eye is produced by REPLAYING the engine's own "
             "build+submit pair, not by calling the build twice");
    return true;
}

void RemoveFrameHook() {
    if (!g_installed) return;
    void* build = apex::Rva(apex::kRenderScene);
    void* submit = apex::Rva(apex::kSubmitScene);
    void* present = apex::Rva(apex::kPresentPath);
    if (build) MH_DisableHook(build);
    if (submit) MH_DisableHook(submit);
    if (present) MH_DisableHook(present);
    g_installed = false;
    SetFullRateActive(false);
}

}  // namespace cotwvr
