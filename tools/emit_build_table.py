"""Turn the ported addresses into the mod's two-build address table.

Reads the phase-one and phase-two results and writes:

  src/cotwvr/apex_addresses.def   one line per address, one column per build
  src/cotwvr/apex.h               those constants become runtime values

WHY A .def FILE. The addresses now have to be chosen at startup rather than at
compile time, but apex.h is also the project's record of HOW each one was found
- paragraphs of it - and that documentation must not move. So the declaration in
apex.h changes in place, one line each, and only the VALUES move out to a table
where a third build is one more column rather than a third edit in fifty places.

    python tools/emit_build_table.py
"""

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src", "cotwvr")
APEX_H = os.path.join(SRC, "apex.h")
DEF = os.path.join(SRC, "apex_addresses.def")

# Resolved by reader agreement and found identical in both builds; they are not
# part of the per-build table because there is nothing to choose between.
DATA_UNCHANGED = {"kTimeManagerPtr", "kJitterFrameCounter", "kFrameClockA",
                  "kFrameClockB", "kTimeObjectPtr", "kTobiiManagerPtr"}


def main():
    p1 = json.load(open(os.path.join(ROOT, "build", "port_may15.json"),
                        encoding="utf-8"))
    p2 = json.load(open(os.path.join(ROOT, "build", "port_phase2.json"),
                        encoding="utf-8"))

    src = open(APEX_H, encoding="utf-8").read()
    current = {m.group(1): int(m.group(2), 16) for m in
               re.finditer(r"constexpr uint32_t (k\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", src)}

    ported = {}
    for r in p1["resolved"]:
        ported[r["name"]] = r["target_rva"]
    for name, rva in p2.items():
        ported[name] = rva
    for name in DATA_UNCHANGED:
        ported.pop(name, None)

    missing = [n for n in ported if n not in current]
    if missing:
        print("[!] not found in apex.h: %s" % ", ".join(missing))
        return 1

    rows = [(n, current[n], ported[n]) for n in ported]
    rows.sort(key=lambda r: r[1])

    with open(DEF, "w", encoding="utf-8", newline="\n") as f:
        f.write("""// EVERY ENGINE ADDRESS, ONCE PER BUILD.  *** GENERATED - see below. ***
//
// The mod's addresses are RVAs into theHunterCotW_F.exe, and a different build
// of the game moves them. This table carries one column per build the addresses
// have been established for; apex.cpp picks a column at startup by fingerprint,
// and every feature then uses the same names it always did.
//
// Regenerate with:
//     python tools/make_signatures.py <reference.exe> <target.exe> --json build/port_X.json
//     python tools/port_build.py      <reference.exe> <target.exe>
//     python tools/emit_build_table.py
//
// Each column was VERIFIED, not guessed: tools/verify_port.py disassembles both
// builds at every pair and requires the instructions to agree on everything
// except the operands a linker chooses. A signature match alone only proves a
// pattern is unique; it does not prove the two are the same routine.
//
//          NAME                        BUILD 0             BUILD 1
//                                      2026-07-17          2026-05-15
//                                      (game update 9.2)
""")
        for name, b0, b1 in rows:
            f.write("APEX_ADDR(%-28s 0x%08X,         0x%08X)\n" % (name + ",", b0, b1))
    print("wrote %s  (%d addresses)" % (DEF, len(rows)))

    # apex.h: the declaration changes in place; the documentation above it does
    # not move a line.
    changed = 0
    for name, b0, b1 in rows:
        pat = re.compile(r"constexpr uint32_t %s\s*=\s*0x[0-9A-Fa-f]+\s*;" % re.escape(name))
        repl = ("extern uint32_t %s;   // per build - see apex_addresses.def" % name)
        src, n = pat.subn(repl, src, count=1)
        changed += n
    open(APEX_H, "w", encoding="utf-8").write(src)
    print("apex.h: %d constants became runtime values" % changed)
    return 0


if __name__ == "__main__":
    sys.exit(main())
