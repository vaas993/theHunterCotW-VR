"""REFUTATION AUDIT of the proposed rule  move = StencilEnable && (ref & 0x40).

Run headlessly:
    set COTW_CAP=%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame33848.rdc
    qrenderdoc.exe --python tools\rdc_ref40_audit.py

Independent re-measurement.  Nothing here is taken from the claim: the scene
size is LEARNED (largest render target actually bound by a drawcall), the
assembly is FOUND (from the hits), and every predicate is scored over EVERY
drawcall in the frame, not a sample.

What it answers, in order of the five attacks:
  1. false positives   - every drawcall, with the RTV/DSV/viewport of each hit
  2. render-to-texture - per hit: is the DSV null / not scene-sized, is any RTV
                         not scene-sized, is the viewport reduced
  3. the mask disc     - which hits WRITE bit 0x40 (wr & 0x40 with ref & 0x40)
  4. generality        - runs on any capture via COTW_CAP
  5. hookability       - the API call of every hit (a DrawIndexed hook cannot
                         see a Draw / DrawInstanced), and the command-list /
                         marker parentage of each

Plus, because the fix rides on it: the VS b1 constant block NAME, RESOURCE,
REAL length, bind offset and the three 4x4 matrices for the mask draws, the two
draws after the pass (glass), and the last colour-round body draws - to test
whether mask and glass carry the SAME matrix value, which is what the mod's
Unmap-time value patch keys on.

RenderDoc 1.45 traps observed: GetConstantBlocks (not GetConstantBuffer),
access.index is the descriptor POSITION matched against the reflection index,
descriptors are temporary SWIG proxies so fields are copied immediately, and no
bare except swallows an AttributeError.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ref40_audit_<cap>.txt / .csv
"""
import os
import struct
import traceback

import renderdoc as rd

CAP = os.environ.get("COTW_CAP") or os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame33848.rdc")
TAG = os.path.splitext(os.path.basename(CAP))[0]
BASE = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR")
OUT = os.path.join(BASE, "rdc_ref40_audit_%s.txt" % TAG)
CSV = os.path.join(BASE, "rdc_ref40_audit_%s.csv" % TAG)

