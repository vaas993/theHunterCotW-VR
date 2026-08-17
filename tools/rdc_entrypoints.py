"""Safety check for the index-count depth matcher.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_entrypoints.py

The viewmodel's depth rounds are matched by "no pixel shader bound + an index
count the colour round taught us".  That is only safe if no OTHER depth-only
draw on the SAME entry point can collide.  The world's depth prepass also
draws with no pixel shader, so this lists every plain-DrawIndexed, PS-less
draw in the frame with its index count - if the prepass reaches the GPU
through the instanced/indirect path instead, the hook never sees it and the
question is moot.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_entrypoints.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_entrypoints.txt")

WEAPON_IDX = {77484, 5007, 162, 6087, 3252, 2484, 4992, 5748, 5958, 9594}

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

    # Every plain DrawIndexed in the frame, with whether a PS is bound.
    say("")
    say("== plain DrawIndexed draws, by pixel-shader presence ==")
    psless = []
    withps = 0
    other_entry = {}
    for a in leaves:
        if not (a.flags & rd.ActionFlags.Drawcall):
            continue
        name = a.GetName(sf)
        entry = name.split("(")[0].replace("ID3D11DeviceContext::", "")
        other_entry[entry] = other_entry.get(entry, 0) + 1
        if entry != "DrawIndexed":
            continue
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        ps = pipe.GetShader(rd.ShaderStage.Pixel)
        if ps == rd.ResourceId.Null():
            psless.append((a.eventId, a.numIndices))
        else:
            withps += 1

    say("entry points used in this frame:")
    for k, v in sorted(other_entry.items(), key=lambda kv: -kv[1]):
        say("   %-40s %d" % (k, v))

    say("")
    say("plain DrawIndexed with a PS bound: %d" % withps)
    say("plain DrawIndexed with NO PS bound: %d" % len(psless))
    say("")
    say("  eid     indices   collides with a weapon mesh?")
    collisions = 0
    for eid, idx in psless:
        hit = idx in WEAPON_IDX
        inpass = 31900 <= eid <= 32600
        if hit and not inpass:
            collisions += 1
        say("  %-7d %-9d %s" % (eid, idx,
                                ("WEAPON MESH - in viewmodel pass" if hit and inpass
                                 else "*** COLLISION - OUTSIDE THE PASS ***" if hit
                                 else "")))
    say("")
    if collisions:
        say("VERDICT: %d PS-less draw(s) outside the viewmodel pass share an index "
            "count with a weapon mesh - the index matcher WOULD shift them."
            % collisions)
    else:
        say("VERDICT: SAFE - no PS-less plain-DrawIndexed draw outside the viewmodel "
            "pass shares an index count with any weapon mesh.")

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
