"""Are two builds of the game the SAME CODE at the same addresses?

Every engine address in this mod is an RVA into theHunterCotW_F.exe, so a
different build of the game is a different mod. The fingerprint check refuses an
unknown build outright, which is right - but "unknown" and "incompatible" are
not the same thing, and the only way to tell them apart is to look.

This compares two executables section by section, and where a section differs it
finds the FIRST and LAST differing byte. That is the question that matters: if
the code section is identical, every RVA is valid and the other build works as
it stands; if it differs only past the end of the code, the code still lines up.

    python tools\\compare_builds.py <known.exe> <unknown.exe>
"""

import hashlib
import struct
import sys


def pe(path):
    with open(path, "rb") as f:
        data = f.read()
    e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[e_lfanew:e_lfanew + 4] == b"PE\0\0", "not a PE file: %s" % path
    coff = e_lfanew + 4
    n_sections, timestamp = struct.unpack_from("<HI", data, coff + 2)
    opt_size = struct.unpack_from("<H", data, coff + 16)[0]
    opt = coff + 20
    magic = struct.unpack_from("<H", data, opt)[0]
    size_of_image = struct.unpack_from("<I", data, opt + 56)[0]
    checksum = struct.unpack_from("<I", data, opt + 64)[0]

    sections = []
    at = opt + opt_size
    for i in range(n_sections):
        name = data[at:at + 8].rstrip(b"\0").decode("ascii", "replace")
        vsize, vaddr, rawsize, rawptr = struct.unpack_from("<IIII", data, at + 8)
        chars = struct.unpack_from("<I", data, at + 36)[0]
        sections.append({"name": name, "vsize": vsize, "vaddr": vaddr,
                         "rawsize": rawsize, "rawptr": rawptr, "chars": chars})
        at += 40
    return {"data": data, "timestamp": timestamp, "size_of_image": size_of_image,
            "checksum": checksum, "magic": magic, "sections": sections}


def body(p, s):
    return p["data"][s["rawptr"]:s["rawptr"] + s["rawsize"]]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    a, b = pe(sys.argv[1]), pe(sys.argv[2])

    print("%-22s %-24s %s" % ("", "KNOWN", "OTHER"))
    print("%-22s %-24s %s" % ("file", sys.argv[1].split("\\")[-2:][0],
                              sys.argv[2].split("\\")[-2:][0]))
    print("%-22s 0x%-22X 0x%X" % ("PE timestamp", a["timestamp"], b["timestamp"]))
    print("%-22s 0x%-22X 0x%X" % ("SizeOfImage", a["size_of_image"], b["size_of_image"]))
    print("%-22s 0x%-22X 0x%X" % ("checksum", a["checksum"], b["checksum"]))
    print("%-22s %-24d %d" % ("sections", len(a["sections"]), len(b["sections"])))
    print()

    if a["timestamp"] == b["timestamp"]:
        print("SAME PE TIMESTAMP - the same linker run produced both, so any")
        print("difference below is something applied AFTERWARDS (a store's DRM")
        print("or wrapper), not a recompile.")
        print()

    names_a = {s["name"]: s for s in a["sections"]}
    names_b = {s["name"]: s for s in b["sections"]}
    only_b = [n for n in names_b if n not in names_a]
    only_a = [n for n in names_a if n not in names_b]
    if only_a:
        print("sections only in KNOWN: %s" % ", ".join(only_a))
    if only_b:
        print("sections only in OTHER: %s" % ", ".join(only_b))
    if only_a or only_b:
        print()

    print("%-10s %-12s %-12s %s" % ("SECTION", "RVA", "SIZE", "VERDICT"))
    code_identical = True
    for name in [s["name"] for s in a["sections"]]:
        sa = names_a[name]
        sb = names_b.get(name)
        executable = bool(sa["chars"] & 0x20000000)
        if not sb:
            print("%-10s 0x%-10X %-12d MISSING from the other build" %
                  (name, sa["vaddr"], sa["rawsize"]))
            if executable:
                code_identical = False
            continue
        if sa["vaddr"] != sb["vaddr"]:
            print("%-10s 0x%-10X %-12d MOVED - other build has it at 0x%X" %
                  (name, sa["vaddr"], sa["rawsize"], sb["vaddr"]))
            if executable:
                code_identical = False
            continue
        da, db = body(a, sa), body(b, sb)
        if da == db:
            print("%-10s 0x%-10X %-12d identical%s" %
                  (name, sa["vaddr"], sa["rawsize"], "  <- CODE" if executable else ""))
            continue

        # Where does it start to differ, and where does it stop?
        n = min(len(da), len(db))
        first = next((i for i in range(n) if da[i] != db[i]), n)
        last = next((i for i in range(n - 1, -1, -1) if da[i] != db[i]), -1)
        diffs = sum(1 for i in range(n) if da[i] != db[i])
        print("%-10s 0x%-10X %-12d DIFFERS: %d bytes, first at +0x%X (RVA 0x%X), "
              "last at +0x%X%s" % (name, sa["vaddr"], sa["rawsize"], diffs,
                                   first, sa["vaddr"] + first, last,
                                   "  <- CODE" if executable else ""))
        if executable:
            code_identical = False

    print()
    if code_identical:
        print("*** EVERY EXECUTABLE SECTION IS BYTE-IDENTICAL AT THE SAME RVAs. ***")
        print("The mod's addresses are valid for the other build. It can be added")
        print("as a second known fingerprint and it will simply work.")
        return 0
    print("The code differs. The addresses cannot be trusted on the other build")
    print("without re-finding them - see where the first difference lands above.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
