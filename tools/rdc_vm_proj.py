"""Does the CONTENT of the VS constant buffers separate viewmodel from world?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_vm_proj.py

The question the mod needs answered at DrawIndexed time: is this draw the gun /
hands / gloves / watch, or is it the world?  Index count fails (a fence draws
3252 too).  Pixel-shader CRC fails (shaders made before the hooks exist).  So:
is there anything in b1 - which the mod already reads - that splits them?

WHAT THIS MEASURES.  Both known b1 layouts carry a full world-view-projection:

    InstanceConsts (256 B)   +0x00 WorldViewProjection   +0x70 World
    LocalConstants (6944 B)  +0x00 World  +0x40 WorldViewProj

WVP = World * View * Proj.  Write the 3x3 part in columns.  Because World's
rotation and View are orthonormal, the object's own scale s and the projection's
x/y scales a,b come out of the composite without knowing View at all:

    s = |(m03, m13, m23)|            the w column, = the object's uniform scale
    a = |(m00, m10, m20)| / s        = cot(hFov/2)   of whatever projection
    b = |(m01, m11, m21)| / s        = cot(vFov/2)   this draw was given
    w0 = m33                         = -(view-space z of the object's origin)

a and b are scale-invariant, so a 3x-scaled rock cannot fake them.  w0 is the
distance from the eye to the object's ORIGIN along the view axis - unaffected by
object scale, because scale does not move the origin.

Every one of those is four floats and a couple of square roots from a buffer the
mod already has mapped.  This script computes them for EVERY drawcall in the
frame, tags the viewmodel range, and counts how cleanly each one cuts.

TRAPS ALREADY PAID FOR (see rdc_instconsts.py):
  * PipeState.GetConstantBuffer does not exist on 1.45 - GetConstantBlocks does,
    returning UsedDescriptor.
  * access.index is the descriptor POSITION, not the HLSL register b1.
  * The Descriptor from .descriptor is a temporary SWIG proxy: copy the fields
    and read the buffer NOW, never across an iteration.
  * Variable offsets are per shader.  Nothing here is hardcoded; every offset
    comes from refl.constantBlocks.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_vm_proj.txt  (summary)
        %LOCALAPPDATA%\theHunterCotWVR\rdc_vm_proj.csv  (one row per drawcall)
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_vm_proj.txt")
CSV = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_vm_proj.csv")

VM_LO, VM_HI = 33313, 33956          # the viewmodel pass, given

out = open(OUT, "w")
csv = open(CSV, "w")


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
    say("%d leaf actions, %d drawcalls, eid %d..%d"
        % (len(leaves), len(draws),
           min(a.eventId for a in draws), max(a.eventId for a in draws)))

    csv.write("eid,vm,numidx,vs,block,blocksize,wvpoff,ok,s,a,b,w0,"
              "near,m22,tx,ty,tz,wldx,wldy,wldz\n")

    refl_cache = {}

    def blocks_for(vsid):
        key = str(vsid)
        if key in refl_cache:
            return refl_cache[key]
        info = None
        try:
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
            info = []
            for bi, cb in enumerate(refl.constantBlocks):
                woff = None
                Woff = None
                for v in cb.variables:
                    nm = str(v.name)
                    if woff is None and nm.startswith("WorldViewProj"):
                        woff = int(v.byteOffset)
                    elif Woff is None and nm == "World":
                        Woff = int(v.byteOffset)
                info.append((bi, str(cb.name), int(cb.byteSize), woff, Woff))
        except Exception:
            info = None
        refl_cache[key] = info
        return info

    rows = []
    for i, a in enumerate(draws):
        eid = a.eventId
        if i % 200 == 0:
            say("   ... draw %d/%d (eid %d)" % (i, len(draws), eid))
        try:
            controller.SetFrameEvent(eid, False)
            pipe = controller.GetPipelineState()
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        except Exception:
            continue
        if vsid == rd.ResourceId.Null():
            continue
        info = blocks_for(vsid)
        if not info:
            continue

        # Copy every descriptor field out and read the buffer IMMEDIATELY.
        reads = []
        try:
            uds = pipe.GetConstantBlocks(rd.ShaderStage.Vertex)
        except Exception:
            continue
        for ud in uds:
            d = getattr(ud, "descriptor", None)
            if d is None:
                reads.append(None)
                continue
            rid = d.resource
            boff = int(getattr(d, "byteOffset", 0))
            bsz = int(getattr(d, "byteSize", 0))
            if rid == rd.ResourceId.Null():
                reads.append(None)
                continue
            want = min(bsz, 1024) if bsz else 1024
            try:
                reads.append(bytes(controller.GetBufferData(rid, boff, want)))
            except Exception:
                reads.append(None)

        picked = None
        for (bi, bname, bsize, woff, Woff) in info:
            if woff is None:
                continue
            data = None
            for d in reads:                      # by clamped length first
                if d is not None and len(d) == bsize:
                    data = d
                    break
            if data is None and bi < len(reads) and reads[bi] is not None \
                    and len(reads[bi]) >= woff + 64:
                data = reads[bi]                 # then declaration order
            if data is None:
                for d in reads:
                    if d is not None and len(d) >= woff + 64:
                        data = d
                        break
            if data is None or len(data) < woff + 64:
                continue
            m = struct.unpack_from("<16f", data, woff)
            if not all(map(math.isfinite, m)):
                continue
            picked = (bname, bsize, woff, m, data, Woff)
            break

        vm = 1 if VM_LO <= eid <= VM_HI else 0
        ni = int(getattr(a, "numIndices", 0))
        if picked is None:
            csv.write("%d,%d,%d,%s,,,,0,,,,,,,,,,,,\n" % (eid, vm, ni, vsid))
            rows.append(dict(eid=eid, vm=vm, ni=ni, ok=0))
            continue

        bname, bsize, woff, m, data, Woff = picked
        s = n3(m[3], m[7], m[11])
        cx = n3(m[0], m[4], m[8])
        cy = n3(m[1], m[5], m[9])
        aa = cx / s if s > 1e-9 else float("nan")
        bb = cy / s if s > 1e-9 else float("nan")
        w0 = m[15]
        near = m[14]
        m22 = m[10]
        wx = wy = wz = float("nan")
        if Woff is not None and len(data) >= Woff + 64:
            W = struct.unpack_from("<16f", data, Woff)
            if all(map(math.isfinite, W)):
                wx, wy, wz = W[12], W[13], W[14]

        csv.write("%d,%d,%d,%s,%s,%d,%d,1,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g,"
                  "%.6g,%.6g,%.6g,%.6g,%.6g,%.6g\n"
                  % (eid, vm, ni, vsid, bname, bsize, woff,
                     s, aa, bb, w0, near, m22,
                     m[12], m[13], m[14], wx, wy, wz))
        csv.flush()
        rows.append(dict(eid=eid, vm=vm, ni=ni, ok=1, s=s, a=aa, b=bb,
                         w0=w0, near=near, m22=m22, block=bname, off=woff))

    # ---------------------------------------------------------------- report
    vmr = [r for r in rows if r["vm"]]
    wr = [r for r in rows if not r["vm"]]
    say("")
    say("=" * 74)
    say("drawcalls total %d   viewmodel-range %d   world %d"
        % (len(rows), len(vmr), len(wr)))
    say("  WVP extracted: viewmodel %d/%d, world %d/%d"
        % (sum(r["ok"] for r in vmr), len(vmr),
           sum(r["ok"] for r in wr), len(wr)))

    say("")
    say("== every draw in the viewmodel range ==")
    for r in vmr:
        if r["ok"]:
            say("  eid %-6d %7d idx  %-16s +0x%02X  s=%.4f a=%.4f b=%.4f "
                "w0=%+.4f near=%.5f"
                % (r["eid"], r["ni"], r["block"], r["off"], r["s"], r["a"],
                   r["b"], r["w0"], r["near"]))
        else:
            say("  eid %-6d %7d idx  (no WVP found)" % (r["eid"], r["ni"]))

    def cut(name, key, lo, hi):
        """How many on each side fall inside [lo,hi]?"""
        def inn(r):
            v = r.get(key)
            return r["ok"] and v is not None and math.isfinite(v) \
                and lo <= v <= hi
        tp = sum(1 for r in vmr if inn(r))
        fn = len(vmr) - tp
        fp = sum(1 for r in wr if inn(r))
        tn = len(wr) - fp
        say("  %-34s TP %3d  FN %3d   FP %4d  TN %4d"
            % ("%s in [%g,%g]" % (name, lo, hi), tp, fn, fp, tn))
        if fp:
            bad = [r["eid"] for r in wr if inn(r)]
            say("       world draws that would fire: %s%s"
                % (bad[:25], " ..." if len(bad) > 25 else ""))
        return tp, fn, fp, tn

    say("")
    say("== distribution of b = cot(vFov/2), scale-invariant ==")
    for tag, lst in (("viewmodel", vmr), ("world", wr)):
        vals = sorted(r["b"] for r in lst
                      if r["ok"] and math.isfinite(r.get("b", float("nan"))))
        if not vals:
            say("  %-10s none" % tag)
            continue
        hist = {}
        for v in vals:
            hist[round(v, 3)] = hist.get(round(v, 3), 0) + 1
        top = sorted(hist.items(), key=lambda kv: -kv[1])[:14]
        say("  %-10s n=%d  min %.4f  max %.4f" % (tag, len(vals), vals[0], vals[-1]))
        say("            commonest: %s"
            % "  ".join("%.3f x%d" % (k, c) for k, c in top))

    say("")
    say("== distribution of a = cot(hFov/2) ==")
    for tag, lst in (("viewmodel", vmr), ("world", wr)):
        vals = sorted(r["a"] for r in lst
                      if r["ok"] and math.isfinite(r.get("a", float("nan"))))
        if not vals:
            continue
        hist = {}
        for v in vals:
            hist[round(v, 3)] = hist.get(round(v, 3), 0) + 1
        top = sorted(hist.items(), key=lambda kv: -kv[1])[:14]
        say("  %-10s n=%d  min %.4f  max %.4f" % (tag, len(vals), vals[0], vals[-1]))
        say("            commonest: %s"
            % "  ".join("%.3f x%d" % (k, c) for k, c in top))

    say("")
    say("== distribution of |w0| = eye-to-origin distance ==")
    for tag, lst in (("viewmodel", vmr), ("world", wr)):
        vals = sorted(abs(r["w0"]) for r in lst
                      if r["ok"] and math.isfinite(r.get("w0", float("nan"))))
        if not vals:
            continue
        q = lambda p: vals[min(len(vals) - 1, int(p * len(vals)))]
        say("  %-10s n=%d  min %.4f  p10 %.4f  med %.4f  p90 %.4f  max %.4f"
            % (tag, len(vals), vals[0], q(.10), q(.50), q(.90), vals[-1]))

    say("")
    say("== candidate cuts ==")
    cut("b", "b", 3.0, 3.8)
    cut("b", "b", 2.0, 99.0)
    cut("a", "a", 2.0, 2.4)
    cut("|w0|<2", "w0abs", -2.0, 2.0)
    for r in rows:
        if r["ok"] and math.isfinite(r.get("w0", float("nan"))):
            r["w0abs"] = abs(r["w0"])
    cut("|w0|", "w0abs", 0.0, 2.0)
    cut("|w0|", "w0abs", 0.0, 1.0)
    cut("|w0|", "w0abs", 0.0, 0.5)
    cut("near", "near", 0.0099, 0.0101)

    say("")
    say("== combined: b>2 AND |w0|<2 ==")
    def both(r):
        return r["ok"] and math.isfinite(r.get("b", float("nan"))) \
            and math.isfinite(r.get("w0", float("nan"))) \
            and r["b"] > 2.0 and abs(r["w0"]) < 2.0
    tp = sum(1 for r in vmr if both(r))
    fp = sum(1 for r in wr if both(r))
    say("  TP %d / %d      FP %d / %d" % (tp, len(vmr), fp, len(wr)))
    if fp:
        say("  world draws firing: %s" % [r["eid"] for r in wr if both(r)][:40])

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
csv.close()
os._exit(0)
