#include "apex.h"

#include "config.h"
#include "log.h"

namespace cotwvr {
namespace apex {
namespace {

uintptr_t g_base = 0;
size_t    g_size = 0;
bool      g_match = false;

}  // namespace

bool Init() {
    HMODULE exe = GetModuleHandleW(nullptr);
    if (!exe) {
        COTW_LOG("[apex] GetModuleHandleW(nullptr) failed");
        return false;
    }
    g_base = reinterpret_cast<uintptr_t>(exe);

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    g_size = nt->OptionalHeader.SizeOfImage;
    const uint32_t stamp = nt->FileHeader.TimeDateStamp;

    g_match = (g_size == kExpectedSizeOfImage) && (stamp == kExpectedTimestamp);
    const bool forced = !g_match && Cfg().apex_ignore_fingerprint;
    if (forced) g_match = true;

    wchar_t name[MAX_PATH]{};
    GetModuleFileNameW(exe, name, MAX_PATH);
    COTW_LOG("[apex] module base 0x%llX size 0x%X stamp 0x%08X",
             (unsigned long long)g_base, (unsigned)g_size, stamp);

    if (!g_match) {
        COTW_LOG("[apex] *** BUILD FINGERPRINT MISMATCH ***");
        COTW_LOG("[apex]   expected size 0x%X stamp 0x%08X",
                 kExpectedSizeOfImage, kExpectedTimestamp);
        COTW_LOG("[apex]   the game has been patched - every hardcoded RVA is now"
                 " suspect and all RVA-based features stay DISABLED.");
        COTW_LOG("[apex]   Stereo/head tracking will not work until the addresses"
                 " are re-found for this build.");
    } else if (forced) {
        COTW_LOG("[apex] *** FINGERPRINT MISMATCH, OVERRIDDEN BY apex_ignore_fingerprint ***");
        COTW_LOG("[apex]   expected size 0x%X stamp 0x%08X - running anyway, at your "
                 "own risk. If the game crashes, set it back to 0.",
                 kExpectedSizeOfImage, kExpectedTimestamp);
    } else {
        COTW_LOG("[apex] fingerprint OK - engine addresses are valid for this build");
    }
    return true;
}

uintptr_t Base()          { return g_base; }
bool      FingerprintMatches() { return g_match; }
size_t    ImageSize()     { return g_size; }

void* Rva(uint32_t rva) {
    if (!g_base || !g_match) return nullptr;
    if (rva >= g_size) return nullptr;
    return reinterpret_cast<void*>(g_base + rva);
}

}  // namespace apex
}  // namespace cotwvr
