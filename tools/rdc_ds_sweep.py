"""DEPTH-STENCIL / VIEWPORT-DEPTH discriminator sweep over the whole frame.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_ds_sweep.py

Question: is there a piece of depth-stencil state, or a viewport depth range,
that is TRUE for essentially every draw in the viewmodel pass 33313..33956 and
FALSE for essentially every other draw in the frame?

rdc_ds_probe.txt established the exact 1.45 attribute names, so nothing here is
guessed:
    d3d.outputMerger.depthStencilState  -> .depthEnable .depthFunction
                                           .depthWrites .stencilEnable
                                           .frontFace/.backFace (StencilFace:
                                           .function .passOperation
                                           .failOperation .depthFailOperation
                                           .compareMask .writeMask .reference)
    d3d.rasterizer.viewports[0]         -> .minDepth .maxDepth .x .y .w .h
    d3d.rasterizer.state                -> .depthClip .depthBias .cullMode

Every field is copied to a plain Python int/float on the spot - the SWIG proxies
are temporaries and must not be held across the loop.

Outputs:
    %LOCALAPPDATA%\theHunterCotWVR\rdc_ds_sweep.csv   one row per drawcall
    %LOCALAPPDATA%\theHunterCotWVR\rdc_ds_sweep.txt   the analysis
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ds_sweep.txt")
CSV = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ds_sweep.csv")

VM_LO, VM_HI = 33313, 33956

out = open(OUT, "w")
csv = open(CSV, "w")


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
    say("%d drawcalls in the frame" % len(draws))

    csv.write("eid,inVM,name,numIndices,numInstances,dsResId,depthEnable,"
              "depthFunc,depthWrites,stencilEnable,fFunc,fPass,fFail,fDepthFail,"
              "readMask,writeMask,ref,facesDiffer,depthReadOnly,"
              "stencilReadOnly,vpMinDepth,vpMaxDepth,vpX,vpY,vpW,vpH,"
              "nEnabledVp,depthClip,depthBias,slopeBias,cullMode,depthTarget\n")

    rows = []
    for i, a in enumerate(draws):
        eid = a.eventId
        controller.SetFrameEvent(eid, False)
        d3d = controller.GetD3D11PipelineState()
        om = d3d.outputMerger
        ds = om.depthStencilState
        f = ds.frontFace
        b = ds.backFace

        r = {}
        r["eid"] = eid
        r["inVM"] = 1 if VM_LO <= eid <= VM_HI else 0
        r["name"] = a.GetName(sf).split("(")[0][:40]
        r["numIndices"] = int(getattr(a, "numIndices", 0))
        r["numInstances"] = int(getattr(a, "numInstances", 0))
        r["dsResId"] = str(ds.resourceId)
        r["depthEnable"] = int(bool(ds.depthEnable))
        r["depthFunc"] = str(ds.depthFunction).split(".")[-1].split(":")[0]
        r["depthWrites"] = int(bool(ds.depthWrites))
        r["stencilEnable"] = int(bool(ds.stencilEnable))
        r["fFunc"] = str(f.function).split(".")[-1].split(":")[0]
        r["fPass"] = str(f.passOperation).split(".")[-1].split(":")[0]
        r["fFail"] = str(f.failOperation).split(".")[-1].split(":")[0]
        r["fDepthFail"] = str(f.depthFailOperation).split(".")[-1].split(":")[0]
        r["readMask"] = int(f.compareMask)
        r["writeMask"] = int(f.writeMask)
        r["ref"] = int(f.reference)
        r["facesDiffer"] = int(
            str(b.function) != str(f.function)
            or int(b.compareMask) != int(f.compareMask)
            or int(b.writeMask) != int(f.writeMask)
            or int(b.reference) != int(f.reference)
            or str(b.passOperation) != str(f.passOperation))
        r["depthReadOnly"] = int(bool(om.depthReadOnly))
        r["stencilReadOnly"] = int(bool(om.stencilReadOnly))

        vps = d3d.rasterizer.viewports
        nen = 0
        vp0 = None
        for vp in vps:
            if vp.enabled and (vp.width or vp.height):
                nen += 1
                if vp0 is None:
                    vp0 = (float(vp.minDepth), float(vp.maxDepth),
                           float(vp.x), float(vp.y),
                           float(vp.width), float(vp.height))
        if vp0 is None:
            vp0 = (-1.0, -1.0, -1.0, -1.0, -1.0, -1.0)
        (r["vpMinDepth"], r["vpMaxDepth"], r["vpX"], r["vpY"],
         r["vpW"], r["vpH"]) = vp0
        r["nEnabledVp"] = nen

        st = d3d.rasterizer.state
        r["depthClip"] = int(bool(st.depthClip))
        r["depthBias"] = int(st.depthBias)
        r["slopeBias"] = float(st.slopeScaledDepthBias)
        r["cullMode"] = str(st.cullMode).split(".")[-1].split(":")[0]

        dt = om.depthTarget
        try:
            r["depthTarget"] = str(dt.resource)
        except Exception:
            r["depthTarget"] = "?"

        rows.append(r)
        csv.write(",".join(str(r[k]) for k in (
            "eid", "inVM", "name", "numIndices", "numInstances", "dsResId",
            "depthEnable", "depthFunc", "depthWrites", "stencilEnable",
            "fFunc", "fPass", "fFail", "fDepthFail", "readMask", "writeMask",
            "ref", "facesDiffer", "depthReadOnly", "stencilReadOnly",
            "vpMinDepth", "vpMaxDepth", "vpX", "vpY", "vpW", "vpH",
            "nEnabledVp", "depthClip", "depthBias", "slopeBias", "cullMode",
            "depthTarget")) + "\n")
        if i % 100 == 0:
            csv.flush()
            say("  ... %d/%d (eid %d)" % (i, len(draws), eid))

    csv.flush()

    inside = [r for r in rows if r["inVM"]]
    outside = [r for r in rows if not r["inVM"]]
    say("")
    say("draws inside  %d..%d : %d" % (VM_LO, VM_HI, len(inside)))
    say("draws outside            : %d" % len(outside))

    def tally(label, keyfn):
        say("")
        say("=" * 78)
        say("== %s ==" % label)
        ins, outs = {}, {}
        for r in inside:
            ins[keyfn(r)] = ins.get(keyfn(r), 0) + 1
        for r in outside:
            outs[keyfn(r)] = outs.get(keyfn(r), 0) + 1
        allk = sorted(set(ins) | set(outs), key=lambda k: (-ins.get(k, 0),
                                                          -outs.get(k, 0),
                                                          str(k)))
        say("  %-46s %8s %8s   %s" % ("value", "inVM", "world", "verdict"))
        for k in allk:
            i, o = ins.get(k, 0), outs.get(k, 0)
            if i and not o:
                v = "VM-ONLY"
            elif o and not i:
                v = "world-only"
            else:
                v = "shared"
            say("  %-46s %8d %8d   %s" % (str(k)[:46], i, o, v))
        # best single-value discriminator for this dimension
        best = None
        for k in allk:
            i, o = ins.get(k, 0), outs.get(k, 0)
            score = i - o * 100
            if best is None or score > best[0]:
                best = (score, k, i, o)
        if best:
            say("  -> best single value: %s  catches %d/%d VM, %d false pos"
                % (str(best[1])[:40], best[2], len(inside), best[3]))
        return ins, outs

    tally("depth function", lambda r: r["depthFunc"])
    tally("depth enable / writes", lambda r: "enable=%d writes=%d"
          % (r["depthEnable"], r["depthWrites"]))
    tally("stencil enable", lambda r: r["stencilEnable"])
    tally("stencil REFERENCE value",
          lambda r: "ref=%d (0x%02X)" % (r["ref"], r["ref"]))
    tally("stencil READ mask (compareMask)",
          lambda r: "read=%d (0x%02X)" % (r["readMask"], r["readMask"]))
    tally("stencil WRITE mask",
          lambda r: "write=%d (0x%02X)" % (r["writeMask"], r["writeMask"]))
    tally("stencil func / pass op",
          lambda r: "%s / pass=%s" % (r["fFunc"], r["fPass"]))
    tally("VIEWPORT depth range",
          lambda r: "min=%.5f max=%.5f" % (r["vpMinDepth"], r["vpMaxDepth"]))
    tally("viewport rect", lambda r: "%.0fx%.0f @%.0f,%.0f"
          % (r["vpW"], r["vpH"], r["vpX"], r["vpY"]))
    tally("depth-stencil STATE OBJECT id", lambda r: r["dsResId"])
    tally("depthClip / bias", lambda r: "clip=%d bias=%d slope=%.3f"
          % (r["depthClip"], r["depthBias"], r["slopeBias"]))
    tally("depth target", lambda r: r["depthTarget"])
    tally("depth/stencil read-only", lambda r: "dRO=%d sRO=%d"
          % (r["depthReadOnly"], r["stencilReadOnly"]))
    tally("FULL depth-stencil tuple",
          lambda r: "dE%d %s dW%d | sE%d %s p=%s rd=0x%02X wr=0x%02X ref=0x%02X"
          % (r["depthEnable"], r["depthFunc"], r["depthWrites"],
             r["stencilEnable"], r["fFunc"], r["fPass"], r["readMask"],
             r["writeMask"], r["ref"]))
    tally("ref+masks only (what OMGetDepthStencilState+GetDesc gives)",
          lambda r: "rd=0x%02X wr=0x%02X ref=0x%02X"
          % (r["readMask"], r["writeMask"], r["ref"]))

    # ---- combined-predicate evaluation ----------------------------------
    say("")
    say("=" * 78)
    say("== candidate predicates, scored ==")

    cands = [
        ("stencil writeMask == 0x96 (150)", lambda r: r["writeMask"] == 150),
        ("stencil writeMask == 0x04 (4)", lambda r: r["writeMask"] == 4),
        ("stencil writeMask in {0x96,0x04}",
         lambda r: r["writeMask"] in (150, 4)),
        ("stencil readMask == 0x40 (64)", lambda r: r["readMask"] == 64),
        ("stencil ref & 0x40", lambda r: bool(r["ref"] & 0x40)),
        ("stencil ref == 0x44 or 0x56", lambda r: r["ref"] in (0x44, 0x56)),
        ("stencil passOp == Replace", lambda r: r["fPass"] == "Replace"),
        ("readMask==0x40 and passOp==Replace",
         lambda r: r["readMask"] == 64 and r["fPass"] == "Replace"),
        ("readMask==0x40 and writeMask!=0",
         lambda r: r["readMask"] == 64 and r["writeMask"] != 0),
        ("depthFunc == AlwaysTrue", lambda r: r["depthFunc"] == "AlwaysTrue"),
        ("depthFunc==AlwaysTrue and depthWrites",
         lambda r: r["depthFunc"] == "AlwaysTrue" and r["depthWrites"]),
        ("vpMinDepth != 0 or vpMaxDepth != 1",
         lambda r: r["vpMinDepth"] != 0.0 or r["vpMaxDepth"] != 1.0),
        ("depthClip == 0", lambda r: r["depthClip"] == 0),
    ]
    say("  %-52s %6s %6s %7s" % ("predicate", "hitVM", "hitW", "recall"))
    for name, fn in cands:
        hi = sum(1 for r in inside if fn(r))
        ho = sum(1 for r in outside if fn(r))
        say("  %-52s %6d %6d %6.1f%%"
            % (name, hi, ho, 100.0 * hi / max(1, len(inside))))

    # ---- what the world uses that overlaps, per predicate ---------------
    say("")
    say("== every distinct (readMask,writeMask,ref,passOp) in the frame ==")
    combo = {}
    for r in rows:
        k = (r["readMask"], r["writeMask"], r["ref"], r["fPass"], r["fFunc"])
        c = combo.setdefault(k, [0, 0, []])
        c[0 if r["inVM"] else 1] += 1
        if len(c[2]) < 6:
            c[2].append(r["eid"])
    say("  %-44s %6s %6s  sample eids" % ("rd/wr/ref/pass/func", "VM", "world"))
    for k in sorted(combo, key=lambda k: (-combo[k][0], -combo[k][1])):
        c = combo[k]
        say("  0x%02X/0x%02X/0x%02X %-12s %-12s %6d %6d  %s"
            % (k[0], k[1], k[2], k[3], k[4], c[0], c[1], c[2]))

    # ---- the VM draws themselves, in order ------------------------------
    say("")
    say("== every draw in %d..%d ==" % (VM_LO, VM_HI))
    say("  %6s %8s %6s  %-14s %-4s %-10s %s"
        % ("eid", "idx", "inst", "depthFunc", "dW", "rd/wr/ref", "name"))
    for r in inside:
        say("  %6d %8d %6d  %-14s %-4d 0x%02X/0x%02X/0x%02X %s"
            % (r["eid"], r["numIndices"], r["numInstances"], r["depthFunc"],
               r["depthWrites"], r["readMask"], r["writeMask"], r["ref"],
               r["name"]))

    # ---- world draws that would be false positives ----------------------
    say("")
    say("== world draws matching readMask==0x40 (false positives) ==")
    fps = [r for r in outside if r["readMask"] == 64]
    say("  %d of %d world draws" % (len(fps), len(outside)))
    for r in fps[:120]:
        say("  %6d idx=%-7d %-14s dW=%d 0x%02X/0x%02X/0x%02X %s %s"
            % (r["eid"], r["numIndices"], r["depthFunc"], r["depthWrites"],
               r["readMask"], r["writeMask"], r["ref"], r["fPass"], r["name"]))
    if len(fps) > 120:
        say("  ... %d more" % (len(fps) - 120))

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
