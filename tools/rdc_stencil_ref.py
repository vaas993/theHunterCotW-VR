"""Hard per-draw counts for the stencil reference value and read mask.

Run headlessly:
    qrenderdoc.exe --python rdc_stencil_ref.py

rdc_state_descs showed the three depth-stencil state objects used by the
viewmodel pass all carry stencil readMask 0x40 and stencil refs 0x44 / 0x40 /
0xD6, and that NO other depth-stencil state in the frame uses read mask 0x40.
That was counted over distinct state objects.  This counts it over DRAWS, so the
recall / precision numbers are unarguable, and tests the exact runtime predicate
    OMGetDepthStencilState(&pDS, &ref);  if (ref & 0x40) -> viewmodel
which costs one context call and needs no GetDesc and no pointer fingerprint.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_stencil_ref.txt
"""
import os
import traceback
from collections import Counter, defaultdict

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_stencil_ref.txt")

BLOCK_LO, BLOCK_HI = 33313, 33956

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
    N_IN = sum(1 for a in draws if BLOCK_LO <= a.eventId <= BLOCK_HI)
    N_OUT = len(draws) - N_IN
    say("%d drawcalls: %d IN block, %d OUT" % (len(draws), N_IN, N_OUT))

    ref_c = defaultdict(lambda: [0, 0, []])
    mask_c = defaultdict(lambda: [0, 0, []])
    bit_c = defaultdict(lambda: [0, 0, []])
    stencil_on = defaultdict(lambda: [0, 0, []])

    for a in draws:
        eid = a.eventId
        i = 0 if BLOCK_LO <= eid <= BLOCK_HI else 1
        controller.SetFrameEvent(eid, True)
        ds = controller.GetD3D11PipelineState().outputMerger.depthStencilState
        ref = int(ds.frontFace.reference)
        msk = int(ds.frontFace.compareMask)
        on = bool(ds.stencilEnable)
        ref_c[ref][i] += 1
        mask_c[msk][i] += 1
        bit_c[bool(ref & 0x40)][i] += 1
        stencil_on[on][i] += 1
        if i == 1 and (ref & 0x40):
            ref_c[ref][2].append(eid)
            bit_c[True][2].append(eid)

    def table(name, d, fmt="%s"):
        say("")
        say("-- %s --" % name)
        for k in sorted(d, key=lambda k: -d[k][0]):
            ci, co = d[k][0], d[k][1]
            mark = "   <<< EXCLUSIVE to the viewmodel block" if ci and not co \
                   else ""
            say("   %-12s IN %4d (%5.1f%%)   OUT %5d (%5.1f%%)%s"
                % (fmt % k, ci, 100.0 * ci / N_IN, co, 100.0 * co / N_OUT,
                   mark))
            if d[k][2]:
                say("        OUT eids: %s" % d[k][2][:20])

    table("OM stencil reference value (OMGetDepthStencilState 2nd out-param)",
          ref_c, "0x%02X")
    table("stencil read/compare mask (from GetDesc)", mask_c, "0x%02X")
    table("stencilEnable", stencil_on)
    table("PREDICATE  (stencilRef & 0x40) != 0", bit_c)

    say("")
    say("== verdict for (stencilRef & 0x40) != 0 ==")
    t = bit_c[True]
    say("   true on %d of %d viewmodel draws   (recall %.1f%%)"
        % (t[0], N_IN, 100.0 * t[0] / N_IN))
    say("   true on %d of %d world draws       (false positives %d)"
        % (t[1], N_OUT, t[1]))
    say("   false negatives: %d" % (N_IN - t[0]))
    if t[0] + t[1]:
        say("   precision %.1f%%" % (100.0 * t[0] / (t[0] + t[1])))

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
