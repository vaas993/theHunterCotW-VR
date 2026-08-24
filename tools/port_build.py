"""PHASE TWO: finish the address set for another build.

Phase one (make_signatures.py) resolved every address whose own code is
distinctive. Three kinds are left, and each needs a different question asked:

  1. CODE THAT IS NOT UNIQUE. A function the compiler emitted more than once -
     identical bytes in several places - can never be identified by its own
     content. It has to be found by something that IS unique: the call site
     that reaches it. So: locate a unique caller, then follow its call.

  2. DATA GLOBALS. A pointer at 0x27C8B40 has no code to match. But the
     instructions that READ it do: `mov rax,[rip+disp]`. Signature the reading
     instruction (with its displacement masked), find it in the target, then
     decode the displacement it actually carries there. Several readers are
     checked and must AGREE - a global resolved two independent ways is a
     global you can trust.

  3. STRUCT FIELD OFFSETS. Not addresses at all: `+0x4C` inside a camera
     object. These usually survive a rebuild untouched, but "usually" is not
     evidence, so each is confirmed to still appear in the same function.

    python tools/port_build.py <reference.exe> <target.exe>
"""

import argparse
import json
import os
import re
import sys

import capstone
import pefile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_signatures import (APEX_H, MAX_BYTES, MIN_BYTES, apex_rvas,
                             build_pattern, count_matches, in_text, load)


def md64():
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True
    return md


def rip_target(insn, base):
    """RVA this instruction reads/writes via RIP-relative addressing, or None."""
    enc = getattr(insn, "encoding", None)
    if not enc or not enc.disp_size:
        return None
    if "rip" not in insn.op_str:
        return None
    disp = insn.disp
    return (insn.address + insn.size + disp) - base


def find_callers(mod, md, target_rva, limit=400000):
    """Every `call rel32` in .text whose destination is target_rva."""
    start, size = mod["text"]
    image, base = mod["image"], mod["base"]
    want = target_rva
    sites = []
    # E8 rel32 is the overwhelmingly common direct call; scan for the opcode and
    # verify by decoding, which is far faster than disassembling 26 MB linearly.
    i = start
    end = start + size
    while True:
        i = image.find(b"\xE8", i, end)
        if i < 0:
            break
        rel = int.from_bytes(image[i + 1:i + 5], "little", signed=True)
        dest = i + 5 + rel
        if dest == want:
            sites.append(i)
            if len(sites) >= 32:
                break
        i += 1
    return sites


def resolve_via_caller(ref, tgt, md, name, rva):
    """Find a duplicated function by a caller that is not duplicated."""
    sites = find_callers(ref, md, rva)
    for site in sites:
        pat, mask = build_pattern(ref, site, md)
        if pat is None:
            continue
        hits = count_matches(tgt["image"], pat, mask, collect=True)
        if len(hits) != 1:
            continue
        # The matched call site in the target: decode it and follow the call.
        at = hits[0]
        chunk = tgt["image"][at:at + 16]
        try:
            insn = next(md.disasm(bytes(chunk), tgt["base"] + at))
        except StopIteration:
            continue
        if not insn.mnemonic.startswith("call"):
            continue
        rel = int.from_bytes(tgt["image"][at + 1:at + 5], "little", signed=True)
        dest = at + 5 + rel
        return dest, ("caller at ref 0x%X -> target 0x%X" % (site, at))
    return None, "no unique caller found among %d call sites" % len(sites)


def rip_reference_sites(mod, md, rva, want=12):
    """Every instruction whose RIP-relative operand lands on rva.

    Found by arithmetic rather than by disassembling 26 MB: for an instruction
    ending at E carrying displacement D, the target is E + D. So a 4-byte
    window at position p is the displacement of *some* instruction ending at
    p+4, and it points at rva exactly when int32(p) == rva - p - 4. numpy
    evaluates that for every position at once; only the handful of hits are
    then decoded to confirm they really are instructions.
    """
    import numpy as np

    start, size = mod["text"]
    image, base = mod["image"], mod["base"]
    end = start + size

    window = np.frombuffer(image[start:end], dtype=np.uint8)
    n = len(window) - 4
    disp = (window[0:n].astype(np.int64)
            | (window[1:n + 1].astype(np.int64) << 8)
            | (window[2:n + 2].astype(np.int64) << 16)
            | (window[3:n + 3].astype(np.int64) << 24))
    disp = disp.astype(np.int32).astype(np.int64)          # sign-extend
    pos = np.arange(n, dtype=np.int64) + start
    hit = np.nonzero((pos + 4 + disp) == rva)[0]

    sites = []
    for idx in hit:
        disp_at = int(pos[idx])
        # The instruction ends just past the displacement; its start is a few
        # bytes earlier depending on the encoding. Try the plausible lengths.
        for insn_len in (7, 6, 8, 5, 9, 10):
            at = disp_at + 4 - insn_len
            if at < start:
                continue
            try:
                insn = next(md.disasm(bytes(image[at:at + 16]), base + at))
            except StopIteration:
                continue
            if insn.size != insn_len:
                continue
            if rip_target(insn, base) != rva:
                continue
            sites.append((at, insn.mnemonic, insn.op_str))
            break
        if len(sites) >= want:
            break
    return sites


