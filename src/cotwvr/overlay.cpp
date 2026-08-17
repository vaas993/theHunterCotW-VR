#include "overlay.h"

#include "audio.h"
#include "cheats.h"
#include "config.h"
#include "gamesettings.h"
#include "log.h"
#include "vr.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include <MinHook.h>

namespace cotwvr {

Overlay g_overlay;

// When the GAME last handed us the pad through the XInput proxy. Declared
// up here because ConsumePad writes it and the mirror window - far below -
// reads it to decide whether it needs to poll the pad itself.
volatile LONG g_lastPadFeed = 0;

namespace {

// --- palette --------------------------------------------------------------
// Warm and woodland rather than the usual neon-on-black: this is a hunting
// game, and a panel that glows blue in a forest at dusk is jarring.
constexpr uint32_t kPanelTop   = 0x1B2422;
constexpr uint32_t kPanelBot   = 0x0C1211;
constexpr uint32_t kAccent     = 0xE0A44C;   // amber
constexpr uint32_t kAccentDim  = 0x7A5A2A;
constexpr uint32_t kText       = 0xEDF1F0;
constexpr uint32_t kTextDim    = 0x93A09C;
constexpr uint32_t kHeading    = 0x8FBF7F;   // muted green
constexpr uint32_t kRule       = 0x2C3936;
constexpr uint32_t kOn         = 0x7FD08A;
constexpr uint32_t kOff        = 0x6B7A76;

// Heading starts a TAB. Group is a section label INSIDE one, and the distinction
// is worth stating because the name does not give it away: adding four Headings
// to organise one long tab silently created four new TABS rather than four
// sections within it.
enum class RK { Heading, Group, Bool, Int, Float, Percent, Action, Key };

// `act` and `val` are last so every existing row initialiser still compiles -
// the members they leave out are value-initialised.
struct Row {
    RK kind;
    const char* label;
    const char* help;
    void* ptr;
    float lo, hi, step;
    const char* const* names;
    int nameCount;
    const char* def;
    void (*act)(int dir);        // RK::Action: left/right nudge it
    const char* (*val)();        // live text, for values the GAME owns
    // *** A ROW THAT ONLY EXISTS SOMETIMES. ***
    //
    // Null means always shown, which is every row but one. When it returns
    // false the row is not drawn and the cursor cannot land on it, so a setting
    // that has no meaning in the current mode is neither visible nor
    // adjustable - rather than sitting there inviting a change that does
    // nothing. Kept as a predicate rather than a flag so there is no second
    // copy of the condition to fall out of step.
    bool (*show)();
};

// --- time of day, as a nudge rather than a slider -------------------------
//
// It was a -1..24 slider where -1 meant "off", and it applied the moment it
// reached 0 and reset itself to -1. So it just flicked between -1 and -0.5 and
// could never be walked up to a usable hour: the control fought the mechanism.
// A clock is a thing you nudge, not a thing you aim at on a scale - and it can
// show the game's REAL time instead of the value we last asked for.
void TimeNudge(int dir) {
    float h = 12.0f;
    if (!GameHour(&h)) return;          // not in a world yet; nothing to nudge
    h += static_cast<float>(dir);
    while (h < 0.0f) h += 24.0f;
    while (h >= 24.0f) h -= 24.0f;
    Cfg().cheat_set_hour = h;
}

const char* TimeText() {
    static char buf[48];
    float h = 0.0f;
    if (!GameHour(&h)) {
        snprintf(buf, sizeof(buf), "-- : --   (not in a world)");
        return buf;
    }
    int hh = static_cast<int>(h);
    int mm = static_cast<int>((h - hh) * 60.0f);
    if (mm >= 60) { mm = 0; ++hh; }
    if (hh >= 24) hh -= 24;
    const char* part = (hh < 5)  ? "night" : (hh < 8)  ? "dawn"
                     : (hh < 17) ? "day"   : (hh < 20) ? "dusk" : "night";
    snprintf(buf, sizeof(buf), "%02d:%02d   %s", hh, mm, part);
    return buf;
}

// --- profiles, and putting everything back --------------------------------
//
// A profile is the same ini under another name (see Config::SaveProfile), so
// there is one file format and one parser. These are just the panel's end of it.
void ProfileSaveAct(int) { Cfg().SaveProfile(Cfg().profile_slot); }
void ProfileLoadAct(int) { Cfg().LoadProfile(Cfg().profile_slot); }

const char* ProfileSaveText() {
    static char buf[64];
    snprintf(buf, sizeof(buf), "slot %d  -  %s", Cfg().profile_slot,
             Cfg().ProfileExists(Cfg().profile_slot) ? "in use" : "empty");
    return buf;
}
const char* ProfileLoadText() {
    static char buf[64];
    snprintf(buf, sizeof(buf), "%s",
             Cfg().ProfileExists(Cfg().profile_slot) ? "press to load" : "slot is empty");
    return buf;
}

// A row that is a NOTICE rather than a setting: it carries a warning in its help
// text and does nothing when nudged. Cheaper than a new row kind, and it keeps
// the warning where the cursor will land on it rather than in a manual nobody
// reads.
void NoteAct(int) {}
const char* NoteText() { return "read the note below"; }

// *** THE SETTINGS THIS MOD IS MEANT TO BE PLAYED ON. ***
//
// Not the same thing as "the defaults": the defaults are what every switch was
// born with, most of them off, because a feature ships off until it is proven.
// This is the tuned configuration - the one arrived at by testing in the
// headset, which is what a new player actually wants and what makes the mod
// shippable to someone who will never read a log.
//
// Two presses, like the full reset, so a stray nudge cannot undo an evening.
int g_recommendArmed = 0;
void RecommendAct(int dir) {
    if (dir > 0 && g_recommendArmed) {
        Config& c = Cfg();
        // *** THIS IS A CONFIGURATION THAT WAS PLAYED, NOT ONE THAT WAS ARGUED
        // FOR. *** Captured 2026-08-16 from the settings actually in use after
        // the DLSS, sharpening and 6DoF work had all been tested in the headset.
        // Where a value here disagrees with the reasoning in a comment
        // elsewhere, this one is what was on screen and working.
        //
        // Picture: the mod's own resolve pass, with DLSS reconstructing on top.
        c.taa_replace_pass = true;
        c.dlss_enable = true;
        c.dlss_preset = 2;
        c.dlss_quality = 2;             // Balanced - the game draws 58% per axis
        c.dlss_upscale_solo = true;
        c.dlss_jitter_mode = 1;         // measured
        c.dlss_auto_exposure = true;
        c.dlss_depth_inverted = true;
        c.dlss_mv_jittered = true;
        c.taa_single_accumulation = false;
        c.dlss_upscale_rect_latch = true;
        c.post_sharpen = 0.35f;
        c.post_saturation = 1.10f;
        // View: the wider field. The per-eye crop is OFF - it is the sharper
        // arrangement in principle and it did not survive testing alongside the
        // reconstruction.
        c.tier1_fov = true;
        c.tier1_per_eye_crop = false;
        // Stereo and head tracking.
        c.stereo = true;
        c.head_tracking = true;
        // Roll ON: your head tips and the horizon tips with it, which is what
        // the world does. It was off for a while because injecting roll was
        // once blamed for a headset vibration - that vibration was traced
        // elsewhere, and this is the setting that has been played since.
        c.head_write_roll = true;
        c.head_tremor_level = 0;        // filtering the view always made it worse
        c.prediction_damp_pct = 0;
        // 6DoF is OFF here, so this is the tracking a new player starts with -
        // it is switched on in one row on the WORLD tab, and the row explains
        // what it adds.
        c.six_dof = false;
        c.six_dof_frame_mode = 1;       // measured: the basis turns with the head
        c.six_dof_scale = 1.00f;
        c.six_dof_anchor_on_enter = true;
        // Weapon depth is ON. It is what puts the gun in your hands instead of
        // pasted to your face, and it is worth the one thing it costs: it feeds
        // a second camera into the resolve's choice of which matrix drew the
        // frame, so on some scenes the WORLD picks up a shake. One row turns it
        // off if that bothers you more than a flat weapon does.
        c.weapon_3d = true;
        // Pacing and the two engine effects that alternate-eye rendering cannot
        // tolerate. Motion blur is computed from frame-to-frame motion, and
        // under AER that motion INCLUDES the eye flip - so it blurs along the
        // 64 mm gap between the eyes every other frame, which is the repeating
        // stereo echo Halo MCC VR names in its own defaults.
        c.desktop_present_unlocked = true;
        c.frame_latency = 1;
        c.apply_game_graphics = true;
        c.graphics_aa = 3;               // FXAA+TAA: the pass DLSS is built on
        c.graphics_motion_blur = 0;
        c.graphics_depth_of_field = 0;   // focus chosen for a monitor, not eyes
        c.Save();
        g_recommendArmed = 0;
        COTW_LOG("[panel] recommended settings applied and saved");
        return;
    }
    g_recommendArmed = (dir < 0) ? 1 : 0;
}
const char* RecommendText() {
    return g_recommendArmed ? "sure?  press RIGHT to confirm" : "press LEFT, then RIGHT";
}

// Deliberately needs left AND right - one nudge arms it, the other does it - so
// a stray press on a menu nobody meant to open cannot wipe a tuned setup.
int g_resetArmed = 0;
void ResetAct(int dir) {
    if (dir > 0 && g_resetArmed) {
        Cfg().ResetToDefaults();
        g_resetArmed = 0;
        return;
    }
    g_resetArmed = (dir < 0) ? 1 : 0;
}
const char* ResetText() {
    return g_resetArmed ? "sure?  press RIGHT to confirm" : "press LEFT, then RIGHT";
}

// *** THE QUALITY ROW ANSWERS THE QUESTION, NOT THE SETTING. ***
//
// "Performance" tells the player nothing they can check. The two sizes tell
// them everything: what the game is drawing this instant, and what the headset
// is being handed. If the second is bigger, DLSS is reconstructing, and the
// player can see that it is rather than take it on trust - which is exactly
// what the first upscaling run failed to give them.
// *** AND IT SAYS SO WHEN IT IS NOT IN EFFECT YET. ***
//
// The game reads its own resolution once, at startup, so moving the quality
// level does nothing to what the game DRAWS until the next launch. That is not
// something to leave in a help page nobody has open at the time: the row itself
// says RESTART TO APPLY, and stops saying it the moment the game comes back at
// the right size. Worked out by comparing the frame the game is actually
// drawing with the frame this setting would have asked for - measured, not
// remembered, so it is right even after a profile load or a manual ini edit.
bool DlssRestartPending() {
    if (!Cfg().dlss_enable) return false;
    uint32_t fw = 0, fh = 0;
    VR().CurrentFrameSize(&fw, &fh);
    if (!fw) return false;                       // nothing drawn yet - say nothing
    const RenderPreset& p = (Cfg().render_custom_w > 0 && Cfg().render_custom_h > 0)
                                ? RenderPreset{"CUSTOM", Cfg().render_custom_w,
                                               Cfg().render_custom_h, ""}
                                : GetRenderPreset(Cfg().render_preset);
    if (p.width <= 0) return false;              // "leave the game alone"
    const int want = ((p.width * DlssRenderPct() / 100) + 1) & ~1;
    return (int)fw != want && abs((int)fw - want) > 2;
}

// The percentage is the Custom slot's value and nothing else's. On Quality or
// Performance it is inert - so it is not shown, rather than shown and ignored,
// which is the kind of row that gets nudged and then blamed for doing nothing.
bool DlssCustomChosen() { return Cfg().dlss_quality == 5; }

const char* DlssQualityText() {
    static char buf[112];
    static const char* const kNames[] = {"DLAA - no upscaling", "Quality",
                                         "Balanced", "Performance",
                                         "Ultra Performance", "Custom"};
    int q = Cfg().dlss_quality;
    if (q < 0) q = 0;
    if (q > 5) q = 5;
    const int pct = DlssRenderPct();
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    VR().CurrentSizes(&rw, &rh, &ow, &oh);
    if (!Cfg().dlss_enable) {
        snprintf(buf, sizeof(buf), "%s  (DLSS is off)", kNames[q]);
    } else if (DlssRestartPending()) {
        snprintf(buf, sizeof(buf), "%s %d%%  -  RESTART TO APPLY", kNames[q], pct);
    } else if (rw && ow > rw) {
        snprintf(buf, sizeof(buf), "%s %d%%  -  %ux%u  ->  %ux%u", kNames[q], pct,
                 rw, rh, ow, oh);
    } else if (rw) {
        snprintf(buf, sizeof(buf), "%s %d%%  -  %ux%u, not rebuilt", kNames[q], pct,
                 rw, rh);
    } else {
        snprintf(buf, sizeof(buf), "%s  %d%%", kNames[q], pct);
    }
    return buf;
}

// *** TWO SWITCHES THAT MUST MOVE TOGETHER GET ONE CONTROL. ***
//
// DLSS cannot run without the mod's per-eye resolve: that pass is where it is
// handed depth, motion vectors and jitter, and with it off DLSS declines every
// frame. Two independent switches therefore had a combination that looks
// enabled and does nothing - the exact fault this project has hit four times
// under other names - so the panel offers ONE row with three states and sets
// both underneath. The ini keeps them separate, because they are separate
// things and a config file is allowed to be more literal than a menu.
void AaModeAct(int dir) {
    Config& c = Cfg();
    int mode = c.dlss_enable ? 2 : (c.taa_replace_pass ? 1 : 0);
    mode += (dir > 0) ? 1 : -1;
    if (mode > 2) mode = 0;
    if (mode < 0) mode = 2;
    c.taa_replace_pass = (mode >= 1);
    c.dlss_enable = (mode == 2);
    // *** AND THE GAME'S OWN ANTI-ALIASING FOLLOWS. ***
    //
    // Both mod modes REPLACE the engine's temporal pass, so that pass has to
    // exist: FXAA+TAA. "Off" wants FXAA alone instead - the engine's own TAA is
    // the one that ghosts in stereo, because each eye's history is the other
    // eye's picture. Written at the next launch, like every game setting.
    c.graphics_aa = (mode == 0) ? 1 : 3;
}

const char* AaModeText() {
    const Config& c = Cfg();
    if (c.dlss_enable) return "NVIDIA DLSS";
    if (c.taa_replace_pass) return "Per-eye smoothing (mod)";
    return "Off - the game's own";
}

// Only shown when DLSS is the chosen mode: the rows under it configure DLSS and
// mean nothing otherwise.
bool DlssChosen() { return Cfg().dlss_enable; }
// The 6DoF detail rows - scale, travel limit, the axis switches - describe
// MOVEMENT, so in 3DoF there is nothing for them to describe.
bool SixDofOn() { return Cfg().six_dof; }
// Shown when the mod is doing the smoothing itself - DLSS ignores these.
bool ModSmoothingChosen() {
    return Cfg().taa_replace_pass && !Cfg().dlss_enable;
}

// The preset's NAME plus what the game is actually drawing, which with DLSS
// upscaling on are two different sizes - and the player is entitled to see
// both on the row that sets it.
const char* RenderPresetText() {
    static char buf[112];
    const RenderPreset& p = GetRenderPreset(Cfg().render_preset);
    uint32_t rw = 0, rh = 0, ow = 0, oh = 0;
    VR().CurrentSizes(&rw, &rh, &ow, &oh);
    if (DlssRestartPending()) {
        snprintf(buf, sizeof(buf), "%s  -  RESTART TO APPLY", p.name);
    } else if (rw && p.width > 0 && (int)rw != p.width) {
        snprintf(buf, sizeof(buf), "%s  -  drawing %ux%u", p.name, rw, rh);
    } else {
        snprintf(buf, sizeof(buf), "%s", p.name);
    }
    return buf;
}

const char* kAxisNames[] = {"row 0  (horizontal)", "row 1  (UP - test only)",
                            "row 2  (horizontal)"};
// Taken straight from the one preset table in gamesettings.cpp. A second,
// hand-maintained copy of the same list would eventually drift, and the symptom
// would be the panel naming one resolution while the game is set to another.
const char* const* PresetNames() {
    static std::vector<const char*> names;
    if (names.empty()) {
        for (int i = 0; i < RenderPresetCount(); ++i) names.push_back(GetRenderPreset(i).name);
    }
    return names.data();
}
int PresetNameCount() { return RenderPresetCount(); }
const char* kHeadTestNames[] = {"off  (real head)", "yaw  30 deg", "pitch 30 deg",
                                "roll 30 deg"};
const char* kPatchFlagNames[] = {"group A only", "group B only", "both"};
const char* kWeaponTestNames[] = {"off", "collapse (gun must vanish)",
                                 "shove 5 m away", "sideways, other layout"};
const char* kWeaponAxisNames[] = {"axis 0", "axis 1", "axis 2"};
const char* kPatchSiteNames[] = {"both places", "first place only",
                                 "second place only", "nothing (control)"};

Row* Rows(int* count) {
    Config& c = Cfg();

// Named states for a yes/no setting. "off" is not the absence of stereo
// here - it is the other kind, and saying OFF hid that for months.
static const char* const kTremorLevelNames[] = {"off", "light", "medium", "strong"};
static const char* const kDlssPresetNames[] = {"game default", "J - less smear",
                                               "K - all-rounder",
                                               "L - strongest, costly",
                                               "M - ghost killer"};
static const char* const kDlssJitterModeNames[] = {"not reported (old)",
                                                   "measured", "manual (ini)"};
static const char* const kStereoModeNames[] = {"AER  (alternate eyes)",
                                              "Full rate  (both eyes)"};
// The two states have NAMES because "off" is not the absence of tracking here -
// it is the other kind, and 3DoF/6DoF is what every headset and every VR mod
// calls them. A row reading OFF would suggest head tracking itself stops.
static const char* const kTrackingModeNames[] = {"3DoF  -  looking only",
                                                 "6DoF  -  looking and moving"};

    static Row rows[] = {
        // =====================================================================
        // FIRST TAB, AND THE PANEL OPENS ON IT UNTIL IT IS DISMISSED.
        //
        // Three things a new player cannot discover by exploring - the game's
        // own anti-aliasing setting, that this is unfinished, and that it is not
        // for online play - plus the two keys they will reach for in the first
        // minute. Everything here is a LABEL rather than help text, because help
        // only shows for the row the cursor is on and a warning nobody selects
        // is a warning nobody reads.
        // =====================================================================
        {RK::Heading, "START HERE", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Group, "WHICH GAME VERSION THIS IS FOR", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Action, "Built for update 9.2 - Peru Hunting Reserve",
         "THIS BUILD OF THE MOD IS MADE FOR GAME UPDATE 9.2 (Peru Hunting Reserve) AND MAY "
         "NOT WORK ON ANY OTHER VERSION. That is not caution for its own sake: several "
         "features work by writing to exact addresses inside the game's own code - the aim "
         "re-seed that fixes the crouch drift, the time-of-day controls, the camera hooks - "
         "and those addresses move whenever the game is patched. On a different version "
         "they mean nothing. "
         "The mod checks this for itself at startup and switches those features OFF rather "
         "than writing to an address that has moved, so a newer game will not be damaged "
         "by it - but you will lose whatever those features do until the mod is rebuilt "
         "for the new version. If the game has updated since you downloaded this, look for "
         "a newer version of the mod.",
         nullptr, 0, 0, 0, nullptr, 0, "9.2 only", &NoteAct, &NoteText},

        {RK::Group, "PLEASE READ - FOUR THINGS", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Action, "1. Set the game's Field of View to 90",
         "IN THE GAME'S OWN VIDEO SETTINGS, not on the VIEW tab here. Slide the game's "
         "Field of View to its maximum, 90. "
         "The mod cannot get more view out of the game than the game draws, and 90 is as "
         "wide as this one goes - the slider stops there and ignores anything larger "
         "written behind its back. Anything less and the picture reaching your headset is "
         "narrower than it needs to be, which shows up as a world that feels too close and "
         "black creeping in at the edges when you look around. "
         "Set it once and leave it. The mod works out its own matching field from there, "
         "and the VIEW tab is for adjusting that afterwards.",
         nullptr, 0, 0, 0, nullptr, 0, "set it to 90", &NoteAct, &NoteText},

        {RK::Action, "2. For DLSS: game AA must be FXAA + TAA",
         "ONLY MATTERS IF YOU WANT DLSS OR THE GHOSTING FIX. The rest of the mod - stereo, "
         "head tracking, 6DoF, the field of view, the weapon - does not care what the "
         "game's anti-aliasing is set to. "
         "But those two work by TAKING OVER the game's own smoothing step, so if the game "
         "is not doing any smoothing there is nothing to take over and they do nothing at "
         "all. Set Anti-aliasing to FXAA+TAA in the game's own Video settings; the mod "
         "cannot set it for you. If the PICTURE tab seems to change nothing however you "
         "set it, this is why. "
         "THE OTHER WAY ROUND MATTERS TOO: if you ever set the mod's Smoothing to Off, put "
         "the game back to FXAA alone. FXAA+TAA with nothing taking it over is what "
         "produces the doubled, smeared edges on trees and antlers in the first place.",
         nullptr, 0, 0, 0, nullptr, 0, "DLSS only", &NoteAct, &NoteText},

        {RK::Action, "3. Single player only - never online",
         "Do not use this mod in multiplayer. It changes what the game renders and how its "
         "camera behaves, it patches memory while the game runs, and anti-cheat systems "
         "cannot tell that apart from anything else that does. Play offline, and treat "
         "your online account as something this mod should never touch.",
         nullptr, 0, 0, 0, nullptr, 0, "important", &NoteAct, &NoteText},

        {RK::Action, "4. Work in progress - expect rough edges",
         "This is an unfinished mod and it will show. Known today: the HUD controls on the "
         "PANEL tab can pull parts of the world about, DLSS models K and J shimmer while "
         "you turn your head unless you switch off 'cut each eye its own view', weapon "
         "depth cannot yet run alongside the ghosting fix, and leaning with 6DoF does not "
         "collide with anything, so you can lean through a wall. Every one of those has a "
         "note on the row that owns it. Nothing here can damage your save or your game "
         "files - the worst case is an odd picture and a restart.",
         nullptr, 0, 0, 0, nullptr, 0, "read me", &NoteAct, &NoteText},

        {RK::Group, "THE TWO KEYS WORTH SETTING NOW", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Key, "Recentre the view",
         "The one key you will want on a finger. It takes whatever direction you are "
         "facing right now as 'straight ahead' and puts the world back where your body is "
         "pointing - after you shift in your chair, after a loading screen, after anything "
         "that leaves the view feeling turned. It also re-anchors leaning, so press it once "
         "you have settled. Press ENTER on this row, then the key or pad button you want.",
         &c.recentre_key, 0, 0, 0, nullptr, 0, "Pause"},

        {RK::Key, "Show the game's menus on a flat screen",
         "The game draws its menus flat and full-frame, which in a headset puts the edges "
         "where your eyes cannot reach. This key shows them on a screen floating in front "
         "of you instead, at a size you can read. The mod usually notices menus on its own "
         "- this is the manual override for when it does not. Press ENTER, then your key.",
         &c.menu_screen_key, 0, 0, 0, nullptr, 0, "Delete"},

        {RK::Group, "WHEN YOU ARE READY", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Do not show this page again",
         "Turn this on and the panel will open on the STEREO tab from now on instead of "
         "here. This page never disappears - it stays as the first tab, so it is always "
         "there to come back to. To bring it back on opening, turn this off again or set "
         "welcome_seen = 0 in the ini.",
         &c.welcome_seen, 0, 0, 0, nullptr, 0, "off"},

        {RK::Action, "It already comes set up - and can be reset",
         "THE MOD SHIPS READY TO PLAY. Every setting is already at the values that were "
         "tested and tuned in a Meta Quest 3 over many sessions, so you do not have to "
         "configure anything before you start - put the headset on and hunt. "
         "The tabs are there for people who want to change things, and changing them is "
         "safe: nothing here can hurt your save or your game files. "
         "IF YOU CHANGE SOMETHING AND THE PICTURE GOES WRONG - and you will, that is what "
         "the tabs are for - go to the PROFILES tab and use 'Apply the recommended "
         "settings'. That puts every tab back to the tested configuration in one press, "
         "so you can experiment freely knowing there is always a way back. "
         "If you use a different headset, the tested values are still the right starting "
         "point; the ones most likely to want changing are the render resolution and the "
         "field of view, both on the VIEW tab.",
         nullptr, 0, 0, 0, nullptr, 0, "ready to play", &NoteAct, &NoteText},

        {RK::Heading, "STEREO", "", nullptr, 0, 0, 0, nullptr, 0, ""},


        {RK::Group, "DEPTH", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Stereoscopic 3D",
         "Renders each eye from a slightly different position, so the world has real depth instead "
         "of being a flat picture on your face. What it costs depends on 'Stereo mode' below.",
         &c.stereo, 0, 0, 0, nullptr, 0, "on"},

        {RK::Bool, "Rendering mode",
         "FULL RATE draws the whole scene twice per frame, so both eyes see the same "
         "moment - the depth is true, and it costs about half your framerate. AER draws one "
         "eye per frame and alternates, keeping the framerate but showing your eyes moments "
         "11ms apart, which corrupts depth when you move and makes close objects "
         "uncomfortable. "
         "You can switch either way while playing. Full rate needs the world on screen and "
         "about 20 seconds of play after loading before it will engage, so if you switch to "
         "it in a menu nothing happens until you are back in the game.",
         &c.full_rate_stereo, 0, 0, 0, kStereoModeNames, 2, "full rate"},

        {RK::Float, "Eye separation (IPD)",
         "How far apart your two eyes are. Larger exaggerates depth and makes the world feel "
         "smaller. 0 mm means both eyes see exactly the same thing - use it to check the rest "
         "of the picture is correct.",
         &c.ipd_mm, 0.0f, 100.0f, 4.0f, nullptr, 0, "64 mm"},

        {RK::Bool, "Swap left / right eye",
         "Turn on if depth looks inside-out - near things appearing far away and far things "
         "appearing close.",
         &c.eye_swap, 0, 0, 0, nullptr, 0, "on"},

        {RK::Group, "ADVANCED  -  DO NOT TOUCH UNLESS NECESSARY", "",
         nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Let the headset set the pace",
         "The VR frame is finished inside the game's own frame, so if the game waits for "
         "your MONITOR's refresh before finishing, the headset ends up running on the "
         "monitor's clock rather than its own. On a 60 Hz desktop next to a 90 Hz headset "
         "that is an uneven beat every third frame. "
         "Alternate-eye mode suffers most, because each eye is only redrawn every other "
         "frame: an uneven beat leaves one eye older than the other by a varying amount, "
         "and that irregularity is seen as flicker rather than as lag. "
         "ON, the mod stops the game waiting. The picture on your MONITOR may tear as a "
         "result - the headset never does, and the monitor image is only a mirror. Turn it "
         "off if you record or stream from that window and the tearing matters more than "
         "the smoothness in the headset.",
         &c.desktop_present_unlocked, 0, 0, 0, nullptr, 0, "on"},

        {RK::Bool, "Write a log file",
         "The mod keeps a diary at %LOCALAPPDATA%\\theHunterCotWVR\\cotwvr.log, with the "
         "previous run beside it as cotwvr.prev.log. It records what your game build is, "
         "which hooks took, every graphics setting it changed as 'old -> new', and a short "
         "summary of the picture every five seconds. IF YOU REPORT A PROBLEM, SEND THAT "
         "FILE - it usually contains the answer. "
         "The cost is about two lines a second, which is nothing next to a frame, so this "
         "is ON. Turning it off stops the running commentary; the opening lines about your "
         "hardware and your game version are written before this setting has even been "
         "read, so a report is still possible either way. The file stops itself at 64 MB "
         "and old per-process copies are cleaned up at every launch.",
         &c.logging, 0, 0, 0, nullptr, 0, "on"},

        {RK::Int, "Frames queued ahead",
         "How far ahead the graphics card may work. 1 is right for VR: the headset sets the "
         "pace, and a frame built three frames ago is submitted against a head position that "
         "has since moved. It arrives unevenly rather than late, and uneven is what the eye "
         "notices. Alternate-eye mode suffers most, because one eye then stays stale longer "
         "on some frames than others. 0 leaves the game's own setting alone. TAKES EFFECT "
         "NEXT TIME YOU START THE GAME.",
         &c.frame_latency, 0, 3, 1, nullptr, 0, "1"},

        {RK::Heading, "PICTURE", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        // ---------------------------------------------------------------------
        // ONE decision at the top - who smooths the picture - and everything
        // below it belongs to whichever answer was given. Rows that apply to the
        // other answer are hidden rather than greyed, so the tab is only ever as
        // long as the choice in force.
        // ---------------------------------------------------------------------
        {RK::Group, "ANTI-ALIASING", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Action, "Smoothing",
         "WHO REMOVES THE JAGGED EDGES AND THE GHOSTING. Push left or right to change it. "
         "OFF is the game untouched, which in this engine means each eye is smoothed using "
         "the OTHER eye's picture 6 cm away - the doubled, smeared edges on trees and "
         "antlers. "
         "PER-EYE SMOOTHING is the mod doing it properly: it takes the game's own smoothing "
         "step over and gives each eye a stored picture of its own. "
         "NVIDIA DLSS hands that same job to DLSS, which is superbly stable and also "
         "unlocks the render-size options below. DLSS is built ON TOP of the mod's pass - "
         "that step is where it is handed the depth and movement data it needs - so "
         "choosing it switches both on together and you cannot get the broken half-and-half "
         "combination. "
         "BOTH OF THE ON SETTINGS NEED THE GAME'S OWN ANTI-ALIASING SET TO FXAA+TAA in its "
         "Video settings - they work by taking that step over, so with the game's AA off "
         "there is nothing to take over and this row does nothing. Nothing else in the mod "
         "depends on it. "
         "CHANGING THIS ROW NEEDS NO RESTART - it takes effect on the next frame, either "
         "way. "
         "IF YOU SET IT TO OFF, CHANGE THE GAME'S ANTI-ALIASING TO FXAA ALONE at the same "
         "time. Left on FXAA+TAA with nothing taking it over, the game goes back to "
         "smoothing each eye with the OTHER eye's picture, and the doubled, smeared edges "
         "on trees and antlers come straight back - that is the fault this mod was built "
         "to fix. FXAA alone has no ghosting; it simply shimmers more on foliage. "
         "AND IF YOU WERE USING DLSS WITH A REDUCED RENDER SIZE (any Quality mode below "
         "DLAA), turning smoothing off leaves the game drawing that smaller picture with "
         "nothing to rebuild it, so everything goes soft. Set Quality mode back to DLAA "
         "and RESTART THE GAME to get your full resolution back - the game only reads its "
         "own resolution when it starts.",
         nullptr, 0, 0, 0, nullptr, 0, "per-eye smoothing", &AaModeAct, &AaModeText},

        {RK::Float, "  ...smoothing strength",
         "How hard the smoothing works. Higher is smoother and steadier, and eventually starts to soften things that move; lower is sharper and shimmers more on grass and distant branches. 1.00 is the tuned value. It moves two things together - how much of each new frame is kept, and how strictly stored colour that no longer matches is thrown away - because those two always have to be traded against each other.",
         &c.taa_ghosting_fix, 0.25f, 2.00f, 0.05f, nullptr, 0, "1.00",
         nullptr, nullptr, ModSmoothingChosen},

        {RK::Float, "  ...sharpen while smoothing",
         "Puts back crispness that frame-smoothing costs - smoothing is an average, and averages are soft. Kept inside the range of the pixels around it, so it cannot make bright halos around trunks and antlers. 0 is off, which is right if you are already using the game's own TAA sharpness slider - two sharpeners stacked look overdone. For sharpening with DLSS, use 'Sharpness' below instead: this one runs before the picture is rebuilt.",
         &c.taa_sharpen, 0.0f, 1.0f, 0.05f, nullptr, 0, "0",
         nullptr, nullptr, ModSmoothingChosen},

        {RK::Group, "FINAL IMAGE", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Float, "Sharpness",
         "Sharpens the finished picture, right before it reaches your headset - which is "
         "the only place worth doing it once DLSS is rebuilding the image, because "
         "anything sharpened earlier is sharpened into pixels that get replaced. NVIDIA "
         "removed DLSS's own sharpener for the same reason and expect the game to do it "
         "here instead. "
         "It cannot make halos: each pixel is only allowed to move as far as its own "
         "neighbours already reach, so trunks and antlers against a bright sky stay clean "
         "however high you push it. 0 is off; 0.3 to 0.5 is a normal amount; past that it "
         "starts to look etched.",
         &c.post_sharpen, 0.0f, 1.0f, 0.05f, nullptr, 0, "0.35"},

        {RK::Float, "Colour strength",
         "How strong the colours are in the finished picture. 1.00 is exactly what the "
         "game produced. Headset optics and the compositor's own colour handling can "
         "leave the world looking flatter than it does on the monitor, and this is the "
         "one stage that sees the final image, so it can put that back. Below 1 drains "
         "toward grey; 1.10 to 1.20 lifts foliage and sky without turning the animals "
         "into cartoons.",
         &c.post_saturation, 0.50f, 1.50f, 0.05f, nullptr, 0, "1.10"},

        {RK::Group, "DLSS  -  RENDER SIZE AND MODEL", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Int, "Quality mode",
         "NVIDIA's own ladder. The game is told to draw a fraction of the resolution you "
         "picked on the VIEW tab, and DLSS rebuilds each eye back up to full size - so you "
         "keep the picture and pay for a fraction of the pixels. DLAA draws everything and "
         "only smooths it. Quality 67%, Balanced 58%, Performance 50%, Ultra Performance "
         "33% - and because those are per axis, Performance means a QUARTER of the pixels. "
         "Custom uses the percentage on the next row. "
         "The two sizes on the right are live: what the game is drawing this second, and "
         "what your headset is being handed. If the second is bigger, DLSS is rebuilding. "
         "TO TRY LEVELS WITHOUT RESTARTING: the game only reads its own resolution when "
         "it starts, so moving this row takes effect at the next launch. But the game's "
         "OWN video menu changes resolution live - lower it there and the mod follows "
         "instantly, keeping the headset picture the same size and letting DLSS make up "
         "the difference. That is a real comparison, frame rate included.",
         &c.dlss_quality, 0, 5, 1, nullptr, 0, "Balanced", nullptr, DlssQualityText,
         DlssChosen},

        {RK::Int, "  ...custom render size (%)",
         "The percentage of each axis the game draws before DLSS rebuilds it - 100 is no "
         "upscaling, 33 is the lowest allowed. For landing between two of NVIDIA's steps, "
         "for example 77 when Quality is a touch soft and DLAA is too expensive. This row "
         "only appears while Quality mode is set to Custom, because that is the only "
         "setting that reads it. Takes effect at the next launch, like the mode itself.",
         &c.dlss_upscale_pct, 33, 100, 1, nullptr, 0, "100 (off)",
         nullptr, nullptr, DlssCustomChosen},

        {RK::Int, "  ...DLSS model",
         "Which of NVIDIA's smoothing models DLSS runs. Changing it takes effect "
         "immediately, so you can try them on the same view. "
         "M and L are the ones to use here: they are NVIDIA's own choices for "
         "reconstruction work, and both are clean in this game. "
         "K AND J LOOK SHIMMERY AND BLURRY WHEN YOU MOVE YOUR HEAD, and that is a known "
         "limitation of this mod rather than a fault in the models - they lean harder on "
         "their memory of previous frames, and this game hands them a per-eye picture they "
         "do not entirely agree with. IF YOU WANT TO USE THEM: go to the VIEW tab and turn "
         "OFF 'cut each eye its own view'. That fixes it completely. You will lose some of "
         "the extra height that setting buys, so afterwards use 'Game field of view' and "
         "'how much wider' on the same tab to fit the picture back to your headset.",
         &c.dlss_preset, 0, 4, 1, kDlssPresetNames, 5, "K", nullptr, nullptr,
         DlssChosen},

        {RK::Group, "ADVANCED  -  DO NOT TOUCH UNLESS NECESSARY", "",
         nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "  ...let DLSS do all the smoothing",
         "With upscaling on there are still TWO things averaging the picture over time: "
         "the mod's own smoothing, and then DLSS on top of its result. Averaging an "
         "average is how a small misalignment in the first one gets treated as real "
         "detail by the second - and the DLSS models differ in how much they trust their "
         "memory, which is why some flavours shimmer when you look around and others do "
         "not. This hands the whole job to DLSS: the mod's pass still works out where "
         "everything moved and still hands over the movement data, but it stops averaging "
         "and passes the current frame straight through. "
         "IF K OR J SHIMMER WHEN YOU MOVE YOUR HEAD, TRY THIS. It is the arrangement the "
         "reference mod for this engine uses.",
         &c.taa_single_accumulation, 0, 0, 0, nullptr, 0, "off", nullptr, nullptr, DlssChosen},

        {RK::Bool, "  ...run DLSS once, not twice",
         "With upscaling on, DLSS could run in two places at once - smoothing the frame "
         "at render size and then rebuilding it - and doing both means every eye is "
         "averaged over time TWICE per frame. That costs double and looks worse than "
         "either alone, because averaging an already-averaged picture is how fine detail "
         "gets wiped off. On is correct; off exists only to see the difference.",
         &c.dlss_upscale_solo, 0, 0, 0, nullptr, 0, "on", nullptr, nullptr, DlssChosen},

        {RK::Int, "  ...DLSS: camera micro-shake",
         "The game deliberately shakes its camera by a quarter of a pixel every frame so "
         "edges get smoothed over time - and DLSS must be told the exact shake each frame "
         "to line its memory up. 'Measured' reads it live from the game's own camera and "
         "is the correct setting; the log shows the value it found. 'Not reported' is the "
         "old behaviour that caused much of the smearing, kept so the difference can be "
         "proven. 'Manual' uses the two numbers in the ini file.",
         &c.dlss_jitter_mode, 0, 2, 1, kDlssJitterModeNames, 3, "measured", nullptr, nullptr, DlssChosen},

        {RK::Bool, "  ...DLSS: exact camera shake",
         "WHERE the shake number comes from. ON reads it straight out of the camera the "
         "frame was actually drawn with - mathematically exact, one value per eye per "
         "frame. OFF uses an estimate built from how the camera moved over the last few "
         "frames, and that estimate assumes the game shakes between TWO positions when it "
         "really uses SIXTEEN, so what it produces is an average of noise. "
         "If the picture shimmers when you MOVE YOUR HEAD but is clean standing still, "
         "this is the first thing to turn on: a wrong shake value only misaligns the "
         "picture while something is moving. Kept off by default only because it arrived "
         "in the same session that DLSS lost its clean look and has never been tested "
         "alone.",
         &c.dlss_jitter_exact, 0, 0, 0, nullptr, 0, "off", nullptr, nullptr, DlssChosen},

        {RK::Float, "  ...DLSS: shake direction",
         "Multiplies the measured micro-shake before DLSS is told about it. 1.00 is the "
         "derived value. If DLSS looks WORSE with the shake reported than without, the "
         "sign convention is backwards on this engine - set -1.00 and compare again. "
         "Settle it once, then leave it.",
         &c.dlss_jitter_scale, -2.0f, 2.0f, 0.25f, nullptr, 0, "1.00", nullptr, nullptr, DlssChosen},

        {RK::Bool, "  ...DLSS: auto exposure",
         "Lets DLSS work out the scene's brightness for itself. The game never hands over "
         "its exposure, and DLSS judges what to keep from frame to frame by brightness - "
         "without this it assumed a fixed value and smeared worst exactly where the "
         "picture is high-contrast: bright sky through dark branches, and snow.",
         &c.dlss_auto_exposure, 0, 0, 0, nullptr, 0, "on", nullptr, nullptr, DlssChosen},

        {RK::Bool, "  ...DLSS: reversed depth",
         "Tells DLSS this engine stores depth back-to-front (near = 1), which Apex games "
         "do. With it wrong, DLSS misjudges which surface is in front along moving edges. "
         "Turn it off only to prove what it is worth.",
         &c.dlss_depth_inverted, 0, 0, 0, nullptr, 0, "on", nullptr, nullptr, DlssChosen},

        {RK::Bool, "  ...DLSS: vectors carry the shake",
         "The movement data the mod hands DLSS is computed from the game's shaken camera, "
         "so the micro-shake is baked into it. This tells DLSS that, so it can subtract "
         "the shake itself. Leave on unless testing.",
         &c.dlss_mv_jittered, 0, 0, 0, nullptr, 0, "on", nullptr, nullptr, DlssChosen},

        {RK::Group, "EXPERIMENTAL  -  LEAVE OFF UNLESS ASKED", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "  ...16-position shake (the game's own)",
         "Switches the game onto its OWN hidden 16-position camera-shake table - a mode "
         "built into the engine that its settings menu never offers. Because it is the "
         "game's own mode, every part of the renderer applies and compensates it "
         "consistently, and the mod reads the exact value each frame straight from the "
         "camera to tell DLSS. This is the proper fix for the edge/texture shake, on "
         "every DLSS flavour. If anything looks worse with it on, turn it off.",
         &c.jitter_mode3, 0, 0, 0, nullptr, 0, "off"},

        {RK::Bool, "  ...better camera shake (8-position)",
         "The game only shakes its camera between TWO positions; DLSS is designed for "
         "eight or more, and the shortfall is what makes edges and fine detail vibrate "
         "slightly. This adds the mod's own shake pattern on top, giving DLSS the eight "
         "positions it wants - and because the mod generates it, the amount reported to "
         "DLSS becomes exact instead of estimated, which also removes the last texture "
         "wobble. Shadows are deliberately left alone. If shadows or lighting ever "
         "start flickering with this on, turn it off.",
         &c.taa_jitter_inject, 0, 0, 0, nullptr, 0, "off"},

        {RK::Float, "  ...shake amount",
         "How far the 8-position shake sweeps. Bigger holds edges (rocks, buildings) "
         "steadier but makes DISTANT TREES flicker - their fine detail pops in and out "
         "as the sample point sweeps a wider area than the game's art was tuned for. "
         "Smaller is the reverse. Start at 0.50 and nudge until both look right.",
         &c.taa_jitter_inject_scale, 0.25f, 1.0f, 0.25f, nullptr, 0, "0.50"},

        {RK::Bool, "  ...head-turn correction (retry)",
         "The game's camera data never carries YOUR head's rotation - only the stick's - so "
         "the smoothing lands short by however much your head turned that frame, and things "
         "shift or shimmer with head movement but not with the stick. This adds that missing "
         "amount back. It was tried before and looked useless - but that test ran while DLSS "
         "was also being fed wrong camera-shake and no exposure, which drowned it, and both "
         "directions shared one sign when they need their own. Turn it on, then settle the "
         "two rows below one at a time.",
         &c.taa_head_rotation_fix, 0, 0, 0, nullptr, 0, "off"},

        {RK::Float, "  ...left-right amount",
         "Turn your head LEFT and RIGHT (do not nod) looking at a tree. 1.0 is the computed "
         "amount; if the tree still drags with your head, the sign is wrong - try -1.0. "
         "Settle this before touching up-down.",
         &c.taa_head_rotation_scale, -2.0f, 2.0f, 0.25f, nullptr, 0, "1.00"},

        {RK::Float, "  ...up-down amount",
         "Now NOD slowly at the same tree. Same idea: 1.0, and if it drags, -1.0. This axis "
         "has its own sign because the game stores the two directions differently.",
         &c.taa_head_rotation_scale_y, -2.0f, 2.0f, 0.25f, nullptr, 0, "1.00"},

        {RK::Group, "DIAGNOSTICS", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Movement-data trace (diagnostic)",
         "Reads back the movement value the mod hands DLSS at the centre of the screen, "
         "once a second, and logs it beside how far the camera turned and how far your "
         "HEAD turned in the same frame. That is the one measurement that can say whether "
         "the movement data keeps up with your head - which is the difference between a "
         "picture that shimmers when you look around and one that does not. Costs a tiny "
         "readback while on. It is a fault-finding tool, not a setting.",
         &c.taa_mv_probe, 0, 0, 0, nullptr, 0, "off"},

        {RK::Bool, "Resolve trace (diagnostic)",
         "Writes a description of the game's smoothing step to the log: how many times it runs each frame, which eye each run belongs to, and which stored picture each one reads. It arms itself once the world is on screen, runs fifteen seconds and stops. It only reads - nothing on screen changes. Costs nothing while off. It is a fault-finding tool, not a setting.",
         &c.taa_probe, 0, 0, 0, nullptr, 0, "off"},

        {RK::Heading, "VIEW", "", nullptr, 0, 0, 0, nullptr, 0, ""},


        {RK::Group, "RESOLUTION", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Int, "Render resolution",
         "Sets the game's own resolution. Use the FULLVIEW sizes: your eye is nearly SQUARE, so a "
         "widescreen render throws away a third of your graphics card off the sides, where you "
         "cannot see it. Bigger is sharper and slower. TAKES EFFECT NEXT TIME YOU START THE GAME. "
         "With DLSS upscaling on (PICTURE tab) this is the size you END UP with - the game is "
         "told to draw a fraction of it and DLSS rebuilds the rest.",
         &c.render_preset, 0.0f, float(PresetNameCount() - 1), 1.0f,
         PresetNames(), PresetNameCount(), "leave alone", nullptr, RenderPresetText},

        {RK::Group, "FIELD OF VIEW", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Float, "Game field of view",
         "How near the world sits. Raise it and everything comes closer; lower it and the world "
         "backs away. Too far from the worked-out value and close-up depth gets exaggerated, so "
         "near things become uncomfortable while distant scenery still looks fine. This and "
         "'how much wider' are the same lever. "
         "IT IS WORKED OUT FOR YOU at every launch, from the render shape and your headset's own "
         "field - until you move it. The moment you do, it becomes YOUR number: the automatic "
         "follower switches itself off and your value survives restarts. The row below shows "
         "which of the two is in charge, and can hand it back.",
         &c.game_fov_deg, 0.0f, 110.0f, 1.0f, nullptr, 0, "100"},

        {RK::Bool, "  ...work it out for me each launch",
         "ON, the mod computes the field of view at every launch and overwrites the row above - "
         "correct for most people, because the right value depends on the render shape and on "
         "your headset, and both are known at startup. "
         "It turns itself OFF the moment you move that row by hand, so a value you set stays set. "
         "Turn it back on here to hand the job back to the mod - your own number will be replaced "
         "the next time the game starts.",
         &c.game_fov_follows_auto, 0, 0, 0, nullptr, 0, "on"},

        {RK::Bool, "FOV value is horizontal",
         "Whether the number above is the game's HORIZONTAL field of view rather than its "
         "vertical one. Most shooters quote horizontal. Getting it backwards makes the "
         "picture about a quarter too wide, which exaggerates close-up depth and is "
         "uncomfortable - while distant scenery still looks perfectly fine.",
         &c.fov_is_horizontal, 0, 0, 0, nullptr, 0, "off"},

        {RK::Group, "EXTENDED FIELD OF VIEW  (EXPERIMENTAL)", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Extend the rendered view",
         "Makes the game draw a wider view than it normally would, so the headset can be given its "
         "full field of view - yours can show about 99 degrees up and down and the game draws less. "
         "Best used with 'Give each eye its own view'. If scenery appears and disappears near the "
         "edges of the picture, lower 'How much wider'.",
         &c.tier1_fov, 0, 1, 1, nullptr, 0, "on"},

        {RK::Float, "  ...how much wider",
         "1.00 is the game untouched. Higher shows more of the world at once, and "
         "everything looks slightly smaller as a result. Change it while standing still "
         "and watch something near the edge of the screen: if the view really is widening, "
         "that object slides toward the middle and more of the world appears beyond it.",
         &c.tier1_fov_k_override, 0.60f, 2.00f, 0.05f, nullptr, 0, "1.20"},

        {RK::Bool, "  ...cut each eye its own view",
         "Shows each eye its own part of the wider picture, angled the way that eye "
         "actually looks - which is what finally gives you the full height your headset "
         "can display instead of the narrower slice the game draws. Needs 'Extend the "
         "rendered view' on to have anything extra to work with. If the picture ever "
         "refuses to merge into one, turn this off first.",
         &c.tier1_per_eye_crop, 0, 1, 1, nullptr, 0, "off"},

        {RK::Heading, "HEAD TRACKING", "", nullptr, 0, 0, 0, nullptr, 0, ""},


        {RK::Bool, "Head tracking",
         "Turn your head and the game world turns with it. See RECENTRING below to set "
         "your current head direction as 'straight ahead' - and to choose the key or pad "
         "buttons that do it. If the view spins, jitters or fights you, turn this off.",
         &c.head_tracking, 0, 0, 0, nullptr, 0, "on"},

        {RK::Group, "TRACKING MODE", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Head tracking mode",
         "3DoF or 6DoF - the two ways a headset can follow you. "
         "3DoF (three degrees of freedom) follows the DIRECTION your head is pointing: "
         "look left, up, tilt your head, and the view turns with you. That is all most "
         "flat-game VR mods can do. "
         "6DoF adds the other three - WHERE your head is. Move around a tree trunk, duck "
         "under a branch, shift sideways to see past your own scope, and the world answers "
         "back with parallax, so close things finally sit at a real distance instead of "
         "feeling painted on. It follows where your head IS, so a step, a duck or a lean "
         "all count; tilting your head onto your shoulder does not, that being a turn "
         "rather than a move. "
         "IN 6DoF NOTHING COLLIDES. The game has no idea the camera moved, so lean far "
         "enough and you will lean through a wall and see the inside of the world. That "
         "cannot be fixed from outside the engine, which is why the travel limit below "
         "exists and why it is set to a lean rather than a walk. "
         "Recentring sets where 'not leaning' is, so press it once you have settled into "
         "your chair - the mod also does this for you when the world first appears.",
         &c.six_dof, 0, 0, 0, kTrackingModeNames, 2, "3DoF"},

        {RK::Float, "  ...movement scale",
         "How far the camera moves for how far your head moves. 1.00 is life size - a "
         "10 cm lean is 10 cm in the world, which is what makes the depth feel right. "
         "Below 1 is gentler and keeps you further from walls; above 1 exaggerates and "
         "starts to feel like the world is on a spring.",
         &c.six_dof_scale, 0.0f, 2.0f, 0.05f, nullptr, 0, "1.00", nullptr, nullptr, SixDofOn},

        {RK::Float, "  ...travel limit",
         "The furthest the camera may get from where you recentred, in metres, in any "
         "direction. This is what stops a room-scale walk - or a tracking glitch - from "
         "putting your view inside a rock. 0.60 m is a comfortable lean from a seated "
         "position. Raise it only if you play standing and know your room is clear.",
         &c.six_dof_limit_m, 0.10f, 2.00f, 0.05f, nullptr, 0, "0.60 m", nullptr, nullptr, SixDofOn},

        {RK::Group, "RECENTRING", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Key, "Recentre key",
         "Takes whatever direction you are facing right now as 'straight ahead'. The "
         "picture does not move when you press it - if you are looking down at the "
         "rifle, you stay looking down at the rifle, and that becomes your new "
         "neutral. Press Enter or A to change it, then the key or pad button you want.",
         &c.recentre_key, 0, 255, 1, nullptr, 0, "19 (Pause)"},

        {RK::Bool, "  ...or click both sticks",
         "Click the left and right sticks together and hold them for a moment. Both "
         "buttons stay hidden from the game until you have let go of both, so you will "
         "not sprint or hold your breath by accident on the way in or on the way out. "
         "Nothing happens until you have held them long enough, and it fires once "
         "however long you hold. Needs head tracking on, and the row below it on.",
         &c.recentre_pad_enable, 0, 0, 0, nullptr, 0, "on"},

        {RK::Int, "  ...how long to hold",
         "Milliseconds both sticks must be held before it recentres. Longer is harder "
         "to trigger by accident; shorter feels more immediate.",
         &c.recentre_pad_hold_ms, 200, 2000, 100, nullptr, 0, "600 ms"},

        {RK::Int, "  ...spread over how many frames",
         "The picture never moves either way. What this changes is how fast the GAME's "
         "own camera swings round underneath while the mod holds the picture still. In "
         "one frame that is a jump the engine's effects cannot cope with, and you get a "
         "brief smear whose look depends on what you are pointed at. Spread over more "
         "frames the swing is slower and the smear smaller. Raise it until the press is "
         "clean. 1 puts the whole thing in a single frame.",
         &c.recentre_ramp_frames, 1, 90, 2, nullptr, 0, "31 frames"},

        {RK::Group, "STABILITY", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Aim stabilisation",
         "A scope magnifies whatever your neck does - at 8x, a tremor you cannot feel is "
         "metres downrange, so the aim wanders on its own. This settles the view while you "
         "are looking through a scope or binoculars, and leaves it raw the rest of the time. "
         "Deliberate movement still gets there; only the shake is taken out.",
         &c.aim_steady, 0, 0, 0, nullptr, 0, "on"},

        {RK::Float, "  ...strength",
         "Milliseconds for the view to catch up with your head. 0 is off. 60 removes about "
         "three quarters of a normal hand tremor and delays a deliberate 5 degree swing by "
         "about 0.14 s, which is hard to notice. 120 is steadier and starts to feel like the "
         "view is following you rather than belonging to you. Past 250 it lags then catches "
         "up, which is worse than the shake.",
         &c.aim_steady_ms, 0.0f, 250.0f, 10.0f, nullptr, 0, "120 ms"},

        {RK::Bool, "  ...only while scoped",
         "On, this settles the view only when you are looking through an optic. Turn it OFF "
         "to have it on all the time - steadier everywhere, but walking about starts to feel "
         "like the view is on a spring.",
         &c.aim_steady_scoped_only, 0, 0, 0, nullptr, 0, "on"},

        {RK::Group, "PITCH  -  WHAT LOOKS UP AND DOWN", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Look up and down with your head only",
         "Stops the stick and the mouse from tilting the view up and down. Turning left and "
         "right still works normally - you cannot turn a chair all the way round - but your "
         "neck becomes the only thing that looks up or down. Two controls fighting over the "
         "same axis is what makes the horizon sit somewhere your head says it is not, which "
         "is the quickest way to feel unwell. The settings panel still scrolls normally.",
         &c.lock_pitch_input, 0, 0, 0, nullptr, 0, "off"},

        {RK::Bool, "  ...on the gamepad stick",
         "Which of the two the lock above applies to. Leave both on unless you want the "
         "mouse locked and the stick free, or the other way round.",
         &c.lock_pitch_pad, 0, 0, 0, nullptr, 0, "on"},

        {RK::Bool, "  ...on the mouse",
         "The mouse half of the lock above.",
         &c.lock_pitch_mouse, 0, 0, 0, nullptr, 0, "on"},

        {RK::Group, "AXIS DIRECTION", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Invert looking left / right",
         "Turn on if the world turns the WRONG WAY when you turn your head - you look left "
         "and it goes right.",
         &c.head_invert, 0, 0, 0, nullptr, 0, "off"},

        {RK::Bool, "Invert looking up / down",
         "Turn on if looking UP sends the view DOWN. This is deliberately separate from the "
         "left/right setting: up-down and left-right are stored in two different places in "
         "the game with opposite signs, so one shared switch would fix one and break the "
         "other.",
         &c.head_pitch_invert, 0, 0, 0, nullptr, 0, "on"},

        {RK::Group, "FREE LOOK", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Weapon follows your head",
         "ON (coupled): turning your head turns your aim, so the gun goes where you look - "
         "the head IS the aim. OFF (decoupled): your head only moves the view and the gun "
         "keeps pointing where the stick aimed it. Coupled suits a scoped rifle; decoupled "
         "lets you glance around without disturbing your shot. The view sits in exactly the "
         "same place either way, so switching never jolts the picture - only the gun "
         "changes behaviour.",
         &c.head_early_write, 0, 0, 0, nullptr, 0, "on"},

        {RK::Key, "Free-look key (hold)",
         "Hold this and the coupling breaks for as long as you hold it: you can look "
         "wherever you like and the weapon stays aimed where it was. Let go and the gun "
         "comes back to your view. This is a Windows key code - 164 is Left Alt. Set it to "
         "0 to switch the key off. Because both modes put the view in the same place, "
         "pressing it never moves the picture.",
         &c.freelook_key, 0, 255, 1, nullptr, 0, "18 (Alt)"},

        {RK::Group, "HEAD ROLL", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Tilt the world with my head (roll)",
         "Tip your head sideways, ear towards your shoulder, and the horizon tips with it. Your "
         "headset has always measured this - it was simply never sent to the game, which is why "
         "the world stays stubbornly level however you lean your head.",
         &c.head_write_roll, 0, 0, 0, nullptr, 0, "on"},

        {RK::Bool, "Invert roll",
         "Turn on if tipping your head LEFT tips the world RIGHT. Which way the field counts is "
         "not known until it is tried.",
         &c.head_roll_invert, 0, 0, 0, nullptr, 0, "on"},

        {RK::Float, "Roll amount",
         "1.0 tilts the world exactly as far as your real head. Lower tilts it less, which some "
         "people find steadier to look at; 0 is the same as turning roll off.",
         &c.head_roll_scale, 0.0f, 1.5f, 0.1f, nullptr, 0, "1.0"},

        {RK::Group, "ADVANCED  -  6DoF DIRECTION AND AXES", "",
         nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Action, "These are already set correctly",
         "Everything under this header decides WHICH WAY your movement is mapped into the game - and it was settled by measurement in the headset, so on this setup it is already right. You would only come here if leaning went somewhere it should not: stepping left moving you forward, or turning your body swinging you around a point off to one side. Each row says which symptom it fixes. If you have changed them and lost track, PROFILES - Apply the recommended settings puts them all back. Nudging this row does nothing; it is a notice.",
         nullptr, 0, 0, 0, nullptr, 0, "settled", &NoteAct, &NoteText},

        {RK::Int, "  ...what leaning is measured against",
         "TRY THIS FIRST IF TURNING YOUR BODY BREAKS LEANING. It decides which direction "
         "the mod treats as 'the way you were facing' when it converts your real movement "
         "into game movement, and the right answer depends on engine behaviour that "
         "cannot be read from outside - so it is a setting rather than a guess. "
         "0 = the way you faced when you last recentred: leaning is correct until you "
         "physically turn, then it goes wrong by however far you turned. "
         "1 = the way your head is facing right now - use this if 0 has that fault. "
         "2 = only how far your head has turned since recentring. 3 = your room's own "
         "directions, ignoring both. "
         "Test each by stepping LEFT after turning your body 90 degrees, four times "
         "round. The right one is left every time.",
         &c.six_dof_frame_mode, 0, 3, 1, nullptr, 0, "1", nullptr, nullptr, SixDofOn},

        {RK::Int, "  ...which way is forward",
         "USE THIS FIRST IF LEANING GOES THE WRONG WAY. Your headset measures where your "
         "head is against your ROOM, not against your chair - so if the play space was set "
         "up facing a different way from the way you sit, every lean lands a quarter or a "
         "half turn round from where it should. Leaning LEFT moving you FORWARD is the "
         "classic quarter-turn version, and no amount of swapping left for right can fix "
         "that, because swapping is a half turn. "
         "TO TEST: face straight ahead and move your whole head SIDEWAYS TO YOUR LEFT - a "
         "small step, or slide your body left in the chair. Do not turn, do not tilt your "
         "head onto your shoulder, and do not lean forward at the same time. Then watch "
         "which way the world went: if you went FORWARD set 90, BACKWARDS set 270, RIGHT "
         "set 180, LEFT it is already correct. One of the four is right and it stays right "
         "until you redraw your play boundary.",
         &c.six_dof_yaw_offset_deg, -180, 270, 90, nullptr, 0, "0 deg", nullptr, nullptr, SixDofOn},

        {RK::Bool, "  ...turn on the spot",
         "Decides what your lean is measured against. ON, it is measured against "
         "YOUR BODY: turn your head and the view turns without moving you, turn with the "
         "stick and your lean comes round with you - so you pivot on yourself, the way "
         "you do in the real world. OFF was the first build's behaviour, where turning "
         "swung you around a point off to one side instead of turning on the spot. "
         "Leave it on; it is switchable only so the difference can be felt directly.",
         &c.six_dof_body_frame, 0, 0, 0, nullptr, 0, "on", nullptr, nullptr, SixDofOn},

        {RK::Bool, "  ...turn the other way",
         "Only if 'turn on the spot' makes the swinging WORSE rather than better. That "
         "means the head turn is being taken out the wrong way round, and this flips it.",
         &c.six_dof_body_frame_invert, 0, 0, 0, nullptr, 0, "off", nullptr, nullptr, SixDofOn},

        {RK::Bool, "  ...swap left / right",
         "Only if leaning LEFT moves the view RIGHT. Which way round the game stores its "
         "sideways axis was never settled by measurement, so it is a switch rather than a "
         "guess - the eye separation had exactly the same switch until one headset session "
         "pinned it.",
         &c.six_dof_invert_x, 0, 0, 0, nullptr, 0, "off", nullptr, nullptr, SixDofOn},

        {RK::Bool, "  ...swap up / down",
         "Only if ducking raises the view instead of lowering it.",
         &c.six_dof_invert_y, 0, 0, 0, nullptr, 0, "off", nullptr, nullptr, SixDofOn},

        {RK::Bool, "  ...swap forward / back",
         "Only if leaning IN pushes the view backwards. Test it on something close - a "
         "rifle in your hands or a branch - because at distance a forward lean barely "
         "shows either way.",
         &c.six_dof_invert_z, 0, 0, 0, nullptr, 0, "off", nullptr, nullptr, SixDofOn},

        {RK::Bool, "  ...write the numbers to the log",
         "Prints once a second what your head did and what the game did with it - your "
         "movement in the room on the left, the movement applied in the game on the right. "
         "If leaning still goes somewhere odd, turn this on, lean deliberately in each "
         "direction for a few seconds, and send me the log; the numbers say which axis "
         "landed where without either of us having to describe a direction in words.",
         &c.six_dof_log, 0, 0, 0, nullptr, 0, "off", nullptr, nullptr, SixDofOn},

        {RK::Group, "ADVANCED  -  DO NOT TOUCH UNLESS NECESSARY", "",
         nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Recentring keeps the picture still",
         "ON: recentring hands your current offset to the game, so the view does not "
         "move at all - only what counts as 'neutral' changes. OFF restores the old "
         "behaviour exactly, where the view snaps round to wherever the game was "
         "aiming - and because an accidental stick-click is only harmless while the "
         "press moves nothing, turning this off turns the stick shortcut off with it. "
         "Leave it on unless you are comparing the two.",
         &c.recentre_compensate, 0, 0, 0, nullptr, 0, "on"},

        {RK::Bool, "  ...recentring goes the wrong way",
         "Turn on if pressing recentre throws the view TWICE as far instead of leaving "
         "it still. Which way the game counts this is not known until it is tried.",
         &c.recentre_compensate_invert, 0, 0, 0, nullptr, 0, "off"},

        {RK::Bool, "Submit the rendered viewpoint",
         "Hands the headset the viewpoint each frame was really drawn from, instead of one "
         "measured a moment later. The runtime lines the picture up against that viewpoint, so "
         "getting it wrong smears the image slightly - none of it while your head is still, more "
         "the faster you turn, which is the small judder you feel when moving. Turn it off and "
         "on while looking around to compare.",
         &c.submit_rendered_pose, 0, 0, 0, nullptr, 0, "on"},

        {RK::Bool, "Lock one head position per frame",
         "The mod tells the game where your head is at several points while it draws each "
         "frame, and until now it asked the headset again each time - so if your head moved "
         "a hair in between, the game got two slightly different answers for the same frame. "
         "That lands as a tiny camera wobble: a few pixels on things close to you, nothing "
         "far away, which is exactly the shake on walls, tables and tree trunks. This locks "
         "one answer per frame. Turn it off if the view ever feels a frame behind.",
         &c.head_latch_per_frame, 0, 0, 0, nullptr, 0, "off"},

        {RK::Int, "Heartbeat tremor filter",
         "LEAVE THIS OFF. It holds the view still through movements smaller than a "
         "twentieth of a degree, and testing showed that OFF is the best setting and "
         "every level above it is worse - which turns out to be the right answer, not a "
         "tuning failure. In VR the world looks still when the view follows your head "
         "EXACTLY, because your inner ear is expecting precisely that motion; hold the "
         "view still through a movement your head really made and you have created a "
         "mismatch, which is what reads as the world shaking. Kept only because it is "
         "the control that proved it. If your pulse is visible on the weapon in your "
         "hands but not on the world, that is a different problem and it is the weapon "
         "that needs smoothing, not the view.",
         &c.head_tremor_level, 0, 3, 1, kTremorLevelNames, 4, "off"},

        {RK::Int, "Prediction dampening",
         "The headset does not report where your head IS - it predicts where it will be by "
         "the time the frame is shown, and prediction exaggerates every small quick "
         "movement, including your pulse. This asks for a prediction closer to now. It is "
         "the same idea as OpenXR Toolkit's 'over-prediction reduction'. Raise it until "
         "the beating settles; too high and the world will start to feel like it trails "
         "your head slightly.",
         &c.prediction_damp_pct, 0, 100, 10, nullptr, 0, "0 %"},

        {RK::Heading, "GAME MENUS", "", nullptr, 0, 0, 0, nullptr, 0, ""},


        {RK::Bool, "Show the game on a flat screen",
         "The game draws its menus flat, at screen scale. Stretched over the headset's whole "
         "field of view their edges - and so most of the options - land outside where your eye "
         "can comfortably look. This puts the game's image on a floating panel in front of you "
         "at a size you can read. Also handy for the map or anything else easier read flat.",
         &c.menu_screen, 0, 0, 0, nullptr, 0, "off"},

        {RK::Key, "Flat screen hotkey",
         "Toggles the panel without opening this one. A Windows key code - 0x71 (113) is F2. "
         "Set 0 to switch the hotkey off. Pick something the game does not already use.",
         &c.menu_screen_key, 0, 255, 1, nullptr, 0, "46 (Delete)"},

        {RK::Bool, "Find menus automatically",
         "Puts the panel up by itself. Two signals, because one cannot cover both cases: the "
         "MAIN menu has no player camera, and the IN-GAME menu is found by the mouse cursor "
         "appearing - it has to leave the world running, so nothing about the camera gives it "
         "away. If the panel ever shows up when you did not want it, the hotkey dismisses it.",
         &c.menu_screen_auto, 0, 0, 0, nullptr, 0, "on"},

        {RK::Group, "THE MENU SCREEN", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Float, "Screen width (metres)",
         "How wide the panel is in the world. With the distance below this decides how big it "
         "looks. 2.4 m at 2.2 m away is about a very large monitor at desk distance.",
         &c.menu_screen_width_m, 0.5f, 8.0f, 0.2f, nullptr, 0, "2.4"},

        {RK::Float, "Screen distance (metres)",
         "How far in front of you the panel sits. Closer looks bigger but is harder on the "
         "eyes; further is more comfortable to read for a long time.",
         &c.menu_screen_distance_m, 0.5f, 8.0f, 0.2f, nullptr, 0, "1.80"},

        {RK::Bool, "Hide the world behind it",
         "Applies to the MAIN menu only, where the world behind is the very same stretched menu "
         "the panel replaces. In an in-game menu you are stood in the world and the panel simply "
         "floats over the scene - blanking it there is what made the first attempt such a mess.",
         &c.menu_screen_hide_world, 0, 0, 0, nullptr, 0, "on"},

        {RK::Heading, "PANEL", "", nullptr, 0, 0, 0, nullptr, 0, ""},


        {RK::Group, "PLACEMENT", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Float, "Panel distance",
         "How far in front of you this panel floats. Move it further away if it is hard to "
         "focus on, closer if it is hard to read.",
         &c.panel_distance, 0.6f, 3.0f, 0.1f, nullptr, 0, "1.3 m"},

        {RK::Float, "Panel size",
         "How wide this panel is, in metres. It scales with distance, so adjust this after "
         "you have set the distance.",
         &c.panel_size, 0.5f, 2.0f, 0.1f, nullptr, 0, "1.0 m"},

        {RK::Bool, "Also show this panel on the monitor",
         "Draws this same panel into the game window, so you can read it and change things without wearing the headset. It appears flat and without its soft edges - one straight copy rather than a second lot of drawing - and the headset is unaffected, because the copy happens after the headset has already been given the frame.",
         &c.panel_on_monitor, 0, 0, 0, nullptr, 0, "on"},

        {RK::Group, "SOUND", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Sound",
         "Plays a short tone when you move the selection or change a value, so you get "
         "feedback without having to read the screen.",
         &c.sound_enabled, 0, 0, 0, nullptr, 0, "on"},

        {RK::Percent, "Sound volume",
         "How loud those tones are, relative to everything else you can hear.",
         &c.sound_volume, 0.0f, 1.0f, 0.1f, nullptr, 0, "50%"},

        {RK::Group, "GAME HUD  -  UNFINISHED, EXPECT PROBLEMS", "",
         nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Action, "About these HUD controls",
         "READ THIS BEFORE TOUCHING THE ROWS BELOW. They move, shrink and hide parts of "
         "the game's own HUD, which the game draws flat across the whole frame. The mod "
         "reaches them by intercepting the draws, and that work is NOT FINISHED: moving or "
         "scaling the HUD can pull parts of the WORLD with it, leave pieces behind, or "
         "blank things that were not meant to go. If the picture breaks, set every row in "
         "this group back to its default and it recovers - nothing here is saved into the "
         "game. They are left in because the compass and the ammo counter sit right on the "
         "edge of a headset's view where they are hardest to read, and a partial fix is "
         "better than none for anyone willing to put up with the rest. Nudge this row does "
         "nothing at all - it is a notice, not a setting.",
         nullptr, 0, 0, 0, nullptr, 0, "", &NoteAct, &NoteText},

        {RK::Int, "Move the HUD left / right",
         "Slides the game's own HUD sideways, in pixels. Negative moves it left. The edge of a headset's view is the worst place for small text and this game puts the compass, health and phone right on it. Your VIEW is not touched - only the HUD is drawn through the moved viewport.",
         &c.hud_move_x, -1200, 1200, 20, nullptr, 0, "0"},

        {RK::Int, "Move the HUD up / down",
         "The same, vertically. Negative moves it up. With the whole HUD in one corner, these two together pull the lot inward without changing its size.",
         &c.hud_move_y, -1200, 1200, 20, nullptr, 0, "0"},

        {RK::Bool, "Remove the sun's coloured flare",
         "Takes the coloured fringing off the sun and leaves the rest of the glow alone. It is one rung of the bloom pyramid - the first step back up - so bright sky, water and lit surfaces keep their bloom. Matched by what the pass DOES rather than by which texture it is, so it keeps working at any resolution.",
         &c.hud_hide_sunflare, 0, 0, 0, nullptr, 0, "off"},

        {RK::Bool, "Remove the screen glare",
         "Takes out the haze the game lays over everything - it reads as a dirty lens or sun scatter, and in a headset it sits on your eyes permanently. It is the bloom pass: three floating-point textures composited back over the scene, each a quarter, an eighth and a sixteenth of the render. Matched by that format rather than by size, so it keeps working if you change resolution. Turn it off if anything else in the picture goes flat.",
         &c.hud_hide_glare, 0, 0, 0, nullptr, 0, "off"},

        {RK::Float, "HUD size",
         "Shrinks the HUD about the centre of the screen. This BOTH pulls it inward and makes it smaller - one operation, they cannot be separated, which is why the two sliders above exist for moving alone. 1.00 leaves it exactly as the game drew it.",
         &c.hud_scale, 0.40f, 1.00f, 0.02f, nullptr, 0, "1.00"},

        {RK::Group, "ADVANCED  -  DO NOT TOUCH UNLESS NECESSARY", "",
         nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Int, "Isolate one HUD element",
         "Steps through the HUD one piece at a time, hiding a different element at each number - the groundwork for giving each part its own position control. 0 shows everything. -1 hides every piece at once, which is the check worth doing FIRST: if -1 does not blank the HUD then the filter is wrong and none of the numbers mean anything.",
         &c.hud_isolate, -1, 255, 1, nullptr, 0, "0 (off)"},

        {RK::Bool, "  ...search post-processing instead",
         "Switches the search above to the full-screen effect passes rather than the HUD. Those WRITE where the HUD blends, so the normal search cannot see them at all - which is why -1 hid every HUD piece and left the lens dirt untouched. Turn this on and sweep again to find the dirt, the flare, and anything else laid over the whole picture.",
         &c.hud_isolate_post, 0, 0, 0, nullptr, 0, "off"},

        {RK::Bool, "HUD trace (diagnostic)",
         "Writes a description of every HUD-looking draw to the log, then switches itself off after a few frames. Turn it ON WHILE PLAYING, with the HUD visible - it starts the moment you switch it on, so arming it from the ini catches the main menu instead and tells us nothing. Costs nothing while off. It is a fault-finding tool, not a setting.",
         &c.hud_probe, 0, 0, 0, nullptr, 0, "off"},

        {RK::Heading, "PROFILES", "", nullptr, 0, 0, 0, nullptr, 0, ""},


        {RK::Int, "Slot",
         "Which of the five saved setups the two rows below act on. Handy for keeping one set of "
         "settings for hunting on foot and another for a tower, or for keeping a known-good one "
         "before experimenting.",
         &c.profile_slot, 1, 5, 1, nullptr, 0, "1"},

        {RK::Action, "Save these settings to the slot",
         "Writes everything currently set into the slot chosen above, overwriting whatever was "
         "there. Left or right - either does it.",
         nullptr, 0, 0, 0, nullptr, 0, "", &ProfileSaveAct, &ProfileSaveText},

        {RK::Action, "Load the slot",
         "Replaces every current setting with the saved ones. Takes effect immediately - there is "
         "nothing to restart.",
         nullptr, 0, 0, 0, nullptr, 0, "", &ProfileLoadAct, &ProfileLoadText},

        {RK::Group, "START FROM A KNOWN GOOD SETUP", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Action, "Apply the recommended settings",
         "PUTS EVERYTHING BACK TO THE TESTED SETUP. These are the values the mod ships "
         "with - tuned in a Meta Quest 3 across many sessions, not the values each switch "
         "happens to be born with. Stereo and head tracking on, 6DoF on, the wider field "
         "with each eye cut to its own view, DLSS smoothing on model M, a little "
         "sharpening and colour, and the two features known to fight the rest left off. "
         "THIS IS THE WAY BACK. If you have changed things and the picture has gone wrong, "
         "or you simply cannot remember what you moved, one press here undoes all of it - "
         "so there is no setting on any tab you cannot safely experiment with. "
         "Needs two presses: LEFT to arm, then RIGHT to confirm. It saves immediately. "
         "On a headset other than a Quest 3 this is still the right starting point; the "
         "render resolution and the field of view on the VIEW tab are the two most likely "
         "to want adjusting afterwards.",
         nullptr, 0, 0, 0, nullptr, 0, "", &RecommendAct, &RecommendText},

        {RK::Group, "RESET", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Action, "Put EVERYTHING back to defaults",
         "Every setting on every tab, back to how the mod ships - including head tracking itself, "
         "which goes off. Needs two presses on purpose: LEFT to arm it, then RIGHT to confirm, so "
         "a stray press cannot wipe a setup you spent an evening tuning. Save a profile first if "
         "you might want the current one back.",
         nullptr, 0, 0, 0, nullptr, 0, "", &ResetAct, &ResetText},

        {RK::Heading, "WEAPON 3D", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Group, "WEAPON DEPTH", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "Weapon in 3D",
         "Gives the held weapon real depth. It looks flat because it rides the camera: "
         "the game re-places it relative to your view every frame, so both eyes see it "
         "in exactly the same place - and your brain reads 'no difference between the "
         "eyes' as 'infinitely far away'. This puts that difference back, so the gun "
         "sits in your hands instead of pasted to your face.",
         &c.weapon_3d, 0, 1, 1, nullptr, 0, "on"},

        {RK::Float, "3D strength",
         "How much difference between the eyes. Small numbers are already visible - 0.02 "
         "is a good start, 0.10 is a lot. Too much is uncomfortable in the same way a "
         "badly set up 3D film is. Negative pushes the gun behind the world instead of "
         "in front: wrong-looking, but a quick way to prove the setting is doing "
         "something.",
         &c.weapon_3d_amount, -0.20f, 0.20f, 0.005f, nullptr, 0, "0.130"},

        {RK::Group, "SIZE AND POSITION", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Float, "Weapon field of view",
         "THE WEAPON FOV. The game draws your weapon and hands with their own field of "
         "view, separate from the world - which is why they can look correct on a monitor "
         "and far too large in a headset. LOWER = wider weapon view, so the gun and hands "
         "shrink. HIGHER = narrower, so they fill more of the screen. 1.00 leaves the "
         "game's own value alone.",
         &c.weapon_view_scale, 0.50f, 2.00f, 0.01f, nullptr, 0, "0.50"},

        {RK::Float, "Weapon left / right",
         "Slides the weapon and hands sideways. Negative moves left, positive right. "
         "1.00 is half the width of your view, so useful values are small.",
         &c.weapon_view_offset_x, -1.00f, 1.00f, 0.01f, nullptr, 0, "0.00"},

        {RK::Float, "Weapon up / down",
         "Raises or lowers the weapon and hands. Useful if the gun sits too low to see "
         "over, or too high and covers what you are aiming at.",
         &c.weapon_view_offset_y, -1.00f, 1.00f, 0.01f, nullptr, 0, "0.00"},

        {RK::Group, "SCOPES AND BINOCULARS", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Bool, "One eye down the scope",
         "Puts both eyes on the same optical axis while you are looking through an optic - which is what closing one eye behind a real scope achieves. A real scope has an exit pupil and only one eye fits behind it; in VR both eyes see the picture from their own position 64 mm apart, so each lines the crosshair up with a different point downrange and you cannot tell which one the bullet follows. Turn this on if you are missing shots you thought were centred.",
         &c.scope_mono, 0, 0, 0, nullptr, 0, "on"},

        {RK::Bool, "  ...for iron sights as well",
         "Does the same when you aim a weapon with no optic. Iron sights have the same problem - two posts to line up, seen from two positions - but there is no scope glass for the mod to notice, so aiming is detected from the view narrowing instead. Works on any weapon without knowing anything about it.",
         &c.scope_mono_ads, 0, 0, 0, nullptr, 0, "off"},

        {RK::Float, "  ...eye separation while scoped",
         "Millimetres between the eyes while an optic is up. 0 is one axis and no ambiguity at all. Raise it if you want some depth back and can live with the crosshair being less certain.",
         &c.scope_ipd_mm, 0.0f, 64.0f, 4.0f, nullptr, 0, "0 mm"},

        {RK::Bool, "Separate strength while scoped",
         "Uses a different 3D strength when you are looking through a scope or binoculars. "
         "Aiming down an optic is a different problem from carrying a gun at the hip - the "
         "scope sits right against your eye and fills the view, so the amount that looks "
         "right for a held weapon is usually far too strong there. OFF uses one strength "
         "everywhere.",
         &c.weapon_3d_scoped_separate, 0, 1, 1, nullptr, 0, "on"},

        {RK::Float, "3D strength while scoped",
         "How far apart your two eyes see the weapon WHILE SCOPED. Only used when "
         "'Separate strength while scoped' is on. Start small and raise it until the scope "
         "has depth without your eyes struggling to merge it - much less than the hip-fire "
         "amount is normal. Negative flips which eye leads.",
         &c.weapon_3d_amount_scoped, -0.20f, 0.20f, 0.005f, nullptr, 0, "0.000"},

        // ---------------------------------------------------------------------
        // LAST TAB ON THE BAR, deliberately. Nothing here is needed to make the
        // mod work or look right - it changes the WORLD rather than the way it
        // is presented - so it sits at the far end, past everything a new player
        // has to walk through once.
        // ---------------------------------------------------------------------
        {RK::Heading, "WORLD", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Group, "TIME OF DAY", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Action, "Time of day",
         "Shows the world's actual clock. Push LEFT to wind it back an hour and RIGHT to "
         "push it forward an hour - the sky, the light and the shadows all move with it. "
         "Hold the direction to run through the day. Wrapping past midnight is fine. To "
         "stop the clock where you like it, switch on 'Freeze time' below.",
         nullptr, 0, 0, 0, nullptr, 0, "the game's own time", &TimeNudge, &TimeText},

        {RK::Bool, "Freeze time",
         "Stops the clock where it is, so the light and the shadows stay put. It holds "
         "whatever hour it was switched ON at, so set the time you want first and then "
         "freeze it. Turn it off and the day carries on from there.",
         &c.cheat_freeze_time, 0, 0, 0, nullptr, 0, "off"},

        {RK::Float, "Day length",
         "How fast the day runs. 1.00 is the game's own speed. Below that the day stretches - "
         "0.25 makes dawn last four times as long, which is the point if you want to hunt in "
         "good light. Above it the sun races across the sky. Set it back to 1.00 and the game "
         "carries on from wherever the clock has got to.",
         &c.cheat_time_scale, 0.05f, 10.0f, 0.05f, nullptr, 0, "1.00"},

        {RK::Group, "WEATHER", "", nullptr, 0, 0, 0, nullptr, 0, ""},

        {RK::Float, "Weather change speed",
         "How fast the weather moves on. -1 leaves the game's own pacing completely alone; "
         "0 holds the current weather; higher values run it faster, so you can watch a "
         "front come through in a minute instead of an hour. This one only takes effect "
         "once the game has asked about the weather at least once, so give it a moment "
         "after loading.",
         &c.cheat_weather_speed, -1.0f, 20.0f, 0.5f, nullptr, 0, "off"},

    };
    *count = int(sizeof(rows) / sizeof(rows[0]));
    return rows;
}

bool IsHeading(const Row& r) { return r.kind == RK::Heading; }
bool IsGroup(const Row& r) { return r.kind == RK::Group; }
bool IsHiddenRow(const Row& r) { return r.show && !r.show(); }
// Everything the cursor must skip past - labels, and rows that do not apply
// right now. Both are handled here so the cursor, the drawing and the scroll
// arithmetic cannot disagree about what is on screen.
bool IsLabelRow(const Row& r) { return IsHeading(r) || IsGroup(r) || IsHiddenRow(r); }

// *** TABS ***
//
// The list had grown to well over a hundred rows across seven headings, which
// meant scrolling past everything you were not looking for to reach anything
// you were. The headings already describe the groups, so they become the tabs -
// no second list to keep in step with the first, which is how a hand-kept menu
// eventually names one thing and shows another.
constexpr int kMaxTabs = 16;
struct TabInfo {
    const char* name;
    int first;      // first row AFTER the heading
    int last;       // one past the last row of this tab
};

int BuildTabs(TabInfo* out, int max) {
    int n = 0;
    Row* rows = Rows(&n);
    int count = 0;
    for (int i = 0; i < n && count < max; ++i) {
        if (!IsHeading(rows[i])) continue;
        if (count > 0) out[count - 1].last = i;
        out[count].name = rows[i].label;
        out[count].first = i + 1;
        out[count].last = n;
        ++count;
    }
    return count;
}

const TabInfo* Tabs(int* count) {
    static TabInfo tabs[kMaxTabs];
    static int n = 0;
    if (n == 0) n = BuildTabs(tabs, kMaxTabs);
    *count = n;
    return tabs;
}

// Which key row is being rebound, if any, and how far along.
//   0 = not capturing
//   1 = capturing, but waiting for every key to come up first, so the ENTER
//       that started the capture cannot immediately bind itself
//   2 = armed; the next key pressed is the new binding
int g_capture = 0;

// A name a person can read, rather than a number they have to look up.
// GetKeyNameText handles the printable keys and most of the rest; the extended
// flag matters for the arrows, Insert/Delete and the numpad, which otherwise
// come back named after their numeric twins.
// The XInput buttons, in the order of their bits, so a captured pad binding can
// be shown as "A" or "Left bumper" instead of a number nobody can act on.
struct PadName { WORD bit; const char* name; };
const PadName kPadNames[] = {
    {XINPUT_GAMEPAD_DPAD_UP, "D-pad up"},
    {XINPUT_GAMEPAD_DPAD_DOWN, "D-pad down"},
    {XINPUT_GAMEPAD_DPAD_LEFT, "D-pad left"},
    {XINPUT_GAMEPAD_DPAD_RIGHT, "D-pad right"},
    {XINPUT_GAMEPAD_START, "Start"},
    {XINPUT_GAMEPAD_BACK, "Back"},
    {XINPUT_GAMEPAD_LEFT_THUMB, "Left stick click"},
    {XINPUT_GAMEPAD_RIGHT_THUMB, "Right stick click"},
    {XINPUT_GAMEPAD_LEFT_SHOULDER, "Left bumper"},
    {XINPUT_GAMEPAD_RIGHT_SHOULDER, "Right bumper"},
    {XINPUT_GAMEPAD_A, "A"},
    {XINPUT_GAMEPAD_B, "B"},
    {XINPUT_GAMEPAD_X, "X"},
    {XINPUT_GAMEPAD_Y, "Y"},
};

const char* KeyName(int vk) {
    static char buf[64];
    if (vk <= 0) return "not set";
    if (vk & kPadBind) {                // a pad button, not a keyboard key
        const WORD bit = WORD(vk & 0xFFFF);
        for (const PadName& pn : kPadNames) {
            if (pn.bit == bit) { snprintf(buf, sizeof(buf), "Pad: %s", pn.name); return buf; }
        }
        snprintf(buf, sizeof(buf), "Pad button 0x%X", bit);
        return buf;
    }
    switch (vk) {                       // ones the API names badly or not at all
        case VK_LBUTTON: return "Left mouse";
        case VK_RBUTTON: return "Right mouse";
        case VK_MBUTTON: return "Middle mouse";
        case VK_XBUTTON1: return "Mouse 4";
        case VK_XBUTTON2: return "Mouse 5";
        default: break;
    }
    UINT scan = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (!scan) { snprintf(buf, sizeof(buf), "key %d", vk); return buf; }
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
        case VK_PRIOR:  case VK_NEXT:   case VK_LEFT: case VK_RIGHT:
        case VK_UP:     case VK_DOWN:   case VK_NUMLOCK: case VK_DIVIDE:
            scan |= 0x100;              // extended
            break;
        default: break;
    }
    if (GetKeyNameTextA((LONG)(scan << 16), buf, sizeof(buf)) > 0) return buf;
    snprintf(buf, sizeof(buf), "key %d", vk);
    return buf;
}

// *** A CONVERSION THAT CANNOT LEAVE GARBAGE ON THE SCREEN. ***
//
// MultiByteToWideChar does NOT truncate when the destination is too small: it
// fails, returns 0, and leaves the buffer holding whatever was on the stack.
// That is what put Chinese characters in the middle of the panel's help text -
// eleven rows had grown past the 640-wide buffer they were being converted
// into, and each one rendered as raw stack memory rather than as its own words.
//
// So: the buffer is always emptied first, and a string too long for it is
// converted as a PREFIX that fits rather than not at all. A cut sentence is a
// cosmetic problem; uninitialised memory drawn as text is not.
void ToWide(const char* src, wchar_t* out, int cap) {
    if (!out || cap <= 0) return;
    out[0] = L'\0';
    if (!src || !*src) return;
    if (MultiByteToWideChar(CP_UTF8, 0, src, -1, out, cap) > 0) return;

    // Too long. Convert as many BYTES as can possibly fit - one byte can never
    // produce more than one wide character in UTF-8 - and terminate by hand.
    int bytes = (int)strnlen(src, (size_t)cap - 1);
    // Never cut inside a multi-byte sequence: step back off any continuation
    // byte. The panel is ASCII today, and this costs nothing to be right about.
    while (bytes > 0 && ((unsigned char)src[bytes] & 0xC0) == 0x80) --bytes;
    const int wrote = MultiByteToWideChar(CP_UTF8, 0, src, bytes, out, cap - 1);
    out[(wrote > 0 && wrote < cap) ? wrote : 0] = L'\0';
}

void FormatValue(const Row& r, char* out, size_t n) {
    // *** A ROW THAT KNOWS MORE THAN ITS VARIABLE MAY SAY SO. ***
    //
    // "Quality" is a true answer and a useless one - the question the player is
    // actually asking is what the game is rendering and what the headset is
    // getting. Any row may supply live text, and when it does that text wins.
    if (r.val && r.kind != RK::Heading && r.kind != RK::Group) {
        snprintf(out, n, "%s", r.val());
        return;
    }
    switch (r.kind) {
        case RK::Heading:
        case RK::Group: out[0] = '\0'; break;
        case RK::Action:
            snprintf(out, n, "%s", r.val ? r.val() : "");
            break;
        case RK::Bool: {
            const bool on = *static_cast<bool*>(r.ptr);
            // A yes/no setting whose two states have NAMES shows the names.
            // "Stereo mode: OFF" tells the player nothing - off is not the
            // absence of stereo here, it is the other kind of stereo.
            if (r.names && r.nameCount >= 2) {
                strcpy_s(out, n, on ? r.names[1] : r.names[0]);
            } else {
                strcpy_s(out, n, on ? "ON" : "OFF");
            }
            break;
        }
        case RK::Int: {
            const int v = *static_cast<int*>(r.ptr);
            if (r.names && v >= 0 && v < r.nameCount) strcpy_s(out, n, r.names[v]);
            else snprintf(out, n, "%d", v);
            break;
        }
        case RK::Key:
            snprintf(out, n, "%s", g_capture ? "PRESS ANY KEY OR BUTTON"
                                             : KeyName(*static_cast<int*>(r.ptr)));
            break;
        case RK::Float: {
            const float v = *static_cast<float*>(r.ptr);
            if (r.hi > 100.0f) {
                if (v < 1.0f) strcpy_s(out, n, "fill headset");
                else snprintf(out, n, "%.0f deg", v);
            } else if (r.hi > 50.0f) {
                snprintf(out, n, "%.0f mm", v);
            } else if (r.step > 0.0f && r.step < 0.01f) {
                // *** ENOUGH DECIMALS TO SHOW THE STEP. ***
                //
                // "%.2f" on a slider that moves in 0.005 prints 0.00 for
                // anything from -0.0049 to +0.0049 - ten distinct settings,
                // including two that are not zero and one that is, all reading
                // the same. That cost real debugging time: a test that asked for
                // "strength 0" was run at a small non-zero shift and the result
                // was taken as proof that the shift does nothing.
                //
                // A value the user cannot distinguish from zero must not be
                // displayed as zero when it is not.
                snprintf(out, n, "%.3f m%s", v, (v != 0.0f && v > -0.0005f &&
                                                 v < 0.0005f) ? " (not 0)" : "");
            } else {
                snprintf(out, n, "%.2f m", v);
            }
            break;
        }
        case RK::Percent:
            snprintf(out, n, "%.0f%%", *static_cast<float*>(r.ptr) * 100.0f);
            break;
    }
}

// --- drawing --------------------------------------------------------------

float SdRoundRect(float px, float py, float cx, float cy, float hw, float hh, float rad) {
    float dx = fabsf(px - cx) - (hw - rad);
    float dy = fabsf(py - cy) - (hh - rad);
    if (dx < 0) dx = 0;
    if (dy < 0) dy = 0;
    return sqrtf(dx * dx + dy * dy) - rad;
}

inline uint32_t Blend(uint32_t dst, uint32_t src, float a) {
    if (a <= 0) return dst;
    if (a >= 1) return src;
    const int sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    const int dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    const int r = int(dr + (sr - dr) * a);
    const int g = int(dg + (sg - dg) * a);
    const int b = int(db + (sb - db) * a);
    return (r << 16) | (g << 8) | b;
}

}  // namespace

bool Overlay::Init() {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = m_width;
    bi.bmiHeader.biHeight = -m_height;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    m_dc = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    if (!m_dc) return false;

    void* bits = nullptr;
    m_bitmap = CreateDIBSection(m_dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!m_bitmap || !bits) return false;
    SelectObject(m_dc, m_bitmap);
    m_dib = static_cast<uint32_t*>(bits);

    m_pixels = new uint32_t[size_t(m_width) * m_height];
    m_mask = new uint8_t[size_t(m_width) * m_height];

    auto mkfont = [](int h, int weight) {
        return CreateFontW(h, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           VARIABLE_PITCH, L"Segoe UI");
    };
    m_fontTitle = mkfont(38, FW_SEMIBOLD);
    m_fontRow = mkfont(28, FW_NORMAL);
    m_fontHelp = mkfont(22, FW_NORMAL);
    m_fontSmall = mkfont(20, FW_SEMIBOLD);

    SetBkMode(m_dc, TRANSPARENT);

    // Skip headings when picking the initial selection.
    int n = 0;
    Row* rows = Rows(&n);
    for (int i = 0; i < n; ++i) {
        if (!IsLabelRow(rows[i])) { m_sel = i; break; }
    }

    audio::Init();
    audio::SetVolume(Cfg().sound_enabled ? Cfg().sound_volume : 0.0f);

    Redraw();
    COTW_LOG("[overlay] ready (%dx%d)", m_width, m_height);
    return true;
}

void Overlay::Shutdown() {
    audio::Shutdown();
    if (m_fontTitle) DeleteObject(m_fontTitle);
    if (m_fontRow) DeleteObject(m_fontRow);
    if (m_fontHelp) DeleteObject(m_fontHelp);
    if (m_fontSmall) DeleteObject(m_fontSmall);
    if (m_bitmap) DeleteObject(m_bitmap);
    if (m_dc) DeleteDC(m_dc);
    delete[] m_pixels;
    delete[] m_mask;
    m_pixels = nullptr;
    m_mask = nullptr;
}

float Overlay::DistanceMetres() const { return Cfg().panel_distance; }
float Overlay::WidthMetres() const { return Cfg().panel_size; }

bool Overlay::TakeDirty() {
    const bool d = m_dirty;
    m_dirty = false;
    return d;
}

// Hash EVERY value the panel displays. Hand-listing "the important ones" is a
// real bug waiting to happen: change any row that was left out and the bitmap
// is never rebuilt, so the new value simply does not appear until something
// else forces a redraw.
uint64_t Overlay::Signature() const {
    uint64_t h = 1469598103934665603ULL;
    auto mix = [&h](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ULL; }
    };
    int n = 0;
    Row* rows = Rows(&n);
    for (int i = 0; i < n; ++i) {
        char buf[96];
        FormatValue(rows[i], buf, sizeof(buf));
        mix(buf, strlen(buf));
        // A row appearing or disappearing changes the panel without changing
        // any value on it - without this the list would not be rebuilt and the
        // row would linger until something else forced a redraw.
        const uint8_t hidden = IsHiddenRow(rows[i]) ? 1 : 0;
        mix(&hidden, 1);
    }
    mix(&m_sel, sizeof(m_sel));
    mix(&m_scroll, sizeof(m_scroll));
    mix(&m_tab, sizeof(m_tab));
    mix(&m_visible, sizeof(m_visible));
    return h;
}

