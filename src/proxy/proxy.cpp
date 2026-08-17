// XINPUT9_1_0.dll proxy for theHunter: Call of the Wild.
//
// theHunterCotW_F.exe statically imports XInputGetState and XInputSetState from
// XINPUT9_1_0.dll, so dropping this next to the exe gets us loaded before the
// engine initialises anything.  Every export is forwarded to the real system
// DLL; the only thing we add is loading cotwvr.dll.
//
// Why XInput and not dxgi.dll (the usual choice for a D3D11 game): dxgi is the
// proxy every other VR/graphics tool wants too - ReShade, geo-11, vorpX - and
// this machine has several of them installed.  XINPUT9_1_0 is loaded just as
// early (it is a static import), collides with nothing, and hands us the
// gamepad for free.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

#include <cstdio>
#include <string>

namespace {

HMODULE g_real = nullptr;   // system XINPUT9_1_0.dll
HMODULE g_mod = nullptr;    // cotwvr.dll

using PFN_GetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using PFN_SetState = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
using PFN_GetCapabilities = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using PFN_GetDSoundGuids = DWORD(WINAPI*)(DWORD, GUID*, GUID*);

PFN_GetState g_getState = nullptr;
PFN_SetState g_setState = nullptr;
PFN_GetCapabilities g_getCaps = nullptr;
PFN_GetDSoundGuids g_getGuids = nullptr;

// cotwvr.dll may edit the pad state on its way into the game (settings panel,
// stick-axis blocking).
using PFN_VrPadPost = void(__cdecl*)(DWORD, XINPUT_STATE*, DWORD);
PFN_VrPadPost g_vrPadPost = nullptr;

std::wstring ModuleDir(HMODULE h) {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(h, path, MAX_PATH);
    std::wstring s(path);
    const size_t slash = s.find_last_of(L'\\');
    return slash == std::wstring::npos ? std::wstring() : s.substr(0, slash + 1);
}

// "No log file at all" is the single most confusing failure mode there is - it
// looks identical whether the DLL was never loaded, was deleted by antivirus, or
// failed to resolve an import.  So when the mod DLL will not load, say so
// somewhere, in the one place a user will be told to look.
void ComplainToDisk(const wchar_t* what, DWORD err) {
    wchar_t local[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
    std::wstring dir = std::wstring(local) + L"\\theHunterCotWVR";
    CreateDirectoryW(dir.c_str(), nullptr);
    FILE* f = _wfopen((dir + L"\\proxy_error.log").c_str(), L"w");
    if (!f) return;
    fwprintf(f, L"theHunter CotW VR proxy could not start the mod.\n\n");
    fwprintf(f, L"Step that failed : %s\n", what);
    fwprintf(f, L"Windows error    : %lu\n\n", err);
    fwprintf(f, L"Most likely causes, in order:\n");
    fwprintf(f, L"  1. cotwvr.dll or openxr_loader.dll is missing from the game folder.\n");
    fwprintf(f, L"     Both must sit next to theHunterCotW_F.exe.\n");
    fwprintf(f, L"  2. Antivirus deleted or quarantined cotwvr.dll. A DLL that injects and\n");
    fwprintf(f, L"     hooks looks exactly like malware - add a folder exclusion.\n");
    fwprintf(f, L"  3. Wrong bitness. theHunter CotW is 64-bit; a 32-bit build will not load.\n");
    fclose(f);
}

void LoadReal() {
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);   // 64-bit process -> System32
    const std::wstring path = std::wstring(sys) + L"\\XINPUT9_1_0.dll";
    g_real = LoadLibraryW(path.c_str());
    if (!g_real) {
        // The 1.4 loader has an identical ABI for everything we forward.
        g_real = LoadLibraryW((std::wstring(sys) + L"\\xinput1_4.dll").c_str());
    }
    if (!g_real) {
        ComplainToDisk(L"loading the real XINPUT9_1_0.dll from System32", GetLastError());
        return;
    }
    g_getState = reinterpret_cast<PFN_GetState>(GetProcAddress(g_real, "XInputGetState"));
    g_setState = reinterpret_cast<PFN_SetState>(GetProcAddress(g_real, "XInputSetState"));
    g_getCaps  = reinterpret_cast<PFN_GetCapabilities>(GetProcAddress(g_real, "XInputGetCapabilities"));
    g_getGuids = reinterpret_cast<PFN_GetDSoundGuids>(GetProcAddress(g_real, "XInputGetDSoundAudioDeviceGuids"));
}

void LoadMod(HMODULE self) {
    const std::wstring dir = ModuleDir(self);
    g_mod = LoadLibraryW((dir + L"cotwvr.dll").c_str());
    if (!g_mod) {
        ComplainToDisk(L"loading cotwvr.dll from the game folder", GetLastError());
        return;
    }
    g_vrPadPost = reinterpret_cast<PFN_VrPadPost>(GetProcAddress(g_mod, "COTWVR_PadPostProcess"));
}

}  // namespace

extern "C" {

DWORD WINAPI Proxy_XInputGetState(DWORD dwUserIndex, XINPUT_STATE* pState) {
    if (!g_getState) return ERROR_DEVICE_NOT_CONNECTED;
    const DWORD result = g_getState(dwUserIndex, pState);
    if (g_vrPadPost) g_vrPadPost(dwUserIndex, pState, result);
    return result;
}

DWORD WINAPI Proxy_XInputSetState(DWORD dwUserIndex, XINPUT_VIBRATION* pVibration) {
    if (!g_setState) return ERROR_DEVICE_NOT_CONNECTED;
    return g_setState(dwUserIndex, pVibration);
}

DWORD WINAPI Proxy_XInputGetCapabilities(DWORD i, DWORD f, XINPUT_CAPABILITIES* c) {
    if (!g_getCaps) return ERROR_DEVICE_NOT_CONNECTED;
    return g_getCaps(i, f, c);
}

DWORD WINAPI Proxy_XInputGetDSoundAudioDeviceGuids(DWORD i, GUID* render, GUID* capture) {
    if (!g_getGuids) return ERROR_DEVICE_NOT_CONNECTED;
    return g_getGuids(i, render, capture);
}

}  // extern "C"

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(self);
        LoadReal();
        LoadMod(self);
    }
    return TRUE;
}
