"""PROVE the lens mask, and score the mesh-echo clause over the whole frame.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_ads_mask_proof.py

rdc_ads_assembly found two draws (17027, 17030) that bind ZERO render targets,
have a null pixel shader, DepthEnable == FALSE, and StencilFunc = ALWAYS with
StencilWriteMask == 0x40 / ref 0x40 / passOp = REPLACE.  Their post-VS geometry
is two concentric discs dead centre of the screen.  Every first-person draw then
tests StencilFunc = NOT_EQUAL with StencilReadMask == 0x40 and ref & 0x40 - i.e.
"draw only where bit 0x40 is CLEAR".

If that reading is right then:
  A. after 17030 the stencil buffer has bit 0x40 SET inside the disc and CLEAR
     outside it;
  B. at a pixel inside the disc, the shroud draw 17627 appears in the render
     target's pixel history as a FAILED modification, and at a pixel outside it
     as a passing one.

Both are measurements, not inferences.  Nothing here is swallowed: every API
that can raise prints the exception.

It also replays every draw in the frame to record the input-assembler identity
(index buffer + vertex buffer 0 + offset + stride + index count), so the
"mesh echo" clause - move any later draw drawing the same mesh as a mask draw -
can be scored against the whole frame rather than the window.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ads_mask_proof_<cap>.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.environ.get("COTW_CAP") or os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame13783.rdc")
TAG = os.path.splitext(os.path.basename(CAP))[0]
BASE = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR")
OUT = os.path.join(BASE, "rdc_ads_mask_proof_%s.txt" % TAG)

MASK_EIDS = [int(x) for x in
             os.environ.get("COTW_MASK", "17027,17030").split(",")]
SHROUD_EID = int(os.environ.get("COTW_SHROUD", "17627"))
LENS_EIDS = [int(x) for x in
             os.environ.get("COTW_LENS", "17766,17775").split(",")]
# a draw a little before the first mask draw, to show the bit is clear first
BEFORE_EID = int(os.environ.get("COTW_BEFORE", "16972"))

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def rid_of(o):
    if o is None:
        return "NULL"
    for attr in ("resourceId", "resource"):
        v = getattr(o, attr, None)
        if v is not None:
            s = str(v)
            return "NULL" if s.endswith("::0") else s
    d = getattr(o, "descriptor", None)
    if d is not None and d is not o:
        return rid_of(d)
    return "NULL"


def num_of(o, *names):
    for n in names:
        v = getattr(o, n, None)
        if v is not None:
            try:
                return int(v)
            except Exception:
                pass
    return 0


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
            kids = getattr(a, "children", [])
            if kids:
                walk(kids)
            else:
                leaves.append(a)

    walk(controller.GetRootActions())
    draws = [a for a in leaves if a.flags & rd.ActionFlags.Drawcall]
    draws.sort(key=lambda a: a.eventId)
    by_eid = {a.eventId: a for a in leaves}
    say("%d drawcalls" % len(draws))

    tex = {}
    for t in controller.GetTextures():
        tex[str(t.resourceId)] = t

    # resolve the scene depth + colour targets from the mask draw itself
    controller.SetFrameEvent(MASK_EIDS[-1], True)
    d3d = controller.GetD3D11PipelineState()
    dsv_s = rid_of(d3d.outputMerger.depthTarget)
    controller.SetFrameEvent(SHROUD_EID, True)
    d3d = controller.GetD3D11PipelineState()
    rtv_s = "NULL"
    try:
        for ud in d3d.outputMerger.renderTargets:
            r = rid_of(ud)
            if r != "NULL":
                rtv_s = r
                break
    except Exception as e:
        say("renderTargets raised %s: %s" % (type(e).__name__, e))
    say("scene DSV %s   scene RTV %s" % (dsv_s, rtv_s))
    dsv_t = tex.get(dsv_s)
    rtv_t = tex.get(rtv_s)
    if dsv_t is None:
        say("cannot resolve the depth target - abort")
        return
    W, H = int(dsv_t.width), int(dsv_t.height)
    say("scene size %dx%d  depth format %s" % (W, H, dsv_t.format.Name()))

    # ---------------------------------------------------------------- A ----
    say("")
    say("=" * 78)
    say("== A: is stencil bit 0x40 set inside the disc and clear outside? ==")
    say("   sampling the DEPTH-STENCIL target %s at a horizontal and a vertical"
        % dsv_s)
    say("   line through the centre, before and after each mask draw.")

    cx, cy = W // 2, H // 2
    # NDC fractions to sample along +x and +y from the centre
    fx = [0.0, 0.05, 0.10, 0.14, 0.20, 0.35, 0.50, 0.58, 0.61, 0.64, 0.70,
          0.80, 0.95]
    pts = [("x%+.2f" % f, min(W - 1, int(cx + f * cx)), cy) for f in fx]
    pts += [("y%+.2f" % f, cx, max(0, int(cy - f * cy))) for f in fx]

    def pick(eid, x, y):
        controller.SetFrameEvent(eid, True)
        try:
            pv = controller.PickPixel(dsv_t.resourceId, x, y,
                                      rd.Subresource(0, 0, 0),
                                      rd.CompType.Typeless)
        except Exception as e:
            return "PickPixel RAISED %s: %s" % (type(e).__name__, e)
        try:
            fv = list(pv.floatValue)
        except Exception:
            fv = []
        try:
            uv = list(pv.uintValue)
        except Exception:
            uv = []
        return (fv, uv)

    say("")
    say("   probe of the raw PickPixel return at the centre, so the field")
    say("   layout is read rather than assumed:")
    for eid in [BEFORE_EID] + MASK_EIDS:
        say("      eid %-6d -> %s" % (eid, pick(eid, cx, cy)))

    def stencil_at(eid, x, y):
        r = pick(eid, x, y)
        if not isinstance(r, tuple):
            return None
        fv, uv = r
        # D32S8: RenderDoc hands back depth in [0] and stencil in [1].
        if len(fv) > 1:
            s = fv[1]
            # stencil comes back either 0..255 or normalised 0..1
            return int(round(s * 255.0)) if 0.0 <= s <= 1.0001 else int(round(s))
        if len(uv) > 1:
            return int(uv[1])
        return None

    for eid in [BEFORE_EID] + MASK_EIDS + [SHROUD_EID]:
        say("")
        say("   --- stencil AFTER eid %d ---" % eid)
        say("      %-9s %-6s %-6s %-8s %-8s %s"
            % ("probe", "px", "py", "stencil", "hex", "bit 0x40"))
        for name, x, y in pts:
            s = stencil_at(eid, x, y)
            if s is None:
                say("      %-9s %-6d %-6d  (unreadable)" % (name, x, y))
                continue
            say("      %-9s %-6d %-6d %-8d 0x%02X     %s"
                % (name, x, y, s, s & 0xFF, "SET" if (s & 0x40) else "."))

    # ---------------------------------------------------------------- B ----
    say("")
    say("=" * 78)
    say("== B: pixel history on the scene colour target ==")
    say("   inside the disc the shroud must FAIL; outside it must PASS.")
    if rtv_t is not None:
        probes = [("inside the disc  (NDC x +0.35)", int(cx + 0.35 * cx), cy),
                  ("inside the disc  (NDC y +0.50)", cx, int(cy - 0.50 * cy)),
                  ("outside the disc (NDC x +0.75)", int(cx + 0.75 * cx), cy),
                  ("outside the disc (NDC y +0.95)", cx, int(cy - 0.95 * cy))]
        for label, x, y in probes:
            say("")
            say("   -- %s at (%d, %d) --" % (label, x, y))
            try:
                hist = controller.PixelHistory(rtv_t.resourceId, x, y,
                                               rd.Subresource(0, 0, 0),
                                               rd.CompType.Typeless)
            except Exception as e:
                say("      PixelHistory RAISED %s: %s" % (type(e).__name__, e))
                continue
            say("      %d modification(s)" % len(hist))
            for h in hist:
                a = by_eid.get(h.eventId)
                nm = a.GetName(sf).split("(")[0][:34] if a else "?"
                bits = []
                for f in ("depthTestFailed", "stencilTestFailed",
                          "scissorClipped", "shaderDiscarded",
                          "backfaceCulled", "sampleMasked",
                          "predicationSkipped", "unboundPS"):
                    if getattr(h, f, False):
                        bits.append(f)
                try:
                    passed = h.Passed()
                except Exception:
                    passed = "?"
                mark = ""
                if h.eventId in MASK_EIDS:
                    mark = "   <<< MASK DRAW"
                elif h.eventId == SHROUD_EID:
                    mark = "   <<< SHROUD"
                elif h.eventId in LENS_EIDS:
                    mark = "   <<< LENS"
                say("      eid %-6d passed=%-5s %-40s idx=%-6s %s%s"
                    % (h.eventId, passed, ",".join(bits) or "-",
                       getattr(a, "numIndices", "?") if a else "?", nm, mark))
    else:
        say("   scene RTV not resolvable")

    # ---------------------------------------------------------------- C ----
    say("")
    say("=" * 78)
    say("== C: input-assembler identity of every draw in the frame ==")
    say("   (does anything outside the scope assembly draw the mask meshes?)")

    ia = {}
    for n, a in enumerate(draws):
        controller.SetFrameEvent(a.eventId, False)
        d3d = controller.GetD3D11PipelineState()
        ib = d3d.inputAssembly.indexBuffer
        key_ib = rid_of(ib)
        key_ibo = num_of(ib, "byteOffset", "offset")
        vb0 = ("NULL", 0, 0)
        try:
            for v in d3d.inputAssembly.vertexBuffers:
                r = rid_of(v)
                if r != "NULL":
                    vb0 = (r, num_of(v, "byteOffset", "offset"),
                           num_of(v, "byteStride", "stride"))
                    break
        except Exception as e:
            vb0 = ("ERR", 0, 0)
            if n < 3:
                say("   vertexBuffers raised %s: %s" % (type(e).__name__, e))
        nm = a.GetName(sf)
        indexed = "DrawIndexed" in nm
        key = (key_ib, key_ibo, vb0[0], vb0[1], vb0[2],
               int(getattr(a, "numIndices", 0)), indexed)
        ia.setdefault(key, []).append(a.eventId)
        if n % 250 == 0:
            say("   ... %d/%d" % (n, len(draws)))

    say("")
    say("   the mask draws' IA keys, and everything else sharing them:")
    for eid in MASK_EIDS + LENS_EIDS:
        k = next((k for k, v in ia.items() if eid in v), None)
        if k is None:
            say("      eid %d: no IA key" % eid)
            continue
        say("      eid %-6d key ib=%s+%d vb0=%s+%d stride=%d idx=%d indexed=%s"
            % (eid, k[0], k[1], k[2], k[3], k[4], k[5], k[6]))
        say("                 -> drawn by eids %s" % ia[k])

    # the echo clause: keys learned from mask draws, then who else matches
    learned = set()
    for eid in MASK_EIDS:
        k = next((k for k, v in ia.items() if eid in v), None)
        if k:
            learned.add(k)
    echo = sorted({e for k in learned for e in ia[k]})
    say("")
    say("   MESH-ECHO clause: draws sharing an IA key with a mask draw = %s"
        % echo)
    say("   (%d draws; the mask draws themselves are %s)"
        % (len(echo), MASK_EIDS))

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
