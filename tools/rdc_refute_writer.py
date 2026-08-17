"""REFUTATION sweep for the proposed "symmetrise the stencil predicate" rule.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_refute_writer.py
    set COTW_CAP=...\cotw_frameNNNNN.rdc   to point at any capture

The CLAIM under test:
    readsFirstPersonBit  = sEn && compareMask == 0x40 && (ref & 0x40)
    writesFirstPersonBit = sEn && writeMask   == 0x40 && (ref & 0x40)
                                && frontFace.passOp == REPLACE
    firstPerson = reads || writes
    guard sceneSized = vp == sceneWH && dsv != NULL && (rtv == NULL || rtv == sceneRTV)
    sceneW/sceneH/sceneRTV learned from the FIRST reads-draw with a colour target.

Things this script measures that the claim did not:
  * WHICH COMMAND LIST / parent marker each draw sits under.  The mod's own
    cbscan.cpp says the glass "is drawn on a DIFFERENT DEFERRED CONTEXT from the
    body ... their thread never sees a tagged draw".  If the mask rides that
    context the learn-then-guard ordering is unsatisfiable.
  * The ORDER of the learn event vs the mask draws (is the guard even armed?).
  * RTV0 identity for every multi-RTV draw (the guard only inspects RTV0).
  * Back-face stencil ops as well as front (GetDesc returns both).
  * Every predicate variant scored over EVERY drawcall, on ANY capture -
    including the non-ADS ones, where a false positive is scenery that slides.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_refute_writer_<cap>.txt / .csv
"""
import os
import traceback

import renderdoc as rd

CAP = os.environ.get("COTW_CAP") or os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame13783.rdc")
TAG = os.path.splitext(os.path.basename(CAP))[0]
BASE = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR")
OUT = os.path.join(BASE, "rdc_refute_writer_%s.txt" % TAG)
CSV = os.path.join(BASE, "rdc_refute_writer_%s.csv" % TAG)

