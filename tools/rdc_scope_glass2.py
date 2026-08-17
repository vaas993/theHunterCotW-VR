"""SCOPE GLASS part 2: read the blocks properly and PROVE which matrix is clip.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_scope_glass2.py

What part 1 got wrong: d3d.vertexShader has no .constantBuffers in 1.45, and
the descriptor byteSize is the WHOLE buffer (65536), so matching a declared
block to a descriptor by size found nothing and every block printed
"NO BUFFER BOUND".

The correct mapping (and the documented trap): UsedDescriptor.access.index is
the descriptor POSITION, so the used descriptors, sorted by that index, line up
with refl.constantBlocks in DECLARATION order - not with the b# register.
Verified below by count and by the proof test.

THE PROOF.  No guessing from matrix shape: for each 4x4 constant M we take the
draw's real post-VS SV_POSITION q and compute q * M^-1.  If M is the matrix the
shader used to reach clip space then the result is the input vertex in
homogeneous local space, whose 4th component is EXACTLY 1.0 for every vertex.
A World matrix, a view matrix or a texture matrix gives anything but 1.

Outputs:
    %LOCALAPPDATA%\theHunterCotWVR\rdc_scope_glass2.txt
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame28475.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_scope_glass2.txt")

GLASS = [19702, 19705, 20439, 20448]      # VS 2205, 1584 idx, SrcAlpha blended
SOLID = [19751, 19946, 20380, 19776]      # tagged viewmodel, known layouts
EXTRA = [19625, 19647, 20056]             # the other blended neighbours

NV = 12          # vertices used for the proof

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
            nm = getattr(holder, "name", None) or getattr(holder, "baseType", "")
            return int(r), int(c or 1), str(nm)
    return 0, 0, "?"


def inv4(m):
    """m = 16 floats, memory row-major. Gauss-Jordan. None if singular."""
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
    """row vector v (4) times 4x4 m stored row-major."""
    return [sum(v[i] * m[i * 4 + j] for i in range(4)) for j in range(4)]


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open: %s" % CAP)

    leaves = []

    def walk(lst):
        for a in lst:
            kids = getattr(a, "children", [])
            if kids:
                walk(kids)
            else:
                leaves.append(a)

    walk(controller.GetRootActions())
    by_eid = {a.eventId: a for a in leaves}

    probed = [False]

    def dump(eid, tag):
        a = by_eid.get(eid)
        say("")
        say("=" * 78)
        say("  eid %-7d [%s]  %d indices"
            % (eid, tag, getattr(a, "numIndices", 0)))
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        eps = controller.GetShaderEntryPoints(vsid)
        refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        say("  VS %s" % str(vsid))

        # ---- used descriptors, sorted by descriptor POSITION ----------
        used = []
        blocks = pipe.GetConstantBlocks(rd.ShaderStage.Vertex)
        for ud in blocks:
            acc = getattr(ud, "access", None)
            d = getattr(ud, "descriptor", None)
            if d is None:
                continue
            # copy off the temporary proxy at once
            used.append((int(getattr(acc, "index", 1 << 30)),
                         d.resource, int(d.byteOffset), int(d.byteSize)))
        used.sort(key=lambda t: t[0])
        if not probed[0]:
            probed[0] = True
            if blocks:
                say("  [probe] UsedDescriptor attrs %s"
                    % [x for x in dir(blocks[0]) if not x.startswith("_")])
                acc = blocks[0].access
                say("  [probe] access attrs %s"
                    % [x for x in dir(acc) if not x.startswith("_")])
                say("  [probe] access.index=%s type=%s stage=%s"
                    % (getattr(acc, "index", "?"), getattr(acc, "type", "?"),
                       getattr(acc, "stage", "?")))
        say("  %d declared block(s), %d bound descriptor(s)"
            % (len(refl.constantBlocks), len(used)))
        if len(used) != len(refl.constantBlocks):
            say("  !! COUNT MISMATCH - mapping by position is unsafe here")

        # ---- post-VS clip positions -----------------------------------
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
        except Exception as e:
            say("  post-VS FAILED %s: %s" % (type(e).__name__, e))
        if clip:
            say("  SV_POSITION[0..2] = %s"
                % "  ".join("(%+.5f %+.5f %+.5f %+.5f)" % p for p in clip[:3]))
            say("  SV_POSITION z: all == %.5f ? %s"
                % (clip[0][2],
                   all(abs(p[2] - clip[0][2]) < 1e-6 for p in clip)))

        cands = []
        for bi, cb in enumerate(refl.constantBlocks):
            slot = int(cb.fixedBindNumber)
            say("")
            say("  -- %s (b%d)  %d bytes declared, %d variables --"
                % (cb.name, slot, cb.byteSize, len(cb.variables)))
            if bi >= len(used):
                say("     no descriptor at position %d" % bi)
                continue
            pos, rid, boff, bsz = used[bi]
            want = min(cb.byteSize, 65536)
            try:
                data = bytes(controller.GetBufferData(rid, boff, want))
            except Exception as e:
                say("     GetBufferData FAILED %s: %s" % (type(e).__name__, e))
                continue
            say("     descriptor pos %d -> %s off=%d, read %d of %d bytes"
                % (pos, str(rid), boff, len(data), cb.byteSize))

            if cb.byteSize <= 512:
                say("     raw:")
                for o in range(0, min(len(data), 512), 16):
                    fl = struct.unpack_from("<4f", data, o)
                    say("       +0x%04X  %08X %08X %08X %08X   "
                        "%+12.5f %+12.5f %+12.5f %+12.5f"
                        % ((o,) + struct.unpack_from("<4I", data, o) + fl))

            for v in cb.variables:
                off = int(v.byteOffset)
                nr, nc, tn = shape_of(v)
                n = max(nr * nc, 1)
                if off + n * 4 > len(data):
                    say("     +0x%04X %-30s %dx%d (past end)"
                        % (off, v.name, nr, nc))
                    continue
                vals = struct.unpack_from("<%df" % n, data, off)
                if not all(map(math.isfinite, vals)):
                    say("     +0x%04X %-30s %dx%d non-finite"
                        % (off, v.name, nr, nc))
                    continue
                if nr >= 3 and nc >= 4:
                    say("     +0x%04X %-30s %dx%d" % (off, v.name, nr, nc))
                    for rr in range(nr):
                        say("            [%s]" % " ".join(
                            "%+13.6f" % vals[rr * nc + c] for c in range(nc)))
                    if n >= 16:
                        c3 = (vals[3], vals[7], vals[11], vals[15])
                        say("            4th column = (%+.5f %+.5f %+.5f %+.5f)"
                            "  %s" % (c3 + (
                                "AFFINE (not a clip matrix)"
                                if (abs(c3[0]) < 1e-6 and abs(c3[1]) < 1e-6
                                    and abs(c3[2]) < 1e-6
                                    and abs(c3[3] - 1) < 1e-6)
                                else "PROJECTIVE",)))
                        say("            row3 (translation) = "
                            "(%+.5f %+.5f %+.5f)"
                            % (vals[12], vals[13], vals[14]))
                        say("            |row3 xyz| = %.4f"
                            % math.sqrt(vals[12] ** 2 + vals[13] ** 2
                                        + vals[14] ** 2))
                        cands.append((cb.name, slot, off, v.name, list(vals)))
                else:
                    say("     +0x%04X %-30s %dx%d = %s"
                        % (off, v.name, nr, nc,
                           " ".join("%+.5f" % x for x in vals[:8])))

            # also treat every aligned 64-byte window as a matrix, so a
            # matrix the reflection does not name cannot hide
            if cb.byteSize <= 1024:
                named = set(int(v.byteOffset) for v in cb.variables)
                for o in range(0, len(data) - 63, 16):
                    if o in named:
                        continue
                    vals = list(struct.unpack_from("<16f", data, o))
                    if all(map(math.isfinite, vals)):
                        cands.append((cb.name, slot, o,
                                      "(unnamed window)", vals))

        # ---- THE PROOF -------------------------------------------------
        if clip and cands:
            say("")
            say("  == PROOF: q * M^-1 must give w == 1.0 for every vertex ==")
            say("     %-26s %-6s %-8s %10s %10s"
                % ("variable", "block", "offset", "w mean", "w spread"))
            for bname, slot, off, vname, vals in cands:
                mi = inv4(vals)
                if mi is None:
                    say("     %-26s b%-5d +0x%04X   singular"
                        % (vname, slot, off))
                    continue
                ws = []
                loc = []
                for q in clip:
                    p = rowmul(list(q), mi)
                    ws.append(p[3])
                    loc.append(p)
                mean = sum(ws) / len(ws)
                spread = max(ws) - min(ws)
                ok = abs(mean - 1.0) < 1e-3 and spread < 1e-3
                mark = "   <<<< CLIP-SPACE MATRIX" if ok else ""
                if vname == "(unnamed window)" and not ok:
                    continue
                say("     %-26s b%-5d +0x%04X %10.5f %10.6f%s"
                    % (vname, slot, off, mean, spread, mark))
                if ok:
                    say("            recovered local verts: %s"
                        % "  ".join("(%+.4f %+.4f %+.4f)"
                                    % (p[0], p[1], p[2]) for p in loc[:3]))

    for eid in GLASS:
        dump(eid, "GLASS (scope lens)")
    for eid in SOLID:
        dump(eid, "SOLID tagged viewmodel")
    for eid in EXTRA:
        dump(eid, "other blended neighbour")

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
