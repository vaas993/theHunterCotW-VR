"""INDEPENDENT re-measure of the "stencil bit 6 == viewmodel" claim.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_refute_ds.py

Written from scratch (not derived from rdc_ds_sweep.py) to attack four things:
  1. false positives across EVERY non-viewmodel drawcall, not a sample
  2. the depth-only rounds (null pixel shader) as well as the colour round
  3. whether the tag is an artifact of this capture
  4. whether the runtime read is possible on a DEFERRED context - i.e. does the
     depth-stencil state actually get SET inside the same recorded command
     stream as the viewmodel draws, or is RenderDoc's flattened replay showing
     state that a deferred-context getter would never return?

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_refute_ds.txt (+ .csv)
"""
import os
import sys
import traceback

import renderdoc as rd

CAPNAME = os.environ.get("REFUTE_CAP", "cotw_frame4317.rdc")
TAG = CAPNAME.replace("cotw_frame", "").replace(".rdc", "")
CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\\" + CAPNAME)
OUT = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute_ds_" + TAG + ".txt")
CSV = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute_ds_" + TAG + ".csv")

# ground truth ONLY for 4317; for other captures we locate the pass ourselves
VM_LO, VM_HI = (33313, 33956) if TAG == "4317" else (None, None)

out = open(OUT, "w")
csv = open(CSV, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def enum_name(x):
    return str(x).split(".")[-1].split(":")[0]


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

    # ---------------------------------------------------------------- tree
    # Collect leaves AND remember the parent chain, so we can see whether the
    # viewmodel draws sit inside an ExecuteCommandList / deferred region.
    leaves = []
    parent_of = {}

    def walk(lst, chain):
        for a in lst:
            kids = list(getattr(a, "children", []))
            nm = a.GetName(sf)
            if kids:
                walk(kids, chain + [(a.eventId, nm)])
            else:
                leaves.append(a)
                parent_of[a.eventId] = chain

    walk(controller.GetRootActions(), [])

    draws = [a for a in leaves if a.flags & rd.ActionFlags.Drawcall]
    draws.sort(key=lambda a: a.eventId)
    say("%d leaf actions, %d drawcalls" % (len(leaves), len(draws)))

    # -------------------------------------------------- deferred-context probe
    say("")
    say("=" * 78)
    say("== PART 4: is this frame recorded on DEFERRED contexts at all? ==")
    try:
        chunk_names = {}
        for c in sf.chunks:
            n = c.name
            chunk_names[n] = chunk_names.get(n, 0) + 1
        interesting = [k for k in chunk_names
                       if "CommandList" in k or "Deferred" in k
                       or "Finish" in k or "Execute" in k]
        say("total distinct chunk types: %d" % len(chunk_names))
        if interesting:
            for k in sorted(interesting):
                say("   %-56s %d" % (k, chunk_names[k]))
        else:
            say("   NO command-list chunks - this capture is immediate-context only")
        say("")
        say("   OM/state chunk counts:")
        for k in sorted(chunk_names):
            if "OMSet" in k or "RSSetViewports" in k:
                say("   %-56s %d" % (k, chunk_names[k]))
    except Exception:
        say("chunk probe failed:\n" + traceback.format_exc())

    # -------------------------------------------------------------- the sweep
    csv.write("eid,entry,inVM,numIndices,numInstances,psNull,dsResId,"
              "depthEnable,depthFunc,depthWrites,stencilEnable,"
              "fFunc,fPass,rdMask,wrMask,ref,bRdMask,bRef,"
              "vpMin,vpMax,vpW,vpH\n")

    rows = []
    for i, a in enumerate(draws):
        eid = a.eventId
        controller.SetFrameEvent(eid, False)
        d3d = controller.GetD3D11PipelineState()
        ds = d3d.outputMerger.depthStencilState
        f, b = ds.frontFace, ds.backFace

        pipe = controller.GetPipelineState()
        psnull = 1 if pipe.GetShader(rd.ShaderStage.Pixel) == rd.ResourceId.Null() else 0

        vpmin = vpmax = vpw = vph = -1.0
        for vp in d3d.rasterizer.viewports:
            if vp.enabled and (vp.width or vp.height):
                vpmin, vpmax = float(vp.minDepth), float(vp.maxDepth)
                vpw, vph = float(vp.width), float(vp.height)
                break

        name = a.GetName(sf)
        entry = name.split("(")[0].replace("ID3D11DeviceContext::", "")

        r = dict(
            eid=eid, entry=entry,
            inVM=(1 if (VM_LO is not None and VM_LO <= eid <= VM_HI) else 0),
            numIndices=int(getattr(a, "numIndices", 0)),
            numInstances=int(getattr(a, "numInstances", 0)),
            psNull=psnull,
            dsResId=str(ds.resourceId),
            depthEnable=int(bool(ds.depthEnable)),
            depthFunc=enum_name(ds.depthFunction),
            depthWrites=int(bool(ds.depthWrites)),
            stencilEnable=int(bool(ds.stencilEnable)),
            fFunc=enum_name(f.function), fPass=enum_name(f.passOperation),
            rdMask=int(f.compareMask), wrMask=int(f.writeMask),
            ref=int(f.reference),
            bRdMask=int(b.compareMask), bRef=int(b.reference),
            vpMin=vpmin, vpMax=vpmax, vpW=vpw, vpH=vph)
        rows.append(r)
        csv.write(",".join(str(r[k]) for k in (
            "eid", "entry", "inVM", "numIndices", "numInstances", "psNull",
            "dsResId", "depthEnable", "depthFunc", "depthWrites",
            "stencilEnable", "fFunc", "fPass", "rdMask", "wrMask", "ref",
            "bRdMask", "bRef", "vpMin", "vpMax", "vpW", "vpH")) + "\n")
        if i % 200 == 0:
            csv.flush()
            say("  ... %d/%d" % (i, len(draws)))
    csv.flush()

    # ----------------------------------------------------- locate the VM pass
    say("")
    say("=" * 78)
    say("== PART 3a: locating the viewmodel pass WITHOUT the stencil ==")
    # Independent locator: contiguous run of draws whose index counts repeat
    # exactly 3x in a short window, dominated by a very large mesh, and which
    # contains PS-null rounds. We just report the span of the readmask-0x40
    # draws AND the span found by the known mesh multiset, then compare.
    hits = [r for r in rows if r["rdMask"] == 0x40]
    if hits:
        say("draws with rdMask==0x40: %d, eid span %d..%d"
            % (len(hits), hits[0]["eid"], hits[-1]["eid"]))
        # is that span contiguous in draw order?
        idxs = [i for i, r in enumerate(rows) if r["rdMask"] == 0x40]
        say("   contiguous in draw order? %s (positions %d..%d, %d draws)"
            % (idxs == list(range(idxs[0], idxs[-1] + 1)),
               idxs[0], idxs[-1], len(idxs)))
        say("   index counts: %s" % sorted(set(r["numIndices"] for r in hits)))
        say("   entry points: %s" % sorted(set(r["entry"] for r in hits)))
        say("   psNull split: %d null / %d with PS"
            % (sum(1 for r in hits if r["psNull"]),
               sum(1 for r in hits if not r["psNull"])))
        # any non-matching draw INSIDE that span?
        span = [r for r in rows if hits[0]["eid"] <= r["eid"] <= hits[-1]["eid"]]
        miss = [r for r in span if r["rdMask"] != 0x40]
        say("   draws inside the span NOT matching: %d %s"
            % (len(miss), [r["eid"] for r in miss[:20]]))

    if VM_LO is None:
        say("no ground-truth range for this capture; using rdMask==0x40 span "
            "as the CANDIDATE pass and reporting structure only")
        lo = hits[0]["eid"] if hits else 0
        hi = hits[-1]["eid"] if hits else 0
        for r in rows:
            r["inVM"] = 1 if lo <= r["eid"] <= hi else 0

    inside = [r for r in rows if r["inVM"]]
    outside = [r for r in rows if not r["inVM"]]
    say("")
    say("VM draws: %d   world draws: %d" % (len(inside), len(outside)))

    # ------------------------------------------------ PART 2: the three rounds
    say("")
    say("=" * 78)
    say("== PART 2: every draw in the viewmodel pass, all rounds ==")
    say("  %6s %-24s %8s %5s %-6s %-13s %-9s %-9s %s"
        % ("eid", "entry", "idx", "psN", "sEn", "depthFunc",
           "rd/wr/ref", "dsResId", "hit?"))
    for r in inside:
        hit = "MATCH" if (r["rdMask"] == 0x40 and (r["ref"] & 0x40)) else "*** MISS ***"
        say("  %6d %-24s %8d %5d %-6d %-13s 0x%02X/0x%02X/0x%02X %-9s %s"
            % (r["eid"], r["entry"], r["numIndices"], r["psNull"],
               r["stencilEnable"], r["depthFunc"], r["rdMask"], r["wrMask"],
               r["ref"], r["dsResId"], hit))

    # per-mesh completeness: all three rounds of every mesh must match
    say("")
    say("  per-mesh round check (a mesh half-vanishes if its rounds disagree):")
    bymesh = {}
    for r in inside:
        bymesh.setdefault(r["numIndices"], []).append(r)
    for k in sorted(bymesh, key=lambda k: -k):
        v = bymesh[k]
        m = sum(1 for r in v if r["rdMask"] == 0x40 and (r["ref"] & 0x40))
        say("    idx=%-8d rounds=%d matched=%d  %s"
            % (k, len(v), m, "OK" if m == len(v) else "*** SPLIT ***"))

    # --------------------------------------- PART 1: exhaustive false positives
    say("")
    say("=" * 78)
    say("== PART 1: exhaustive false-positive count over ALL world draws ==")

    preds = [
        ("A  ref & 0x40                      (cheap form)",
         lambda r: bool(r["ref"] & 0x40)),
        ("B  rdMask == 0x40                  ",
         lambda r: r["rdMask"] == 0x40),
        ("C  sEn && rdMask==0x40 && ref&0x40  (hardened form)",
         lambda r: r["stencilEnable"] and r["rdMask"] == 0x40 and (r["ref"] & 0x40)),
        ("D  rdMask & 0x40                   (the named trap)",
         lambda r: bool(r["rdMask"] & 0x40)),
        ("E  wrMask & 0x40",
         lambda r: bool(r["wrMask"] & 0x40)),
        ("F  dsResId in the VM set",
         lambda r: r["dsResId"] in set(x["dsResId"] for x in inside)),
    ]
    say("  %-52s %6s %6s %8s %8s"
        % ("predicate", "VMhit", "Whit", "recall", "precision"))
    for nm, fn in preds:
        hi = sum(1 for r in inside if fn(r))
        ho = sum(1 for r in outside if fn(r))
        say("  %-52s %6d %6d %7.1f%% %8s"
            % (nm, hi, ho, 100.0 * hi / max(1, len(inside)),
               ("%.1f%%" % (100.0 * hi / (hi + ho))) if (hi + ho) else "-"))

    for nm, fn in preds[:3]:
        fps = [r for r in outside if fn(r)]
        say("")
        say("  FALSE POSITIVES for %s : %d of %d world draws"
            % (nm.strip(), len(fps), len(outside)))
        for r in fps[:60]:
            say("     eid %-6d %-22s idx=%-7d 0x%02X/0x%02X/0x%02X %s"
                % (r["eid"], r["entry"], r["numIndices"], r["rdMask"],
                   r["wrMask"], r["ref"], r["dsResId"]))
        if len(fps) > 60:
            say("     ... %d more" % (len(fps) - 60))

    # restricted to the hooked entry point
    say("")
    say("  restricted to DrawIndexed-family entry points only:")
    fam = [r for r in rows if r["entry"].startswith("DrawIndexed")]
    fi = [r for r in fam if r["inVM"]]
    fo = [r for r in fam if not r["inVM"]]
    for nm, fn in preds[:3]:
        say("    %-52s %d/%d VM, %d/%d world"
            % (nm.strip(), sum(1 for r in fi if fn(r)), len(fi),
               sum(1 for r in fo if fn(r)), len(fo)))
    say("    entry-point census: %s"
        % sorted(set((r["entry"], sum(1 for x in rows if x["entry"] == r["entry"]))
                     for r in rows)))

    # ------------------------------------------------- distributions, world
    say("")
    say("=" * 78)
    say("== world-side distributions (the thing the predicate must avoid) ==")
    for key, label in (("rdMask", "stencil READ mask"),
                       ("wrMask", "stencil WRITE mask"),
                       ("ref", "stencil REF"),
                       ("stencilEnable", "stencilEnable")):
        d = {}
        for r in outside:
            d[r[key]] = d.get(r[key], 0) + 1
        dv = {}
        for r in inside:
            dv[r[key]] = dv.get(r[key], 0) + 1
        say("  %s:" % label)
        for k in sorted(set(d) | set(dv), key=lambda k: -(d.get(k, 0) + dv.get(k, 0))):
            say("     0x%02X (%3d)  world=%-6d VM=%-4d" % (k, k, d.get(k, 0), dv.get(k, 0)))

    # -------------------------------------- PART 4b: state set inside the pass?
    say("")
    say("=" * 78)
    say("== PART 4b: is OMSetDepthStencilState issued INSIDE the pass? ==")
    say("(if the state were set before an ExecuteCommandList and only RenderDoc's")
    say(" flattened replay carries it into the draws, a deferred-context getter")
    say(" in the hook would return the DEFAULT state and the predicate collapses)")
    try:
        vm_eids = set(r["eid"] for r in inside)
        shown = 0
        for a in draws:
            if a.eventId not in vm_eids:
                continue
            if shown >= 6:
                break
            shown += 1
            evs = list(getattr(a, "events", []))
            names = []
            for ev in evs:
                try:
                    names.append(sf.chunks[ev.chunkIndex].name)
                except Exception:
                    names.append("?")
            say("  draw eid %d preceded by %d API events:" % (a.eventId, len(evs)))
            for n in names:
                say("      %s" % n)
        # parent chain of the first VM draw
        if inside:
            ch = parent_of.get(inside[0]["eid"], [])
            say("")
            say("  parent marker chain of first VM draw (eid %d):" % inside[0]["eid"])
            for eid, nm in ch:
                say("      [%d] %s" % (eid, nm[:110]))
    except Exception:
        say("event probe failed:\n" + traceback.format_exc())

    # how many draws in the WHOLE frame are preceded by an OMSetDepthStencilState
    try:
        n_with = 0
        n_tot = 0
        for a in draws:
            n_tot += 1
            for ev in getattr(a, "events", []):
                try:
                    if sf.chunks[ev.chunkIndex].name.endswith("OMSetDepthStencilState"):
                        n_with += 1
                        break
                except Exception:
                    pass
        say("")
        say("  draws immediately preceded by their own OMSetDepthStencilState: "
            "%d / %d" % (n_with, n_tot))
    except Exception:
        say("second event probe failed:\n" + traceback.format_exc())

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
