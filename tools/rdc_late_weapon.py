"""Ground truth pass 7: what exactly does the late forward weapon draw paint?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_late_weapon.py

Pass 6 found the visible weapon is re-drawn AFTER post: depth-only at 31987 /
32125 (VS F2F0B7D5, no PS) then color at 32513 (VS 11F22A82, PS FBECF3C0 - an
original Shader Toggler CRC) into scene color 622.  This pass:
  1. Saves scene color 622 immediately BEFORE 31987, BEFORE 32513 and AFTER
     32513 - the diff shows exactly what the late pass paints and whether an
     unshifted G-buffer ghost sits beneath it.
  2. Scans the VS cbuffers of 32513 for matrices - a plain DrawIndexed weapon
     draw carries its transform in per-draw constants, which is the proper
     per-eye lever.
  3. Lists all draws in 31900..32600 into 622/619 with VS/PS CRCs - the full
     late weapon pass (barrel/scope/hands may be separate draws).

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_late_weapon.txt + PNGs.
"""
import math
import os
import struct
import traceback
import zlib

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_late_weapon.txt")
PNGDIR = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR")

SCENE_COLOR = 622
DEPTH_RT = 619

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def floats(data, off, n):
    return struct.unpack_from("<%df" % n, data, off)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def orthonormal_3x3(rows, tol=0.05):
    for r in rows:
        n = math.sqrt(dot(r, r))
        if abs(n - 1.0) > tol:
            return False
    if abs(dot(rows[0], rows[1])) > tol:
        return False
    if abs(dot(rows[0], rows[2])) > tol:
        return False
    if abs(dot(rows[1], rows[2])) > tol:
        return False
    ident = abs(rows[0][0] - 1) < 1e-4 and abs(rows[1][1] - 1) < 1e-4 and \
            abs(rows[2][2] - 1) < 1e-4 and abs(rows[0][1]) < 1e-4
    return not ident


def classify16(m):
    hits = []
    if not all(map(math.isfinite, m)):
        return hits
    rows = [m[0:3], m[4:7], m[8:11]]
    if orthonormal_3x3(rows):
        px, py, pz = m[12], m[13], m[14]
        hits.append(("affine(row-major)",
                     "pos=(%.3f, %.3f, %.3f) d=%.3f"
                     % (px, py, pz, math.sqrt(px*px+py*py+pz*pz))))
    cols = [(m[0], m[4], m[8]), (m[1], m[5], m[9]), (m[2], m[6], m[10])]
    if orthonormal_3x3(cols):
        px, py, pz = m[3], m[7], m[11]
        hits.append(("affine(3x4)",
                     "pos=(%.3f, %.3f, %.3f) d=%.3f"
                     % (px, py, pz, math.sqrt(px*px+py*py+pz*pz))))
    for name, w, h, z1, z2 in (("proj(row-major)", m[0], m[5], m[11], m[15]),
                               ("proj(col-major)", m[0], m[5], m[14], m[15])):
        if abs(abs(z1) - 1.0) < 1e-3 and abs(z2) < 1e-6 and w > 0.05 and h > 0.05:
            fovy = 2.0 * math.atan(1.0 / h) * 180.0 / math.pi
            fovx = 2.0 * math.atan(1.0 / w) * 180.0 / math.pi
            hits.append((name, "fovY=%.2f fovX=%.2f" % (fovy, fovx)))
    return hits


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

    textures = {}
    scene_rid = None
    for t in controller.GetTextures():
        textures[str(t.resourceId)] = t
        digits = "".join(ch for ch in str(t.resourceId) if ch.isdigit())
        if digits == str(SCENE_COLOR):
            scene_rid = t.resourceId
    say("scene color: %s" % scene_rid)

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

    # ---- 1. before/after images of the late pass -------------------------
    def snap(eid, tag):
        controller.SetFrameEvent(eid, True)
        ts = rd.TextureSave()
        ts.resourceId = scene_rid
        ts.destType = rd.FileType.PNG
        ts.mip = 0
        path = os.path.join(PNGDIR, "rdc_622_%s.png" % tag)
        ok = controller.SaveTexture(ts, path)
        say("  622 at eid %d -> %s : %s" % (eid, path, ok))

    if scene_rid is not None:
        say("")
        say("== scene color 622 snapshots ==")
        snap(31980, "before_late")     # before the first depth-only draw
        snap(32512, "before_color")    # right before the color draw
        snap(32513, "after_color")     # right after it

    # ---- 2. cbuffers of the color draw ------------------------------------
    say("")
    say("== VS/PS cbuffers at eid 32513 ==")
    controller.SetFrameEvent(32513, True)
    pipe = controller.GetPipelineState()
    for stage, label in ((rd.ShaderStage.Vertex, "VS"),
                         (rd.ShaderStage.Pixel, "PS")):
        for slot in range(14):
            try:
                cb = pipe.GetConstantBuffer(stage, slot, 0)
            except Exception:
                continue
            if cb.resourceId == rd.ResourceId.Null():
                continue
            try:
                data = controller.GetBufferData(cb.resourceId, cb.byteOffset,
                                                cb.byteSize)
            except Exception as e:
                say(" %s cb%d GetBufferData failed: %s" % (label, slot, e))
                continue
            say(" %s cb%d %s offset=%d size=%d" %
                (label, slot, cb.resourceId, cb.byteOffset, len(data)))
            for off in range(0, min(len(data), 8192) - 63, 16):
                m = floats(data, off, 16)
                for kind, detail in classify16(m):
                    say("   +0x%04X %-18s %s" % (off, kind, detail))

    # also dump first 512 bytes of the biggest VS cb raw, for layout work
    say("")
    say("== raw head of VS cbuffers at 32513 ==")
    for slot in range(14):
        try:
            cb = pipe.GetConstantBuffer(rd.ShaderStage.Vertex, slot, 0)
        except Exception:
            continue
        if cb.resourceId == rd.ResourceId.Null():
            continue
        data = controller.GetBufferData(cb.resourceId, cb.byteOffset,
                                        cb.byteSize)
        if not data:
            continue
        say(" VS cb%d (%d bytes):" % (slot, len(data)))
        for off in range(0, min(len(data), 256), 16):
            vals = floats(data, off, 4)
            say("   +0x%04X  % 12.5f % 12.5f % 12.5f % 12.5f"
                % (off, vals[0], vals[1], vals[2], vals[3]))

    # ---- 3. all draws in the late window ----------------------------------
    say("")
    say("== draws eid 31900..32600 into 622/619 ==")
    for a in leaves:
        if not (a.flags & rd.ActionFlags.Drawcall):
            continue
        if a.eventId < 31900 or a.eventId > 32600:
            continue
        outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
        keep = a.depthOut != rd.ResourceId.Null() or outs
        digits_out = ["".join(ch for ch in str(o) if ch.isdigit())
                      for o in outs]
        digits_d = "".join(ch for ch in str(a.depthOut) if ch.isdigit())
        if str(SCENE_COLOR) not in digits_out and digits_d != str(DEPTH_RT):
            continue
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        vs = pipe.GetShader(rd.ShaderStage.Vertex)
        ps = pipe.GetShader(rd.ShaderStage.Pixel)
        say("  %5d idx=%-6d VS %08X PS %08X %s"
            % (a.eventId, a.numIndices, crc_of(vs) or 0, crc_of(ps) or 0,
               a.GetName(sf)[:44]))

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
