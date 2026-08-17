"""Does the bit-0x40 mask actually BEHAVE like the invisible circle, and can one
shift serve the mask and the lens glass?

Run headlessly:
    set COTW_CAP=%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame33848.rdc
    set COTW_EIDS=15852,15855,16549,16558,16351,16374,16202,16080
    qrenderdoc.exe --python tools\rdc_ref40_geom.py

For each eid:
  * post-VS geometry: NDC and pixel bounding box, w range, centre, radius
    distribution - a filled disc pinned to the target centre looks different
    from a lens ring
  * the VS b-register 1 constant block: NAME, resource, REAL D3D length, bind
    offset, and the three 4x4 matrices at +0x00 / +0x40 / +0x80, printed in
    full so mask and glass can be compared value by value.  If they are the
    same bytes, one shift serves both and the mod's Unmap-time value patch
    already has a handle on them; if they differ, it does not.
  * GetUsage on that constant buffer, with the API call at every usage event -
    Map/Unmap versus UpdateSubresource decides whether the mod's existing
    value patch (which lives in Hook_Unmap only) can ever see the write.

RenderDoc 1.45: GetConstantBlocks, access.index is the descriptor POSITION
matched against the reflection index, descriptor fields copied immediately.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ref40_geom_<cap>.txt
"""
import os
import struct
import traceback

import renderdoc as rd

CAP = os.environ.get("COTW_CAP") or os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame33848.rdc")
TAG = os.path.splitext(os.path.basename(CAP))[0]
EIDS = [int(x) for x in (os.environ.get("COTW_EIDS") or
                         "15852,15855,16549,16558,16351,16374,16202,16080"
                         ).split(",") if x.strip()]
