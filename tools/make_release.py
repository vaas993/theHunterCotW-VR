"""Assemble the shareable release folder, and the zip beside it.

Everything a player copies into the game folder, built from what was actually
compiled rather than from whatever happened to be in the release folder last
time. Run it after build.bat and after PyInstaller.

THE SETTINGS FILE IS GENERATED, NOT COPIED. cotwvr.ini in the game folder is a
working file: it carries whatever was last being tested, a diagnostic switch
left on, and welcome_seen = 1 because the person testing has seen the first-run
page a hundred times. Shipping that file hands every player one machine's
half-finished experiment. So the shipped ini takes the STRUCTURE of the current
one - every key the mod knows, with its explanatory comments - and sets every
value from the recommended tables the launcher and the panel share.

    python tools\\make_release.py [--version 1.1]
"""

import argparse
import ast
import os
import re
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
LAUNCHER = os.path.join(ROOT, "launcher")
GAME_INI = r"C:\Program Files (x86)\Steam\steamapps\common\theHunterCotW\cotwvr.ini"

# Settings that must not ship the way a test machine leaves them, whatever the
# recommended tables say. Each one is here because it was found ON in a working
# ini, not out of caution.
SHIPPED_OVERRIDES = {
    "welcome_seen": "0",        # every player gets the first-run page once
    "dlss_probe": "0",          # a diagnostic, and a noisy one
    "logging": "1",             # the support channel, on by default
    "graphics_aa": "3",         # FXAA+TAA - DLSS cannot run without it
}


def launcher_tables():
    """RECOMMENDED + RECOMMENDED_MOD_EXTRA + the graphics values, as ini pairs.

    Read as literals rather than imported: importing pulls in tkinter, and a
    packaging step that needs a display is a packaging step that fails on the
    machine you least want it to.
    """
    path = os.path.join(LAUNCHER, "cotwvr_launcher.py")
    with open(path, encoding="utf-8") as f:
        tree = ast.parse(f.read())
    out, menu, applies = {}, {}, {}
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        name = getattr(node.targets[0], "id", "")
        if name in ("RECOMMENDED", "RECOMMENDED_MOD_EXTRA"):
            out.update(ast.literal_eval(node.value))
        elif name == "RECOMMENDED_GAME_MENU":
            menu = ast.literal_eval(node.value)
        elif name == "MOD_APPLIES_GRAPHICS":
            applies = ast.literal_eval(node.value)
    # The same derivation the launcher does: the graphics values the mod hands
    # to the game, under their ini names.
    out["apply_game_graphics"] = "1"
    for game_key, ini_key in applies.items():
        if game_key in menu:
            out[ini_key] = menu[game_key]
    out.update(SHIPPED_OVERRIDES)
    return out


def readme_with_issues(path):
    """README.txt, with its KNOWN ISSUES section written from the launcher.

    The launcher shows the list on screen and the README repeats it, which is
    the same two-copies-of-one-claim trap the recommended settings were in. The
    section is last in the file, so everything from its underline to the end is
    replaced - one list, in one place, appearing in both.
    """
    with open(path, encoding="utf-8") as f:
        text = f.read()
    marker = "KNOWN ISSUES\n------------\n"
    at = text.find(marker)
    if at < 0:
        return text, False

    with open(os.path.join(LAUNCHER, "cotwvr_launcher.py"), encoding="utf-8") as f:
        tree = ast.parse(f.read())
    issues = []
    for node in tree.body:
        if (isinstance(node, ast.Assign)
                and getattr(node.targets[0], "id", "") == "KNOWN_ISSUES"):
            issues = ast.literal_eval(node.value)
    if not issues:
        return text, False

    out = []
    for symptom, detail in issues:
        out.append("* " + symptom)
        words, line = (detail).split(), "  "
        for w in words:
            if len(line) + len(w) + 1 > 78:
                out.append(line)
                line = "  "
            line += (" " if line.strip() else "") + w
        if line.strip():
            out.append(line)
        out.append("")
    return text[:at + len(marker)] + "\n".join(out).rstrip() + "\n", True


def parsed_keys():
    """Every key cotwvr.ini's parser actually understands (config.cpp)."""
    path = os.path.join(ROOT, "src", "cotwvr", "config.cpp")
    with open(path, encoding="utf-8") as f:
        return set(re.findall(r'_stricmp\(key,\s*"([^"]+)"\)', f.read()))


