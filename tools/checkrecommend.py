"""ONE RECOMMENDED CONFIGURATION, NOT TWO.

There are two buttons in this mod that claim to put you on the tested settings:
"Use the tested settings" in the in-game panel (overlay.cpp, RecommendAct) and
"Use recommended" in the launcher (cotwvr_launcher.py). They are written in
different languages, in different files, by hand.

Two recommendations that disagree are worse than either one alone: press both
and you end up on a configuration nobody has ever run. And the disagreement is
invisible - each button does exactly what it says, and only a player who uses
both ever sees the result.

So they are compared here, at build time:

  CONFLICT   both set the same ini key to different values  -> build fails
  MISSING    the panel sets a key the launcher never mentions -> build fails,
             because the launcher's button would then leave that setting alone
             and quietly hand back a different configuration

The launcher is allowed to set keys the panel does not - the render resolution
and the field of view only take effect at startup, so the panel has no business
offering them.
"""

import ast
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OVERLAY = os.path.join(ROOT, "src", "cotwvr", "overlay.cpp")
LAUNCHER = os.path.join(ROOT, "launcher", "cotwvr_launcher.py")

# The dicts in the launcher that together make up "Use recommended". Every one
# of them is a plain literal, so they can be read without running tkinter.
LAUNCHER_DICTS = ("RECOMMENDED", "RECOMMENDED_MOD_EXTRA")


def panel_values():
    """Every `c.key = value;` inside RecommendAct()."""
    with open(OVERLAY, encoding="utf-8") as f:
        text = f.read()
    start = text.find("void RecommendAct(")
    if start < 0:
        sys.exit("checkrecommend: RecommendAct not found in overlay.cpp")
    end = text.find("\nvoid ", start + 1)
    body = text[start:end if end > 0 else len(text)]
    out = {}
    for key, value in re.findall(r"\bc\.(\w+)\s*=\s*([^;]+);", body):
        out[key] = value.strip()
    return out


def launcher_values():
    """Everything "Use recommended" writes - including what it DERIVES.

    The graphics values are not typed out: they are looked up in the table of
    what the game's own menu should read, so the list a player sees and the
    value written to the ini cannot disagree. That derivation has to be
    repeated here or those keys are invisible to this check - which is how
    GraphicsAA, the one setting DLSS cannot run without, sat at FXAA-alone in
    a shipped package with both "recommended" buttons reporting agreement.
    """
    with open(LAUNCHER, encoding="utf-8") as f:
        tree = ast.parse(f.read())
    out, menu, applies = {}, {}, {}
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        name = getattr(node.targets[0], "id", "")
        if name in LAUNCHER_DICTS:
            out.update(ast.literal_eval(node.value))
        elif name == "RECOMMENDED_GAME_MENU":
            menu = ast.literal_eval(node.value)
        elif name == "MOD_APPLIES_GRAPHICS":
            applies = ast.literal_eval(node.value)
    out["apply_game_graphics"] = "1"
    for game_key, ini_key in applies.items():
        if game_key in menu:
            out[ini_key] = menu[game_key]
    return out


def same(a, b):
    """Do a C++ literal and an ini string mean the same setting?"""
    a = a.strip().rstrip("f").strip()
    b = b.strip().rstrip("f").strip()
    if a in ("true", "false"):
        a = "1" if a == "true" else "0"
    if b in ("true", "false"):
        b = "1" if b == "true" else "0"
    try:
        return abs(float(a) - float(b)) < 1e-4
    except ValueError:
        return a == b


def main():
    panel = panel_values()
    launcher = launcher_values()

    conflicts, missing = [], []
    for key, value in sorted(panel.items()):
        if key not in launcher:
            missing.append((key, value))
        elif not same(value, launcher[key]):
            conflicts.append((key, value, launcher[key]))

    if not conflicts and not missing:
        print("checkrecommend.py: the panel and the launcher recommend the same "
              "%d settings." % len(panel))
        return 0

    print("checkrecommend.py: the two 'recommended' buttons disagree.")
    for key, panel_value, launcher_value in conflicts:
        print("   CONFLICT  %-28s panel %-10s launcher %s"
              % (key, panel_value, launcher_value))
    for key, panel_value in missing:
        print("   MISSING   %-28s panel sets %s, the launcher does not mention it"
              % (key, panel_value))
    print()
    print("   Decide which value is right, then put it in BOTH:")
    print("     %s   (RecommendAct)" % os.path.relpath(OVERLAY, ROOT))
    print("     %s   (RECOMMENDED / RECOMMENDED_MOD_EXTRA)"
          % os.path.relpath(LAUNCHER, ROOT))
    return 1


if __name__ == "__main__":
    sys.exit(main())
