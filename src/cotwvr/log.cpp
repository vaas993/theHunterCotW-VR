#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace cotwvr {
namespace {

FILE* g_file = nullptr;
std::mutex g_mutex;
LARGE_INTEGER g_freq{};
LARGE_INTEGER g_start{};

bool g_enabled = true;
bool g_debugger = false;
long long g_written = 0;
bool g_capped = false;

// *** A LOG THAT CANNOT FILL A DRIVE. ***
//
// Measured on this machine: normal play writes about 2.4 lines a second (the
// once-per-five-seconds counters), roughly 1 MB an hour. The diagnostic
// switches multiply that - the longest verbose session here was 8.7 MB in 53
// minutes - and one left on by accident had nothing to stop it. 64 MB is far
// beyond any real bug report and far short of a problem.
constexpr long long kMaxBytes = 64ll * 1024 * 1024;

// How many per-process logs to keep. The game starts a short-lived process
// before the one that renders, and _SH_DENYWR means whichever wins the race
// owns cotwvr.log while the other falls back to a file of its own - so one of
// these is left behind on EVERY launch. On the machine this was written on that
// had reached 187 files and about 100 MB, silently. They are worth keeping for
// the run you are debugging and worthless after that.
constexpr size_t kKeepPidLogs = 3;

double Now() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return double(t.QuadPart - g_start.QuadPart) / double(g_freq.QuadPart);
}

// Delete all but the newest few cotwvr.pidNNNN.log. Never touches cotwvr.log or
// cotwvr.prev.log - those two are the ones a bug report asks for.
void PruneProcessLogs(const std::wstring& dir) {
    if (dir.empty()) return;
    std::vector<std::pair<unsigned long long, std::wstring>> found;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\cotwvr.pid*.log").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        ULARGE_INTEGER t{};
        t.LowPart = fd.ftLastWriteTime.dwLowDateTime;
        t.HighPart = fd.ftLastWriteTime.dwHighDateTime;
        found.emplace_back(t.QuadPart, dir + L"\\" + fd.cFileName);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (found.size() <= kKeepPidLogs) return;
    std::sort(found.begin(), found.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = kKeepPidLogs; i < found.size(); ++i) {
        // A file another process still holds open simply refuses; that is the
        // right outcome and there is nothing to report about it.
        DeleteFileW(found[i].second.c_str());
    }
}

}  // namespace

std::wstring LogDirectory() {
    wchar_t* local = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        dir = std::wstring(local) + L"\\theHunterCotWVR";
        CoTaskMemFree(local);
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

void LogInit() {
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_start);
    // OutputDebugString is not free: it takes a machine-wide named mutex and
    // signals two events, so every line pays for a debugger that is almost
    // never there. It also broadcasts the mod's internals to any DebugView
    // window on the system, which is somebody else's business.
    g_debugger = IsDebuggerPresent() != FALSE;

    const std::wstring dir = LogDirectory();
    PruneProcessLogs(dir);
    const std::wstring path = dir + L"\\cotwvr.log";
    // Keep the previous run: a crash-on-launch investigation always wants the
    // run before the one that just overwrote it.
    MoveFileExW(path.c_str(), (dir + L"\\cotwvr.prev.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    // Shared read so the log can be tailed while the game runs.
    g_file = _wfsopen(path.c_str(), L"w", _SH_DENYWR);

    // MORE THAN ONE PROCESS LOADS THIS DLL. The game starts an early process
    // that runs briefly before the real one, and _SH_DENYWR means whichever gets
    // there first OWNS the file - so the process that actually renders can end
    // up unable to log a single line.
    //
    // That cost several rounds of diagnosis: the log showed nine lines ending at
    // the probe device and was read as "the mod is hanging", while VR was in
    // fact running perfectly in a process that simply had nowhere to write. Fall
    // back to a per-process file so the busy process is never the silent one.
    if (!g_file) {
        wchar_t alt[MAX_PATH]{};
        _snwprintf_s(alt, MAX_PATH, _TRUNCATE, L"%ls\\cotwvr.pid%lu.log", dir.c_str(),
                     GetCurrentProcessId());
        g_file = _wfsopen(alt, L"w", _SH_DENYWR);
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    Logf("theHunter: Call of the Wild VR - log opened %04d-%02d-%02d %02d:%02d:%02d",
         st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

void LogShutdown() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file) {
        fclose(g_file);
        g_file = nullptr;
    }
}

void LogRaw(const char* text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file && g_enabled) {
        // fflush stays. A log that loses its last page is a log that never
        // records the thing that went wrong, and at a couple of lines a second
        // the flush is not measurable next to a frame.
        const int n = fprintf(g_file, "[%9.3f] %s\n", Now(), text);
        fflush(g_file);
        if (n > 0) g_written += n;
        if (g_written >= kMaxBytes && !g_capped) {
            g_capped = true;
            g_enabled = false;
            fprintf(g_file, "[%9.3f] *** log reached %lld MB and stopped. Turn a "
                            "diagnostic off, or set logging = 0 in cotwvr.ini. ***\n",
                    Now(), kMaxBytes / (1024 * 1024));
            fflush(g_file);
        }
    }
    if (g_debugger) {
        OutputDebugStringA(text);
        OutputDebugStringA("\n");
    }
}

void LogSetEnabled(bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_capped) return;          // the cap is not something a setting undoes
    g_enabled = on;
}

bool LogEnabled() {
    return g_file != nullptr && g_enabled;
}

void Logf(const char* fmt, ...) {
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, args);
    va_end(args);
    LogRaw(buf);
}

}  // namespace cotwvr
