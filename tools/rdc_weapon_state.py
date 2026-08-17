"""Ground truth pass 4: full state of the REAL weapon draws.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_weapon_state.py

Pass 3 (PixelHistory on rifle pixels) proved the weapon is drawn by
DrawIndexedInstancedIndirect at eids 24853 / 24869 / 24901 - an entry point
the mod never hooked.  This pass answers what those draws ARE:
  - VS/PS CRCs (Shader Toggler-compatible) of each weapon draw
  - how many OTHER events use the same shaders (weapon-specific or shared?)
  - constant buffers + matrix scan (generic PipeState API - the D3D11Shader
    struct lost .constantBuffers in this build)
  - VS/PS input resources (the instance-transform buffer for indirect draws)
  - every draw in eid 24500..28242 with its PS CRC, to bound the weapon
    draw range and see the batching

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_weapon_state.txt
"""
import math
import os
import struct
import traceback
import zlib

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_weapon_state.txt")

WEAPON_EIDS = [24853, 24869, 24901]
WINDOW = (24500, 28242)      # tail of the gbuffer pass
WINDOW_CAP = 200

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
        hits.append(("affine(row-major)",
                     "pos=(%.3f, %.3f, %.3f)" % (m[12], m[13], m[14])))
    cols = [(m[0], m[4], m[8]), (m[1], m[5], m[9]), (m[2], m[6], m[10])]
    if orthonormal_3x3(cols):
        hits.append(("affine(col-major)",
                     "pos=(%.3f, %.3f, %.3f)" % (m[3], m[7], m[11])))
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

    # ---- deep dump of the three weapon draws -----------------------------
    weapon_ps = {}
    weapon_vs = {}
    for eid in WEAPON_EIDS:
        say("")
        say("======== weapon draw eid %d ========" % eid)
        a = by_eid.get(eid)
        if a is not None:
            say("action: %s" % a.GetName(sf))
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()

        vs = pipe.GetShader(rd.ShaderStage.Vertex)
        ps = pipe.GetShader(rd.ShaderStage.Pixel)
        vs_crc = crc_of(vs)
        ps_crc = crc_of(ps)
        weapon_vs[str(vs)] = (vs, vs_crc)
        weapon_ps[str(ps)] = (ps, ps_crc)
        say("VS %s crc %08X" % (vs, vs_crc or 0))
        say("PS %s crc %08X" % (ps, ps_crc or 0))

        vp = pipe.GetViewport(0)
        say("viewport x=%.0f y=%.0f w=%.0f h=%.0f zmin=%.4f zmax=%.4f"
            % (vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth))

        # constant buffers via reflection block count
        try:
            refl = pipe.GetShaderReflection(rd.ShaderStage.Vertex)
            nblocks = len(refl.constantBlocks) if refl else 0
        except Exception:
            nblocks = 4
        for slot in range(nblocks):
            try:
                cb = pipe.GetConstantBuffer(rd.ShaderStage.Vertex, slot, 0)
            except Exception:
                continue
            if cb.resourceId == rd.ResourceId.Null():
                continue
            try:
                data = controller.GetBufferData(cb.resourceId, cb.byteOffset,
                                                cb.byteSize)
            except Exception:
                continue
            if not data:
                continue
            say("VS cb%d %s  offset %d  %d bytes" %
                (slot, cb.resourceId, cb.byteOffset, len(data)))
            for off in range(0, min(len(data), 8192) - 63, 16):
                m = floats(data, off, 16)
                for kind, detail in classify16(m):
                    say("   +0x%04X %-18s %s" % (off, kind, detail))

        # VS read-only resources (instance transforms for indirect draws)
        try:
            ro = pipe.GetReadOnlyResources(rd.ShaderStage.Vertex)
            say("VS SRVs:")
            for i, bound in enumerate(ro):
                # v1.45: BoundResourceArray has .resources list
                resources = getattr(bound, "resources", None)
                if resources is None:
                    resources = [bound]
                for br in resources:
                    rid = getattr(br, "resourceId", None)
                    if rid and rid != rd.ResourceId.Null():
                        say("   slot %d: %s" % (i, rid))
        except Exception as e:
            say("VS SRVs failed: %s" % e)

    # ---- how shared are the weapon shaders? ------------------------------
    say("")
    say("== usage of the weapon draw shaders across the frame ==")
    for label, table in (("PS", weapon_ps), ("VS", weapon_vs)):
        for key, (rid, crc) in table.items():
            try:
                usage = [u for u in controller.GetUsage(rid)
                         if u.usage != rd.ResourceUsage.Unused]
            except Exception as e:
                say("  %s %s: GetUsage failed %s" % (label, key, e))
                continue
            eids = sorted({u.eventId for u in usage})
            say("  %s crc %08X (%s): used at %d events: %s%s"
                % (label, crc or 0, key, len(eids), eids[:30],
                   " ..." if len(eids) > 30 else ""))

    # ---- PS CRC of every draw in the window ------------------------------
    say("")
    say("== draws in eid %d..%d with PS CRCs ==" % WINDOW)
    n = 0
    for a in leaves:
        if not (a.flags & rd.ActionFlags.Drawcall):
            continue
        if a.eventId < WINDOW[0] or a.eventId > WINDOW[1]:
            continue
        n += 1
        if n > WINDOW_CAP:
            say("  ... window cap reached")
            break
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        ps = pipe.GetShader(rd.ShaderStage.Pixel)
        vs = pipe.GetShader(rd.ShaderStage.Vertex)
        mark = " <== WEAPON" if a.eventId in WEAPON_EIDS else ""
        say("  %5d PS %08X VS %08X  %-40s%s"
            % (a.eventId, crc_of(ps) or 0, crc_of(vs) or 0,
               a.GetName(sf)[:40], mark))

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
