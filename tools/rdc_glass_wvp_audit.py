"""AUDIT: where does the scope/binocular GLASS keep its clip matrix?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_glass_wvp_audit.py
    set COTW_CAP=...\cotw_frame28475.rdc  to point it at the other capture

Nothing here is taken on trust.  Everything is re-measured:

  1. Full state sweep of every drawcall: stencil (enable/read/write/ref), blend,
     VS/PS, index count.  Tagged = stencilEnable && readMask==0x40 && ref&0x40.
  2. GLASS CANDIDATES using the MOD'S OWN semantics, which are forward-only:
     sinceStencil resets to 0 at a tagged draw and increments per draw, glassHit
     is (!tagged && blended && sinceStencil <= weapon_3d_glass(=4)).  So the
     window is draw POSITIONS t+1..t+4 after a tagged draw, not +-4, not eids.
  3. For every tagged draw and every candidate: the descriptor actually bound at
     VS slot b1 (resource, byteOffset==firstConstant*16, byteSize), the shader's
     reflected block list, EVERY 4x4 variable by name at its reflected offset,
     raw 4th-column bit patterns, and the ground truth WVP offset from the NAME.
  4. Full DXBC disassembly of every candidate VS.
  5. Arithmetic proof with EXACT per-vertex correspondence via the index buffers
     (not a nearest-neighbour fudge): position k of the draw maps input vertex
     origIdx[k] to post-VS vertex pvIdx[k].
  6. Rule evaluation: current rule (|translation|>100) and the proposed rule
     (4th column of +0x00 exactly (0,0,0,1)) against ground truth, per draw.

TRAPS (already paid for):
  * No PipeState.GetConstantBuffer on 1.45 - GetConstantBlocks -> UsedDescriptor.
  * UsedDescriptor.access.index is the DESCRIPTOR POSITION (index into
    refl.constantBlocks), NOT the HLSL register number.
  * Descriptor from .descriptor is a temporary SWIG proxy - copy fields, read now.
  * Constant offsets are PER SHADER.  Walk refl.constantBlocks.
  * Never skip a block as "bulk array" - limit here is 65536.
  * d3d11.vertexShader has no .constantBuffers on this build.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_glass_wvp_audit_<capname>.txt
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.environ.get("COTW_CAP") or os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame18022.rdc")
TAG = os.path.splitext(os.path.basename(CAP))[0]
OUT = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_glass_wvp_audit_" + TAG + ".txt")

MAX_BLOCK = 65536
GLASS_FWD = 4          # Cfg().weapon_3d_glass default
MAX_VERT = 2500

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


def bits(f):
    return "%08X" % struct.unpack("<I", struct.pack("<f", f))[0]


def is_affine(m):
    """4th COLUMN exactly (0,0,0,1) - the proposed rule's test, bit-exact."""
    return m[3] == 0.0 and m[7] == 0.0 and m[11] == 0.0 and m[15] == 1.0


def all_finite(m):
    return all(map(math.isfinite, m))


def rule_proposed(data):
    if len(data) < 0x80:
        return 0, "block < 0x80"
    m0 = struct.unpack_from("<16f", data, 0)
    if not all_finite(m0):
        return 0, "+0x00 non-finite"
    if not is_affine(m0):
        return 0, "+0x00 not affine -> it IS the clip matrix"
    m1 = struct.unpack_from("<16f", data, 0x40)
    if not all_finite(m1):
        return 0, "+0x40 non-finite -> refuse"
    if is_affine(m1):
        return 0, "+0x40 also affine -> refuse"
    return 0x40, "+0x00 affine, +0x40 projective"


def rule_current(data):
    if len(data) < 0x80:
        return 0, "block < 0x80"
    m = struct.unpack_from("<16f", data, 0)
    tx, ty, tz = m[12], m[13], m[14]
    if not (math.isfinite(tx) and math.isfinite(ty) and math.isfinite(tz)):
        return 0, "non-finite"
    ws = abs(tx) > 100.0 or abs(ty) > 100.0 or abs(tz) > 100.0
    return (0x40, "|t|>100 -> World at +0x00") if ws else \
           (0, "|t|<=100 -> assumed WVP at +0x00")


