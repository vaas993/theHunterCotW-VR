"""Map the captured frame's anatomy - ground truth pass 2.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_frame_anatomy.py

Produces:
  1. PNG of the final backbuffer - settles whether a weapon was even on
     screen in this capture (pass 1 found the three weapon shaders present
     but bound to ZERO draws).
  2. Pass structure: consecutive draw ranges grouped by (render targets,
     depth target), with target sizes/formats - the frame has no markers,
     so this is the only pass skeleton available.
  3. Which pixel shaders are actually USED, per event - via GetUsage per
     shader (precomputed by RenderDoc, cheap), never per-draw replay.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_anatomy.txt + backbuffer PNG.
"""
import os
import traceback
import zlib

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_anatomy.txt")
PNG = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_backbuffer.png")

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def main():
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(CAP, "", None)
    say("OpenFile -> %s" % res)
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open")

    sf = controller.GetStructuredFile()
    actions = controller.GetRootActions()

    leaves = []

    def walk(lst):
        for a in lst:
            kids = getattr(a, "children", [])
            if kids:
                walk(kids)
            else:
                leaves.append(a)

    walk(actions)
    say("%d leaf actions, eid range %d..%d"
        % (len(leaves), leaves[0].eventId, leaves[-1].eventId))

    textures = {t.resourceId: t for t in controller.GetTextures()}

    # ---- 1. save the final backbuffer -----------------------------------
    say("")
    say("== backbuffer ==")
    swap = [t for t in controller.GetTextures()
            if t.creationFlags & rd.TextureCategory.SwapBuffer]
    for t in swap:
        say("  swapchain tex %s  %dx%d  %s" %
            (t.resourceId, t.width, t.height, t.format.Name()))
    if swap:
        controller.SetFrameEvent(leaves[-1].eventId, True)
        ts = rd.TextureSave()
        ts.resourceId = swap[0].resourceId
        ts.destType = rd.FileType.PNG
        ts.mip = 0
        ok = controller.SaveTexture(ts, PNG)
        say("  saved -> %s : %s" % (PNG, ok))

    # ---- 2. pass structure from action outputs --------------------------
    say("")
    say("== pass structure (grouped by outputs+depth) ==")

    def texname(rid):
        t = textures.get(rid)
        if t is None:
            return str(rid)
        return "%s %dx%d %s" % (rid, t.width, t.height, t.format.Name())

    segs = []
    cur_key = None
    for a in leaves:
        if not (a.flags & (rd.ActionFlags.Drawcall | rd.ActionFlags.Dispatch)):
            continue
        outs = tuple(o for o in a.outputs if o != rd.ResourceId.Null())
        key = (outs, a.depthOut, bool(a.flags & rd.ActionFlags.Dispatch))
        if key != cur_key:
            segs.append([a.eventId, a.eventId, key, 0])
            cur_key = key
        segs[-1][1] = a.eventId
        segs[-1][3] += 1

    say("%d segments" % len(segs))
    for first, last, (outs, depth, is_disp), n in segs:
        kind = "DISPATCH" if is_disp else "draw"
        say("  eid %5d..%5d  %4d %s" % (first, last, n, kind))
        for o in outs:
            say("      out  %s" % texname(o))
        if depth != rd.ResourceId.Null():
            say("      depth %s" % texname(depth))

    # ---- 3. which pixel shaders are used, and where ----------------------
    say("")
    say("== pixel shader usage ==")
    resources = controller.GetResources()
    ps_events = {}          # crc -> sorted eids
    crc_of = {}
    used_ps = 0
    for r in resources:
        if r.type != rd.ResourceType.Shader:
            continue
        try:
            entries = controller.GetShaderEntryPoints(r.resourceId)
            if not entries:
                continue
            refl = controller.GetShader(rd.ResourceId.Null(), r.resourceId,
                                        entries[0])
        except Exception:
            continue
        if refl.stage != rd.ShaderStage.Pixel:
            continue
        raw = bytes(refl.rawBytes)
        crc = zlib.crc32(raw) & 0xFFFFFFFF
        crc_of[r.resourceId] = crc
        usage = [u for u in controller.GetUsage(r.resourceId)
                 if u.usage != rd.ResourceUsage.Unused]
        if not usage:
            continue
        used_ps += 1
        eids = sorted(u.eventId for u in usage)
        ps_events.setdefault(crc, []).extend(eids)

    say("pixel shaders USED this frame: %d distinct CRCs (%d objects)"
        % (len(ps_events), used_ps))

    # eid -> crc map, to label the frame timeline
    eid_ps = {}
    for crc, eids in ps_events.items():
        for e in eids:
            eid_ps.setdefault(e, set()).add(crc)

    say("")
    say("== per-draw PS timeline (eid: crc) ==")
    for a in leaves:
        if not (a.flags & rd.ActionFlags.Drawcall):
            continue
        crcs = eid_ps.get(a.eventId)
        label = " ".join("%08X" % c for c in sorted(crcs)) if crcs else "-"
        say("  %5d %-40s %s" % (a.eventId, a.GetName(sf)[:40], label))

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
