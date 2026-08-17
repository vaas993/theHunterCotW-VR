"""Same IA sweep, run on the OTHER capture, to test generalisation.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_ia_sweep2.py

rdc_ia_sweep.py found that in cotw_frame4317 the nine viewmodel meshes each own
a private index buffer that no other draw in the frame touches.  "Unique in one
frame" is exactly the claim that already burned index count, so this repeats the
measurement on cotw_frame18616 (different session, 3.3 GB).  Ground truth there
is not given, so the viewmodel block is located the same way the mod would have
to: the largest contiguous run of plain DrawIndexed calls carrying the known
weapon index counts.  Then: do those draws' index buffers appear anywhere else
in that frame?

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ia_sweep2.csv + .txt
"""
import os
import time
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame18616.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ia_sweep2.txt")
CSV = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ia_sweep2.csv")

# index counts seen on the viewmodel in the two earlier captures
KNOWN = {77484, 5007, 162, 6087, 5958, 10272, 3252, 2484, 9594, 4992, 5748}

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def rid_str(r):
    if r is None:
        return "0"
    s = str(r)
    d = "".join(ch for ch in s if ch.isdigit())
    return d if d else s


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
    say("%d leaf actions, %d drawcalls, eids %d..%d"
        % (len(leaves), len(draws), draws[0].eventId, draws[-1].eventId))

    csv = open(CSV, "w")
    csv.write("eid,api,numIndices,numInstances,topology,layout,ib,ibfmt,"
              "idxoff,baseVertex,nvb,vb0,vb0stride,vb0off,strides\n")
    recs = []
    t1 = time.time()
    errors = 0
    for i, a in enumerate(draws):
        eid = a.eventId
        try:
            controller.SetFrameEvent(eid, False)
            ia = controller.GetD3D11PipelineState().inputAssembly
            layout = rid_str(getattr(ia, "resourceId", None))
            topo = str(getattr(ia, "topology", "?"))
            vbids, strides, offs = [], [], []
            for vb in ia.vertexBuffers:
                r = rid_str(getattr(vb, "resourceId", None))
                if r == "0":
                    continue
                vbids.append(r)
                strides.append(int(getattr(vb, "byteStride", 0)))
                offs.append(int(getattr(vb, "byteOffset", 0)))
            ib = ia.indexBuffer
            ibid = rid_str(getattr(ib, "resourceId", None))
            ibfmt = int(getattr(ib, "byteStride", 0))
            name = a.GetName(sf)
            api = name.split("(")[0] if name else "?"
            rec = (eid, api, int(getattr(a, "numIndices", 0)),
                   int(getattr(a, "numInstances", 0)), topo, layout, ibid,
                   ibfmt, int(getattr(a, "indexOffset", 0)),
                   int(getattr(a, "baseVertex", 0)), len(vbids),
                   vbids[0] if vbids else "-",
                   strides[0] if strides else -1,
                   offs[0] if offs else -1,
                   "+".join(str(s) for s in strides))
            recs.append(rec)
            csv.write(",".join(str(x) for x in rec) + "\n")
        except Exception as e:
            errors += 1
            if errors <= 5:
                say("  eid %d FAILED %s: %s" % (eid, type(e).__name__, e))
        if i % 300 == 0:
            say("  ...%d/%d (%.1fs)" % (i, len(draws), time.time() - t1))
            csv.flush()
    csv.close()
    say("swept %d draws, %d errors, %.1fs" % (len(recs), errors,
                                              time.time() - t1))

    # ---- locate the viewmodel block --------------------------------------
    cand = [r for r in recs
            if r[1].endswith("DrawIndexed") and r[2] in KNOWN and r[3] == 0]
    say("")
    say("== draws carrying a known weapon index count (plain DrawIndexed) ==")
    for r in cand:
        say("  eid %-6d n=%-6d lay=%-6s ib=%-6s vb0=%-6s str=%s"
            % (r[0], r[2], r[5], r[6], r[11], r[14]))
    if not cand:
        say("  none - this capture has no viewmodel on screen; "
            "no generalisation test possible")
        controller.Shutdown()
        cap.Shutdown()
        return

    # largest contiguous cluster by eid gap
    clusters, cur = [], [cand[0]]
    for prev, r in zip(cand, cand[1:]):
        if r[0] - prev[0] <= 400:
            cur.append(r)
        else:
            clusters.append(cur)
            cur = [r]
    clusters.append(cur)
    clusters.sort(key=len, reverse=True)
    block = clusters[0]
    lo, hi = block[0][0], block[-1][0]
    say("")
    say("== %d cluster(s); largest is eid %d..%d with %d draws =="
        % (len(clusters), lo, hi, len(block)))

    inpass = [r for r in recs if lo <= r[0] <= hi]
    outpass = [r for r in recs if not (lo <= r[0] <= hi)]
    say("  draws inside that eid range : %d" % len(inpass))
    say("  draws outside               : %d" % len(outpass))

    vm_ib = set(r[6] for r in inpass)
    vm_lay = set(r[5] for r in inpass)
    vm_vb = set(r[11] for r in inpass)
    out_ib = set(r[6] for r in outpass)
    out_lay = set(r[5] for r in outpass)
    out_vb = set(r[11] for r in outpass)

    say("")
    say("== index buffers ==")
    say("  distinct IBs in the block : %d  %s" % (len(vm_ib), sorted(vm_ib)))
    say("  of those, also used outside: %s" % sorted(vm_ib & out_ib))
    hit = sum(1 for r in inpass if r[6] not in out_ib)
    fp = sum(1 for r in outpass if r[6] in vm_ib)
    say("  -> in-block draws on a block-exclusive IB: %d/%d" % (hit, len(inpass)))
    say("  -> outside draws that would match the block IB set: %d/%d"
        % (fp, len(outpass)))

    say("")
    say("== input layouts ==")
    say("  distinct layouts in the block: %d  %s" % (len(vm_lay), sorted(vm_lay)))
    say("  also used outside            : %s" % sorted(vm_lay & out_lay))
    fp = sum(1 for r in outpass if r[5] in vm_lay)
    say("  -> outside draws matching the block layout set: %d/%d"
        % (fp, len(outpass)))

    say("")
    say("== vertex buffers (slot 0) ==")
    say("  distinct VB0s in the block  : %d" % len(vm_vb))
    say("  also used outside           : %s" % sorted(vm_vb & out_vb))
    fp = sum(1 for r in outpass if r[11] in vm_vb)
    say("  -> outside draws matching the block VB0 set: %d/%d"
        % (fp, len(outpass)))

    say("")
    say("== index-count collisions in THIS frame (why count fails) ==")
    for n in sorted({r[2] for r in block}):
        same = [r for r in recs if r[2] == n]
        outside_same = [r for r in same if not (lo <= r[0] <= hi)]
        say("  n=%-6d %d draw(s) frame-wide, %d outside the block %s"
            % (n, len(same), len(outside_same),
               [r[0] for r in outside_same][:8]))

    say("")
    say("== every draw in the block ==")
    for r in inpass:
        say("  eid %-6d %-34s n=%-6d lay=%-6s ib=%-6s vb0=%-6s str=%s"
            % (r[0], r[1], r[2], r[5], r[6], r[11], r[14]))

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
