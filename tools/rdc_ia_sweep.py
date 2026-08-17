"""Input-assembler state for EVERY drawcall: can IA state isolate the viewmodel?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_ia_sweep.py

The mod must decide at DrawIndexed time whether a draw is the first-person
viewmodel.  Index count alone collides with world meshes; shader CRC is
unusable (shaders exist before the hooks).  This pass asks whether the INPUT
ASSEMBLER separates them: input-layout object, primitive topology, vertex
buffer resource ids + strides + offsets, index buffer resource id + format +
offset.  All of those are one IAGet* call away inside the hook.

API notes for 1.45 (see rdc_cb_api.py for how these were established):
  * controller.GetD3D11PipelineState().inputAssembly holds .resourceId (the
    ID3D11InputLayout object), .layouts (element descs), .vertexBuffers,
    .indexBuffer, .topology.  Field names are probed, never assumed - the
    probe section at the top of the output prints the real dir().
  * Anything read off a SWIG proxy is copied to a plain Python value inside the
    same loop iteration; nothing is held across SetFrameEvent calls.
  * Every failure is recorded, never swallowed - a silent except turns an
    AttributeError into a false negative.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ia_sweep.txt   (report)
        %LOCALAPPDATA%\theHunterCotWVR\rdc_ia_sweep.csv   (per-draw record)
"""
import os
import time
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ia_sweep.txt")
CSV = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ia_sweep.csv")

VM_LO, VM_HI = 33313, 33956          # the viewmodel pass, inclusive

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def rid_str(r):
    """ResourceId -> short numeric string, stable across the run."""
    if r is None:
        return "0"
    s = str(r)
    digits = "".join(ch for ch in s if ch.isdigit())
    return digits if digits else s


