"""False-negative hunt by GEOMETRY IDENTITY, not by index count.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_refute_ds3.py

If the gun/hands are also rendered somewhere else in the frame - a shadow map,
a reflection, a depth-only occlusion pass - and that other draw does NOT carry
the stencil tag, the mod would shift the main pass and leave a mismatched copy
behind. Index count alone cannot tell "same mesh" from "coincidence", so this
keys on the actual (index buffer, first index, index count, vertex buffer)
tuple: the same mesh drawn twice from the same buffers is the same geometry.

Also lists everything drawn AFTER the viewmodel pass (HUD / reticle / post),
which is where viewmodel-adjacent content that must move with the weapon would
live if it exists.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_refute3_<tag>.txt
"""
import os
import traceback

import renderdoc as rd

CAPNAME = os.environ.get("REFUTE_CAP", "cotw_frame4317.rdc")
TAG = CAPNAME.replace("cotw_frame", "").replace(".rdc", "")
CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\\" + CAPNAME)
OUT = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute3_" + TAG + ".txt")

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def main():
    cap = rd.OpenCaptureFile()
    say("capture: %s" % CAP)
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return

    sf = controller.GetStructuredFile()
    leaves = []

    def walk(lst):
        for a in lst:
            kids = list(getattr(a, "children", []))
            if kids:
                walk(kids)
            else:
                leaves.append(a)

    walk(controller.GetRootActions())
    draws = sorted([a for a in leaves if a.flags & rd.ActionFlags.Drawcall],
                   key=lambda a: a.eventId)
    say("%d drawcalls" % len(draws))

    recs = []
    for i, a in enumerate(draws):
        controller.SetFrameEvent(a.eventId, False)
        d3d = controller.GetD3D11PipelineState()
        f = d3d.outputMerger.depthStencilState.frontFace
        ib = d3d.inputAssembly.indexBuffer
        try:
            ibid = str(ib.resource)
            iboff = int(ib.byteOffset)
        except Exception:
            ibid, iboff = "?", -1
        vbs = []
        try:
            for vb in d3d.inputAssembly.vertexBuffers[:2]:
                vbs.append("%s+%d" % (str(vb.resourceId), int(vb.byteOffset)))
        except Exception:
            pass
        nm = a.GetName(sf)
        recs.append(dict(
            eid=a.eventId,
            entry=nm.split("(")[0].replace("ID3D11DeviceContext::", ""),
            n=int(getattr(a, "numIndices", 0)),
            first=int(getattr(a, "indexOffset", 0)),
            basev=int(getattr(a, "baseVertex", 0)),
            ib=ibid, iboff=iboff, vb="|".join(vbs),
            rd_=int(f.compareMask), ref=int(f.reference),
            tag=(f.compareMask == 0x40 and (f.reference & 0x40) != 0)))
        if i % 400 == 0:
            say("  ... %d/%d" % (i, len(draws)))

    tagged = [r for r in recs if r["tag"]]
    say("")
    say("tagged draws: %d, eid %d..%d"
        % (len(tagged), tagged[0]["eid"], tagged[-1]["eid"]) if tagged
        else "no tagged draws")

    # ---- geometry identity: same (ib, first, count) drawn anywhere else -----
    say("")
    say("=" * 78)
    say("== the SAME GEOMETRY drawn outside the tagged pass? ==")
    keys = set((r["ib"], r["iboff"], r["first"], r["n"]) for r in tagged)
    say("   %d distinct (indexBuffer, bufOffset, firstIndex, count) keys in "
        "the tagged pass" % len(keys))
    elsewhere = [r for r in recs
                 if not r["tag"]
                 and (r["ib"], r["iboff"], r["first"], r["n"]) in keys]
    say("   draws with an identical key OUTSIDE the tagged pass: %d"
        % len(elsewhere))
    for r in elsewhere[:60]:
        say("      eid %-7d %-24s n=%-7d first=%-8d ib=%s rd=0x%02X ref=0x%02X"
            % (r["eid"], r["entry"], r["n"], r["first"], r["ib"],
               r["rd_"], r["ref"]))

    # looser: same index buffer at all
    say("")
    ibs = set(r["ib"] for r in tagged)
    say("   index buffers used by the tagged pass: %d" % len(ibs))
    for b in sorted(ibs):
        users = [r for r in recs if r["ib"] == b]
        t = sum(1 for r in users if r["tag"])
        say("      %-22s used by %4d draws (%d tagged, %d NOT tagged)"
            % (b, len(users), t, len(users) - t))
        if len(users) - t:
            for r in [x for x in users if not x["tag"]][:12]:
                say("           eid %-7d %-24s n=%-7d first=%-8d rd=0x%02X ref=0x%02X"
                    % (r["eid"], r["entry"], r["n"], r["first"],
                       r["rd_"], r["ref"]))

    # same for vertex buffers
    say("")
    vbset = set(r["vb"] for r in tagged if r["vb"])
    say("   vertex-buffer bindings used by the tagged pass: %d" % len(vbset))
    shared = [r for r in recs if not r["tag"] and r["vb"] in vbset]
    say("   NON-tagged draws sharing a tagged vertex-buffer binding: %d"
        % len(shared))
    for r in shared[:40]:
        say("      eid %-7d %-24s n=%-7d rd=0x%02X ref=0x%02X  vb=%s"
            % (r["eid"], r["entry"], r["n"], r["rd_"], r["ref"], r["vb"][:60]))

    # ---- what happens after the pass ---------------------------------------
    if tagged:
        last = tagged[-1]["eid"]
        after = [r for r in recs if r["eid"] > last]
        say("")
        say("=" * 78)
        say("== %d draws AFTER the viewmodel pass (HUD / reticle / post) =="
            % len(after))
        for r in after:
            say("   eid %-7d %-28s n=%-7d rd=0x%02X ref=0x%02X"
                % (r["eid"], r["entry"], r["n"], r["rd_"], r["ref"]))

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