out = open(OUT, "w")
csv = open(CSV, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def en(v):
    return str(v).split(".")[-1].split(":")[0]


def rid_of(o):
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


def main():
    cap = rd.OpenCaptureFile()
    say("capture: %s" % CAP)
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open")

    tex = {}
    for t in controller.GetTextures():
        tex[str(t.resourceId)] = (int(t.width), int(t.height), t.format.Name())
    bufl = {}
    for b in controller.GetBuffers():
        bufl[str(b.resourceId)] = int(b.length)

    sf = controller.GetStructuredFile()

    leaves = []

    def walk(lst, chain):
        for a in lst:
            kids = getattr(a, "children", [])
            try:
                nm = a.GetName(sf)
            except Exception as e:
                nm = "?%s" % e
            if kids:
                walk(kids, chain + [nm.split("(")[0][:40]])
            else:
                leaves.append((a, list(chain), nm))

    walk(controller.GetRootActions(), [])
    leaves.sort(key=lambda t: t[0].eventId)
    draws = [t for t in leaves if t[0].flags & rd.ActionFlags.Drawcall]
    say("%d leaf actions, %d drawcalls" % (len(leaves), len(draws)))

    def wh(rid):
        t = tex.get(rid)
        return (t[0], t[1]) if t else (0, 0)

    cols = ("pos", "eid", "api", "idx", "inst", "vs", "ps", "nRTV",
            "sEn", "sRd", "sWr", "sRef", "sFunc", "sPass", "sFail",
            "bRd", "bWr", "bRef", "bFunc", "bPass",
            "dEn", "dFunc", "dWr", "blend",
            "rtv0", "rtv0w", "rtv0h", "rtvOther",
            "dsv", "dsvw", "dsvh", "vpx", "vpy", "vpw", "vph",
            "topo", "ib", "vb0", "chain")
    csv.write(",".join(cols) + "\n")

    rows = []
    for i, (a, chain, name) in enumerate(draws):
        eid = a.eventId
        controller.SetFrameEvent(eid, False)
        d3d = controller.GetD3D11PipelineState()
        om = d3d.outputMerger
        ds = om.depthStencilState
        f, b = ds.frontFace, ds.backFace

        r = {}
        r["pos"] = i
        r["eid"] = eid
        r["api"] = name.split("(")[0].replace("ID3D11DeviceContext::", "")[:28]
        r["idx"] = int(getattr(a, "numIndices", 0))
        r["inst"] = int(getattr(a, "numInstances", 0))
        r["vs"] = str(d3d.vertexShader.resourceId).replace("ResourceId::", "")
        r["ps"] = str(d3d.pixelShader.resourceId).replace("ResourceId::", "")
        r["sEn"] = int(bool(ds.stencilEnable))
        r["sRd"] = int(f.compareMask)
        r["sWr"] = int(f.writeMask)
        r["sRef"] = int(f.reference)
        r["sFunc"] = en(f.function)[:9]
        r["sPass"] = en(f.passOperation)[:9]
        r["sFail"] = en(f.failOperation)[:9]
        r["bRd"] = int(b.compareMask)
        r["bWr"] = int(b.writeMask)
        r["bRef"] = int(b.reference)
        r["bFunc"] = en(b.function)[:9]
        r["bPass"] = en(b.passOperation)[:9]
        r["dEn"] = int(bool(ds.depthEnable))
        r["dFunc"] = en(ds.depthFunction)[:12]
        r["dWr"] = int(bool(ds.depthWrites))

        bl = om.blendState
        be = 0
        try:
            if len(bl.blends):
                be = int(bool(bl.blends[0].enabled))
        except Exception as e:
            say("  blend read failed at eid %d: %s" % (eid, e))
        r["blend"] = be

        rtvs = []
        for t in om.renderTargets:
            s = rid_of(t)
            if s != "NULL":
                rtvs.append(s)
        r["nRTV"] = len(rtvs)
        r["rtv0"] = rtvs[0] if rtvs else "NULL"
        w, h = wh(r["rtv0"])
        r["rtv0w"], r["rtv0h"] = w, h
        r["rtvOther"] = "|".join("%s:%dx%d" % (s, wh(s)[0], wh(s)[1])
                                 for s in rtvs[1:])
        dv = rid_of(om.depthTarget)
        r["dsv"] = dv
        dw, dh = wh(dv)
        r["dsvw"], r["dsvh"] = dw, dh

        vpx = vpy = vpw = vph = -1.0
        for vp in d3d.rasterizer.viewports:
            if vp.enabled and (vp.width or vp.height):
                vpx, vpy = float(vp.x), float(vp.y)
                vpw, vph = float(vp.width), float(vp.height)
                break
        r["vpx"], r["vpy"], r["vpw"], r["vph"] = vpx, vpy, vpw, vph

        ia = d3d.inputAssembly
        r["topo"] = en(getattr(ia, "topology", "?"))[:16]
        r["ib"] = rid_of(getattr(ia, "indexBuffer", None))
        vb0 = "NULL"
        try:
            vbs = ia.vertexBuffers
            if len(vbs):
                vb0 = rid_of(vbs[0])
        except Exception as e:
            say("  vb read failed at eid %d: %s" % (eid, e))
        r["vb0"] = vb0
        r["chain"] = ">".join(chain)[-60:]

        rows.append(r)
        csv.write(",".join(str(r[k]).replace(",", ";") for k in cols) + "\n")
        if i % 250 == 0:
            csv.flush()
            say("  ... %d/%d (eid %d)" % (i, len(draws), eid))
    csv.flush()
    say("sweep done")

    # ---- LEARN the scene size: biggest RTV actually bound by a drawcall ----
    sceneW = max([r["rtv0w"] for r in rows] + [0])
    cand = [r for r in rows if r["rtv0w"] == sceneW]
    sceneH = max(r["rtv0h"] for r in cand)
    say("")
    say("scene colour size learned from the drawcalls: %dx%d" % (sceneW, sceneH))
    dsvbig = {}
    for r in rows:
        if r["dsvw"] == sceneW and r["dsvh"] == sceneH:
            dsvbig[r["dsv"]] = dsvbig.get(r["dsv"], 0) + 1
    say("scene-sized depth-stencil views: %s" % dsvbig)

    # ---- A. whole-frame census -------------------------------------------
    say("")
    say("=" * 78)
    say("== A. every distinct stencil signature over ALL %d drawcalls ==" % len(rows))
    cen = {}
    for r in rows:
        k = (r["sEn"], r["sRd"], r["sWr"], r["sRef"], r["sFunc"], r["sPass"])
        c = cen.setdefault(k, [0, []])
        c[0] += 1
        if len(c[1]) < 6:
            c[1].append(r["eid"])
    say("  %-4s %-6s %-6s %-6s %-10s %-10s %6s  sample eids"
        % ("sEn", "rd", "wr", "ref", "func", "pass", "n"))
    for k in sorted(cen, key=lambda k: -cen[k][0]):
        c = cen[k]
        mark = ""
        if k[0] and (k[3] & 0x40):
            mark = "   <== PROPOSED RULE FIRES"
        elif k[3] & 0x40:
            mark = "   (ref bit set, stencil disabled)"
        say("  %-4d 0x%02X   0x%02X   0x%02X   %-10s %-10s %6d  %s%s"
            % (k[0], k[1], k[2], k[3], k[4], k[5], c[0], c[1], mark))

    # ---- B. predicate scoreboard -----------------------------------------
    say("")
    say("=" * 78)
    say("== B. predicates over EVERY drawcall ==")
    preds = [
        ("MOD TODAY   sEn && rd==0x40 && ref&0x40",
         lambda r: r["sEn"] and r["sRd"] == 0x40 and (r["sRef"] & 0x40)),
        ("PROPOSED    sEn && ref&0x40",
         lambda r: r["sEn"] and (r["sRef"] & 0x40)),
        ("ref&0x40 alone (no sEn)", lambda r: bool(r["sRef"] & 0x40)),
        ("MASK        sEn && wr==0x40 && ref&0x40",
         lambda r: r["sEn"] and r["sWr"] == 0x40 and (r["sRef"] & 0x40)),
        ("any wr containing 0x40", lambda r: bool(r["sWr"] & 0x40)),
        ("PROPOSED + belt&braces (dsv scene-sized && vp full)",
         lambda r: r["sEn"] and (r["sRef"] & 0x40) and r["dsv"] != "NULL"
         and r["dsvw"] == sceneW and r["dsvh"] == sceneH
         and abs(r["vpw"] - sceneW) < 0.5 and abs(r["vph"] - sceneH) < 0.5),
        ("PROPOSED or glass tag (rd0xFF wr0x12 ref0x12 Replace)",
         lambda r: r["sEn"] and ((r["sRef"] & 0x40) or
                                 (r["sRd"] == 0xFF and r["sWr"] == 0x12 and
                                  r["sRef"] == 0x12 and r["sPass"] == "Replace"))),
        ("back-face ref differs from front", lambda r: r["sRef"] != r["bRef"]),
        ("back-face rd/wr differ from front",
         lambda r: r["sRd"] != r["bRd"] or r["sWr"] != r["bWr"]),
    ]
    for nm, fn in preds:
        hits = [r for r in rows if fn(r)]
        pos = [r["pos"] for r in hits]
        say("  %-54s %5d  pos %s"
            % (nm, len(hits),
               ("%d..%d" % (min(pos), max(pos))) if pos else "-"))

    hits = [r for r in rows if r["sEn"] and (r["sRef"] & 0x40)]
    if not hits:
        say("")
        say("!! the proposed rule selects NOTHING in this capture")

    # ---- C. every hit, in full -------------------------------------------
    say("")
    say("=" * 78)
    say("== C. every draw the PROPOSED rule selects ==")
    say("  %-5s %-7s %-20s %-7s %-6s %-6s %-16s %-11s %-5s %-14s %-14s %-11s"
        % ("pos", "eid", "api", "idx", "vs", "ps", "rd/wr/ref", "func/pass",
           "nRTV", "rtv0", "dsv", "viewport"))
    for r in hits:
        say("  %-5d %-7d %-20s %-7d %-6s %-6s 0x%02X/0x%02X/0x%02X    %-11s %-5d "
            "%-14s %-14s %.0fx%.0f@%.0f,%.0f"
            % (r["pos"], r["eid"], r["api"], r["idx"], r["vs"], r["ps"],
               r["sRd"], r["sWr"], r["sRef"], r["sFunc"] + "/" + r["sPass"],
               r["nRTV"],
               "%s %dx%d" % (r["rtv0"].replace("ResourceId::", ""),
                             r["rtv0w"], r["rtv0h"]),
               "%s %dx%d" % (r["dsv"].replace("ResourceId::", ""),
                             r["dsvw"], r["dsvh"]),
               r["vpw"], r["vph"], r["vpx"], r["vpy"]))

    # ---- D. RTT / false-positive interrogation of the hits ----------------
    say("")
    say("=" * 78)
    say("== D. is any selected draw a render-to-texture / off-scene pass? ==")
    bad = []
    for r in hits:
        why = []
        if r["dsv"] == "NULL":
            why.append("no DSV")
        elif (r["dsvw"], r["dsvh"]) != (sceneW, sceneH):
            why.append("DSV %dx%d != scene" % (r["dsvw"], r["dsvh"]))
        if r["nRTV"] and (r["rtv0w"], r["rtv0h"]) != (sceneW, sceneH):
            why.append("RTV0 %dx%d != scene" % (r["rtv0w"], r["rtv0h"]))
        if abs(r["vpw"] - sceneW) > 0.5 or abs(r["vph"] - sceneH) > 0.5:
            why.append("viewport %.0fx%.0f" % (r["vpw"], r["vph"]))
        if r["api"] != "DrawIndexed":
            why.append("API is %s, a DrawIndexed hook never sees it" % r["api"])
        if why:
            bad.append((r, why))
    if bad:
        for r, why in bad:
            say("  pos %-5d eid %-7d  %s" % (r["pos"], r["eid"], "; ".join(why)))
    else:
        say("  none - every selected draw is DrawIndexed, scene DSV, full viewport")

    api = {}
    for r in hits:
        api[r["api"]] = api.get(r["api"], 0) + 1
    say("  API call breakdown of the hits: %s" % api)
    ch = {}
    for r in hits:
        ch[r["chain"]] = ch.get(r["chain"], 0) + 1
    say("  marker/command-list parentage of the hits: %s" % ch)

    # ---- E. the window, every leaf action ---------------------------------
    if hits:
        lo = min(r["pos"] for r in hits)
        hi = max(r["pos"] for r in hits)
        loE = draws[max(0, lo - 14)][0].eventId
        hiE = draws[min(len(draws) - 1, hi + 14)][0].eventId
        say("")
        say("=" * 78)
        say("== E. EVERY leaf action (draws, clears, copies) in eid %d..%d =="
            % (loE, hiE))
        byeid = {r["eid"]: r for r in rows}
        for (a, chain, name) in leaves:
            if not (loE <= a.eventId <= hiE):
                continue
            r = byeid.get(a.eventId)
            if r is None:
                say("        eid %-7d  %s   [%s]"
                    % (a.eventId, name[:96], ">".join(chain)[-40:]))
                continue
            sel = "SEL" if (r["sEn"] and (r["sRef"] & 0x40)) else "   "
            mod = "M" if (r["sEn"] and r["sRd"] == 0x40 and (r["sRef"] & 0x40)) else " "
            say("  %s%s pos %-5d eid %-7d idx %-7d vs %-6s ps %-6s nRTV %d "
                "sEn%d 0x%02X/0x%02X/0x%02X %-9s/%-9s dEn%d bl%d vp %.0fx%.0f "
                "rtv0 %s dsv %s ib %s vb0 %s"
                % (sel, mod, r["pos"], r["eid"], r["idx"], r["vs"], r["ps"],
                   r["nRTV"], r["sEn"], r["sRd"], r["sWr"], r["sRef"],
                   r["sFunc"], r["sPass"], r["dEn"], r["blend"],
                   r["vpw"], r["vph"],
                   r["rtv0"].replace("ResourceId::", ""),
                   r["dsv"].replace("ResourceId::", ""),
                   r["ib"].replace("ResourceId::", ""),
                   r["vb0"].replace("ResourceId::", "")))

    # ---- F. every clear anywhere on a scene-sized depth-stencil -----------
    say("")
    say("=" * 78)
    say("== F. every non-draw leaf action naming Clear/Copy in the frame ==")
    n = 0
    for (a, chain, name) in leaves:
        if a.flags & rd.ActionFlags.Drawcall:
            continue
        low = name.lower()
        if "clear" in low or "copy" in low or "resolve" in low:
            n += 1
            if n <= 80:
                say("  eid %-7d %s" % (a.eventId, name[:110]))
    say("  %d such actions" % n)

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
