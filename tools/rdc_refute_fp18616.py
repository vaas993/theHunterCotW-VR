"""Nail the four FALSE POSITIVES in cotw_frame18616.rdc.

eids 19710/19714/19718 (2355 idx) and 22798 (1980 idx) fire the mod's test.
Show that:
  * their REAL WorldViewProj is at b1+0x00 and carries the WORLD projection
    (b = 1.19190), so they are world geometry beyond argument;
  * the 3.374 comes from b1+0x40, which in PerInstanceConstants is not a matrix
    at all but Opacity / PositionScale / UVScale followed by the World matrix;
  * therefore  b = sqrt(PositionScale^2 + 1) / UVScale.y  - an authoring ratio
    of two unrelated per-instance floats.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_refute_fp18616.txt
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame18616.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute_fp18616.txt")

EIDS = [19710, 19718, 22798, 31910]

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def n3(x, y, z):
    return math.sqrt(x * x + y * y + z * z)


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return

    controller.SetFrameEvent(EIDS[0], False)
    st = controller.GetD3D11PipelineState()
    rng = rd.DescriptorRange()
    rng.offset = 0
    rng.count = int(st.descriptorCount)
    locs = controller.GetDescriptorLocations(st.descriptorStore, [rng])
    B1 = None
    for i, L in enumerate(locs):
        if "ConstantBlock" in str(L.category) and "Vertex" in str(L.stageMask) \
                and int(L.fixedBindNumber) == 1:
            B1 = i
            break
    r1 = rd.DescriptorRange()
    r1.offset = B1
    r1.count = 1

    for eid in EIDS:
        controller.SetFrameEvent(eid, False)
        pipe = controller.GetPipelineState()
        st = controller.GetD3D11PipelineState()
        vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        psid = pipe.GetShader(rd.ShaderStage.Pixel)
        say("")
        say("=" * 74)
        say("eid %d  vs=%s  ps=%s" % (eid, vsid, psid))
        try:
            ds = pipe.GetDepthTarget()
            say("  depth=%s" % (ds.resource if ds else None))
            rts = [str(o.resource) for o in pipe.GetOutputTargets()
                   if o.resource != rd.ResourceId.Null()]
            say("  colour targets=%s" % rts)
        except Exception as e:
            say("  targets ? %s" % e)

        names = {}
        try:
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
            for cb in refl.constantBlocks:
                if int(cb.fixedBindNumber) != 1:
                    continue
                say("  b1 block = %s (%d bytes)" % (cb.name, cb.byteSize))
                for v in cb.variables:
                    names[int(v.byteOffset)] = str(v.name)
        except Exception as e:
            say("  reflection failed %s" % e)

        ds1 = controller.GetDescriptors(st.descriptorStore, [r1])
        d = ds1[0]
        rid = d.resource
        boff = int(getattr(d, "byteOffset", 0))
        if rid == rd.ResourceId.Null():
            continue
        data = bytes(controller.GetBufferData(rid, boff, 320))
        for off in (0x00, 0x40):
            m = struct.unpack_from("<16f", data, off)
            s = n3(m[3], m[7], m[11])
            b = n3(m[1], m[5], m[9]) / s if 1e-4 < s < 1e4 else float("nan")
            say("  --- the mod reads 16 floats at +0x%02X ---" % off)
            for r in range(4):
                say("        [%s]" % "  ".join("%+13.5f" % m[r * 4 + c]
                                               for c in range(4)))
            say("        s=|m3,m7,m11|=%.5f   b=|m1,m5,m9|/s=%.5f%s"
                % (s, b, "    <<<< FIRES -> tagged VIEWMODEL"
                   if 3.37 <= b <= 3.41 else ""))
        # name the floats the +0x40 read actually consumed
        say("  --- what those 16 floats really are (by reflection) ---")
        for k in range(16):
            byte = 0x40 + k * 4
            nm = None
            best = -1
            for o, n in names.items():
                if o <= byte and o > best:
                    best, nm = o, n
            say("        m[%2d] @+0x%03X  %-22s = %+.5f"
                % (k, byte, nm or "?", struct.unpack_from("<f", data, byte)[0]))
        ps = struct.unpack_from("<f", data, 0x44)[0]
        uv = struct.unpack_from("<f", data, 0x4C)[0]
        say("        => sqrt(PositionScale^2+1)/UVScale.y = sqrt(%.5f^2+1)/%.5f"
            " = %.5f" % (ps, uv, math.sqrt(ps * ps + 1.0) / uv if uv else 0))

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
