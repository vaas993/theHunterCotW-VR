"""
theHunter: Call of the Wild VR - settings launcher

Sits beside the game exe. Edits the MOD's own settings - and nothing else - then
starts the game.

WHY IT EXISTS. Several settings only take effect at startup - the render
resolution and the field of view are read once, before the world is built - so
changing them from the in-game panel means quitting and relaunching anyway. And
they are far easier to judge on a monitor than through a headset. So the things
you set once, up front, live here; the things you tune while playing stay in the
in-game panel.

ONE FILE, EDITED IN PLACE.
  cotwvr.ini      beside the game exe - the mod's settings

It is edited by REPLACING THE VALUE and leaving every other byte alone, the same
way the mod's own C++ does it (gamesettings.cpp) and for the same reason: the ini
carries a page of explanatory comments worth keeping.

THE GAME'S OWN settings.json IS NOT TOUCHED HERE, ON PURPOSE. It used to be, and
it never held: the game rewrites that file from memory every time it quits, and
regenerates it outright on the first run after a reinstall, so anything the
launcher wrote between sessions lasted until the next quit and no longer - which
reads from the outside as "the launcher did nothing". The MOD still writes the
resolution, the FOV and the screen-space effects into it at every launch from
DllMain, which is the only moment that survives; the launcher's copy of that job
was redundant. What is left of it is a read-only list on the page: the graphics
settings worth setting once, in the game's own video menu.
"""

import os
import re
import subprocess
import sys
import tkinter as tk
from tkinter import ttk, messagebox

APP_ID = "518790"
TITLE = "theHunter: Call of the Wild - VR settings"
AUTHOR = "Vaas993"
VERSION = "1.1.1"
# The game build this was written against. NOT enforced anywhere - the mod finds
# its addresses by fingerprint and says so in the log if it cannot - it is here
# so a player on a later update knows which fact to check first.
GAME_BUILD = "9.2 (Peru Hunting Reserve)"


# --------------------------------------------------------------------------
# THE FIRST-RUN PAGE. Every line is something that costs an evening to find out
# on your own, and nothing that can be found by looking at the window.
#
# The same points the in-game panel opens with, deliberately: a player who
# dismissed one should not meet a different set of facts in the other.
# --------------------------------------------------------------------------

WELCOME_POINTS = [
    ("Put the headset on first",
     "The mod needs an OpenXR runtime already running - Meta Link / Quest Link, "
     "SteamVR or Virtual Desktop. Start that, then launch the game."),
    ("Set the game's field of view to 90",
     "In the game's own video menu. It is a VERTICAL number and 90 is its "
     "maximum; anything lower and the mod is handed a narrower picture than "
     "your headset can show, which reads as looking down a tube."),
    ("DLSS needs an NVIDIA RTX card",
     "It is NVIDIA's own reconstruction and runs on nothing else. On any other "
     "card, switch DLSS off in the launcher - everything else in the mod works "
     "the same, and the mod's own anti-aliasing pass takes over."),
    ("DLSS needs FXAA + TAA in the game's video menu",
     "This one is mandatory, and only for DLSS: the mod's picture pass replaces "
     "the game's anti-aliasing, so if the game is set to anything else there is "
     "nothing for it to replace and DLSS never runs. Everything else in the mod "
     "works whatever you choose there."),
    ("Two keys worth knowing",
     "PAUSE recentres the view - use it whenever forward stops being forward. "
     "DELETE brings up the flat game screen, for menus and the map. "
     "Both can be changed in the in-game panel."),
    ("The settings that ship with it were tested on a Quest 3",
     "If a session of experimenting leaves the picture wrong, press \"Use "
     "recommended\" here or \"Use the tested settings\" in the in-game panel, "
     "and you are back to a configuration known to work."),
    ("Single player only",
     "Do not use this in multiplayer sessions. It changes how the game renders "
     "and reads memory to do so."),
    ("Built for game update %s" % GAME_BUILD,
     "The mod recognises the game by fingerprint. On a different update it will "
     "say so in its log and start nothing, rather than guess - so a game update "
     "means waiting for a mod update, not a broken installation."),
    ("It is a work in progress",
     "Expect rough edges. The known ones are listed at the bottom of this "
     "window - scroll down and read them before deciding something is broken. "
     "The log folder button is where a bug report starts."),
]


# --------------------------------------------------------------------------
# Locating things
# --------------------------------------------------------------------------

def here():
    """The folder this exe/script sits in - which is the game folder."""
    if getattr(sys, "frozen", False):
        return os.path.dirname(sys.executable)
    return os.path.dirname(os.path.abspath(__file__))


def notice_path():
    """Beside the ini, but NOT in it.

    cotwvr.ini belongs to the mod: the C++ rewrites it from its own fields, so a
    key it does not know is dropped the next time the game runs. A launcher-only
    preference kept there would be forgotten on the next launch and the dialog
    would come back - precisely what the player asked it not to do.
    """
    return os.path.join(here(), "cotwvr_launcher.cfg")


def _pref(name):
    try:
        with open(notice_path(), "r", encoding="utf-8", errors="replace") as f:
            return ("%s=0" % name) in f.read().replace(" ", "")
    except OSError:
        return False


def _set_pref(name):
    """Add one line, keeping whatever is already there.

    The first version of this REPLACED the file, so dismissing the welcome page
    un-dismissed the save notice and vice versa - two switches sharing one file
    and each forgetting the other.
    """
    lines = []
    try:
        with open(notice_path(), "r", encoding="utf-8", errors="replace") as f:
            lines = [ln.rstrip(chr(13) + chr(10)) for ln in f
                     if ln.strip() and not ln.startswith("#")
                     and not ln.replace(" ", "").startswith(name + "=")]
    except OSError:
        pass
    lines.append("%s = 0" % name)
    try:
        with open(notice_path(), "w", encoding="utf-8", newline="") as f:
            f.write("# launcher-only preferences. Safe to delete." + chr(10))
            f.write(chr(10).join(lines) + chr(10))
    except OSError:
        pass


def welcome_dismissed():
    return _pref("welcome")


def dismiss_welcome():
    _set_pref("welcome")


def notice_dismissed():
    try:
        with open(notice_path(), "r", encoding="utf-8", errors="replace") as f:
            return "save_notice=0" in f.read().replace(" ", "")
    except OSError:
        return False


def dismiss_notice():
    _set_pref("save_notice")


def steam_running():
    """Is the Steam client up?

    Asked with tasklist rather than the registry: Steam leaves its last PID in
    HKCU\Software\Valve\Steam\ActiveProcess even after it has gone, so that
    key answers "was Steam running" and this question is "is it running now".
    CREATE_NO_WINDOW because a console flashing over a settings window looks
    like something went wrong.
    """
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq steam.exe", "/NH"],
            capture_output=True, text=True, timeout=10,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
        return "steam.exe" in (out or "").lower()
    except (OSError, subprocess.SubprocessError):
        # Cannot tell - say no, which routes through steam:// and works either
        # way. Guessing yes ends in the DRM dialog.
        return False


def process_running(image):
    """Is a process with this exe name up? Same question as steam_running()."""
    try:
        out = subprocess.run(
            ["tasklist", "/FI", "IMAGENAME eq %s" % image, "/NH"],
            capture_output=True, text=True, timeout=10,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)).stdout
        return image.lower() in (out or "").lower()
    except (OSError, subprocess.SubprocessError):
        return False


def ini_path():
    return os.path.join(here(), "cotwvr.ini")


# The three helpers that used to sit here - documents_dir(), active_steam_id64()
# and settings_json_path() - have been deleted along with the writes they served.
# They existed only to find the game's settings.json, and nothing in this file
# opens that file any more. Code that still knows how to find a file it must not
# write is how the write comes back.


# --------------------------------------------------------------------------
# Reading and writing, without disturbing anything else
# --------------------------------------------------------------------------

def read_ini(path):
    out = {}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    k, v = line.split("=", 1)
                    out[k.strip()] = v.strip()
    except OSError:
        pass
    return out


def write_ini(path, changes):
    """Replace values in place. Comments, order and unknown keys are untouched."""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return False, "could not read cotwvr.ini - launch the game once first"

    for key, value in changes.items():
        pat = re.compile(r"(?m)^(\s*%s\s*=\s*).*$" % re.escape(key))
        if pat.search(text):
            text = pat.sub(lambda m: m.group(1) + str(value), text, count=1)
        else:
            text = text.rstrip("\r\n") + "\n%s = %s\n" % (key, value)
    try:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(text)
    except OSError as e:
        return False, str(e)
    return True, ""


