#include "rdoc.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

#include "renderdoc_app.h"

#include "log.h"

namespace cotwvr {
namespace {

RENDERDOC_API_1_6_0* g_api = nullptr;

// The mod's own copy of RenderDoc, kept next to the game so the path does not
// depend on the build tree existing on the machine.
std::wstring RenderDocPath() {
    wchar_t exe[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir(exe);
    const size_t slash = dir.find_last_of(L'\\');
    if (slash != std::wstring::npos) dir = dir.substr(0, slash + 1);
    return dir + L"renderdoc.dll";
}

std::wstring CaptureDir() {
    wchar_t local[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return L".";
    const std::wstring root = std::wstring(local) + L"\\theHunterCotWVR";
    const std::wstring dir = root + L"\\captures";
    CreateDirectoryW(root.c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

}  // namespace

bool RenderDocReady() { return g_api != nullptr; }

bool InitRenderDoc() {
    if (g_api) return true;

    // If RenderDoc's own UI launched the game it is already present; otherwise
    // load the copy shipped beside the game.
    HMODULE mod = GetModuleHandleW(L"renderdoc.dll");
    if (!mod) {
        const std::wstring path = RenderDocPath();
        mod = LoadLibraryW(path.c_str());
        if (!mod) {
            COTW_LOG("[rdoc] renderdoc.dll not found next to the game (%ls)", path.c_str());
            return false;
        }
    }

    auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
    if (!getApi) {
        COTW_LOG("[rdoc] RENDERDOC_GetAPI missing");
        return false;
    }
    if (getApi(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&g_api)) != 1 || !g_api) {
        COTW_LOG("[rdoc] RENDERDOC_GetAPI failed");
        g_api = nullptr;
        return false;
    }

    const std::wstring dir = CaptureDir();
    const std::string utf8(dir.begin(), dir.end());
    g_api->SetCaptureFilePathTemplate((utf8 + "\\cotw").c_str());

    // Capture buffer contents so constant buffers can be inspected offline -
    // without these the capture opens but the matrices are not in it.
    g_api->SetCaptureOptionU32(eRENDERDOC_Option_SaveAllInitials, 1);
    g_api->SetCaptureOptionU32(eRENDERDOC_Option_RefAllResources, 1);

    int major = 0, minor = 0, patch = 0;
    g_api->GetAPIVersion(&major, &minor, &patch);
    COTW_LOG("[rdoc] RenderDoc %d.%d.%d loaded, captures go to %ls", major, minor, patch,
             dir.c_str());
    return true;
}

void RenderDocPoll() {
    if (!g_api) return;

    static DWORD last = 0;
    const DWORD now = GetTickCount();
    if (now - last < 500) return;
    last = now;

    wchar_t local[MAX_PATH]{};
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) return;
    const std::wstring trigger =
        std::wstring(local) + L"\\theHunterCotWVR\\capture.trigger";
    if (GetFileAttributesW(trigger.c_str()) == INVALID_FILE_ATTRIBUTES) return;

    DeleteFileW(trigger.c_str());
    TriggerRenderDocCapture();
}

void TriggerRenderDocCapture() {
    if (!g_api) {
        COTW_LOG("[rdoc] not loaded");
        return;
    }
    g_api->TriggerCapture();
    COTW_LOG("[rdoc] capture triggered - the next frame will be written");
}

}  // namespace cotwvr