out = open(OUT, "w")
csv = open(CSV, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def en(v):
    return str(v).split(".")[-1].split(":")[0]


def rid_of(o):
    """.resourceId on some structs, .resource on descriptor-based ones.
    Try both, never swallow silently."""
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
    buf = {}
    for b in controller.GetBuffers():
        buf[str(b.resourceId)] = int(b.length)

    sf = controller.GetStructuredFile()

    # ---- flatten, but KEEP the parent chain -------------------------------
    leaves = []

    def walk(lst, chain):
        for a in lst:
            kids = getattr(a, "children", [])
            nm = ""
            try:
                nm = a.GetName(sf)
            except Exception:
                nm = "?"
            if kids:
                walk(kids, chain + [nm.split("(")[0][:34]])
            else:
                leaves.append((a, list(chain), nm))

    walk(controller.GetRootActions(), [])
    draws = [(a, c, n) for (a, c, n) in leaves
             if a.flags & rd.ActionFlags.Drawcall]
    draws.sort(key=lambda t: t[0].eventId)
    say("%d leaf actions, %d drawcalls" % (len(leaves), len(draws)))

    cols = ("pos", "eid", "kind", "idx", "inst", "vs", "ps", "psNull",
            "sEn", "sFunc", "sRd", "sWr", "sRef", "sPassF", "sPassB",
            "sRdB", "sWrB", "sRefB",
            "dEn", "dWr", "nRTV", "rtv0", "rtv0wh", "rtvAll",
            "dsv", "dsvwh", "vpWH", "vpXY",
            "ib", "ibOff", "vb0", "vb0Off", "stride",
            "READS", "WRITES", "chain")
    csv.write(",".join(cols) + "\n")

    rows = []
    for i, (a, chain, name) in enumerate(draws):
        eid = a.eventId
        controller.SetFrameEvent(eid, False)
        d3d = controller.GetD3D11PipelineState()
        om = d3d.outputMerger
        ds = om.depthStencilState
        f = ds.frontFace
        b = ds.backFace

        r = {}
        r["pos"] = i
        r["eid"] = eid
        fl = a.flags
        kind = "DrawIndexed" if (fl & rd.ActionFlags.Indexed) else "Draw"
        if fl & rd.ActionFlags.Instanced:
            kind += "Inst"
        if fl & rd.ActionFlags.Indirect:
            kind += "Indirect"
        r["kind"] = kind
        r["idx"] = int(getattr(a, "numIndices", 0))
        r["inst"] = int(getattr(a, "numInstances", 0))
        r["vs"] = str(d3d.vertexShader.resourceId).replace("ResourceId::", "")
        psid = str(d3d.pixelShader.resourceId)
        r["ps"] = psid.replace("ResourceId::", "")
        r["psNull"] = 1 if psid.endswith("::0") else 0

        r["sEn"] = int(bool(ds.stencilEnable))
        r["sFunc"] = en(f.function)
        r["sRd"] = int(f.compareMask)
        r["sWr"] = int(f.writeMask)
        r["sRef"] = int(f.reference)
        r["sPassF"] = en(f.passOperation)
        r["sPassB"] = en(b.passOperation)
        r["sRdB"] = int(b.compareMask)
        r["sWrB"] = int(b.writeMask)
        r["sRefB"] = int(b.reference)
        r["dEn"] = int(bool(ds.depthEnable))
        r["dWr"] = int(bool(ds.depthWrites))

        rtvs = []
        for ud in om.renderTargets:
            rr = rid_of(ud)
            if rr != "NULL":
                rtvs.append(rr.replace("ResourceId::", ""))
        r["nRTV"] = len(rtvs)
        r["rtv0"] = rtvs[0] if rtvs else "NULL"
        t0 = tex.get("ResourceId::" + r["rtv0"]) if rtvs else None
        r["rtv0wh"] = ("%dx%d" % (t0[0], t0[1])) if t0 else "-"
        r["rtvAll"] = "|".join(rtvs) if rtvs else "-"

        dv = rid_of(om.depthTarget)
        r["dsv"] = dv.replace("ResourceId::", "")
        td = tex.get(dv)
        r["dsvwh"] = ("%dx%d" % (td[0], td[1])) if td else "-"

        vp = None
        for v in d3d.rasterizer.viewports:
            if v.enabled and (v.width or v.height):
                vp = (float(v.x), float(v.y), float(v.width), float(v.height))
                break
        if vp is None:
            vp = (-1., -1., -1., -1.)
        r["vpWH"] = "%.0fx%.0f" % (vp[2], vp[3])
        r["vpXY"] = "%.0f;%.0f" % (vp[0], vp[1])
        r["_vpw"], r["_vph"] = vp[2], vp[3]

        ia = d3d.inputAssembly
        r["ib"] = rid_of(ia.indexBuffer).replace("ResourceId::", "")
        r["ibOff"] = int(getattr(ia.indexBuffer, "byteOffset", 0))
        vbs = list(ia.vertexBuffers)
        if vbs:
            r["vb0"] = rid_of(vbs[0]).replace("ResourceId::", "")
            r["vb0Off"] = int(getattr(vbs[0], "byteOffset", 0))
            r["stride"] = int(getattr(vbs[0], "byteStride", 0))
        else:
            r["vb0"], r["vb0Off"], r["stride"] = "NULL", 0, 0

        r["READS"] = 1 if (r["sEn"] and r["sRd"] == 0x40
                           and (r["sRef"] & 0x40)) else 0
        r["WRITES"] = 1 if (r["sEn"] and r["sWr"] == 0x40
                            and (r["sRef"] & 0x40)
                            and r["sPassF"] == "Replace") else 0
        r["chain"] = " > ".join(chain)[:80].replace(",", ";")
        r["name"] = name.split("(")[0][:30]

        rows.append(r)
        csv.write(",".join(str(r[k]) for k in cols) + "\n")
        if i % 250 == 0:
            csv.flush()
            say("  ... %d/%d (eid %d)" % (i, len(draws), eid))
    csv.flush()
    say("sweep done: %d drawcalls" % len(rows))

    # ------------------------------------------------------------------
    say("")
    say("=" * 78)
    say("== A. predicate counts over EVERY drawcall ==")
    reads = [r for r in rows if r["READS"]]
    writes = [r for r in rows if r["WRITES"]]
    say("READS  (sEn & rd==0x40 & ref&0x40)                    : %d" % len(reads))
    say("WRITES (sEn & wr==0x40 & ref&0x40 & frontPass=Replace): %d" % len(writes))
    if reads:
        say("READS  span pos %d..%d  eid %d..%d"
            % (reads[0]["pos"], reads[-1]["pos"], reads[0]["eid"],
               reads[-1]["eid"]))
    for r in writes:
        say("  WRITES hit: pos %d eid %d %s idx=%d vs=%s ps=%s%s nRTV=%d "
            "rtv=%s dsv=%s %s vp=%s  sten sE%d rd0x%02X wr0x%02X ref0x%02X "
            "%s/%s back(rd0x%02X wr0x%02X ref0x%02X %s) dE%d dW%d | %s"
            % (r["pos"], r["eid"], r["kind"], r["idx"], r["vs"], r["ps"],
               " NULLPS" if r["psNull"] else "", r["nRTV"], r["rtvAll"],
               r["dsv"], r["dsvwh"], r["vpWH"], r["sEn"], r["sRd"], r["sWr"],
               r["sRef"], r["sFunc"], r["sPassF"], r["sRdB"], r["sWrB"],
               r["sRefB"], r["sPassB"], r["dEn"], r["dWr"], r["chain"]))

    # looser variants, for FP hunting
    say("")
    say("== A2. looser writer variants (false-positive hunt) ==")
    variants = [
        ("sEn & (wr & 0x40)", lambda r: r["sEn"] and (r["sWr"] & 0x40)),
        ("sEn & (wr & 0x40) & passF==Replace",
         lambda r: r["sEn"] and (r["sWr"] & 0x40) and r["sPassF"] == "Replace"),
        ("sEn & wr==0x40",
         lambda r: r["sEn"] and r["sWr"] == 0x40),
        ("sEn & wr==0x40 & ref&0x40",
         lambda r: r["sEn"] and r["sWr"] == 0x40 and (r["sRef"] & 0x40)),
        ("PROPOSED (+passF==Replace)", lambda r: bool(r["WRITES"])),
        ("sEn & rd==0x40 (reads, no ref test)",
         lambda r: r["sEn"] and r["sRd"] == 0x40),
        ("sEn & ref&0x40 only (known trap)",
         lambda r: r["sEn"] and (r["sRef"] & 0x40)),
    ]
    for nm, fn in variants:
        hits = [r for r in rows if fn(r)]
        say("  %-42s %4d hits   eids %s"
            % (nm, len(hits), [h["eid"] for h in hits][:14]))

    # ------------------------------------------------------------------
    say("")
    say("=" * 78)
    say("== B. the assembly, and where the guard gets armed ==")
    if not reads:
        say("NO reads-draws in this capture - nothing first-person tagged.")
    else:
        lo = min(min(r["pos"] for r in reads),
                 min([r["pos"] for r in writes] or [10 ** 9]))
        hi = max(max(r["pos"] for r in reads),
                 max([r["pos"] for r in writes] or [-1]))
        learn = next((r for r in reads if r["nRTV"] > 0), None)
        say("first WRITES pos: %s"
            % (min([r["pos"] for r in writes]) if writes else "-"))
        say("first READS  pos: %d (eid %d)" % (reads[0]["pos"], reads[0]["eid"]))
        if learn:
            say("LEARN event (first READS draw with a colour target): pos %d "
                "eid %d  vp %s rtv %s"
                % (learn["pos"], learn["eid"], learn["vpWH"], learn["rtvAll"]))
            if writes and min(r["pos"] for r in writes) < learn["pos"]:
                say("  *** THE MASK DRAWS PRECEDE THE LEARN EVENT BY %d DRAWS."
                    % (learn["pos"] - min(r["pos"] for r in writes)))
                say("  *** Inside one frame the guard is NOT YET ARMED when the "
                    "mask arrives.")
        else:
            say("NO reads-draw has a colour target -> guard can never be armed "
                "from reads-draws in this capture.")

        say("")
        say("-- every draw from firstWRITES-4 to lastREADS+6 --")
        a0 = max(0, lo - 4)
        a1 = min(len(rows) - 1, hi + 6)
        say("  %5s %7s %-16s %7s %-6s %-6s %-4s %-22s %-5s %-11s %-11s %s"
            % ("pos", "eid", "kind", "idx", "vs", "ps", "nRTV", "sten rd/wr/ref f/p",
               "dEn", "vp", "dsv", "rtv"))
        for r in rows[a0:a1 + 1]:
            mark = "R" if r["READS"] else (" W" if r["WRITES"] else "  ")
            say("%s %5d %7d %-16s %7d %-6s %-6s %-4d 0x%02X/0x%02X/0x%02X %-6s/%-7s "
                "%-5d %-11s %-11s %s"
                % (mark, r["pos"], r["eid"], r["kind"], r["idx"], r["vs"],
                   r["ps"], r["nRTV"], r["sRd"], r["sWr"], r["sRef"],
                   r["sFunc"][:6], r["sPassF"][:7], r["dEn"], r["vpWH"],
                   r["dsv"] + " " + r["dsvwh"], r["rtvAll"]))

    # ------------------------------------------------------------------
    say("")
    say("=" * 78)
    say("== C. command-list / marker parentage ==")
    chains = {}
    for r in rows:
        chains.setdefault(r["chain"], []).append(r["pos"])
    say("%d distinct parent chains" % len(chains))
    for c in sorted(chains, key=lambda k: -len(chains[k]))[:25]:
        p = chains[c]
        say("  %5d draws  pos %d..%d   %s" % (len(p), p[0], p[-1], c or "<root>"))
    if reads:
        say("")
        say("parent chain of the READS draws : %s"
            % sorted(set(r["chain"] for r in reads)))
        say("parent chain of the WRITES draws: %s"
            % sorted(set(r["chain"] for r in writes)))

    # ------------------------------------------------------------------
    say("")
    say("=" * 78)
    say("== D. the guard, applied ==")
    if reads:
        learn = next((r for r in reads if r["nRTV"] > 0), None)
        if learn:
            SW, SH = learn["_vpw"], learn["_vph"]
            SRTV = learn["rtv0"]
            say("sceneW=%.0f sceneH=%.0f sceneRTV=%s" % (SW, SH, SRTV))

            def guard(r):
                return (r["_vpw"] == SW and r["_vph"] == SH
                        and r["dsv"] != "NULL"
                        and (r["nRTV"] == 0 or r["rtv0"] == SRTV))

            sel = [r for r in rows if (r["READS"] or r["WRITES"]) and guard(r)]
            rej = [r for r in rows if (r["READS"] or r["WRITES"]) and not guard(r)]
            say("union selected & guard-passed: %d" % len(sel))
            say("union selected & GUARD REJECTED: %d" % len(rej))
            for r in rej:
                say("   REJECTED pos %d eid %d vp %s dsv %s rtv %s"
                    % (r["pos"], r["eid"], r["vpWH"], r["dsv"], r["rtvAll"]))
            # would the guard alone admit anything else?
            gonly = [r for r in rows if guard(r)]
            say("draws in the frame that PASS THE GUARD at all: %d "
                "(guard is not a selector, just a filter)" % len(gonly))

    # ------------------------------------------------------------------
    say("")
    say("=" * 78)
    say("== E. mesh-echo keys (IB,ibOff,VB0,vb0Off,stride,idx) ==")
    keys = {}
    for r in rows:
        k = (r["ib"], r["ibOff"], r["vb0"], r["vb0Off"], r["stride"], r["idx"])
        keys.setdefault(k, []).append(r["eid"])
    if writes:
        for r in writes:
            k = (r["ib"], r["ibOff"], r["vb0"], r["vb0Off"], r["stride"],
                 r["idx"])
            say("  mask eid %d key %s -> drawn by eids %s"
                % (r["eid"], k, keys[k]))
    # any key shared by >2 draws that includes a mask?  and how many keys
    # collide frame-wide at all
    coll = {k: v for k, v in keys.items() if len(v) > 1}
    say("  keys used by >1 draw, frame-wide: %d of %d" % (len(coll), len(keys)))

    # ------------------------------------------------------------------
    say("")
    say("=" * 78)
    say("== F. every distinct stencil tuple in the frame ==")
    tup = {}
    for r in rows:
        k = (r["sEn"], r["sRd"], r["sWr"], r["sRef"], r["sFunc"], r["sPassF"])
        c = tup.setdefault(k, [0, []])
        c[0] += 1
        if len(c[1]) < 5:
            c[1].append(r["eid"])
    say("  %-4s %-26s %-8s %6s  sample eids" % ("sEn", "rd/wr/ref", "func/pass",
                                                "count"))
    for k in sorted(tup, key=lambda k: -tup[k][0]):
        say("  sE%d  0x%02X/0x%02X/0x%02X  %-8s/%-8s %6d  %s"
            % (k[0], k[1], k[2], k[3], k[4][:8], k[5][:8], tup[k][0],
               tup[k][1]))

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
