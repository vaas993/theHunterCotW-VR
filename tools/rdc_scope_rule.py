"""Score the runtime offset rules against the TRUTH, for every draw in the pass.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_scope_rule.py

TRUTH for a draw = the candidate offset o in {0x00,0x40,0x80} whose 4x4 at
block+o satisfies  q * M^-1 -> w == 1.0  for every real post-VS vertex.  That is
not a shape heuristic, it is the matrix the shader demonstrably used.

RULE A (what the mod does today, cbscan.cpp WvpOffsetIn):
    translation at +0x00 bigger than 100 units  ->  0x40, else 0x00

RULE B (proposed): take the LOWEST candidate offset whose 4th COLUMN - the four
floats the shift itself multiplies by, m[3] m[7] m[11] m[15] - is not (0,0,0,1).
A clip matrix must have a non-trivial 4th column or the perspective divide could
not exist; an affine World/Texture matrix always has exactly (0,0,0,1), and on
such a matrix the shift degenerates into "add k to the translation", which is
the failure being diagnosed.

Also reported: what the mod's own glass window would catch, since the window is
counted forward from the last tagged draw.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_scope_rule.txt
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame28475.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_scope_rule.txt")

LO, HI = 1130, 1180          # draw indices around the viewmodel pass
CANDS = (0x00, 0x40, 0x80)
NV = 10

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


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


def rule_a(blk):
    """cbscan.cpp WvpOffsetIn, verbatim."""
    if len(blk) < 0x80:
        return 0
    tx, ty, tz = struct.unpack_from("<3f", blk, 48)
    if not all(map(math.isfinite, (tx, ty, tz))):
        return 0
    return 0x40 if (abs(tx) > 100.0 or abs(ty) > 100.0
                    or abs(tz) > 100.0) else 0x00


def rule_b(blk, cands=(0x00, 0x40)):
    """lowest offset whose 4th column is not (0,0,0,1)."""
    for o in cands:
        if o + 64 > len(blk):
            break
        m = struct.unpack_from("<16f", blk, o)
        if not all(map(math.isfinite, m)):
            continue
        c = (m[3], m[7], m[11], m[15])
        if abs(c[0]) + abs(c[1]) + abs(c[2]) > 1e-6 or abs(c[3] - 1.0) > 1e-6:
            return o
    return None


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return

    leaves = []

    def walk(lst):
        for a in lst:
            kids = getattr(a, "children", [])
            if kids:
                walk(kids)
            else:
                leaves.append(a)

    walk(controller.GetRootActions())
    draws = [a for a in leaves if a.flags & rd.ActionFlags.Drawcall]
    draws.sort(key=lambda a: a.eventId)

    say("")
    say("%-5s %-7s %-8s %-3s %-3s %-16s %-6s %-7s %-7s %-7s %s"
        % ("idx", "eid", "indices", "T", "B", "block(b1)", "size",
           "TRUTH", "ruleA", "ruleB", "verdict"))

    sinceStencil = 99999
    nA = nB = ntot = 0
    for i in range(LO, min(HI + 1, len(draws))):
        a = draws[i]
        eid = a.eventId
        controller.SetFrameEvent(eid, True)
        d3d = controller.GetD3D11PipelineState()
        om = d3d.outputMerger
        ds = om.depthStencilState
        f = ds.frontFace
        tagged = bool(ds.stencilEnable) and int(f.compareMask) == 0x40 \
            and bool(int(f.reference) & 0x40)
        blend = 0
        try:
            bl = list(om.blendState.blends)
            blend = int(bool(bl[0].enabled)) if bl else 0
        except Exception:
            pass
        if tagged:
            sinceStencil = 0
        elif sinceStencil < 99999:
            sinceStencil += 1

        pipe = controller.GetPipelineState()
        try:
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        except Exception:
            continue

        used = []
        for ud in pipe.GetConstantBlocks(rd.ShaderStage.Vertex):
            d = getattr(ud, "descriptor", None)
            acc = getattr(ud, "access", None)
            if d is None:
                continue
            used.append((int(getattr(acc, "index", 1 << 30)), d.resource,
                         int(d.byteOffset)))
        used.sort(key=lambda t: t[0])

        bname, bsize, blk = "-", 0, b""
        for bi, cb in enumerate(refl.constantBlocks):
            if int(cb.fixedBindNumber) != 1 or bi >= len(used):
                continue
            bname, bsize = cb.name, int(cb.byteSize)
            try:
                blk = bytes(controller.GetBufferData(
                    used[bi][1], used[bi][2], min(bsize, 4096)))
            except Exception:
                blk = b""
            break
        if not blk:
            say("%-5d %-7d %-8d %-3s %-3s %-16s %-6d  (no b1 block)"
                % (i, eid, getattr(a, "numIndices", 0), "T" if tagged else ".",
                   "B" if blend else ".", bname, bsize))
            continue

        # ---- truth ----
        clip = []
        try:
            vo = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            if vo and vo.vertexResourceId != rd.ResourceId.Null():
                st = int(vo.vertexByteStride)
                raw = bytes(controller.GetBufferData(
                    vo.vertexResourceId, int(vo.vertexByteOffset), st * NV))
                for k in range(NV):
                    if (k + 1) * st <= len(raw):
                        clip.append(struct.unpack_from("<4f", raw, k * st))
        except Exception:
            pass
        truth = None
        for o in CANDS:
            if o + 64 > len(blk) or not clip:
                continue
            m = list(struct.unpack_from("<16f", blk, o))
            if not all(map(math.isfinite, m)):
                continue
            mi = inv4(m)
            if mi is None:
                continue
            ws = [sum(q[t] * mi[t * 4 + 3] for t in range(4)) for q in clip]
            if abs(sum(ws) / len(ws) - 1.0) < 1e-3 and max(ws) - min(ws) < 1e-3:
                truth = o
                break

        ra = rule_a(blk)
        rb = rule_b(blk)
        ok_a = (truth is not None and ra == truth)
        ok_b = (truth is not None and rb == truth)
        if truth is not None:
            ntot += 1
            nA += ok_a
            nB += ok_b
        verdict = ""
        if truth is not None:
            verdict = "A %s  B %s" % ("ok" if ok_a else "WRONG",
                                      "ok" if ok_b else "WRONG")
        glasswin = (not tagged) and blend and sinceStencil <= 4
        if glasswin:
            verdict += "   [mod's glass window catches this]"
        elif blend and not tagged:
            verdict += "   [blended, window does NOT catch: sinceStencil=%d]" \
                % sinceStencil
        say("%-5d %-7d %-8d %-3s %-3s %-16s %-6d %-7s %-7s %-7s %s"
            % (i, eid, getattr(a, "numIndices", 0), "T" if tagged else ".",
               "B" if blend else ".", bname[:16], bsize,
               "?" if truth is None else "0x%02X" % truth,
               "0x%02X" % ra, "none" if rb is None else "0x%02X" % rb,
               verdict))

    say("")
    say("rule A (translation > 100 at +0x00) : %d/%d correct" % (nA, ntot))
    say("rule B (4th column != (0,0,0,1))    : %d/%d correct" % (nB, ntot))
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
