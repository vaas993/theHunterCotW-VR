#include "hwbp.h"

#include "apex.h"
#include "config.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>

namespace cotwvr {
namespace {

// --- the catch table -------------------------------------------------------
//
// Keyed by the RVA of the instruction AFTER the access (which is what the CPU
// reports for a data breakpoint - the access has already happened). Distinct
// instructions are few, so a small linear table is plenty and needs no locking
// beyond an interlocked claim.
struct Hit {
    uint32_t rva;
    volatile LONG count;
    volatile LONG slotMask;       // DR0/DR1 says which watched address
};
constexpr int kMaxHits = 64;
Hit g_hits[kMaxHits];
volatile LONG g_hitCount = 0;
volatile LONG g_totalHits = 0;
volatile LONG g_overflow = 0;
volatile LONG g_foreign = 0;   // hits from outside the game module
uintptr_t g_foreignLast = 0;   // the last foreign RIP, absolute - a memcpy in
                               // the CRT or driver is still an answer

uintptr_t g_moduleBase = 0;
PVOID g_veh = nullptr;

void NoteHit(uintptr_t rip, DWORD64 dr6) {
    InterlockedIncrement(&g_totalHits);
    // BOTH ends. Only checking the lower one let an address past the end of the
    // image be recorded as "+0x63C0C460", which is not an RVA at all.
    if (!g_moduleBase || rip < g_moduleBase ||
        rip >= g_moduleBase + apex::kExpectedSizeOfImage) {
        InterlockedIncrement(&g_foreign);
        g_foreignLast = rip;
        return;
    }
    const uint32_t rva = static_cast<uint32_t>(rip - g_moduleBase);
    const LONG n = g_hitCount;
    for (LONG i = 0; i < n && i < kMaxHits; ++i) {
        if (g_hits[i].rva == rva) {
            InterlockedIncrement(&g_hits[i].count);
            InterlockedOr(&g_hits[i].slotMask, static_cast<LONG>(dr6 & 0x3));
            return;
        }
    }
    if (n >= kMaxHits) { InterlockedIncrement(&g_overflow); return; }
    const LONG idx = InterlockedIncrement(&g_hitCount) - 1;
    if (idx < 0 || idx >= kMaxHits) return;
    g_hits[idx].rva = rva;
    g_hits[idx].count = 1;
    g_hits[idx].slotMask = static_cast<LONG>(dr6 & 0x3);
}

// A data breakpoint arrives as a SINGLE_STEP with the matching bit set in DR6.
// Clear DR6 or it latches and every later exception looks the same.
LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* c = ep->ContextRecord;
    const DWORD64 dr6 = c->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;   // not one of ours
    NoteHit(static_cast<uintptr_t>(c->Rip), dr6);
    c->Dr6 = 0;
    return EXCEPTION_CONTINUE_EXECUTION;
}

// --- arming ----------------------------------------------------------------
//
// Debug registers are PER THREAD, and Windows will not reliably let a thread
// set its own through GetThreadContext. A helper thread suspends each target
// and sets them - and it walks EVERY thread of the process, because the
// writer's thread is precisely what we do not know.
struct ArmRequest {
    void* addr;
    int   slot;          // 0 or 1 -> DR0 / DR1
    char  what[32];
};
ArmRequest g_pending[2];
volatile LONG g_pendingCount = 0;
volatile LONG g_armedThreads = 0;

DWORD WINAPI ArmThread(LPVOID) {
    Sleep(200);
    const LONG n = g_pendingCount;
    if (!n) return 0;
    const DWORD self = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        COTW_LOG("[hwbp] thread snapshot failed (%lu) - nothing armed", GetLastError());
        return 0;
    }
    long armed = 0;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == self) continue;    // cannot set our own
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                       THREAD_SET_CONTEXT,
                                   FALSE, te.th32ThreadID);
            if (!th) continue;
            if (SuspendThread(th) == (DWORD)-1) { CloseHandle(th); continue; }
            CONTEXT c{};
            c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &c)) {
                for (LONG i = 0; i < n && i < 2; ++i) {
                    const ArmRequest& r = g_pending[i];
                    if (r.slot == 0) c.Dr0 = reinterpret_cast<DWORD64>(r.addr);
                    else             c.Dr1 = reinterpret_cast<DWORD64>(r.addr);
                    const int shift = r.slot * 4;
                    DWORD64 dr7 = c.Dr7;
                    dr7 |= (DWORD64)1 << (r.slot * 2);        // local enable
                    dr7 &= ~((DWORD64)0xF << (16 + shift));   // clear RW/LEN
                    dr7 |= ((DWORD64)0x1 << (16 + shift));    // RW = 01, WRITES ONLY
                    dr7 |= ((DWORD64)0x3 << (18 + shift));    // LEN = 11, four bytes
                    c.Dr7 = dr7;
                }
                c.Dr6 = 0;
                if (SetThreadContext(th, &c)) ++armed;
            }
            ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    InterlockedExchange(&g_armedThreads, armed);
    for (LONG i = 0; i < n && i < 2; ++i)
        COTW_LOG("[hwbp] watching WRITES to %s at %p (DR%d) on %ld threads",
                 g_pending[i].what, g_pending[i].addr, g_pending[i].slot, armed);
    return 0;
}

}  // namespace

