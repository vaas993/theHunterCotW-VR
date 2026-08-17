"""Where the TAA resolve and the velocity pass get their PREVIOUS camera.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_taa_prevvp.py

WHY THIS EXISTS. Per-eye TAA history fixed the ghosting and introduced blur,
because TAA REPROJECTS its history and the reprojection still described a
different render. The live probe read only six PIXEL-shader constant buffers at
the resolve, found no 704-byte camera block among them, and concluded the camera
matrix was not there. Replaying the capture's state showed why that was wrong:

    resolve draw, chunk 105269
      PS cb0 1584  cb1 384  cb2 192  cb3 160  cb4 64  cb5 192  cb6 96
         cb7 112  cb8 64
      VS cb0 = 704 BYTES - the shared camera block, on the VERTEX shader

Nine pixel-shader buffers and fourteen vertex ones; the probe sampled six of one
and none of the other. So this asks the capture directly, where buffer CONTENTS
are readable, instead of spending another headset run.

WHAT IT PRINTS. For the TAA resolve and for the single full-screen draw that
writes the motion-vector target, every PS and VS constant buffer, and every
16-byte-aligned 4x4 inside them that looks like a camera clip matrix. Each one is
compared against the CURRENT camera (VS cb0 +0x000):

    SAME      -> this slot holds the current view-projection
    DIFFERENT -> this slot holds a DIFFERENT camera, i.e. the previous render.
                 That is the matrix the per-eye fix has to write.

The capture is mono, so "previous render" here means the previous FRAME rather
than the other eye. That does not matter for identification: the question is
WHICH BUFFER AND OFFSET carries a camera that is not the current one.
"""
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame28218.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_taa_prevvp.txt")

R11G11B10 = rd.ResourceFormatType.R11G11B10
R16G16 = 34          # DXGI_FORMAT_R16G16_FLOAT, checked by component count below

lines = []


def out(s=""):
    lines.append(s)
    print(s)


def is_clip_matrix(m):
    """The structural test the mod uses, kept identical so the two agree.

    A perspective world->clip matrix has a positive x scale in [0.1, 40], a
    positive y scale in the same range, and a near plane that is small and
    positive. Anything else - identity padding, a colour matrix, a bone - fails.
    """
    a = abs(m[0]) + abs(m[4]) + abs(m[8])
    b = abs(m[1]) + abs(m[5]) + abs(m[9])
    if not (0.1 < a < 40.0 and 0.1 < b < 40.0):
        return False
    # w row of a perspective matrix is a direction, not (0,0,0,1)
    w = (m[3], m[7], m[11])
    if abs(m[15]) > 1e-6:
        return False
    return abs(w[0]) + abs(w[1]) + abs(w[2]) > 1e-6


def mats_in(buf):
    for off in range(0, max(0, len(buf) - 63), 16):
        m = struct.unpack_from("<16f", buf, off)
        if is_clip_matrix(m):
            yield off, m


def same(a, b):
    return all(abs(x - y) <= 1e-6 * max(1.0, abs(x)) for x, y in zip(a, b))


def cb_list(state, stage):
    """The bound constant buffers, however this RenderDoc spells it.

    Three API guesses have now failed in a row - ReplayStatus, GetTexture,
    GetConstantBuffer - each costing a replay. So ask the object what it has
    instead of asserting what it should have, and take the first form that
    answers. Each entry comes back as (resourceId, byteOffset, byteSize).
    """
    def unpack(cb):
        # 1.45 wraps the binding in a UsedDescriptor; older builds hand back the
        # descriptor itself. Take whichever is there.
        d = getattr(cb, "descriptor", cb)
        rid = getattr(d, "resource", None)
        if rid is None:
            rid = getattr(d, "resourceId", None)
        return rid, getattr(d, "byteOffset", 0), getattr(d, "byteSize", 0)

    # 1.x abstracted pipeline state
    for name in ("GetConstantBuffer", "GetConstantBlock"):
        fn = getattr(state, name, None)
        if fn:
            res = []
            for slot in range(14):
                try:
                    cb = fn(stage, slot, 0)
                except Exception:
                    break
                rid, off, size = unpack(cb)
                if rid is None:
                    out("  API PROBE - fields on %s: %s" %
                        (type(cb).__name__,
                         ", ".join(n for n in dir(cb) if not n.startswith("_"))))
                    return []
                res.append((rid, off, size))
            if res:
                return res
    # the D3D11-specific state object
    d3d = getattr(state, "GetD3D11PipelineState", None)
    if d3d:
        p = d3d()
        st = {rd.ShaderStage.Vertex: p.vertexShader,
              rd.ShaderStage.Pixel: p.pixelShader}.get(stage)
        if st is not None:
            return [(cb.resourceId, cb.vecOffset * 16, cb.vecCount * 16)
                    for cb in st.constantBuffers]
    return []