// Move to another tab and put the selection on its first real row.
void Overlay::MoveTab(int dir) {
    int tabCount = 0;
    const TabInfo* tabs = Tabs(&tabCount);
    if (tabCount <= 0) return;
    m_tab = (m_tab + dir + tabCount) % tabCount;
    int n = 0;
    Row* rows = Rows(&n);
    m_sel = tabs[m_tab].first;
    for (int i = tabs[m_tab].first; i < tabs[m_tab].last && i < n; ++i) {
        if (!IsLabelRow(rows[i])) { m_sel = i; break; }
    }
    m_scroll = 0;
    audio::Play(audio::Cue::Move);
}

// Which tab a row belongs to, so the selection and the tab bar cannot disagree.
void Overlay::SyncTabToSelection() {
    int tabCount = 0;
    const TabInfo* tabs = Tabs(&tabCount);
    for (int t = 0; t < tabCount; ++t) {
        if (m_sel >= tabs[t].first && m_sel < tabs[t].last) { m_tab = t; return; }
    }
}

void Overlay::MoveSelection(int dir) {
    int n = 0;
    Row* rows = Rows(&n);
    int tabCount = 0;
    const TabInfo* tabs = Tabs(&tabCount);
    // Movement stays INSIDE the current tab. Running off the end used to slide
    // into a neighbouring section, which with tabs would leave the selection and
    // the tab bar showing different things.
    const int lo = (m_tab < tabCount) ? tabs[m_tab].first : 0;
    const int hi = (m_tab < tabCount) ? tabs[m_tab].last : n;
    int i = m_sel;
    for (int guard = 0; guard < n * 2; ++guard) {
        i += dir;
        if (i < lo || i >= hi) { audio::Play(audio::Cue::Limit); return; }
        if (!IsLabelRow(rows[i])) { m_sel = i; audio::Play(audio::Cue::Move); return; }
    }
}

