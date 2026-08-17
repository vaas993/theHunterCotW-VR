"""Does the stencil signature generalise to a SECOND capture?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_ds_crosscap.py

cotw_frame4317 says: stencil compareMask (StencilReadMask) == 0x40 fires on
27/27 viewmodel draws and 0/2187 world draws.  "Verified-unique-in-one-frame
does not generalise" is exactly the trap that killed the index-count idea, so
this re-runs the same sweep on cotw_frame18616.rdc - a different session,
different scene - WITHOUT being told where the viewmodel pass is.

If the predicate is real, the draws it selects in this capture should be a
contiguous late block whose index counts are the known weapon meshes.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ds_crosscap.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame18616.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ds_crosscap.txt")

KNOWN_MESHES = {77484, 5007, 162, 6087, 5958, 10272, 3252, 2484, 9594}

out = open(OUT, "w")


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

    hits = []          # readMask == 0x40
    refhits = []       # ref & 0x40
    combo = {}
    for i, a in enumerate(draws):
        controller.SetFrameEvent(a.eventId, False)
        ds = controller.GetD3D11PipelineState().outputMerger.depthStencilState
        f = ds.frontFace
        rd_m, wr_m, ref = int(f.compareMask), int(f.writeMask), int(f.reference)
        dfunc = str(ds.depthFunction).split(".")[-1].split(":")[0]
        dw = int(bool(ds.depthWrites))
        sfun = str(f.function).split(".")[-1].split(":")[0]
        spass = str(f.passOperation).split(".")[-1].split(":")[0]
        k = (dfunc, dw, rd_m, wr_m, ref, sfun, spass)
        c = combo.setdefault(k, [0, []])
        c[0] += 1
        if len(c[1]) < 5:
            c[1].append(a.eventId)
        if rd_m == 0x40:
            hits.append((a.eventId, int(getattr(a, "numIndices", 0)),
                         dfunc, dw, rd_m, wr_m, ref, spass,
                         a.GetName(sf).split("(")[0][:36]))
        if ref & 0x40:
            refhits.append(a.eventId)
        if i % 200 == 0:
            say("  ... %d/%d (eid %d)" % (i, len(draws), a.eventId))

    say("")
    say("=" * 78)
    say("== every distinct depth-stencil signature in this capture ==")
    say("  %-12s %3s %5s %5s %5s %-10s %-9s %6s  sample eids"
        % ("depthFunc", "dW", "rdMsk", "wrMsk", "ref", "sFunc", "sPass", "n"))
    for k in sorted(combo, key=lambda k: -combo[k][0]):
        c = combo[k]
        mark = "  <== readMask 0x40" if k[2] == 0x40 else ""
        say("  %-12s %3d  0x%02X  0x%02X  0x%02X %-10s %-9s %6d  %s%s"
            % (k[0], k[1], k[2], k[3], k[4], k[5], k[6], c[0], c[1], mark))

    say("")
    say("== draws with stencil readMask == 0x40 ==")
    say("  %d of %d drawcalls (%.2f%%)"
        % (len(hits), len(draws), 100.0 * len(hits) / max(1, len(draws))))
    say("  %d of %d drawcalls have ref & 0x40" % (len(refhits), len(draws)))
    if hits:
        eids = [h[0] for h in hits]
        say("  eid span %d .. %d" % (min(eids), max(eids)))
        counts = [h[1] for h in hits]
        known = [c for c in counts if c in KNOWN_MESHES]
        say("  %d/%d of them have an index count from the 4317 weapon mesh set"
            % (len(known), len(counts)))
        say("  distinct index counts: %s" % sorted(set(counts)))
        say("")
        say("  %6s %8s  %-13s %-3s %-16s %-9s %s"
            % ("eid", "idx", "depthFunc", "dW", "rd/wr/ref", "pass", "name"))
        for h in hits:
            say("  %6d %8d  %-13s %-3d 0x%02X/0x%02X/0x%02X      %-9s %s%s"
                % (h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8],
                   "" if h[1] in KNOWN_MESHES else "   (new mesh)"))

    # anything else late in the frame that readMask 0x40 would MISS
    if hits:
        lo, hi = min(h[0] for h in hits), max(h[0] for h in hits)
        say("")
        say("== all drawcalls inside the selected span %d..%d ==" % (lo, hi))
        sel = set(h[0] for h in hits)
        for a in draws:
            if lo <= a.eventId <= hi:
                say("  %6d %8d  %s%s"
                    % (a.eventId, int(getattr(a, "numIndices", 0)),
                       a.GetName(sf).split("(")[0][:40],
                       "" if a.eventId in sel else "   <== NOT SELECTED"))

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
