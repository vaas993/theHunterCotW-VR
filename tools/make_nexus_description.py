"""Write the Nexus Mods description page, in BBCode.

Nexus does not take Markdown - it takes BBCode, and it has no tables - so the
GitHub page cannot simply be pasted across. This writes the same content in the
form Nexus accepts.

THE KNOWN-ISSUES LIST IS NOT RETYPED HERE. It comes from KNOWN_ISSUES in the
launcher, which is where it lives for the in-game window and the README as well.
A mod page that disagrees with the window in front of the player is worse than
one that says nothing, and the way that happens is somebody updating three
copies and missing one.

    python tools\\make_nexus_description.py
    -> nexus\\description.bbcode   (paste into the Nexus description box)
    -> nexus\\upload_notes.txt     (the fields to fill in around it)
"""

import ast
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "nexus")

VERSION = "1.1.1"
GAME_BUILD = "9.2 (Peru Hunting Reserve)"
AUTHOR = "Vaas993"

VIRUSTOTAL = "https://www.virustotal.com/gui/file/055e61b94789423e51c95a449ad7595b590904f806df7da31b765057d13b00f5"
DISCORD = "https://discord.gg/gbC9AkG2Xn"
PATREON = "https://www.patreon.com/cw/Vaas993"
PROFILE = "https://www.nexusmods.com/profile/Vaas993/mods"
GITHUB = "https://github.com/vaas993/theHunterCotW-VR"


def known_issues():
    path = os.path.join(ROOT, "launcher", "cotwvr_launcher.py")
    with open(path, encoding="utf-8") as f:
        tree = ast.parse(f.read())
    for node in tree.body:
        if (isinstance(node, ast.Assign)
                and getattr(node.targets[0], "id", "") == "KNOWN_ISSUES"):
            return ast.literal_eval(node.value)
    return []


def h(text):
    """A section heading, sized so the page can be skimmed."""
    return "[size=5][b]%s[/b][/size]" % text


