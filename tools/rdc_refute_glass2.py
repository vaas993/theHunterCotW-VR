"""REFUTATION part 2: per-draw ground truth, measured not inferred.

Run headlessly:
    set COTW_CAP=...\cotw_frame28475.rdc
    set COTW_CSV=...\rdc_refute_glass_scope.csv      (from part 1)
    set COTW_OUT=...\rdc_refute_glass2_scope.txt
    qrenderdoc.exe --python tools\rdc_refute_glass2.py

Part 1 established the frame's stencil tag and the blended neighbours.  Its
phase 2 died on d3d.vertexShader.constantBuffers, which does not exist in 1.45 -
so the slot mapping here comes from the descriptor API, ordered by descriptor
POSITION (access.index), which lines up with refl.constantBlocks in DECLARATION
order.  Where a shader declares exactly one block the mapping is unambiguous and
that is the case that matters (the glass VS).

GROUND TRUTH is the forward transform: take the real VSIn POSITION and the real
VSOut SV_POSITION for the same vertices, and find which 64-byte window of which
constant block reproduces SV_POSITION.  That cannot be satisfied by a matrix
that merely looks projective.

Reported per draw:
  * every constant block: name, b#, declared size, bound buffer, BIND OFFSET,
    and the buffer's REAL ByteWidth (the mod snapshots only >= 256 B buffers)
  * every 4x4 by name at its reflected offset, with its column 3
  * the winning window (ground truth) and its residual
  * rule A (shipping) and rule B (proposed) verdicts
  * LooksLikeCloseModel() emulated on +0x00 - does PatchWeaponMatrix fire here
  * ORTHOGRAPHIC check: is any ground-truth clip matrix affine in column 3?
    that is the one shape that breaks rule B
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(os.environ.get(
    "COTW_CAP", r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame28475.rdc"))
CSVIN = os.path.expandvars(os.environ.get(
    "COTW_CSV", r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute_glass_scope.csv"))
OUT = os.path.expandvars(os.environ.get(
    "COTW_OUT", r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute_glass2_scope.txt"))

CTX = 32
NEAR = 4
NV = 24

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def en(x):
    return str(x).split(".")[-1].split(":")[0]


def shape_of(v):
    t = getattr(v, "type", None)
    for holder in (t, getattr(t, "descriptor", None)):
        if holder is None:
            continue
        r = getattr(holder, "rows", None)
        c = getattr(holder, "columns", None)
        if r:
            nm = getattr(holder, "name", None) or getattr(holder, "baseType", "")
            return int(r), int(c or 1), str(nm)
    return 0, 0, "?"


def rowmul(v, m):
    return [sum(v[i] * m[i * 4 + j] for i in range(4)) for j in range(4)]


def colmul(m, v):
    return [sum(m[j * 4 + i] * v[i] for i in range(4)) for j in range(4)]


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


def looks_close(m):
    """cbscan.cpp LooksLikeCloseModel, exactly."""
    if len(m) < 16:
        return False, 0.0, "short"
    if any(abs(m[i]) >= 1e-4 for i in (3, 7, 11)):
        return False, 0.0, "not affine"
    if abs(m[15] - 1.0) > 1e-3:
        return False, 0.0, "m15 != 1"
    for r in range(3):
        v = m[r * 4:r * 4 + 3]
        l2 = sum(x * x for x in v)
        if not math.isfinite(l2) or l2 < 0.04 or l2 > 25.0:
            return False, 0.0, "basis row %d len2 %.4f" % (r, l2)
    d = math.sqrt(m[12] ** 2 + m[13] ** 2 + m[14] ** 2)
    if not math.isfinite(d) or d < 0.05 or d > 2.0:
        return False, d, "|t| %.4f outside 0.05..2.0" % d
    return True, d, "FIRES"


def main():
    say("capture: %s" % CAP)
    say("draw list from: %s" % CSVIN)
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
    say("%d draws in the csv" % len(rows))

    tagidx = [r["i"] for r in rows if r["tagged"]]
    lo = max(0, min(tagidx) - CTX)
    hi = min(len(rows) - 1, max(tagidx) + CTX)
    window = set(range(lo, hi + 1))

    # every VS used by a blended draw next to the tagged pass = glass candidates
    tset = set(tagidx)
    glassvs = set(r["vs"] for r in rows
                  if r["blend"] and any(abs(r["i"] - t) <= NEAR for t in tset))
    say("blended-near VS ids: %s" % ", ".join(sorted(glassvs)))
    allglass = [r for r in rows if r["vs"] in glassvs]
    say("draws ANYWHERE in the frame using one of those VS ids: %d" % len(allglass))
    for r in allglass:
        say("   i=%-5d eid=%-7d idx=%-7d blend=%d tagged=%d vs=%s"
            % (r["i"], r["eid"], r["indices"], r["blend"], r["tagged"], r["vs"]))

    targets = sorted(set(list(window) + [r["i"] for r in allglass]))
    say("deep set: %d draws (window %d..%d plus every glass-VS draw)"
        % (len(targets), lo, hi))

    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return

    bufsize = {}
    consts192 = consts128 = 0
    try:
        for b in controller.GetBuffers():
            bufsize[str(b.resourceId)] = int(b.length)
            if int(b.length) == 192:
                consts192 += 1
            elif int(b.length) == 128:
                consts128 += 1
        say("GetBuffers: %d buffers, %d of them exactly 192 B, %d exactly 128 B"
            % (len(bufsize), consts192, consts128))
    except Exception as e:
        say("GetBuffers FAILED %s: %s" % (type(e).__name__, e))

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
    by_i = {i: a for i, a in enumerate(draws)}

    score = {"A": [0, 0], "B": [0, 0]}
    failA, failB = [], []
    ortho = []
    detail_for = set(r["i"] for r in allglass)
    seen_layout = {}
    say("")
    say("=" * 100)
    say("%-6s %-8s %-7s %-8s %-16s %-6s %-7s %-6s %-6s %-6s %s"
        % ("i", "eid", "kind", "indices", "b1 block", "decl", "realBuf",
           "bindOf", "TRUTH", "ruleA", "ruleB / notes"))

    for i in targets:
        a = by_i.get(i)
        if a is None:
            continue
        r = rows[i]
        eid = a.eventId
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        try:
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        except Exception as e:
            say("  i=%-5d eid=%-7d reflection FAILED %s: %s"
                % (i, eid, type(e).__name__, e))
            continue

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
            say("  i=%-5d GetConstantBlocks FAILED %s: %s"
                % (i, type(e).__name__, e))
        used.sort(key=lambda t: t[0])

        blocks = []
        for bi, cb in enumerate(refl.constantBlocks):
            if bi >= len(used):
                blocks.append((cb, None, 0, 0, b""))
                continue
            pos, rid, boff, bsz = used[bi]
            want = min(max(int(cb.byteSize), 16), 65536)
            try:
                data = bytes(controller.GetBufferData(rid, boff, want))
            except Exception as e:
                say("  i=%-5d GetBufferData FAILED %s: %s"
                    % (i, type(e).__name__, e))
                data = b""
            blocks.append((cb, rid, boff, bsz, data))

        # post-VS + input
        clip, inpos = [], []
        try:
            vo = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            if vo and vo.vertexResourceId != rd.ResourceId.Null():
                st, base = int(vo.vertexByteStride), int(vo.vertexByteOffset)
                n = min(int(getattr(vo, "numIndices", 0)) or NV, NV)
                raw = bytes(controller.GetBufferData(vo.vertexResourceId, base,
                                                     st * n))
                for k in range(n):
                    if (k + 1) * st <= len(raw):
                        clip.append(struct.unpack_from("<4f", raw, k * st))
            vi = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSIn)
            if vi and vi.vertexResourceId != rd.ResourceId.Null():
                st, base = int(vi.vertexByteStride), int(vi.vertexByteOffset)
                n = min(len(clip) or NV, NV)
                raw = bytes(controller.GetBufferData(vi.vertexResourceId, base,
                                                     st * n))
                cc = int(vi.format.compCount)
                cw = int(vi.format.compByteWidth)
                ct = en(vi.format.compType)
                for k in range(n):
                    o = k * st
                    if o + st > len(raw):
                        break
                    if cw == 4 and ct.startswith("Float") and cc >= 3:
                        inpos.append(struct.unpack_from("<3f", raw, o))
                    elif cw == 2 and ct.startswith("SNorm") and cc >= 3:
                        v = struct.unpack_from("<%dh" % cc, raw, o)
                        inpos.append(tuple(x / 32767.0 for x in v[:3]))
                    elif cw == 2 and ct.startswith("UNorm") and cc >= 3:
                        v = struct.unpack_from("<%dH" % cc, raw, o)
                        inpos.append(tuple(x / 65535.0 for x in v[:3]))
                    elif cw == 2 and ct.startswith("Float") and cc >= 3:
                        inpos.append(None)
        except Exception as e:
            say("  i=%-5d postVS FAILED %s: %s" % (i, type(e).__name__, e))
        if inpos and any(p is None for p in inpos):
            inpos = []

        # candidates: every 16-byte-aligned 64-byte window of every block
        cands = []
        for cb, rid, boff, bsz, data in blocks:
            named = {}
            for v in cb.variables:
                nr, nc, tn = shape_of(v)
                if nr >= 4 and nc >= 4:
                    named[int(v.byteOffset)] = v.name
                elif nr == 3 and nc == 4:
                    named[int(v.byteOffset)] = v.name + "(3x4)"
            lim = min(len(data), 2048)
            for o in range(0, max(lim - 63, 0), 16):
                vals = list(struct.unpack_from("<16f", data, o))
                if not all(map(math.isfinite, vals)):
                    continue
                cands.append((cb.name, int(cb.fixedBindNumber), o,
                              named.get(o, "(unnamed)"), vals))

        truth = None
        if clip and inpos and cands:
            n = min(len(clip), len(inpos))
            best = None
            for (bn, slot, o, vn, vals) in cands:
                for conv in ("row", "col"):
                    err = 0.0
                    for k in range(n):
                        p = list(inpos[k]) + [1.0]
                        q = rowmul(p, vals) if conv == "row" else colmul(vals, p)
                        err = max(err, max(abs(q[j] - clip[k][j])
                                           for j in range(4)))
                    if best is None or err < best[0]:
                        best = (err, bn, slot, o, vn, conv, vals, n)
            if best and best[0] < 5e-3:
                truth = best

        b1 = None
        for cb, rid, boff, bsz, data in blocks:
            if int(cb.fixedBindNumber) == 1:
                b1 = (cb, rid, boff, bsz, data)
        rA = ruleA(b1[4]) if b1 else None
        rB = ruleB(b1[4], len(b1[4])) if b1 else None

        kind = "TAGGED" if r["tagged"] else ("blend" if r["blend"] else "-")
        note = ""
        if truth:
            note = "truth=%s@+0x%02X(%s,%s) err=%.1e" % (
                truth[4], truth[3], truth[1], truth[5], truth[0])
            if truth[2] == 1 and b1 is not None:
                toff = truth[3]
                if rA == toff:
                    score["A"][0] += 1
                else:
                    score["A"][1] += 1
                    failA.append((i, eid, toff, rA, r["blend"]))
                if rB == toff:
                    score["B"][0] += 1
                else:
                    score["B"][1] += 1
                    failB.append((i, eid, toff, rB, r["blend"]))
                c3 = (truth[6][3], truth[6][7], truth[6][11], truth[6][15])
                if is0001(c3):
                    ortho.append((i, eid, toff, truth[4]))
        else:
            note = "truth=NONE"

        say("%-6d %-8d %-7s %-8d %-16s %-6s %-7s %-6s %-6s %-6s %s"
            % (i, eid, kind, r["indices"],
               b1[0].name[:16] if b1 else "-",
               b1[0].byteSize if b1 else "-",
               bufsize.get(str(b1[1]), "?") if b1 else "-",
               b1[2] if b1 else "-",
               ("+0x%02X" % truth[3]) if truth and truth[2] == 1 else "-",
               ("0x%02X" % rA) if rA is not None else "-",
               ("0x%02X " % rB if rB is not None else "none ") + note))

        key = (r["vs"], b1[0].name if b1 else "-", b1[0].byteSize if b1 else 0)
        want_detail = i in detail_for or key not in seen_layout
        seen_layout.setdefault(key, i)
        if want_detail:
            say("      --- detail eid %d  VS %s ---" % (eid, r["vs"]))
            for bi, (cb, rid, boff, bsz, data) in enumerate(blocks):
                say("      block[%d] %-20s b%-2d declared %5dB  buffer %s "
                    "real %sB  bindOffset %d  bindSize %d  read %dB"
                    % (bi, cb.name, int(cb.fixedBindNumber), cb.byteSize,
                       str(rid), bufsize.get(str(rid), "?"), boff, bsz,
                       len(data)))
                for v in cb.variables:
                    o = int(v.byteOffset)
                    nr, nc, tn = shape_of(v)
                    n = max(nr * nc, 1)
                    if o + n * 4 > len(data):
                        say("         +0x%04X %-28s %dx%d (past end)"
                            % (o, v.name, nr, nc))
                        continue
                    vals = struct.unpack_from("<%df" % n, data, o)
                    if nr >= 4 and nc >= 4:
                        c3 = (vals[3], vals[7], vals[11], vals[15])
                        say("         +0x%04X %-28s 4x4 col3=(%+.6f %+.6f "
                            "%+.6f %+.6f) %s  row3=(%+.4f %+.4f %+.4f) |t|=%.4f"
                            % (o, v.name, c3[0], c3[1], c3[2], c3[3],
                               "AFFINE" if is0001(c3) else "PROJECTIVE",
                               vals[12], vals[13], vals[14],
                               math.sqrt(vals[12] ** 2 + vals[13] ** 2
                                         + vals[14] ** 2)))
                        say("            bits col3 = %08X %08X %08X %08X"
                            % struct.unpack("<4I",
                                            struct.pack("<4f", *c3)))
                    else:
                        say("         +0x%04X %-28s %dx%d = %s"
                            % (o, v.name, nr, nc,
                               " ".join("%+.4f" % x for x in vals[:8])))
            if b1 and len(b1[4]) >= 64:
                m0 = list(struct.unpack_from("<16f", b1[4], 0))
                ok, d, why = looks_close(m0)
                say("      LooksLikeCloseModel(+0x00) -> %s (%s)"
                    % ("TRUE - PatchWeaponMatrix WOULD fire" if ok
                       else "false", why))
                say("      IsWeaponBuffer(realBytes=%s, off=%s) -> %s"
                    % (bufsize.get(str(b1[1]), "?"), b1[2],
                       "TRUE (192 && 0)"
                       if bufsize.get(str(b1[1]), -1) == 192 and b1[2] == 0
                       else "false"))
                say("      mod snapshot possible (realBytes >= 256)? %s ; "
                    "BindShiftedWeaponCB needs bindOffset+256 <= snapshot: %s"
                    % (bufsize.get(str(b1[1]), -1) >= 256,
                       (b1[2] + 256) <= max(bufsize.get(str(b1[1]), 0), 0)))
            if truth:
                say("      GROUND TRUTH %s @ +0x%02X of %s (b%d), %s-vector, "
                    "max residual %.3e over %d verts"
                    % (truth[4], truth[3], truth[1], truth[2], truth[5],
                       truth[0], truth[7]))
                say("      first 3 VSIn  %s"
                    % "  ".join("(%+.4f %+.4f %+.4f)" % p for p in inpos[:3]))
                say("      first 3 VSOut %s"
                    % "  ".join("(%+.4f %+.4f %+.4f %+.4f)" % p
                                for p in clip[:3]))

    say("")
    say("=" * 100)
    say("== SCOREBOARD (draws with a b1 block AND a proven ground truth) ==")
    say("   rule A shipping : %d correct, %d wrong" % tuple(score["A"]))
    for f in failA:
        say("      WRONG i=%-5d eid=%-7d truth +0x%02X got %s blended=%d"
            % (f[0], f[1], f[2], "-" if f[3] is None else "0x%02X" % f[3], f[4]))
    say("   rule B proposed : %d correct, %d wrong" % tuple(score["B"]))
    for f in failB:
        say("      WRONG i=%-5d eid=%-7d truth +0x%02X got %s blended=%d"
            % (f[0], f[1], f[2], "none" if f[3] is None else "0x%02X" % f[3],
               f[4]))
    say("   ORTHOGRAPHIC ground truths (affine col3 - would break rule B): %d"
        % len(ortho))
    for o in ortho:
        say("      i=%-5d eid=%-7d +0x%02X %s" % o)

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
