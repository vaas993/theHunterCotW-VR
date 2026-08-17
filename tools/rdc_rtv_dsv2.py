"""Follow-up: exactly WHO else uses the viewmodel's render targets.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_rtv_dsv2.py

rdc_rtv_dsv.py established that the viewmodel pass binds nothing of its own:
  depth round  -> no RTV,        DSV view 18562  (+80 world draws)
  round 2      -> RTV view 18617, DSV view 18562  (+393 world draws)
  colour round -> RTV view 18565, DSV view 18562  (+132 world draws)

That is already a NO for "an RTV the viewmodel owns".  This pass asks the two
follow-up questions that decide whether the dimension is worth anything at all:

  1. WHERE do those world users sit?  If every one of them is finished long
     before 33313, "RTV 18565 bound" is still not a discriminator on its own,
     but the report should say so precisely rather than hand-wave.
  2. Do the OMSetRenderTargets calls around the pass boundary show the engine
     rebinding anything - a fresh view object, a different slot count, a
     read-only depth flag - that a hook could see?

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_rtv_dsv2.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_rtv_dsv2.txt")

VM_LO, VM_HI = 33313, 33956

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def runs(eids):
    """Collapse a sorted eid list into contiguous-ish blocks for reading."""
    if not eids:
        return []
    blocks = [[eids[0], eids[0], 1]]
    for e in eids[1:]:
        if e - blocks[-1][1] <= 200:
            blocks[-1][1] = e
            blocks[-1][2] += 1
        else:
            blocks.append([e, e, 1])
    return blocks


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

    # resource ids behind the three viewmodel bindings
    VM_DSV_RES = "ResourceId::18561"
    VM_RT_A = "ResourceId::18616"      # R16G16, round 2
    VM_RT_B = "ResourceId::18564"      # R11G11B10, colour round

    groups = {"dsv-only 18561": [], "rt 18616 + dsv": [], "rt 18564 + dsv": []}
    for a in draws:
        outs = [str(o) for o in a.outputs if o != rd.ResourceId.Null()]
        d = str(a.depthOut)
        if d != VM_DSV_RES:
            continue
        if not outs:
            groups["dsv-only 18561"].append(a.eventId)
        elif outs == [VM_RT_A]:
            groups["rt 18616 + dsv"].append(a.eventId)
        elif outs == [VM_RT_B]:
            groups["rt 18564 + dsv"].append(a.eventId)

    say("")
    say("== where the OTHER users of each viewmodel binding live ==")
    for name, eids in groups.items():
        eids = sorted(eids)
        outside = [e for e in eids if not (VM_LO <= e <= VM_HI)]
        inside = [e for e in eids if VM_LO <= e <= VM_HI]
        say("")
        say("-- %s : %d in-range, %d outside --" % (name, len(inside), len(outside)))
        say("   contiguous blocks of the OUTSIDE users (gap>200 splits):")
        for lo, hi, n in runs(outside):
            say("      %6d..%-6d  %4d draws" % (lo, hi, n))
        after = [e for e in outside if e > VM_HI]
        before = [e for e in outside if e < VM_LO]
        say("   %d outside users BEFORE the pass, %d AFTER"
            % (len(before), len(after)))
        if before:
            say("   nearest one before the pass: eid %d (gap %d events)"
                % (before[-1], VM_LO - before[-1]))
        if after:
            say("   nearest one after the pass : eid %d" % after[0])

    # ---- the API calls that set the targets around the pass boundary ------
    say("")
    say("== OMSetRenderTargets-ish API calls near the pass boundary ==")
    sf = controller.GetStructuredFile()

    def chunkname(ev):
        try:
            return sf.chunks[ev.chunkIndex].name
        except Exception:
            return "?"

    # every action (not just draws) in a window around the pass
    for a in leaves:
        if not (VM_LO - 120 <= a.eventId <= VM_HI + 60):
            continue
        nm = a.GetName(sf)
        if a.flags & rd.ActionFlags.Drawcall:
            tag = "DRAW %d idx" % getattr(a, "numIndices", 0)
        elif a.flags & rd.ActionFlags.Clear:
            tag = "CLEAR"
        elif a.flags & rd.ActionFlags.Copy:
            tag = "COPY"
        else:
            tag = ""
        say("   %6d %-10s %s" % (a.eventId, tag, nm[:110]))

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
