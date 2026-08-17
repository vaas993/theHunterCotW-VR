"""Third pass: how many of the RTV/DSV false positives is the hook even ABLE to see?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_rtv_dsv3.py

The mod hooks ID3D11DeviceContext::DrawIndexed.  A world draw that shares the
viewmodel's render targets but arrives through Draw(), DrawInstanced() or
DrawIndexedInstancedIndirect() is invisible to that hook and therefore is NOT a
false positive for this discriminator.  rdc_rtv_dsv2.py counted 80 / 393 / 132
sharers by raw draw count; this recounts them per D3D11 entry point so the
false-positive figure quoted to the mod is the one the mod will actually
experience.

Also records, per binding, whether the sharers precede the viewmodel pass, since
"which targets are bound" and "how many times they have been bound this frame"
are different discriminators and only the first is free.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_rtv_dsv3.txt
"""
import os
import re
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_rtv_dsv3.txt")

VM_LO, VM_HI = 33313, 33956
VM_DSV_RES = "ResourceId::18561"
VM_RT_A = "ResourceId::18616"
VM_RT_B = "ResourceId::18564"

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

    def entry(a):
        nm = a.GetName(sf)
        m = re.search(r"::(\w+)\s*\(", nm)
        return m.group(1) if m else nm[:40]

    # entry point census of the whole frame
    census = {}
    for a in draws:
        census[entry(a)] = census.get(entry(a), 0) + 1
    say("")
    say("== every draw entry point in the frame ==")
    for k, v in sorted(census.items(), key=lambda kv: -kv[1]):
        say("   %-34s %5d" % (k, v))

    def binding(a):
        outs = [str(o) for o in a.outputs if o != rd.ResourceId.Null()]
        d = str(a.depthOut)
        if d != VM_DSV_RES:
            return None
        if not outs:
            return "A: no RTV        + dsv 18561"
        if outs == [VM_RT_A]:
            return "B: rtv 18616     + dsv 18561"
        if outs == [VM_RT_B]:
            return "C: rtv 18564     + dsv 18561"
        return None

    say("")
    say("== the three viewmodel bindings, sharers split by entry point ==")
    for want in ("A: no RTV        + dsv 18561",
                 "B: rtv 18616     + dsv 18561",
                 "C: rtv 18564     + dsv 18561"):
        ins, outs_by_ep = [], {}
        for a in draws:
            if binding(a) != want:
                continue
            if VM_LO <= a.eventId <= VM_HI:
                ins.append(a.eventId)
            else:
                outs_by_ep.setdefault(entry(a), []).append(a.eventId)
        say("")
        say("-- %s --" % want)
        say("   in-range draws : %d  (all DrawIndexed: %s)"
            % (len(ins),
               all(entry(a) == "DrawIndexed" for a in draws
                   if a.eventId in ins)))
        tot_hookable = 0
        for ep, eids in sorted(outs_by_ep.items(), key=lambda kv: -len(kv[1])):
            hookable = ep == "DrawIndexed"
            if hookable:
                tot_hookable += len(eids)
            say("   outside via %-30s %5d  %s  eids %d..%d"
                % (ep, len(eids),
                   "<-- HOOK SEES THESE" if hookable else "(hook never sees)",
                   min(eids), max(eids)))
        say("   => false positives the DrawIndexed hook would actually take: %d"
            % tot_hookable)

    # Any binding at all that a DrawIndexed hook would see exclusively?
    say("")
    say("== restricted to DrawIndexed only: every (rtv set, dsv) combo ==")
    combos = {}
    for a in draws:
        if entry(a) != "DrawIndexed":
            continue
        key = (tuple(str(o) for o in a.outputs if o != rd.ResourceId.Null()),
               str(a.depthOut))
        s = combos.setdefault(key, [[], []])
        s[0 if VM_LO <= a.eventId <= VM_HI else 1].append(a.eventId)
    for (outs, d), (i, o) in sorted(combos.items(), key=lambda kv: -len(kv[1][0])):
        if not i and not o:
            continue
        mark = "  <<<< EXCLUSIVE" if i and not o else ""
        say("   in=%-4d out=%-5d rtv=%s dsv=%s%s"
            % (len(i), len(o), list(outs) or "(none)", d, mark))

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