OUT = os.path.expandvars(
    r"%%LOCALAPPDATA%%\theHunterCotWVR\rdc_ref40_geom_%s.txt" % TAG)

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

    bufl = {}
    for b in controller.GetBuffers():
        bufl[str(b.resourceId)] = int(b.length)
    tex = {}
    for t in controller.GetTextures():
        tex[str(t.resourceId)] = (int(t.width), int(t.height))

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
    byeid = {}
    for a in leaves:
        try:
            byeid[a.eventId] = a.GetName(sf)
        except Exception as e:
            byeid[a.eventId] = "?%s" % e
    draws = {a.eventId: a for a in leaves if a.flags & rd.ActionFlags.Drawcall}

    # learn the scene size from the biggest colour target actually bound
    sceneW = sceneH = 0
    for a in leaves:
        if not (a.flags & rd.ActionFlags.Drawcall):
            continue
        controller.SetFrameEvent(a.eventId, False)
        for t in controller.GetD3D11PipelineState().outputMerger.renderTargets:
            r = getattr(t, "resource", None)
            if r is None:
                continue
            wh = tex.get(str(r))
            if wh and wh[0] * wh[1] > sceneW * sceneH:
                sceneW, sceneH = wh
        if a.eventId > max(EIDS) + 400:
            break
    say("scene size taken as %dx%d" % (sceneW, sceneH))

    refl_cache = {}

    def reflect(vsid):
        k = str(vsid)
        if k not in refl_cache:
            eps = controller.GetShaderEntryPoints(vsid)
            refl_cache[k] = (controller.GetShader(rd.ResourceId.Null(), vsid,
                                                  eps[0]) if eps else None)
        return refl_cache[k]

    cbres = {}

    for eid in EIDS:
        a = draws.get(eid)
        say("")
        say("=" * 78)
        say("== eid %d  %s ==" % (eid, byeid.get(eid, "?")[:80]))
        if a is None:
            say("   not a drawcall in this capture")
            continue
        controller.SetFrameEvent(eid, True)

        # ---- post-VS ------------------------------------------------------
        try:
            pv = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        except Exception as e:
            say("   post-VS RAISED %s: %s" % (type(e).__name__, e))
            pv = None
        if pv is not None and pv.vertexResourceId != rd.ResourceId.Null():
            st = int(pv.vertexByteStride)
            n = int(getattr(pv, "numIndices", 0)) or int(getattr(a, "numIndices", 0))
            n = min(n, 200000)
            raw = bytes(controller.GetBufferData(pv.vertexResourceId,
                                                 int(pv.vertexByteOffset),
                                                 st * n))
            xs, ys, ws = [], [], []
            for k in range(n):
                o = k * st
                if o + 16 > len(raw):
                    break
                x, y, z, w = struct.unpack_from("<4f", raw, o)
                if w == 0.0 or w != w:
                    continue
                xs.append(x / w)
                ys.append(y / w)
                ws.append(w)
            if xs:
                px = [(v * 0.5 + 0.5) * sceneW for v in xs]
                py = [(0.5 - v * 0.5) * sceneH for v in ys]
                cx, cy = sceneW * 0.5, sceneH * 0.5
                rr = sorted(((a1 - cx) ** 2 + (b1 - cy) ** 2) ** 0.5
                            for a1, b1 in zip(px, py))
                say("   post-VS %d verts   w %.5f..%.5f" % (len(xs), min(ws), max(ws)))
                say("   NDC  x[%+.4f,%+.4f]  y[%+.4f,%+.4f]"
                    % (min(xs), max(xs), min(ys), max(ys)))
                say("   px   x[%.1f,%.1f]  y[%.1f,%.1f]   centre of bbox (%.1f,%.1f)"
                    % (min(px), max(px), min(py), max(py),
                       (min(px) + max(px)) * 0.5, (min(py) + max(py)) * 0.5))
                say("   radius from TARGET CENTRE (%.0f,%.0f): min %.1f  "
                    "median %.1f  max %.1f" % (cx, cy, rr[0], rr[len(rr) // 2],
                                               rr[-1]))
                fr = [0.0, 0.1, 0.25, 0.5, 0.75, 0.9, 1.0]
                say("   radius quantiles: %s"
                    % " ".join("%.0f%%=%.0f" % (f * 100,
                                                rr[min(len(rr) - 1,
                                                       int(f * (len(rr) - 1)))])
                               for f in fr))
            else:
                say("   post-VS produced no finite vertices")
        else:
            say("   no post-VS data")

        # ---- the VS b1 constant block ------------------------------------
        pipe = controller.GetPipelineState()
        vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        say("   VS %s" % str(vsid))
        refl = reflect(vsid)
        if refl is None:
            say("   no VS reflection")
            continue
        for k, cb in enumerate(refl.constantBlocks):
            say("   declares block[%d] b%s %-24s %d bytes"
                % (k, cb.fixedBindNumber, cb.name, int(cb.byteSize)))
        bi = None
        for k, cb in enumerate(refl.constantBlocks):
            if int(cb.fixedBindNumber) == 1:
                bi, bname, bdecl = k, cb.name, int(cb.byteSize)
                break
        if bi is None:
            say("   VS declares no b1")
            continue
        rid, boff, bsize = "NULL", 0, 0
        for ud in pipe.GetConstantBlocks(rd.ShaderStage.Vertex):
            acc = getattr(ud, "access", None)
            if acc is None or int(getattr(acc, "index", -1)) != bi:
                continue
            d = getattr(ud, "descriptor", None)
            if d is None:
                continue
            rid = str(getattr(d, "resource", "NULL"))
            boff = int(getattr(d, "byteOffset", 0))
            bsize = int(getattr(d, "byteSize", 0))
            break
        real = bufl.get(rid, -1)
        say("   b1 = %-22s block %-22s decl %d B, REAL D3D length %d B, "
            "bind offset %d size %d" % (rid, bname, bdecl, real, boff, bsize))
        cbres[eid] = rid
        if rid != "NULL" and real > 0:
            want = min(real - boff, 192)
            data = bytes(controller.GetBufferData(
                [b.resourceId for b in controller.GetBuffers()
                 if str(b.resourceId) == rid][0], boff, want))
            for base in (0x00, 0x40, 0x80):
                if base + 64 <= len(data):
                    m = struct.unpack_from("<16f", data, base)
                    say("     +0x%02X  [% .5f % .5f % .5f % .5f]" % (base, m[0], m[1], m[2], m[3]))
                    say("            [% .5f % .5f % .5f % .5f]" % (m[4], m[5], m[6], m[7]))
                    say("            [% .5f % .5f % .5f % .5f]" % (m[8], m[9], m[10], m[11]))
                    say("            [% .5f % .5f % .5f % .5f]" % (m[12], m[13], m[14], m[15]))

    # ---- how is that constant buffer WRITTEN? ----------------------------
    say("")
    say("=" * 78)
    say("== how each of those b1 buffers is written ==")
    allres = {str(b.resourceId): b.resourceId for b in controller.GetBuffers()}
    for rid in sorted(set(cbres.values())):
        if rid == "NULL" or rid not in allres:
            continue
        say("")
        say("  %s  (%d bytes)" % (rid, bufl.get(rid, -1)))
        try:
            usage = controller.GetUsage(allres[rid])
        except Exception as e:
            say("    GetUsage RAISED %s: %s" % (type(e).__name__, e))
            continue
        kinds = {}
        cpuw = []
        for u in usage:
            k = str(u.usage).split(".")[-1]
            kinds[k] = kinds.get(k, 0) + 1
            if "CPU" in k or "Copy" in k or "Clear" in k:
                cpuw.append((int(u.eventId), k))
        say("    usage kinds: %s" % kinds)
        say("    %d CPU-write / copy events" % len(cpuw))
        for e, k in cpuw[:40]:
            say("      eid %-7d %-14s %s" % (e, k, byeid.get(e, "?")[:70]))

    say("")
    say("== every Map / Unmap / UpdateSubresource action in the frame, tallied ==")
    tal = {}
    for a in leaves:
        nm = byeid.get(a.eventId, "")
        for key in ("Map", "Unmap", "UpdateSubresource", "CopyResource",
                    "CopySubresourceRegion"):
            if key in nm:
                tal[key] = tal.get(key, 0) + 1
                break
    say("   %s" % tal)

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