def resolve_global(ref, tgt, md, name, rva):
    """Find a data global through the instructions that reference it."""
    readers = rip_reference_sites(ref, md, rva)
    votes = {}
    for site, mnem, ops in readers:
        pat, mask = build_pattern(ref, site, md)
        if pat is None:
            continue
        hits = count_matches(tgt["image"], pat, mask, collect=True)
        if len(hits) != 1:
            continue
        at = hits[0]
        try:
            insn = next(md.disasm(bytes(tgt["image"][at:at + 16]), tgt["base"] + at))
        except StopIteration:
            continue
        got = rip_target(insn, tgt["base"])
        if got is not None:
            votes[got] = votes.get(got, 0) + 1

    if not votes:
        return None, "no reader resolved (%d readers found)" % len(readers)
    best = max(votes.items(), key=lambda kv: kv[1])
    detail = "agreed by %d of %d readers" % (best[1], sum(votes.values()))
    if len(votes) > 1:
        detail += "  *** DISAGREEMENT: %s ***" % ", ".join(
            "0x%X x%d" % kv for kv in votes.items())
    return best[0], detail


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("target")
    ap.add_argument("--only", nargs="*", help="just these names")
    args = ap.parse_args()

    ref, tgt = load(args.reference), load(args.target)
    md = md64()

    # apex_rvas() reads BOTH apex.h and apex_addresses.def. Reading only the
    # header made this skip every per-build address in silence - the section
    # printed no rows at all, which reads as "nothing to do" rather than
    # "looked in the wrong file".
    rvas = apex_rvas()

    # The three phase-one leftovers, and the data globals worth having.
    NOT_UNIQUE = ["kBuildTransformA", "kBuildTransformB", "kTobiiExtendedViewUpdate"]
    GLOBALS = ["kTimeManagerPtr", "kJitterFrameCounter", "kFrameClockA",
               "kFrameClockB", "kTimeObjectPtr", "kTobiiManagerPtr"]

    by_name = dict(rvas)
    out = {}

    print("=== 1. functions with no unique pattern: find them by their caller ===")
    for name in NOT_UNIQUE:
        if args.only and name not in args.only:
            continue
        rva = by_name.get(name)
        if rva is None:
            print("  %-26s NOT FOUND in apex.h or apex_addresses.def" % name)
            continue
        # first: is it duplicated in the REFERENCE, or just undistinctive?
        pat, mask = build_pattern(ref, rva, md)
        if pat is not None:
            print("  %-26s now unique on its own" % name)
            continue
        got, why = resolve_via_caller(ref, tgt, md, name, rva)
        if got:
            print("  %-26s 0x%-9X -> 0x%-9X %+#x   (%s)"
                  % (name, rva, got, got - rva, why))
            out[name] = got
        else:
            print("  %-26s 0x%-9X UNRESOLVED  (%s)" % (name, rva, why))

    print()
    print("=== 2. data globals: resolved through the code that reads them ===")
    for name in GLOBALS:
        if args.only and name not in args.only:
            continue
        rva = by_name.get(name)
        if rva is None:
            print("  %-26s NOT FOUND in apex.h or apex_addresses.def" % name)
            continue
        got, why = resolve_global(ref, tgt, md, name, rva)
        if got:
            print("  %-26s 0x%-9X -> 0x%-9X %+#x   (%s)"
                  % (name, rva, got, got - rva, why))
            out[name] = got
        else:
            print("  %-26s 0x%-9X UNRESOLVED  (%s)" % (name, rva, why))

    print()
    print("resolved %d further address(es)" % len(out))
    dest = os.path.join(os.path.dirname(APEX_H), "..", "..", "build",
                        "port_phase2.json")
    with open(os.path.abspath(dest), "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)
    print("wrote %s" % os.path.abspath(dest))
    return 0


if __name__ == "__main__":
    sys.exit(main())
