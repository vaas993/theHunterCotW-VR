"""REFUTATION part 4: which NAMED constant feeds SV_POSITION, from the DXBC.

    set COTW_CAP=...  set COTW_CSV=...  set COTW_OUT=...
    qrenderdoc.exe --python tools\rdc_refute_glass4.py

Part 3 proved the glass by forward transform, but the solid viewmodel meshes
use packed/SNorm positions that a naive decode cannot reproduce exactly, so
they scored "best candidate" rather than "proved".  Two methods that need no
vertex decode are used here instead, and they have to agree:

  1. DATAFLOW ON THE SHADER ITSELF.  Disassemble the VS, find the instruction
     that writes o0 (SV_POSITION), and taint backwards through the temporaries.
     Whatever named constant-buffer variable lands in that cone IS the matrix
     that reaches clip space.  Nothing about matrix shape is assumed.

  2. q * M^-1.  Take the replayed SV_POSITION and multiply by the inverse of
     each candidate; the matrix the shader actually used gives w == 1 exactly.

Then the two runtime rules are scored against that, over every distinct vertex
shader in and around the first-person pass - the two known layouts included,
because the question is not only "does the new rule find the glass" but "does
it still get the gun right".
"""
import math
import os
import re
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(os.environ.get(
    "COTW_CAP", r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame28475.rdc"))
CSVIN = os.path.expandvars(os.environ.get(
    "COTW_CSV", r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute_glass_scope.csv"))
OUT = os.path.expandvars(os.environ.get(
    "COTW_OUT", r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute_glass4_scope.txt"))

CTX = 8
NEAR = 4
NV = 16

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def shape_of(v):
    t = getattr(v, "type", None)
    for holder in (t, getattr(t, "descriptor", None)):
        if holder is None:
            continue
        r = getattr(holder, "rows", None)
        c = getattr(holder, "columns", None)
        if r:
            return int(r), int(c or 1)
    return 0, 0


def is0001(c):
    return (abs(c[0]) < 1e-6 and abs(c[1]) < 1e-6 and abs(c[2]) < 1e-6
            and abs(c[3] - 1.0) < 1e-6)


def ruleA(blk):
    if len(blk) < 0x80:
        return None
    m = struct.unpack_from("<16f", blk, 0)
    t = (m[12], m[13], m[14])
    if not all(map(math.isfinite, t)):
        return 0
    return 0x40 if any(abs(x) > 100.0 for x in t) else 0x00


def ruleB(blk, nbytes):
    for o in (0x00, 0x40):
        if o + 64 > nbytes or o + 64 > len(blk):
            break
        m = struct.unpack_from("<16f", blk, o)
        c = (m[3], m[7], m[11], m[15])
        if not all(map(math.isfinite, c)):
            continue
        if not is0001(c):
            return o
    return None


def inv4(m):
    a = [[m[r * 4 + c] for c in range(4)] + [1.0 if r == c else 0.0
                                             for c in range(4)]
         for r in range(4)]
    for col in range(4):
        piv = max(range(col, 4), key=lambda r: abs(a[r][col]))
        if abs(a[piv][col]) < 1e-12:
            return None
        a[col], a[piv] = a[piv], a[col]
        d = a[col][col]
        a[col] = [x / d for x in a[col]]
        for r in range(4):
            if r == col:
                continue
            f = a[r][col]
            if f:
                a[r] = [x - f * y for x, y in zip(a[r], a[col])]
    return [a[r][c] for r in range(4) for c in range(4, 8)]


def rowmul(v, m):
    return [sum(v[i] * m[i * 4 + j] for i in range(4)) for j in range(4)]


TOK = re.compile(r"[A-Za-z_][A-Za-z0-9_]*(?:\[[^\]]*\])?")


def base_of(tok):
    t = tok.strip().strip(",")
    t = t.split(".")[0]
    t = re.sub(r"\[[^\]]*\]", "", t)
    return t


def taint_o0(lines):
    """Backward dataflow from o0 through the temporaries.

    Returns the set of named constant-buffer variables (and inputs) that
    SV_POSITION actually depends on."""
    instrs = []
    for ln in lines:
        s = ln.strip()
        m = re.match(r"^\d+:\s+(\S+)\s+(.*)$", s)
        if not m:
            continue
        opc, rest = m.group(1), m.group(2)
        ops = [o.strip() for o in rest.split(",")]
        if not ops:
            continue
        instrs.append((opc, ops[0], ops[1:]))
    need = {"o0"}
    feed = set()
    for opc, dst, srcs in reversed(instrs):
        db = base_of(dst)
        if db not in need:
            continue
        for s in srcs:
            for t in TOK.findall(s):
                b = base_of(t)
                if b in ("l", "abs", "neg", "sat"):
                    continue
                if re.match(r"^[rvox]\d+$", b):
                    need.add(b)
                elif re.match(r"^r\d+$", b):
                    need.add(b)
                else:
                    feed.add(b)
    return feed, need


def main():
    say("capture: %s" % CAP)
    rows = []
    with open(CSVIN) as f:
        hdr = f.readline().strip().split(",")
        for line in f:
            p = line.strip().split(",")
            if len(p) != len(hdr):
                continue
            r = dict(zip(hdr, p))
            for k in ("i", "eid", "indices", "inst", "sEn", "readMask", "ref",
                      "tagged", "blend"):
                r[k] = int(r[k])
            rows.append(r)

    tagidx = [r["i"] for r in rows if r["tagged"]]
    tset = set(tagidx)
    glassvs = set(r["vs"] for r in rows
                  if r["blend"] and any(abs(r["i"] - t) <= NEAR for t in tset))
    window = list(range(max(0, min(tagidx) - CTX),
                        min(len(rows) - 1, max(tagidx) + CTX) + 1))
    pool = [rows[i] for i in window] + [r for r in rows if r["vs"] in glassvs]

    # one representative draw per distinct vertex shader
    firstvs = {}
    for r in pool:
        firstvs.setdefault(r["vs"], r)
    say("distinct vertex shaders in/around the first-person pass: %d"
        % len(firstvs))

    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return

    bufsize = {}
    for b in controller.GetBuffers():
        bufsize[str(b.resourceId)] = int(b.length)

    leaves = []

    def walk(lst):
        for a in lst:
            kids = getattr(a, "children", [])
            if kids:
                walk(kids)
            else:
                leaves.append(a)

    walk(controller.GetRootActions())
    draws = sorted([a for a in leaves if a.flags & rd.ActionFlags.Drawcall],
                   key=lambda a: a.eventId)

    scoreA = [0, 0]
    scoreB = [0, 0]
    verdicts = []

    for vs, r in sorted(firstvs.items(), key=lambda kv: kv[1]["i"]):
        i, eid = r["i"], r["eid"]
        say("")
        say("=" * 92)
        say("VS %s  first at i=%d eid=%d  %d indices  %s%s"
            % (vs, i, eid, r["indices"], "TAGGED " if r["tagged"] else "",
               "blended" if r["blend"] else ""))
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        try:
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        except Exception as e:
            say("  reflection FAILED %s: %s" % (type(e).__name__, e))
            continue

        # ---- disassembly + dataflow -----------------------------------
        dis = ""
        for tgt in ("", "DXBC"):
            try:
                dis = controller.DisassembleShader(rd.ResourceId.Null(), refl, tgt)
                if dis:
                    break
            except Exception as e:
                say("  DisassembleShader(%r) FAILED %s: %s"
                    % (tgt, type(e).__name__, e))
        lines = dis.split("\n") if dis else []
        feed, cone = taint_o0(lines)
        decls = [l.strip() for l in lines if "dcl_constantbuffer" in l]
        for d in decls:
            say("  %s" % d)
        say("  SV_POSITION depends on named constants: %s"
            % (", ".join(sorted(feed)) if feed else "(none found)"))
        pos_lines = [l.strip() for l in lines
                     if re.search(r"\bo0\b", l) and ":" in l]
        for l in pos_lines[:3]:
            say("     writes o0: %s" % l)

        # ---- blocks ----------------------------------------------------
        used = []
        try:
            for ud in pipe.GetConstantBlocks(rd.ShaderStage.Vertex):
                acc = getattr(ud, "access", None)
                d = getattr(ud, "descriptor", None)
                if d is None:
                    continue
                used.append((int(getattr(acc, "index", 1 << 30)),
                             d.resource, int(d.byteOffset), int(d.byteSize)))
        except Exception as e:
            say("  GetConstantBlocks FAILED %s: %s" % (type(e).__name__, e))
        used.sort(key=lambda t: t[0])

        b1data, b1name, b1decl, b1buf, b1off = b"", "-", 0, None, 0
        named = []
        for bi, cb in enumerate(refl.constantBlocks):
            if bi >= len(used):
                continue
            pos, rid, boff, bsz = used[bi]
            want = min(int(cb.byteSize), 65536)
            try:
                data = bytes(controller.GetBufferData(rid, boff, want))
            except Exception as e:
                say("  GetBufferData FAILED %s: %s" % (type(e).__name__, e))
                continue
            if int(cb.fixedBindNumber) == 1:
                b1data, b1name, b1decl = data, cb.name, int(cb.byteSize)
                b1buf, b1off = rid, boff
            say("  block %-20s b%-2d %6dB declared, buffer %s real %sB, "
                "bindOffset %d, read %dB"
                % (cb.name, int(cb.fixedBindNumber), cb.byteSize, str(rid),
                   bufsize.get(str(rid), "?"), boff, len(data)))
            for v in cb.variables:
                nr, nc = shape_of(v)
                o = int(v.byteOffset)
                if nr >= 4 and nc >= 4 and o + 64 <= len(data):
                    m = list(struct.unpack_from("<16f", data, o))
                    if all(map(math.isfinite, m)):
                        named.append((cb.name, int(cb.fixedBindNumber), o,
                                      v.name, m))

        # ---- q * M^-1 --------------------------------------------------
        clip = []
        try:
            pv = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            if pv and pv.vertexResourceId != rd.ResourceId.Null():
                st = int(pv.vertexByteStride)
                n = min(int(getattr(pv, "numIndices", 0)) or 0, NV)
                raw = bytes(controller.GetBufferData(pv.vertexResourceId,
                                                     int(pv.vertexByteOffset),
                                                     st * n))
                for k in range(n):
                    if (k + 1) * st <= len(raw):
                        q = struct.unpack_from("<4f", raw, k * st)
                        if all(map(math.isfinite, q)):
                            clip.append(q)
        except Exception as e:
            say("  postVS FAILED %s: %s" % (type(e).__name__, e))

        winner = None
        if clip:
            say("  q * M^-1 (w must be exactly 1.0 for the real clip matrix):")
            for (bn, slot, o, vn, m) in named:
                mi = inv4(m)
                if mi is None:
                    say("     %-28s b%d +0x%04X   singular" % (vn, slot, o))
                    continue
                ws = [rowmul(list(q), mi)[3] for q in clip]
                mean = sum(ws) / len(ws)
                spread = max(ws) - min(ws)
                ok = abs(mean - 1.0) < 1e-4 and spread < 1e-4
                say("     %-28s b%d +0x%04X   w mean %10.6f spread %.2e%s"
                    % (vn, slot, o, mean, spread,
                       "   <<< CLIP MATRIX" if ok else ""))
                if ok and slot == 1 and winner is None:
                    winner = (o, vn, m)

        rA = ruleA(b1data) if b1data else None
        rB = ruleB(b1data, len(b1data)) if b1data else None
        say("  b1 = %s (%dB declared, real %sB, bindOffset %d)"
            % (b1name, b1decl, bufsize.get(str(b1buf), "?"), b1off))
        say("  rule A (shipping) -> %s ; rule B (proposed) -> %s"
            % ("-" if rA is None else "0x%02X" % rA,
               "none" if rB is None else "0x%02X" % rB))
        if winner:
            say("  PROVEN clip matrix in b1: %s at +0x%02X" % (winner[1], winner[0]))
            agreeA = (rA == winner[0])
            agreeB = (rB == winner[0])
            scoreA[0 if agreeA else 1] += 1
            scoreB[0 if agreeB else 1] += 1
            say("  -> rule A %s ; rule B %s"
                % ("CORRECT" if agreeA else "WRONG",
                   "CORRECT" if agreeB else "WRONG"))
            verdicts.append((vs, i, eid, b1name, b1decl, winner[0], rA, rB,
                             sorted(feed)))
            # does the DXBC agree with the numeric winner?
            if feed and winner[1] not in feed:
                say("  !! DATAFLOW DISAGREES: o0 depends on %s, not %s"
                    % (", ".join(sorted(feed)), winner[1]))
            elif feed:
                say("  dataflow agrees: o0 depends on %s" % winner[1])
        else:
            say("  no b1 matrix proved for this shader")

    say("")
    say("=" * 92)
    say("== per-shader verdict ==")
    say("%-16s %-6s %-8s %-18s %-6s %-7s %-6s %-6s %s"
        % ("vs", "i", "eid", "b1 block", "decl", "PROVEN", "ruleA", "ruleB",
           "o0 depends on"))
    for v in verdicts:
        say("%-16s %-6d %-8d %-18s %-6d +0x%02X   %-6s %-6s %s"
            % (v[0], v[1], v[2], v[3], v[4], v[5],
               "-" if v[6] is None else "0x%02X" % v[6],
               "none" if v[7] is None else "0x%02X" % v[7],
               ", ".join(v[8])))
    say("")
    say("rule A: %d correct, %d wrong" % tuple(scoreA))
    say("rule B: %d correct, %d wrong" % tuple(scoreB))
    say("")
    say("DONE")
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    say("EXCEPTION:")
    say(traceback.format_exc())

out.close()
os._exit(0)
