"""The viewmodel transform, read out of the constant buffer.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_viewmodel_cb.py

The shader sweep found the dedicated viewmodel forward pass in the current
capture: one contiguous block of events, depth-only rounds first (null pixel
shader), then the colour round bound to the three known weapon CRCs.  This pass
goes to those colour draws and asks the only question left: WHERE IS THE
TRANSFORM.

The renderer is camera-relative, so the weapon is the only opaque geometry whose
transform sits within about a metre of the origin - that is what makes it
findable without knowing the buffer layout.  For every colour draw we dump every
VS constant buffer and scan it for an orthonormal 3x3 with a near translation,
in both row-major and 3x4 layouts.

What matters in the output is not just that a transform is found, but WHICH SLOT
and WHAT OFFSET, and whether that stays put across the ten meshes.  A per-object
buffer changes every draw; a shared view buffer does not.  The mod needs the
per-object one, at a fixed offset, to shift the gun per eye.
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
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_viewmodel_cb.txt")

# The colour round, from rdc_shader_sweep.txt.  33632 is 7A86206F, 33714..33784
# are F616CE07, 33851..33956 are FBECF3C0.  33482..33500 (DD3310AF) is included
# because it sits inside the same block and may be part of the same pass.
COLOUR_EIDS = [33482, 33632, 33714, 33749, 33784, 33851, 33876, 33901, 33956]

# The depth-only rounds ahead of it - null pixel shader.  Sampled, not dumped:
# we only need to confirm they share the colour round's constant buffers, since
# a clip-space shift has to patch them too or depth-equal tests kill the gun.
DEPTH_EIDS = [33313, 33338, 33373, 33406]

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
    leaves = []

    def walk(lst):
        for a in lst:
            kids = getattr(a, "children", [])
            if kids:
                walk(kids)
            else:
                leaves.append(a)

    walk(controller.GetRootActions())
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

    # Every near-transform found, keyed by (slot, offset, layout), so the report
    # at the end can say which candidate is stable across the whole pass.
    hits = {}

    def scan_blob(eid, slot, data):
        found = 0
        end = len(data) - 63
        for off in range(0, max(end, 0), 16):
            m = floats(data, off, 16)
            if not all(map(math.isfinite, m)):
                continue
            for layout, rows, pos in (
                ("row", [m[0:3], m[4:7], m[8:11]], (m[12], m[13], m[14])),
                ("3x4", [(m[0], m[4], m[8]), (m[1], m[5], m[9]),
                         (m[2], m[6], m[10])], (m[3], m[7], m[11])),
            ):
                if not orthonormal_3x3(rows):
                    continue
                px, py, pz = pos
                d = math.sqrt(px * px + py * py + pz * pz)
                if not (0.005 < d < 2.0):
                    continue
                found += 1
                if found <= 8:
                    say("     cb%-2d +0x%05X %s pos=(%+.4f, %+.4f, %+.4f) |p|=%.4f"
                        % (slot, off, layout, px, py, pz, d))
                hits.setdefault((slot, off, layout), []).append((eid, pos))
        return found

    def dump_draw(eid, tag):
        a = by_eid.get(eid)
        if a is None:
            say("  eid %d: not a leaf action" % eid)
            return
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        vs = pipe.GetShader(rd.ShaderStage.Vertex)
        ps = pipe.GetShader(rd.ShaderStage.Pixel)
        idx = getattr(a, "numIndices", 0)
        say("")
        say("  -- eid %d [%s] VS %08X PS %08X  %d indices --"
            % (eid, tag, crc_of(vs) or 0, crc_of(ps) or 0, idx))

        total = 0
        for slot in range(14):
            try:
                cb = pipe.GetConstantBuffer(rd.ShaderStage.Vertex, slot, 0)
            except Exception:
                continue
            if cb.resourceId == rd.ResourceId.Null():
                continue
            try:
                data = controller.GetBufferData(cb.resourceId, cb.byteOffset,
                                                cb.byteSize)
            except Exception as e:
                say("     cb%d GetBufferData failed: %s" % (slot, e))
                continue
            say("     cb%-2d %s  off 0x%X  %d bytes"
                % (slot, cb.resourceId, cb.byteOffset, len(data)))
            total += scan_blob(eid, slot, data)
        if total == 0:
            say("     (no camera-near orthonormal transform in any VS cbuffer)")

    say("")
    say("=== THE COLOUR ROUND ===")
    for eid in COLOUR_EIDS:
        dump_draw(eid, "colour")

    say("")
    say("=== THE DEPTH ROUNDS (sampled) ===")
    for eid in DEPTH_EIDS:
        dump_draw(eid, "depth")

    # A candidate that appears at the same slot+offset in EVERY colour draw is
    # a shared buffer; one that appears in each draw with a DIFFERENT position
    # is the per-object transform, which is the one worth patching.
    say("")
    say("=== CANDIDATES, ranked by how much of the pass they cover ===")
    ranked = sorted(hits.items(), key=lambda kv: -len(kv[1]))
    for (slot, off, layout), seen in ranked[:30]:
        eids = [e for e, _ in seen]
        uniq = set("%.4f,%.4f,%.4f" % p for _, p in seen)
        kind = "PER-OBJECT" if len(uniq) > 1 else "shared/constant"
        say("  cb%-2d +0x%05X %s  %d draw(s)  %d distinct pos  %s"
            % (slot, off, layout, len(seen), len(uniq), kind))
        for e, p in seen[:6]:
            say("        eid %-6d (%+.4f, %+.4f, %+.4f)" % (e, p[0], p[1], p[2]))
    if not ranked:
        say("  none - the transform is not a plain matrix in a VS cbuffer")

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
