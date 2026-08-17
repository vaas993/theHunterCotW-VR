#include "gamesettings.h"

#include "config.h"
#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace cotwvr {
namespace {

// How much of a render the headset can actually DISPLAY depends on its shape,
// and the numbers are not intuitive.
//
// Measured from the runtime (VDXR, Quest 3): each eye sees 54 deg outward and
// 40 deg inward horizontally, 44 up and 55 down vertically. The vertical FOV is
// pinned to 99 deg by the auto-FOV calculation, so the render's ASPECT is what
// decides the horizontal:
//
//     horizontal FOV = 2 * atan( tan(49.5 deg) * aspect )
//
// Both eyes are shown the same image, so a pixel is useful if EITHER eye can
// see it - which means the image is fully used right up to +/-54 deg, and every
// degree beyond that is rendered for nobody:
//
//     aspect 0.92 ->  94 deg : all used, but 7 deg of black at each outer edge
//     aspect 1.00 ->  99 deg : all used, 4.5 deg of black at each outer edge
//     aspect 1.18 -> 108 deg : all used AND no black - the exact optimum
//     aspect 1.33 -> 115 deg : 12% of every frame is drawn where nobody can see it
//     aspect 1.78 -> 133 deg : 35% drawn for nobody
//
// So 1.18:1 is the shape to render: it is the widest that is still entirely
// visible, and the narrowest that leaves no black. Anything wider is pure cost.
const RenderPreset kPresets[] = {
    {"leave the game alone",  0,    0,    "the game's own settings are untouched"},
    // --- 1:1, all visible, small black wedge at the far outer edge of each eye
    {"1:1  1600 x 1600",      1600, 1600, "all visible - very light, for headroom"},
    {"1:1  1920 x 1920",      1920, 1920, "all visible - light"},
    {"1:1  2160 x 2160",      2160, 2160, "all visible - good balance"},
    {"1:1  2560 x 2560",      2560, 2560, "all visible - sharp"},
    {"1:1  2880 x 2880",      2880, 2880, "all visible - sharper, 8.3 MP"},
    {"1:1  3072 x 3072",      3072, 3072, "all visible - matches the headset's own size"},
    {"1:1  3360 x 3360",      3360, 3360, "all visible - supersampled, heavy"},
    // --- 5:4 and 4:3: progressively more of the frame drawn outside the display
    {"5:4  2048 x 1638",      2048, 1638, "6% wasted - light"},
    {"5:4  2560 x 2048",      2560, 2048, "6% wasted"},
    {"4:3  1920 x 1440",      1920, 1440, "12% wasted - light"},
    {"4:3  2560 x 1920",      2560, 1920, "12% wasted"},
    {"4:3  2880 x 2160",      2880, 2160, "12% wasted"},
    {"4:3  3200 x 2400",      3200, 2400, "12% wasted - heavy"},
    // --- wide, only if nothing else is accepted: most of the frame is thrown away
    {"16:10 2560 x 1600",     2560, 1600, "27% wasted"},
    {"16:9  2560 x 1440",     2560, 1440, "35% wasted"},
    // --- 1.18:1, the exact shape of this headset's field of view. Appended
    //     rather than inserted so nobody's saved preset number changes meaning.
    {"HEADSET 1880 x 1600",   1880, 1600, "perfect fit - very light, for headroom"},
    {"HEADSET 2256 x 1920",   2256, 1920, "perfect fit - light"},
    {"HEADSET 2536 x 2160",   2536, 2160, "perfect fit - good balance"},
    {"HEADSET 2824 x 2400",   2824, 2400, "perfect fit - recommended, 6.8 MP"},
    {"HEADSET 3008 x 2560",   3008, 2560, "perfect fit - sharp, 7.7 MP"},
    {"HEADSET 3384 x 2880",   3384, 2880, "perfect fit - supersampled, heavy"},
    // --- 1.072:1, the shape that is actually right for this game.
    //
    // TWO facts, both measured rather than assumed, and the second corrected the
    // first:
    //
    // 1. The game's FOV slider STOPS AT 90 and ignores anything larger written
    //    into settings.json. The mod was writing 108 and then telling the
    //    headset 108, while the game had drawn far less - that mismatch is what
    //    stretched the picture, worse toward the edges.
    //
    // 2. That slider is the VERTICAL field of view, not the horizontal one.
    //    Proof: at aspect 0.853 with the slider on 90, the horizontal that
    //    actually looked correct was 80 deg, and
    //        2 * atan( tan(45 deg) * 0.853 ) = 80.9 deg
    //    which lands within a degree. The old assumption (slider = horizontal)
    //    predicted 90 and was wrong by ten.
    //
    // So the vertical is FIXED at 90 whatever we do - the headset's 99 deg
    // cannot be filled, and ~4.5 deg of black at top and bottom is unavoidable.
    // But the horizontal is
    //
    //     horizontal FOV = 2 * atan( aspect )        (since tan(90/2) = 1)
    //
    // so a WIDER render buys horizontal field for free, at no cost in vertical.
    // aspect = tan(47 deg) = 1.0724 gives exactly the headset's 94 deg across.
    // Going wider still (1.376 = 108 deg) removes the last slivers of black at
    // each eye's outer edge, at 28% more pixels.
    {"FOV90 1716 x 1600",     1716, 1600, "94 deg across - very light"},
    {"FOV90 2058 x 1920",     2058, 1920, "94 deg across - light"},
    {"FOV90 2316 x 2160",     2316, 2160, "94 deg across - good balance"},
    {"FOV90 2574 x 2400",     2574, 2400, "94 deg across - RECOMMENDED, 6.2 MP"},
    {"FOV90 2744 x 2560",     2744, 2560, "94 deg across - sharp, 7.0 MP"},
    {"FOV90 3088 x 2880",     3088, 2880, "94 deg across - supersampled, 8.9 MP"},
    // Wider still: 108 deg across, so neither eye has any black at its outer
    // edge. Costs 28% more pixels than the 94 deg entries.
    {"FOV90-WIDE 2752 x 2000", 2752, 2000, "108 deg across - no edge black, light"},
    {"FOV90-WIDE 3304 x 2400", 3304, 2400, "108 deg across - no edge black, 7.9 MP"},
    {"FOV90-WIDE 3854 x 2800", 3854, 2800, "108 deg across - no edge black, heavy"},
    // --- 0.926:1 - TALLER THAN WIDE, because the headset's own frustum is.
    //
    // Measured from the runtime rather than guessed:
    //     runtime eye0 fov  L-54.0  R40.0  U44.0  D-55.0 deg
    // which is 94 deg across and 99 deg TALL - an aspect of 0.926:1 in tangent
    // space. Every other family here is 1:1 or wider, so with the widest one
    // selected we submit +/-47.5 h, +/-38.4 v: about right horizontally and
    // 22 degrees short vertically. The headset can show that; we were not
    // drawing it.
    //
    // Near-square is not a coincidence of this headset. A real per-eye frustum
    // is near-square on every current device, which is why Luke Ross's mods land
    // on ratios like 0.974:1 and the Quest 3's own recommendation is 0.941:1.
    // 16:9 is the shape of a monitor, not of an eye.
    //
    // The game's FOV slider is VERTICAL and stops at 90, so these give roughly
    // 90 v x 86 h - trading a few degrees of width for a large gain in height.
    {"FULLVIEW 1784 x 1928", 1784, 1928, "fills the headset - very light, for headroom"},
    {"FULLVIEW 2224 x 2400", 2224, 2400, "fills the headset - light"},
    {"FULLVIEW 2560 x 2768", 2560, 2768, "fills the headset - good balance"},
    {"FULLVIEW 2880 x 3112", 2880, 3112, "fills the headset - RECOMMENDED, 9.0 MP"},
    {"FULLVIEW 3072 x 3320", 3072, 3320, "fills the headset - sharp, 10.2 MP"},
    {"FULLVIEW 3456 x 3736", 3456, 3736, "fills the headset - supersampled, heavy"},
    // --- Sized so that AFTER the per-eye crop each eye lands on the headset's
    //     own recommended pixel count.
    //
    // The crop keeps about 78% of the width and 80% of the height for each eye,
    // so roughly 63% of what is rendered reaches an eye at all. At 3456x3736
    // that leaves 2696x2996 per eye against a recommendation of 3072x3264 -
    // measurably soft. Dividing the recommendation by the crop fractions gives
    // these numbers, which cost ~24% more pixels than the entry above.
    {"NATIVE-PER-EYE 3936 x 4072", 3936, 4072,
     "each eye lands on the headset's own resolution - heaviest"},
};
constexpr int kPresetCount = int(sizeof(kPresets) / sizeof(kPresets[0]));

// The game's FOV slider stops here, and it ignores larger values written into
// settings.json - confirmed against the game's own video menu, which still read
// 80 after the mod had written 108.
constexpr int kGameFovMax = 90;

// The game keeps per-Steam-account settings; use the most recently written.
std::wstring FindSettingsJson() {
    wchar_t* docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return L"";
    std::wstring root = std::wstring(docs) +
        L"\\Avalanche Studios\\theHunter Call of the Wild\\Saves\\settings";
    CoTaskMemFree(docs);

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return L"";

    std::wstring best;
    FILETIME bestTime{};
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == L'.') continue;
        const std::wstring candidate = root + L"\\" + fd.cFileName + L"\\settings.json";
        WIN32_FILE_ATTRIBUTE_DATA ad{};
        if (!GetFileAttributesExW(candidate.c_str(), GetFileExInfoStandard, &ad)) continue;
        if (best.empty() || CompareFileTime(&ad.ftLastWriteTime, &bestTime) > 0) {
            best = candidate;
            bestTime = ad.ftLastWriteTime;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

// Replace "Key": <number> in place. The file is small and flat, so this is
// simpler and far less risky than parsing and re-emitting JSON - a malformed
// rewrite would cost the player their settings.
// Read "Key": <number> without changing anything. Same scan as the writer below,
// so the two cannot disagree about where a value starts and ends.
bool ReadNumber(const std::string& json, const char* key, int* out) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t k = json.find(needle);
    if (k == std::string::npos) return false;
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    size_t start = colon + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;
    size_t end = start;
    while (end < json.size() && (isdigit((unsigned char)json[end]) || json[end] == '-')) ++end;
    if (end == start) return false;
    if (out) *out = atoi(json.substr(start, end - start).c_str());
    return true;
}

bool ReplaceNumber(std::string& json, const char* key, int value) {
    const std::string needle = std::string("\"") + key + "\"";
    const size_t k = json.find(needle);
    if (k == std::string::npos) return false;
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return false;
    size_t start = colon + 1;
    while (start < json.size() && (json[start] == ' ' || json[start] == '\t')) ++start;
    size_t end = start;
    while (end < json.size() && (isdigit((unsigned char)json[end]) || json[end] == '-')) ++end;
    if (end == start) return false;
    json.replace(start, end - start, std::to_string(value));
    return true;
}

}  // namespace

