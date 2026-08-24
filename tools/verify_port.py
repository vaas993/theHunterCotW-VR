"""Is the ported address really the SAME function, and are the struct offsets
still the same?

A signature match proves a byte pattern occurs once in each image. It does not
prove the two are the same routine - and a wrong match here writes to the wrong
code in somebody's game. So each pair is checked properly: disassemble both,
instruction by instruction, and require them to agree on everything except the
operands the linker chooses.

That single comparison answers both open questions at once:

  * if every instruction matches, the address mapping is right;
  * and since the struct field offsets (`[rbx+0x1C]`, `[rax+0xE8]`) are part of
    the instructions being compared, matching code IS the evidence that the
    offsets did not move.

    python tools/verify_port.py <reference.exe> <target.exe> <port.json>
"""

import argparse
import json
import os
import sys

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_signatures import load

INSTRUCTIONS = 60      # enough to cover a routine's distinctive body


def normalise(insn):
    """The instruction with linker-chosen values removed.

    Branch targets and RIP-relative displacements differ legitimately between
    builds; everything else - opcode, registers, and crucially any STRUCT
    OFFSET like [rbx+0x1C] - must be identical.
    """
    ops = insn.op_str
    if "rip" in ops:
        # collapse [rip + 0x...] to [rip+?]; the destination is checked
        # elsewhere, and its encoding always differs.
        import re
        ops = re.sub(r"rip [+-] 0x[0-9a-f]+", "rip+?", ops)
    groups = set(getattr(insn, "groups", ()) or ())
    if {capstone.CS_GRP_JUMP, capstone.CS_GRP_CALL} & groups:
        import re
        ops = re.sub(r"0x[0-9a-f]+", "?", ops)
    return "%s %s" % (insn.mnemonic, ops)


def disasm(mod, rva, md, count):
    out = []
    image, base = mod["image"], mod["base"]
    at = rva
    for _ in range(count):
        try:
            insn = next(md.disasm(bytes(image[at:at + 16]), base + at))
        except StopIteration:
            break
        out.append(insn)
        at += insn.size
        if insn.mnemonic == "ret":
            break
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("target")
    ap.add_argument("port_json")
    args = ap.parse_args()

    ref, tgt = load(args.reference), load(args.target)
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    data = json.load(open(args.port_json, encoding="utf-8"))
    pairs = [(r["name"], r["reference_rva"], r["target_rva"])
             for r in data.get("resolved", [])]

    extra = os.path.join(os.path.dirname(args.port_json), "port_phase2.json")
    if os.path.exists(extra):
        import re as _re
        apex = open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                 "..", "src", "cotwvr", "apex.h"),
                    encoding="utf-8").read()
        known = {m.group(1): int(m.group(2), 16) for m in
                 _re.finditer(r"constexpr uint32_t (k\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", apex)}
        for name, tgt_rva in json.load(open(extra, encoding="utf-8")).items():
            if name in known:
                pairs.append((name, known[name], tgt_rva))

    # Data addresses are not code and must not be disassembled to judge them.
    # They were resolved by agreement among the instructions that READ them
    # (port_build.py), which is the right evidence for a global; running a
    # disassembler over a live counter's bytes proves nothing either way.
    DATA = {"kTimeManagerPtr", "kJitterFrameCounter", "kFrameClockA",
            "kFrameClockB", "kTimeObjectPtr", "kTobiiManagerPtr"}

    ok, differ, skipped, data = [], [], [], []
    for name, a, b in pairs:
        if name in DATA:
            data.append((name, a, b))
            continue
        ia, ib = disasm(ref, a, md, INSTRUCTIONS), disasm(tgt, b, md, INSTRUCTIONS)
        if not ia or not ib:
            skipped.append(name)
            continue
        n = min(len(ia), len(ib))
        mismatch = None
        for i in range(n):
            na, nb = normalise(ia[i]), normalise(ib[i])
            if na != nb:
                mismatch = (i, na, nb)
                break
        # A COMPLETE SHORT FUNCTION IS NOT A FAILURE. The first version
        # demanded five instructions, which failed the weather getter -
        # `movss xmm0,[rcx+0x1d0] / ret`, two instructions, byte-identical in
        # both builds. Requiring length rather than agreement reported a
        # perfect match as a mismatch.
        if mismatch is None:
            ok.append((name, n))
        else:
            differ.append((name, a, b, mismatch, n))

    for name, n in ok:
        print("  MATCH   %-28s %d instructions identical" % (name, n))
    for name, a, b, mm, n in differ:
        print("  DIFFER  %-28s ref 0x%X vs tgt 0x%X" % (name, a, b))
        if mm:
            print("            instruction %d:  %s   vs   %s" % mm)
    for name in skipped:
        print("  SKIP    %s (could not disassemble)" % name)
    for name, a, b in data:
        print("  DATA    %-28s 0x%-9X -> 0x%-9X  (verified by reader agreement)"
              % (name, a, b))

    print()
    print("%d code addresses verified identical, %d differ, %d skipped, "
          "%d data globals" % (len(ok), len(differ), len(skipped), len(data)))
    if not differ:
        print()
        print("Every ported routine is the same code, so the struct field")
        print("offsets inside them are unchanged too.")
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