void Overlay::Adjust(int dir) {
    int n = 0;
    Row* rows = Rows(&n);
    if (m_sel < 0 || m_sel >= n) return;
    Row& r = rows[m_sel];

    switch (r.kind) {
        case RK::Heading:
        case RK::Group:
            return;
        case RK::Action:
            if (r.act) { r.act(dir); audio::Play(audio::Cue::Move); }
            return;
        case RK::Bool: {
            bool* b = static_cast<bool*>(r.ptr);
            *b = !*b;
            audio::Play(*b ? audio::Cue::On : audio::Cue::Off);
            break;
        }
        case RK::Key:      // rebound by pressing a key, not by nudging
            audio::Play(audio::Cue::Limit);
            return;
        case RK::Int: {
            int* v = static_cast<int*>(r.ptr);
            // r.step was IGNORED here, so a row declared 200..2000 step 100 moved
            // one unit a press - 400 presses to cross its own range, which reads
            // as a broken setting. Every other Int row declares step 1, so they
            // behave exactly as before.
            int step = (int)r.step;
            if (step < 1) step = 1;
            int nv = *v + step * dir;
            if (nv < (int)r.lo) nv = (int)r.lo;
            if (nv > (int)r.hi) nv = (int)r.hi;
            // Clamping rather than refusing: a step that would overshoot lands on
            // the limit instead of doing nothing. Already AT the limit still
            // reports the limit, which is what the step-1 rows did before.
            if (nv == *v) { audio::Play(audio::Cue::Limit); return; }
            *v = nv;
            audio::Play(audio::Cue::Adjust);
            break;
        }
        case RK::Float:
        case RK::Percent: {
            float* v = static_cast<float*>(r.ptr);
            float nv = *v + r.step * dir;
            if (nv < r.lo - 1e-4f || nv > r.hi + 1e-4f) { audio::Play(audio::Cue::Limit); return; }
            if (nv < r.lo) nv = r.lo;
            if (nv > r.hi) nv = r.hi;
            // Land exactly on zero when stepping across it. Accumulated float
            // error otherwise leaves a residue like 1.7e-9 that reads as 0.000
            // and still shifts - "off" has to mean off, not nearly off.
            if (nv > -r.step * 0.5f && nv < r.step * 0.5f) nv = 0.0f;
            *v = nv;
            audio::Play(audio::Cue::Adjust);
            break;
        }
    }

    // *** A VALUE THE PLAYER SET BY HAND IS THEIRS FROM THEN ON. ***
    //
    // The field of view is computed for the player at every launch, from the
    // render shape and the headset's own vertical field, and written back over
    // whatever is in the ini - which is right up until the moment they set it
    // themselves. Then it silently undid their change on the next launch: they
    // move the slider, it looks right, they restart, and it is back where it
    // was. Nothing in the panel said why, because nothing in the panel knew.
    //
    // Touching the row is the statement of intent. From that press onwards the
    // number is theirs and the automatic follower stands down - which is what
    // game_fov_follows_auto always meant, it just had no way of being set from
    // inside the headset.
    if ((r.ptr == (void*)&Cfg().game_fov_deg ||
         r.ptr == (void*)&Cfg().fov_is_horizontal) &&
        Cfg().game_fov_follows_auto) {
        Cfg().game_fov_follows_auto = false;
        COTW_LOG("[panel] field of view set by hand (%.1f deg) - the automatic "
                 "follower is now OFF so this value survives a restart",
                 Cfg().game_fov_deg);
    }

    // Sound settings must take effect the moment they are changed, or the
    // feedback tone contradicts the value on screen.
    audio::SetVolume(Cfg().sound_enabled ? Cfg().sound_volume : 0.0f);
    Cfg().Save();
}