# read_game_json() and write_game_json() lived here. Both are gone. There is now
# no code path in this launcher that opens the game's settings.json at all - for
# reading or for writing. Any settings.json.vrlauncher_backup left on disk by an
# older build is left exactly where it is; it costs nothing and it is the only
# record of what the file looked like before.


# --------------------------------------------------------------------------
# What the launcher exposes
#
# Deliberately NOT all 139 mod settings. The ones here are those that either
# only take effect at startup, or are simply easier to judge on a monitor. The
# in-game panel keeps everything else, including the whole ADVANCED tab.
# --------------------------------------------------------------------------

RECOMMENDED = {
    # *** THE SAME SET THE IN-GAME PANEL APPLIES, PLUS WHAT ONLY EXISTS HERE. ***
    #
    # The mod's own "Use the tested settings" (overlay.cpp, RecommendAct) and
    # this table are the two places a recommendation can come from, and two
    # recommendations that disagree is worse than either alone: a player who
    # presses one and then the other gets a configuration that was never tested.
    # tools/checkrecommend.py compares the two at build time and fails the build
    # if they drift, so every value below that has a counterpart in the panel is
    # the SAME value.
    #
    # Tested on a Quest 3. The entries that carry a real argument:
    #
    #   render_preset     the eye is near-square (measured 0.925:1 from the
    #                     runtime's own tangents), so a wide render throws its
    #                     pixels away off the sides.
    #   tier1_*           the game draws wider than normal and each eye is given
    #                     its own slice of it, which recovers the ~22 deg of
    #                     height the headset can show and the game was not.
    #   dlss_*            DLAA, not upscaling: the picture is reconstructed at
    #                     full size on every machine, and trading pixels for
    #                     frames is a choice each graphics card makes for itself.
    #
    # Applied to the boxes, NOT written straight to disk - you see what changed
    # and press Save yourself.

    "enabled": "1",
    "head_tracking": "1",
    "stereo": "1",
    "full_rate_stereo": "1",
    "ipd_mm": "64",
    "render_preset": "32",
    "render_custom_w": "0",
    "render_custom_h": "0",
    # Automatic, from the headset's own vertical FOV. On a Quest 3 that is 99,
    # which the game clamps to its 90 maximum - so this reads 90 either way here,
    # and adapts on a headset that sees less.
    "force_game_fov": "0",
    "game_fov_deg": "100",       # the game's number is VERTICAL and caps at 90
    "fov_is_horizontal": "0",
    # 0, not 1. It reads the projection back off the GPU once a second to prove
    # the FOV lever reaches the renderer - worth having while that was in doubt,
    # not worth a readback per second now it is settled.
    "tier1_measure_fov": "0",
    "tier1_fov": "1",
    "tier1_fov_k_override": "1.200",
    "tier1_per_eye_crop": "0",
    # The picture. taa_replace_pass is not optional under DLSS - it is the pass
    # that writes the motion vectors DLSS reconstructs from - so the two are one
    # control on this page and one control in the panel.
    "taa_replace_pass": "1",
    "dlss_enable": "1",
    "dlss_preset": "2",         # M: the model that does not shimmer in this game
    "dlss_quality": "2",        # DLAA - upscaling is opt-in per machine
    "dlss_upscale_pct": "100",
    "post_sharpen": "0.35",
    "post_saturation": "1.10",
    # Movement.
    "six_dof": "0",
    "six_dof_frame_mode": "1",  # measured: the basis turns with the head
    "six_dof_scale": "1.00",
    "logging": "1",
}


# THE GAME'S OWN GRAPHICS - RECOMMENDED, NOT WRITTEN FROM HERE.
#
# The same verified setup, and the two entries that carry the whole argument:
#
#   SSAO / SSR / contact shadows
#                     all reuse the previous frame. Under full-rate stereo that
#                     frame was drawn from the OTHER eye, so the history they
#                     blend against never matches: flicker, and ghosting around
#                     trees.
#   GraphicsAA 3      FXAA + TAA, and mandatory for DLSS: the mod REPLACES the
#                     engine's temporal pass, and that is where DLSS gets its
#                     motion vectors. With FXAA alone there is no pass to
#                     replace and DLSS cannot run.
#
# These are settings.json keys, and this launcher does not write settings.json.
# They are here as the single source of the numbers shown on the page, so the
# list the player reads and the values the mod applies cannot drift apart.
RECOMMENDED_GAME_MENU = {
    "GraphicsSSAO": "0",
    "GraphicsSSReflection": "0",
    "GraphicsContactShadows": "0",
    "GraphicsAA": "3",
    "GraphicsTAASharpness": "40",
    "GraphicsMotionBlur": "0",
    "GraphicsDepthOfField": "0",
    "GraphicsVignette": "0",
    "GraphicsPostEffects": "0",
    "GraphicsTextureDetail": "3",
    "GraphicsLodFactor": "3",
    "GraphicsShadowsResolution": "2",
    "GraphicsWaterDetail": "3",
    "GraphicsTerrainTessellation": "3",
    "GraphicsVolumeFogQuality": "0",
    "GraphicsFurQuality": "3",
    "GraphicsGI": "0",
    "GraphicsHeatHaze": "0",
    "GraphicsAniso": "4",
    "DisplayVSync": "0",
    "DisplayFullscreen": "0",
    "GameFOV": "90",
}


RENDER_PRESETS = [
    # ONLY the shapes that still make sense are LISTED - the rest are hidden,
    # not deleted.
    #
    # The old 1:1 / 5:4 / 4:3 / 16:10 / 16:9 / HEADSET / FOV90 families date from
    # when both eyes were shown the same centred image, so the render had to be
    # the shape of the headset's frustum. The per-eye crop measures the real FOV
    # at runtime and takes each eye's own rectangle, so shape is no longer a
    # choice: only size, and whether the render covers both eyes.
    #
    # They are HIDDEN rather than removed because the number here IS the index
    # into kPresets, and deleting rows renumbers every entry after them - the
    # off-by-one that already sent 2880x3112 to the game as 3072x3320 once.
    # Anyone with an old value saved still gets exactly what they picked.
    ("-1", "CUSTOM"),
    ("0", "leave the game alone"),
    ("31", "FULLVIEW 1784x1928  3.4 MP"),
    ("32", "FULLVIEW 2224x2400  5.3 MP"),
    ("33", "FULLVIEW 2560x2768  7.1 MP"),
    ("34", "FULLVIEW 2880x3112  9.0 MP  *"),
    ("35", "FULLVIEW 3072x3320  10.2 MP"),
    ("36", "FULLVIEW 3456x3736  12.9 MP"),
    ("37", "NATIVE 3936x4072  16.0 MP"),
]

ON_OFF = [("1", "on"), ("0", "off")]

# *** THE WHOLE MOD, ON OR OFF, WITHOUT DELETING ANYTHING. ***
#
# This row already existed as "Mod enabled" with a bare on/off beside it, and it
# was not recognised as the way to play the game normally again - which is a
# fair reading of a label that names a switch rather than an outcome. Off is a
# genuine stock game: cotwvr.ini is read, the mod sees this, and returns before
# it installs a hook, applies a graphics setting or touches the game in any way.
# Every file stays where it is, and this same row turns it back on.
MOD_ENABLED = [
    ("1", "on  -  play in VR"),
    ("0", "off  -  play the game normally"),
]

# Not on/off - the setting picks between two ways of drawing stereo, and "off"
# does not mean "no stereo", it means the alternate-eye path. Naming it on/off
# hid the actual choice.
# NOT on/off either, and for the same reason. "Lean and move your body" with a
# yes/no beside it never says WHICH state is which - and the two states have
# names every VR player already knows. Word for word the same as the in-game
# panel's TRACKING MODE row (overlay.cpp, kTrackingModeNames): a player who
# reads one and then the other must not have to work out that they mean the
# same thing.
TRACKING_MODE = [
    ("0", "3DoF  -  looking only"),
    ("1", "6DoF  -  looking and moving"),
]

STEREO_MODE = [
    ("1", "Full rate  -  both eyes, same instant"),
    ("0", "AER  -  alternate eyes, full framerate"),
]

