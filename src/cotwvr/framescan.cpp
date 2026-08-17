#include "framescan.h"

#include "apex.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

namespace cotwvr {
namespace {

constexpr int kDepth = 40;

struct Trace {
    void* frames[kDepth];
    USHORT count;
    DWORD thread;
    volatile LONG have;
};

Trace g_present{};
Trace g_camera{};
volatile LONG g_reported = 0;

void Capture(Trace& t) {
    if (t.have) return;
    // RtlCaptureStackBackTrace is safe in a hot hook: no allocation, no locks,
    // no loader involvement. Anything heavier here would tank the framerate,
    // which is how these investigations usually get abandoned.
    t.count = RtlCaptureStackBackTrace(1, kDepth, t.frames, nullptr);
    t.thread = GetCurrentThreadId();
    InterlockedExchange(&t.have, 1);
}

void LogTrace(const char* what, const Trace& t) {
    const uintptr_t base = apex::Base();
    const size_t size = apex::ImageSize();
    COTW_LOG("[frame] --- stack at %s (thread %lu, %u frames) ---", what, t.thread,
             (unsigned)t.count);
    for (USHORT i = 0; i < t.count; ++i) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(t.frames[i]);
        if (a >= base && a < base + size) {
            COTW_LOG("[frame]   %2u  theHunterCotW_F.exe + 0x%08llX", i,
                     (unsigned long long)(a - base));
        } else {
            HMODULE m = nullptr;
            char name[64] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                       GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(a), &m) && m) {
                char path[MAX_PATH]{};
                GetModuleFileNameA(m, path, MAX_PATH);
                const char* slash = strrchr(path, '\\');
                strncpy_s(name, slash ? slash + 1 : path, _TRUNCATE);
            }
            COTW_LOG("[frame]   %2u  %s", i, name);
        }
    }
}

}  // namespace

void FrameScanNotePresent() { Capture(g_present); }
void FrameScanNoteCamera()  { Capture(g_camera); }

void FrameScanReport() {
    if (g_reported) return;
    if (!g_present.have || !g_camera.have) return;
    InterlockedExchange(&g_reported, 1);

    LogTrace("Present", g_present);
    LogTrace("the camera position lever", g_camera);

    const bool sameThread = g_present.thread == g_camera.thread;
    COTW_LOG("[frame] ================================================");
    COTW_LOG("[frame] Present thread %lu, camera thread %lu -> %s",
             g_present.thread, g_camera.thread,
             sameThread ? "SAME THREAD" : "*** DIFFERENT THREADS ***");
    if (sameThread) {
        COTW_LOG("[frame] The scene is built on the presenting thread, so calling the "
                 "frame function twice is viable. Full-rate stereo is ON THE TABLE.");
    } else {
        COTW_LOG("[frame] The engine draws on a worker thread. Calling a frame function "
                 "twice from Present cannot work; full-rate stereo needs a different "
                 "approach entirely.");
    }

    // The frame function contains BOTH the camera setup and the Present call, so
    // it appears in both stacks. The deepest such shared return address is the
    // innermost function that spans the whole frame - the best candidate.
    const uintptr_t base = apex::Base();
    const size_t size = apex::ImageSize();
    COTW_LOG("[frame] --- return addresses common to BOTH stacks (frame-function "
             "candidates, innermost first) ---");
    int found = 0;
    for (USHORT i = 0; i < g_present.count; ++i) {
        const uintptr_t a = reinterpret_cast<uintptr_t>(g_present.frames[i]);
        if (a < base || a >= base + size) continue;
        for (USHORT j = 0; j < g_camera.count; ++j) {
            if (g_camera.frames[j] != g_present.frames[i]) continue;
            COTW_LOG("[frame]   +0x%08llX   (present depth %u, camera depth %u)",
                     (unsigned long long)(a - base), i, j);
            ++found;
            break;
        }
    }
    if (!found) {
        COTW_LOG("[frame]   none - the camera and Present do not share a caller, which "
                 "means there is no single 'render a frame' function to call twice.");
    }
    COTW_LOG("[frame] ================================================");
}

}  // namespace cotwvr
