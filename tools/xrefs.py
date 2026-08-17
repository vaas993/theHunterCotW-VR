"""Find x64 RIP-relative references to an address (or to a named string).

On x64 the engine reaches its data with `lea reg,[rip+disp32]` /
`mov reg,[rip+disp32]`, so a reference to address VA appears as a 4-byte
displacement D at file offset `off` satisfying

    base + text_start + off + 4 + D == VA

Rearranged, `D + off == VA - base - text_start - 4`.  The left-hand side does
not depend on the target, so it is computed ONCE for the whole .text section
and every subsequent lookup is a vectorised comparison.  Brute-forcing per
target instead takes ~40 s a string, which makes an exploratory session
unbearable.

Caveat worth remembering: this finds full 32-bit displacements only.  An
instruction using a disp8 (target within 127 bytes) is invisible to it - the
same blind spot the Far Cry 2 notes recorded.

    python tools/xrefs.py --str CCameraManager
    python tools/xrefs.py --str CCameraObject CCameraManager --exact
    python tools/xrefs.py --va 0x141941C28
    python tools/xrefs.py --grep-str camera --exact-off
"""
import argparse
import re
import sys

import numpy as np
import pefile

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\theHunterCotW\theHunterCotW_F.exe"


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        self.data = self.pe.get_memory_mapped_image()
        sec = next(s for s in self.pe.sections if s.Name.rstrip(b"\0") == b".text")
        self.text_start = sec.VirtualAddress
        self.text_end = sec.VirtualAddress + sec.Misc_VirtualSize
        text = self.data[self.text_start:self.text_end]

        a = np.frombuffer(text, dtype=np.uint8).astype(np.int64)
        d = (a[0:-3] | (a[1:-2] << 8) | (a[2:-1] << 16) | (a[3:] << 24))
        d = d.astype(np.int64)
        d[d >= 0x80000000] -= 0x100000000          # sign-extend disp32
        self.key = d + np.arange(len(d), dtype=np.int64)   # D + off
        del a, d

    def xrefs(self, va, limit=40):
        k = va - self.base - self.text_start - 4
        idx = np.nonzero(self.key == k)[0]
        return [self.text_start + int(i) for i in idx[:limit]]

    def find_strings(self, needle, exact=False):
        out = []
        for m in re.finditer(rb"[\x20-\x7e]{3,200}", self.data):
            try:
                t = m.group().decode("ascii")
            except UnicodeDecodeError:
                continue
            if (t == needle) if exact else (needle.lower() in t.lower()):
                out.append((m.start(), t))
        return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--str", dest="strs", nargs="*", default=[])
    ap.add_argument("--va", dest="vas", nargs="*", default=[])
    ap.add_argument("--exact", action="store_true", help="string must match exactly")
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--exe", default=EXE)
    args = ap.parse_args()

    img = Image(args.exe)
    print("; base 0x%X  .text rva 0x%X..0x%X" % (img.base, img.text_start, img.text_end))
    print()

    for s in args.strs:
        found = img.find_strings(s, args.exact)
        if not found:
            print("'%s' : no such string" % s)
            continue
        for rva, txt in found:
            va = img.base + rva
            refs = img.xrefs(va, args.limit)
            print("'%s'  rva 0x%08X va 0x%X" % (txt, rva, va))
            if refs:
                for r in refs:
                    print("      xref at rva 0x%08X  (va 0x%X)" % (r, img.base + r))
            else:
                print("      (no full-disp32 xref found)")
            print()

    for v in args.vas:
        va = int(v, 0)
        refs = img.xrefs(va, args.limit)
        print("va 0x%X : %d xref(s)" % (va, len(refs)))
        for r in refs:
            print("      xref at rva 0x%08X  (va 0x%X)" % (r, img.base + r))


if __name__ == "__main__":
    main()