def dump_stage(ctrl, state, stage, label, current):
    found = []
    for slot, (rid, boff, bsize) in enumerate(cb_list(state, stage)):
        if rid == rd.ResourceId.Null():
            continue
        try:
            data = ctrl.GetBufferData(rid, boff, bsize)
        except Exception as e:
            out("  %s cb%-2d  unreadable: %s" % (label, slot, e))
            continue
        if len(data) < 64:
            continue
        cb = None
        hits = list(mats_in(bytes(data)))
        if not hits:
            continue
        for off, m in hits:
            verdict = "SAME as the current camera"
            if current is None:
                verdict = "(first one seen - taking this as CURRENT)"
            elif not same(m, current):
                verdict = "*** DIFFERENT CAMERA - a PREVIOUS render ***"
            out("  %s cb%-2d  %5d bytes  +0x%03X   %s" %
                (label, slot, len(data), off, verdict))
            out("        row3 %12.4f %12.4f %12.4f %12.4f" %
                (m[12], m[13], m[14], m[15]))
            found.append((label, slot, off, m))
    return found


def main():
    # Same open sequence as the scripts that already work against this build -
    # this RenderDoc has no rd.ReplayStatus, and guessing at the API cost the
    # first attempt outright.
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(CAP, "", None)
    out("OpenFile -> %s" % res)
    res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if ctrl is None:
        out("could not start replay: %s" % res)
        return
    out("replay open")

    leaves = []

    def walk(lst):
        for a in lst:
            kids = getattr(a, "children", [])
            if kids:
                walk(kids)
            else:
                leaves.append(a)

    walk(ctrl.GetRootActions())
    out("%d leaf action(s)" % len(leaves))

    # There is no ctrl.GetTexture(id) in this build - textures come as one list.
    textures = {t.resourceId: t for t in ctrl.GetTextures()}
    out("%d texture(s)" % len(textures))

    # The two draws, found by what they DO rather than by a remembered index -
    # but searched from the END of the frame. Every SetFrameEvent replays the
    # capture up to that event, so walking all 1320 actions forwards is minutes
    # of work; the resolve sits at about draw 746 of 785, which is a few dozen
    # actions from the back.
    resolve = None
    velocity = None
    for a in reversed(leaves):
        if resolve is not None and velocity is not None:
            break
        try:
            ctrl.SetFrameEvent(a.eventId, True)
            s = ctrl.GetPipelineState()
            live = [r for r in s.GetOutputTargets()
                    if r.resource != rd.ResourceId.Null()]
        except Exception:
            continue
        if not live:
            continue
        idx = getattr(a, "numIndices", 0)
        if resolve is None and idx == 3 and len(live) == 1 and \
           s.GetDepthTarget().resource == rd.ResourceId.Null():
            tex = textures.get(live[0].resource)
            if tex and tex.format.compCount == 3 and tex.width > 1000:
                resolve = a.eventId
        if velocity is None and len(live) == 2:
            tex = textures.get(live[1].resource)
            if tex and tex.format.compCount == 2 and \
               tex.format.compByteWidth == 2 and tex.width > 1000:
                velocity = a.eventId

    out("resolve eventId  = %s" % resolve)
    out("velocity eventId = %s" % velocity)

    for name, eid in (("TAA RESOLVE", resolve), ("VELOCITY PASS", velocity)):
        if eid is None:
            out("\n=== %s not found ===" % name)
            continue
        out("\n=== %s (eventId %d) ===" % (name, eid))
        ctrl.SetFrameEvent(eid, True)
        state = ctrl.GetPipelineState()
        # VS cb0 is the 704-byte shared camera block; its +0x000 is the CURRENT
        # view-projection, and everything else is judged against it.
        vs = cb_list(state, rd.ShaderStage.Vertex)
        ps = cb_list(state, rd.ShaderStage.Pixel)
        out("  %d VS and %d PS constant buffer slot(s) reported" % (len(vs), len(ps)))
        if not vs and not ps:
            out("  API PROBE - names available on the pipeline state object:")
            out("    " + ", ".join(n for n in dir(state)
                                   if "onst" in n or "uffer" in n or "D3D11" in n))
        current = None
        if vs and vs[0][0] != rd.ResourceId.Null():
            try:
                d = bytes(ctrl.GetBufferData(vs[0][0], vs[0][1], vs[0][2]))
                if len(d) >= 64:
                    m = struct.unpack_from("<16f", d, 0)
                    if is_clip_matrix(m):
                        current = m
                        out("  current camera taken from VS cb0 +0x000 (%d bytes)"
                            % len(d))
            except Exception as e:
                out("  could not read VS cb0: %s" % e)
        dump_stage(ctrl, state, rd.ShaderStage.Pixel, "PS", current)
        dump_stage(ctrl, state, rd.ShaderStage.Vertex, "VS", current)

    ctrl.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    out(traceback.format_exc())

with open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))
print("written to %s" % OUT)

# This RenderDoc has no headless python runner - renderdoccmd's subcommands are
# capture/convert/replay only - so the script runs inside the full qrenderdoc,
# which leaves its window open afterwards. Exit the process outright once the
# file is written, so a diagnostic never leaves a GUI sitting on the desktop.
os._exit(0)
