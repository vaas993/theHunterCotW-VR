"""BINOCULAR GLASS, part 2: which draws are actually the lenses, and where does
their vertex shader keep the clip matrix?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_glass_binos2.py

Part 1 (rdc_glass_binos.py, and its CSV) established the frame:
    draws 2052..2060 carry the first-person tag (StencilEnable, readMask==0x40,
    ref&0x40) - three meshes (77484, 26178, 9594) in three rounds.
    The blended draws sit at 2024..2051 (before) and 2061 (immediately after).
"Within N draws of a tag" is a POSITION rule, not an eid rule - draws here are
~22 eids apart, which is why a +-8 eid window found nothing.

The lens has to be identified by what it DRAWS, not by where it sits, so this
uses the post-VS clip positions:
  * viewmodel geometry is a few tens of centimetres from the camera, so its
    clip w is tiny; world geometry is metres to kilometres away.
  * a lens covers a big, centred piece of the screen.
Both come straight out of GetPostVSData, and they cannot be argued with.

Then, for every candidate and for the tagged solid draws next to them, every
constant block the VS reads is printed by name at its reflected offset, and the
shader is disassembled: the DXBC names the cb register and the vector index that
feed o0 (SV_POSITION) outright, so the clip matrix does not have to be guessed.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_glass_binos2.txt
"""
import math
import os
import re
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame18022.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_glass_binos2.txt")

MAX_BLOCK = 65536
LO_POS, HI_POS = 1990, 2102       # draw positions to profile with post-VS

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def en(v):
    return str(v).split(".")[-1].split(":")[0]


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