# DLSS quality, NVIDIA's own ladder and their own per-axis ratios. The quality
# level decides how much of the picture the game actually draws, so it is read
# once at startup: changing it means a restart either way, which is precisely
# what this window is for.
DLSS_QUALITY = [
    ("0", "DLAA  -  full size, best picture"),
    ("1", "Quality  -  67%"),
    ("2", "Balanced  -  58%"),
    ("3", "Performance  -  50%"),
    ("4", "Ultra Performance  -  33%"),
    ("5", "Custom  -  the percentage below"),
]

# The reconstruction model. NVIDIA ship several and they are not
# interchangeable: two of them are visibly wrong in this game, which is a fact
# about the game's motion vectors rather than about the model.
#
# JUST THE NAMES, AND NO ADVICE. A reason repeated on all five lines is a reason
# read on none of them - the list becomes a paragraph you scroll rather than four
# letters you choose between. Which model suits a given machine and a given scene
# is not something this window can know, so it does not claim to: the shipped
# default is a choice, not a recommendation printed beside every option.
DLSS_MODEL = [
    ("4", "M"),
    ("3", "L"),
    ("2", "K"),
    ("1", "J"),
    ("0", "default"),
]

# *** THREE WAYS TO SMOOTH THE PICTURE, NOT A DLSS SWITCH. ***
#
# This was an on/off, and off did more than it said: the launcher wrote
# taa_replace_pass alongside it, so turning DLSS off ALSO turned off the mod's
# own per-eye pass and dropped the player onto the game's own temporal
# anti-aliasing - which is the one that ghosts in stereo, because each eye's
# history is the OTHER eye's picture. Anybody without an RTX card met that by
# following the obvious instruction.
#
# The in-game panel has offered these three for a while (overlay.cpp, AaModeAct
# / AaModeText); this is the same choice with the same words, so the two windows
# describe one setting rather than two.
#
# The value is a UI mode, not an ini key. _collect() turns it into the three
# keys that actually exist - including the game's own anti-aliasing, which has
# to follow: the mod's pass REPLACES the engine's temporal one, so it needs
# FXAA+TAA to exist, while "off" wants FXAA alone precisely because the engine's
# TAA is what ghosts.
AA_MODE = [
    ("2", "NVIDIA DLSS  -  RTX cards only"),
    ("1", "Per-eye smoothing (mod)  -  any card"),
    ("0", "Off  -  the game's own"),
]

MOD_ROWS = [
    ("VR", [
        ("enabled", "VR mod", MOD_ENABLED,
         "off = a stock game with the files still installed; "
         "turn it back on here"),
        ("head_tracking", "Head tracking", ON_OFF, ""),
        ("stereo", "Stereo 3D", ON_OFF, ""),
        ("full_rate_stereo", "Stereo mode", STEREO_MODE,
         ""),
        ("six_dof", "Head tracking mode", TRACKING_MODE,
         "6DoF adds leaning and stepping"),
        ("ipd_mm", "Eye separation (mm)", None, ""),
    ]),
    ("Resolution", [
        ("render_preset", "Render resolution", RENDER_PRESETS,
         "next launch"),
        # Shown only when the preset above is CUSTOM.
        ("render_custom_w", "Width", None, ""),
        ("render_custom_h", "Height", None, ""),
    ]),
    # *** ONE SWITCH, TWO INI KEYS. ***
    #
    # DLSS cannot run without the mod's own resolve pass: that pass is what
    # writes the motion vectors DLSS reconstructs from, and without it DLSS is
    # handed nothing and declines every frame. They were two separate switches,
    # which meant one of them could be off and the picture would look broken for
    # a reason nothing on the page explained. Saving this row writes both.
    ("Picture", [
        ("aa_mode", "Anti-aliasing", AA_MODE,
         "the mod sets the game's own to match"),
        ("dlss_quality", "Quality level", DLSS_QUALITY, "next launch"),
        # Shown only when the level above is Custom.
        ("dlss_upscale_pct", "Draw this % of each axis", None, ""),
        ("dlss_preset", "Model", DLSS_MODEL, ""),
    ]),
    ("Field of view", [
        # THE ONE ROW THAT DECIDES WHAT THE GAME IS TOLD.
        #
        # This was missing, and its absence was invisible: the row below reads
        # "Game field of view", so it looks like the setting that matters, but
        # the mod OVERWRITES that one at launch to match whatever it worked out
        # here (gamesettings.cpp, "the mod's layer FOV follows automatically").
        # There was no way to choose the number from this window at all.
        ("force_game_fov", "Field of view given to the game",
         [("0", "automatic - fill the headset"), ("70", "70"), ("75", "75"),
          ("80", "80"), ("85", "85"), ("90", "90 (the game's maximum)")],
         "vertical; written on Save"),
        # headset_vfov_deg is deliberately NOT here. It feeds the "automatic"
        # choice above and nothing else, it is a property of the hardware rather
        # than a preference, and a Quest 3's 99 is over the game's 90 cap so it
        # changes nothing on this machine. It stays in cotwvr.ini for anyone on a
        # headset that sees less than 90 degrees vertically.
        ("game_fov_deg", "How near the world sits", None,
         "raise = closer, lower = further"),
        ("tier1_fov", "Widen the view", ON_OFF,
         "draws wider than normal"),
        ("tier1_fov_k_override", "How much wider", None,
         "1.00 = off, try 1.30"),
        ("tier1_per_eye_crop", "Give each eye its own view", ON_OFF,
         "needs the two above"),
    ]),
]

# How the game's own video menu words each number. Used ONLY to turn a
# recommended value into the words the player will see on the screen in front of
# them - there is no widget behind any of these any more.
QUALITY4 = [("0", "off"), ("1", "low"), ("2", "medium"), ("3", "high")]
QUALITY3 = [("0", "low"), ("1", "medium"), ("2", "high")]
AA_MODES = [("0", "off"), ("1", "FXAA"), ("2", "TAA"), ("3", "FXAA + TAA")]
ANISO = [("0", "off"), ("2", "2x"), ("4", "4x"), ("8", "8x"), ("16", "16x")]

# Shown, but owned by the mod and rewritten by it at every launch: displayed
# greyed out and never written back from here. Empty at the moment.
#
# game_fov_deg used to be in here, and greying it out was wrong. It IS the most
# useful control the panel has - it sets how wide an angle the rendered image is
# claimed to cover, so raising it brings the world nearer and lowering it pushes
# it away. What made it look unsettable was the mod resetting it at every
# launch; that is now conditional on game_fov_follows_auto, which this window
# clears the moment you choose a value of your own.
READ_ONLY_ROWS = set()

# The graphics options the MOD re-applies at launch, and the ini key each one
# lives under. Only these seven: they are the ones that matter in VR and the ones
# worth defending against the game's own writeback (gamesettings.cpp, behind
# apply_game_graphics). Everything else in the game's video menu is the player's
# to keep, and nothing here writes it.
MOD_APPLIES_GRAPHICS = {
    "GraphicsMotionBlur":     "graphics_motion_blur",
    "GraphicsDepthOfField":   "graphics_depth_of_field",
    "GraphicsVignette":       "graphics_vignette",
    "GraphicsSSAO":           "graphics_ssao",
    "GraphicsSSReflection":   "graphics_ssr",
    "GraphicsContactShadows": "graphics_contact_shadows",
    # GraphicsAA IS here now, and leaving it out was the bug. The mod writes
    # this key whatever happens; the only question was WHICH value, and with
    # no entry the answer came from whatever the working ini held - which was
    # 1, FXAA alone, the one value that stops DLSS working at all.
    #
    # The game may still not keep it, which is why the video-menu list below
    # names anti-aliasing as well. Writing it is worth doing anyway: it can
    # only ever move the setting TOWARDS the one the mod needs.
    "GraphicsAA":             "graphics_aa",
}

