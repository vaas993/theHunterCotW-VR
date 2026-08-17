"""Ground truth pass 3: which draws actually render the weapon?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_weapon_pixels.py

Pass 2 proved the weapon IS on screen (scoped rifle, bottom of frame) while
the three Shader Toggler CRCs are bound to ZERO draws - they don't draw this
weapon.  So ask the capture directly: PixelHistory at pixels inside the rifle
names every event that wrote them.  For each writing draw, dump the bound
PS/VS (with CRC-32 like Shader Toggler's), the viewport, and scan its VS
constant buffers for matrices - the weapon transform ground truth.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_weapon_pixels.txt
"""
import math
import os
import struct
import traceback
import zlib

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_weapon_pixels.txt")

# Pixels inside the rifle, chosen off the saved backbuffer (3840x2880):
# scope body, scope turret, barrel below scope, hand/fingers.
PIXELS = [(1344, 2438), (1900, 2496), (2112, 2765), (1075, 2554)]

# The G-buffer albedo target and the prepass depth target from pass 2.
GBUF_RT = 629
DEPTH_RT = 619

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def floats(data, off, n):
    return struct.unpack_from("<%df" % n, data, off)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def orthonormal_3x3(rows, tol=0.03):
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
        hits.append(("affine(row-major)", "pos=(%.3f, %.3f, %.3f)" % (m[12], m[13], m[14])))
    cols = [(m[0], m[4], m[8]), (m[1], m[5], m[9]), (m[2], m[6], m[10])]
    if orthonormal_3x3(cols):
        hits.append(("affine(col-major)", "pos=(%.3f, %.3f, %.3f)" % (m[3], m[7], m[11])))
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
    by_eid = {a.eventId: a for a in leaves}
    last_eid = leaves[-1].eventId

    # Resolve the target ResourceIds.
    textures = controller.GetTextures()
    gbuf = depth = None
    for t in textures:
        s = str(t.resourceId)
        if s.endswith("::" + str(GBUF_RT)) or s == "ResourceId::%d" % GBUF_RT:
            gbuf = t
        if s.endswith("::" + str(DEPTH_RT)) or s == "ResourceId::%d" % DEPTH_RT:
            depth = t
    # Fallback: match by parsing the int out of the id string.
    if gbuf is None or depth is None:
        for t in textures:
            digits = "".join(ch for ch in str(t.resourceId) if ch.isdigit())
            if digits == str(GBUF_RT):
                gbuf = t
            if digits == str(DEPTH_RT):
                depth = t
    say("gbuffer tex: %s  depth tex: %s"
        % (gbuf.resourceId if gbuf else None, depth.resourceId if depth else None))
    if gbuf is None:
        say("cannot find gbuffer target - abort")
        return

    # ---- pixel history on the rifle pixels ------------------------------
    controller.SetFrameEvent(last_eid, True)
    weapon_eids = {}
    for (x, y) in PIXELS:
        say("")
        say("== PixelHistory gbuffer %s at (%d, %d) ==" % (gbuf.resourceId, x, y))
        try:
            hist = controller.PixelHistory(gbuf.resourceId, x, y,
                                           rd.Subresource(0, 0, 0),
                                           rd.CompType.Typeless)
        except Exception as e:
            say("  failed: %s" % e)
            continue
        say("  %d modifications" % len(hist))
        for h in hist:
            a = by_eid.get(h.eventId)
            name = a.GetName(sf)[:44] if a else "?"
            passed = h.Passed() if hasattr(h, "Passed") else "?"
            say("   eid %-6d passed=%-5s depthFail=%s  %s"
                % (h.eventId, passed,
                   getattr(h, "depthTestFailed", "?"), name))
            if passed is True:
                weapon_eids.setdefault(h.eventId, []).append((x, y))

    say("")
    say("== distinct passing writer events: %s ==" % sorted(weapon_eids))

    # ---- dump state at each writer draw ----------------------------------
    refl_cache = {}

    def crc_of(rid):
        if rid in refl_cache:
            return refl_cache[rid]
        try:
            entries = controller.GetShaderEntryPoints(rid)
            refl = controller.GetShader(rd.ResourceId.Null(), rid, entries[0])
            crc = zlib.crc32(bytes(refl.rawBytes)) & 0xFFFFFFFF
        except Exception:
            crc = None
        refl_cache[rid] = crc
        return crc

    for eid in sorted(weapon_eids):
        say("")
        say("== state at eid %d  (wrote %s) ==" % (eid, weapon_eids[eid]))
        a = by_eid.get(eid)
        if a is not None:
            say("  action: %s  numIndices=%d numInstances=%d"
                % (a.GetName(sf), a.numIndices, a.numInstances))
        controller.SetFrameEvent(eid, True)
        st = controller.GetD3D11PipelineState()

        vs_rid = st.vertexShader.resourceId
        ps_rid = st.pixelShader.resourceId
        say("  VS %s crc %08X" % (vs_rid, crc_of(vs_rid) or 0))
        say("  PS %s crc %08X" % (ps_rid, crc_of(ps_rid) or 0))

        for vp in st.rasterizer.viewports[:2]:
            say("  viewport x=%.0f y=%.0f w=%.0f h=%.0f zmin=%.3f zmax=%.3f"
                % (vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth))

        # VS constant buffers: scan for matrices.
        for slot, cb in enumerate(st.vertexShader.constantBuffers):
            if cb.resourceId == rd.ResourceId.Null():
                continue
            try:
                data = controller.GetBufferData(cb.resourceId, 0, 0)
            except Exception:
                continue
            if not data:
                continue
            say("  VS cb%d  %s  %d bytes" % (slot, cb.resourceId, len(data)))
            for off in range(0, min(len(data), 4096) - 63, 16):
                m = floats(data, off, 16)
                for kind, detail in classify16(m):
                    say("     +0x%04X %-18s %s" % (off, kind, detail))

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
