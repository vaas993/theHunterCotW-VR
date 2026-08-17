"""Two follow-ups to rdc_vm_proj.py.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_vm_proj2.py

rdc_vm_proj.py found that the scale-invariant vertical projection term

    b = |(m01,m11,m21)| / |(m03,m13,m23)|      (= cot(vFov/2))

taken from the WorldViewProjection inside b1 reads 3.38806 on all 27 viewmodel
draws and 1.00000 on 396 of the 409 world draws that carry a perspective WVP.
Exactly two world-tagged draws also read 3.38806: eids 34005 and 34014.

PART 1 - what are 34005 and 34014?  They sit 49 events past the end of the
given viewmodel range, draw 1584 indices twice, and their World translation is
(0.12, -0.14, -0.33) - camera-relative, not the 8000-unit absolute coordinates
everything else uses.  Dump their render targets, shaders and neighbours so the
question "extra viewmodel geometry, or a genuine false positive?" gets an answer
rather than an assumption.

PART 2 - the runtime-faithful false-positive count.  Part 1 of the earlier run
used the SHADER REFLECTION to know where the matrix lives.  The mod has no
reflection: at DrawIndexed it calls VSGetConstantBuffers1 for register b1 and
looks into its own snapshot of that range.  It does not know whether this draw's
b1 is InstanceConsts (matrix at +0x00), LocalConstants (+0x40) or the 192-byte
variant (+0x40).  So this measures the BLIND test - read b1, try a matrix at
every 16-byte-aligned offset in the first 256 bytes, fire if ANY of them yields
b in the window - over every drawcall in the frame.  That is the pessimistic
bound on what the mod would actually do.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_vm_proj2.txt
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_vm_proj2.txt")

VM_LO, VM_HI = 33313, 33956
B_LO, B_HI = 3.37, 3.41          # the window round 3.38806

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
    say("replay open")

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

    # ------------------------------------------------------------- PART 1
    say("")
    say("=" * 74)
    say("PART 1 - every drawcall from the start of the viewmodel pass to the end")
    say("         of the frame, with its render targets")
    for a in draws:
        if a.eventId < 33280:
            continue
        eid = a.eventId
        try:
            controller.SetFrameEvent(eid, False)
            pipe = controller.GetPipelineState()
        except Exception as e:
            say("  eid %-6d state failed %s" % (eid, e))
            continue
        rts = []
        try:
            for o in pipe.GetOutputTargets():
                r = o.resource
                if r == rd.ResourceId.Null():
                    continue
                d = controller.GetTexture(r)
                rts.append("%s %dx%d" % (r, d.width, d.height) if d else str(r))
        except Exception:
            pass
        dep = ""
        try:
            ds = pipe.GetDepthTarget()
            if ds is not None and ds.resource != rd.ResourceId.Null():
                dep = " depth=%s" % ds.resource
        except Exception:
            pass
        ps = "null"
        vs = "?"
        try:
            p = pipe.GetShader(rd.ShaderStage.Pixel)
            ps = "null" if p == rd.ResourceId.Null() else str(p)
            vs = str(pipe.GetShader(rd.ShaderStage.Vertex))
        except Exception:
            pass
        say("  eid %-6d %7d idx  vs=%-16s ps=%-16s out=[%s]%s"
            % (eid, getattr(a, "numIndices", 0), vs, ps, ", ".join(rts), dep))

    # ------------------------------------------------------------- PART 2
    say("")
    say("=" * 74)
    say("PART 2 - blind b1 scan: no reflection, try a 4x4 at every 16-byte")
    say("         offset in the first 256 bytes of the b1 range")
    say("         fire if b = |col1|/|colW| lands in [%.2f, %.2f]" % (B_LO, B_HI))

    refl_cache = {}

    def b1_pos(vsid):
        """Descriptor position of register b1, from the shader's own binds."""
        key = str(vsid)
        if key in refl_cache:
            return refl_cache[key]
        pos = None
        try:
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
            for i, cb in enumerate(refl.constantBlocks):
                if int(cb.fixedBindNumber) == 1:
                    pos = i
                    break
        except Exception:
            pos = None
        refl_cache[key] = pos
        return pos

    fires = {"vm": [], "world": []}
    seen = {"vm": 0, "world": 0}
    nob1 = {"vm": 0, "world": 0}
    hits_at_offset = {}
    for i, a in enumerate(draws):
        eid = a.eventId
        tag = "vm" if VM_LO <= eid <= VM_HI else "world"
        seen[tag] += 1
        if i % 300 == 0:
            say("   ... draw %d/%d (eid %d)" % (i, len(draws), eid))
        try:
            controller.SetFrameEvent(eid, False)
            pipe = controller.GetPipelineState()
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        except Exception:
            nob1[tag] += 1
            continue
        if vsid == rd.ResourceId.Null():
            nob1[tag] += 1
            continue
        pos = b1_pos(vsid)
        if pos is None:
            nob1[tag] += 1
            continue
        data = None
        try:
            uds = pipe.GetConstantBlocks(rd.ShaderStage.Vertex)
            if pos < len(uds):
                d = getattr(uds[pos], "descriptor", None)
                if d is not None:
                    rid = d.resource
                    boff = int(getattr(d, "byteOffset", 0))
                    if rid != rd.ResourceId.Null():
                        data = bytes(controller.GetBufferData(rid, boff, 256))
        except Exception:
            data = None
        if not data or len(data) < 64:
            nob1[tag] += 1
            continue

        hit = None
        for off in range(0, min(len(data), 256) - 63, 16):
            m = struct.unpack_from("<16f", data, off)
            if not all(map(math.isfinite, m)):
                continue
            s = n3(m[3], m[7], m[11])
            if not (1e-4 < s < 1e4):
                continue
            b = n3(m[1], m[5], m[9]) / s
            if B_LO <= b <= B_HI:
                hit = (off, b)
                hits_at_offset[off] = hits_at_offset.get(off, 0) + 1
                break
        if hit:
            fires[tag].append((eid, getattr(a, "numIndices", 0), hit[0], hit[1]))

    say("")
    say("  drawcalls examined: viewmodel %d, world %d" % (seen["vm"], seen["world"]))
    say("  no readable b1     : viewmodel %d, world %d" % (nob1["vm"], nob1["world"]))
    say("  FIRED  viewmodel   : %d / %d" % (len(fires["vm"]), seen["vm"]))
    say("  FIRED  world       : %d / %d" % (len(fires["world"]), seen["world"]))
    say("  offsets that produced a hit: %s"
        % sorted((("+0x%02X" % k), v) for k, v in hits_at_offset.items()))
    say("")
    say("  world draws that fired:")
    for eid, ni, off, b in fires["world"]:
        say("    eid %-6d %7d idx  at +0x%02X  b=%.5f" % (eid, ni, off, b))
    say("")
    say("  viewmodel draws that did NOT fire: %s"
        % [e for e in range(0)] or "")
    got = {e for e, _, _, _ in fires["vm"]}
    miss = [a.eventId for a in draws
            if VM_LO <= a.eventId <= VM_HI and a.eventId not in got]
    say("    %s" % (miss if miss else "none - all %d fired" % seen["vm"]))

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