def read_blocks(controller, pipe, refl):
    got = {}
    try:
        uds = list(pipe.GetConstantBlocks(rd.ShaderStage.Vertex))
    except Exception as e:
        say("      GetConstantBlocks RAISED %s: %s" % (type(e).__name__, e))
        return []
    for ud in uds:
        acc = getattr(ud, "access", None)
        aidx = int(getattr(acc, "index", -1)) if acc is not None else -1
        d = getattr(ud, "descriptor", None)
        if d is None:
            continue
        rid = d.resource
        boff = int(getattr(d, "byteOffset", 0))
        bsz = int(getattr(d, "byteSize", 0))
        if rid == rd.ResourceId.Null():
            got[aidx] = (boff, bsz, rid, b"")
            continue
        try:
            data = bytes(controller.GetBufferData(
                rid, boff, min(bsz, MAX_BLOCK) if bsz else MAX_BLOCK))
        except Exception as e:
            say("      buffer read failed for descriptor %d: %s" % (aidx, e))
            data = b""
        got[aidx] = (boff, bsz, rid, data)

    res = []
    for i, cb in enumerate(refl.constantBlocks):
        boff, bsz, rid, data = got.get(i, (0, 0, rd.ResourceId.Null(), b""))
        res.append((i, cb.name, int(cb.fixedBindNumber), int(cb.byteSize),
                    boff, bsz, rid, data, cb))
    return res


