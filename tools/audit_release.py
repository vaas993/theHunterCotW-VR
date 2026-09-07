"""Check the release folder before it goes anywhere.

Assembling a package and shipping a package are different acts. This is the
second one: it re-asks, of the files that will actually be uploaded, the
questions that are easy to assume the answer to.

  * is every binary the one that was just built, by hash - not by date
  * is the settings file free of diagnostics and of one machine's state
  * does anything in it name a person, a drive or a folder from this machine
  * does the README still describe settings the mod no longer has
  * is the zip complete, and does it match the folder beside it

Exit code is non-zero if anything fails, so it can gate an upload.
"""

import hashlib
import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Words that must not reach a stranger's machine: this machine's drives, the
# author's own paths, and the names of the working folders.
PRIVATE = [
    re.compile(r"[A-Za-z]:\\+(?:Users|Games Mods|SteamLibrary)", re.I),
    re.compile(r"\bvaas\b(?!993)", re.I),          # the account, not the alias
    re.compile(r"AppData\\+Local\\+Temp", re.I),
]

# A setting whose name says it is a diagnostic must not ship switched on.
#
# ONLY THE SWITCHES. The first version matched by name alone and reported five
# failures that were all HOW OFTEN and HOW LONG - camera_probe_every = 300 is a
# period, and it is inert while camera_probe is 0. An audit that cries wolf on
# its own first run is an audit that gets ignored, so what counts as a switch is
# read from the declaration rather than guessed from the name.
DIAG_NAME = re.compile(r"^[a-z_]*(?:probe|verbose|dump|scan|_log)[a-z_]*$")


def diagnostic_switches():
    """Diagnostic settings declared bool in config.h - the ones with an on."""
    with open(os.path.join(ROOT, "src", "cotwvr", "config.h"),
              encoding="utf-8") as f:
        return {n for n in re.findall(r"^\s*bool\s+(\w+)\s*=", f.read(), re.M)
                if DIAG_NAME.match(n)}


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:12].upper()


def main():
    version = sys.argv[1] if len(sys.argv) > 1 else "1.1.1"
    folder = os.path.join(ROOT, "release", "theHunterCotW-VR-v%s" % version)
    zip_path = folder + ".zip"
    fails, notes = [], []

    if not os.path.isdir(folder):
        print("no such release folder: %s" % folder)
        return 1

    # -- 1. the binaries are the ones just built ---------------------------
    built = {
        "cotwvr.dll": os.path.join(ROOT, "build", "cotwvr.dll"),
        "XINPUT9_1_0.dll": os.path.join(ROOT, "build", "XINPUT9_1_0.dll"),
        "openxr_loader.dll": os.path.join(ROOT, "build", "openxr_loader.dll"),
        os.path.join("VR Settings", "theHunterCotW VR Settings.exe"): os.path.join(
            ROOT, "launcher", "dist", "VR Settings", "theHunterCotW VR Settings.exe"),
    }
    print("BINARIES")
    for name, src in built.items():
        dst = os.path.join(folder, name)
        if not os.path.exists(dst):
            fails.append("%s is missing from the package" % name); continue
        if not os.path.exists(src):
            notes.append("%s has no build to compare against" % name); continue
        a, b = sha(src), sha(dst)
        ok = a == b
        print("   %-32s %s %s" % (name, b, "" if ok else "!= built %s" % a))
        if not ok:
            fails.append("%s in the package is not the file that was built" % name)

    # -- 2. the settings file ---------------------------------------------
    ini = os.path.join(folder, "cotwvr.ini")
    print("SETTINGS FILE")
    if not os.path.exists(ini):
        fails.append("cotwvr.ini is missing")
    else:
        text = open(ini, encoding="utf-8", errors="replace").read()
        switches = diagnostic_switches()
        values = dict(re.findall(r"^\s*(\w+)\s*=\s*([^\s#]+)", text, re.M))
        on = [(k, values[k]) for k in sorted(switches)
              if k in values and values[k] not in ("0", "false")]
        for k, v in on:
            fails.append("diagnostic '%s = %s' would ship switched on" % (k, v))
        seen = re.search(r"^\s*welcome_seen\s*=\s*(\S+)", text, re.M)
        if not seen or seen.group(1) != "0":
            fails.append("welcome_seen is not 0 - new players would never see "
                         "the first-run page")
        print("   %d diagnostic switches, all off" % len(switches)
              if not on else "   %d diagnostic switches ON" % len(on))
        print("   welcome_seen = %s" % (seen.group(1) if seen else "?"))

    # -- 3. nothing from this machine in any text file ---------------------
    print("PRIVACY")
    hits = 0
    for name in sorted(os.listdir(folder)):
        path = os.path.join(folder, name)
        if not name.lower().endswith((".txt", ".ini", ".md", ".cfg")):
            continue
        text = open(path, encoding="utf-8", errors="replace").read()
        for pat in PRIVATE:
            for m in pat.finditer(text):
                hits += 1
                fails.append("%s carries '%s' from this machine"
                             % (name, m.group(0)))
    print("   %d text files scanned, %d leaks" % (
        sum(1 for n in os.listdir(folder)
            if n.lower().endswith((".txt", ".ini", ".md", ".cfg"))), hits))

    # -- 4. the README describes settings that exist -----------------------
    print("README")
    readme = os.path.join(folder, "README.txt")
    if not os.path.exists(readme):
        fails.append("README.txt is missing")
    else:
        cfg = open(os.path.join(ROOT, "src", "cotwvr", "config.cpp"),
                   encoding="utf-8").read()
        known = set(re.findall(r'_stricmp\(key,\s*"([^"]+)"\)', cfg))
        text = open(readme, encoding="utf-8", errors="replace").read()
        # Only words that LOOK like ini keys - two or more underscores - so
        # ordinary prose is not mistaken for a setting name.
        for word in set(re.findall(r"\b[a-z]+_[a-z_]{3,}\b", text)):
            if word.count("_") >= 1 and word not in known and "_" in word:
                if re.match(r"^(steamapps|rungameid|theHunterCotW)", word):
                    continue
                if re.search(r"%s\." % re.escape(word), text):
                    continue           # it is a filename, not a setting
                notes.append("README mentions '%s', which is not a setting "
                             "the mod has" % word)
        print("   %d characters, %d sections" % (
            len(text), text.count("\n---")))

    # -- 5. the zip matches the folder ------------------------------------
    print("ZIP")
    if not os.path.exists(zip_path):
        fails.append("the zip is missing")
    else:
        with zipfile.ZipFile(zip_path) as z:
            inside = {i.filename: i.file_size for i in z.infolist()}
        on_disk = []
        for root, _dirs, names in os.walk(folder):
            for n in names:
                full = os.path.join(root, n)
                on_disk.append((os.path.relpath(full, folder).replace("\\", "/"), full))
        for name, full in on_disk:
            size = os.path.getsize(full)
            if name not in inside:
                fails.append("%s is in the folder but not in the zip" % name)
            elif inside[name] != size:
                fails.append("%s in the zip is a different size" % name)
        print("   %d files, %.1f MB" % (len(inside),
                                        os.path.getsize(zip_path) / 1048576.0))

    print()
    for n in notes:
        print("   note: %s" % n)
    if fails:
        print()
        for f in fails:
            print("   FAIL: %s" % f)
        print("\n%d problem(s) - not ready to share." % len(fails))
        return 1
    print("Ready to share.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