void Overlay::Activate() {
    int n = 0;
    Row* rows = Rows(&n);
    if (m_sel < 0 || m_sel >= n) return;
    if (rows[m_sel].kind == RK::Key) {
        // Wait for every key to come up before arming, or the ENTER that got us
        // here would bind itself the instant we started listening.
        g_capture = 1;
        audio::Play(audio::Cue::Move);
        return;
    }
    if (rows[m_sel].kind == RK::Bool) Adjust(1);
    else audio::Play(audio::Cue::Move);
}

void Overlay::Toggle() {
    m_visible = !m_visible;
    audio::Play(m_visible ? audio::Cue::Open : audio::Cue::Close);

    // *** THE FIRST-RUN PAGE, ON EVERY OPEN UNTIL IT IS DISMISSED. ***
    //
    // Not once per session and not once ever: until the player ticks "do not
    // show this page again" they land here every time they open the panel. The
    // three things on it - the game's own anti-aliasing, single player only,
    // and that this is unfinished - are the ones that cost a stranger an
    // evening if they are missed, and a page that shows once can be missed by
    // opening the panel while putting the headset on.
    //
    // It is a TAB, not a modal: nothing is blocked, the cursor can walk
    // straight off it, and it stays reachable afterwards rather than being
    // gone forever the moment it is dismissed.
    if (m_visible && !Cfg().welcome_seen) {
        m_tab = 0;
        m_scroll = 0;
        int n = 0;
        Row* rows = Rows(&n);
        for (int i = 0; i < n; ++i) {
            if (!IsLabelRow(rows[i])) { m_sel = i; break; }
        }
    }

    if (!m_visible) Cfg().Save();
    m_dirty = true;
    COTW_LOG("[overlay] %s", m_visible ? "opened" : "closed");
}

