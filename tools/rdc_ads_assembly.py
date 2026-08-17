"""COMPLETE draw list of the ADS scope assembly, and what distinguishes each piece.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_ads_assembly.py
    set COTW_CAP=...  to point at the other capture

Capture is taken with the scope RAISED and NO mod hooks - stock rendering.

Stage 1  full-frame sweep, one GetD3D11PipelineState per draw:
             stencil predicate, blend, colour write mask, depth state,
             RTV/DSV resources and their SIZES, viewport, VS/PS ids.
Stage 2  anchor on the stencil predicate (stencilEnable, compareMask == 0x40
             EXACTLY, ref & 0x40) -> first/last tagged draw.
Stage 3  deep dump of the window [firstTagged - PAD, lastTagged + PAD]:
             mesh identity (IB/VB0 resource+offset+stride), post-VS clip-space
             bounding box, constant blocks with REAL buffer lengths, PS SRVs
             (so a render-to-texture consumer is visible), full OM state.
Stage 4  candidate rules, scored inside the window and over the whole frame.

RenderDoc 1.45 notes that are already paid for:
  * PipeState.GetConstantBuffer does not exist; GetConstantBlocks returns
    UsedDescriptor, and UsedDescriptor.access.index is the descriptor POSITION,
    not the HLSL register.
  * Descriptors from .descriptor are temporary SWIG proxies - copy immediately.

Outputs (all under %LOCALAPPDATA%\theHunterCotWVR\):
    rdc_ads_assembly_<cap>.txt    the report
    rdc_ads_assembly_<cap>.csv    one row per drawcall, whole frame
"""
import math
import os
import struct
import traceback
import zlib

import renderdoc as rd

CAP = os.environ.get("COTW_CAP") or os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame13783.rdc")
TAG = os.path.splitext(os.path.basename(CAP))[0]
BASE = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR")
OUT = os.path.join(BASE, "rdc_ads_assembly_%s.txt" % TAG)
CSV = os.path.join(BASE, "rdc_ads_assembly_%s.csv" % TAG)

PAD = int(os.environ.get("COTW_PAD", "45"))   # draws either side of the pass
MAXV = 6000                                   # post-VS verts to read per draw

out = open(OUT, "w")
csv = open(CSV, "w")


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
            return int(r), int(c or 1)
    return 0, 0


def rid_of(o):
    """This build spells it .resourceId on some structs and .resource on the
    descriptor-based ones. Never guess - try both, and NEVER swallow silently."""
    if o is None:
        return "NULL"
    for attr in ("resourceId", "resource"):
        v = getattr(o, attr, None)
        if v is not None:
            s = str(v)
            return "NULL" if s.endswith("::0") else s
    d = getattr(o, "descriptor", None)
    if d is not None and d is not o:
        return rid_of(d)
    return "NULL"


def num_of(o, *names):
    for n in names:
        v = getattr(o, n, None)
        if v is not None:
            try:
                return int(v)
            except Exception:
                pass
    return 0


