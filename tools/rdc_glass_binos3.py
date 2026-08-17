"""BINOCULAR GLASS, part 3: prove the matrix, and count the lenses.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_glass_binos3.py

Part 2 found the lens: eid 31395, 2208 indices, alpha-blended, untagged, sitting
in exactly the clip volume of the tagged 26178-index binocular body, and its VS
(2302) declares a THIRD block layout:

    InstanceConsts (128 B, register b1)   +0x00 World   +0x40 WorldViewProjection

and its DXBC says outright

    0: mul r0, v0.yyyy, WorldViewProjection[1]
    1: mad r0, v0.xxxx, WorldViewProjection[0], r0
    2: mad r0, v0.zzzz, WorldViewProjection[2], r0
    3: add o0, r0, WorldViewProjection[3]

This checks that by arithmetic rather than by reading: take the draw's own input
positions, push them through each candidate matrix in the block in both storage
conventions, and see which one reproduces the post-VS clip positions RenderDoc
captured.  Only the real object-to-clip matrix can.

It also splits the post-VS positions into clusters, to see whether the single
2208-index draw is one lens or two.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_glass_binos3.txt
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame18022.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_glass_binos3.txt")

EIDS = [(31395, "GLASS lens"), (31246, "SOLID binocular body")]
MAX_VB = 3000

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open")

    for eid, label in EIDS:
        say("")
        say("=" * 90)
        say("== eid %d  (%s) ==" % (eid, label))
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        d3d = controller.GetD3D11PipelineState()

        # ---- the block, by reflection --------------------------------------
        vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        eps = controller.GetShaderEntryPoints(vsid)
        refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        blocks = list(pipe.GetConstantBlocks(rd.ShaderStage.Vertex))
        reads = {}
        for ud in blocks:
            acc = getattr(ud, "access", None)
            aidx = int(getattr(acc, "index", -1)) if acc is not None else -1
            d = getattr(ud, "descriptor", None)
            if d is None:
                continue
            rid = d.resource
            if rid == rd.ResourceId.Null():
                continue
            boff = int(getattr(d, "byteOffset", 0))
            bsz = int(getattr(d, "byteSize", 0))
            try:
                reads[aidx] = bytes(controller.GetBufferData(
                    rid, boff, min(bsz, 65536) if bsz else 65536))
            except Exception as e:
                say("  descriptor %d read failed: %s" % (aidx, e))

        cbk = refl.constantBlocks[0]
        data = reads.get(0, b"")
        say("  block %s (b%s) %d B declared, %d B read"
            % (cbk.name, cbk.fixedBindNumber, cbk.byteSize, len(data)))

        cands = []
        for v in cbk.variables:
            off = int(v.byteOffset)
            td = getattr(v.type, "descriptor", v.type)
            r_ = int(getattr(td, "rows", 0) or 0)
            c_ = int(getattr(td, "columns", 0) or 0)
            if r_ == 4 and c_ == 4 and off + 64 <= len(data):
                m = struct.unpack_from("<16f", data, off)
                cands.append((off, v.name, m))
                say("  candidate +0x%04X %-24s last col (%+.4f %+.4f %+.4f "
                    "%+.4f)  last row (%+.4f %+.4f %+.4f %+.4f)"
                    % (off, v.name, m[3], m[7], m[11], m[15],
                       m[12], m[13], m[14], m[15]))

        # ---- the input positions -------------------------------------------
        ia = d3d.inputAssembly
        say("  -- input layout --")
        pos_el = None
        for i, l in enumerate(ia.layouts):
            fmt = l.format
            say("     [%d] %s%d slot=%d off=%d fmt=%s comp=%sx%s perInst=%s"
                % (i, l.semanticName, l.semanticIndex, l.inputSlot,
                   l.byteOffset, str(fmt.compType).split(".")[-1],
                   fmt.compCount, fmt.compByteWidth, l.perInstance))
            if pos_el is None and l.semanticName.upper().startswith("POSITION"):
                pos_el = l
        if pos_el is None:
            say("     no POSITION element - skipping the arithmetic check")
            continue

        vbs = list(ia.vertexBuffers)
        vb = vbs[pos_el.inputSlot] if pos_el.inputSlot < len(vbs) else None
        if vb is None or vb.resourceId == rd.ResourceId.Null():
            say("     POSITION's vertex buffer is not bound")
            continue
        stride = int(vb.byteStride)
        base = int(vb.byteOffset) + int(pos_el.byteOffset)
        say("     POSITION from slot %d, vb stride %d, base 0x%X"
            % (pos_el.inputSlot, stride, base))
        raw = bytes(controller.GetBufferData(vb.resourceId, base,
                                             stride * MAX_VB))
        fmt = pos_el.format
        ctype = str(fmt.compType).split(".")[-1]
        cw = int(fmt.compByteWidth)
        cc = int(fmt.compCount)
        say("     POSITION format: %s %d x %dB" % (ctype, cc, cw))

        def decode(off):
            if cw == 4 and ctype in ("Float", "SFloat"):
                return struct.unpack_from("<3f", raw, off)
            if cw == 2 and ctype in ("Float", "SFloat"):
                h = struct.unpack_from("<3e", raw, off)
                return tuple(float(x) for x in h)
            if cw == 2 and ctype in ("SNorm",):
                v = struct.unpack_from("<3h", raw, off)
                return tuple(x / 32767.0 for x in v)
            if cw == 2 and ctype in ("UNorm",):
                v = struct.unpack_from("<3H", raw, off)
                return tuple(x / 65535.0 for x in v)
            if cw == 2 and ctype in ("SInt",):
                v = struct.unpack_from("<3h", raw, off)
                return tuple(float(x) for x in v)
            return None

        verts = []
        for k in range(MAX_VB):
            o = k * stride
            if o + cw * 3 > len(raw):
                break
            p = decode(o)
            if p and all(map(math.isfinite, p)):
                verts.append(p)
        say("     decoded %d input positions, e.g. %s"
            % (len(verts), " ".join("(%.3f %.3f %.3f)" % v for v in verts[:3])))

        # ---- the post-VS clip positions -------------------------------------
        pv = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        if pv.vertexResourceId == rd.ResourceId.Null():
            say("     no post-VS data")
            continue
        pstride = int(pv.vertexByteStride)
        pn = min(int(getattr(pv, "numIndices", 0)) or 0, 2000)
        praw = bytes(controller.GetBufferData(pv.vertexResourceId,
                                              int(pv.vertexByteOffset),
                                              pstride * pn))
        say("     post-VS: %d verts, stride %d, format %s x%s"
            % (pn, pstride, str(pv.format.compType).split(".")[-1],
               pv.format.compCount))
        clips = []
        for k in range(pn):
            o = k * pstride
            if o + 16 > len(praw):
                break
            p = struct.unpack_from("<4f", praw, o)
            if all(map(math.isfinite, p)):
                clips.append(p)
        say("     %d clip positions, e.g. %s"
            % (len(clips),
               " ".join("(%.3f %.3f %.3f %.3f)" % c for c in clips[:2])))

        # ---- which candidate reproduces them? -------------------------------
        def apply_row(m, p):        # clip = (p,1) . M   (rows of M in memory)
            return tuple(p[0] * m[0 + j] + p[1] * m[4 + j] + p[2] * m[8 + j]
                         + m[12 + j] for j in range(4))

        def apply_col(m, p):        # clip = M . (p,1)   (columns in memory)
            return tuple(p[0] * m[j * 4 + 0] + p[1] * m[j * 4 + 1]
                         + p[2] * m[j * 4 + 2] + m[j * 4 + 3]
                         for j in range(4))

        say("  -- which candidate maps the input positions onto the post-VS "
            "clip positions? --")
        sample = clips[:120]
        for off, name, m in cands:
            for conv, fn in (("row-vector (v.M)", apply_row),
                             ("col-vector (M.v)", apply_col)):
                tr = [fn(m, v) for v in verts[:1200]]
                tot = 0.0
                worst = 0.0
                for c in sample:
                    best = min(max(abs(c[i] - t[i]) for i in range(4))
                               for t in tr) if tr else 9e9
                    tot += best
                    worst = max(worst, best)
                say("     +0x%04X %-24s %-18s  mean nearest err %.6f, "
                    "worst %.6f%s"
                    % (off, name, conv, tot / max(1, len(sample)), worst,
                       "   <<< MATCH" if worst < 1e-3 else ""))

        # ---- one lens or two? -----------------------------------------------
        pts = []
        for c in clips:
            if abs(c[3]) > 1e-6:
                pts.append((c[0] / c[3], c[1] / c[3]))
        if pts:
            xs = sorted(p[0] for p in pts)
            gaps = [(xs[i + 1] - xs[i], xs[i], xs[i + 1])
                    for i in range(len(xs) - 1)]
            gaps.sort(reverse=True)
            say("  -- lens clustering (NDC x of %d verts) --" % len(pts))
            say("     x span %.3f .. %.3f, y span %.3f .. %.3f"
                % (min(p[0] for p in pts), max(p[0] for p in pts),
                   min(p[1] for p in pts), max(p[1] for p in pts)))
            say("     three biggest gaps along x: %s"
                % ", ".join("%.4f (between %.3f and %.3f)" % g
                            for g in gaps[:3]))
            if gaps and gaps[0][0] > 0.05:
                split = (gaps[0][1] + gaps[0][2]) / 2.0
                left = [p for p in pts if p[0] < split]
                right = [p for p in pts if p[0] >= split]
                say("     -> TWO clusters about x=%.3f: %d verts left, "
                    "%d verts right" % (split, len(left), len(right)))
            else:
                say("     -> one connected cluster in x")

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
