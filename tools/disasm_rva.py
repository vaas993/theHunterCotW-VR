"""Disassemble around an RVA in theHunterCotW_F.exe.

A GUI disassembler is better for exploring; this is better for the repetitive
"what does the code at this address do" lookups that dominate a camera hunt.

    python tools/disasm_rva.py 0x1234ABC            # 60 instructions from there
    python tools/disasm_rva.py 0x1234ABC -n 200     # more
    python tools/disasm_rva.py 0x1234ABC --back 40  # start 40 bytes earlier
"""
import argparse
import sys

import capstone
import pefile

EXE = r"C:\Program Files (x86)\Steam\steamapps\common\theHunterCotW\theHunterCotW_F.exe"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rva", help="RVA, e.g. 0x1234ABC (or a VA with base 0x140000000)")
    ap.add_argument("-n", type=int, default=60, help="instruction count")
    ap.add_argument("--back", type=int, default=0, help="start this many bytes earlier")
    ap.add_argument("--exe", default=EXE)
    args = ap.parse_args()

    pe = pefile.PE(args.exe, fast_load=True)
    base = pe.OPTIONAL_HEADER.ImageBase

    rva = int(args.rva, 0)
    if rva >= base:          # a VA was passed
        rva -= base
    rva -= args.back

    data = pe.get_memory_mapped_image()
    if rva >= len(data):
        sys.exit("RVA 0x%X is outside the image (size 0x%X)" % (rva, len(data)))

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    chunk = data[rva:rva + args.n * 15]
    print("; %s" % args.exe)
    print("; base 0x%X   rva 0x%X   va 0x%X" % (base, rva, base + rva))
    print()
    for i, ins in enumerate(md.disasm(chunk, base + rva)):
        if i >= args.n:
            break
        print("%016X  %-8X  %-24s %-10s %s" % (
            ins.address, ins.address - base,
            ins.bytes.hex(), ins.mnemonic, ins.op_str))


if __name__ == "__main__":
    main()
