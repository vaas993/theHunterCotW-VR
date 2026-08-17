"""PISTOL-SCOPE ADS: prove which draw is the lens stencil mask, and find the
render-to-texture passes.  Companion to rdc_ads_assembly (whole-frame sweep).

Run headlessly:
    qrenderdoc.exe --python tools\rdc_pistol_lens.py

Established by the sweep of cotw_frame33848.rdc (956 drawcalls):
    pos 887/888  eid 15852/15855  VS 2205  PS NULL  1584 idx  ZERO RTVs
                 stencil AlwaysTrue/Replace  read 0xFF  WRITE 0x40  ref 0x40
                 -> the only two draws in the whole frame that STAMP bit 0x40
    pos 889..918 eid 15901..16490  the 30 stencil-tagged first-person draws,
                 every one of them read 0x40 / NotEqual / ref & 0x40
                 -> they are masked OUT wherever bit 0x40 is set
    pos 919/920  eid 16549/16558  VS 2205  PS 1320  1584 idx  SAME IB+VB as the
                 mask -> the lens glass, drawn with colour, blended

This script closes the remaining gaps:
  1. post-VS clip-space + screen-space bounding box of every assembly draw, so
     "the mask covers the lens" is measured, not assumed
  2. PixelHistory inside the mask disc and outside it, on the main colour target
     - the viewmodel draws must show stencilTestFailed inside and not outside
  3. every SRV the lens pixel shader samples, with its texture size, and whether
     any of those textures is written earlier in this same frame (= a
     render-to-texture pass the hook must not disturb)
  4. the constant blocks at the mask draw vs a tagged draw, with the REAL D3D
     buffer length, because cbscan.cpp refuses buffers < 256 bytes

RenderDoc 1.45: GetConstantBlocks -> UsedDescriptor; access.index is the
descriptor POSITION not the HLSL register; .descriptor is a temporary proxy.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_pistol_lens.txt
"""
import os
import struct
import traceback

import renderdoc as rd

CAP = os.environ.get("COTW_CAP") or os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame33848.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_pistol_lens.txt")

MASK = [15852, 15855]
ASSEMBLY = [15718, 15740, 15770, 15797,          # pre-pass, stencil bit 0x04
            15852, 15855,                        # the mask
            15901, 15926, 15947, 15950, 15957, 15964, 15982, 15992,
            16034, 16202, 16284, 16351, 16374, 16399, 16424, 16462, 16490,
            16549, 16558, 16605]
LENS_COLOUR = 16549
SHROUD = 16202
MAIN_RTV = "ResourceId::622"

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def en(v):
    return str(v).split(".")[-1].split(":")[0]


