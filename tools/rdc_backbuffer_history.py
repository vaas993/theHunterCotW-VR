"""Ground truth pass 6: what draw actually paints the VISIBLE weapon pixels?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_backbuffer_history.py

Pass 5 found plain DrawIndexed events late in the frame (31987, 32125, 32513
- the last with PS FBECF3C0, one of the three original Shader Toggler CRCs!)
writing depth at the scope pixel AFTER post-processing.  If the visible rifle
is re-drawn forward after post, the whole picture changes again.  So ask the
backbuffer itself: PixelHistory at rifle pixels + full state of those late
draws.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_backbuffer_history.txt
"""
import os
import traceback
import zlib

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_backbuffer_history.txt")

PIXELS = [(1900, 2496), (1344, 2438), (1075, 2554)]
LATE_EIDS = [31987, 32125, 32513]

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
    by_eid = {a.eventId: a for a in leaves}

    textures = {str(t.resourceId): t for t in controller.GetTextures()}
    swap = [t for t in controller.GetTextures()
            if t.creationFlags & rd.TextureCategory.SwapBuffer]
    say("backbuffer: %s" % (swap[0].resourceId if swap else None))
    if not swap:
        return
    bb = swap[0].resourceId

    refl_cache = {}

    def crc_of(rid):
        key = str(rid)
        if key in refl_cache:
            return refl_cache[key]
        crc = None
        try:
            entries = controller.GetShaderEntryPoints(rid)
            if entries:
                refl = controller.GetShader(rd.ResourceId.Null(), rid,
                                            entries[0])
                crc = zlib.crc32(bytes(refl.rawBytes)) & 0xFFFFFFFF
        except Exception:
            pass
        refl_cache[key] = crc
        return crc

    # ---- backbuffer pixel history ----------------------------------------
    writers = set()
    for (x, y) in PIXELS:
        say("")
        say("== PixelHistory BACKBUFFER at (%d, %d) ==" % (x, y))
        try:
            hist = controller.PixelHistory(bb, x, y, rd.Subresource(0, 0, 0),
                                           rd.CompType.Typeless)
        except Exception as e:
            say("  failed: %s" % e)
            continue
        say("  %d modifications" % len(hist))
        for h in hist:
            a = by_eid.get(h.eventId)
            name = a.GetName(sf)[:50] if a else "?"
            passed = h.Passed() if hasattr(h, "Passed") else "?"
            say("   eid %-6d passed=%-5s %s" % (h.eventId, passed, name))
            if passed is True:
                writers.add(h.eventId)

    # ---- state at every passing writer + the late depth draws ------------
    say("")
    say("== state at backbuffer writers + late depth draws ==")
    for eid in sorted(writers | set(LATE_EIDS)):
        a = by_eid.get(eid)
        if a is None or not (a.flags & (rd.ActionFlags.Drawcall)):
            say("  eid %d: not a draw (%s)" %
                (eid, a.GetName(sf) if a else "?"))
            continue
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        vs = pipe.GetShader(rd.ShaderStage.Vertex)
        ps = pipe.GetShader(rd.ShaderStage.Pixel)
        vp = pipe.GetViewport(0)
        say("")
        say("  eid %-6d %s" % (eid, a.GetName(sf)))
        say("    numIndices=%d numInstances=%d" %
            (a.numIndices, a.numInstances))
        say("    VS %08X  PS %08X" % (crc_of(vs) or 0, crc_of(ps) or 0))
        say("    viewport x=%.0f y=%.0f w=%.0f h=%.0f z=%.3f..%.3f"
            % (vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth))
        # outputs
        for o in a.outputs:
            if o != rd.ResourceId.Null():
                t = textures.get(str(o))
                say("    out  %s %s" %
                    (o, "%dx%d %s" % (t.width, t.height, t.format.Name())
                     if t else ""))
        if a.depthOut != rd.ResourceId.Null():
            t = textures.get(str(a.depthOut))
            say("    depth %s %s" %
                (a.depthOut, "%dx%d %s" % (t.width, t.height,
                                           t.format.Name()) if t else ""))

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
