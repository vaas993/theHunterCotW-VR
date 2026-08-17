"""Resolve an RVA to the function that contains it, using the PE .pdata table.

x64 PE images carry a RUNTIME_FUNCTION array in .pdata giving every function's
exact begin and end RVA.  That is authoritative, so it replaces the usual
"scan backwards for a prologue" heuristic - which happily returns a function
that does not contain the target address (a trap recorded in the Far Cry 2
notes).

    python tools/func_at.py 0xD8722
    python tools/func_at.py 0xD8722 0xD88F2 --callers
"""
import argparse
import struct
import sys

import numpy as np
import pefile

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\theHunterCotW\theHunterCotW_F.exe"


def load(exe):
    pe = pefile.PE(exe, fast_load=True)
    pe.parse_data_directories(
        [pefile.DIRECTORY_ENTRY["IMAGE_DIRECTORY_ENTRY_EXCEPTION"]])
    return pe


def runtime_functions(pe):
    """Return a sorted numpy array of (begin, end, unwind)."""
    sec = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".pdata")
    data = pe.get_memory_mapped_image()[
        sec.VirtualAddress: sec.VirtualAddress + sec.Misc_VirtualSize]
    n = len(data) // 12
    arr = np.frombuffer(data[:n * 12], dtype=np.uint32).reshape(n, 3)
    # trailing zero entries pad the section
    arr = arr[arr[:, 0] != 0]
    return arr[np.argsort(arr[:, 0])]


def find(arr, rva):
    i = int(np.searchsorted(arr[:, 0], rva, side="right")) - 1
    if i < 0:
        return None
    begin, end, unwind = (int(x) for x in arr[i])
    if begin <= rva < end:
        return begin, end, unwind
    return None


def call_sites(pe, target_rva, limit=40):
    """Find direct `call rel32` (E8) sites that reach target_rva."""
    base = pe.OPTIONAL_HEADER.ImageBase
    data = pe.get_memory_mapped_image()
    sec = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    start, size = sec.VirtualAddress, sec.Misc_VirtualSize
    text = data[start:start + size]

    a = np.frombuffer(text, dtype=np.uint8)
    is_e8 = np.nonzero(a[:-4] == 0xE8)[0]
    out = []
    for off in is_e8:
        rel = struct.unpack_from("<i", text, int(off) + 1)[0]
        if start + int(off) + 5 + rel == target_rva:
            out.append(start + int(off))
            if len(out) >= limit:
                break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rvas", nargs="+")
    ap.add_argument("--callers", action="store_true")
    ap.add_argument("--exe", default=EXE)
    args = ap.parse_args()

    pe = load(args.exe)
    base = pe.OPTIONAL_HEADER.ImageBase
    arr = runtime_functions(pe)
    print("; .pdata holds %d runtime functions" % len(arr))
    print()

    for r in args.rvas:
        rva = int(r, 0)
        if rva >= base:
            rva -= base
        hit = find(arr, rva)
        if not hit:
            print("rva 0x%08X : no .pdata entry (leaf//data?)" % rva)
            continue
        begin, end, _ = hit
        print("rva 0x%08X  ->  FUNCTION 0x%08X .. 0x%08X   (va 0x%X, size %d, +0x%X into it)"
              % (rva, begin, end, base + begin, end - begin, rva - begin))
        if args.callers:
            cs = call_sites(pe, begin)
            print("      direct call sites: %d" % len(cs))
            for c in cs:
                fn = find(arr, c)
                where = ("inside 0x%08X" % fn[0]) if fn else "?"
                print("        call at rva 0x%08X  (%s)" % (c, where))
        print()


main()
