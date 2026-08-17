"""Is OMGetRenderTargets(1, ...) enough, or must the hook query all 8 slots?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_rtv_dsv5.py

The candidate discriminator is "no render target bound AND the main-scene depth
bound".  The cheap way to test it in a DrawIndexed hook is
OMGetRenderTargets(1, &rtv0, &dsv) - two AddRefs instead of nine.  That is only
equivalent to "no render target bound" if no draw in the frame ever leaves slot
0 null while binding a higher slot.  D3D11 apps normally bind from slot 0, but
normally is not a measurement.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_rtv_dsv5.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_rtv_dsv5.txt")

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

    # ActionDescription.outputs is indexed BY SLOT, so a hole shows up directly.
    holes = []
    patterns = {}
    for a in draws:
        outs = list(a.outputs)
        occupied = tuple(i for i, o in enumerate(outs)
                         if o != rd.ResourceId.Null())
        patterns[occupied] = patterns.get(occupied, 0) + 1
        if occupied and occupied[0] != 0:
            holes.append((a.eventId, occupied))

    say("")
    say("== which OM slots are occupied, over all %d draws ==" % len(draws))
    for pat, n in sorted(patterns.items(), key=lambda kv: -kv[1]):
        say("   slots %-24s %5d draws" % (str(pat) if pat else "(none)", n))
    say("")
    say("draws that bind a slot > 0 while slot 0 is NULL: %d" % len(holes))
    for eid, pat in holes[:30]:
        say("   eid %d slots %s" % (eid, pat))
    say("")
    say("=> OMGetRenderTargets(1, &rtv0, &dsv) is %s"
        % ("SUFFICIENT - slot 0 is null only when every slot is null"
           if not holes else "NOT sufficient - query all 8"))

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
