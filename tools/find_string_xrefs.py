"""Find ASCII/UTF-16 strings in theHunterCotW_F.exe and the code that references them.

The Apex engine names things in data tables (property names, class names, event
names), and those names are the cheapest route into the code that uses them.
This is how the Far Cry 2 work found the player type registry.

    python tools/find_string_xrefs.py Camera
    python tools/find_string_xrefs.py --list fov camera view
    python tools/find_string_xrefs.py --exact "CPlayer"

x64 references strings with RIP-relative LEA, so an xref is found by computing,
for every `lea reg, [rip+disp32]`, whether the target equals the string's VA.
Scanning every instruction is slow, so this searches the raw disp32 encodings
instead - which misses nothing that uses a full 32-bit displacement.
"""
import argparse
import re
import struct
import sys

import pefile

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\theHunterCotW\theHunterCotW_F.exe"


def find_strings(data, needle, exact, encoding):
    """Yield (rva, text) for strings containing/equal to needle."""
    out = []
    if encoding == "ascii":
        pat = re.compile(rb"[\x20-\x7e]{3,200}")
        step = 1
    else:
        pat = re.compile(rb"(?:[\x20-\x7e]\x00){3,200}")
        step = 1
    for m in pat.finditer(data):
        raw = m.group()
        try:
            text = raw.decode("utf-16-le" if encoding == "utf16" else "ascii")
        except UnicodeDecodeError:
            continue
        text = text.rstrip("\x00")
        hit = (text == needle) if exact else (needle.lower() in text.lower())
        if hit:
            out.append((m.start(), text))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("needles", nargs="+")
    ap.add_argument("--exact", action="store_true")
    ap.add_argument("--utf16", action="store_true")
    ap.add_argument("--max-strings", type=int, default=40)
    ap.add_argument("--max-xrefs", type=int, default=20)
    ap.add_argument("--exe", default=EXE)
    args = ap.parse_args()

    pe = pefile.PE(args.exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    data = pe.get_memory_mapped_image()

    text_sec = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    text_start = text_sec.VirtualAddress
    text_end = text_start + text_sec.Misc_VirtualSize
    text = data[text_start:text_end]

    for needle in args.needles:
        strs = find_strings(data, needle, args.exact, "utf16" if args.utf16 else "ascii")
        print("=" * 70)
        print("'%s' -> %d string(s)" % (needle, len(strs)))
        for rva, txt in strs[:args.max_strings]:
            va = base + rva
            # lea r64, [rip+disp32] -> target = next_insn_va + disp32.
            # Search the .text for any disp32 that resolves to this string.
            xrefs = []
            for off in range(0, len(text) - 4):
                disp = struct.unpack_from("<i", text, off)[0]
                # the disp32 field ends at off+4, so the next instruction VA is
                # base + text_start + off + 4
                if base + text_start + off + 4 + disp == va:
                    xrefs.append(text_start + off)
                    if len(xrefs) >= args.max_xrefs:
                        break
            print("  rva 0x%08X va 0x%X  %-40r  xrefs: %s" % (
                rva, va, txt[:40],
                ", ".join("0x%X" % x for x in xrefs) if xrefs else "(none found)"))


if __name__ == "__main__":
    main()
