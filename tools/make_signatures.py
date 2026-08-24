"""PHASE ONE of build-independent addressing: can each engine address be found
by what the code LOOKS like, instead of where it sits?

Every address in apex.h is an RVA into one specific build. A different build
moves them, and the mod then refuses everything - correctly, because a guessed
address is how you corrupt somebody's game. The way out is a signature: a byte
pattern taken from the known-good build, with the bytes the LINKER rewrites
masked out, searched for in whatever build is actually running.

WHAT GETS MASKED, AND WHY IT MATTERS. Two builds of identical source produce
different bytes wherever the compiler encoded a *relative* value:

    call 0x6445CC        E8 xx xx xx xx      rel32 to the target
    mov  rax,[rip+0x123] 48 8B 05 xx xx xx xx  displacement to the data
    movabs rax,0x14012345 48 B8 xx*8         absolute address

Mask those operand bytes and the pattern describes the INSTRUCTIONS rather than
the layout - which is exactly the part that survives relocation. Leave them in
and every pattern fails on the first call instruction, which is why the earlier
raw 24-byte attempt matched only 25 of 62.

A pattern is grown instruction by instruction until it is UNIQUE in the build it
came from. A signature that matches twice is worse than none: it resolves to
whichever copy is found first, silently.

    python tools/make_signatures.py <reference.exe> <target.exe> [--json out.json]
"""

import argparse
import json
import os
import re
import sys

import capstone
import pefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APEX_H = os.path.join(ROOT, "src", "cotwvr", "apex.h")

MIN_BYTES = 24        # never accept a pattern shorter than this
MAX_BYTES = 200       # give up past here; the address needs finding by hand


def load(path):
    pe = pefile.PE(path, fast_load=True)
    image = bytearray(pe.get_memory_mapped_image())
    base = pe.OPTIONAL_HEADER.ImageBase
    text = None
    for s in pe.sections:
        name = s.Name.rstrip(b"\0").decode("ascii", "replace")
        if name == ".text":
            text = (s.VirtualAddress, s.Misc_VirtualSize)
    return {"path": path, "image": bytes(image), "base": base, "text": text,
            "stamp": pe.FILE_HEADER.TimeDateStamp,
            "size_of_image": pe.OPTIONAL_HEADER.SizeOfImage}


def apex_rvas():
    """Every `constexpr uint32_t kName = 0x...;` in apex.h, in file order."""
    with open(APEX_H, encoding="utf-8") as f:
        src = f.read()
    out = []
    for m in re.finditer(r"constexpr uint32_t (k\w+)\s*=\s*(0x[0-9A-Fa-f]+)\s*;", src):
        out.append((m.group(1), int(m.group(2), 16)))
    return out


def in_text(mod, rva):
    if not mod["text"]:
        return False
    start, size = mod["text"]
    return start <= rva < start + size


def build_pattern(mod, rva, md):
    """Bytes + mask describing the code at rva, grown until unique.

    mask[i] True  = this byte must match
            False = the linker chose it; ignore it
    """
    image = mod["image"]
    pat, mask = bytearray(), []
    at = rva
    while len(pat) < MAX_BYTES:
        chunk = image[at:at + 16]
        if not chunk:
            break
        try:
            insn = next(md.disasm(bytes(chunk), mod["base"] + at))
        except StopIteration:
            break
        raw = bytearray(insn.bytes)
        m = [True] * len(raw)

        # insn.encoding, NOT insn.detail.x86.encoding: this capstone build
        # (binding 5.0.7 / core 5.0.1280) raises AttributeError on `.detail`
        # while every field it exposes is reachable directly. Detail IS
        # populated - `.operands` works - only that one accessor is broken.
        enc = getattr(insn, "encoding", None)
        if enc is not None:
            # RIP-relative and absolute displacements: linker-chosen.
            if enc.disp_size:
                for i in range(enc.disp_offset, enc.disp_offset + enc.disp_size):
                    if 0 <= i < len(m):
                        m[i] = False
            # Immediates: mask only the ones that encode a LOCATION - branch
            # targets and 64-bit absolutes. A real constant (a loop count, a
            # float bit pattern) is part of the code and must stay.
            if enc.imm_size:
                is_branch = bool({capstone.CS_GRP_JUMP, capstone.CS_GRP_CALL} &
                                 set(getattr(insn, "groups", ()) or ()))
                looks_absolute = enc.imm_size == 8
                if is_branch or looks_absolute:
                    for i in range(enc.imm_offset, enc.imm_offset + enc.imm_size):
                        if 0 <= i < len(m):
                            m[i] = False

        pat += raw
        mask += m
        at += insn.size

        if len(pat) >= MIN_BYTES:
            if count_matches(image, pat, mask, limit=2) == 1:
                return bytes(pat), mask
    return None, None