# The same seven as MOD ini keys, so "Use recommended" can put them back.
#
# THE ONLY THING THAT SURVIVED FROM THE GAME-GRAPHICS TAB. The tab's widgets are
# gone and so is the settings.json write, but these seven ini values are what the
# mod feeds to the game at every launch - and they can drift: the copy of
# cotwvr.ini in the game folder today has graphics_aa = 3 where the recommended
# value is 1. Derived from the table above rather than typed out again, so the
# number the player reads on the page is by construction the number that ends up
# in the ini.
# THE REST OF THE TESTED CONFIGURATION - every setting that differs from the
# built-in default and has no row on this page.
#
# WHAT IS DELIBERATELY NOT HERE: diagnostics and probes, the cheats, and the
# address/CRC constants. A probe left on is not a recommendation, a frozen clock
# is a preference for one session, and the constants only look different because
# the ini prints them in another form.
#
# This is what makes "Use recommended" restore the configuration that was
# actually tested - the scope going single-axis, the weapon's depth and position, the
# head-write axes, the recentre ramp at 31 frames, the aim steadiness at 120 ms -
# rather than just the fifteen rows the window happens to show.
RECOMMENDED_MOD_EXTRA = {
    "aim_steady": "1",
    "aim_steady_ms": "120",
    "apply_game_graphics": "1",
    "desktop_present_unlocked": "1",
    # The DLSS wiring. None of these is a taste - each one states a fact about
    # how this engine hands its buffers over, and getting one wrong is a picture
    # that shimmers rather than a picture that looks different.
    "dlss_auto_exposure": "1",
    "dlss_depth_inverted": "1",
    "dlss_jitter_mode": "1",
    "dlss_mv_jittered": "1",
    "dlss_upscale_rect_latch": "1",
    "dlss_upscale_solo": "1",
    "eye_swap": "1",
    "fov_is_horizontal": "0",
    "frame_latency": "1",
    "freelook_key": "0x12",
    "full_rate_correct_delta": "0",
    "full_rate_freeze_clock": "0",
    "full_rate_freeze_time": "0",
    "graphics_contact_shadows": "0",
    "graphics_depth_of_field": "0",
    "graphics_motion_blur": "0",
    "graphics_ssao": "0",
    "graphics_ssr": "0",
    "graphics_vignette": "0",
    "head_block_reseed_yaw": "0",
    "head_keep_aim_on_reseed": "0",
    "head_matrix_yaw_invert": "1",
    "head_pitch_invert": "1",
    "head_roll_invert": "1",
    "head_rotate_lever_pitch": "1",
    # OFF. Injecting head roll into the game's own camera was what read as a
    # vibration in the headset; your head still rolls, the game's camera is
    # simply not asked to roll with it.
    "head_tremor_level": "0",
    "head_write_pitch": "1",
    "head_write_roll": "1",
    "head_write_yaw": "1",
    # Nothing filters the head pose. Every attempt to smooth it - damping,
    # tremor levels, prediction - made the world less steady, not more, because
    # a view that lags the head by any amount at all is a view that swims.
    "prediction_damp_pct": "0",
    "six_dof_anchor_on_enter": "1",
    "taa_single_accumulation": "0",
    "hook_context_vtable": "0",
    "hook_device_creation": "1",
    "lock_pitch_input": "0",
    "menu_screen_distance_m": "1.80",
    "menu_screen_log_signals": "0",
    "menu_screen_key": "0x2E",
    "rebuild_contexts_on_start": "0",
    "recentre_key": "0x13",
    "recentre_pad_chord": "0xC0",
    "recentre_ramp_frames": "31",
    "scope_mono": "1",
    "submit_rendered_pose": "1",
    # ON. The gun has real depth and sits in your hands rather than reading as
    # infinitely far away. What it costs is in KNOWN_ISSUES: it feeds a second
    # camera into the resolve's choice of which matrix drew the frame, so the
    # WORLD can pick up a shake on some scenes. One row in the panel turns it
    # off for anyone who would rather have the steadier picture.
    "weapon_3d": "1",
    "weapon_3d_amount": "0.1300",
    "weapon_3d_amount_scoped": "0.0000",
    "weapon_3d_glass": "24",
    "weapon_3d_glass_follow_body_eye": "0",
    "weapon_3d_skip_scene_picture": "1",
    "weapon_cb_depth": "1.75",
    "weapon_distance_m": "0.400",
    "weapon_pass_tail_pct": "0",
    "weapon_pos_x": "0.500",
    "weapon_pos_y": "0.040",
    "weapon_pos_z": "0.500",
    "weapon_view_scale": "0.500",
}


RECOMMENDED_MOD_GRAPHICS = dict(
    [("apply_game_graphics", "1")] +
    [(ini_key, RECOMMENDED_GAME_MENU[game_key])
     for game_key, ini_key in MOD_APPLIES_GRAPHICS.items()])


# --------------------------------------------------------------------------
# WHAT TO SET IN THE GAME'S OWN VIDEO MENU - a list to read, not controls.
#
# These were editable rows on a second tab until the launcher stopped writing
# settings.json. Same rows, same recommended values, no widgets: (settings.json
# key, the label the game uses, the choice table it is worded with, and a short
# reason where there is a real one).
#
# The VALUE is never written here - it is looked up in RECOMMENDED_GAME_MENU, so
# there is exactly one place a recommendation can be changed.
# --------------------------------------------------------------------------

# The mod writes these itself at every launch. Shown so there is somewhere to
# look when the picture is wrong.
WINDOW_MODE = [("0", "windowed"), ("1", "fullscreen")]

MENU_MOD_APPLIES = [
    ("GraphicsSSAO", "Ambient occlusion (SSAO)", ON_OFF,
     "rebuilt from the previous frame, so it flickers"),
    ("GraphicsSSReflection", "Screen-space reflections", ON_OFF,
     "same reused frame"),
    ("GraphicsContactShadows", "Contact shadows", ON_OFF, "same reused frame"),
    ("GraphicsMotionBlur", "Motion blur", ON_OFF, "smears every head turn"),
    ("GraphicsDepthOfField", "Depth of field", ON_OFF,
     "in a headset your own eyes do the focusing"),
    ("GraphicsVignette", "Vignette", ON_OFF,
     "darkens the edges you actually look through"),
    ("DisplayFullscreen", "Display mode", WINDOW_MODE, ""),
    ("GameFOV", "Field of view", None, ""),

]

# Nothing writes these once the tab is gone - they are the player's own.
MENU_PLAYER_ONLY = [
    ("GraphicsPostEffects", "Other post effects", ON_OFF, ""),
    ("GraphicsTAASharpness", "TAA sharpness", None,
     "only matters if you put TAA back on"),
    ("GraphicsVolumeFogQuality", "Volumetric fog", QUALITY4,
     "expensive, and it reads flat in stereo"),
    ("GraphicsGI", "Global illumination", ON_OFF, ""),
    ("GraphicsHeatHaze", "Heat haze", ON_OFF, ""),
    ("GraphicsTextureDetail", "Texture detail", QUALITY4, ""),
    ("GraphicsLodFactor", "Level of detail", QUALITY4,
     "a headset shows the pop-in that a monitor hides"),
    ("GraphicsShadowsResolution", "Shadow resolution", QUALITY3, ""),
    ("GraphicsWaterDetail", "Water detail", QUALITY4, ""),
    ("GraphicsTerrainTessellation", "Terrain tessellation", QUALITY4, ""),
    ("GraphicsFurQuality", "Fur quality", QUALITY4, ""),
    ("GraphicsAniso", "Anisotropic filtering", ANISO, ""),
    ("DisplayVSync", "V-Sync", ON_OFF,
     "the headset paces the frames; the monitor must not"),
]

# THE WHOLE RECOMMENDED SET, in the order the game's own menu lists them.
# Anti-aliasing is first and is included even though the mod cannot make it
# stick - this is a list of what to SET, and one that omits it is wrong.
MENU_RECOMMENDED = [
    # FIRST, because it is the only one that stops a feature working outright.
    ("GraphicsAA", "Anti-aliasing", AA_MODES, "required for DLSS"),
    ("DisplayFullscreen", "Display mode", WINDOW_MODE, ""),
    ("DisplayVSync", "V-Sync", ON_OFF, ""),
    ("GameFOV", "Field of view", None, ""),
    ("GraphicsMotionBlur", "Motion blur", ON_OFF, ""),
]

# THESE TWO NUMBERS ARE WHAT KEEP THE WINDOW NARROW. The list above is the widest
# thing on the page, and the window must not grow sideways. Height is free - the page scrolls - so a reason that wraps onto a
# second line costs nothing and a reason that widens the column costs the shape
# of the whole window.
# --------------------------------------------------------------------------
# WHAT IS KNOWN TO BE WRONG, ON THE PAGE RATHER THAN IN A FILE NOBODY OPENS.
#
# A player who meets one of these and does not know it is known assumes a broken
# installation and starts changing settings at random - which is how a working
# configuration gets taken apart. Written as "what you will see" first and the
# reason second, because the symptom is what they arrive with.
#
# THE ONLY COPY. tools/make_release.py writes README.txt's KNOWN ISSUES section
# from this list, so the file in the zip and the window on screen cannot drift
# apart - the same rule the two "recommended" buttons now live under.
# --------------------------------------------------------------------------

