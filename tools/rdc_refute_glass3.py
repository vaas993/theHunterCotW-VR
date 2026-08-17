"""REFUTATION part 3: GROUND TRUTH by forward transform, per draw.

    set COTW_CAP=...   set COTW_CSV=...   set COTW_OUT=...
    qrenderdoc.exe --python tools\rdc_refute_glass3.py

No inverse, no shape heuristic, no trust in the previous investigation.  For
each draw:

    real POSITION out of the real vertex buffer (decoded through the input
    layout)  ->  through candidate M  ->  must land on the real post-VS
    SV_POSITION that RenderDoc replayed.

Matching is nearest-neighbour over the sampled vertices, so vertex ORDER
between VSIn and VSOut cannot create a false negative.  Both storage
conventions are tried (row-vector v*M and column-vector M*v) so a transposed
upload cannot hide.

Then, and only then, the two candidate runtime rules are scored against that
ground truth, and the shift the mod would actually apply is simulated on the
winning matrix to confirm it produces a HORIZONTAL clip-space displacement.
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
    "COTW_OUT", r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute_glass3_scope.txt"))

CTX = 6          # draws either side of the tagged pass
NEAR = 4
MAXVB = 400      # input vertices decoded
MAXPV = 400      # post-VS vertices read
SAMPLE = 60      # clip positions matched

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


def apply_row(m, p):
    return tuple(p[0] * m[0 + j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j]
                 for j in range(4))


def apply_col(m, p):
    return tuple(p[0] * m[j * 4 + 0] + p[1] * m[j * 4 + 1]
                 + p[2] * m[j * 4 + 2] + m[j * 4 + 3] for j in range(4))


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
    window = set(range(max(0, min(tagidx) - CTX),
                       min(len(rows) - 1, max(tagidx) + CTX) + 1))
    targets = sorted(set(list(window) + [r["i"] for r in rows
                                         if r["vs"] in glassvs]))
    say("targets: %d draws" % len(targets))

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

    score = {"A": [0, 0], "B": [0, 0]}
    failA, failB = [], []
    ortho = []
    say("")

    for i in targets:
        if i >= len(draws):
            continue
        a = draws[i]
        r = rows[i]
        eid = a.eventId
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        d3d = controller.GetD3D11PipelineState()
        try:
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        except Exception as e:
            say("i=%d eid=%d reflection FAILED %s: %s"
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
            say("i=%d GetConstantBlocks FAILED %s: %s" % (i, type(e).__name__, e))
        used.sort(key=lambda t: t[0])

        b1data, b1name, b1decl, b1buf, b1off = b"", "-", 0, None, 0
        cands = []
        for bi, cb in enumerate(refl.constantBlocks):
            if bi >= len(used) or int(cb.byteSize) > 4096:
                continue
            pos, rid, boff, bsz = used[bi]
            try:
                data = bytes(controller.GetBufferData(rid, boff,
                                                      min(int(cb.byteSize), 4096)))
            except Exception as e:
                say("i=%d GetBufferData FAILED %s: %s" % (i, type(e).__name__, e))
                continue
            if int(cb.fixedBindNumber) == 1:
                b1data, b1name, b1decl = data, cb.name, int(cb.byteSize)
                b1buf, b1off = rid, boff
            named = {}
            for v in cb.variables:
                nr, nc = shape_of(v)
                if nr >= 4 and nc >= 4:
                    named[int(v.byteOffset)] = v.name
            for o in range(0, max(len(data) - 63, 0), 16):
                m = list(struct.unpack_from("<16f", data, o))
                if all(map(math.isfinite, m)):
                    cands.append((cb.name, int(cb.fixedBindNumber), o,
                                  named.get(o, "(unnamed)"), m))

        # ---- real input positions ------------------------------------
        ia = d3d.inputAssembly
        pos_el = None
        for l in ia.layouts:
            if l.semanticName.upper().startswith("POSITION"):
                pos_el = l
                break
        verts = []
        if pos_el is not None:
            vbs = list(ia.vertexBuffers)
            vb = vbs[pos_el.inputSlot] if pos_el.inputSlot < len(vbs) else None
            if vb is not None and vb.resourceId != rd.ResourceId.Null():
                stride = int(vb.byteStride)
                base = int(vb.byteOffset) + int(pos_el.byteOffset)
                fmt = pos_el.format
                ct, cw, cc = en(fmt.compType), int(fmt.compByteWidth), int(
                    fmt.compCount)
                try:
                    raw = bytes(controller.GetBufferData(vb.resourceId, base,
                                                         stride * MAXVB))
                except Exception:
                    raw = b""
                for k in range(MAXVB):
                    o = k * stride
                    if o + cw * 3 > len(raw):
                        break
                    if cw == 4 and ct in ("Float", "SFloat"):
                        p = struct.unpack_from("<3f", raw, o)
                    elif cw == 2 and ct in ("Float", "SFloat"):
                        p = tuple(float(x)
                                  for x in struct.unpack_from("<3e", raw, o))
                    elif cw == 2 and ct == "SNorm":
                        p = tuple(x / 32767.0
                                  for x in struct.unpack_from("<3h", raw, o))
                    elif cw == 2 and ct == "UNorm":
                        p = tuple(x / 65535.0
                                  for x in struct.unpack_from("<3H", raw, o))
                    else:
                        p = None
                    if p and all(map(math.isfinite, p)):
                        verts.append(p)

        # ---- real post-VS clip positions -----------------------------
        clips = []
        try:
            pv = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            if pv and pv.vertexResourceId != rd.ResourceId.Null():
                ps = int(pv.vertexByteStride)
                pn = min(int(getattr(pv, "numIndices", 0)) or 0, MAXPV)
                praw = bytes(controller.GetBufferData(pv.vertexResourceId,
                                                      int(pv.vertexByteOffset),
                                                      ps * pn))
                for k in range(pn):
                    o = k * ps
                    if o + 16 > len(praw):
                        break
                    p = struct.unpack_from("<4f", praw, o)
                    if all(map(math.isfinite, p)):
                        clips.append(p)
        except Exception as e:
            say("i=%d postVS FAILED %s: %s" % (i, type(e).__name__, e))

        rA = ruleA(b1data) if b1data else None
        rB = ruleB(b1data, len(b1data)) if b1data else None
        kind = "TAGGED" if r["tagged"] else ("blend" if r["blend"] else "-")

        if not verts or not clips or not cands:
            say("i=%-5d eid=%-7d %-6s idx=%-6d %-16s %4dB  verts=%d clips=%d "
                "cands=%d -> NO GROUND TRUTH  A=%s B=%s"
                % (i, eid, kind, r["indices"], b1name, b1decl, len(verts),
                   len(clips), len(cands),
                   "-" if rA is None else "0x%02X" % rA,
                   "none" if rB is None else "0x%02X" % rB))
            continue

        sample = clips[:SAMPLE]
        best = None
        results = []
        for (bn, slot, o, vn, m) in cands:
            for conv, fn in (("row", apply_row), ("col", apply_col)):
                tr = [fn(m, v) for v in verts]
                worst = 0.0
                tot = 0.0
                for c in sample:
                    e = min(max(abs(c[j] - t[j]) for j in range(4)) for t in tr)
                    tot += e
                    worst = max(worst, e)
                mean = tot / len(sample)
                results.append((worst, mean, bn, slot, o, vn, conv, m))
                if best is None or worst < best[0]:
                    best = (worst, mean, bn, slot, o, vn, conv, m)

        gt = best if best and best[0] < 1e-4 else None
        say("i=%-5d eid=%-7d %-6s idx=%-6d b1=%-16s %4dB real=%sB off=%d  "
            "verts=%d clips=%d"
            % (i, eid, kind, r["indices"], b1name, b1decl,
               bufsize.get(str(b1buf), "?"), b1off, len(verts), len(clips)))
        for (worst, mean, bn, slot, o, vn, conv, m) in sorted(results)[:4]:
            say("      %-22s b%-2d +0x%02X %-4s worst %.6f mean %.6f%s"
                % (vn[:22], slot, o, conv, worst, mean,
                   "   <<< GROUND TRUTH" if gt and (o, conv, slot) ==
                   (gt[4], gt[6], gt[3]) else ""))
        if gt is None:
            say("      -> no candidate reproduces SV_POSITION; nothing scored")
            continue

        c3 = (gt[7][3], gt[7][7], gt[7][11], gt[7][15])
        say("      truth col3 = (%+.6f %+.6f %+.6f %+.6f)  %s"
            % (c3 + ("AFFINE - ORTHOGRAPHIC" if is0001(c3) else "PROJECTIVE",)))
        if is0001(c3):
            ortho.append((i, eid, gt[4], gt[5]))

        # what the mod's shift does to the winning matrix
        if gt[6] == "row":
            k = 0.02
            m2 = list(gt[7])
            m2[0] += k * m2[3]
            m2[4] += k * m2[7]
            m2[8] += k * m2[11]
            m2[12] += k * m2[15]
            d = [apply_row(m2, v) for v in verts[:6]]
            o0 = [apply_row(gt[7], v) for v in verts[:6]]
            dx = [(d[j][0] / d[j][3] - o0[j][0] / o0[j][3])
                  if abs(d[j][3]) > 1e-9 and abs(o0[j][3]) > 1e-9 else float('nan')
                  for j in range(len(d))]
            dy = [(d[j][1] / d[j][3] - o0[j][1] / o0[j][3])
                  if abs(d[j][3]) > 1e-9 and abs(o0[j][3]) > 1e-9 else float('nan')
                  for j in range(len(d))]
            say("      ShiftWeaponClipX(k=0.02) on the truth matrix: "
                "d(ndc.x)=%s  d(ndc.y)=%s"
                % (" ".join("%+.5f" % x for x in dx[:4]),
                   " ".join("%+.5f" % y for y in dy[:4])))

        if b1data and gt[3] == 1:
            toff = gt[4]
            if rA == toff:
                score["A"][0] += 1
            else:
                score["A"][1] += 1
                failA.append((i, eid, toff, rA, r["blend"], b1name, b1decl))
            if rB == toff:
                score["B"][0] += 1
            else:
                score["B"][1] += 1
                failB.append((i, eid, toff, rB, r["blend"], b1name, b1decl))

    say("")
    say("=" * 90)
    say("== SCOREBOARD against forward-transform ground truth ==")
    say("   rule A (shipping)  %d correct  %d wrong" % tuple(score["A"]))
    for f in failA:
        say("      WRONG i=%-5d eid=%-7d truth+0x%02X got %s blend=%d %s %dB"
            % (f[0], f[1], f[2], "-" if f[3] is None else "0x%02X" % f[3],
               f[4], f[5], f[6]))
    say("   rule B (proposed)  %d correct  %d wrong" % tuple(score["B"]))
    for f in failB:
        say("      WRONG i=%-5d eid=%-7d truth+0x%02X got %s blend=%d %s %dB"
            % (f[0], f[1], f[2], "none" if f[3] is None else "0x%02X" % f[3],
               f[4], f[5], f[6]))
    say("   orthographic ground truths (would break rule B): %d" % len(ortho))
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