def main():
    cap = rd.OpenCaptureFile()
    say("capture: %s" % CAP)
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open")

    # ---------- resource tables -------------------------------------------
    tex = {}
    for t in controller.GetTextures():
        tex[str(t.resourceId)] = (int(t.width), int(t.height),
                                  t.format.Name(), int(t.arraysize),
                                  bool(t.creationFlags & rd.TextureCategory.SwapBuffer))
    buf = {}
    for b in controller.GetBuffers():
        buf[str(b.resourceId)] = int(b.length)
    resname = {}
    for r in controller.GetResources():
        resname[str(r.resourceId)] = r.name

    swap = [k for k, v in tex.items() if v[4]]
    say("backbuffer(s): %s" % [(k, tex[k][:3]) for k in swap])

    def tdesc(rid):
        s = str(rid)
        if s.endswith("::0") or s == "ResourceId::0":
            return "NULL"
        t = tex.get(s)
        nm = resname.get(s, "")
        if t is None:
            return "%s%s" % (s, (" '%s'" % nm) if nm else "")
        return "%s %dx%d %s%s%s" % (s, t[0], t[1], t[2],
                                    " arr%d" % t[3] if t[3] > 1 else "",
                                    (" '%s'" % nm) if nm else "")

    # ---------- actions ----------------------------------------------------
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
    by_eid = {a.eventId: a for a in leaves}
    say("%d leaf actions, %d drawcalls" % (len(leaves), len(draws)))

    crc_cache = {}

    def crc_of(rid):
        s = str(rid)
        if s in crc_cache:
            return crc_cache[s]
        c = 0
        try:
            eps = controller.GetShaderEntryPoints(rid)
            if eps:
                refl = controller.GetShader(rd.ResourceId.Null(), rid, eps[0])
                c = zlib.crc32(bytes(refl.rawBytes)) & 0xFFFFFFFF
        except Exception:
            c = 0
        crc_cache[s] = c
        return c

    # ---------- STAGE 1: full-frame sweep ---------------------------------
    say("")
    say("=" * 78)
    say("== STAGE 1: full-frame state sweep ==")
    cols = ("pos", "eid", "idx", "inst", "vs", "ps", "psNull", "tagged",
            "sEn", "sFunc", "sRd", "sWr", "sRef", "sPass", "sFail", "sZFail",
            "dEn", "dFunc", "dWr", "blend", "cWrite", "nRTV", "rtv0",
            "rtv0w", "rtv0h", "rtv0fmt", "dsv", "dsvw", "dsvh", "dRO", "sRO",
            "vpX", "vpY", "vpW", "vpH", "vpZ0", "vpZ1", "cull", "name")
    csv.write(",".join(cols) + "\n")

    rows = []
    for i, a in enumerate(draws):
        eid = a.eventId
        controller.SetFrameEvent(eid, False)
        d3d = controller.GetD3D11PipelineState()
        om = d3d.outputMerger
        ds = om.depthStencilState
        f = ds.frontFace

        r = {}
        r["pos"] = i
        r["eid"] = eid
        r["idx"] = int(getattr(a, "numIndices", 0))
        r["inst"] = int(getattr(a, "numInstances", 0))
        vs = str(d3d.vertexShader.resourceId)
        ps = str(d3d.pixelShader.resourceId)
        r["vs"] = vs.replace("ResourceId::", "")
        r["ps"] = ps.replace("ResourceId::", "")
        r["psNull"] = 1 if (ps == "ResourceId::0" or ps.endswith("::0")) else 0
        r["sEn"] = int(bool(ds.stencilEnable))
        r["sFunc"] = en(f.function)
        r["sRd"] = int(f.compareMask)
        r["sWr"] = int(f.writeMask)
        r["sRef"] = int(f.reference)
        r["sPass"] = en(f.passOperation)
        r["sFail"] = en(f.failOperation)
        r["sZFail"] = en(f.depthFailOperation)
        r["tagged"] = 1 if (r["sEn"] and r["sRd"] == 0x40
                            and (r["sRef"] & 0x40)) else 0
        r["dEn"] = int(bool(ds.depthEnable))
        r["dFunc"] = en(ds.depthFunction)
        r["dWr"] = int(bool(ds.depthWrites))
        r["dRO"] = int(bool(om.depthReadOnly))
        r["sRO"] = int(bool(om.stencilReadOnly))

        bl = 0
        cw = -1
        try:
            bs = om.blendState.blends
            if len(bs):
                bl = int(bool(bs[0].enabled))
                cw = int(bs[0].writeMask)
        except Exception:
            pass
        r["blend"] = bl
        r["cWrite"] = cw

        rtvs = []
        rtv_err = ""
        try:
            for ud in d3d.outputMerger.renderTargets:
                rid = rid_of(ud)
                if rid != "NULL":
                    rtvs.append(rid)
        except Exception as e:
            rtv_err = "%s: %s" % (type(e).__name__, e)
        if not rtvs:
            # fall back to the generic pipeline-state descriptor API
            try:
                p = controller.GetPipelineState()
                for ud in p.GetOutputTargets():
                    rid = rid_of(getattr(ud, "descriptor", ud))
                    if rid != "NULL":
                        rtvs.append(rid)
            except Exception as e:
                rtv_err += " | fallback %s: %s" % (type(e).__name__, e)
        if rtv_err and i < 3:
            say("  NOTE rtv lookup: %s" % rtv_err)
        r["nRTV"] = len(rtvs)
        r0 = rtvs[0] if rtvs else "NULL"
        r["rtv0"] = r0.replace("ResourceId::", "")
        t0 = tex.get(r0)
        r["rtv0w"] = t0[0] if t0 else 0
        r["rtv0h"] = t0[1] if t0 else 0
        r["rtv0fmt"] = t0[2] if t0 else "-"
        r["_rtvs"] = rtvs

        dv = "NULL"
        try:
            dv = rid_of(om.depthTarget)
        except Exception as e:
            if i < 3:
                say("  NOTE dsv lookup: %s: %s" % (type(e).__name__, e))
        if dv == "NULL":
            try:
                p = controller.GetPipelineState()
                dv = rid_of(getattr(p.GetDepthTarget(), "descriptor",
                                    p.GetDepthTarget()))
            except Exception:
                dv = "NULL"
        r["dsv"] = dv.replace("ResourceId::", "")
        td = tex.get(dv)
        r["dsvw"] = td[0] if td else 0
        r["dsvh"] = td[1] if td else 0

        vp = None
        for v in d3d.rasterizer.viewports:
            if v.enabled and (v.width or v.height):
                vp = (float(v.x), float(v.y), float(v.width), float(v.height),
                      float(v.minDepth), float(v.maxDepth))
                break
        if vp is None:
            vp = (-1., -1., -1., -1., -1., -1.)
        (r["vpX"], r["vpY"], r["vpW"], r["vpH"], r["vpZ0"], r["vpZ1"]) = vp
        r["cull"] = en(d3d.rasterizer.state.cullMode)
        r["name"] = a.GetName(sf).split("(")[0][:28].replace(",", ";")

        rows.append(r)
        csv.write(",".join(str(r[k]) for k in cols) + "\n")
        if i % 250 == 0:
            csv.flush()
            say("  ... %d/%d (eid %d)" % (i, len(draws), eid))
    csv.flush()
    say("  sweep done, %d rows" % len(rows))

    # ---------- STAGE 2: anchor on the stencil predicate -------------------
    tagged = [r for r in rows if r["tagged"]]
    say("")
    say("=" * 78)
    say("== STAGE 2: the first-person pass ==")
    say("tagged draws (stencilEnable, readMask==0x40 EXACT, ref&0x40): %d"
        % len(tagged))
    if not tagged:
        say("NO TAGGED DRAWS - abort")
        controller.Shutdown()
        cap.Shutdown()
        return
    tp = [r["pos"] for r in tagged]
    say("first tagged: pos %d eid %d   last tagged: pos %d eid %d"
        % (tp[0], tagged[0]["eid"], tp[-1], tagged[-1]["eid"]))
    say("tagged eids: %s" % [r["eid"] for r in tagged])
    # contiguity of the tagged run
    gaps = []
    for k in range(1, len(tp)):
        if tp[k] - tp[k - 1] > 1:
            gaps.append((tp[k - 1], tp[k], tp[k] - tp[k - 1] - 1))
    say("gaps inside the tagged run (untagged draws in between): %d" % len(gaps))
    for g in gaps:
        say("   after pos %d -> next tagged pos %d : %d untagged draws between"
            % g)

    lo = max(0, tp[0] - PAD)
    hi = min(len(rows) - 1, tp[-1] + PAD)
    say("")
    say("WINDOW pos %d..%d  = eid %d..%d  (%d draws)"
        % (lo, hi, rows[lo]["eid"], rows[hi]["eid"], hi - lo + 1))

    win = rows[lo:hi + 1]

    # ---------- STAGE 3: deep dump of the window --------------------------
    say("")
    say("=" * 78)
    say("== STAGE 3: every draw in the window, in full ==")

    detail = {}
    for r in win:
        eid = r["eid"]
        a = by_eid.get(eid)
        controller.SetFrameEvent(eid, True)
        d3d = controller.GetD3D11PipelineState()
        pipe = controller.GetPipelineState()
        d = {}

        # --- mesh identity ---
        try:
            ib = d3d.inputAssembly.indexBuffer
            d["ib"] = rid_of(ib).replace("ResourceId::", "")
            d["ibOff"] = num_of(ib, "byteOffset", "offset")
        except Exception as e:
            d["ib"], d["ibOff"] = "ERR:%s" % type(e).__name__, 0
        vb = []
        try:
            for k, v in enumerate(d3d.inputAssembly.vertexBuffers):
                rid = rid_of(v)
                if rid == "NULL":
                    continue
                vb.append((k, rid.replace("ResourceId::", ""),
                           num_of(v, "byteOffset", "offset"),
                           num_of(v, "byteStride", "stride"),
                           buf.get(rid, -1)))
        except Exception as e:
            vb = [("ERR", str(e)[:40], 0, 0, -1)]
        d["vb"] = vb
        try:
            d["topo"] = en(d3d.inputAssembly.topology)
        except Exception:
            d["topo"] = "?"
        d["baseVertex"] = int(getattr(a, "baseVertex", 0)) if a else 0
        d["indexOffset"] = int(getattr(a, "indexOffset", 0)) if a else 0

        # --- shader crcs ---
        d["vsCrc"] = crc_of(d3d.vertexShader.resourceId)
        d["psCrc"] = crc_of(d3d.pixelShader.resourceId)

        # --- pixel-shader SRVs: does it SAMPLE a render target? ---
        srvs = []
        try:
            for ud in pipe.GetReadOnlyResources(rd.ShaderStage.Pixel):
                dd = getattr(ud, "descriptor", None)
                if dd is None:
                    continue
                rs = getattr(dd, "resource", None)
                if rs is None or str(rs).endswith("::0"):
                    continue
                s = str(rs)
                t = tex.get(s)
                srvs.append((s.replace("ResourceId::", ""),
                             t[0] if t else 0, t[1] if t else 0,
                             t[2] if t else "-",
                             resname.get(s, "")))
        except Exception as e:
            srvs = [("ERR", 0, 0, str(e)[:40], "")]
        d["srv"] = srvs

        # --- post-VS clip-space bbox ---
        bbox = None
        nv = 0
        try:
            vo = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            if vo and vo.vertexResourceId != rd.ResourceId.Null():
                st = int(vo.vertexByteStride)
                cnt = int(getattr(vo, "numIndices", 0)) or 0
                cnt = min(cnt or MAXV, MAXV)
                raw = bytes(controller.GetBufferData(
                    vo.vertexResourceId, int(vo.vertexByteOffset), st * cnt))
                xs, ys, zs, ws = [], [], [], []
                for k in range(cnt):
                    if (k + 1) * st > len(raw):
                        break
                    x, y, z, w = struct.unpack_from("<4f", raw, k * st)
                    if not all(map(math.isfinite, (x, y, z, w))) or abs(w) < 1e-9:
                        continue
                    xs.append(x / w)
                    ys.append(y / w)
                    zs.append(z / w)
                    ws.append(w)
                    nv += 1
                if xs:
                    bbox = (min(xs), max(xs), min(ys), max(ys),
                            min(zs), max(zs), min(ws), max(ws))
        except Exception as e:
            d["bboxErr"] = str(e)[:60]
        d["bbox"] = bbox
        d["nv"] = nv

        # --- constant blocks (VS), with real buffer length ---
        cbs = []
        try:
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
            decl = [(int(cb.fixedBindNumber), cb.name, int(cb.byteSize))
                    for cb in refl.constantBlocks]
            used = []
            for ud in pipe.GetConstantBlocks(rd.ShaderStage.Vertex):
                acc = getattr(ud, "access", None)
                dd = getattr(ud, "descriptor", None)
                if dd is None:
                    continue
                used.append((int(getattr(acc, "index", 1 << 30)),
                             str(dd.resource), int(getattr(dd, "byteOffset", 0)),
                             int(getattr(dd, "byteSize", 0))))
            used.sort(key=lambda t: t[0])
            for k, (reg, nm, sz) in enumerate(decl):
                u = next((x for x in used if x[0] == k), None)
                if u is None:
                    cbs.append((reg, nm, sz, "-", 0, 0, -1))
                else:
                    cbs.append((reg, nm, sz, u[1].replace("ResourceId::", ""),
                                u[2], u[3], buf.get(u[1], -1)))
        except Exception as e:
            cbs = [(-1, "REFL ERR %s" % str(e)[:40], 0, "-", 0, 0, -1)]
        d["cb"] = cbs
        detail[eid] = d

    # ---- print the window table -------------------------------------------
    say("")
    say("LEGEND  T=stencil-tagged (rd==0x40 & ref&0x40)  B=blend  P=null PS")
    say("        cw=colour write mask (0 = writes no colour)")
    say("")
    hdr = ("%-5s %-7s %-8s %-4s %-3s %-9s %-9s %-3s %-4s %-16s %-3s %-12s "
           "%-14s %s")
    say(hdr % ("pos", "eid", "indices", "inst", "TBP", "vs", "ps", "cw",
               "dEn/W", "sten rd/wr/ref", "nRT", "rtv0", "dsv", "viewport"))
    say("-" * 150)
    for r in win:
        d = detail[r["eid"]]
        flags = "%s%s%s" % ("T" if r["tagged"] else ".",
                            "B" if r["blend"] else ".",
                            "P" if r["psNull"] else ".")
        say(hdr % (r["pos"], r["eid"], r["idx"], r["inst"], flags,
                   "%08X" % d["vsCrc"], "%08X" % d["psCrc"],
                   "%d" % r["cWrite"] if r["cWrite"] >= 0 else "-",
                   "%d/%d" % (r["dEn"], r["dWr"]),
                   "%d %s 0x%02X/0x%02X/0x%02X" % (
                       r["sEn"], r["sFunc"][:4], r["sRd"], r["sWr"], r["sRef"]),
                   r["nRTV"],
                   "%s %dx%d" % (r["rtv0"], r["rtv0w"], r["rtv0h"]),
                   "%s %dx%d" % (r["dsv"], r["dsvw"], r["dsvh"]),
                   "%.0fx%.0f@%.0f,%.0f z%.2f-%.2f" % (
                       r["vpW"], r["vpH"], r["vpX"], r["vpY"],
                       r["vpZ0"], r["vpZ1"])))

    # ---- per-draw expanded record ----------------------------------------
    say("")
    say("=" * 78)
    say("== per-draw expanded records ==")
    for r in win:
        eid = r["eid"]
        d = detail[eid]
        say("")
        say("-" * 74)
        say("pos %-5d eid %-7d  %s" % (r["pos"], eid, r["name"]))
        say("   indices=%d instances=%d topo=%s baseVertex=%d indexOffset=%d"
            % (r["idx"], r["inst"], d["topo"], d["baseVertex"],
               d["indexOffset"]))
        say("   VS %s crc %08X    PS %s crc %08X%s"
            % (r["vs"], d["vsCrc"], r["ps"], d["psCrc"],
               "   <<< NULL PIXEL SHADER" if r["psNull"] else ""))
        say("   IB %s +%d    VB %s" % (d["ib"], d["ibOff"], d["vb"]))
        say("   RTV(%d) %s" % (r["nRTV"],
                               ", ".join(tdesc("ResourceId::" + x)
                                         for x in [y.replace("ResourceId::", "")
                                                   for y in r["_rtvs"]])
                               or "(none bound)"))
        say("   DSV %s  depthReadOnly=%d stencilReadOnly=%d"
            % (tdesc("ResourceId::" + r["dsv"]) if r["dsv"] != "NULL" else "NULL",
               r["dRO"], r["sRO"]))
        say("   blend=%d colourWriteMask=%s   depth en=%d func=%s writes=%d"
            % (r["blend"], r["cWrite"], r["dEn"], r["dFunc"], r["dWr"]))
        say("   stencil en=%d func=%s rd=0x%02X wr=0x%02X ref=0x%02X "
            "pass=%s fail=%s zfail=%s%s"
            % (r["sEn"], r["sFunc"], r["sRd"], r["sWr"], r["sRef"],
               r["sPass"], r["sFail"], r["sZFail"],
               "   <<< TAGGED" if r["tagged"] else ""))
        say("   viewport %.1fx%.1f @%.1f,%.1f  z %.3f..%.3f  cull=%s"
            % (r["vpW"], r["vpH"], r["vpX"], r["vpY"], r["vpZ0"], r["vpZ1"],
               r["cull"]))
        if d["bbox"]:
            b = d["bbox"]
            say("   postVS NDC bbox  x %+.4f..%+.4f  y %+.4f..%+.4f  "
                "z %+.4f..%+.4f  w %.3f..%.3f   (%d verts)"
                % (b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], d["nv"]))
            cx = (b[0] + b[1]) / 2.0
            cy = (b[2] + b[3]) / 2.0
            sx = (b[1] - b[0]) / 2.0
            sy = (b[3] - b[2]) / 2.0
            say("                    centre (%+.4f, %+.4f)  half-extent "
                "(%.4f, %.4f)  aspect %.3f"
                % (cx, cy, sx, sy, (sx / sy) if sy else 0.0))
            if r["vpW"] > 0:
                px = r["vpX"] + (cx * 0.5 + 0.5) * r["vpW"]
                py = r["vpY"] + (0.5 - cy * 0.5) * r["vpH"]
                say("                    screen centre approx (%.0f, %.0f) px"
                    % (px, py))
        else:
            say("   postVS bbox: none (%s)" % d.get("bboxErr", "no VSOut"))
        say("   VS constant blocks:")
        for (reg, nm, sz, rid, off, bsz, real) in d["cb"]:
            say("      b%-2s %-24s declared %6d B  buffer %-8s +%-7d "
                "bind %6d B  REAL %s B%s"
                % (reg, nm[:24], sz, rid, off, bsz, real,
                   "   [<256 B: mod's cbscan skips it]"
                   if 0 <= real < 256 else ""))
        if d["srv"]:
            say("   PS SRVs:")
            for (s, w, h, fm, nm) in d["srv"][:14]:
                mark = ""
                if w and h and (w < 1024 or h < 1024):
                    mark = "   <<< small texture"
                say("      t %-8s %5dx%-5d %-24s %s%s" % (s, w, h, fm, nm, mark))

    # ---------- STAGE 4: rule candidates ----------------------------------
    say("")
    say("=" * 78)
    say("== STAGE 4: what is unique to the window ==")

    def tally(label, keyfn, rowsin, rowsout):
        say("")
        say("-- %s --" % label)
        ins, outs, samp = {}, {}, {}
        for r in rowsin:
            k = keyfn(r)
            ins[k] = ins.get(k, 0) + 1
            samp.setdefault(k, []).append(r["eid"])
        for r in rowsout:
            k = keyfn(r)
            outs[k] = outs.get(k, 0) + 1
        allk = sorted(set(ins) | set(outs),
                      key=lambda k: (-ins.get(k, 0), -outs.get(k, 0), str(k)))
        say("   %-52s %6s %6s  %s" % ("value", "win", "rest", "sample eids"))
        for k in allk[:40]:
            i, o = ins.get(k, 0), outs.get(k, 0)
            mark = "  <<< WINDOW-ONLY" if i and not o else ""
            say("   %-52s %6d %6d  %s%s"
                % (str(k)[:52], i, o, samp.get(k, [])[:6], mark))

    winset = set(id(r) for r in win)
    rest = [r for r in rows if id(r) not in winset]
    tally("RTV0 resource + size", lambda r: "%s %dx%d %s"
          % (r["rtv0"], r["rtv0w"], r["rtv0h"], r["rtv0fmt"]), win, rest)
    tally("DSV resource + size",
          lambda r: "%s %dx%d" % (r["dsv"], r["dsvw"], r["dsvh"]), win, rest)
    tally("viewport rect", lambda r: "%.0fx%.0f@%.0f,%.0f"
          % (r["vpW"], r["vpH"], r["vpX"], r["vpY"]), win, rest)
    tally("stencil tuple (rd/wr/ref/func/pass)",
          lambda r: "en%d %s 0x%02X/0x%02X/0x%02X %s"
          % (r["sEn"], r["sFunc"], r["sRd"], r["sWr"], r["sRef"], r["sPass"]),
          win, rest)
    tally("depth tuple", lambda r: "en%d %s w%d dRO%d sRO%d"
          % (r["dEn"], r["dFunc"], r["dWr"], r["dRO"], r["sRO"]), win, rest)
    tally("blend / colour-write", lambda r: "blend%d cw%d"
          % (r["blend"], r["cWrite"]), win, rest)
    tally("nRTV + null PS", lambda r: "nRTV%d psNull%d"
          % (r["nRTV"], r["psNull"]), win, rest)
    tally("vertex shader", lambda r: r["vs"], win, rest)
    tally("pixel shader", lambda r: r["ps"], win, rest)

    say("")
    say("== candidate DrawIndexed-hook rules, scored ==")

    # what the window's DSVs / RTVs are, for the rule set
    win_dsv = {}
    for r in win:
        win_dsv[r["dsv"]] = win_dsv.get(r["dsv"], 0) + 1
    say("   window DSVs: %s" % sorted(win_dsv.items(), key=lambda kv: -kv[1]))

    tagged_dsv = set(r["dsv"] for r in rows if r["tagged"])
    tagged_rtv = set()
    for r in rows:
        if r["tagged"]:
            for x in r["_rtvs"]:
                tagged_rtv.add(x.replace("ResourceId::", ""))
    say("   DSV(s) used by tagged draws: %s" % sorted(tagged_dsv))
    say("   RTV(s) used by tagged draws: %s" % sorted(tagged_rtv))

    full_w = max(r["vpW"] for r in rows)
    full_h = max(r["vpH"] for r in rows)
    say("   largest viewport in the frame: %.0fx%.0f" % (full_w, full_h))

    cands = [
        ("tagged (today's rule)", lambda r: bool(r["tagged"])),
        ("DSV in tagged-DSV set",
         lambda r: r["dsv"] in tagged_dsv and r["dsv"] != "NULL"),
        ("DSV in tagged-DSV set AND viewport is full size",
         lambda r: r["dsv"] in tagged_dsv and r["dsv"] != "NULL"
         and r["vpW"] >= full_w - 1 and r["vpH"] >= full_h - 1),
        ("DSV in tagged-DSV set AND (nRTV==0 OR rtv0 in tagged RTVs)",
         lambda r: r["dsv"] in tagged_dsv and r["dsv"] != "NULL"
         and (r["nRTV"] == 0 or r["rtv0"] in tagged_rtv)),
        ("DSV in tagged-DSV set AND rtv is full-size or none",
         lambda r: r["dsv"] in tagged_dsv and r["dsv"] != "NULL"
         and (r["nRTV"] == 0 or (r["rtv0w"] >= full_w - 1))),
        ("stencilEnable AND readMask==0x40 (ignore ref)",
         lambda r: r["sEn"] and r["sRd"] == 0x40),
        ("stencilEnable AND ref&0x40 (ignore mask)",
         lambda r: r["sEn"] and (r["sRef"] & 0x40)),
        ("stencilEnable AND writeMask&0x40",
         lambda r: r["sEn"] and (r["sWr"] & 0x40)),
        ("stencilEnable AND (rd&0x40 or wr&0x40 or ref&0x40)",
         lambda r: r["sEn"] and ((r["sRd"] & 0x40) or (r["sWr"] & 0x40)
                                 or (r["sRef"] & 0x40))),
        ("colour write mask == 0", lambda r: r["cWrite"] == 0),
        ("nRTV == 0", lambda r: r["nRTV"] == 0),
        ("null PS", lambda r: bool(r["psNull"])),
    ]
    say("   %-58s %6s %6s %6s" % ("rule", "win", "rest", "recall%"))
    for nm, fn in cands:
        hi = sum(1 for r in win if fn(r))
        ho = sum(1 for r in rest if fn(r))
        say("   %-58s %6d %6d %6.1f"
            % (nm, hi, ho, 100.0 * hi / max(1, len(win))))

    # ---- save the backbuffer so the lens can be located ------------------
    say("")
    if swap:
        try:
            controller.SetFrameEvent(draws[-1].eventId, True)
            ts = rd.TextureSave()
            ts.resourceId = rd.ResourceId()
            for t in controller.GetTextures():
                if t.creationFlags & rd.TextureCategory.SwapBuffer:
                    ts.resourceId = t.resourceId
                    break
            ts.mip = 0
            ts.slice.sliceIndex = 0
            ts.destType = rd.FileType.PNG
            png = os.path.join(BASE, "rdc_ads_backbuffer_%s.png" % TAG)
            ok = controller.SaveTexture(ts, png)
            say("backbuffer PNG -> %s (%s)" % (png, ok))
        except Exception as e:
            say("SaveTexture failed: %s" % e)

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
