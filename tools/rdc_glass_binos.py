"""BINOCULAR GLASS: where does its vertex shader keep the clip-space matrix?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_glass_binos.py

The mod gives the viewmodel stereo depth by shifting clip-space X on the draw's
projection matrix.  It finds first-person draws by the engine's own tag
(StencilEnable, StencilReadMask == 0x40 EXACTLY, ref & 0x40) - 57/57 viewmodel,
0/4319 world.  Scope/binocular GLASS is a separate ALPHA-BLENDED pass that does
NOT carry that tag; the mod catches it as "blended, within 4 draws of a tagged
draw", and shifting +0x00 / +0x40 there makes the lenses DETACH and move
up-and-right.  A clip-space X shift can only move things horizontally, so the
field being modified is not a clip matrix at all.

So: sweep the frame, find the tagged pass, find the blended draws inside it, and
for each of those print EVERY constant block the VS reads, EVERY variable by name
at its reflected offset, with values - and then DISASSEMBLE the vertex shader and
show every instruction that writes o0 (SV_POSITION).  The disassembly names the
cb register and the vector index outright, so nothing has to be inferred.

TRAPS ALREADY PAID FOR (see rdc_instconsts.py / rdc_cb_api.py):
  * PipeState.GetConstantBuffer does not exist on 1.45; it is GetConstantBlocks
    returning UsedDescriptor.  A bare `except: continue` hides that.
  * UsedDescriptor.access.index is the DESCRIPTOR POSITION, i.e. the index into
    refl.constantBlocks - NOT the HLSL register number (fixedBindNumber).
  * The Descriptor from .descriptor is a temporary SWIG proxy: copy the fields
    and read the buffer immediately.
  * Constant offsets are PER SHADER.  Walk refl.constantBlocks, never hardcode.
  * Do not skip big blocks: LocalConstants is 6944 B and is exactly the one that
    matters.  Limit here is 65536.

Outputs:
    %LOCALAPPDATA%\theHunterCotWVR\rdc_glass_binos.txt   the analysis
    %LOCALAPPDATA%\theHunterCotWVR\rdc_glass_binos.csv   per-draw state sweep
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame18022.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_glass_binos.txt")
CSV = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_glass_binos.csv")

MAX_BLOCK = 65536          # never "skip as bulk array"
GLASS_WINDOW = 6           # draws either side of a tagged draw

out = open(OUT, "w")
csv = open(CSV, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def en(v):
    return str(v).split(".")[-1].split(":")[0]


def shape_of(v):
    """(rows, cols, typename) however this build spells it."""
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
    """What shape is this 4x4, read as 16 floats in buffer order?

    Row-vector convention (clip = v . M) puts the projective term in the last
    COLUMN: (m3, m7, m11, m15) ~ (0,0,+-1,0).
    Column-vector / HLSL column_major storage puts it in the last ROW:
    (m12,m13,m14,m15) ~ (0,0,+-1,0).
    An affine World/View matrix has (0,0,0,1) in whichever slot.
    """
    col3 = (m[3], m[7], m[11], m[15])
    row3 = (m[12], m[13], m[14], m[15])
    notes = []
    if abs(col3[3]) < 1e-4 and (abs(col3[2]) > 0.5 or abs(col3[0]) > 0.5
                                or abs(col3[1]) > 0.5):
        notes.append("PROJECTIVE (last column %s)"
                     % ("%+.3f %+.3f %+.3f %+.3f" % col3))
    if abs(row3[3]) < 1e-4 and (abs(row3[2]) > 0.5 or abs(row3[0]) > 0.5
                                or abs(row3[1]) > 0.5):
        notes.append("PROJECTIVE-T (last row %s)"
                     % ("%+.3f %+.3f %+.3f %+.3f" % row3))
    if abs(col3[3] - 1.0) < 1e-4 and abs(col3[0]) < 1e-4 \
            and abs(col3[1]) < 1e-4 and abs(col3[2]) < 1e-4:
        notes.append("affine (last column 0,0,0,1) - translation in last ROW "
                     "(%+.2f %+.2f %+.2f)" % (m[12], m[13], m[14]))
    if abs(row3[3] - 1.0) < 1e-4 and abs(row3[0]) < 1e-4 \
            and abs(row3[1]) < 1e-4 and abs(row3[2]) < 1e-4:
        notes.append("affine-T (last row 0,0,0,1) - translation in last COLUMN "
                     "(%+.2f %+.2f %+.2f)" % (m[3], m[7], m[11]))
    return "; ".join(notes) if notes else "neither (not a standard transform?)"


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open: %s" % CAP)

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
    by_eid = {a.eventId: a for a in draws}
    say("%d drawcalls in the frame" % len(draws))

    # ---------------- stage 1: state sweep -------------------------------
    csv.write("eid,numIndices,numInstances,stencilEnable,readMask,writeMask,"
              "ref,fFunc,fPass,depthEnable,depthWrites,depthFunc,blend0En,"
              "blendSrc,blendDst,blendOp,blendWriteMask,vs,ps,name\n")
    rows = []
    for i, a in enumerate(draws):
        eid = a.eventId
        controller.SetFrameEvent(eid, False)
        d3d = controller.GetD3D11PipelineState()
        om = d3d.outputMerger
        ds = om.depthStencilState
        f = ds.frontFace
        bs = om.blendState
        b0 = bs.blends[0] if len(bs.blends) else None
        cb = b0.colorBlend if b0 is not None else None
        r = {
            "eid": eid,
            "numIndices": int(getattr(a, "numIndices", 0)),
            "numInstances": int(getattr(a, "numInstances", 0)),
            "stencilEnable": int(bool(ds.stencilEnable)),
            "readMask": int(f.compareMask),
            "writeMask": int(f.writeMask),
            "ref": int(f.reference),
            "fFunc": en(f.function),
            "fPass": en(f.passOperation),
            "depthEnable": int(bool(ds.depthEnable)),
            "depthWrites": int(bool(ds.depthWrites)),
            "depthFunc": en(ds.depthFunction),
            "blend0En": int(bool(b0.enabled)) if b0 is not None else 0,
            "blendSrc": en(cb.source) if cb is not None else "?",
            "blendDst": en(cb.destination) if cb is not None else "?",
            "blendOp": en(cb.operation) if cb is not None else "?",
            "blendWriteMask": int(b0.writeMask) if b0 is not None else 0,
            "vs": str(d3d.vertexShader.resourceId),
            "ps": str(d3d.pixelShader.resourceId),
            "name": a.GetName(sf).split("(")[0].replace(",", ";")[:40],
        }
        rows.append(r)
        csv.write(",".join(str(r[k]) for k in (
            "eid", "numIndices", "numInstances", "stencilEnable", "readMask",
            "writeMask", "ref", "fFunc", "fPass", "depthEnable", "depthWrites",
            "depthFunc", "blend0En", "blendSrc", "blendDst", "blendOp",
            "blendWriteMask", "vs", "ps", "name")) + "\n")
        if i % 250 == 0:
            csv.flush()
            say("  ... swept %d/%d (eid %d)" % (i, len(draws), eid))
    csv.flush()

    by_r = {r["eid"]: r for r in rows}

    # ---------------- stage 2: the first-person pass ---------------------
    fp = [r for r in rows
          if r["stencilEnable"] and r["readMask"] == 0x40 and (r["ref"] & 0x40)]
    say("")
    say("=" * 78)
    say("== first-person tag (stencilEnable, readMask==0x40, ref&0x40) ==")
    say("  %d of %d draws" % (len(fp), len(rows)))
    if not fp:
        say("  NOTHING TAGGED - stopping")
        controller.Shutdown()
        cap.Shutdown()
        return
    say("  eids %d .. %d" % (fp[0]["eid"], fp[-1]["eid"]))
    say("  %6s %8s %6s %-10s %-10s %-8s %s"
        % ("eid", "idx", "blend", "rd/wr/ref", "depth", "vs", "name"))
    for r in fp:
        say("  %6d %8d %6d 0x%02X/0x%02X/0x%02X dE%d dW%d %-14s %s"
            % (r["eid"], r["numIndices"], r["blend0En"], r["readMask"],
               r["writeMask"], r["ref"], r["depthEnable"], r["depthWrites"],
               r["vs"][:14], r["name"]))

    fp_eids = sorted(r["eid"] for r in fp)
    lo, hi = fp_eids[0], fp_eids[-1]

    # ---------------- stage 3: the blended draws around them -------------
    say("")
    say("=" * 78)
    say("== every draw in [%d-8 .. %d+8], blended ones flagged ==" % (lo, hi))
    say("  %6s %8s %-6s %-6s %-22s %-14s %s"
        % ("eid", "idx", "tagged", "blend", "src/dst/op", "vs", "name"))
    for r in rows:
        if not (lo - 8 <= r["eid"] <= hi + 8):
            continue
        tagged = (r["stencilEnable"] and r["readMask"] == 0x40
                  and (r["ref"] & 0x40))
        say("  %6d %8d %-6s %-6s %-22s %-14s %s"
            % (r["eid"], r["numIndices"], "TAG" if tagged else "",
               "BLEND" if r["blend0En"] else "",
               "%s/%s/%s" % (r["blendSrc"], r["blendDst"], r["blendOp"]),
               r["vs"][:14], r["name"]))

    glass = []
    for r in rows:
        if not r["blend0En"]:
            continue
        tagged = (r["stencilEnable"] and r["readMask"] == 0x40
                  and (r["ref"] & 0x40))
        if tagged:
            continue
        near = min((abs(r["eid"] - e) for e in fp_eids), default=10 ** 9)
        if near <= GLASS_WINDOW:
            glass.append((r, near))
    say("")
    say("== GLASS CANDIDATES: blended, untagged, within %d draws of a tag =="
        % GLASS_WINDOW)
    say("  %d candidate(s)" % len(glass))
    for r, near in glass:
        say("  eid %-6d idx=%-7d dist=%d  %s/%s  vs=%s ps=%s  %s"
            % (r["eid"], r["numIndices"], near, r["blendSrc"], r["blendDst"],
               r["vs"][:14], r["ps"][:14], r["name"]))

    # a solid tagged draw next to each glass draw, for the comparison
    def nearest_tag(eid):
        return min(fp_eids, key=lambda e: abs(e - eid))

    targets = []
    for r, near in glass:
        targets.append((r["eid"], "GLASS"))
    seen = set()
    for r, near in glass:
        t = nearest_tag(r["eid"])
        if t not in seen:
            seen.add(t)
            targets.append((t, "SOLID-NEIGHBOUR"))

    # ---------------- stage 4: constants + disassembly -------------------
    dis_targets = []
    try:
        dis_targets = list(controller.GetDisassemblyTargets(False))
    except Exception as e:
        say("GetDisassemblyTargets failed: %s" % e)
    say("")
    say("disassembly targets: %s" % dis_targets)

    summary = []

    def dump(eid, tag):
        a = by_eid.get(eid)
        r = by_r.get(eid, {})
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        d3d = controller.GetD3D11PipelineState()
        try:
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        except Exception as e:
            say("  eid %d reflection failed: %s" % (eid, e))
            say(traceback.format_exc())
            return

        say("")
        say("=" * 78)
        say("  eid %-6d [%s]  %d indices  vs=%s  ps=%s"
            % (eid, tag, getattr(a, "numIndices", 0), vsid,
               d3d.pixelShader.resourceId))
        say("  blend %s/%s/%s   stencil rd=0x%02X wr=0x%02X ref=0x%02X %s"
            % (r.get("blendSrc"), r.get("blendDst"), r.get("blendOp"),
               r.get("readMask", 0), r.get("writeMask", 0), r.get("ref", 0),
               r.get("fFunc")))

        # ---- what the D3D11 state says is bound where (register truth) ----
        say("  -- D3D11 VS constant-buffer slots --")
        try:
            for si, b in enumerate(d3d.vertexShader.constantBuffers):
                rid = getattr(b, "resourceId", None)
                if rid is None or rid == rd.ResourceId.Null():
                    continue
                say("     b%-2d %s  vecOffset=%s vecCount=%s"
                    % (si, rid, getattr(b, "vecOffset", "?"),
                       getattr(b, "vecCount", "?")))
        except Exception as e:
            say("     (d3d11 vertexShader.constantBuffers unavailable: %s)" % e)

        # ---- the descriptor list, mapped by access.index ------------------
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

        say("  -- declared blocks (%d) / descriptor reads (%d) --"
            % (len(refl.constantBlocks), len(reads)))
        for bi, cbk in enumerate(refl.constantBlocks):
            got = reads.get(bi)
            if got is None:
                # fall back: match by size
                got = next((v for v in reads.values()
                            if len(v[3]) == cbk.byteSize), None)
            say("")
            say("  ***** %s  (HLSL register b%s, descriptor index %d, "
                "%d bytes declared, %d read) *****"
                % (cbk.name, cbk.fixedBindNumber, bi, cbk.byteSize,
                   len(got[3]) if got else 0))
            if got is None:
                say("     NO BOUND BUFFER FOUND FOR THIS BLOCK")
                continue
            rid, boff, bsz, data = got
            say("     buffer %s off 0x%X size %d" % (rid, boff, bsz))
            for v in cbk.variables:
                off = int(v.byteOffset)
                nrows, ncols, tn = shape_of(v)
                n = max(nrows * ncols, 1)
                if off + n * 4 > len(data):
                    say("     +0x%04X %-30s %dx%d %-12s (past end of read)"
                        % (off, v.name, nrows, ncols, tn))
                    continue
                vals = struct.unpack_from("<%df" % n, data, off)
                if not all(map(math.isfinite, vals)):
                    say("     +0x%04X %-30s %dx%d %-12s non-finite"
                        % (off, v.name, nrows, ncols, tn))
                    continue
                if nrows >= 4 and ncols >= 4:
                    say("     +0x%04X %-30s %dx%d %s"
                        % (off, v.name, nrows, ncols, tn))
                    for rr in range(nrows):
                        say("            [%s]" % " ".join(
                            "%+11.4f" % vals[rr * ncols + c]
                            for c in range(ncols)))
                    say("            -> %s" % classify(vals[:16]))
                    summary.append((eid, tag, cbk.name, cbk.fixedBindNumber,
                                    off, v.name, tuple(vals[:16])))
                elif nrows >= 3 and ncols >= 4:
                    say("     +0x%04X %-30s %dx%d %s"
                        % (off, v.name, nrows, ncols, tn))
                    for rr in range(nrows):
                        say("            [%s]" % " ".join(
                            "%+11.4f" % vals[rr * ncols + c]
                            for c in range(ncols)))
                else:
                    say("     +0x%04X %-30s %dx%d %-12s = %s"
                        % (off, v.name, nrows, ncols, tn,
                           " ".join("%+.4f" % x for x in vals[:8])
                           + (" ..." if n > 8 else "")))

        # ---- the disassembly: which cb feeds o0 (SV_POSITION)? -----------
        say("")
        say("  -- vertex shader disassembly: every line touching o0 / cb --")
        try:
            tgt = dis_targets[0] if dis_targets else ""
            dis = controller.DisassembleShader(rd.ResourceId.Null(), refl, tgt)
        except Exception as e:
            dis = ""
            say("     DisassembleShader failed: %s" % e)
        if dis:
            lines = dis.split("\n")
            say("     (%d lines; showing declarations, the o0 writes and "
                "their feeders)" % len(lines))
            for ln in lines:
                s = ln.strip()
                if s.startswith("dcl_") or s.startswith("//") and "cb" in s:
                    say("     | %s" % s[:150])
            say("     ---- instructions writing o0 ----")
            keep = []
            for i2, ln in enumerate(lines):
                if "o0." in ln or " o0" in ln:
                    keep.append(i2)
            ctx = set()
            for i2 in keep:
                for j in range(max(0, i2 - 6), min(len(lines), i2 + 2)):
                    ctx.add(j)
            for j in sorted(ctx):
                say("     %4d| %s" % (j, lines[j].rstrip()[:150]))
            say("     ---- every cb reference in the whole shader ----")
            refs = {}
            import re
            for ln in lines:
                for m2 in re.finditer(r"cb(\d+)\[(\d+)\]", ln):
                    refs.setdefault(int(m2.group(1)), set()).add(int(m2.group(2)))
            for reg in sorted(refs):
                idxs = sorted(refs[reg])
                say("     cb%d[] vectors used: %s"
                    % (reg, ", ".join(str(x) for x in idxs[:40])
                       + (" ..." if len(idxs) > 40 else "")))
                say("            -> byte offsets: %s"
                    % ", ".join("0x%X" % (x * 16) for x in idxs[:40]))

        # ---- post-VS: what clip positions did this draw actually make? ---
        try:
            pv = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            if pv.vertexResourceId != rd.ResourceId.Null():
                stride = int(pv.vertexByteStride)
                nverts = min(int(getattr(pv, "numIndices", 0)) or 8, 8)
                raw = bytes(controller.GetBufferData(
                    pv.vertexResourceId, int(pv.vertexByteOffset),
                    stride * nverts))
                say("")
                say("  -- post-VS clip positions (first %d verts, stride %d) --"
                    % (nverts, stride))
                for k in range(nverts):
                    if (k + 1) * stride > len(raw):
                        break
                    p = struct.unpack_from("<4f", raw, k * stride)
                    ndc = ("(%.3f %.3f %.3f)"
                           % (p[0] / p[3], p[1] / p[3], p[2] / p[3])) \
                        if abs(p[3]) > 1e-6 else "(w~0)"
                    say("     v%-3d clip (%+9.3f %+9.3f %+9.3f %+9.3f) ndc %s"
                        % (k, p[0], p[1], p[2], p[3], ndc))
            else:
                say("  -- post-VS: no data --")
        except Exception as e:
            say("  -- post-VS failed: %s --" % e)

    for eid, tag in targets:
        try:
            dump(eid, tag)
        except Exception:
            say("EXC while dumping eid %d:" % eid)
            say(traceback.format_exc())

    # ---------------- stage 5: cross-compare ------------------------------
    say("")
    say("=" * 78)
    say("== 4x4 matrices seen, glass vs solid ==")
    say("  %-6s %-16s %-16s %-8s %-28s %s"
        % ("eid", "tag", "block", "offset", "name", "last col / last row"))
    for eid, tag, blk, reg, off, name, m in summary:
        say("  %-6d %-16s %-16s +0x%04X  %-28s col3=(%+.3f %+.3f %+.3f %+.3f) "
            "row3=(%+.3f %+.3f %+.3f %+.3f)"
            % (eid, tag, blk, off, name, m[3], m[7], m[11], m[15],
               m[12], m[13], m[14], m[15]))

    say("")
    say("== do any glass matrices match a solid neighbour's WVP? ==")
    solids = [s for s in summary if s[1] == "SOLID-NEIGHBOUR"]
    glasses = [s for s in summary if s[1] == "GLASS"]
    for g in glasses:
        best = None
        for s in solids:
            d = max(abs(a - b) for a, b in zip(g[6], s[6]))
            if best is None or d < best[0]:
                best = (d, s)
        if best:
            say("  glass eid %d %s+0x%04X %-24s  closest solid: eid %d %s+0x%04X"
                " %-24s  max|diff| = %.4f"
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
csv.close()
os._exit(0)
