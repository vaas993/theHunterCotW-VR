"""Capture B: locate the viewmodel WITHOUT assuming an eid window, then re-score.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_ib_crosscap.py

The IB claim's capture-B replication located the block by searching for the
index counts already known from capture A - i.e. it assumed the answer to find
the pass, then reported the pass's index buffers as a discovery.  This locates
candidate first-person draws blind, from an intrinsic pipeline tag only, and
then asks the questions that matter for the IB rule:

  1. Does capture A's learned pointer set transfer to capture B at all?
  2. Is the capture-B mesh set the same as capture A's?
  3. Are there tagged draws OUTSIDE the contiguous window the IB rule assumed
     (which the window-derived pointer set would therefore have missed)?

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ib_crosscap.txt
"""
import os
import time
import traceback
from collections import defaultdict

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame18616.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ib_crosscap.txt")

# learned in capture A - resource ids and mesh index counts
A_IB_SET = {"71664", "71666", "90682", "90985", "90987",
            "91142", "91325", "91326", "91346"}
A_COUNTS = {77484, 5007, 162, 6087, 5958, 3252, 2484, 10272, 9594}

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def rid(r):
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

    blen = {}
    for b in controller.GetBuffers():
        blen[rid(b.resourceId)] = int(b.length)

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
    say("%d drawcalls, eids %d..%d"
        % (len(draws), draws[0].eventId, draws[-1].eventId))

    recs = []
    errs = 0
    for i, a in enumerate(draws):
        eid = a.eventId
        try:
            controller.SetFrameEvent(eid, False)
            d3d = controller.GetD3D11PipelineState()
            ia = d3d.inputAssembly
            ib = rid(getattr(ia.indexBuffer, "resourceId", None))
            dss = d3d.outputMerger.depthStencilState
            ff = dss.frontFace
            ref = int(getattr(ff, "reference", 0))
            cm = int(getattr(ff, "compareMask", 0))
            se = bool(getattr(dss, "stencilEnable", False))
            nm = a.GetName(sf)
            api = nm.split("(")[0] if nm else "?"
            recs.append(dict(eid=eid, api=api, ib=ib,
                             n=int(getattr(a, "numIndices", 0)),
                             ref=ref, cm=cm, se=se))
        except Exception:
            errs += 1
        if i % 400 == 0:
            say("  ...%d/%d (%.1fs)" % (i, len(draws), time.time() - t0))
    say("swept %d, %d errors" % (len(recs), errs))

    DI = "ID3D11DeviceContext::DrawIndexed"

    # ---- locate the viewmodel blind, from the intrinsic tag only ----------
    tagged = [r for r in recs if r["se"] and r["cm"] == 0x40
              and (r["ref"] & 0x40)]
    say("")
    say("== blind location: draws whose stencil says first-person ==")
    say("  %d of %d drawcalls tagged" % (len(tagged), len(recs)))
    if not tagged:
        say("  NONE - the intrinsic tag does not generalise. Stop.")
        controller.Shutdown()
        cap.Shutdown()
        return
    eids = [r["eid"] for r in tagged]
    say("  eid span %d..%d" % (min(eids), max(eids)))
    others = [r for r in recs if min(eids) <= r["eid"] <= max(eids)
              and not (r["se"] and r["cm"] == 0x40 and (r["ref"] & 0x40))]
    say("  untagged draws interleaved inside that span: %d" % len(others))
    for r in others[:10]:
        say("     eid %d %s n=%d" % (r["eid"], r["api"], r["n"]))
    say("  entry points of tagged draws: %s"
        % dict((k, sum(1 for r in tagged if r["api"] == k))
               for k in set(r["api"] for r in tagged)))

    B_IB = sorted(set(r["ib"] for r in tagged))
    B_COUNTS = sorted(set(r["n"] for r in tagged))
    say("")
    say("== capture B viewmodel, as found blind ==")
    say("  %d distinct index buffers: %s" % (len(B_IB), B_IB))
    say("  %d distinct index counts : %s" % (len(B_COUNTS), B_COUNTS))
    per = defaultdict(list)
    for r in tagged:
        per[r["ib"]].append(r["eid"])
    for k in sorted(per, key=lambda k: -len(per[k])):
        say("     ib %-7s rounds=%d eids=%s" % (k, len(per[k]), per[k]))

    # ---- Q1: does capture A's learned set transfer? -----------------------
    say("")
    say("== Q1: does the pointer set learned in capture A transfer? ==")
    say("  capture A index buffers : %s" % sorted(A_IB_SET))
    say("  capture B index buffers : %s" % B_IB)
    say("  intersection            : %s" % sorted(A_IB_SET & set(B_IB)))
    hitB = sum(1 for r in tagged if r["ib"] in A_IB_SET)
    say("  capture-A set applied to capture B: %d/%d viewmodel draws matched"
        % (hitB, len(tagged)))
    stale = [r for r in recs if r["ib"] in A_IB_SET]
    say("  draws in capture B binding a capture-A viewmodel id: %d" % len(stale))
    for r in stale[:20]:
        istag = (r["se"] and r["cm"] == 0x40 and (r["ref"] & 0x40))
        say("     eid %-6d %-40s n=%-6d ib=%-7s firstperson=%s"
            % (r["eid"], r["api"], r["n"], r["ib"], istag))
    say("  -> of those, WORLD draws wrongly matched by the stale set: %d"
        % sum(1 for r in stale
              if not (r["se"] and r["cm"] == 0x40 and (r["ref"] & 0x40))))

    # ---- Q2: is the mesh set the same? ------------------------------------
    say("")
    say("== Q2: is the viewmodel mesh set stable between sessions? ==")
    say("  capture A counts: %s" % sorted(A_COUNTS))
    say("  capture B counts: %s" % B_COUNTS)
    say("  in A only       : %s" % sorted(A_COUNTS - set(B_COUNTS)))
    say("  in B only       : %s" % sorted(set(B_COUNTS) - A_COUNTS))
    say("  shared          : %s" % sorted(A_COUNTS & set(B_COUNTS)))

    # ---- Q3: tagged draws outside the assumed window ----------------------
    WIN_LO, WIN_HI = 31910, 32568          # the window the IB claim assumed
    say("")
    say("== Q3: the eid window the IB claim assumed for capture B ==")
    say("  window %d..%d" % (WIN_LO, WIN_HI))
    outside_tagged = [r for r in tagged
                      if not (WIN_LO <= r["eid"] <= WIN_HI)]
    say("  first-person draws OUTSIDE that window: %d" % len(outside_tagged))
    for r in outside_tagged[:20]:
        say("     eid %-6d n=%-6d ib=%s" % (r["eid"], r["n"], r["ib"]))
    win_ib = set(r["ib"] for r in recs if WIN_LO <= r["eid"] <= WIN_HI)
    missed = sum(1 for r in tagged if r["ib"] not in win_ib)
    say("  viewmodel draws a window-derived IB set would MISS: %d/%d"
        % (missed, len(tagged)))

    # ---- scoring both rules on capture B ----------------------------------
    say("")
    say("== both rules scored on capture B (pass = the tagged draws) ==")
    tset = set(r["eid"] for r in tagged)
    inp = [r for r in recs if r["eid"] in tset]
    outp = [r for r in recs if r["eid"] not in tset]
    di_out = [r for r in outp if r["api"] == DI]
    S = set(r["ib"] for r in inp)
    say("  population: %d total, %d viewmodel, %d world (%d DrawIndexed)"
        % (len(recs), len(inp), len(outp), len(di_out)))

    def score(label, pred):
        h = sum(1 for r in inp if pred(r))
        f = [r for r in outp if pred(r)]
        say("  %-48s hits %2d/%-3d  FP %4d/%-5d (%3d DrawIndexed)"
            % (label, h, len(inp), len(f), len(outp),
               sum(1 for r in f if r["api"] == DI)))

    score("index-buffer set learned IN capture B", lambda r: r["ib"] in S)
    score("index-buffer set learned in capture A", lambda r: r["ib"] in A_IB_SET)
    score("intrinsic stencil tag (no learning)",
          lambda r: r["se"] and r["cm"] == 0x40 and (r["ref"] & 0x40))
    score("index count in capture A's mesh list",
          lambda r: r["n"] in A_COUNTS)

    say("")
    say("DONE %.1fs" % (time.time() - t0))
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    say("EXCEPTION:")
    say(traceback.format_exc())

out.close()
os._exit(0)
