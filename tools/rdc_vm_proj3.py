"""Does the viewmodel's projection term hold in a SECOND, independent capture?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_vm_proj3.py

rdc_vm_proj.py / _proj2.py showed, in cotw_frame4317.rdc, that

    b = |(m01,m11,m21)| / |(m03,m13,m23)|      of the WVP in b1

reads 3.38806 on every draw of the first-person pass and 1.00000 on essentially
every world draw.  One capture proves nothing about generality, and a mod that
hardcodes 3.388 from a single frame is exactly the kind of "verified unique in
one frame" mistake index count already made.

So this repeats the measurement on cotw_frame18616.rdc - a different session,
different map position - WITHOUT being told where the viewmodel is.  If the
first-person pass really has its own fixed projection, a small isolated cluster
of b will fall out of the histogram on its own.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_vm_proj3.txt
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame18616.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_vm_proj3.txt")

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def n3(x, y, z):
    return math.sqrt(x * x + y * y + z * z)


def main():
    cap = rd.OpenCaptureFile()
    say("capture: %s" % CAP)
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
    say("%d drawcalls, eid %d..%d"
        % (len(draws), min(a.eventId for a in draws),
           max(a.eventId for a in draws)))

    refl_cache = {}

    def b1_pos(vsid):
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

    # eid -> best (b, off, w0, depthRT)
    got = []
    for i, a in enumerate(draws):
        eid = a.eventId
        if i % 300 == 0:
            say("   ... draw %d/%d (eid %d)" % (i, len(draws), eid))
        try:
            controller.SetFrameEvent(eid, False)
            pipe = controller.GetPipelineState()
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        except Exception:
            continue
        if vsid == rd.ResourceId.Null():
            continue
        pos = b1_pos(vsid)
        if pos is None:
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
        if not data or len(data) < 128:
            continue
        # only the two real layout offsets, as the mod would use
        for off in (0x00, 0x40):
            if off + 64 > len(data):
                continue
            m = struct.unpack_from("<16f", data, off)
            if not all(map(math.isfinite, m)):
                continue
            s = n3(m[3], m[7], m[11])
            if not (1e-4 < s < 1e4):
                continue
            b = n3(m[1], m[5], m[9]) / s
            aa = n3(m[0], m[4], m[8]) / s
            if not (1e-3 < b < 1e3):
                continue
            dep = ""
            try:
                ds = pipe.GetDepthTarget()
                if ds is not None and ds.resource != rd.ResourceId.Null():
                    dep = str(ds.resource)
            except Exception:
                pass
            got.append((eid, int(getattr(a, "numIndices", 0)), off, aa, b,
                        m[15], dep))
            break

    say("")
    say("=" * 74)
    say("%d of %d drawcalls yielded a plausible WVP in b1" % (len(got), len(draws)))
    hist = {}
    for g in got:
        hist[round(g[4], 4)] = hist.get(round(g[4], 4), 0) + 1
    say("")
    say("== histogram of b = cot(vFov/2) ==")
    for k, v in sorted(hist.items(), key=lambda kv: -kv[1]):
        say("   b = %-12s x %d" % (k, v))

    say("")
    say("== every draw with b > 2.0 (the candidate first-person cluster) ==")
    hi = [g for g in got if g[4] > 2.0]
    say("   %d draw(s)" % len(hi))
    for eid, ni, off, aa, b, w0, dep in hi:
        say("   eid %-6d %7d idx  +0x%02X  a=%.5f b=%.5f w0=%+.4f depth=%s"
            % (eid, ni, off, aa, b, w0, dep))
    if hi:
        eids = [g[0] for g in hi]
        say("   eid span %d..%d, contiguous block? %s"
            % (min(eids), max(eids),
               "yes" if max(eids) - min(eids) < 1200 else "NO - scattered"))

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