def dump_block(pfx, entry):
    (i, name, reg, decl, boff, bsz, rid, data, cb) = entry
    say("%s[%d] %-18s b%-2d declared %5d B   descriptor res=%s "
        "byteOffset=0x%X (firstConstant=%d) byteSize=%d  read %d B"
        % (pfx, i, name, reg, decl, str(rid), boff, boff // 16, bsz, len(data)))
    mats = []
    for v in cb.variables:
        off = int(v.byteOffset)
        rows, cols, tn = shape_of(v)
        if rows == 4 and cols == 4 and off + 64 <= len(data):
            m = struct.unpack_from("<16f", data, off)
            mats.append((off, v.name, m))
    for off, nm, m in mats:
        say("%s    +0x%04X %-26s col3bits=[%s %s %s %s] affine=%s"
            % (pfx, off, nm, bits(m[3]), bits(m[7]), bits(m[11]), bits(m[15]),
               is_affine(m)))
        for r in range(4):
            say("%s        [%s]" % (pfx, " ".join("%+11.5f" % m[r * 4 + c]
                                                  for c in range(4))))
    for v in cb.variables:
        off = int(v.byteOffset)
        rows, cols, tn = shape_of(v)
        if rows == 4 and cols == 4:
            continue
        n = max(rows * cols, 1)
        if off + n * 4 > len(data):
            say("%s    +0x%04X %-26s %dx%d (past end)"
                % (pfx, off, v.name, rows, cols))
            continue
        vals = struct.unpack_from("<%df" % n, data, off)
        say("%s    +0x%04X %-26s %dx%d = %s"
            % (pfx, off, v.name, rows, cols,
               " ".join("%+.4f" % x for x in vals[:8])))
    return mats


def truth_offset(entry):
    (i, name, reg, decl, boff, bsz, rid, data, cb) = entry
    for v in cb.variables:
        rows, cols, tn = shape_of(v)
        if rows == 4 and cols == 4 and \
                v.name.lower().startswith("worldviewproj"):
            return int(v.byteOffset), v.name
    return None, None


def arith(controller, pipe, eid, b1):
    (bi, bname, breg, bdecl, boff, bsz, brid, data, cb) = b1
    mats = []
    for v in cb.variables:
        off = int(v.byteOffset)
        rows, cols, tn = shape_of(v)
        if rows == 4 and cols == 4 and off + 64 <= len(data):
            mats.append((off, v.name, struct.unpack_from("<16f", data, off)))
    if not mats:
        say("  (no 4x4 in b1 to test)")
        return

    try:
        attrs = list(pipe.GetVertexInputs())
        vbs = list(pipe.GetVBuffers())
    except Exception as e:
        say("  vertex input query failed: %s" % e)
        return
    pos = None
    for at in attrs:
        nm = str(getattr(at, "name", "")).upper()
        if nm.startswith("POSITION") or nm in ("SV_POSITION", "POS"):
            pos = at
            break
    if pos is None:
        say("  no POSITION attribute; attrs = %s"
            % [str(getattr(a, "name", "?")) for a in attrs])
        return
    slot = int(getattr(pos, "vertexBuffer", 0))
    if slot >= len(vbs):
        say("  POSITION slot %d not bound" % slot)
        return
    vb = vbs[slot]
    if vb.resourceId == rd.ResourceId.Null():
        say("  POSITION vertex buffer not bound")
        return
    stride = int(vb.byteStride)
    base = int(vb.byteOffset) + int(getattr(pos, "byteOffset", 0))
    fmt = pos.format
    ctype = en(fmt.compType)
    cw = int(fmt.compByteWidth)
    cc = int(fmt.compCount)
    say("  POSITION: slot %d stride %d base 0x%X  %s x%d x%dB"
        % (slot, stride, base, ctype, cc, cw))
    raw = bytes(controller.GetBufferData(vb.resourceId, base,
                                         stride * MAX_VERT))

    def decode(k):
        o = k * stride
        if o + cw * 3 > len(raw):
            return None
        if cw == 4 and ctype in ("Float", "SFloat"):
            return struct.unpack_from("<3f", raw, o)
        if cw == 2 and ctype in ("Float", "SFloat"):
            return tuple(float(x) for x in struct.unpack_from("<3e", raw, o))
        if cw == 2 and ctype == "SNorm":
            return tuple(x / 32767.0 for x in struct.unpack_from("<3h", raw, o))
        if cw == 2 and ctype == "UNorm":
            return tuple(x / 65535.0 for x in struct.unpack_from("<3H", raw, o))
        if cw == 2 and ctype == "SInt":
            return tuple(float(x) for x in struct.unpack_from("<3h", raw, o))
        return None

    pv = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
    if pv.vertexResourceId == rd.ResourceId.Null():
        say("  no post-VS data")
        return
    pstride = int(pv.vertexByteStride)
    pn = int(getattr(pv, "numIndices", 0))
    say("  post-VS: numIndices %d stride %d idxRes=%s idxStride=%s baseVertex=%s"
        % (pn, pstride, str(getattr(pv, "indexResourceId", "?")),
           getattr(pv, "indexByteStride", "?"), getattr(pv, "baseVertex", "?")))
    n = min(pn, MAX_VERT)
    pvraw = bytes(controller.GetBufferData(pv.vertexResourceId,
                                           int(pv.vertexByteOffset),
                                           pstride * (MAX_VERT + 8)))

    pidx = None
    try:
        if getattr(pv, "indexResourceId",
                   rd.ResourceId.Null()) != rd.ResourceId.Null():
            istr = int(pv.indexByteStride)
            iraw = bytes(controller.GetBufferData(pv.indexResourceId,
                                                  int(pv.indexByteOffset),
                                                  istr * n))
            cnt = len(iraw) // istr
            f = ("<%dI" % cnt) if istr == 4 else ("<%dH" % cnt)
            pidx = list(struct.unpack(f, iraw[:cnt * istr]))[:n]
    except Exception as e:
        say("  post-VS index read failed: %s" % e)

    oidx = None
    try:
        ib = pipe.GetIBuffer()
        if ib is not None and ib.resourceId != rd.ResourceId.Null():
            istr = int(ib.byteStride)
            iraw = bytes(controller.GetBufferData(ib.resourceId,
                                                  int(ib.byteOffset), istr * n))
            cnt = len(iraw) // istr
            f = ("<%dI" % cnt) if istr == 4 else ("<%dH" % cnt)
            oidx = list(struct.unpack(f, iraw[:cnt * istr]))[:n]
    except Exception as e:
        say("  IA index read failed: %s" % e)

    say("  index buffers: post-VS %s, IA %s"
        % ("yes" if pidx else "no", "yes" if oidx else "no"))
    if pidx and oidx:
        say("     first 8 IA indices      %s" % oidx[:8])
        say("     first 8 post-VS indices %s" % pidx[:8])

    def clip_at(j):
        o = j * pstride
        if o + 16 > len(pvraw):
            return None
        c = struct.unpack_from("<4f", pvraw, o)
        return c if all_finite(c) else None

    def row_apply(m, p):
        return tuple(p[0] * m[0 + j] + p[1] * m[4 + j] + p[2] * m[8 + j]
                     + m[12 + j] for j in range(4))

    def col_apply(m, p):
        return tuple(p[0] * m[j * 4 + 0] + p[1] * m[j * 4 + 1]
                     + p[2] * m[j * 4 + 2] + m[j * 4 + 3] for j in range(4))

    pairs = []
    mode = "none"
    if pidx and oidx and len(pidx) == len(oidx):
        seen = set()
        for k in range(len(pidx)):
            key = (oidx[k], pidx[k])
            if key in seen:
                continue
            seen.add(key)
            p = decode(oidx[k])
            c = clip_at(pidx[k])
            if p and c and all_finite(p):
                pairs.append((p, c))
            if len(pairs) >= 400:
                break
        mode = "exact via index buffers"
    if not pairs:
        for k in range(min(n, 400)):
            p = decode(k)
            c = clip_at(k)
            if p and c and all_finite(p):
                pairs.append((p, c))
        mode = "k-to-k fallback"
    say("  correspondence: %s, %d pairs" % (mode, len(pairs)))
    if not pairs:
        return

    say("  -- which 4x4 in %s reproduces the post-VS clip positions? --" % bname)
    best = None
    for off, nm, m in mats:
        for conv, fn in (("row-vector v.M", row_apply),
                         ("col-vector M.v", col_apply)):
            worst = 0.0
            tot = 0.0
            for p, c in pairs:
                t = fn(m, p)
                e = max(abs(c[i] - t[i]) for i in range(4))
                tot += e
                worst = max(worst, e)
            mean = tot / len(pairs)
            hit = worst < 1e-3
            say("     +0x%04X %-26s %-16s mean %.6f worst %.6f%s"
                % (off, nm, conv, mean, worst, "   <<< MATCH" if hit else ""))
            if hit and (best is None or worst < best[3]):
                best = (off, nm, conv, worst)
    if best is None:
        say("     NOTHING IN b1 REPRODUCES THE CLIP POSITIONS")
        return
    say("     -> object-to-clip is %s at +0x%04X, %s"
        % (best[1], best[0], best[2]))

    off, nm, conv, _ = best
    mm = dict((o, list(x)) for o, _n, x in mats)
    m = mm[off]
    k = 0.05
    ms = list(m)
    if conv.startswith("row"):
        ms[0] += k * ms[3]
        ms[4] += k * ms[7]
        ms[8] += k * ms[11]
        ms[12] += k * ms[15]
        fn = row_apply
    else:
        ms[0] += k * ms[12]
        ms[1] += k * ms[13]
        ms[2] += k * ms[14]
        ms[3] += k * ms[15]
        fn = col_apply
    dev = 0.0
    dxmin = dxmax = None
    for p, c in pairs:
        a = fn(m, p)
        b = fn(ms, p)
        dev = max(dev, abs((b[0] - a[0]) - k * a[3]),
                  abs(b[1] - a[1]), abs(b[2] - a[2]), abs(b[3] - a[3]))
        if abs(a[3]) > 1e-9 and abs(b[3]) > 1e-9:
            d = b[0] / b[3] - a[0] / a[3]
            dxmin = d if dxmin is None else min(dxmin, d)
            dxmax = d if dxmax is None else max(dxmax, d)
    say("     shift check k=%.3f at +0x%04X layout %s: max deviation from a pure "
        "clip.x += k*clip.w = %.3e; NDC x moves %.5f..%.5f"
        % (k, off, "0" if conv.startswith("row") else "1", dev,
           dxmin if dxmin is not None else 0.0,
           dxmax if dxmax is not None else 0.0))

    cur, _ = rule_current(data)
    if cur != off:
        say("     the CURRENT rule shifts +0x%04X instead." % cur)
        wm = mm.get(cur)
        if wm is None:
            say("        +0x%04X is not a declared 4x4 here" % cur)
        else:
            w = list(wm)
            w[0] += k * w[3]
            w[4] += k * w[7]
            w[8] += k * w[11]
            w[12] += k * w[15]
            ch = [i for i in range(16) if abs(w[i] - wm[i]) > 1e-12]
            say("        elements changed: %s" % (ch if ch else "NONE"))
            for i in ch:
                say("           m[%d] %+.6f -> %+.6f" % (i, wm[i], w[i]))


def main():
    cap = rd.OpenCaptureFile()
    say("capture: %s" % CAP)
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open")

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
        cbl = b0.colorBlend if b0 is not None else None
        tagged = bool(ds.stencilEnable) and int(f.compareMask) == 0x40 and \
            (int(f.reference) & 0x40) != 0
        rows.append({
            "pos": i, "eid": eid,
            "idx": int(getattr(a, "numIndices", 0)),
            "inst": int(getattr(a, "numInstances", 0)),
            "sEn": int(bool(ds.stencilEnable)),
            "rd": int(f.compareMask), "wr": int(f.writeMask),
            "ref": int(f.reference),
            "blend": int(bool(b0.enabled)) if b0 is not None else 0,
            "bsrc": en(cbl.source) if cbl is not None else "?",
            "bdst": en(cbl.destination) if cbl is not None else "?",
            "bop": en(cbl.operation) if cbl is not None else "?",
            "tagged": tagged,
            "vs": str(d3d.vertexShader.resourceId),
            "ps": str(d3d.pixelShader.resourceId),
            "name": a.GetName(sf).split("(")[0][:34],
        })
        if i % 400 == 0:
            say("  ... swept %d/%d" % (i, len(draws)))

    fp = [r for r in rows if r["tagged"]]
    say("")
    say("=" * 92)
    say("== first-person tag: %d of %d draws ==" % (len(fp), len(rows)))
    for r in fp:
        say("  pos %-5d eid %-6d idx %-7d blend=%d rd/wr/ref=0x%02X/0x%02X/0x%02X"
            " vs=%-16s ps=%-16s" % (r["pos"], r["eid"], r["idx"], r["blend"],
                                    r["rd"], r["wr"], r["ref"], r["vs"], r["ps"]))
    if not fp:
        say("NOTHING TAGGED - stopping")
        controller.Shutdown()
        cap.Shutdown()
        return

    since = 99999
    cands = []
    for r in rows:
        if r["tagged"]:
            since = 0
            continue
        if since < 99999:
            since += 1
        if r["blend"] and since <= GLASS_FWD:
            r["since"] = since
            cands.append(r)
    say("")
    say("== GLASS CANDIDATES by the mod's own rule (untagged, blended, "
        "sinceStencil<=%d DRAW POSITIONS) ==" % GLASS_FWD)
    say("  %d candidate(s)" % len(cands))
    for r in cands:
        say("  pos %-5d eid %-6d since=%d idx=%-7d %s/%s/%s vs=%-16s ps=%-16s %s"
            % (r["pos"], r["eid"], r["since"], r["idx"], r["bsrc"], r["bdst"],
               r["bop"], r["vs"], r["ps"], r["name"]))

    lo = min(r["pos"] for r in fp) - 6
    hi = max(r["pos"] for r in fp) + 14
    say("")
    say("== every draw in draw-POSITION range [%d..%d] ==" % (lo, hi))
    say("  %-6s %-7s %-8s %-4s %-6s %-24s %-16s %-16s %s"
        % ("pos", "eid", "idx", "TAG", "BLEND", "src/dst/op", "vs", "ps", "name"))
    for r in rows:
        if not (lo <= r["pos"] <= hi):
            continue
        say("  %-6d %-7d %-8d %-4s %-6s %-24s %-16s %-16s %s"
            % (r["pos"], r["eid"], r["idx"], "TAG" if r["tagged"] else "",
               "BLEND" if r["blend"] else "",
               "%s/%s/%s" % (r["bsrc"], r["bdst"], r["bop"]),
               r["vs"], r["ps"], r["name"]))

    targets = [(r["eid"], "GLASS-CAND") for r in cands]
    targets += [(r["eid"], "TAGGED") for r in fp]
    targets.sort()

    wvp_bytes = {}
    verdict = []

    for eid, label in targets:
        say("")
        say("=" * 92)
        r = next(x for x in rows if x["eid"] == eid)
        say("== %s  pos %d  eid %d  idx %d  vs %s =="
            % (label, r["pos"], eid, r["idx"], r["vs"]))
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        try:
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        except Exception as e:
            say("  reflection failed: %s" % e)
            continue

        entries = read_blocks(controller, pipe, refl)
        say("  %d constant block(s) declared" % len(entries))
        b1 = None
        for e in entries:
            dump_block("    ", e)
            if e[2] == 1:
                b1 = e
        if b1 is None:
            say("  NO BLOCK AT b1 - the mod patches slot 1")
            continue

        toff, tname = truth_offset(b1)
        data = b1[7]
        cur, curwhy = rule_current(data)
        prop, propwhy = rule_proposed(data)
        say("  GROUND TRUTH (by name): %s at +0x%s"
            % (tname, ("%04X" % toff) if toff is not None else "??"))
        say("  current  rule -> 0x%02X   (%s)" % (cur, curwhy))
        say("  proposed rule -> 0x%02X   (%s)" % (prop, propwhy))
        if toff is not None and len(data) >= toff + 64:
            wvp_bytes[eid] = data[toff:toff + 64]
        verdict.append((label, eid, r["idx"], b1[1], b1[3], toff, tname,
                        cur, prop, b1[4]))

        if label == "GLASS-CAND":
            try:
                dis = controller.DisassembleShader(rd.ResourceId.Null(), refl,
                                                   "DXBC")
                say("  ---- DXBC (vertex shader %s) ----" % r["vs"])
                for ln in dis.splitlines():
                    say("      %s" % ln)
            except Exception as e:
                say("  disassembly failed: %s" % e)

        try:
            arith(controller, pipe, eid, b1)
        except Exception as e:
            say("  arithmetic failed: %s" % e)
            say(traceback.format_exc())

    say("")
    say("=" * 92)
    say("== byte-identical WVP comparison (each draw at ITS truth offset) ==")
    ks = sorted(wvp_bytes)
    any_eq = False
    for i in range(len(ks)):
        for j in range(i + 1, len(ks)):
            if wvp_bytes[ks[i]] == wvp_bytes[ks[j]]:
                say("  eid %d == eid %d  BIT IDENTICAL" % (ks[i], ks[j]))
                any_eq = True
    if not any_eq:
        say("  no two draws share a byte-identical clip matrix")

    say("")
    say("=" * 92)
    say("== VERDICT TABLE ==")
    say("  %-11s %-7s %-8s %-16s %-6s %-7s %-24s %-12s %-12s %s"
        % ("label", "eid", "idx", "block", "size", "truth", "truthName",
           "current", "proposed", "b1off"))
    bad_cur = bad_prop = 0
    for (label, eid, idx, bn, bs, toff, tname, cur, prop, boff) in verdict:
        okc = (toff is not None and cur == toff)
        okp = (toff is not None and prop == toff)
        if not okc:
            bad_cur += 1
        if not okp:
            bad_prop += 1
        say("  %-11s %-7d %-8d %-16s %-6d %-7s %-24s %-12s %-12s 0x%X"
            % (label, eid, idx, bn, bs,
               ("0x%02X" % toff) if toff is not None else "none", str(tname),
               "0x%02X%s" % (cur, "" if okc else " WRONG"),
               "0x%02X%s" % (prop, "" if okp else " WRONG"), boff))
    say("  current rule wrong on %d/%d, proposed rule wrong on %d/%d"
        % (bad_cur, len(verdict), bad_prop, len(verdict)))

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
