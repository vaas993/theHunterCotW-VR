"""PISTOL-SCOPE ADS, part 2: where the scope picture comes from, and what a
DrawIndexed hook can see at draw time.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_pistol_rtt.py

Open questions after rdc_pistol_lens.py:
  a) the lens pixel shader (PS 1320, eid 16549) samples ResourceId::2522,
     3854x2800 R11G11B10_FLOAT - the same size and format as the main scene
     colour 622 - but 2522 is never an RTV in this frame.  So it is filled by a
     COPY, not by drawing.  Confirm by listing every non-draw action.
  b) the 128x128 chain at eid 15175..15395 renders into its own textures right
     before the viewmodel pass.  Identify it so the rule can be shown to reject
     it.
  c) which draws in the whole frame bind constant buffer ResourceId::144 (the
     192-byte InstanceConsts the mask and the glass share) - cbscan.cpp skips
     buffers < 256 bytes, so this decides whether a CB patch is even possible.
  d) the mask mesh: solid disc or annulus?  Measured from post-VS positions.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_pistol_rtt.txt
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.environ.get("COTW_CAP") or os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame33848.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_pistol_rtt.txt")

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

    tex = {}
    for t in controller.GetTextures():
        tex[str(t.resourceId)] = "%dx%d %s" % (t.width, t.height,
                                               t.format.Name())
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
    leaves.sort(key=lambda a: a.eventId)
    draws = [a for a in leaves if a.flags & rd.ActionFlags.Drawcall]
    byeid = {a.eventId: a for a in draws}

    # ---------------------------------------------------------------- a ----
    say("")
    say("=" * 78)
    say("== a. every COPY / CLEAR / DISPATCH between the world pass and the ")
    say("      viewmodel pass (eid 14000..17000) ==")
    for a in leaves:
        if not (14000 <= a.eventId <= 17000):
            continue
        fl = a.flags
        if fl & rd.ActionFlags.Drawcall:
            continue
        kinds = []
        for nm in ("Copy", "Clear", "Dispatch", "Resolve", "Present",
                   "SetMarker", "PushMarker", "PopMarker"):
            if fl & getattr(rd.ActionFlags, nm, 0):
                kinds.append(nm)
        if not kinds or kinds == ["SetMarker"]:
            continue
        say("   eid %-7d %-24s %s" % (a.eventId, ",".join(kinds),
                                      a.GetName(sf)[:110]))

    say("")
    say("   -- every action in the frame that mentions 2522 or 622 by name --")
    for a in leaves:
        n = a.GetName(sf)
        if "2522" in n or "Copy" in n:
            if not (13000 <= a.eventId <= 17000):
                continue
            say("   eid %-7d %s" % (a.eventId, n[:130]))

    say("")
    say("   -- who writes ResourceId::2522, per the resource usage list --")
    for s in ("2522", "622", "674"):
        rid = None
        for t in controller.GetTextures():
            if str(t.resourceId) == "ResourceId::" + s:
                rid = t.resourceId
                break
        if rid is None:
            say("   %s: not a texture" % s)
            continue
        try:
            us = controller.GetUsage(rid)
        except Exception as e:
            say("   %s: GetUsage RAISED %s" % (s, e))
            continue
        kinds = {}
        for u in us:
            k = str(u.usage).split(".")[-1]
            kinds.setdefault(k, []).append(u.eventId)
        say("   ResourceId::%s  %s" % (s, tex.get("ResourceId::" + s, "?")))
        for k in sorted(kinds, key=lambda k: -len(kinds[k])):
            v = kinds[k]
            say("      %-28s %4d  eids %s%s"
                % (k, len(v), v[:10], " ..." if len(v) > 10 else ""))

    # ---------------------------------------------------------------- b ----
    say("")
    say("=" * 78)
    say("== b. the 128x128 chain at eid 15175..15395 ==")
    for eid in (15175, 15195, 15215, 15235):
        if eid not in byeid:
            continue
        a = byeid[eid]
        controller.SetFrameEvent(eid, True)
        p = controller.GetPipelineState()
        d3d = controller.GetD3D11PipelineState()
        outs = [str(o) for o in a.outputs if o != rd.ResourceId.Null()]
        say("   eid %-7d %s" % (eid, a.GetName(sf)[:80]))
        say("      RTV %s" % [(o, tex.get(o, "?")) for o in outs])
        try:
            for ud in p.GetReadOnlyResources(rd.ShaderStage.Pixel):
                d = getattr(ud, "descriptor", None)
                if d is None:
                    continue
                r = getattr(d, "resource", None)
                if r is None or r == rd.ResourceId.Null():
                    continue
                say("      SRV %s %s" % (str(r), tex.get(str(r), "?")))
        except Exception as e:
            say("      SRV RAISED %s: %s" % (type(e).__name__, e))
        vp = None
        for v in d3d.rasterizer.viewports:
            if v.enabled and (v.width or v.height):
                vp = (v.x, v.y, v.width, v.height)
                break
        say("      viewport %s   DSV %s" % (vp, str(a.depthOut)))

    # ---------------------------------------------------------------- c ----
    say("")
    say("=" * 78)
    say("== c. every draw in the frame binding constant buffer 144 to the VS ==")
    hits = []
    for a in draws:
        controller.SetFrameEvent(a.eventId, False)
        d3d = controller.GetD3D11PipelineState()
        try:
            cbs = d3d.vertexShader.constantBuffers
        except Exception:
            continue
        for si, cb in enumerate(cbs):
            if str(getattr(cb, "resourceId", "")) == "ResourceId::144":
                hits.append((a.eventId, si, int(getattr(a, "numIndices", 0)),
                             str(d3d.vertexShader.resourceId),
                             str(d3d.pixelShader.resourceId)))
                break
    say("   %d draws bind ResourceId::144 (192 bytes) as a VS constant buffer"
        % len(hits))
    for eid, slot, idx, vs, ps in hits:
        say("      eid %-7d b%d  idx %-7d vs %-16s ps %s" % (eid, slot, idx, vs, ps))

    # ---------------------------------------------------------------- d ----
    say("")
    say("=" * 78)
    say("== d. shape of the mask mesh (post-VS, screen space) ==")
    for eid in (15852, 15855, 16549, 16558):
        if eid not in byeid:
            continue
        controller.SetFrameEvent(eid, True)
        pv = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        if pv is None or pv.vertexResourceId == rd.ResourceId.Null():
            say("   eid %d no post-VS" % eid)
            continue
        st = int(pv.vertexByteStride)
        n = min(int(getattr(pv, "numIndices", 0)) or 1584, 4000)
        raw = bytes(controller.GetBufferData(pv.vertexResourceId,
                                             int(pv.vertexByteOffset), st * n))
        pts = []
        for k in range(n):
            o = k * st
            if o + 16 > len(raw):
                break
            x, y, z, w = struct.unpack_from("<4f", raw, o)
            if w and w == w:
                pts.append(((x / w * 0.5 + 0.5) * 3854.0,
                            (0.5 - y / w * 0.5) * 2800.0))
        if not pts:
            continue
        cx = sum(p[0] for p in pts) / len(pts)
        cy = sum(p[1] for p in pts) / len(pts)
        rs = sorted(math.hypot(p[0] - cx, p[1] - cy) for p in pts)
        say("   eid %-7d %d verts, centre (%.0f, %.0f) px" % (eid, len(pts), cx, cy))
        say("      radius min %.1f  p10 %.1f  median %.1f  p90 %.1f  max %.1f px"
            % (rs[0], rs[len(rs) // 10], rs[len(rs) // 2],
               rs[len(rs) * 9 // 10], rs[-1]))
        inner = sum(1 for r in rs if r < rs[-1] * 0.5)
        say("      %d of %d verts inside half the outer radius -> %s"
            % (inner, len(rs),
               "SOLID DISC (fan centre present)" if inner <= 3
               else "ANNULUS / RING or a filled mesh"))

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