def shipped_ini(source, values):
    """The working ini's structure, the recommended set's values.

    A recommended key missing from the source file is NOT automatically wrong:
    the working ini was last written by whichever build was running at the time,
    so a setting added since is simply not in it yet. The question worth asking
    is whether the MOD understands the key - so that is the question asked. It
    is appended if it does, and reported as a typo if it does not.
    """
    with open(source, encoding="utf-8") as f:
        text = f.read()
    known = parsed_keys()

    # *** A SHIPPED FILE SHOULD NOT DESCRIBE SETTINGS THAT NO LONGER EXIST. ***
    #
    # The working ini was written by an older build, so it can still carry a key
    # that has since been removed - along with the paragraph of comment above it
    # explaining a feature the player will never find. The mod ignores the line
    # and rewrites the file without it the first time it saves, so this is
    # tidiness rather than correctness, but a settings file is documentation and
    # documentation for something that is gone is worse than none.
    lines, kept, dropped = text.splitlines(), [], []
    pending = []                       # the comment block above the current key
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("#"):
            pending.append(line)
            continue
        key = stripped.split("=", 1)[0].strip() if "=" in stripped else ""
        if key and key not in known:
            dropped.append(key)
            pending = []               # and the comment that described it
            continue
        kept.extend(pending)
        pending = []
        kept.append(line)
    kept.extend(pending)
    text = "\n".join(kept) + "\n"

    unknown, appended = [], []
    for key, value in values.items():
        pat = re.compile(r"(?m)^(\s*%s\s*=\s*).*$" % re.escape(key))
        if pat.search(text):
            text = pat.sub(lambda m: m.group(1) + str(value), text, count=1)
        elif key in known:
            text = text.rstrip("\r\n") + "\n%s = %s\n" % (key, value)
            appended.append(key)
        else:
            unknown.append(key)
    return text, unknown, appended, dropped


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="1.1.1")
    ap.add_argument("--ini", default=None)
    args = ap.parse_args()

    out_dir = os.path.join(ROOT, "release", "theHunterCotW-VR-v%s" % args.version)
    os.makedirs(out_dir, exist_ok=True)

    # *** PACKAGING MUST NOT DEPEND ON THE MOD BEING INSTALLED. ***
    #
    # The shipped ini borrows its STRUCTURE - every key, with its paragraph of
    # explanation - from an existing one, and that used to mean the copy in the
    # game folder. Which is missing the moment the mod is uninstalled to test a
    # clean install, i.e. exactly when the package is being built. The last one
    # WE generated is just as good a structure and is always here.
    source = args.ini
    if not source:
        for candidate in (GAME_INI, os.path.join(out_dir, "cotwvr.ini")):
            if os.path.exists(candidate):
                source = candidate
                break
    if not source or not os.path.exists(source):
        print("   [!] no cotwvr.ini to take the structure from - pass --ini")
        return 1
    print("   structure from %s" % source)

    files = [
        (os.path.join(BUILD, "cotwvr.dll"), "cotwvr.dll"),
        (os.path.join(BUILD, "XINPUT9_1_0.dll"), "XINPUT9_1_0.dll"),
        (os.path.join(BUILD, "openxr_loader.dll"), "openxr_loader.dll"),
        (os.path.join(ROOT, "thirdparty", "dlss", "bin", "nvngx_dlss.dll"),
         "nvngx_dlss.dll"),
        (os.path.join(LAUNCHER, "dist", "theHunterCotW VR Settings.exe"),
         "theHunterCotW VR Settings.exe"),
        (os.path.join(ROOT, "release", "README.txt"), "README.txt"),
    ]

    missing = [src for src, _ in files if not os.path.exists(src)]
    if missing:
        for src in missing:
            print("   [!] missing %s" % src)
        return 1

    for src, name in files:
        if name == "README.txt":
            text, generated = readme_with_issues(src)
            with open(os.path.join(out_dir, name), "w",
                      encoding="utf-8", newline="") as f:
                f.write(text)
            print("   %-32s %9d bytes%s" % (name, len(text),
                  "  (known issues from the launcher)" if generated else ""))
            continue
        shutil.copy2(src, os.path.join(out_dir, name))
        print("   %-32s %9d bytes" % (name, os.path.getsize(src)))

    text, unknown, appended, dropped = shipped_ini(source, launcher_tables())
    with open(os.path.join(out_dir, "cotwvr.ini"), "w",
              encoding="utf-8", newline="") as f:
        f.write(text)
    print("   %-32s %9d bytes  (generated at the recommended settings)"
          % ("cotwvr.ini", len(text)))
    for key in dropped:
        print("       dropped '%s' - the mod no longer has this setting" % key)
    for key in appended:
        print("       added '%s' - newer than the ini it was generated from" % key)
    for key in unknown:
        print("   [!] recommended key '%s' is not understood by config.cpp - "
              "a typo, or a setting that has been removed" % key)

    zip_path = out_dir + ".zip"
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(os.listdir(out_dir)):
            z.write(os.path.join(out_dir, name), name)
    print()
    print("   %s  %.1f MB" % (os.path.basename(zip_path),
                              os.path.getsize(zip_path) / 1048576.0))
    print()
    print("   BEFORE PUBLISHING: upload this zip to virustotal.com and swap the")
    print("   scan link in README.md and tools/make_nexus_description.py - the")
    print("   old link points at the OLD zip, and a scan that does not match the")
    print("   download is worse than none.")
    return 1 if unknown else 0


if __name__ == "__main__":
    sys.exit(main())