bool Overlay::ConsumePad(DWORD userIndex, XINPUT_STATE* state) {
    if (!state) return false;

    // Everything below is per SLOT. This runs once for every user index the game
    // polls - not once per frame - and with one shared button history a second
    // connected pad reporting all zeros wiped the real pad's state between
    // polls. A held button then read as a fresh press every time, which is why
    // one press acted like ten. Steam Input's virtual device means a second
    // "connected" pad is the normal case, not an exotic one.
    const unsigned idx = (userIndex < 4) ? userIndex : 0;
    WORD& prevButtons = m_prevButtons[idx];
    DWORD& dirHeldSince = m_dirHeldSince[idx];
    int& stickDir = m_stickDir[idx];
    DWORD& stickSince = m_stickSince[idx];

    const WORD b = state->Gamepad.wButtons;
    const WORD went = WORD(b & ~prevButtons);

    // Remember them whatever happens next. A pad binding has to be testable
    // while the panel is CLOSED - that is the whole point of a hotkey - and the
    // capture loop in Tick() reads this too.
    m_padButtons[idx] = b;
    // When the game last handed us the pad. The mirror window polls it
    // itself only while this has gone quiet - see PollPadForMirror.
    InterlockedExchange(&g_lastPadFeed, (LONG)GetTickCount());

    // NO pad button opens the panel - Insert on the keyboard is the only way in.
    // Every pad binding tried so far collided with something the game itself
    // uses, and a panel that steals a button mid-hunt is worse than one that
    // needs a keypress to reach. The pad still drives the panel once it is open.

    if (!m_visible) {
        prevButtons = b;
        return false;
    }

    // *** THE CAPTURE HAPPENS HERE, NOT IN Tick(). ***
    //
    // It used to cache the buttons and let the keyboard loop in Tick() read the
    // copy - and the copy was always empty by the time it looked, which the log
    // said plainly: "[bind] listening: pad buttons = 0x0000" for eight seconds
    // solid while buttons were being pressed. Far Cry 2's mod does not have that
    // problem because it captures in the pad callback itself
    // (fc2vr BindingHandlePad, "from the XInput proxy's post-process"), where
    // the state is in its hands and cannot have gone stale or been missed.
    //
    // `went` rather than `b` is what closes the obvious trap: it is a button
    // that has just gone DOWN, so the A press that opened the capture - still
    // held at that moment - cannot bind itself.
    if (g_capture) {
        if (g_capture == 1 && !b) {
            g_capture = 2;                  // everything released: now listening
            COTW_LOG("[bind] armed (pad) - press any key or button");
        }
        if (g_capture == 2 && went) {
            for (const PadName& pn : kPadNames) {
                if (went & pn.bit) {
                    int n2 = 0;
                    Row* rr = Rows(&n2);
                    if (m_sel >= 0 && m_sel < n2 && rr[m_sel].kind == RK::Key) {
                        *static_cast<int*>(rr[m_sel].ptr) = kPadBind | pn.bit;
                        COTW_LOG("[bind] captured pad button %s", pn.name);
                        audio::Play(audio::Cue::On);
                    }
                    g_capture = 0;
                    m_dirty = true;
                    break;
                }
            }
        }
        prevButtons = b;
        memset(&state->Gamepad, 0, sizeof(state->Gamepad));
        state->dwPacketNumber++;
        return true;
    }

    const DWORD now = GetTickCount();

    // A HELD d-pad direction repeats, slowly at first then accelerating, the same
    // as the keyboard. Stepping a value one press at a time is unusable for
    // something with a few hundred settings.
    const WORD kHeldDirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
                           XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
    const WORD heldDir = WORD(b & kHeldDirs);
    if (heldDir && heldDir == WORD(prevButtons & kHeldDirs)) {
        if (!dirHeldSince) dirHeldSince = now;
    } else {
        dirHeldSince = 0;
    }
    bool autoRepeat = false;
    if (dirHeldSince) {
        const DWORD held = now - dirHeldSince;
        if (held >= 400) {
            DWORD interval = 200;
            if (held > 3000) interval = 40;
            else if (held > 1500) interval = 80;
            else if (held > 800) interval = 130;
            autoRepeat = (now - m_lastInput) >= interval;
        }
    }
    const WORD act = WORD(went | (autoRepeat ? heldDir : 0));

    if (act & XINPUT_GAMEPAD_DPAD_UP) { MoveSelection(-1); m_lastInput = now; }
    else if (act & XINPUT_GAMEPAD_DPAD_DOWN) { MoveSelection(1); m_lastInput = now; }
    else if (act & XINPUT_GAMEPAD_DPAD_LEFT) { Adjust(-1); m_lastInput = now; }
    else if (act & XINPUT_GAMEPAD_DPAD_RIGHT) { Adjust(1); m_lastInput = now; }
    else if (went & XINPUT_GAMEPAD_LEFT_SHOULDER) { MoveTab(-1); m_lastInput = now; }
    else if (went & XINPUT_GAMEPAD_RIGHT_SHOULDER) { MoveTab(1); m_lastInput = now; }
    else if (went & XINPUT_GAMEPAD_A) { Activate(); m_lastInput = now; }
    else if (went & XINPUT_GAMEPAD_B) { Toggle(); m_lastInput = now; }
    else {
        // The stick: edge-triggered with hysteresis, then the same accelerating
        // repeat as the d-pad above.
        //
        // It used to fire on a bare 170 ms timer with no notion of the stick
        // ever returning to centre. Two things went wrong with that. A single
        // flick that took a moment to fall back through the threshold scored
        // several steps, so the selection shot past whatever you were aiming
        // for; and because the only gate was elapsed time, how fast it ran
        // depended on how often the game happened to poll us rather than on
        // anything the player did.
        //
        // Requiring the stick to come back under a LOWER threshold before it
        // can act again makes one push exactly one step, always - and the gap
        // between the two thresholds keeps a stick resting slightly off-centre
        // from chattering.
        const short ly = state->Gamepad.sThumbLY;
        const short lx = state->Gamepad.sThumbLX;
        const short kOn = 20000;    // must pass this to act
        const short kOff = 12000;   // must fall back under this to re-arm

        int dir = 0;
        if (ly > kOn) dir = 1;
        else if (ly < -kOn) dir = 2;
        else if (lx < -kOn) dir = 3;
        else if (lx > kOn) dir = 4;

        const bool centred = ly < kOff && ly > -kOff && lx < kOff && lx > -kOff;
        if (centred) { stickDir = 0; stickSince = 0; }

        bool fire = false;
        if (dir && dir != stickDir) {
            stickDir = dir;
            stickSince = now;
            fire = true;                 // the push itself, always one step
        } else if (dir && stickSince) {
            const DWORD held = now - stickSince;
            if (held >= 400) {           // a normal push never repeats
                DWORD interval = 220;
                if (held > 3000) interval = 60;
                else if (held > 1500) interval = 100;
                else if (held > 800) interval = 150;
                fire = (now - m_lastInput) >= interval;
            }
        }

        if (fire) {
            switch (stickDir) {
                case 1: MoveSelection(-1); break;
                case 2: MoveSelection(1); break;
                case 3: Adjust(-1); break;
                case 4: Adjust(1); break;
                default: break;
            }
            m_lastInput = now;
        }
    }

    prevButtons = b;

    // The panel owns the pad while it is open, so the player does not walk off
    // a cliff while adjusting a slider.
    memset(&state->Gamepad, 0, sizeof(state->Gamepad));
    state->dwPacketNumber++;
    return true;
}

