"""Fail the build if config.cpp can read a setting it cannot write.

Four times now a key has been added to Load() and forgotten in Save(). Every
time, the setting appeared to work until the game exited, and then came back
switched off - which reads exactly like a broken feature, not a lost line. The
last occurrence cost a full test cycle chasing "no depth, just flicker" when
half the feature had turned itself off on save.

The runtime guard in VerifyRoundTrip() was supposed to catch this. It could not:
its threshold was a hand-typed number that had fallen twelve keys behind. A
check whose expected value is maintained by the same hand that forgets is not a
check, so this reads both sides out of the source instead.
"""

import re
import sys
from pathlib import Path

SRC = Path(__file__).resolve().parent.parent / "src" / "cotwvr" / "config.cpp"


def main() -> int:
    text = SRC.read_text(encoding="utf-8")

    accepted = set(re.findall(r'_stricmp\(key, "([a-z_0-9]+)"', text))
    written = set(re.findall(r'fprintf\(f, "([a-z_0-9]+) = ', text))

    ok = True

    dropped = sorted(accepted - written)
    if dropped:
        ok = False
        print("config.cpp: %d setting(s) can be READ but never WRITTEN." % len(dropped))
        print("These reset to their defaults every time the game exits:")
        for key in dropped:
            print("    %s" % key)
        print("Add an fprintf for each in Config::Save().")

    # Keys are only half of it: a value that cannot be parsed back is just as
    # dead as a missing line. Save() writes several keys in hex, and atoi() stops
    # at the 'x' and returns 0 - which zeroed the three shader CRCs that identify
    # the weapon and took a whole test cycle to find.
    if re.search(r"auto num\s*=.*atoi\(", text):
        ok = False
        print("config.cpp: num() uses atoi(), which cannot read the hex values")
        print("Save() writes (0x...). Use strtoul(value, nullptr, 0) instead.")

    hex_written = set(re.findall(r'fprintf\(f, "([a-z_0-9]+) = 0x', text))
    if hex_written and re.search(r"auto num\s*=.*atoi\(", text):
        print("Affected keys: %s" % ", ".join(sorted(hex_written)))

    # The runtime guard's threshold has to match, or it goes quiet again.
    declared = re.search(r"kKeysUnderstood = (\d+)", text)
    if declared and int(declared.group(1)) != len(accepted):
        ok = False
        print("config.cpp: kKeysUnderstood is %s but Assign() accepts %d."
              % (declared.group(1), len(accepted)))
        print("Set it to %d, or the round-trip warning stays dead." % len(accepted))

    if ok:
        print("config round-trip OK - all %d settings survive a save." % len(accepted))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
