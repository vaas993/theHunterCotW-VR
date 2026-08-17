"""Cross-capture attack: locate the viewmodel pass WITHOUT the stencil, then
score the stencil predicate against that independent location.

Run headlessly:
    set REFUTE_CAP=cotw_frame18616.rdc
    qrenderdoc.exe --python tools\rdc_refute_ds2.py

Independent locators used (none of them look at stencil):
  L1  the 77484-index mesh (the hands) - a count nothing in the world carries
  L2  the August-2 ground-truth weapon index set from cbscan.cpp, established
      before any stencil work existed
  L3  the 3x-repeat structure: a contiguous run of DrawIndexed calls in which
      each index count occurs exactly three times, in the same order each round

Then, and only then, the stencil predicate is scored against it, plus:
  * every draw ANYWHERE in the frame whose index count is a known weapon mesh,
    inside or outside the located span (candidate false negatives)
  * every draw in the frame with ref&0x40 or rdMask==0x40 (candidate false
    positives), regardless of where it is

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_refute2_<tag>.txt
"""
import os
import traceback

import renderdoc as rd

CAPNAME = os.environ.get("REFUTE_CAP", "cotw_frame18616.rdc")
TAG = CAPNAME.replace("cotw_frame", "").replace(".rdc", "")
CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\\" + CAPNAME)
OUT = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute2_" + TAG + ".txt")
CSV = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute2_" + TAG + ".csv")