KNOWN_ISSUES = [
    ("Objects can shake or shimmer with DLSS on",
     "Worst on close-up geometry and edges. Weapon depth makes it worse - it "
     "confuses the picture pass about which camera drew the frame, and the "
     "result is seen as the WORLD shaking rather than the weapon. Two things "
     "help: DLSS models M and L reduce the shaking noticeably, and turning "
     "weapon depth off on the WEAPON tab removes its share of it entirely."),
    ("The HUD is drawn flat across the whole view",
     "Rather than at a comfortable distance. The switches that move it are in "
     "the in-game panel and currently break more than they fix, which is why "
     "they carry a warning."),
    ("Leaning does not collide with anything",
     "6DoF moves the camera, not the character, so lean far enough and you will "
     "lean through a wall. The travel limit in the panel is the guard."),
    ("The scope's magnified picture is positioned from your head",
     "Not from the gun - so holding free-look while aimed slides it off the "
     "scope."),
    ("Sizes and shapes are worked out for a Quest 3",
     "Other headsets run fine but may lose some field or waste pixels. Use the "
     "CUSTOM resolution and raise \"How much wider\" until it fills the view."),
]


MENU_WRAP = 460          # the explanatory lines above and below the list
MENU_REASON_WRAP = 200   # the third column


def _menu_text(key, choices):
    """The recommended value for a game setting, worded the way the game words it."""
    value = RECOMMENDED_GAME_MENU.get(key, "")
    for val, text in (choices or []):
        if val == value or _num_eq(val, value):
            return text
    return value


# --------------------------------------------------------------------------
# The window
# --------------------------------------------------------------------------

