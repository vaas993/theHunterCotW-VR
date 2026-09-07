"""Write the Discord posts for a release.

Discord is not a mod page and should not be treated like one. A wall of text in
an announcements channel is scrolled past; what works is a short post that says
what it is, where to get it, and where to go when it breaks - with the detail
left on the pages built for it.

THE HARD CONSTRAINT IS 2000 CHARACTERS per message (4000 with Nitro, which
nobody should have to have to read your announcement). Going over does not warn
you - the client simply refuses to send, usually after you have written it. So
every post is measured here and reported.

Discord's markdown is its own: **bold**, *italic*, `code`, # and ## headings,
- lists. MASKED LINKS - [text](url) - ARE NOT RELIABLE in plain messages, only
in embeds, so every link here is a bare URL, which always works and auto-links.

    python tools\\make_discord_posts.py
"""

import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "discord")

VERSION = "1.1.1"
GAME_BUILD = "9.2 (Peru Hunting Reserve)"

# Fill these in once the pages are live.
GITHUB = "https://github.com/vaas993/theHunterCotW-VR"
NEXUS = "https://www.nexusmods.com/thehuntercallofthewild/mods/YOUR-ID"

LIMIT = 2000


ANNOUNCEMENT = """# theHunter: Call of the Wild - VR  |  v%(version)s

**Native OpenXR stereo VR is out.** The game renders in real 3D - the world is
drawn from its own camera, once per eye. Your head turns the view, you can lean
and step around inside it, and your weapon has depth in your hands.

Not a screen-in-a-void wrapper, and not a 3D filter.

**What's in it**
- Real stereo rendering, per eye
- 6DoF - lean around a trunk, duck under a branch, put your eye behind the scope
- NVIDIA DLSS, **or** the mod's own per-eye anti-aliasing if you have no RTX card
- A settings window before you play, and a full settings panel inside the headset
- One button that puts you back on settings that are known to work

**Get it**
Nexus: %(nexus)s
GitHub: %(github)s

**Before you ask**
- Built for game update **%(build)s**
- **Single player only** - do not use it in multiplayer
- DLSS needs an RTX card. Everything else works on any card
- It is a work in progress. The known issues are listed on both pages, and
  reading them first will save you an evening

Problems go in the support channel, and please attach your log - it usually
contains the answer. The settings window has an **Open log folder** button.
"""


PINNED_HELP = """# Reporting a problem with the VR mod

Almost every question is answered by one file, so please start with it.

**1. Get the log**
Open **VR Settings\theHunterCotW VR Settings.exe** and press
**Open log folder**, or go to:
`%%LOCALAPPDATA%%\\theHunterCotWVR`

Attach **cotwvr.log**. If the problem happened on the run *before* the one you
just did, attach **cotwvr.prev.log** instead - the mod keeps both.

**2. Tell us three things**
- Your headset and how you connect it (Link cable, Air Link, Virtual Desktop, SteamVR)
- Your graphics card
- What you saw, and what you expected instead

**Things worth trying first**
- **Black screen in the headset, but the game looks fine on the monitor** - HDR
  is on. Turn it off in Windows display settings.
- **DLSS is not doing anything** - it needs an NVIDIA RTX card, and the game's
  anti-aliasing must be **FXAA + TAA**. The mod sets that itself; check the
  game's video menu if in doubt.
- **The picture will not merge into one** - turn off *Give each eye its own view*
  in the launcher.
- **You messed up the settings** - press **Use recommended** in the launcher, or
  **Use the tested settings** in the in-game panel. Nothing is unrecoverable.
- **You want the game normal again without uninstalling** - set **VR mod** to
  *off* in the launcher. Every file stays where it is.
"""


SHORT_BLURB = """Native OpenXR stereo VR for theHunter: Call of the Wild - the world is drawn
from the game's own camera, once per eye. Head tracking, 6DoF leaning, DLSS or
the mod's own per-eye anti-aliasing on any card. Single player, game update
%(build)s.

%(nexus)s
""" % {"build": GAME_BUILD, "nexus": NEXUS}


def main():
    os.makedirs(OUT, exist_ok=True)
    posts = {
        "announcement.md": ANNOUNCEMENT % {
            "version": VERSION, "build": GAME_BUILD,
            "nexus": NEXUS, "github": GITHUB},
        "pinned_help.md": PINNED_HELP,
        "short_blurb.txt": SHORT_BLURB,
    }
    over = False
    for name, text in posts.items():
        with open(os.path.join(OUT, name), "w", encoding="utf-8",
                  newline="\r\n") as f:
            f.write(text)
        n = len(text)
        flag = ""
        if n > LIMIT:
            flag = "  [!] OVER the %d character limit - Discord will refuse it" % LIMIT
            over = True
        print("   discord\\%-20s %4d / %d characters%s" % (name, n, LIMIT, flag))

    if "YOUR-NAME" in GITHUB or "YOUR-ID" in NEXUS:
        print("   [!] the GitHub and/or Nexus address is still a placeholder - "
              "set them at the top of this script")
    return 1 if over else 0


if __name__ == "__main__":
    raise SystemExit(main())