// *** KEEPING THE PANEL'S ARROWS OUT OF THE GAME. ***
//
// The pad was already handled - ConsumePad blanks it - but the keyboard went
// straight past us, so arrowing around the panel walked the player about too.
//
// Far Cry 2's mod solves the same problem by hooking DirectInput and zeroing
// what the game reads (fc2vr dinput_hook.cpp, SetOverlayCapture). That does not
// transfer: this is a modern title and it does not take its keyboard through
// DirectInput. A low-level keyboard hook does the same job one layer earlier and
// does not care how the game reads input, because nothing has read it yet.
//
// The catch, and why this is not three lines: a key swallowed here never reaches
// the async key state either, so GetAsyncKeyState would stop seeing the very
// keys the panel runs on - it would go deaf to its own arrows. So the hook keeps
// its OWN table of what is down and the panel reads that. Same shape as FC2:
// capture at the source instead of sampling downstream.
namespace {

volatile LONG g_kbDown[256];
HHOOK g_kbHook = nullptr;
volatile LONG g_kbReady = 0;

// Only the keys the panel actually uses. Swallowing everything would be simpler
// and would also eat Alt+F4 and the game's own bindings while the panel is up.
bool IsPanelKey(int vk) {
    switch (vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT:
        case VK_RETURN: case VK_ESCAPE: case VK_TAB:
        case VK_PRIOR: case VK_NEXT:
            return true;
        // INSERT IS DELIBERATELY NOT IN THAT LIST. It is the way in and out of
        // the panel, so it is the one key that must keep working even if this
        // hook misbehaves - swallowing it would let a bug in here lock the
        // player out of their own settings with no way back. The game does not
        // use Insert for anything, so letting it reach the game costs nothing.
        default:
            return false;
    }
}

LRESULT CALLBACK LowLevelKeyboard(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && lParam) {
        const KBDLLHOOKSTRUCT* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
        const int vk = int(k->vkCode) & 0xFF;
        const bool down = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        const bool up = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        if (down || up) {
            InterlockedExchange(&g_kbDown[vk], down ? 1 : 0);
            // Swallow only while the panel is open AND this process is in front,
            // so nothing is taken from the desktop or from another window.
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (g_overlay.Visible() && pid == GetCurrentProcessId() && IsPanelKey(vk)) {
                return 1;                // handled here; the game never sees it
            }
        }
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// A low-level hook needs a thread with a message pump, and the callback has to
// be quick - Windows silently drops a hook that dawdles, which would look like
// the panel randomly losing its keys.
DWORD WINAPI KeyboardHookThread(LPVOID) {
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, &LowLevelKeyboard,
                                 GetModuleHandleW(nullptr), 0);
    if (!g_kbHook) {
        COTW_LOG("[overlay] low-level keyboard hook failed (%lu) - the panel's "
                 "arrows will still reach the game", GetLastError());
        return 0;
    }
    InterlockedExchange(&g_kbReady, 1);
    COTW_LOG("[overlay] keyboard hook installed - the panel's own keys stop here "
             "instead of moving the player as well");
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

}  // namespace

namespace {

// *** RAW INPUT IS THE HALF A KEYBOARD HOOK CANNOT REACH. ***
//
// The low-level hook above stops two of the three ways a game can read a key: it
// blocks the WM_KEYDOWN message, and because the key never reaches the async
// state, it blocks GetAsyncKeyState too.
//
// It does NOT stop Raw Input. WM_INPUT is delivered by the raw input manager
// before the hook chain runs, so a game reading keys that way still gets every
// one of them - which is exactly what was happening: the panel swallowed the
// arrows and the player walked about anyway.
//
// So the key is neutralised where the game collects it instead. VKey 0xFF is the
// documented "keyboard overrun" value and every sane reader skips it, which is a
// gentler lie than pretending the call failed - returning an error from
// GetRawInputData would put the game's input code down a path it may not expect.
using PFN_GetRawInputData = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
using PFN_GetRawInputBuffer = UINT(WINAPI*)(PRAWINPUT, PUINT, UINT);
PFN_GetRawInputData o_GetRawInputData = nullptr;
PFN_GetRawInputBuffer o_GetRawInputBuffer = nullptr;

void SilenceKeyboard(RAWINPUT* ri) {
    if (!ri || ri->header.dwType != RIM_TYPEKEYBOARD) return;
    ri->data.keyboard.MakeCode = 0;
    ri->data.keyboard.VKey = 0xFF;          // "overrun" - readers skip it
    ri->data.keyboard.Message = WM_NULL;
    ri->data.keyboard.Flags = RI_KEY_BREAK;
}

// LOOK UP AND DOWN WITH YOUR HEAD ONLY - the mouse half.
//
// Only lLastY is cleared. lLastX still turns the player, because you cannot turn
// a chair 180 degrees; it is the vertical axis that fights the neck, by tilting
// the horizon away from where the head says it is.
//
// Done here rather than by clamping the camera afterwards because the game reads
// its own pitch back for the weapon and the crosshair. Removing the input leaves
// every one of those readers agreeing with each other; overwriting the result
// afterwards leaves them disagreeing, which is the shape of half the bugs in
// this project.
void LockMousePitch(RAWINPUT* ri) {
    if (!ri || ri->header.dwType != RIM_TYPEMOUSE) return;
    if (!Cfg().lock_pitch_input || !Cfg().lock_pitch_mouse) return;
    ri->data.mouse.lLastY = 0;
}

UINT WINAPI Hook_GetRawInputData(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size,
                                 UINT hdrSize) {
    const UINT r = o_GetRawInputData(h, cmd, data, size, hdrSize);
    if (data && cmd == RID_INPUT && r != (UINT)-1) {
        if (g_overlay.Visible()) SilenceKeyboard(static_cast<RAWINPUT*>(data));
        LockMousePitch(static_cast<RAWINPUT*>(data));
    }
    return r;
}

// The batched form. Less common, but a game that uses it would be completely
// unaffected by hooking only the single-message call, and the failure would look
// identical - so both are covered rather than guessing which this one uses.
UINT WINAPI Hook_GetRawInputBuffer(PRAWINPUT data, PUINT size, UINT hdrSize) {
    const UINT n = o_GetRawInputBuffer(data, size, hdrSize);
    if (data && n != (UINT)-1 && n > 0 &&
        (g_overlay.Visible() ||
         (Cfg().lock_pitch_input && Cfg().lock_pitch_mouse))) {
        // Walked by hand rather than with NEXTRAWINPUTBLOCK: that macro is
        // written in terms of QWORD, which this translation unit does not have.
        // It only means "advance by dwSize, rounded up to 8", so the arithmetic
        // is spelled out instead of dragging in a header for one line.
        RAWINPUT* ri = data;
        for (UINT i = 0; i < n; ++i) {
            if (g_overlay.Visible()) SilenceKeyboard(ri);
            LockMousePitch(ri);
            const size_t step = (size_t(ri->header.dwSize) + 7u) & ~size_t(7u);
            ri = reinterpret_cast<RAWINPUT*>(reinterpret_cast<BYTE*>(ri) + step);
        }
    }
    return n;
}

void InstallRawInputHooks() {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    if (!u32) return;
    struct { const char* name; void* hook; void** orig; } k[] = {
        {"GetRawInputData", &Hook_GetRawInputData, (void**)&o_GetRawInputData},
        {"GetRawInputBuffer", &Hook_GetRawInputBuffer, (void**)&o_GetRawInputBuffer},
    };
    for (auto& e : k) {
        void* target = reinterpret_cast<void*>(GetProcAddress(u32, e.name));
        if (!target) continue;
        MH_STATUS s = MH_CreateHook(target, e.hook, e.orig);
        if ((s == MH_OK || s == MH_ERROR_ALREADY_CREATED) && MH_EnableHook(target) == MH_OK) {
            COTW_LOG("[overlay] %s hooked - keys stop at the panel instead of also "
                     "reaching the game", e.name);
        } else {
            COTW_LOG("[overlay] %s hook failed (%d)", e.name, (int)s);
        }
    }
}

}  // namespace

void EnsureKeyboardHook() {
    static bool started = false;
    if (started) return;
    started = true;
    InstallRawInputHooks();
    if (HANDLE t = CreateThread(nullptr, 0, &KeyboardHookThread, nullptr, 0, nullptr)) {
        CloseHandle(t);
    }
}

// Is that key down? BOTH sources, either one counting.
//
// Reading only the hook's table was wrong, and it cost the panel its own open
// key. That table only knows about events seen SINCE the hook went in, so if the
// hook is late, is dropped by Windows for being slow, or simply never sees a
// particular key, it reports "up" forever and the panel goes deaf.
//
// Asking Windows alone is wrong the other way: a swallowed key never reaches the
// async state at all.
//
// Either source saying "down" is the honest answer. An un-swallowed key shows up
// in the async state; a swallowed one shows up in the table. Neither can invent
// a press, so OR is safe as well as robust.
bool PanelKeyDown(int vk) {
    if (vk <= 0 || vk > 0xFF) return false;
    if ((GetAsyncKeyState(vk) & 0x8000) != 0) return true;
    if (InterlockedCompareExchange(&g_kbReady, 0, 0)) return g_kbDown[vk] != 0;
    return false;
}

// A binding is a key or a pad button; ask the right hardware.
bool BindingDown(int code) {
    if (code <= 0) return false;
    if (code & kPadBind) return (g_overlay.PadButtons() & WORD(code & 0xFFFF)) != 0;
    return PanelKeyDown(code);
}

// Is a key row listening for its new binding right now? Callers outside
// Overlay::Tick have to ask, because Tick's swallow does not reach them.
bool BindingCaptureActive() { return g_capture != 0; }


// *** THE PANEL IN A WINDOW OF ITS OWN. ***
//
// The first attempt copied the panel into the game's backbuffer at Present. It
// crashed the game - with the headset off AND on - and the second attempt, which
// unbound the render targets first and checked the device, crashed it as well.
// Two crashes is enough to stop trying to be clever with somebody else's
// swapchain: whatever this engine is doing at Present, a texture copy into its
// backbuffer is not survivable, and a convenience feature is not worth a single
// crash.
//
// So this touches no D3D at all. A plain window, its own thread, and GDI drawing
// the same pixel buffer the headset quad is fed from. It cannot interfere with
// the game's rendering because it never speaks to the game's renderer.
//
// The pixels are premultiplied BGRA, which is exactly what a 32-bit BI_RGB DIB
// wants, so no conversion is needed. GDI ignores the alpha, so the panel's
// transparent surround arrives black - the same flat look the backbuffer version
// would have had, and fine for reading values at a desk.
//
// Tearing is possible: the game thread redraws the buffer while this thread
// paints it. That is a torn frame at worst, on a settings panel, and worth it to
// avoid a lock on the render thread's path.
namespace {

HWND g_mirrorWnd = nullptr;
volatile LONG g_mirrorQuit = 0;

// *** THE PAD, READ BY US WHEN THE GAME IS NOT READING IT. ***
//
// Pad input reaches the panel through the XInput proxy, which only fires when
// THE GAME polls - and a game that is not the foreground window stops polling.
// So the moment you click on this window to look at it, the controller goes
// dead, which is precisely backwards for a window whose whole purpose is to be
// used instead of the headset.
//
// This polls XInput directly, but only while the game's own feed has gone quiet
// for a moment. Otherwise both paths would step the selection and every press
// would count twice.
//
// The system DLL by name, not our own proxy sitting beside the exe.
using PFN_XIGetState = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
PFN_XIGetState g_xiGetState = nullptr;

void PollPadForMirror() {
    const DWORD quietFor =
        GetTickCount() - (DWORD)InterlockedCompareExchange(&g_lastPadFeed, 0, 0);
    if (quietFor < 250) return;              // the game is feeding us; leave it

    if (!g_xiGetState) {
        for (const wchar_t* name : {L"xinput1_4.dll", L"xinput1_3.dll",
                                    L"xinput9_1_0.dll"}) {
            if (HMODULE m = LoadLibraryW(name)) {
                g_xiGetState =
                    (PFN_XIGetState)GetProcAddress(m, "XInputGetState");
                if (g_xiGetState) break;
            }
        }
        if (!g_xiGetState) return;
    }
    for (DWORD i = 0; i < 4; ++i) {
        XINPUT_STATE st{};
        if (g_xiGetState(i, &st) != ERROR_SUCCESS) continue;
        g_overlay.ConsumePad(i, &st);        // same path the proxy uses
    }
}

LRESULT CALLBACK MirrorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            PollPadForMirror();
            return 0;
        case WM_CLOSE:
            ShowWindow(h, SW_HIDE);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// *** OVER THE GAME'S WINDOW, WITHOUT TOUCHING ITS RENDERING. ***
//
// Copying the panel into the backbuffer crashed the game twice, so nothing here
// speaks to D3D. This is a borderless, click-through, per-pixel-alpha window
// parked over the game's client area. It LOOKS like an in-game overlay and is
// technically nothing of the sort - which is exactly why it cannot break the
// game's rendering.
//
// UpdateLayeredWindow with ULW_ALPHA wants premultiplied BGRA, and premultiplied
// BGRA is precisely what the panel already is - the same buffer the headset quad
// is fed. So the rounded corners and the soft shadow survive here, where the
// backbuffer copy would have shown them as black.
//
// WS_EX_TRANSPARENT keeps it click-through: the game never loses focus to it, so
// the game keeps polling the pad and the proxy keeps feeding us.
HDC g_memDc = nullptr;
HBITMAP g_dib = nullptr;
void* g_dibBits = nullptr;
int g_dibW = 0, g_dibH = 0;

// The game's own window: the largest visible top-level window this process owns.
BOOL CALLBACK PickGameWindow(HWND h, LPARAM lp) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h)) return TRUE;
    if (h == g_mirrorWnd) return TRUE;
    RECT r{};
    if (!GetClientRect(h, &r)) return TRUE;
    const long area = (r.right - r.left) * (long)(r.bottom - r.top);
    auto* best = reinterpret_cast<std::pair<HWND, long>*>(lp);
    if (area > best->second) { best->first = h; best->second = area; }
    return TRUE;
}