class Launcher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(TITLE)
        # Sized to the CONTENT now that the preset labels are short. It was
        # 1040 wide purely to fit a sentence-per-row resolution list.
        # 720 IS THE WIDTH, and losing a whole tab does not buy any of it back:
        # what that bought is height, which the page spends on the video-menu
        # list and scrolls when it runs out.
        self.minsize(720, 540)
        try:
            self.call("tk", "scaling", 1.25)
        except tk.TclError:
            pass

        self.ini = ini_path()
        self.ini_values = read_ini(self.ini)

        self.vars = {}         # key -> StringVar, all of them cotwvr.ini keys
        self._told_to_verify = False   # the check-it-in-game notice, shown once
        # Set by "Use recommended": ini keys that have no widget of their own -
        # the graphics values the mod hands to the game. Written on Save, and
        # only after the player has asked for the recommended set.
        self._pending_ini = {}

        # The Width/Height boxes, hidden unless the preset is CUSTOM. Two
        # permanently visible "0 = use the preset" fields were clutter for
        # everyone who never wants them.
        self._custom_widgets = []
        self._preset_combo = None
        # The same trick for the DLSS percentage: it is meaningless unless the
        # quality level is Custom, and a box that does nothing is worse than no
        # box at all.
        self._dlss_pct_widgets = []
        self._quality_combo = None
        # The anti-aliasing mode: read from two ini keys, written back to three.
        self._aa_combo = None
        self._dlss_widgets = []

        self._build()
        # After the page is built, so it opens on top of a real window rather
        # than an empty grey rectangle.
        self.after(120, self.welcome)
        # Opening with a custom size already saved must SHOW that, not a preset
        # the game is not using.
        try:
            cw = int(str(self.ini_values.get("render_custom_w", "0")).strip() or 0)
            ch = int(str(self.ini_values.get("render_custom_h", "0")).strip() or 0)
        except ValueError:
            cw = ch = 0
        if cw > 0 and ch > 0 and self._preset_combo is not None:
            for val, text in RENDER_PRESETS:
                if val == "-1":
                    self._preset_combo.set(text)
                    break
        self._show_aa_mode(self.ini_values.get("dlss_enable"),
                           self.ini_values.get("taa_replace_pass"))
        self._sync_custom_rows()
        # NOTHING SETS THE WINDOW SIZE HERE, and nothing needs to. The page sits
        # inside a Canvas, and a canvas does not pass its contents' size on to
        # the window - measured before and after this change, the window asks for
        # 570x396 either way and opens at the 720x540 minimum above. So the list
        # added below cannot widen the window however long it grows; it can only
        # make the page scroll, which is what the scrollbar is for.

    # -- layout ------------------------------------------------------------
    def _build(self):
        head = ttk.Frame(self, padding=(16, 12, 16, 6))
        head.pack(fill="x")
        ttk.Label(head, text="theHunter: Call of the Wild - VR",
                  font=("Segoe UI", 15, "bold")).pack(anchor="w")
        ttk.Label(head, text="a VR mod by %s   -   version %s   -   built for game "
                             "update %s" % (AUTHOR, VERSION, GAME_BUILD),
                  foreground="#444").pack(anchor="w")
        sub = "the mod's own settings - written to cotwvr.ini, and nothing else"
        ttk.Label(head, text=sub, foreground="#666").pack(anchor="w")

        # *** THE FOOTER IS PACKED BEFORE THE NOTEBOOK, ON PURPOSE. ***
        #
        # pack() hands out space in the order widgets are added, and the
        # notebook takes everything it can with expand=True. Packed after it,
        # the Save and Launch buttons were the first thing squeezed off when
        # the window was dragged smaller - the one part of the window that must
        # never disappear. Claiming the bottom strip first makes the notebook
        # shrink instead.
        foot = ttk.Frame(self, padding=(16, 6, 16, 14))
        foot.pack(side="bottom", fill="x")
        self.status = ttk.Label(foot, text=self._where(), foreground="#666")
        self.status.pack(side="left")
        ttk.Button(foot, text="Launch game", command=self.launch).pack(side="right")
        ttk.Button(foot, text="Save", command=self.save).pack(side="right", padx=8)
        ttk.Button(foot, text="Use recommended",
                   command=self.apply_recommended).pack(side="right")
        # THE FIRST THING ANY BUG REPORT NEEDS, one click away. Telling someone
        # to paste a path into Explorer is a step at which most reports stop.
        ttk.Button(foot, text="Open log folder",
                   command=self.open_log_folder).pack(side="right", padx=8)

        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=12, pady=8)

        # ONE PAGE. The "Game graphics" tab that used to sit beside this one is
        # gone: every widget on it wrote settings.json, the game overwrites that
        # file from memory on its way out, and the mod re-applies what matters
        # from cotwvr.ini at every launch anyway. What the tab knew is now the
        # read-only list at the bottom of this page.
        #
        # Two columns, with these pinned across the bottom. They are the short
        # groups, and putting them under the columns keeps the window narrow -
        # side by side they were adding their own width to the page.
        BOTTOM = ("Field of view",)

        outer = ttk.Frame(nb)
        nb.add(outer, text="VR mod")
        page = self._scrollable(outer)
        page.columnconfigure(0, weight=0)
        page.columnconfigure(1, weight=1)

        top = [(g, rows) for g, rows in MOD_ROWS if g not in BOTTOM]
        bottom = [(g, rows) for g, rows in MOD_ROWS if g in BOTTOM]

        total = sum(len(rows) for _, rows in top)
        left, right, used = [], [], 0
        for group, rows in top:
            if used < (total + 1) // 2 or not left:
                left.append((group, rows))
                used += len(rows)
            else:
                right.append((group, rows))

        rows_used = 0
        for col, column in ((0, left), (1, right)):
            r = 0
            for group, grows in column:
                box = self._group(page, group, grows)
                box.grid(row=r, column=col, sticky="new", padx=(0, 10),
                         pady=(0, 10))
                r += 1
            rows_used = max(rows_used, r)

        for group, grows in bottom:
            box = self._group(page, group, grows)
            box.grid(row=rows_used, column=0, columnspan=2, sticky="new",
                     padx=(0, 10), pady=(0, 10))
            rows_used += 1

        # The list of what to set in the game itself, full width under the rest.
        self._menu_panel(page).grid(row=rows_used, column=0, columnspan=2,
                                    sticky="new", padx=(0, 10), pady=(0, 10))
        self._issues_panel(page).grid(row=rows_used + 1, column=0, columnspan=2,
                                      sticky="new", padx=(0, 10), pady=(0, 10))

    def _issues_panel(self, parent):
        """Read-only, and below the settings on purpose - it is reference, not
        something to act on."""
        box = ttk.LabelFrame(parent, padding=10, text="KNOWN ISSUES")
        for r, (symptom, detail) in enumerate(KNOWN_ISSUES):
            ttk.Label(box, text="- " + symptom, font=("Segoe UI", 9, "bold"),
                      wraplength=MENU_WRAP, justify="left").grid(
                row=r * 2, column=0, sticky="w", pady=(6 if r else 0, 0))
            ttk.Label(box, text="   " + detail, foreground="#666",
                      wraplength=MENU_WRAP, justify="left").grid(
                row=r * 2 + 1, column=0, sticky="w")
        return box

    def _menu_panel(self, parent):
        """
        The read-only list of what to set in the game's own video menu.

        READ-ONLY ON PURPOSE, and it must not be mistakable for a control: there
        is no box, no dropdown and nothing to click, because the launcher writes
        cotwvr.ini and nothing else. A widget here would be a widget that does
        nothing - which is precisely the trap the old Game-graphics tab fell
        into, where every dropdown wrote a file the game overwrote on its way
        out.
        """
        box = ttk.LabelFrame(parent, padding=10,
                             text="SET THESE IN THE GAME'S OWN VIDEO MENU")
        bold = ("Segoe UI", 9, "bold")
        r = 0

        def line(text, colour="#666", pady=(0, 8), font=None):
            nonlocal r
            w = ttk.Label(box, text=text, foreground=colour, justify="left",
                          wraplength=MENU_WRAP)
            if font:
                w.configure(font=font)
            w.grid(row=r, column=0, columnspan=3, sticky="w", pady=pady)
            r += 1

        # A LIST TO COPY INTO A MENU. Nothing else.
        #
        # It carried a paragraph above, a reason beside every row and two more
        # paragraphs below, explaining which file writes what. That is the
        # mod's business, not the player's, and it made the page four times
        # longer than the thing it is for.
        for key, label, choices, _reason in MENU_RECOMMENDED:
            ttk.Label(box, text=label).grid(
                row=r, column=0, sticky="w", padx=(12, 10), pady=1)
            ttk.Label(box, text=_menu_text(key, choices), font=bold).grid(
                row=r, column=1, sticky="w", pady=1)
            r += 1

        return box

    def _where(self):
        if not os.path.exists(self.ini):
            return "cotwvr.ini not found - is this beside the game exe?"
        return "editing %s - the game's own settings are left alone" % (
            os.path.basename(self.ini),)

    def _scrollable(self, outer):
        """A page that scrolls ONLY when it has to.

        A scrollbar that is always there is clutter on a window that usually
        fits; one that is never there hides settings the moment the window is
        dragged smaller. So it appears and disappears with the need.
        """
        canvas = tk.Canvas(outer, highlightthickness=0, borderwidth=0)
        bar = ttk.Scrollbar(outer, orient="vertical", command=canvas.yview)
        inner = ttk.Frame(canvas, padding=10)
        win = canvas.create_window((0, 0), window=inner, anchor="nw")
        canvas.configure(yscrollcommand=bar.set)
        canvas.pack(side="left", fill="both", expand=True)

        def refresh(_e=None):
            canvas.configure(scrollregion=canvas.bbox("all"))
            # The page must fill the width it is given, or the two columns sit
            # squashed against the left edge.
            canvas.itemconfigure(win, width=canvas.winfo_width())
            need = inner.winfo_reqheight() > canvas.winfo_height()
            if need and not bar.winfo_ismapped():
                bar.pack(side="right", fill="y")
            elif not need and bar.winfo_ismapped():
                bar.pack_forget()
                canvas.yview_moveto(0)

        inner.bind("<Configure>", refresh)
        canvas.bind("<Configure>", refresh)

        # The wheel only scrolls while the pointer is over a page that scrolls,
        # so it never fights the dropdowns.
        def wheel(e):
            if bar.winfo_ismapped():
                canvas.yview_scroll(-1 if e.delta > 0 else 1, "units")
        canvas.bind("<Enter>", lambda e: canvas.bind_all("<MouseWheel>", wheel))
        canvas.bind("<Leave>", lambda e: canvas.unbind_all("<MouseWheel>"))
        return inner

    def _group(self, parent, title, rows):
        box = ttk.LabelFrame(parent, text=title, padding=10)
        notes = []
        for r, (key, label, choices, help_text) in enumerate(rows):
            label_widget = ttk.Label(box, text=label)
            label_widget.grid(row=r, column=0, sticky="w", padx=(0, 10), pady=3)
            # Every widget on the page is a cotwvr.ini key now. There is no
            # second source to choose between, so there is no way for a value to
            # be read from one file and written to another.
            cur = self.ini_values.get(key)
            cur = "" if cur is None else str(cur)
            var = tk.StringVar(value=cur)
            self.vars[key] = var
            if choices:
                labels = [t for _, t in choices]
                widest = max((len(t) for t in labels), default=20)
                combo = ttk.Combobox(box, values=labels, state="readonly",
                                     width=min(max(widest + 2, 16), 30))
                for val, text in choices:
                    if val == cur or _num_eq(val, cur):
                        combo.set(text)
                        break
                combo.grid(row=r, column=1, sticky="w", pady=3)
                # NOTE: tk.bind REPLACES the handler for a sequence unless "+"
                # is passed. A do-nothing bind on this event used to sit below
                # and silently overwrote the one that reveals the CUSTOM boxes,
                # so choosing CUSTOM did nothing. Do not add one.
                if key == "render_preset":
                    self._preset_combo = combo
                    combo.bind("<<ComboboxSelected>>",
                               lambda e: self._sync_custom_rows())
                if key == "dlss_quality":
                    self._quality_combo = combo
                    combo.bind("<<ComboboxSelected>>",
                               lambda e: self._sync_custom_rows())
                if key == "aa_mode":
                    self._aa_combo = combo
                    combo.bind("<<ComboboxSelected>>",
                               lambda e: self._sync_custom_rows())
                if key in ("dlss_quality", "dlss_preset"):
                    self._dlss_widgets.append((label_widget, combo))
                var.combo = combo          # remembered for save()
                var.choices = choices
            else:
                entry = ttk.Entry(box, textvariable=var, width=12)
                entry.grid(row=r, column=1, sticky="w", pady=3)
                # A BOX THE MOD OVERWRITES MUST NOT LOOK LIKE A BOX YOU CAN FILL
                # IN. This one is reported by the mod, not set here: it is
                # rewritten at every launch to match the FOV actually given to
                # the game. Left as a plain white field it read as a setting -
                # it read as a setting on sight - and typing in it did nothing that
                # survived the next launch. Greyed and disabled, it reads as
                # what it is.
                if key in READ_ONLY_ROWS:
                    entry.state(["disabled"])
                var.combo = None
                var.choices = None
            if key in ("render_custom_w", "render_custom_h"):
                self._custom_widgets.append((label_widget, entry))
            if key == "dlss_upscale_pct":
                self._dlss_pct_widgets.append((label_widget, entry))
            if help_text:
                notes.append((label, help_text))

        # A NOTE COLUMN BESIDE EVERY ROW IS WHAT MADE THE WINDOW WIDE - two
        # groups side by side each carried one. Collected into a single grey
        # line under the group instead, where it costs height that was going
        # spare rather than width that was not.
        if notes:
            text = (notes[0][1] if len(notes) == 1
                    else "   ".join("%s: %s" % n for n in notes))
            ttk.Label(box, text=text, foreground="#888",
                      wraplength=360, justify="left").grid(
                row=len(rows), column=0, columnspan=2, sticky="w", pady=(6, 0))
        return box

    # -- actions -----------------------------------------------------------
    def _aa_mode(self):
        """The mode currently shown, as its value string."""
        if self._aa_combo is None:
            return "2"
        text = self._aa_combo.get()
        for val, t in AA_MODE:
            if t == text:
                return val
        return "2"

    def _show_aa_mode(self, dlss_enable, taa_replace):
        """Point the dropdown at what these two ini values mean together."""
        if self._aa_combo is None:
            return
        try:
            mode = "2" if int(float(dlss_enable or 0)) else (
                   "1" if int(float(taa_replace or 0)) else "0")
        except (TypeError, ValueError):
            mode = "2"
        for val, text in AA_MODE:
            if val == mode:
                self._aa_combo.set(text)
                break

    def _sync_custom_rows(self):
        """Show each hand-typed box only while its dropdown says Custom."""
        custom = False
        if self._preset_combo is not None:
            custom = self._preset_combo.get().startswith("CUSTOM")
        pct = False
        if self._quality_combo is not None:
            pct = self._quality_combo.get().startswith("Custom")
        dlss = self._aa_mode() == "2"
        pct = pct and dlss
        for pairs, show in ((self._custom_widgets, custom),
                            (self._dlss_pct_widgets, pct),
                            (self._dlss_widgets, dlss)):
            for label, entry in pairs:
                if show:
                    label.grid()
                    entry.grid()
                else:
                    label.grid_remove()
                    entry.grid_remove()

    def _collect(self):
        """Everything to write, and it all goes into cotwvr.ini."""
        ini_changes = {}
        for key, var in self.vars.items():
            if key in READ_ONLY_ROWS:   # the mod owns it; writing it back here
                continue                # would only put it out of step

            if getattr(var, "combo", None) is not None:
                text = var.combo.get()
                value = None
                for val, t in var.choices:
                    if t == text:
                        value = val
                        break
                if value is None:
                    continue
            else:
                value = var.get().strip()
                if value == "":
                    continue
            ini_changes[key] = value

        # CUSTOM is a UI sentinel, not a preset index. When it is chosen the
        # width/height carry the resolution and the preset is left alone; when it
        # is not, the custom pair is cleared so a stale value from a previous
        # session cannot silently override the preset the player just picked.
        if ini_changes.get("render_preset") == "-1":
            ini_changes.pop("render_preset", None)
            for k in ("render_custom_w", "render_custom_h"):
                if not str(ini_changes.get(k, "")).strip().isdigit():
                    ini_changes[k] = "0"
        else:
            ini_changes["render_custom_w"] = "0"
            ini_changes["render_custom_h"] = "0"

        # HAND-SET "how near the world sits" MUST SURVIVE THE NEXT LAUNCH.
        #
        # The mod resets game_fov_deg on every launch to match the FOV it gave
        # the game, so the two cannot drift - which also meant any value tuned by
        # hand was gone before the world appeared. So say which of the two
        # applies, rather than leaving it to be discovered: choose the number the
        # mod would have picked anyway and it keeps following automatically;
        # choose anything else and the mod is told to leave it alone
        # (game_fov_follows_auto = 0).
        #
        # The number the mod would pick follows the same rule as
        # gamesettings.cpp: force_game_fov above zero is used as given, otherwise
        # the headset's own vertical FOV, clamped to the 90 the game's slider
        # stops at. Both are read from the ini rather than restated here.
        #
        # This used to sit inside "if a render resolution was chosen", which it
        # never had anything to do with - it is decided entirely by the two FOV
        # boxes on this page.
        ini_now = read_ini(self.ini)
        ini_now.update({k: str(v) for k, v in ini_changes.items()})
        fov = _game_fov_for(ini_now)
        if fov:
            try:
                chosen = float(ini_changes.get("game_fov_deg", fov))
            except (TypeError, ValueError):
                chosen = float(fov)
            ini_changes["game_fov_follows_auto"] = (
                "1" if abs(chosen - float(fov)) < 0.5 else "0")

        # *** ONE CHOICE ON THE PAGE, THREE KEYS IN THE FILE. ***
        #
        # aa_mode is not a setting the mod has - it is how this window words a
        # choice that lives in three of them. Expanded here so the file never
        # sees the invented name, and so the game's own anti-aliasing follows:
        # the mod's pass needs the engine's temporal one to EXIST before it can
        # replace it, and "off" wants FXAA alone because the engine's TAA blends
        # each eye with the other eye's picture.
        mode = ini_changes.pop("aa_mode", None)
        if mode is not None:
            ini_changes["dlss_enable"] = "1" if mode == "2" else "0"
            ini_changes["taa_replace_pass"] = "0" if mode == "0" else "1"
            ini_changes["graphics_aa"] = "1" if mode == "0" else "3"

        # *** DLSS AND ITS RESOLVE PASS ARE ONE SWITCH. ***
        #
        # taa_replace_pass is what writes the motion vectors DLSS reconstructs
        # from. With it off, DLSS is handed nothing and declines every frame -
        # so the player sees no anti-aliasing, no upscaling and no explanation,
        # having switched on the thing that was supposed to provide them. The
        # page shows one control; the file keeps both keys, because they are
        # genuinely two passes and the mod's own panel can still separate them.
        # The graphics values that have no widget, put there by "Use
        # recommended". Nothing overlaps: these are graphics_* keys and every
        # widget above is not.
        ini_changes.update(self._pending_ini)
        return ini_changes

    def apply_recommended(self):
        """
        Fills the boxes; does not touch the file. Nothing is written until Save,
        so the change can be looked over - and undone by closing the window -
        before it becomes real.
        """
        changed = 0
        queued = {}
        for key, value in RECOMMENDED.items():
            var = self.vars.get(key)
            # *** A KEY WITH NO WIDGET IS STILL PART OF THE RECOMMENDATION. ***
            #
            # Several of these have no row on the page - some because they are
            # facts about the engine rather than preferences, some because the
            # row was removed later. They used to be SKIPPED here, which meant
            # "Use recommended" quietly handed back a configuration missing
            # whichever settings had no box, and nothing said so. Queued for
            # Save instead, exactly like the graphics values below.
            if var is None:
                queued[key] = value
                if str(self.ini_values.get(key, "")).strip() != value:
                    changed += 1
                continue
            if getattr(var, "combo", None) is not None:
                for val, text in var.choices:
                    if _num_eq(val, value):
                        if var.combo.get() != text:
                            changed += 1
                        var.combo.set(text)
                        break
            else:
                if var.get().strip() != value:
                    changed += 1
                var.set(value)

        # AND THE SEVEN THE MOD HANDS TO THE GAME. They have no boxes to fill -
        # the page shows them as a list to read - so they are queued for Save
        # instead. Queued, not written: the same rule as everything above.
        extra = sum(1 for k, v in RECOMMENDED_MOD_GRAPHICS.items()
                    if str(self.ini_values.get(k, "")).strip() != v)
        # Both: the graphics values derived from the menu table, and every
        # other setting of his that has no widget on this page.
        self._pending_ini = dict(RECOMMENDED_MOD_EXTRA)
        self._pending_ini.update(RECOMMENDED_MOD_GRAPHICS)
        self._pending_ini.update(queued)

        # aa_mode has no entry in RECOMMENDED - it is not a real setting - so
        # it is pointed at what the two real ones say.
        self._show_aa_mode(RECOMMENDED.get("dlss_enable", "0"),
                           RECOMMENDED.get("taa_replace_pass", "0"))
        self._sync_custom_rows()

        bits = []
        if changed:
            bits.append("%d setting%s" % (changed, "" if changed == 1 else "s"))
        if extra:
            bits.append("%d the mod gives the game" % extra)
        self.status.config(
            text=("already on the recommended settings" if not bits
                  else "%s changed - press Save to keep them" % " and ".join(bits)),
            foreground="#666" if not bits else "#a70")

    # _game_running() lived here. It existed only to warn that saving graphics
    # settings while the game was open would be undone when the game quit. The
    # launcher does not write the game's settings any more, and cotwvr.ini is
    # read at launch and never written by the game, so saving while it runs is
    # simply a change that applies next time.

    def save(self):
        ini_changes = self._collect()
        ok, err = write_ini(self.ini, ini_changes)
        if ok:
            # SET IT IN THE GAME, ONCE. Everything on this page went into
            # cotwvr.ini, which is the mod's file and stays put. The graphics
            # options in the list below are the game's own, and the game's video
            # menu is the only place that shows what it is really using - all the
            # more so now that nothing here writes them. Said once per session,
            # not on every Save.
            self.ini_values = read_ini(self.ini)
            self._pending_ini = {}
            if not self._told_to_verify and not notice_dismissed():
                self._told_to_verify = True
                self._save_notice()
            self.status.config(
                text="saved to cotwvr.ini - set the listed options in the game once",
                foreground="#2a7")
            return True
        messagebox.showerror(TITLE, err or "could not write cotwvr.ini")
        return False

    def _save_notice(self):
        """The saved-OK notice, with a way to stop being told.

        tkinter's messagebox cannot carry a checkbox, and the alternative - a
        yes/no phrased as "show this again?" - makes the player answer a
        question about the dialog instead of reading it. A small window of our
        own costs twenty lines and says what it means.
        """
        win = tk.Toplevel(self)
        win.title(TITLE)
        win.transient(self)
        win.resizable(False, False)
        body = ttk.Frame(win, padding=16)
        body.pack(fill="both", expand=True)
        ttk.Label(
            body, justify="left", wraplength=430,
            text=("Saved to cotwvr.ini." + chr(10) + chr(10) +
                  "This window does not change the game's own settings. Open "
                  "the game's VIDEO menu once and set the options listed at "
                  "the bottom of the page." + chr(10) + chr(10) +
                  "The mod re-applies the screen-space effects and the window "
                  "mode itself at every launch, so most of them should already "
                  "read that way - the menu is where to check.")
        ).pack(anchor="w")
        hide = tk.BooleanVar(value=False)
        ttk.Checkbutton(body, text="Do not show this again",
                        variable=hide).pack(anchor="w", pady=(14, 0))

        def close():
            if hide.get():
                dismiss_notice()
            win.destroy()

        ttk.Button(body, text="OK", command=close).pack(anchor="e", pady=(14, 0))
        win.protocol("WM_DELETE_WINDOW", close)
        win.grab_set()
        win.wait_window()

    def open_log_folder(self):
        """%LOCALAPPDATA%	heHunterCotWVR, where the mod writes cotwvr.log."""
        folder = os.path.join(os.environ.get("LOCALAPPDATA", ""),
                              "theHunterCotWVR")
        if not os.path.isdir(folder):
            messagebox.showinfo(
                TITLE,
                "No log folder yet - it appears the first time the game runs "
                "with the mod installed." + chr(10) + chr(10) + folder)
            return
        try:
            os.startfile(folder)
        except OSError:
            messagebox.showinfo(TITLE, folder)

    def welcome(self):
        """READ THIS FIRST - shown once, with a way to stop showing it.

        Everything on it is something a player can only learn by losing an
        evening to it: that DLSS needs an option set in the GAME's menu, that
        this is single-player business, that the mod is aimed at one game
        build. A README next to the exe is not read; a window that opens by
        itself is.
        """
        if welcome_dismissed():
            return
        win = tk.Toplevel(self)
        win.title("theHunter: Call of the Wild VR - read this first")
        win.transient(self)
        win.resizable(False, False)
        body = ttk.Frame(win, padding=18)
        body.pack(fill="both", expand=True)

        ttk.Label(body, text="theHunter: Call of the Wild - VR",
                  font=("Segoe UI", 13, "bold")).pack(anchor="w")
        ttk.Label(body, text="a VR mod by %s   -   version %s" % (AUTHOR, VERSION),
                  foreground="#444").pack(anchor="w", pady=(0, 12))

        for title, text in WELCOME_POINTS:
            ttk.Label(body, text=title, font=("Segoe UI", 9, "bold")).pack(
                anchor="w", pady=(8, 0))
            ttk.Label(body, text=text, justify="left", wraplength=520,
                      foreground="#333").pack(anchor="w")

        hide = tk.BooleanVar(value=False)
        ttk.Checkbutton(body, text="Do not show this again",
                        variable=hide).pack(anchor="w", pady=(16, 0))

        def close():
            if hide.get():
                dismiss_welcome()
            win.destroy()

        ttk.Button(body, text="Start", command=close).pack(anchor="e", pady=(12, 0))
        win.protocol("WM_DELETE_WINDOW", close)
        win.grab_set()
        win.wait_window()

    def launch(self):
        if not self.save():
            return
        # *** THE EXE DIRECTLY - BUT ONLY IF STEAM IS ALREADY RUNNING. ***
        #
        # Starting the exe ourselves is worth doing: the XINPUT proxy beside it
        # is then loaded by the process we started, with no Steam launcher step
        # in between deciding what runs. But the game is DRM-wrapped, so run it
        # with Steam closed and it puts up "Steam must be running to play this
        # title" - twice, because it starts two processes - and quits.
        #
        # The old fallback could not catch that. It only fired if Popen ITSELF
        # failed, and Popen succeeds perfectly: the process starts, and refuses
        # a moment later inside its own code where nothing here can see it. A
        # fallback that waits for an exception cannot help when the failure is
        # somebody else's dialog box.
        #
        # So ask the question BEFORE launching rather than reacting afterwards.
        # steam:// starts Steam itself and then the game, which is the one route
        # that works from a cold machine.
        exe = os.path.join(here(), "theHunterCotW_F.exe")
        if os.path.exists(exe) and steam_running():
            try:
                self.status.config(text="starting the game...", foreground="#666")
                self.update_idletasks()
                # *** DETACHED, OR THE LAUNCHER CANNOT CLEAN UP AFTER ITSELF. ***
                #
                # This window is a PyInstaller one-file exe: it unpacks itself
                # into %TEMP%\_MEInnnnnn and deletes that on the way out. A
                # child started the ordinary way INHERITS this process's open
                # handles, so the game kept the unpacked folder busy and the
                # delete failed - producing a "Failed to remove temporary
                # directory" warning box at the exact moment the player is
                # putting the headset on, which reads as the mod crashing.
                #
                # DETACHED_PROCESS plus close_fds hands the game nothing to
                # hold, so the folder is free by the time the bootloader wants
                # it back.
                flags = (getattr(subprocess, "DETACHED_PROCESS", 0) |
                         getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))
                proc = subprocess.Popen([exe], cwd=here(), close_fds=True,
                                        creationflags=flags)
                self._watch_launch(proc, 0)
                return
            except OSError:
                pass
        self._launch_via_steam()

    def _watch_launch(self, proc, ticks):
        """*** DID IT ACTUALLY START, OR JUST START AND GIVE UP? ***

        The DRM refusal happens INSIDE the game a moment after Popen has
        already succeeded, so the only honest test is whether anything is still
        running a few seconds later. This asks that instead of predicting it -
        which also covers the causes not guessed at, elevation mismatch being
        the likely one when Steam IS running and the game still refuses.

        It does NOT simply watch our own process, because the game hands off to
        a second one: our child exiting is normal, and treating that as failure
        would launch the game a second time through Steam. What matters is
        whether an exe of that name exists at all.
        """
        if proc.poll() is None:
            if ticks >= 12:                      # ~6 s and still up: it is away
                self.destroy()
                return
            self.after(500, lambda: self._watch_launch(proc, ticks + 1))
            return
        if process_running("theHunterCotW_F.exe"):
            self.destroy()                       # handed off to its real process
            return
        self.status.config(text="the game closed itself - trying through Steam",
                           foreground="#a70")
        self.update_idletasks()
        self._launch_via_steam()

    def _launch_via_steam(self):
        """The route that works from a cold machine: Steam starts, then the game."""
        try:
            os.startfile("steam://rungameid/%s" % APP_ID)
        except OSError:
            messagebox.showerror(
                TITLE,
                "Could not start the game." + chr(10) + chr(10) +
                "It has to be started with Steam running. Start Steam, then "
                "press Launch again - or start the game however you normally "
                "do, the mod loads either way." + chr(10) + chr(10) +
                "If Steam IS running and the game still says it is not, this "
                "window is probably running as administrator and Steam is not. "
                "Close it and open it normally.")
            return
        self.after(2500, self.destroy)


# The game's FOV slider is VERTICAL and stops at 90; it silently ignores
# anything larger. Measured: the projection is exactly 1.00000 at slider 90.
GAME_FOV_MAX = 90


def _game_fov_for(ini):
    """
    The number to write into GameFOV, by the same rule as gamesettings.cpp.

    Kept deliberately small and reading from the ini rather than restating the
    values, because one number owned in two places is what produced the FOV-0
    bug: the mod treated only negatives as "auto", so a 0 in the ini was written
    to the game verbatim as a field of view of zero.
    """
    try:
        forced = int(float(ini.get("force_game_fov", "-1")))
    except (TypeError, ValueError):
        forced = -1
    if forced > 0:
        return min(forced, GAME_FOV_MAX)
    try:
        vfov = float(ini.get("headset_vfov_deg", "99"))
    except (TypeError, ValueError):
        vfov = 99.0
    return min(int(vfov + 0.5), GAME_FOV_MAX)


def _num_eq(a, b):
    try:
        return float(a) == float(b)
    except (TypeError, ValueError):
        return False


if __name__ == "__main__":
    Launcher().mainloop()