bool HwbpInit() {
    if (g_veh) return true;
    g_moduleBase = reinterpret_cast<uintptr_t>(apex::Rva(0));
    if (!g_moduleBase) return false;
    g_veh = AddVectoredExceptionHandler(1, &Veh);
    if (!g_veh) {
        COTW_LOG("[hwbp] AddVectoredExceptionHandler failed (%lu)", GetLastError());
        return false;
    }
    COTW_LOG("[hwbp] handler installed. One CPU exception per watched write - a "
             "measurement, not a mode to play in.");
    return true;
}

void HwbpWatch(void* addr, const char* what) {
    if (!addr) return;
    if ((reinterpret_cast<uintptr_t>(addr) & 3) != 0) {
        COTW_LOG("[hwbp] %p is not 4-byte aligned - the CPU cannot watch it", addr);
        return;
    }
    const LONG n = g_pendingCount;
    for (LONG i = 0; i < n && i < 2; ++i) {
        if (g_pending[i].addr == addr) return;      // already watching this one
    }
    if (n >= 2) return;                             // only DR0/DR1 are used
    const LONG idx = InterlockedIncrement(&g_pendingCount) - 1;
    if (idx < 0 || idx >= 2) return;
    g_pending[idx].addr = addr;
    g_pending[idx].slot = (int)idx;
    lstrcpynA(g_pending[idx].what, what ? what : "?", sizeof(g_pending[idx].what));
    if (HANDLE t = CreateThread(nullptr, 0, &ArmThread, nullptr, 0, nullptr)) {
        CloseHandle(t);
    }
}

void HwbpReport() {
    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (!g_pendingCount) return;
    if (!last) { last = now; return; }
    if (now - last < 5000) return;
    last = now;

    const LONG n = g_hitCount;
    if (!n) {
        COTW_LOG("[hwbp] NOTHING has written the watched address yet (%ld foreign). "
                 "Either the breakpoint did not arm, the address is wrong, or the "
                 "writer thread started after arming.", (long)g_foreign);
        return;
    }
    COTW_LOG("[hwbp] %ld writes (%ld from outside the game module, last foreign RIP "
             "%p) from %ld distinct instructions%s:",
             (long)g_totalHits, (long)g_foreign, (void*)g_foreignLast, (long)n,
             g_overflow ? " (TABLE FULL - list incomplete)" : "");
    for (LONG i = 0; i < n && i < kMaxHits; ++i) {
        const Hit& h = g_hits[i];
        COTW_LOG("[hwbp]   +0x%08X  %6ld writes  (DR mask %ld)",
                 h.rva, (long)h.count, (long)h.slotMask);
    }
}

}  // namespace cotwvr