# cbscan.cpp, August 2, established with no knowledge of any stencil bit.
GT_WEAPON_IDX = {77484, 5007, 162, 6087, 3252, 2484, 4992, 5748, 5958, 9594}

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
    leaves = []

    def walk(lst):
        for a in lst:
            kids = list(getattr(a, "children", []))
            if kids:
                walk(kids, )
            else:
                leaves.append(a)

    walk(controller.GetRootActions())
    draws = [a for a in leaves if a.flags & rd.ActionFlags.Drawcall]
    draws.sort(key=lambda a: a.eventId)
    say("%d drawcalls" % len(draws))

    # command-list census
    try:
        cn = {}
        for c in sf.chunks:
            cn[c.name] = cn.get(c.name, 0) + 1
        for k in sorted(cn):
            if ("CommandList" in k or "Commandlist" in k or "Deferred" in k
                    or "OMSetDepthStencilState" in k):
                say("   chunk %-56s %d" % (k, cn[k]))
    except Exception:
        say("chunk census failed:\n" + traceback.format_exc())

    csv.write("eid,entry,numIndices,psNull,dsResId,stencilEnable,depthFunc,"
              "rdMask,wrMask,ref\n")
    rows = []
    for i, a in enumerate(draws):
        eid = a.eventId
        controller.SetFrameEvent(eid, False)
        d3d = controller.GetD3D11PipelineState()
        ds = d3d.outputMerger.depthStencilState
        f = ds.frontFace
        pipe = controller.GetPipelineState()
        psnull = 1 if pipe.GetShader(rd.ShaderStage.Pixel) == rd.ResourceId.Null() else 0
        nm = a.GetName(sf)
        entry = nm.split("(")[0].replace("ID3D11DeviceContext::", "")
        r = dict(eid=eid, entry=entry, numIndices=int(getattr(a, "numIndices", 0)),
                 psNull=psnull, dsResId=str(ds.resourceId),
                 stencilEnable=int(bool(ds.stencilEnable)),
                 depthFunc=enum_name(ds.depthFunction),
                 rdMask=int(f.compareMask), wrMask=int(f.writeMask),
                 ref=int(f.reference))
        rows.append(r)
        csv.write(",".join(str(r[k]) for k in (
            "eid", "entry", "numIndices", "psNull", "dsResId", "stencilEnable",
            "depthFunc", "rdMask", "wrMask", "ref")) + "\n")
        if i % 300 == 0:
            csv.flush()
            say("  ... %d/%d" % (i, len(draws)))
    csv.flush()

    # ------------------------------------------------ L1/L2: independent anchor
    say("")
    say("=" * 78)
    say("== L1: every draw in the frame with index count 77484 (the hands) ==")
    h = [r for r in rows if r["numIndices"] == 77484]
    for r in h:
        say("   eid %-7d %-24s psNull=%d 0x%02X/0x%02X/0x%02X %s"
            % (r["eid"], r["entry"], r["psNull"], r["rdMask"], r["wrMask"],
               r["ref"], r["dsResId"]))
    if h:
        lo, hi = min(x["eid"] for x in h), max(x["eid"] for x in h)
        say("   -> %d draws, eid span %d..%d" % (len(h), lo, hi))
    else:
        say("   none - the hands mesh is not 77484 in this capture")
        lo = hi = None

    say("")
    say("== L2: every draw in the frame with a GROUND-TRUTH weapon index count ==")
    say("   (cbscan.cpp August-2 set, no stencil knowledge)")
    gt = [r for r in rows if r["numIndices"] in GT_WEAPON_IDX]
    say("   %d draws" % len(gt))
    bycount = {}
    for r in gt:
        bycount.setdefault(r["numIndices"], []).append(r)
    for k in sorted(bycount, key=lambda k: -k):
        v = bycount[k]
        eids = [x["eid"] for x in v]
        m = sum(1 for x in v if x["rdMask"] == 0x40 and (x["ref"] & 0x40))
        say("   idx=%-7d  %2d draws  stencil-matched %2d  eids %s"
            % (k, len(v), m, eids[:14]))

    # ------------------------------------------- L3: the 3x-repeat contiguous run
    say("")
    say("== L3: contiguous runs where every index count occurs exactly 3x ==")
    best = None
    n = len(rows)
    for start in range(n):
        cnt = {}
        for end in range(start, min(start + 60, n)):
            c = rows[end]["numIndices"]
            cnt[c] = cnt.get(c, 0) + 1
            ln = end - start + 1
            if ln >= 12 and len(cnt) >= 4 and all(v == 3 for v in cnt.values()):
                if best is None or ln > best[0]:
                    best = (ln, start, end, dict(cnt))
    if best:
        ln, s, e, cnt = best
        say("   longest such run: %d draws, eid %d..%d, %d distinct meshes"
            % (ln, rows[s]["eid"], rows[e]["eid"], len(cnt)))
        say("   meshes: %s" % sorted(cnt.keys(), reverse=True))
        say("   entry points: %s" % sorted(set(rows[i]["entry"] for i in range(s, e + 1))))
        say("   stencil-matched in that run: %d/%d"
            % (sum(1 for i in range(s, e + 1)
                   if rows[i]["rdMask"] == 0x40 and (rows[i]["ref"] & 0x40)), ln))
        vm_lo, vm_hi = rows[s]["eid"], rows[e]["eid"]
    else:
        say("   no such run found")
        vm_lo = vm_hi = None

    # ---------------------------------------------- score the stencil predicate
    say("")
    say("=" * 78)
    say("== the stencil predicate scored against the INDEPENDENT location ==")
    if vm_lo is None and lo is not None:
        vm_lo, vm_hi = lo, hi
    if vm_lo is None:
        say("   no independent location available")
    else:
        say("   independent viewmodel span: eid %d..%d" % (vm_lo, vm_hi))
        inside = [r for r in rows if vm_lo <= r["eid"] <= vm_hi]
        outside = [r for r in rows if not (vm_lo <= r["eid"] <= vm_hi)]
        say("   %d draws inside, %d outside" % (len(inside), len(outside)))
        for nm, fn in (
                ("ref & 0x40", lambda r: bool(r["ref"] & 0x40)),
                ("rdMask == 0x40", lambda r: r["rdMask"] == 0x40),
                ("sEn && rdMask==0x40 && ref&0x40",
                 lambda r: r["stencilEnable"] and r["rdMask"] == 0x40 and (r["ref"] & 0x40)),
                ("rdMask & 0x40 (trap)", lambda r: bool(r["rdMask"] & 0x40))):
            hi_ = sum(1 for r in inside if fn(r))
            ho_ = sum(1 for r in outside if fn(r))
            say("     %-38s %3d/%3d inside, %4d/%4d outside"
                % (nm, hi_, len(inside), ho_, len(outside)))
        say("")
        say("   ALL draws outside that span with ref&0x40 or rdMask==0x40:")
        bad = [r for r in outside if (r["ref"] & 0x40) or r["rdMask"] == 0x40]
        if not bad:
            say("     none")
        for r in bad[:80]:
            say("     eid %-7d %-24s idx=%-7d 0x%02X/0x%02X/0x%02X %s"
                % (r["eid"], r["entry"], r["numIndices"], r["rdMask"],
                   r["wrMask"], r["ref"], r["dsResId"]))
        say("")
        say("   ALL draws inside that span WITHOUT the tag (false negatives):")
        fn_ = [r for r in inside if not (r["rdMask"] == 0x40 and (r["ref"] & 0x40))]
        if not fn_:
            say("     none")
        for r in fn_[:80]:
            say("     eid %-7d %-24s idx=%-7d 0x%02X/0x%02X/0x%02X"
                % (r["eid"], r["entry"], r["numIndices"], r["rdMask"],
                   r["wrMask"], r["ref"]))

    # ------------------------------------------- the full tagged span, whatever it is
    say("")
    say("=" * 78)
    say("== everything the predicate selects, in draw order ==")
    sel = [r for r in rows if r["rdMask"] == 0x40 and (r["ref"] & 0x40)]
    say("   %d draws selected" % len(sel))
    for r in sel:
        say("     eid %-7d %-22s idx=%-7d psN=%d %-13s 0x%02X/0x%02X/0x%02X %s"
            % (r["eid"], r["entry"], r["numIndices"], r["psNull"],
               r["depthFunc"], r["rdMask"], r["wrMask"], r["ref"], r["dsResId"]))
    if sel:
        pos = [i for i, r in enumerate(rows) if r["rdMask"] == 0x40 and (r["ref"] & 0x40)]
        say("   contiguous in draw order? %s"
            % (pos == list(range(pos[0], pos[-1] + 1))))

    say("")
    say("== world-side stencil distributions ==")
    for key in ("rdMask", "ref"):
        d = {}
        for r in rows:
            if r["rdMask"] == 0x40 and (r["ref"] & 0x40):
                continue
            d[r[key]] = d.get(r[key], 0) + 1
        say("  %s: %s" % (key, ", ".join("0x%02X=%d" % (k, d[k])
                                         for k in sorted(d, key=lambda k: -d[k]))))

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