HWND GameWindow() {
    std::pair<HWND, long> best{nullptr, 0};
    EnumWindows(&PickGameWindow, reinterpret_cast<LPARAM>(&best));
    return best.first;
}

void PaintLayered() {
    const int w = g_overlay.Width(), h = g_overlay.Height();
    const uint32_t* px = g_overlay.Pixels();
    if (!g_mirrorWnd || !px) return;

    if (!g_dib || g_dibW != w || g_dibH != h) {
        if (g_dib) { DeleteObject(g_dib); g_dib = nullptr; }
        if (!g_memDc) g_memDc = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;              // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        g_dib = CreateDIBSection(g_memDc, &bi, DIB_RGB_COLORS, &g_dibBits, nullptr, 0);
        if (!g_dib) return;
        SelectObject(g_memDc, g_dib);
        g_dibW = w;
        g_dibH = h;
    }
    if (!g_dibBits) return;
    memcpy(g_dibBits, px, size_t(w) * h * 4);

    // Centred on the game's client area, in screen coordinates.
    POINT pos{60, 60};
    if (HWND game = GameWindow()) {
        RECT rc{};
        GetClientRect(game, &rc);
        POINT tl{0, 0};
        ClientToScreen(game, &tl);
        pos.x = tl.x + ((rc.right - rc.left) - w) / 2;
        pos.y = tl.y + ((rc.bottom - rc.top) - h) / 2;
        if (pos.x < 0) pos.x = 0;
        if (pos.y < 0) pos.y = 0;
    }

    SIZE size{w, h};
    POINT src{0, 0};
    BLENDFUNCTION bf{};
    bf.BlendOp = AC_SRC_OVER;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;               // premultiplied, which it is
    HDC screen = GetDC(nullptr);
    UpdateLayeredWindow(g_mirrorWnd, screen, &pos, &size, g_memDc, &src, 0, &bf,
                        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
}

DWORD WINAPI MirrorThread(LPVOID) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &MirrorProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"cotwvr_panel_overlay";
    RegisterClassExW(&wc);

    g_mirrorWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE,
        wc.lpszClassName, L"theHunter CotW VR", WS_POPUP,
        0, 0, g_overlay.Width(), g_overlay.Height(),
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_mirrorWnd) {
        COTW_LOG("[panel] could not create the on-screen window (%lu)", GetLastError());
        return 0;
    }
    SetTimer(g_mirrorWnd, 1, 33, nullptr);
    COTW_LOG("[panel] on-screen panel ready - drawn over the game window, with no "
             "D3D involved");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}

}  // namespace