// *** THE SIZE THIS LAUNCH IS BUILT AROUND, FIXED WHEN IT WAS WRITTEN. ***
//
// The upscaler needs to know what the headset should be handed, and the answer
// is the preset the GAME WAS STARTED WITH - not whatever the panel row happens
// to be showing now. Reading the live row made scrolling that row rebuild both
// eye swapchains on every nudge and visibly change the world, which the owner
// reported at once and was right to: a setting that says TAKES EFFECT NEXT TIME
// YOU START THE GAME must not do anything this time.
//
// Zero until ApplyGameSettings has run, and zero forever if no preset is
// selected - in which case the caller falls back to its own arithmetic.
int g_launchTargetW = 0, g_launchTargetH = 0;

void LaunchRenderTarget(int* w, int* h) {
    *w = g_launchTargetW;
    *h = g_launchTargetH;
}

int RenderPresetCount() { return kPresetCount; }

const RenderPreset& GetRenderPreset(int index) {
    if (index < 0 || index >= kPresetCount) index = 0;
    return kPresets[index];
}

void ApplyGameSettings() {
    // A CUSTOM SIZE WINS OVER THE PRESET.
    //
    // The presets are a menu of sensible shapes, not a limit. A headset this
    // table has never seen wants a shape none of them carry, and the mod does
    // not care what the number is: the per-eye crop and the FOV both work off
    // whatever the game actually renders, measured at runtime.
    static RenderPreset custom{};
    const bool useCustom = Cfg().render_custom_w > 0 && Cfg().render_custom_h > 0;
    if (useCustom) {
        custom.name = "CUSTOM";
        custom.width = Cfg().render_custom_w;
        custom.height = Cfg().render_custom_h;
        custom.note = "your own resolution";
    }
    // A SECOND RESOLUTION FOR ALTERNATE-EYE MODE LIVED HERE, and it is gone.
    // The reasoning was sound - one eye per frame can afford more pixels than
    // two - but it made the resolution depend on the rendering mode, which is
    // a second thing to keep in your head for a sharpness difference nobody
    // asked for. One resolution, whichever mode is running.
    const RenderPreset& p = useCustom ? custom : GetRenderPreset(Cfg().render_preset);
    if (p.width <= 0 || p.height <= 0) {
        COTW_LOG("[gamecfg] render preset 0 - leaving the game's own settings alone");
        return;
    }

    const std::wstring path = FindSettingsJson();
    if (path.empty()) {
        COTW_LOG("[gamecfg] could not find the game's settings.json - resolution "
                 "not changed");
        return;
    }

    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) {
        COTW_LOG("[gamecfg] settings.json is not readable");
        return;
    }
    std::string json;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) json.append(buf, n);
    fclose(f);

    // Keep one pristine copy from before the mod ever touched it.
    const std::wstring backup = path + L".cotwvr_backup";
    if (GetFileAttributesW(backup.c_str()) == INVALID_FILE_ATTRIBUTES) {
        CopyFileW(path.c_str(), backup.c_str(), TRUE);
        COTW_LOG("[gamecfg] kept an original copy at settings.json.cotwvr_backup");
    }

    // *** DLSS UPSCALING IS ONE KNOB, NOT TWO. ***
    //
    // The upscaler rebuilds each eye by 100/pct on its way to the headset. On its
    // own that buys nothing: the game still draws everything, so the frame rate
    // is unchanged and the extra pixels are resampled away by the compositor.
    // The frames come from the OTHER half - the game drawing less - and that is
    // this file's job, because this is what writes the game's resolution.
    //
    // So the preset stays the size the headset is HANDED, and the game is told
    // to render pct% of it. The two multiply back out: 3072x3320 at 58% is
    // rendered 1782x1926 and reconstructed to 3072x3320. Aspect is untouched,
    // both axes scale by the same factor, so the FOV maths below is unaffected.
    // Stamped here, where the resolution is decided for this launch, so nothing
    // downstream has to guess which preset the game is actually running.
    g_launchTargetW = p.width;
    g_launchTargetH = p.height;

    int renderW = p.width, renderH = p.height;
    const int upct = DlssRenderPct();
    if (Cfg().dlss_enable && upct >= 33 && upct <= 99) {
        renderW = ((p.width * upct / 100) + 1) & ~1;    // even, as D3D prefers
        renderH = ((p.height * upct / 100) + 1) & ~1;
        COTW_LOG("[gamecfg] DLSS upscaling at %d%%: the headset still gets %dx%d, "
                 "but the game is told to render %dx%d - %d%% of the pixels. That "
                 "is where the frame rate comes from; the mod rebuilds the size.",
                 upct, p.width, p.height, renderW, renderH, upct * upct / 100);
    }

    const bool okW = ReplaceNumber(json, "DisplayWidth", renderW);
    const bool okH = ReplaceNumber(json, "DisplayHeight", renderH);

    // The FOV that exactly fills the headset depends only on the render's shape:
    //     horizontal FOV = 2 * atan( tan(headset vertical half) * aspect )
    // so it is worth computing rather than guessing. -1 asks for that.
    int fovToWrite = Cfg().force_game_fov;
    // ZERO MEANS 'DO NOT FORCE', NOT 'USE ZERO'.
    //
    // Only negatives counted as auto, so a 0 in the ini was written into the
    // game's settings.json verbatim: '[gamecfg] set the game to 3072x3320,
    // FOV 0'. A field of view of zero is not a setting anyone can have meant,
    // and off is far more likely to be spelled 0 than -1.
    if (fovToWrite <= 0) {
        // *** THE GAME'S FOV IS VERTICAL. WRITE A VERTICAL NUMBER. ***
        //
        // This computed the HORIZONTAL angle needed and wrote that, which is why
        // a near-square preset produced 'needs 95 degrees horizontally' and then a
        // warning that the game would ignore it. The game's own slider is the
        // VERTICAL field of view - so the number to write is simply how much
        // vertical the headset wants, and the horizontal follows from the shape.
        const float kDeg = 0.01745329252f;
        // The aspect the game will actually RENDER at, which under upscaling is
        // the reduced size - the same ratio to within a pixel, but this is the
        // number the FOV has to agree with.
        const float aspect = float(renderW) / float(renderH);
        fovToWrite = int(Cfg().headset_vfov_deg + 0.5f);
        const float hFov =
            2.0f * atanf(tanf(fovToWrite * 0.5f * kDeg) * aspect) / kDeg;
        COTW_LOG("[gamecfg] auto FOV: %dx%d is aspect %.3f - asking the game for %d "
                 "degrees VERTICAL, which gives %.0f degrees across",
                 renderW, renderH, aspect, fovToWrite, hFov);
        // The game's slider stops at 90 and silently ignores anything higher.
        // Asking for more and then TELLING THE HEADSET we got it is what made
        // the picture stretch, so say so loudly rather than assume it took.
        if (fovToWrite > kGameFovMax) {
            COTW_LOG("[gamecfg] *** the game's FOV maximum is %d - it will IGNORE %d and "
                     "render %d instead. Pick one of the FOV90 presets, whose shape is "
                     "built around that limit, or the picture will stretch. ***",
                     kGameFovMax, fovToWrite, kGameFovMax);
            fovToWrite = kGameFovMax;
        }
    }
    if (fovToWrite > 0) {
        ReplaceNumber(json, "GameFOV", fovToWrite);
    }
    // Windowed is what lets an arbitrary size be used at all - a fullscreen
    // swapchain is limited to modes the display actually advertises.
    ReplaceNumber(json, "DisplayFullscreen", 0);

    // THE GAME'S GRAPHICS OPTIONS, APPLIED HERE FOR THE SAME REASON AS THE SIZE.
    //
    // These used to be written by the launcher, into this same file, while the
    // game was closed - and they kept reverting. The game rewrites the whole
    // file from memory when it quits, and regenerates it on the first run after
    // a reinstall, so a write made between sessions lasts exactly until the next
    // one ends. The resolution never suffered from that because it is written
    // from DllMain on every launch. So are these now.
    //
    // The old value is logged beside the new one: the two together are the only
    // way to tell 'the mod did not write' from 'the game wrote back over it',
    // and not being able to tell those apart is what cost the owner an evening.
    if (Cfg().apply_game_graphics) {
        const struct { const char* key; int want; } kGraphics[] = {
            {"GraphicsMotionBlur",     Cfg().graphics_motion_blur},
            {"GraphicsDepthOfField",   Cfg().graphics_depth_of_field},
            {"GraphicsVignette",       Cfg().graphics_vignette},
            {"GraphicsSSAO",           Cfg().graphics_ssao},
            {"GraphicsSSReflection",   Cfg().graphics_ssr},
            {"GraphicsContactShadows", Cfg().graphics_contact_shadows},
            {"GraphicsAA",             Cfg().graphics_aa},
        };
        for (const auto& g : kGraphics) {
            if (g.want < 0) continue;              // -1 = leave the player's own
            int before = -1;
            const bool had = ReadNumber(json, g.key, &before);
            if (!had) {
                COTW_LOG("[gamecfg] %s is not in this build's settings.json - skipped",
                         g.key);
                continue;
            }
            if (before == g.want) {
                COTW_LOG("[gamecfg] %-22s already %d", g.key, g.want);
                continue;
            }
            ReplaceNumber(json, g.key, g.want);
            COTW_LOG("[gamecfg] %-22s %d -> %d", g.key, before, g.want);
        }
    }

    if (!okW || !okH) {
        COTW_LOG("[gamecfg] settings.json did not contain the expected keys "
                 "(width %d, height %d) - nothing written", okW, okH);
        return;
    }

    f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        COTW_LOG("[gamecfg] settings.json is not writable (read-only?)");
        return;
    }
    fwrite(json.data(), 1, json.size(), f);
    fclose(f);

    COTW_LOG("[gamecfg] set the game to %dx%d, FOV %d  (%s)", renderW, renderH,
             fovToWrite, p.name);
    COTW_LOG("[gamecfg] %s", p.note);
    // Keep the mod's layer FOV in step automatically. Two numbers that must
    // agree, set by hand in two different files, is a mismatch waiting to
    // happen - and a mismatch here is exactly what made the picture look
    // stretched earlier.
    if (!Cfg().game_fov_follows_auto) {
        COTW_LOG("[gamecfg] game_fov_deg left at %.1f - you set it by hand "
                 "(game_fov_follows_auto = 0)", Cfg().game_fov_deg);
    } else if (fovToWrite > 0 &&
               fabsf(Cfg().game_fov_deg - float(fovToWrite)) > 0.5f) {
        Cfg().game_fov_deg = float(fovToWrite);
        Cfg().fov_is_horizontal = true;
        Cfg().Save();
        COTW_LOG("[gamecfg] the mod's layer FOV follows automatically: "
                 "game_fov_deg = %d (horizontal)", fovToWrite);
    }
}

}  // namespace cotwvr