def main():
    t0 = time.time()
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open (%.1fs)" % (time.time() - t0))

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
    say("%d leaf actions, %d drawcalls" % (len(leaves), len(draws)))

    # ---------------- probe: what does this build actually expose? --------
    probe_eid = None
    for a in draws:
        if VM_LO <= a.eventId <= VM_HI:
            probe_eid = a.eventId
            break
    if probe_eid is None:
        probe_eid = draws[0].eventId
    controller.SetFrameEvent(probe_eid, True)

    say("")
    say("== API probe at eid %d ==" % probe_eid)
    d3d = None
    try:
        d3d = controller.GetD3D11PipelineState()
        say("  GetD3D11PipelineState -> %s" % type(d3d).__name__)
        say("  attrs: %s" % [n for n in dir(d3d) if not n.startswith("_")])
    except Exception as e:
        say("  GetD3D11PipelineState RAISED %s: %s" % (type(e).__name__, e))
    if d3d is not None:
        ia = getattr(d3d, "inputAssembly", None)
        say("  inputAssembly -> %s" % type(ia).__name__)
        if ia is not None:
            say("    attrs: %s" % [n for n in dir(ia) if not n.startswith("_")])
            for nm in ("resourceId", "topology", "layouts", "vertexBuffers",
                       "indexBuffer", "bytecode"):
                v = getattr(ia, nm, "ABSENT")
                if nm in ("layouts", "vertexBuffers"):
                    try:
                        say("    .%s: %d entries; entry attrs %s"
                            % (nm, len(v),
                               [n for n in dir(v[0]) if not n.startswith("_")]
                               if len(v) else "-"))
                        for i, e in enumerate(list(v)[:12]):
                            say("        [%d] %s" % (i, " ".join(
                                "%s=%r" % (k, getattr(e, k))
                                for k in dir(e)
                                if not k.startswith("_")
                                and not callable(getattr(e, k)))))
                    except Exception as e2:
                        say("    .%s probe failed: %s" % (nm, e2))
                elif nm == "indexBuffer":
                    try:
                        say("    .indexBuffer attrs: %s"
                            % [n for n in dir(v) if not n.startswith("_")])
                        say("        %s" % " ".join(
                            "%s=%r" % (k, getattr(v, k)) for k in dir(v)
                            if not k.startswith("_")
                            and not callable(getattr(v, k))))
                    except Exception as e2:
                        say("    .indexBuffer probe failed: %s" % e2)
                else:
                    say("    .%s = %r" % (nm, v))

    pipe = controller.GetPipelineState()
    say("  generic PipeState IA-ish methods:")
    for n in sorted(dir(pipe)):
        if n.startswith("_"):
            continue
        low = n.lower()
        if any(k in low for k in ("vbuffer", "ibuffer", "topolog", "input",
                                  "vertex", "index", "restart", "stride")):
            say("     %s" % n)
    for meth in ("GetVBuffers", "GetIBuffer", "GetPrimitiveTopology",
                 "GetVertexInputs"):
        fn = getattr(pipe, meth, None)
        if fn is None:
            say("     %s: ABSENT" % meth)
            continue
        try:
            got = fn()
            if meth in ("GetVBuffers", "GetVertexInputs"):
                say("     %s -> %d entries, attrs %s"
                    % (meth, len(got),
                       [n for n in dir(got[0]) if not n.startswith("_")]
                       if len(got) else "-"))
            else:
                say("     %s -> %r ; attrs %s"
                    % (meth, got,
                       [n for n in dir(got) if not n.startswith("_")]))
        except Exception as e:
            say("     %s RAISED %s: %s" % (meth, type(e).__name__, e))

    # ---------------- the sweep -------------------------------------------
    say("")
    say("== sweeping %d drawcalls ==" % len(draws))
    csv = open(CSV, "w")
    csv.write("eid,inpass,api,numIndices,numInstances,topology,layout,"
              "ib,ibfmt,iboff,idxoff,baseVertex,nvb,vb0,vb0stride,vb0off,"
              "strides,vbids,layoutsig\n")

    recs = []
    errors = 0
    t1 = time.time()
    for i, a in enumerate(draws):
        eid = a.eventId
        try:
            controller.SetFrameEvent(eid, False)
            d3d = controller.GetD3D11PipelineState()
            ia = d3d.inputAssembly

            layout_id = rid_str(getattr(ia, "resourceId", None))
            topo = str(getattr(ia, "topology", "?"))

            # element signature - copied out immediately
            sig_parts = []
            try:
                for el in ia.layouts:
                    sig_parts.append("%s%d:%s:s%d:o%s:%s" % (
                        getattr(el, "semanticName", "?"),
                        int(getattr(el, "semanticIndex", 0)),
                        getattr(el, "format", None).Name()
                        if hasattr(getattr(el, "format", None), "Name")
                        else str(getattr(el, "format", "?")),
                        int(getattr(el, "inputSlot", 0)),
                        getattr(el, "byteOffset", 0),
                        "I" if getattr(el, "perInstance", False) else "V"))
            except Exception as e:
                sig_parts = ["ERR:%s" % e]
            layoutsig = "|".join(sig_parts)

            vbids, strides, offsets = [], [], []
            try:
                for vb in ia.vertexBuffers:
                    r = rid_str(getattr(vb, "resourceId", None))
                    if r == "0":
                        continue
                    vbids.append(r)
                    strides.append(int(getattr(vb, "byteStride", 0)))
                    offsets.append(int(getattr(vb, "byteOffset", 0)))
            except Exception as e:
                vbids = ["ERR"]

            ibid, ibstride, iboff = "0", 0, 0
            try:
                ib = ia.indexBuffer
                ibid = rid_str(getattr(ib, "resourceId", None))
                ibstride = int(getattr(ib, "byteStride", 0))
                iboff = int(getattr(ib, "byteOffset", 0))
            except Exception as e:
                ibid = "ERR"

            name = a.GetName(sf)
            api = name.split("(")[0] if name else "?"
            rec = dict(
                eid=eid,
                inpass=(VM_LO <= eid <= VM_HI),
                api=api,
                n=int(getattr(a, "numIndices", 0)),
                inst=int(getattr(a, "numInstances", 0)),
                topo=topo,
                layout=layout_id,
                ib=ibid, ibfmt=ibstride, iboff=iboff,
                idxoff=int(getattr(a, "indexOffset", 0)),
                basev=int(getattr(a, "baseVertex", 0)),
                vbids=vbids, strides=strides, offsets=offsets,
                sig=layoutsig)
            recs.append(rec)
            csv.write("%d,%d,%s,%d,%d,%s,%s,%s,%d,%d,%d,%d,%d,%s,%s,%s,%s,%s,%s\n"
                      % (eid, 1 if rec["inpass"] else 0, api, rec["n"],
                         rec["inst"], topo, layout_id, ibid, ibstride, iboff,
                         rec["idxoff"], rec["basev"], len(vbids),
                         vbids[0] if vbids else "-",
                         strides[0] if strides else -1,
                         offsets[0] if offsets else -1,
                         "+".join(str(s) for s in strides),
                         "+".join(vbids), layoutsig.replace(",", ";")))
        except Exception as e:
            errors += 1
            if errors <= 10:
                say("  eid %d FAILED %s: %s" % (eid, type(e).__name__, e))
        if i % 200 == 0:
            say("  ...%d/%d draws (%.1fs)" % (i, len(draws), time.time() - t1))
            csv.flush()
    csv.close()
    say("  swept %d draws, %d errors, %.1fs"
        % (len(recs), errors, time.time() - t1))

    inp = [r for r in recs if r["inpass"]]
    outp = [r for r in recs if not r["inpass"]]
    say("")
    say("in-pass draws (%d..%d): %d" % (VM_LO, VM_HI, len(inp)))
    say("other draws            : %d" % len(outp))

    def tally(label, keyfn, show_all=False):
        say("")
        say("=" * 74)
        say("== %s ==" % label)
        ins, outs = {}, {}
        for r in inp:
            ins[keyfn(r)] = ins.get(keyfn(r), 0) + 1
        for r in outp:
            outs[keyfn(r)] = outs.get(keyfn(r), 0) + 1
        keys = set(ins) | set(outs)
        rows = sorted(keys, key=lambda k: (-ins.get(k, 0), -outs.get(k, 0)))
        say("  %-46s %6s %8s   %s" % ("value", "INpass", "outside", "verdict"))
        shown = 0
        for k in rows:
            a_, b_ = ins.get(k, 0), outs.get(k, 0)
            if not show_all and a_ == 0 and shown > 40:
                continue
            v = ("EXCLUSIVE to viewmodel" if a_ and not b_ else
                 "viewmodel-absent" if not a_ else
                 "shared (%d outside)" % b_)
            say("  %-46s %6d %8d   %s" % (str(k)[:46], a_, b_, v))
            shown += 1
        # exclusivity summary
        excl = [k for k in keys if ins.get(k, 0) and not outs.get(k, 0)]
        cov = sum(ins[k] for k in excl)
        say("  -> %d value(s) exclusive to the pass, covering %d/%d in-pass "
            "draws (%.1f%%), 0 outside draws"
            % (len(excl), cov, len(inp),
               100.0 * cov / len(inp) if inp else 0))
        return ins, outs

    tally("INPUT LAYOUT object", lambda r: r["layout"])
    tally("PRIMITIVE TOPOLOGY", lambda r: r["topo"])
    tally("API entry point", lambda r: r["api"])
    tally("VB0 stride", lambda r: r["strides"][0] if r["strides"] else -1)
    tally("all VB strides", lambda r: "+".join(str(s) for s in r["strides"]))
    tally("num vertex buffers", lambda r: len(r["vbids"]))
    tally("INDEX BUFFER resource", lambda r: r["ib"])
    tally("index format (bytes)", lambda r: r["ibfmt"])
    tally("VB0 resource", lambda r: r["vbids"][0] if r["vbids"] else "-")
    tally("all VB resources", lambda r: "+".join(r["vbids"]))
    tally("IB byteOffset (bound)", lambda r: r["iboff"])
    tally("layout element signature", lambda r: r["sig"][:44])

    # combos worth a runtime test
    tally("(topology, VB0 stride)",
          lambda r: "%s / %s" % (r["topo"],
                                 r["strides"][0] if r["strides"] else -1))
    tally("(api, VB0 stride)",
          lambda r: "%s / %s" % (r["api"],
                                 r["strides"][0] if r["strides"] else -1))
    tally("(nvb, strides, ibfmt)",
          lambda r: "%d / %s / %d" % (len(r["vbids"]),
                                      "+".join(str(s) for s in r["strides"]),
                                      r["ibfmt"]))

    # full layout signatures, in-pass, printed once each
    say("")
    say("=" * 74)
    say("== full layout element signatures used INSIDE the pass ==")
    seen = {}
    for r in inp:
        seen.setdefault((r["layout"], r["sig"]), []).append(r["eid"])
    for (lay, sig), eids in sorted(seen.items(), key=lambda kv: -len(kv[1])):
        say("")
        say("  layout %s  x%d draws  eids %s" % (lay, len(eids), eids[:8]))
        for part in sig.split("|"):
            say("       %s" % part)

    say("")
    say("=" * 74)
    say("== every in-pass draw, in full ==")
    for r in inp:
        say("  eid %-6d %-26s n=%-6d inst=%-3d topo=%-22s layout=%-6s "
            "ib=%-6s fmt=%d iboff=%d idxoff=%-7d basev=%-7d nvb=%d "
            "strides=%s vbs=%s offs=%s"
            % (r["eid"], r["api"], r["n"], r["inst"], r["topo"], r["layout"],
               r["ib"], r["ibfmt"], r["iboff"], r["idxoff"], r["basev"],
               len(r["vbids"]), "+".join(str(s) for s in r["strides"]),
               "+".join(r["vbids"]),
               "+".join(str(o) for o in r["offsets"])))

    say("")
    say("DONE total %.1fs" % (time.time() - t0))
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    say("EXCEPTION:")
    say(traceback.format_exc())

out.close()
os._exit(0)