def main():
    os.makedirs(OUT, exist_ok=True)
    issues = "\n".join("[*][b]%s[/b] - %s" % (s, d[0].lower() + d[1:])
                       for s, d in known_issues())

    page = """[center][size=6][b]theHunter: Call of the Wild - VR[/b][/size]
[size=4]Native OpenXR stereo VR[/size]
[i]a VR mod by %(author)s[/i][/center]

[line]

The world is drawn from the game's own camera, [b]once per eye[/b] - real depth,
your head turns the view, you can lean and step around inside it, and your weapon
has depth in your hands.

This is not a screen-in-a-void wrapper and not a 3D filter. The game renders in
stereo.

[line]

%(h_req)s
[list]
[*][b]Game:[/b] theHunter: Call of the Wild, update [b]%(build)s[/b] (Steam)
[*][b]Headset:[/b] anything with an OpenXR runtime - Meta Quest Link, SteamVR,
Virtual Desktop
[*][b]DLSS:[/b] optional, and needs an NVIDIA [b]RTX[/b] card. There is a
full-quality alternative for every other card - see below
[*][b]SINGLE PLAYER ONLY.[/b] Do not use this in multiplayer sessions
[/list]

This is a work in progress. Expect rough edges - the known ones are listed
further down, and reading them first will save you an evening.

[line]

%(h_install)s
[list=1]
[*]Unpack the archive.
[*]Copy [b]every file[/b] into the game folder, beside [b]theHunterCotW_F.exe[/b]
(usually [i]...\\steamapps\\common\\theHunterCotW\\[/i]).
[*]Run [b]VR Settings\theHunterCotW VR Settings.exe[/b] - the settings window
lives in that subfolder.
[*]Read the page it opens with, then press [b]Use recommended[/b], [b]Save[/b],
and [b]Launch game[/b].
[*]Have the headset on before the game finishes loading.
[/list]

[b]No game file is modified.[/b] The mod is only the files you added, so
uninstalling is deleting them again.

[line]

%(h_aa)s
You do [b]not[/b] need an RTX card to use this mod. The launcher and the in-game
panel both offer three, and the mod sets the game's own anti-aliasing to match
whichever you pick:

[list]
[*][b]NVIDIA DLSS[/b] - RTX cards only. The best picture, and the only one that
can also buy frame rate back by rendering smaller and rebuilding the size.
[*][b]Per-eye smoothing (mod)[/b] - [b]any card, any brand.[/b] The mod replaces
the game's temporal pass with its own, which keeps each eye's history separate,
so you get the smoothing without the cross-eye ghosting. This is the one to use
without an RTX card.
[*][b]Off - the game's own[/b] - nothing from the mod. Sharp, aliased, and
foliage will shimmer.
[/list]

[line]

%(h_controls)s
[list]
[*][b]Insert[/b] (or Ctrl+Alt+O) - open the settings panel inside the headset
[*][b]Pause[/b] - recentre the view. Use it whenever forward stops being forward
[*][b]Delete[/b] - show the flat game screen, for menus and the map
[*][b]Alt[/b] (held) - free look: the view turns, the weapon stays put
[/list]

Everything can be rebound in the panel, and the panel works with a gamepad.

[line]

%(h_settings)s
Two windows, and they show the same settings:

[list]
[*][b]VR Settings\theHunterCotW VR Settings.exe[/b] - the things you set once
before playing:
resolution, field of view, anti-aliasing, whether the mod is on at all.
[*][b]The in-game panel[/b] - everything, while you play, in the headset. Every
row explains itself in full.
[/list]

Both have a [b]Use recommended[/b] button that puts you back on a configuration
that has actually been played, so no amount of experimenting can leave you
stranded.

[line]

%(h_issues)s
[list]
%(issues)s
[/list]

[line]

%(h_help)s
The mod writes a log to [b]%%LOCALAPPDATA%%\\theHunterCotWVR\\cotwvr.log[/b], and
the settings window has an [b]Open log folder[/b] button. It records what was
detected, which hooks took and every setting the mod changed.
[b]Attach it to any bug report - it usually contains the answer.[/b]

[list]
[*][b]Black screen in the headset, but the game looks fine on the monitor[/b] -
HDR is on. Turn it off in Windows display settings.
[*][b]The mod does nothing at all[/b] - the game has probably been updated past
this build. It identifies the game by fingerprint and refuses to guess.
[*][b]You want the game back to normal without uninstalling[/b] - set
[b]VR mod[/b] to [i]off[/i] in the launcher. Every file stays where it is.
[/list]

[line]

%(h_av)s
[b]3 of 66 engines[/b] on VirusTotal flag the download; 63 pass it clean. File
by file it is narrower still: the settings window and everything it needs scan
[b]0 of 70[/b], as do the OpenXR loader and NVIDIA's own signed DLSS library.
The only two flagged are [i]cotwvr.dll[/i] (1 of 70) and [i]XINPUT9_1_0.dll[/i]
(2 of 70) - the two that do the actual work.

That is not a coincidence, and it is worth saying plainly: XINPUT9_1_0.dll is a
small DLL named after a system library whose job is to load another DLL, which
is how the game loads this mod and also how a certain kind of malware works.
cotwvr.dll then patches the engine's code in memory so it renders one eye at a
time. Heuristic scanners recognise the TECHNIQUE - they are not identifying
anything specific, which is why every major engine passes it and only the
high-false-positive ones do not. Nothing is code-signed.

[list]
[*][b]VirusTotal scan of this exact release:[/b] [url=%(vt)s]%(vt)s[/url]
[*][b]The complete source code is public:[/b] [url=%(github)s]%(github)s[/url] -
read it, or build it yourself with BUILDING.md
[/list]

[line]

%(h_links)s
[list]
[*][b]Discord[/b] (support and updates): [url=%(discord)s]%(discord)s[/url]
[*][b]Source and releases:[/b] [url=%(github)s]%(github)s[/url]
[*][b]Support my work:[/b] [url=%(patreon)s]%(patreon)s[/url]
[*][b]My other mods:[/b] [url=%(profile)s]%(profile)s[/url]
[/list]

[b]Downloads are official only from the links above.[/b] Please do not re-upload
this mod elsewhere: mirrors go stale the moment it is updated, and they strip the
support links people need when something goes wrong.

[line]

%(h_credits)s
Built by [b]%(author)s[/b].

Includes NVIDIA DLSS ([i]nvngx_dlss.dll[/i], redistributed under the NVIDIA DLSS
SDK licence), the Khronos [b]OpenXR[/b] loader (Apache 2.0) and [b]MinHook[/b]
(BSD 2-clause).

theHunter: Call of the Wild is a trademark of Expansive Worlds / Avalanche
Studios. This mod is unofficial and is not affiliated with or endorsed by them.
""" % {
        "author": AUTHOR, "build": GAME_BUILD, "issues": issues,
        "discord": DISCORD, "patreon": PATREON, "profile": PROFILE,
        "github": GITHUB, "vt": VIRUSTOTAL,
        "h_av": h("Antivirus warnings, and why"),
        "h_req": h("Requirements"),
        "h_install": h("Installation"),
        "h_aa": h("Anti-aliasing - three choices, and DLSS is only one of them"),
        "h_controls": h("Controls"),
        "h_settings": h("Where the settings are"),
        "h_issues": h("Known issues"),
        "h_help": h("If something goes wrong"),
        "h_links": h("Links"),
        "h_credits": h("Credits and third-party software"),
    }

    with open(os.path.join(OUT, "description.bbcode"), "w",
              encoding="utf-8", newline="\r\n") as f:
        f.write(page)

    notes = """NEXUS UPLOAD - THE FIELDS AROUND THE DESCRIPTION
================================================
Everything here is a box on the upload form. The description itself is in
description.bbcode - open it, select all, copy, paste.

MOD NAME
    theHunter Call of the Wild VR

SUMMARY  (the one-liner under the title, keep it under ~250 characters)
    Native OpenXR stereo VR. The world is drawn from the game's own camera,
    once per eye - real depth, head tracking, 6DoF leaning, and DLSS or the
    mod's own per-eye anti-aliasing. Single player only.

VERSION
    %(version)s

CATEGORY
    Pick the closest one the game's section offers - Miscellaneous is the
    usual home for something that changes how the game renders.

TAGS  (pick from the list Nexus offers - these are the ones that fit)
    VR, Gameplay, Immersion, Performance, Utilities

REQUIREMENTS
    None to add. The package is self-contained. An OpenXR runtime is needed but
    that is a property of the player's headset, not a Nexus requirement.

PERMISSIONS  (answer these honestly - the mod DOES contain third-party files)
    The package includes:
      - nvngx_dlss.dll        NVIDIA, redistributed under the DLSS SDK licence
      - openxr_loader.dll     Khronos, Apache 2.0
      - MinHook               BSD 2-clause, compiled into cotwvr.dll
    So when Nexus asks whether the mod contains assets from other authors, the
    answer is yes, and the credits section of the description names all three.

FILE TO UPLOAD
    release\\theHunterCotW-VR-v%(version)s.zip     (Main file)
    Version: %(version)s
    Description: first public release

CHANGELOG for this version
    - Native OpenXR stereo VR, one eye at a time from the game's own camera
    - NVIDIA DLSS, or the mod's own per-eye smoothing on any other card
    - 6DoF: lean and step around inside the world
    - Settings launcher, and a full settings panel inside the headset
    - Built for game update %(build)s

BEFORE YOU PUBLISH
    - Fill the GitHub address into tools/make_nexus_description.py (GITHUB) and
      run it again, or the description links to a placeholder.
    - Images sell a mod page more than any text. A screenshot or two of the
      headset view, and the settings window, are worth adding.
""" % {"version": VERSION, "build": GAME_BUILD}

    with open(os.path.join(OUT, "upload_notes.txt"), "w",
              encoding="utf-8", newline="\r\n") as f:
        f.write(notes)

    print("   nexus\\description.bbcode   %d characters" % len(page))
    print("   nexus\\upload_notes.txt     %d characters" % len(notes))
    if "YOUR-NAME" in GITHUB:
        print("   [!] the GitHub address is still a placeholder - set GITHUB "
              "at the top of this script")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