def count_matches(image, pat, mask, limit=None, collect=False):
    """How many places match, honouring the mask. Anchored on the longest run
    of must-match bytes so this stays fast on a 26 MB image."""
    best_start, best_len = 0, 0
    run_start, run_len = 0, 0
    for i, must in enumerate(mask):
        if must:
            if run_len == 0:
                run_start = i
            run_len += 1
            if run_len > best_len:
                best_len, best_start = run_len, run_start
        else:
            run_len = 0
    if best_len == 0:
        return 0 if not collect else []
    anchor = pat[best_start:best_start + best_len]

    hits, found, i = 0, [], 0
    while True:
        i = image.find(anchor, i)
        if i < 0:
            break
        start = i - best_start
        if start >= 0 and start + len(pat) <= len(image):
            ok = True
            for j, must in enumerate(mask):
                if must and image[start + j] != pat[j]:
                    ok = False
                    break
            if ok:
                hits += 1
                if collect:
                    found.append(start)
                if limit and hits >= limit:
                    return found if collect else hits
        i += 1
    return found if collect else hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("reference")
    ap.add_argument("target")
    ap.add_argument("--json")
    args = ap.parse_args()

    ref, tgt = load(args.reference), load(args.target)
    print("reference  %s  stamp 0x%08X" % (os.path.basename(args.reference), ref["stamp"]))
    print("target     %s  stamp 0x%08X" % (os.path.basename(args.target), tgt["stamp"]))
    print()

    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
    md.detail = True

    rvas = apex_rvas()
    resolved, ambiguous, missing, notcode, nopattern = [], [], [], [], []

    for name, rva in rvas:
        if not in_text(ref, rva):
            notcode.append((name, rva))
            continue
        pat, mask = build_pattern(ref, rva, md)
        if pat is None:
            nopattern.append((name, rva))
            continue
        hits = count_matches(tgt["image"], pat, mask, collect=True)
        if len(hits) == 1:
            delta = hits[0] - rva
            resolved.append((name, rva, hits[0], delta, len(pat)))
        elif len(hits) > 1:
            ambiguous.append((name, rva, len(hits)))
        else:
            missing.append((name, rva, len(pat)))

    print("%-30s %-11s %-11s %s" % ("ADDRESS", "REFERENCE", "TARGET", "DELTA"))
    for name, rva, hit, delta, plen in resolved:
        print("  %-28s 0x%-9X 0x%-9X %+#x" % (name, rva, hit, delta))

    print()
    print("RESOLVED   %d" % len(resolved))
    print("AMBIGUOUS  %d   %s" % (len(ambiguous), ", ".join(n for n, _, _ in ambiguous)))
    print("MISSING    %d   %s" % (len(missing), ", ".join(n for n, _, _ in missing)))
    print("NOT CODE   %d   %s" % (len(notcode), ", ".join(n for n, _ in notcode)))
    print("NO PATTERN %d   %s" % (len(nopattern), ", ".join(n for n, _ in nopattern)))
    print()
    total = len(rvas)
    print("%d of %d addresses resolved automatically (%.0f%%)" %
          (len(resolved), total, 100.0 * len(resolved) / total if total else 0))

    if args.json:
        with open(args.json, "w", encoding="utf-8") as f:
            json.dump({
                "reference": {"path": args.reference, "stamp": ref["stamp"]},
                "target": {"path": args.target, "stamp": tgt["stamp"],
                           "size_of_image": tgt["size_of_image"]},
                "resolved": [{"name": n, "reference_rva": r, "target_rva": h,
                              "delta": d, "pattern_bytes": p}
                             for n, r, h, d, p in resolved],
                "ambiguous": [n for n, _, _ in ambiguous],
                "missing": [n for n, _, _ in missing],
                "not_code": [n for n, _ in notcode],
                "no_pattern": [n for n, _ in nopattern],
            }, f, indent=2)
        print("wrote %s" % args.json)
    return 0


if __name__ == "__main__":
    sys.exit(main())