// Follows the panel: shown while it is open, hidden when it is not.
void UpdateMirrorWindow() {
    static bool started = false;
    if (!Cfg().panel_on_monitor) {
        if (g_mirrorWnd) ShowWindow(g_mirrorWnd, SW_HIDE);
        return;
    }
    if (!started) {
        started = true;
        if (HANDLE t = CreateThread(nullptr, 0, &MirrorThread, nullptr, 0, nullptr)) {
            CloseHandle(t);
        }
        return;
    }
    if (!g_mirrorWnd) return;
    const bool want = g_overlay.Visible();
    const bool shown = IsWindowVisible(g_mirrorWnd) != 0;
    if (want != shown) ShowWindow(g_mirrorWnd, want ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (want) PaintLayered();
}

void Overlay::Tick() {
    UpdateMirrorWindow();
    EnsureKeyboardHook();   // idempotent; needs its own message-pump thread

    // The log switch is the one setting whose effect lives OUTSIDE the config
    // object - the logger keeps its own copy, set once at startup. Without this
    // the row would move, save, and change nothing until the next launch, which
    // is the exact failure the config guard exists to catch in the other
    // direction.
    static bool logWas = Cfg().logging;
    if (logWas != Cfg().logging) {
        logWas = Cfg().logging;
        if (logWas) LogSetEnabled(true);   // say so while it can still be said
        COTW_LOG("[panel] logging switched %s", logWas ? "ON" : "OFF");
        LogSetEnabled(logWas);
    }

    // Keyboard, for anyone without a pad. Ctrl+Alt+O so it cannot be hit by
    // accident, plus arrows/enter while open.
    // Fires on the press, then REPEATS while the key is held - slowly at first,
    // then accelerating. One step per press is fine for a handful of settings
    // and miserable for a few hundred, which is exactly what hunting a shader
    // index needs.
    auto pressed = [](int vk, bool needMods) {
        static bool was[256] = {};
        static DWORD downAt[256] = {};
        static DWORD lastFire[256] = {};
        const int i = vk & 0xFF;

        // PanelKeyDown, not GetAsyncKeyState: the keys this panel uses are
        // swallowed before they reach the async state, so asking Windows would
        // report them as never pressed.
        bool down = PanelKeyDown(vk);
        if (needMods) {
            down = down && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                   (GetAsyncKeyState(VK_MENU) & 0x8000);
        }
        const DWORD now = GetTickCount();

        if (down && !was[i]) {                  // the initial press
            was[i] = true;
            downAt[i] = now;
            lastFire[i] = now;
            return true;
        }
        if (!down) { was[i] = false; return false; }

        // Held: a 400 ms grace period so a normal press never repeats, then
        // accelerating 200 ms -> 40 ms the longer it is held down.
        const DWORD held = now - downAt[i];
        if (held < 400) return false;
        DWORD interval = 200;
        if (held > 3000) interval = 40;
        else if (held > 1500) interval = 80;
        else if (held > 800) interval = 130;
        if (now - lastFire[i] < interval) return false;
        lastFire[i] = now;
        return true;
    };

    // Edge only - no repeat. Holding Insert must not flap the panel open and
    // shut, and holding Enter must not toggle a setting dozens of times.
    auto pressedOnce = [](int vk, bool needMods) {
        static bool was[256] = {};
        bool down = PanelKeyDown(vk);
        if (needMods) {
            down = down && (GetAsyncKeyState(VK_CONTROL) & 0x8000) &&
                   (GetAsyncKeyState(VK_MENU) & 0x8000);
        }
        const bool edge = down && !was[vk & 0xFF];
        was[vk & 0xFF] = down;
        return edge;
    };

    // Insert opens and closes it: one key, no modifier, and CotW does not use
    // it for anything.
    // Rebinding a key: listen for the next real press and take it, so nobody has
    // to look up a virtual key code to change a hotkey.
    if (g_capture) {
        bool anyDown = false;
        int hit = 0;
        for (int vk = 0x01; vk <= 0xFE; ++vk) {
            if (vk == VK_LBUTTON || vk == VK_RBUTTON) continue;   // clicking is not binding
            // PanelKeyDown, NOT GetAsyncKeyState. The keys this panel uses are
            // swallowed by the hook before the async state ever sees them, so
            // asking Windows reported ENTER as already released while it was
            // still held down - which armed the capture instantly and let the
            // very key that opened it bind itself.
            if (!PanelKeyDown(vk)) continue;
            anyDown = true;
            if (g_capture == 2 && !hit) hit = vk;
        }
        // THE PAD IS NOT READ HERE. It is captured in ConsumePad, on that slot's
        // own live state. Doing it in both places meant two racing capture paths,
        // and this one works off a cached value that can be a poll out of date -
        // enough for the button that opened the capture to be taken as the new
        // binding. One owner per device.
        const WORD pad = g_overlay.PadButtons();
        if (pad) anyDown = true;             // still counts as "not released yet"
        if (g_capture == 1 && !anyDown) {
            g_capture = 2;                               // everything up: arm
            COTW_LOG("[bind] armed - press a key or a pad button");
        }
        // Report what we can actually SEE while armed, once a second. A bind
        // that refuses pad buttons looks identical whether the pad never reaches
        // this DLL at all or reaches it and is not recognised - and those need
        // opposite fixes. This says which, without another round of guessing.
        if (g_capture == 2) {
            static DWORD lastSaid = 0;
            const DWORD now2 = GetTickCount();
            if (now2 - lastSaid > 1000) {
                lastSaid = now2;
                COTW_LOG("[bind] listening: pad buttons = 0x%04X, keyboard = %s",
                         pad, hit ? "something down" : "nothing down");
            }
        }
        if (g_capture == 2 && hit) {
            if (hit == VK_ESCAPE) {                      // cancel, keep the old one
                audio::Play(audio::Cue::Limit);
            } else {
                int n2 = 0;
                Row* rr = Rows(&n2);
                if (m_sel >= 0 && m_sel < n2 && rr[m_sel].kind == RK::Key) {
                    *static_cast<int*>(rr[m_sel].ptr) = hit;
                    audio::Play(audio::Cue::On);
                }
            }
            g_capture = 0;
        }
        // REPAINT BEFORE LEAVING. This block returns early to swallow input, and
        // the redraw check lives at the BOTTOM of Tick - so while it was
        // listening the panel never repainted and the "PRESS ANY KEY OR BUTTON"
        // prompt never appeared on screen. The row was in capture mode and
        // looked exactly as it had a moment earlier, which reads as "nothing
        // happened" rather than "waiting for you".
        const uint64_t capSig = Signature();
        if (capSig != m_lastSignature) {
            m_lastSignature = capSig;
            Redraw();
            m_dirty = true;
        }
        return;                          // swallow everything while listening
    }

    if (pressedOnce(VK_INSERT, false)) Toggle();
    if (pressedOnce('O', true)) Toggle();      // Ctrl+Alt+O kept as an alternative
    // The flat screen, so the game's menus can be reached without opening this
    // panel first. Rebindable, and edge-triggered like every other toggle here.
    {
        // Edge-triggered by hand: the binding may be a pad button, which
        // pressedOnce() cannot see because it only asks the keyboard.
        static bool wasDown = false;
        const bool down = BindingDown(Cfg().menu_screen_key);
        if (down && !wasDown) Cfg().menu_screen = !Cfg().menu_screen;
        wasDown = down;
    }

    if (m_visible) {
        if (pressedOnce(VK_PRIOR, false)) MoveTab(-1);      // Page Up
        if (pressedOnce(VK_NEXT, false)) MoveTab(1);        // Page Down
        if (pressedOnce(VK_TAB, false)) MoveTab(1);
        if (pressed(VK_UP, false)) MoveSelection(-1);
        if (pressed(VK_DOWN, false)) MoveSelection(1);
        if (pressed(VK_LEFT, false)) Adjust(-1);
        if (pressed(VK_RIGHT, false)) Adjust(1);
        if (pressedOnce(VK_RETURN, false)) Activate();
        if (pressedOnce(VK_ESCAPE, false)) Toggle();
    }

    const uint64_t sig = Signature();
    if (sig != m_lastSignature) {
        m_lastSignature = sig;
        Redraw();
        m_dirty = true;
    }
}

void Overlay::Redraw() {
    const int W = m_width, H = m_height;

    // Shadow colour underneath everything; the mask decides where it shows.
    for (int i = 0; i < W * H; ++i) m_dib[i] = 0x000000;

    const float cx = W * 0.5f, cy = H * 0.5f;
    const float hw = W * 0.5f - 26.0f, hh = H * 0.5f - 26.0f;
    const float rad = 26.0f;

    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const float d = SdRoundRect(x + 0.5f, y + 0.5f, cx, cy, hw, hh, rad);
            float aPanel = 0.5f - d;
            aPanel = aPanel < 0 ? 0 : (aPanel > 1 ? 1 : aPanel);
            // Soft drop shadow spreading outward from the panel edge.
            float aShadow = 1.0f - (d / 26.0f);
            aShadow = aShadow < 0 ? 0 : (aShadow > 1 ? 1 : aShadow);
            aShadow *= 0.45f;

            const float a = aPanel > aShadow ? aPanel * 0.97f : aShadow;
            m_mask[y * W + x] = uint8_t(a * 255.0f + 0.5f);

            if (aPanel > 0.0f) {
                // Vertical gradient for the panel body.
                const float t = float(y) / float(H);
                const uint32_t bg = Blend(kPanelTop, kPanelBot, t);
                m_dib[y * W + x] = Blend(m_dib[y * W + x], bg, aPanel);
            }
        }
    }

    auto fillRect = [&](int x, int y, int w, int h, uint32_t col, float a) {
        for (int j = y; j < y + h; ++j) {
            if (j < 0 || j >= H) continue;
            for (int i = x; i < x + w; ++i) {
                if (i < 0 || i >= W) continue;
                m_dib[j * W + i] = Blend(m_dib[j * W + i], col, a);
            }
        }
    };

    // `h` bounds the box so wrapped text cannot run past it into whatever is
    // drawn below - which is exactly how the help text ended up overlapping the
    // footer once the rows got longer.
    auto textBox = [&](const wchar_t* s, int x, int y, int w, int h, HFONT f,
                       uint32_t col, UINT fmt) {
        SelectObject(m_dc, f);
        SetTextColor(m_dc, RGB((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF));
        RECT rc{x, y, x + w, y + h};
        DrawTextW(m_dc, s, -1, &rc, fmt | DT_NOPREFIX);
    };
    auto text = [&](const wchar_t* s, int x, int y, int w, HFONT f, uint32_t col,
                    UINT fmt) { textBox(s, x, y, w, 40, f, col, fmt); };

    const int left = 62;
    const int right = W - 62;

    // --- header ---
    text(L"theHunter: Call of the Wild", left, 44, 700, m_fontTitle, kText, DT_LEFT);
    text(L"VIRTUAL REALITY", left, 88, 700, m_fontSmall, kAccent, DT_LEFT);
    // The author's name, opposite the title on the same line, in the dim colour
    // the panel uses for things that are true rather than things you can act
    // on. Right-aligned so it cannot collide with the title however long the
    // title gets, and on the header line so it is present on every tab without
    // taking a row from any of them.
    text(L"a VR mod by Vaas993", right - 700, 52, 700, m_fontSmall, kTextDim, DT_RIGHT);
    fillRect(left, 122, right - left, 2, kRule, 1.0f);

    // --- rows ---
    // The list is longer than the panel, so it scrolls. Every entry is the same
    // height, headings included, which keeps the scrolling arithmetic honest -
    // variable heights are how a list ends up drawing past its own bottom.
    int n = 0;
    Row* rows = Rows(&n);

    // --- tab bar ---
    SyncTabToSelection();
    int tabCount = 0;
    const TabInfo* tabs = Tabs(&tabCount);

    const int tabTop = 134;
    const int tabH = 40;
    {
        int tx = left;
        for (int t = 0; t < tabCount; ++t) {
            wchar_t wname[64];
            ToWide(tabs[t].name, wname, 64);
            SelectObject(m_dc, m_fontSmall);
            RECT meas{0, 0, 0, 0};
            DrawTextW(m_dc, wname, -1, &meas, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
            const int tw = (meas.right - meas.left) + 34;
            const bool on = (t == m_tab);
            if (on) {
                fillRect(tx, tabTop, tw, tabH, kAccent, 0.18f);
                fillRect(tx, tabTop + tabH - 3, tw, 3, kAccent, 1.0f);
            }
            text(wname, tx + 17, tabTop + 10, tw, m_fontSmall, on ? kAccent : kTextDim,
                 DT_LEFT);
            tx += tw + 6;
            if (tx > right - 120) break;      // never run off the panel
        }
    }
    fillRect(left, tabTop + tabH, right - left, 2, kRule, 1.0f);

    // --- rows of the CURRENT TAB only ---
    const int rowH = 34;
    const int listTop = tabTop + tabH + 16;
    // The vertical budget, in ONE place. The list stops where the help block
    // begins, and the help block is sized to hold the longest description
    // rather than whatever was left over - the old 96 px box ellipsised the
    // longer entries, and the help is where a setting explains itself.
    const int kHelpH = 156;                  // about six lines at this font
    // Default line + controls hint, PLUS a margin at the very bottom.
    //
    // The hint used to end 14 px from the edge of the panel. On a monitor that
    // reads as tight; in a headset it reads as unreadable - the outer edge of a
    // quad is the worst place on it for small text, and that row is the only
    // thing telling a new player which buttons work. It now ends 30 px clear.
    const int kFooterH = 104;
    const int kHintBottomGap = 30;
    const int helpTop = H - kFooterH - kHelpH;
    const int listBottom = helpTop - 30;     // the rule sits in that gap
    const int visible = (listBottom - listTop) / rowH;

    const int tabFirst = (m_tab < tabCount) ? tabs[m_tab].first : 0;
    const int tabLast  = (m_tab < tabCount) ? tabs[m_tab].last : n;
    const int tabRows  = tabLast - tabFirst;

    // *** THE CURSOR MUST NOT BE LEFT STANDING ON A ROW THAT VANISHED. ***
    //
    // The selection can be sitting on a row that a change elsewhere - or a
    // profile load - has just made inapplicable. Walk it back to the nearest
    // row that is still there, or forward if it was the first.
    if (m_sel >= 0 && m_sel < n && IsHiddenRow(rows[m_sel])) {
        int j = m_sel;
        while (j > tabFirst && IsLabelRow(rows[j])) --j;
        if (j <= tabFirst || IsLabelRow(rows[j])) {
            j = m_sel;
            while (j < tabLast - 1 && IsLabelRow(rows[j])) ++j;
        }
        if (j >= tabFirst && j < tabLast && !IsLabelRow(rows[j])) m_sel = j;
    }

    // Scrolling is now relative to the tab, not the whole list.
    const int selRel = m_sel - tabFirst;
    if (selRel < m_scroll + 1) m_scroll = selRel - 1;
    if (selRel > m_scroll + visible - 2) m_scroll = selRel - visible + 2;
    if (m_scroll > tabRows - visible) m_scroll = tabRows - visible;
    if (m_scroll < 0) m_scroll = 0;

    int y = listTop;
    for (int k = m_scroll; k < tabRows && k < m_scroll + visible; ++k) {
        const int i = tabFirst + k;
        if (i < 0 || i >= n) break;
        const Row& r = rows[i];
        wchar_t wlabel[160];
        ToWide(r.label, wlabel, 160);

        if (IsHeading(r)) continue;   // headings ARE the tabs now
        // Not applicable in the current mode: no row, no gap, no cursor stop.
        if (IsHiddenRow(r)) continue;

        // A section label inside the tab: the text in the accent colour with a
        // rule beside it, no value column, and the cursor passes straight over.
        if (IsGroup(r)) {
            RECT gr{left, y + 4, right, y + rowH};
            SetTextColor(m_dc, RGB((kAccent >> 16) & 0xFF, (kAccent >> 8) & 0xFF,
                                   kAccent & 0xFF));
            SelectObject(m_dc, m_fontRow);
            DrawTextW(m_dc, wlabel, -1, &gr, DT_LEFT | DT_SINGLELINE | DT_TOP);
            SIZE ts{};
            GetTextExtentPoint32W(m_dc, wlabel, (int)wcslen(wlabel), &ts);
            const int lineX = left + ts.cx + 14;
            if (lineX < right) {
                fillRect(lineX, y + rowH / 2 + 2, right - lineX, 1, kAccent, 0.35f);
            }
            y += rowH;
            continue;
        }

        const bool sel = (i == m_sel);
        if (sel) {
            fillRect(left - 22, y - 3, right - left + 44, rowH - 2, kAccent, 0.14f);
            fillRect(left - 22, y - 3, 4, rowH - 2, kAccent, 1.0f);
        }

        // *** THE LABEL AND THE VALUE MUST NOT SHARE PIXELS. ***
        //
        // They did. The label box was x=62 w=520, ending at 582, while the value
        // box began at right-400 = 562 - twenty pixels of overlap, so any long
        // label ran underneath its own value and both became unreadable. Both
        // columns are now derived from ONE width, so they cannot drift apart
        // again, with a real gap between them.
        //
        // Single line and ellipsised: silently clipping a label mid-letter reads
        // as a rendering fault, while an ellipsis reads as what it is and says
        // there is more to the name.
        char val[96];
        FormatValue(r, val, sizeof(val));
        wchar_t wval[96];
        ToWide(val, wval, 96);

        // *** THE VALUE COLUMN IS AS WIDE AS THE VALUE NEEDS. ***
        //
        // It was a fixed 340 px, sized for "PRESS ANY KEY OR BUTTON", and the
        // rows that report two resolutions were cut off mid-number:
        // "Quality 67% - 2058x2224 -> 3072x..." tells the player less than
        // nothing. The column now measures its own text and takes what it needs
        // up to 62% of the row, leaving the label - which is ellipsised anyway,
        // and short on exactly the rows whose values are long - the rest.
        const int colGap = 26;
        SelectObject(m_dc, m_fontRow);
        SIZE vsz{};
        GetTextExtentPoint32W(m_dc, wval, (int)wcslen(wval), &vsz);
        int valW = vsz.cx + 12;
        if (valW < 340) valW = 340;
        const int valMax = ((right - left) * 62) / 100;
        if (valW > valMax) valW = valMax;
        const int labelW = (right - left) - valW - colGap;
        const UINT oneLine = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS;

        textBox(wlabel, left, y, labelW, rowH - 4, m_fontRow,
                sel ? kText : kTextDim, DT_LEFT | oneLine);

        uint32_t vcol = sel ? kAccent : kTextDim;
        if (r.kind == RK::Bool) vcol = *static_cast<bool*>(r.ptr) ? kOn : kOff;
        // While a binding is being captured the prompt IS the value, and it has
        // to read as an instruction rather than as a setting - so it takes the
        // accent colour whether or not its row happens to be selected.
        if (r.kind == RK::Key && val[0] == 'P' && val[1] == 'R') vcol = kAccent;
        textBox(wval, right - valW, y, valW, rowH - 4, m_fontRow, vcol,
                DT_RIGHT | oneLine);

        y += rowH;
    }

    // Scroll indicators, so it is obvious there is more than fits.
    if (m_scroll > 0) {
        text(L"▲  more above", right - 220, listTop - 26, 220, m_fontSmall,
             kAccentDim, DT_RIGHT);
    }
    if (m_scroll + visible < tabRows) {
        text(L"▼  more below", right - 220, listBottom + 4, 220, m_fontSmall,
             kAccentDim, DT_RIGHT);
    }

    // --- help for the selected row ---
    fillRect(left, helpTop - 20, right - left, 2, kRule, 1.0f);

    if (m_sel >= 0 && m_sel < n) {
        const Row& r = rows[m_sel];
        // 2048, against a longest help text of 981 - and ToWide truncates
        // safely rather than failing if one ever outgrows even this.
        wchar_t whelp[2048];
        // While a key row is listening, the help area says what to do rather
        // than describing the setting - at that moment the instruction IS the
        // useful text, and the description can wait until it is bound.
        if (r.kind == RK::Key && g_capture) {
            ToWide("PRESS ANY KEY OR CONTROLLER BUTTON to bind it to this setting."
                   "        ESC cancels and keeps the current one.",
                   whelp, 2048);
        } else {
            ToWide(r.help, whelp, 2048);
        }
        // Clipped to its own box, and ellipsised rather than overflowing.
        textBox(whelp, left, helpTop, right - left, kHelpH, m_fontHelp, kTextDim,
                DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

        if (r.def && r.def[0]) {
            char defbuf[64];
            snprintf(defbuf, sizeof(defbuf), "default: %s", r.def);
            wchar_t wdef[64];
            ToWide(defbuf, wdef, 64);
            textBox(wdef, left, H - kHintBottomGap - 26 - 30, 400, 26,
                    m_fontSmall, kAccentDim, DT_LEFT);
        }
    }

    // --- controls hint ---
    textBox(L"LB / RB  or  PgUp / PgDn   change tab     •     D-pad / stick  move"
            L"     •     left / right  change     •     A  toggle     •     B or Insert"
            L"  close",
            left, H - kHintBottomGap - 26, right - left, 26,
            m_fontSmall, kTextDim, DT_LEFT);

    // --- compose: GDI wrote opaque RGB, the mask carries the shape, and the
    // result must be premultiplied for the compositor.
    for (int i = 0; i < W * H; ++i) {
        const uint32_t c = m_dib[i];
        const uint32_t a = m_mask[i];
        const uint32_t rr = (((c >> 16) & 0xFF) * a) / 255;
        const uint32_t gg = (((c >> 8) & 0xFF) * a) / 255;
        const uint32_t bb = ((c & 0xFF) * a) / 255;
        m_pixels[i] = (a << 24) | (rr << 16) | (gg << 8) | bb;
    }
}

}  // namespace cotwvr
