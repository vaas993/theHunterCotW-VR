"""Scan theHunterCotW_F.exe for a byte pattern, the way Cheat Engine's
aobscanmodule does, and report every hit as an RVA.

Signatures survive game patches where hardcoded addresses do not, which is why
every engine address in this project is found by pattern and then recorded with
its RVA for this specific build.

    python tools/aobscan.py "89 41 30 8B 42 04 89 41 34"
    python tools/aobscan.py "F3 0F 10 7F 3C 41" --context 32

'??' or '?' is a wildcard byte.
"""
import argparse
import re
import sys

import pefile

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\theHunterCotW\theHunterCotW_F.exe"


def pattern_to_regex(pat):
    out = b""
    for tok in pat.split():
        if tok in ("??", "?", "*"):
            out += b"."
        else:
            out += re.escape(bytes([int(tok, 16)]))
    return re.compile(out, re.DOTALL)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pattern")
    ap.add_argument("--context", type=int, default=0,
                    help="also print this many bytes around each hit")
    ap.add_argument("--section", default=".text")
    ap.add_argument("--exe", default=EXE)
    args = ap.parse_args()

    pe = pefile.PE(args.exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase
    data = pe.get_memory_mapped_image()

    sec = next(s for s in pe.sections
               if s.Name.rstrip(b"\0").decode() == args.section)
    start = sec.VirtualAddress
    end = start + sec.Misc_VirtualSize
    blob = data[start:end]

    rx = pattern_to_regex(args.pattern)
    hits = [start + m.start() for m in rx.finditer(blob)]

    print("pattern : %s" % args.pattern)
    print("section : %s  rva 0x%X..0x%X" % (args.section, start, end))
    print("hits    : %d %s" % (len(hits), "(UNIQUE)" if len(hits) == 1 else
                               "(NOT UNIQUE - needs a longer signature)" if len(hits) > 1
                               else "(NOT FOUND)"))
    for h in hits:
        print("   rva 0x%08X   va 0x%X" % (h, base + h))
        if args.context:
            lo = max(0, h - args.context)
            hi = min(len(data), h + args.context)
            print("      %s" % data[lo:h].hex())
            print("   >> %s" % data[h:hi].hex())
    return 0 if hits else 1


sys.exit(main())