def rid_str(o, *names):
    for n in names:
        v = getattr(o, n, None)
        if v is not None:
            return str(v)
    return "NULL"


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
        tex[str(t.resourceId)] = (int(t.width), int(t.height), t.format.Name(),
                                  int(t.arraysize))
    bufl = {}
    for b in controller.GetBuffers():
        bufl[str(b.resourceId)] = int(b.length)

    def tdesc(s):
        t = tex.get(str(s))
        return "%dx%d %s" % (t[0], t[1], t[2]) if t else "?"

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
    draws.sort(key=lambda a: a.eventId)
    byeid = {a.eventId: a for a in draws}

    # ---------------------------------------------------------------- 1 ----
    say("")
    say("=" * 78)
    say("== 1. post-VS bounding box of every assembly draw ==")
    say("   clip-space is divided through by w; screen box is in pixels of the")
    say("   3854x2800 main target.  'w range' shows the view-space depth spread.")
    say("")
    say("   %-7s %-8s %-7s %-34s %-30s %s"
        % ("eid", "idx", "verts", "NDC x/y box", "screen px box", "w range"))
    boxes = {}
    for eid in ASSEMBLY:
        if eid not in byeid:
            continue
        a = byeid[eid]
        controller.SetFrameEvent(eid, True)
        try:
            pv = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
        except Exception as e:
            say("   eid %-7d post-VS RAISED %s: %s" % (eid, type(e).__name__, e))
            continue
        if pv is None or pv.vertexResourceId == rd.ResourceId.Null():
            say("   eid %-7d no post-VS data" % eid)
            continue
        st = int(pv.vertexByteStride)
        n = int(getattr(pv, "numIndices", 0)) or int(getattr(a, "numIndices", 0))
        n = min(n, 120000)
        raw = bytes(controller.GetBufferData(pv.vertexResourceId,
                                             int(pv.vertexByteOffset), st * n))
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
        if not xs:
            say("   eid %-7d no finite verts" % eid)
            continue
        x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
        sx0 = (x0 * 0.5 + 0.5) * 3854.0
        sx1 = (x1 * 0.5 + 0.5) * 3854.0
        sy0 = (0.5 - y1 * 0.5) * 2800.0
        sy1 = (0.5 - y0 * 0.5) * 2800.0
        boxes[eid] = (x0, x1, y0, y1, sx0, sx1, sy0, sy1)
        say("   %-7d %-8d %-7d x[%+.3f %+.3f] y[%+.3f %+.3f]  "
            "x[%6.0f %6.0f] y[%6.0f %6.0f]  %.3f..%.3f"
            % (eid, int(a.numIndices), len(xs), x0, x1, y0, y1,
               sx0, sx1, sy0, sy1, min(ws), max(ws)))

    # ---------------------------------------------------------------- 2 ----
    say("")
    say("=" * 78)
    say("== 2. PixelHistory: is the mask really cutting the viewmodel? ==")
    if MASK[0] in boxes:
        b = boxes[MASK[0]]
        cx = int((b[4] + b[5]) * 0.5)
        cy = int((b[6] + b[7]) * 0.5)
        # a point just outside the disc, on the same scanline
        ox = int(b[4] - (b[5] - b[4]) * 0.35)
        say("   mask disc screen box x[%.0f %.0f] y[%.0f %.0f]"
            % (b[4], b[5], b[6], b[7]))
    else:
        cx, cy, ox = 1927, 1400, 900
        say("   (mask box unknown, falling back to screen centre)")

    for label, px, py in (("INSIDE the disc", cx, cy),
                          ("OUTSIDE the disc", ox, cy)):
        say("")
        say("   ---- %s : pixel (%d, %d) on %s ----"
            % (label, px, py, MAIN_RTV))
        try:
            controller.SetFrameEvent(draws[-1].eventId, True)
            rid = None
            for t in controller.GetTextures():
                if str(t.resourceId) == MAIN_RTV:
                    rid = t.resourceId
                    break
            hist = controller.PixelHistory(rid, px, py, rd.Subresource(),
                                           rd.CompType.Typeless)
        except Exception as e:
            say("      PixelHistory RAISED %s: %s" % (type(e).__name__, e))
            say(traceback.format_exc())
            continue
        say("      %d modification events" % len(hist))
        say("      %-8s %-9s %-9s %-9s %-9s %-9s %s"
            % ("eid", "sten", "depth", "backface", "scissor", "discard",
               "postmod rgba"))
        for m in hist:
            if m.eventId < 15700 or m.eventId > 16700:
                continue
            pm = m.postMod
            say("      %-8d %-9s %-9s %-9s %-9s %-9s %.3f %.3f %.3f"
                % (m.eventId,
                   "FAIL" if m.stencilTestFailed else "pass",
                   "FAIL" if m.depthTestFailed else "pass",
                   "cull" if m.backfaceCulled else "-",
                   "clip" if m.scissorClipped else "-",
                   "disc" if m.shaderDiscarded else "-",
                   pm.col.floatValue[0], pm.col.floatValue[1],
                   pm.col.floatValue[2]))

    # ---------------------------------------------------------------- 3 ----
    say("")
    say("=" * 78)
    say("== 3. what the lens pixel shader samples (render-to-texture hunt) ==")
    for eid in (LENS_COLOUR, SHROUD, 15175, 16651):
        if eid not in byeid:
            continue
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        say("")
        say("   ---- eid %d ----" % eid)
        try:
            srvs = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel)
        except Exception as e:
            say("      GetReadOnlyResources RAISED %s: %s" % (type(e).__name__, e))
            continue
        for i, ud in enumerate(srvs):
            d = getattr(ud, "descriptor", None)
            if d is None:
                continue
            r = getattr(d, "resource", None)
            if r is None or r == rd.ResourceId.Null():
                continue
            acc = getattr(ud, "access", None)
            say("      t[desc %s] %s  %s"
                % (getattr(acc, "index", "?"), str(r), tdesc(r)))

    # which of those textures are RENDER TARGETS written earlier this frame
    say("")
    say("   ---- every texture used as an RTV anywhere in the frame, with the")
    say("        eid range that writes it (a hook sees these as OMSetRenderTargets)")
    written = {}
    for a in draws:
        for o in a.outputs:
            if o == rd.ResourceId.Null():
                continue
            s = written.setdefault(str(o), [])
            s.append(a.eventId)
    for k in sorted(written, key=lambda k: -len(written[k])):
        v = written[k]
        say("      %-18s %-24s %4d draws  eid %d..%d"
            % (k, tdesc(k), len(v), min(v), max(v)))

    # ---------------------------------------------------------------- 4 ----
    say("")
    say("=" * 78)
    say("== 4. constant blocks: the mask draw vs a tagged draw ==")
    for eid in (15852, 15855, 16202, 16549, 15901):
        if eid not in byeid:
            continue
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        say("")
        say("   ---- eid %d ----" % eid)
        try:
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        except Exception as e:
            say("      reflection RAISED %s: %s" % (type(e).__name__, e))
            continue
        used = list(pipe.GetConstantBlocks(rd.ShaderStage.Vertex))
        say("      VS %s   %d declared block(s), %d used descriptor(s)"
            % (vsid, len(refl.constantBlocks), len(used)))
        for bi, cb in enumerate(refl.constantBlocks):
            rid, boff, bsz = "NULL", 0, 0
            for ud in used:
                acc = getattr(ud, "access", None)
                if acc is None or int(getattr(acc, "index", -1)) != bi:
                    continue
                d = getattr(ud, "descriptor", None)
                if d is None:
                    continue
                rid = str(getattr(d, "resource", "NULL"))
                boff = int(getattr(d, "byteOffset", 0))
                bsz = int(getattr(d, "byteSize", 0))
                break
            real = bufl.get(rid, -1)
            say("      b%s %-24s declared %5d B, %2d vars, resource %-16s "
                "real %8d B, byteOffset %7d %s"
                % (cb.fixedBindNumber, cb.name, int(cb.byteSize),
                   len(cb.variables), rid, real, boff,
                   "" if real >= 256 else "<< cbscan SKIPS (<256 B)"))
            for v in cb.variables[:10]:
                td = getattr(getattr(v, "type", None), "descriptor", None)
                say("           +0x%04X %-34s %sx%s"
                    % (int(v.byteOffset), v.name,
                       getattr(td, "rows", "?"), getattr(td, "columns", "?")))

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
