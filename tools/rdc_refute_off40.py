"""What is actually at b1+0x40 in the 125-draw world pass (eids 25411..27317)
whose "b" sweeps 1.04 .. 9.57 and passes within 0.02 of the mod's window?

If it is a real projection, the value is constrained.  If it is unrelated data
(bounds, wind, LOD fade), then "b" there is a meaningless number that can take
ANY value from scene to scene - and the fact that none of them landed inside
[3.37,3.41] in this one capture is luck, not separation.

Also: prove that dropping +0x40 is not an option, by showing which viewmodel
draws have NO valid matrix at +0x00.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_refute_off40.txt
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute_off40.txt")

# world draws from the near-miss continuum, then two viewmodel draws
EIDS = [25622, 25853, 26362, 25450, 27047, 33313, 33632, 33456, 33851, 34005]

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
        say("")
        say("=" * 74)
        say("eid %d   vs=%s" % (eid, vsid))

        # reflection: what does this shader declare at register b1?
        try:
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
            found = False
            for cb in refl.constantBlocks:
                if int(cb.fixedBindNumber) != 1:
                    continue
                found = True
                say("  declares b1 = %s  (%d bytes, %d vars)"
                    % (cb.name, cb.byteSize, len(cb.variables)))
                for v in cb.variables[:14]:
                    say("     +0x%04X %s" % (v.byteOffset, v.name))
            if not found:
                say("  declares NO constant block at register b1"
                    "  -> at runtime b1 holds a STALE binding")
        except Exception as e:
            say("  reflection failed: %s" % e)

        ds1 = controller.GetDescriptors(st.descriptorStore, [r1])
        d = ds1[0]
        rid = d.resource
        boff = int(getattr(d, "byteOffset", 0))
        say("  slot b1 -> %s +%d" % (rid, boff))
        if rid == rd.ResourceId.Null():
            continue
        data = bytes(controller.GetBufferData(rid, boff, 256))
        for off in (0x00, 0x40):
            m = struct.unpack_from("<16f", data, off)
            s = n3(m[3], m[7], m[11])
            b = n3(m[1], m[5], m[9]) / s if 1e-4 < s < 1e4 else float("nan")
            say("  +0x%02X rows:" % off)
            for r in range(4):
                say("        [%s]" % "  ".join("%+12.5f" % m[r * 4 + c]
                                               for c in range(4)))
            say("        s=%.5f  b=%.5f%s"
                % (s, b, "   <<< FIRES" if 3.37 <= b <= 3.41 else ""))

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