def classify(m):
    col3 = (m[3], m[7], m[11], m[15])
    row3 = (m[12], m[13], m[14], m[15])
    n = []
    if abs(col3[3]) < 1e-3 and max(abs(x) for x in col3[:3]) > 0.5:
        n.append("PROJECTIVE, w from last COLUMN (%+.3f %+.3f %+.3f %+.3f)"
                 % col3)
    if abs(row3[3]) < 1e-3 and max(abs(x) for x in row3[:3]) > 0.5:
        n.append("PROJECTIVE-T, w from last ROW (%+.3f %+.3f %+.3f %+.3f)"
                 % row3)
    if abs(col3[3] - 1.0) < 1e-3 and max(abs(x) for x in col3[:3]) < 1e-3:
        n.append("AFFINE, translation in last ROW (%+.2f %+.2f %+.2f)"
                 % (m[12], m[13], m[14]))
    if abs(row3[3] - 1.0) < 1e-3 and max(abs(x) for x in row3[:3]) < 1e-3:
        n.append("AFFINE-T, translation in last COLUMN (%+.2f %+.2f %+.2f)"
                 % (m[3], m[7], m[11]))
    return "; ".join(n) if n else "no standard projective/affine shape"


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open: %s" % CAP)

    names = {}
    try:
        for r in controller.GetResources():
            names[str(r.resourceId)] = r.name
    except Exception as e:
        say("GetResources failed: %s" % e)

    sf = controller.GetStructuredFile()
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
    say("%d drawcalls" % len(draws))

    # ---------------- post-VS profile of the frame tail -------------------
    say("")
    say("=" * 100)
    say("== post-VS profile, draw positions %d..%d ==" % (LO_POS, HI_POS))
    say("  w = clip w = view-space distance.  Viewmodel geometry is <1; the "
        "world is metres away.")
    say("  %5s %6s %8s %-6s %-6s %8s %8s %-26s %-26s %s"
        % ("pos", "eid", "idx", "blend", "tag", "wmin", "wmax",
           "ndc x range", "ndc y range", "coverage"))

    prof = {}
    for pos in range(LO_POS, min(HI_POS, len(draws))):
        a = draws[pos]
        eid = a.eventId
        try:
            controller.SetFrameEvent(eid, True)
        except Exception as e:
            say("  %5d %6d SetFrameEvent failed: %s" % (pos, eid, e))
            continue
        d3d = controller.GetD3D11PipelineState()
        om = d3d.outputMerger
        ds = om.depthStencilState
        f = ds.frontFace
        tagged = bool(ds.stencilEnable) and int(f.compareMask) == 0x40 \
            and (int(f.reference) & 0x40)
        bl = om.blendState.blends[0] if len(om.blendState.blends) else None
        blended = bool(bl.enabled) if bl is not None else False

        wmin = wmax = None
        xr = yr = (None, None)
        cov = 0.0
        try:
            pv = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            if pv.vertexResourceId != rd.ResourceId.Null():
                stride = int(pv.vertexByteStride)
                nv = int(getattr(pv, "numIndices", 0))
                nv = min(nv if nv else 0, 600)
                if nv and stride >= 16:
                    raw = bytes(controller.GetBufferData(
                        pv.vertexResourceId, int(pv.vertexByteOffset),
                        stride * nv))
                    xs, ys, ws = [], [], []
                    for k in range(nv):
                        o = k * stride
                        if o + 16 > len(raw):
                            break
                        p = struct.unpack_from("<4f", raw, o)
                        if not all(map(math.isfinite, p)) or abs(p[3]) < 1e-9:
                            continue
                        ws.append(p[3])
                        xs.append(p[0] / p[3])
                        ys.append(p[1] / p[3])
                    if ws:
                        wmin, wmax = min(ws), max(ws)
                        xr = (min(xs), max(xs))
                        yr = (min(ys), max(ys))
                        cov = (min(xr[1], 1) - max(xr[0], -1)) * \
                              (min(yr[1], 1) - max(yr[0], -1)) / 4.0
        except Exception as e:
            say("  %5d %6d post-VS failed: %s" % (pos, eid, e))

        prof[pos] = dict(eid=eid, idx=int(getattr(a, "numIndices", 0)),
                         blended=blended, tagged=tagged, wmin=wmin, wmax=wmax,
                         xr=xr, yr=yr, cov=cov,
                         vs=str(d3d.vertexShader.resourceId),
                         ps=str(d3d.pixelShader.resourceId))
        say("  %5d %6d %8d %-6s %-6s %8s %8s %-26s %-26s %6.1f%%  vs=%s ps=%s"
            % (pos, eid, prof[pos]["idx"], "BLEND" if blended else "",
               "TAG" if tagged else "",
               "%.3f" % wmin if wmin is not None else "-",
               "%.3f" % wmax if wmax is not None else "-",
               "%.2f..%.2f" % xr if xr[0] is not None else "-",
               "%.2f..%.2f" % yr if yr[0] is not None else "-",
               100.0 * max(cov, 0.0),
               prof[pos]["vs"], prof[pos]["ps"]))

    # ---------------- pick the candidates ---------------------------------
    tagpos = [p for p in prof if prof[p]["tagged"]]
    say("")
    say("tagged positions: %s" % sorted(tagpos))
    vm_w = [prof[p]["wmax"] for p in tagpos if prof[p]["wmax"] is not None]
    say("tagged (viewmodel) clip-w range: %s"
        % ("%.3f .. %.3f" % (min(vm_w), max(vm_w)) if vm_w else "unknown"))

    near = 2.0
    if vm_w:
        near = max(vm_w) * 3.0
    cands = [p for p in sorted(prof)
             if prof[p]["blended"] and prof[p]["wmax"] is not None
             and prof[p]["wmax"] <= near]
    say("")
    say("== BLENDED draws whose geometry is as close to the camera as the "
        "viewmodel (w <= %.3f) ==" % near)
    for p in cands:
        say("  pos %-5d eid %-6d idx=%-7d w %.3f..%.3f  cover %.1f%%  %s"
            % (p, prof[p]["eid"], prof[p]["idx"], prof[p]["wmin"],
               prof[p]["wmax"], 100 * prof[p]["cov"],
               "TAGGED" if prof[p]["tagged"] else "untagged"))

    targets = [(prof[p]["eid"], "GLASS-CAND" if not prof[p]["tagged"]
                else "TAGGED-BLENDED") for p in cands]
    for p in sorted(tagpos):
        if not prof[p]["tagged"]:
            continue
        targets.append((prof[p]["eid"], "TAGGED-SOLID"))
    # also always look at the draw immediately after the tagged pass
    if tagpos:
        after = max(tagpos) + 1
        if after in prof and prof[after]["eid"] not in [t[0] for t in targets]:
            targets.append((prof[after]["eid"], "AFTER-PASS"))

    dis_targets = []
    try:
        dis_targets = list(controller.GetDisassemblyTargets(False))
    except Exception:
        pass

    summary = []

    def dump(eid, tag):
        a = next((d for d in draws if d.eventId == eid), None)
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        d3d = controller.GetD3D11PipelineState()
        vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        eps = controller.GetShaderEntryPoints(vsid)
        refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])

        say("")
        say("=" * 100)
        say("  eid %-6d [%s]  %d indices  vs=%s(%s)  ps=%s(%s)"
            % (eid, tag, getattr(a, "numIndices", 0), vsid,
               names.get(str(vsid), ""), d3d.pixelShader.resourceId,
               names.get(str(d3d.pixelShader.resourceId), "")))

        # textures the pixel shader samples - names sometimes say what it is
        try:
            ros = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel)
            tex = []
            for ud in ros:
                d = getattr(ud, "descriptor", None)
                if d is None:
                    continue
                rid = getattr(d, "resource", None)
                if rid is None or rid == rd.ResourceId.Null():
                    continue
                tex.append("%s(%s)" % (rid, names.get(str(rid), "")))
            say("  PS textures: %s" % (", ".join(tex[:12]) or "none"))
        except Exception as e:
            say("  PS textures unavailable: %s" % e)

        say("  -- D3D11 VS constant-buffer slots (register truth) --")
        try:
            for si, b in enumerate(d3d.vertexShader.constantBuffers):
                rid = getattr(b, "resourceId", None)
                if rid is None or rid == rd.ResourceId.Null():
                    continue
                say("     b%-2d %s  vecOffset=%s vecCount=%s"
                    % (si, rid, getattr(b, "vecOffset", "?"),
                       getattr(b, "vecCount", "?")))
        except Exception as e:
            say("     unavailable: %s" % e)

        blocks = list(pipe.GetConstantBlocks(rd.ShaderStage.Vertex))
        reads = {}
        for ud in blocks:
            acc = getattr(ud, "access", None)
            aidx = int(getattr(acc, "index", -1)) if acc is not None else -1
            d = getattr(ud, "descriptor", None)
            if d is None:
                continue
            rid = d.resource
            boff = int(getattr(d, "byteOffset", 0))
            bsz = int(getattr(d, "byteSize", 0))
            if rid == rd.ResourceId.Null():
                continue
            want = min(bsz, MAX_BLOCK) if bsz else MAX_BLOCK
            try:
                data = bytes(controller.GetBufferData(rid, boff, want))
            except Exception as e:
                say("     descriptor[%d] read failed: %s" % (aidx, e))
                continue
            reads[aidx] = (rid, boff, bsz, data)

        for bi, cbk in enumerate(refl.constantBlocks):
            got = reads.get(bi)
            if got is None:
                got = next((v for v in reads.values()
                            if len(v[3]) == cbk.byteSize), None)
            say("")
            say("  ***** %s  (HLSL register b%s, descriptor %d, %d B declared, "
                "%d B read) *****"
                % (cbk.name, cbk.fixedBindNumber, bi, cbk.byteSize,
                   len(got[3]) if got else 0))
            if got is None:
                say("     NO BOUND BUFFER")
                continue
            rid, boff, bsz, data = got
            say("     buffer %s  byteOffset 0x%X  byteSize %d" % (rid, boff, bsz))
            for v in cbk.variables:
                off = int(v.byteOffset)
                nr, nc, tn = shape_of(v)
                n = max(nr * nc, 1)
                if off + n * 4 > len(data):
                    say("     +0x%04X %-30s %dx%d %-14s (past end)"
                        % (off, v.name, nr, nc, tn))
                    continue
                vals = struct.unpack_from("<%df" % n, data, off)
                if not all(map(math.isfinite, vals)):
                    say("     +0x%04X %-30s %dx%d %-14s non-finite"
                        % (off, v.name, nr, nc, tn))
                    continue
                if nr >= 3 and nc >= 4:
                    say("     +0x%04X %-30s %dx%d %s" % (off, v.name, nr, nc, tn))
                    for rr in range(nr):
                        say("            [%s]" % " ".join(
                            "%+11.4f" % vals[rr * nc + c] for c in range(nc)))
                    if n >= 16:
                        say("            -> %s" % classify(vals[:16]))
                        summary.append((eid, tag, cbk.name,
                                        cbk.fixedBindNumber, off, v.name,
                                        tuple(vals[:16])))
                else:
                    say("     +0x%04X %-30s %dx%d %-14s = %s%s"
                        % (off, v.name, nr, nc, tn,
                           " ".join("%+.4f" % x for x in vals[:8]),
                           " ..." if n > 8 else ""))

        say("")
        say("  -- disassembly: what feeds o0 (SV_POSITION)? --")
        try:
            tgt = dis_targets[0] if dis_targets else ""
            dis = controller.DisassembleShader(rd.ResourceId.Null(), refl, tgt)
        except Exception as e:
            dis = ""
            say("     DisassembleShader failed: %s" % e)
        if dis:
            lines = dis.split("\n")
            say("     %d lines" % len(lines))
            for ln in lines:
                s = ln.strip()
                if s.startswith("dcl_constantbuffer") or \
                        s.startswith("dcl_output") or s.startswith("dcl_input"):
                    say("     | %s" % s[:150])
            keep = [i for i, ln in enumerate(lines) if re.search(r"\bo0(\.|,|\s)", ln)]
            ctx = set()
            for i in keep:
                for j in range(max(0, i - 8), min(len(lines), i + 2)):
                    ctx.add(j)
            say("     ---- o0 writes and their feeders ----")
            for j in sorted(ctx):
                say("     %4d| %s" % (j, lines[j].rstrip()[:150]))
            refs = {}
            for ln in lines:
                for m2 in re.finditer(r"cb(\d+)\[(\d+)\]", ln):
                    refs.setdefault(int(m2.group(1)), set()).add(int(m2.group(2)))
            say("     ---- cb vectors referenced anywhere ----")
            for reg in sorted(refs):
                idxs = sorted(refs[reg])
                say("     cb%d: vectors %s" % (reg, idxs[:48]))
                say("          byte offsets %s"
                    % ", ".join("0x%X" % (x * 16) for x in idxs[:48]))

    for eid, tag in targets:
        try:
            dump(eid, tag)
        except Exception:
            say("EXC dumping eid %d:" % eid)
            say(traceback.format_exc())

    say("")
    say("=" * 100)
    say("== every 4x4 seen ==")
    for eid, tag, blk, reg, off, name, m in summary:
        say("  eid %-6d %-16s %-18s b%-2s +0x%04X %-26s  col3=(%+.3f %+.3f "
            "%+.3f %+.3f) row3=(%+.3f %+.3f %+.3f %+.3f)"
            % (eid, tag, blk, reg, off, name, m[3], m[7], m[11], m[15],
               m[12], m[13], m[14], m[15]))

    say("")
    say("== glass matrices vs the tagged draws' matrices ==")
    solids = [s for s in summary if "TAGGED" in s[1]]
    for g in [s for s in summary if "TAGGED" not in s[1]]:
        best = None
        for s in solids:
            d = max(abs(x - y) for x, y in zip(g[6], s[6]))
            if best is None or d < best[0]:
                best = (d, s)
        if best:
            say("  eid %d %s+0x%04X %-24s  <-> eid %d %s+0x%04X %-24s  "
                "max|diff|=%.4f"
                % (g[0], g[2], g[4], g[5], best[1][0], best[1][2], best[1][4],
                   best[1][5], best[0]))

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
