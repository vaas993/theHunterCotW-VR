"""Ground truth pass 5: the weapon's instance data + prepass draws.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_weapon_buffers.py

Pass 4 proved the weapon draws with SHARED uber-shaders through the GPU-driven
indirect path - shader identity can never isolate it.  What CAN: the renderer
is camera-relative, so the weapon is the only opaque geometry whose transform
sits within ~1 m of the origin.  This pass:
  1. PixelHistory on the depth-prepass target (619) at a rifle pixel - names
     the PREPASS weapon draws + their VS (a clip-space shift must patch the
     prepass too or depth-equal tests kill the gun).
  2. At gbuffer weapon draw 24853: dumps ALL VS-visible buffers (cbuffers via
     both APIs, SRVs via introspection) and scans them for camera-near
     transforms (any float3 with |p| < 1.5 within plausible matrix rows) and
     classic matrices.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_weapon_buffers.txt
"""
import math
import os
import struct
import traceback
import zlib

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_weapon_buffers.txt")

DEPTH_RT = 619
SCOPE_PIXEL = (1900, 2496)
WEAPON_EID = 24853

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

    textures = controller.GetTextures()
    depth_tex = None
    for t in textures:
        digits = "".join(ch for ch in str(t.resourceId) if ch.isdigit())
        if digits == str(DEPTH_RT):
            depth_tex = t
    say("depth tex: %s" % (depth_tex.resourceId if depth_tex else None))

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

    # ---- 1. prepass pixel history ---------------------------------------
    if depth_tex is not None:
        x, y = SCOPE_PIXEL
        say("")
        say("== PixelHistory DEPTH %s at (%d, %d) ==" %
            (depth_tex.resourceId, x, y))
        try:
            hist = controller.PixelHistory(depth_tex.resourceId, x, y,
                                           rd.Subresource(0, 0, 0),
                                           rd.CompType.Typeless)
            say("  %d modifications" % len(hist))
            passers = []
            for h in hist:
                a = by_eid.get(h.eventId)
                name = a.GetName(sf)[:44] if a else "?"
                passed = h.Passed() if hasattr(h, "Passed") else "?"
                say("   eid %-6d passed=%-5s %s" % (h.eventId, passed, name))
                if passed is True:
                    passers.append(h.eventId)
            say("  passing: %s" % sorted(set(passers)))
            for eid in sorted(set(passers)):
                a = by_eid.get(eid)
                if a is None or not (a.flags & rd.ActionFlags.Drawcall):
                    continue
                controller.SetFrameEvent(eid, True)
                pipe = controller.GetPipelineState()
                vs = pipe.GetShader(rd.ShaderStage.Vertex)
                ps = pipe.GetShader(rd.ShaderStage.Pixel)
                say("   -> eid %d VS %08X PS %08X  %s"
                    % (eid, crc_of(vs) or 0, crc_of(ps) or 0,
                       a.GetName(sf)[:50]))
        except Exception as e:
            say("  failed: %s" % e)

    # ---- 2. buffers at the gbuffer weapon draw ---------------------------
    say("")
    say("== buffers at weapon draw eid %d ==" % WEAPON_EID)
    controller.SetFrameEvent(WEAPON_EID, True)
    pipe = controller.GetPipelineState()

    # Introspect the read-only-resources API once so nothing fails silently.
    try:
        ro = pipe.GetReadOnlyResources(rd.ShaderStage.Vertex)
        say("GetReadOnlyResources -> %d entries, type %s"
            % (len(ro), type(ro[0]).__name__ if len(ro) else "-"))
        if len(ro):
            say("  entry attrs: %s"
                % [a for a in dir(ro[0]) if not a.startswith("_")][:20])
    except Exception as e:
        say("GetReadOnlyResources failed: %s" % e)
        ro = []

    buffers = {str(b.resourceId): b for b in controller.GetBuffers()}

    def scan_blob(label, data, cap_bytes=1 << 20):
        """Scan a blob for camera-near affine transforms.

        Looks for a 3x3 orthonormal block with an adjacent translation whose
        magnitude is < 1.5 (camera-relative near geometry = the weapon), in
        both row-major (pos at m[12..14]) and 3x4 (pos at m[3,7,11]) layouts.
        Also reports plain matrices for reference, capped.
        """
        n_near = 0
        n_mat = 0
        end = min(len(data), cap_bytes) - 63
        for off in range(0, max(end, 0), 16):
            m = floats(data, off, 16)
            if not all(map(math.isfinite, m)):
                continue
            rows = [m[0:3], m[4:7], m[8:11]]
            if orthonormal_3x3(rows):
                n_mat += 1
                px, py, pz = m[12], m[13], m[14]
                d = math.sqrt(px * px + py * py + pz * pz)
                if 0.01 < d < 1.5:
                    n_near += 1
                    if n_near <= 20:
                        say("   %s +0x%06X row-major pos=(%.3f, %.3f, %.3f) d=%.3f"
                            % (label, off, px, py, pz, d))
            cols = [(m[0], m[4], m[8]), (m[1], m[5], m[9]),
                    (m[2], m[6], m[10])]
            if orthonormal_3x3(cols):
                n_mat += 1
                px, py, pz = m[3], m[7], m[11]
                d = math.sqrt(px * px + py * py + pz * pz)
                if 0.01 < d < 1.5:
                    n_near += 1
                    if n_near <= 20:
                        say("   %s +0x%06X 3x4/col pos=(%.3f, %.3f, %.3f) d=%.3f"
                            % (label, off, px, py, pz, d))
        say("   %s: %d orthonormal blocks, %d with |pos| in 0.01..1.5"
            % (label, n_mat, n_near))

    # cbuffers - try generic API with reflection-independent slots 0..13
    say("")
    say("-- VS constant buffers --")
    for slot in range(14):
        try:
            cb = pipe.GetConstantBuffer(rd.ShaderStage.Vertex, slot, 0)
        except Exception as e:
            continue
        if cb.resourceId == rd.ResourceId.Null():
            continue
        try:
            data = controller.GetBufferData(cb.resourceId, cb.byteOffset,
                                            cb.byteSize)
        except Exception as e:
            say(" cb%d GetBufferData failed: %s" % (slot, e))
            continue
        say(" cb%d %s  %d bytes" % (slot, cb.resourceId, len(data)))
        scan_blob("cb%d" % slot, data)

    # SRVs - use whatever shape the introspection revealed
    say("")
    say("-- VS SRVs --")
    for i, entry in enumerate(ro):
        rid = None
        # v1.45 UsedDescriptor: .descriptor is a Descriptor with .resource
        desc = getattr(entry, "descriptor", None)
        if desc is not None:
            rid = getattr(desc, "resource", None)
        if rid is None:
            rid = getattr(entry, "resourceId", None)
        if rid is None or rid == rd.ResourceId.Null():
            continue
        b = buffers.get(str(rid))
        kind = "buffer %d bytes" % b.length if b else "texture"
        reg = None
        acc = getattr(entry, "access", None)
        if acc is not None:
            reg = getattr(acc, "index", None)
        say(" srv[%s] reg t%s: %s (%s)" % (i, reg, rid, kind))
        if b is not None and b.length <= (16 << 20):
            try:
                data = controller.GetBufferData(rid, 0, 0)
            except Exception as e:
                say("   GetBufferData failed: %s" % e)
                continue
            scan_blob("t%s" % reg, data)

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
