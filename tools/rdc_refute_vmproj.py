"""REFUTATION SWEEP for the "b = cot(vFov/2) from b1" viewmodel discriminator.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_refute_vmproj.py

WHY THIS IS NOT rdc_vm_proj2.py AGAIN.  That script decided which buffer was
"b1" by asking the SHADER REFLECTION which declared constant block has
fixedBindNumber==1, and SKIPPED the draw when the shader declares none.  It
skipped 559 world draws that way and reported them as "no readable b1".

The mod has no reflection.  At DrawIndexed it calls

    ctx1->VSGetConstantBuffers1(1, 1, &cb, &first, &num);

which returns whatever is bound at D3D11 vertex constant SLOT 1 on the context
right now - including a LEFTOVER binding from an earlier draw whose shader did
declare b1.  A slot binding is context state, it is not cleared because the
current shader ignores it.  So every one of those 559 draws WILL read bytes and
CAN fire.  This script reads the raw per-slot descriptor store
(D3D11State.descriptorStore, location fixedBindNumber==1, stageMask Vertex),
which is the faithful equivalent, and tests EVERY drawcall.

Also measured here:
  * the three rounds of the viewmodel pass separately (depth-only vs colour)
  * the nearest world b to the viewmodel value, i.e. the real margin
  * how many distinct buffers serve b1 and how many draws share each one
    (the shadow-copy / deferred-context aliasing question)
  * b at all 13 offsets vs only {+0x00,+0x40}

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_refute_vmproj_<tag>.txt / .csv
"""
import math
import os
import struct
import sys
import traceback

import renderdoc as rd

TAG = os.environ.get("RDC_TAG", "4317")
CAPS = {
    "4317": (r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc",
             33313, 33956),
    "18616": (r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame18616.rdc",
              31910, 32626),
}
CAP = os.path.expandvars(CAPS[TAG][0])
VM_LO, VM_HI = CAPS[TAG][1], CAPS[TAG][2]
OUT = os.path.expandvars(
    r"%%LOCALAPPDATA%%\theHunterCotWVR\rdc_refute_vmproj_%s.txt" % TAG)
CSV = os.path.expandvars(
    r"%%LOCALAPPDATA%%\theHunterCotWVR\rdc_refute_vmproj_%s.csv" % TAG)

B_LO, B_HI = 3.37, 3.41

out = open(OUT, "w")
csv = open(CSV, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def n3(x, y, z):
    return math.sqrt(x * x + y * y + z * z)


def bval(m):
    """The claim's b, with the mandatory s guard. None if guard rejects."""
    if not all(map(math.isfinite, m)):
        return None
    s = n3(m[3], m[7], m[11])
    if not (1e-4 < s < 1e4):
        return None
    b = n3(m[1], m[5], m[9]) / s
    if not math.isfinite(b):
        return None
    return b


def main():
    cap = rd.OpenCaptureFile()
    say("capture %s" % CAP)
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open;  viewmodel range given as %d..%d" % (VM_LO, VM_HI))

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

    # ---- locate the flat descriptor index of VERTEX constant slot b1 -------
    controller.SetFrameEvent(draws[0].eventId, False)
    st = controller.GetD3D11PipelineState()
    rng = rd.DescriptorRange()
    rng.offset = 0
    rng.count = int(st.descriptorCount)
    locs = controller.GetDescriptorLocations(st.descriptorStore, [rng])
    vs_cb = {}
    for i, L in enumerate(locs):
        if "ConstantBlock" in str(getattr(L, "category", "")) \
                and "Vertex" in str(getattr(L, "stageMask", "")):
            vs_cb[int(L.fixedBindNumber)] = i
    B1 = vs_cb.get(1)
    say("VS constant slot b1 -> flat descriptor index %s" % B1)
    if B1 is None:
        say("cannot locate b1 - abort")
        return

    r1 = rd.DescriptorRange()
    r1.offset = B1
    r1.count = 1

    csv.write("eid,vm,numidx,ps_null,depth,vs,b1res,b1off,nbytes,"
              "b_00,b_40,fire2,fire13,fire13_off,best_b\n")

    rows = []
    b1_users = {}
    for i, a in enumerate(draws):
        eid = a.eventId
        vm = 1 if VM_LO <= eid <= VM_HI else 0
        ni = int(getattr(a, "numIndices", 0))
        if i % 250 == 0:
            say("   ... draw %d/%d (eid %d)" % (i, len(draws), eid))
        try:
            controller.SetFrameEvent(eid, False)
            pipe = controller.GetPipelineState()
            st = controller.GetD3D11PipelineState()
        except Exception:
            continue

        try:
            vsid = str(pipe.GetShader(rd.ShaderStage.Vertex))
            psid = pipe.GetShader(rd.ShaderStage.Pixel)
            ps_null = 1 if psid == rd.ResourceId.Null() else 0
        except Exception:
            vsid, ps_null = "?", -1
        dep = ""
        try:
            ds = pipe.GetDepthTarget()
            if ds is not None and ds.resource != rd.ResourceId.Null():
                dep = str(ds.resource)
        except Exception:
            pass

        # ---- the raw slot-b1 binding, stale or not -----------------------
        b1res, b1off, data = "", 0, None
        try:
            ds1 = controller.GetDescriptors(st.descriptorStore, [r1])
            d = ds1[0]
            rid = d.resource
            b1off = int(getattr(d, "byteOffset", 0))
            if rid != rd.ResourceId.Null():
                b1res = str(rid)
                data = bytes(controller.GetBufferData(rid, b1off, 256))
        except Exception:
            data = None

        if b1res:
            b1_users.setdefault(b1res, []).append(eid)

        nb = len(data) if data else 0
        b00 = b40 = None
        fire13, f13off, best = 0, -1, None
        if data and nb >= 64:
            m = struct.unpack_from("<16f", data, 0)
            b00 = bval(m)
        if data and nb >= 128:
            m = struct.unpack_from("<16f", data, 64)
            b40 = bval(m)
        if data:
            for off in range(0, max(0, min(nb, 256) - 63), 16):
                m = struct.unpack_from("<16f", data, off)
                b = bval(m)
                if b is None:
                    continue
                if best is None or abs(b - 3.38806) < abs(best - 3.38806):
                    best = b
                if B_LO <= b <= B_HI and not fire13:
                    fire13, f13off = 1, off

        def infire(b):
            return b is not None and B_LO <= b <= B_HI
        fire2 = 1 if (infire(b00) or infire(b40)) else 0

        csv.write("%d,%d,%d,%d,%s,%s,%s,%d,%d,%s,%s,%d,%d,%d,%s\n"
                  % (eid, vm, ni, ps_null, dep, vsid, b1res, b1off, nb,
                     "" if b00 is None else "%.6g" % b00,
                     "" if b40 is None else "%.6g" % b40,
                     fire2, fire13, f13off,
                     "" if best is None else "%.6g" % best))
        rows.append(dict(eid=eid, vm=vm, ni=ni, ps_null=ps_null, dep=dep,
                         vs=vsid, b1res=b1res, nb=nb, b00=b00, b40=b40,
                         fire2=fire2, fire13=fire13, f13off=f13off, best=best))
    csv.flush()

    vmr = [r for r in rows if r["vm"]]
    wr = [r for r in rows if not r["vm"]]

    say("")
    say("=" * 74)
    say("TOTAL drawcalls %d   in-range %d   out-of-range %d"
        % (len(rows), len(vmr), len(wr)))
    say("  draws with NO readable slot-b1 buffer: in-range %d, out %d"
        % (sum(1 for r in vmr if r["nb"] == 0),
           sum(1 for r in wr if r["nb"] == 0)))

    say("")
    say("== THE MOD'S TEST: offsets {+0x00,+0x40}, b in [%.2f,%.2f] =="
        % (B_LO, B_HI))
    tp = sum(r["fire2"] for r in vmr)
    fp = sum(r["fire2"] for r in wr)
    say("  in-range  FIRED %d / %d      (false negatives %d)"
        % (tp, len(vmr), len(vmr) - tp))
    say("  out-range FIRED %d / %d      (false positives, pre-triage)"
        % (fp, len(wr)))
    say("  out-of-range draws that fire:")
    for r in wr:
        if r["fire2"]:
            say("    eid %-6d %7d idx  ps_null=%d depth=%-18s b00=%s b40=%s "
                "vs=%s b1=%s"
                % (r["eid"], r["ni"], r["ps_null"], r["dep"],
                   "%.5f" % r["b00"] if r["b00"] is not None else "-",
                   "%.5f" % r["b40"] if r["b40"] is not None else "-",
                   r["vs"], r["b1res"]))

    say("")
    say("== all-13-offsets variant ==")
    say("  in-range FIRED %d / %d ; out-range FIRED %d / %d"
        % (sum(r["fire13"] for r in vmr), len(vmr),
           sum(r["fire13"] for r in wr), len(wr)))
    for r in wr:
        if r["fire13"] and not r["fire2"]:
            say("    extra: eid %-6d %7d idx at +0x%02X" % (r["eid"], r["ni"], r["f13off"]))

    say("")
    say("== the in-range pass, per draw, split by round ==")
    for r in vmr:
        say("  eid %-6d %7d idx  %-10s b00=%-10s b40=%-10s fire=%d"
            % (r["eid"], r["ni"],
               "DEPTHONLY" if r["ps_null"] else "colour",
               "%.5f" % r["b00"] if r["b00"] is not None else "-",
               "%.5f" % r["b40"] if r["b40"] is not None else "-",
               r["fire2"]))
    dep_only = [r for r in vmr if r["ps_null"] == 1]
    col = [r for r in vmr if r["ps_null"] == 0]
    say("  depth-only draws %d, fired %d" % (len(dep_only), sum(r["fire2"] for r in dep_only)))
    say("  colour draws     %d, fired %d" % (len(col), sum(r["fire2"] for r in col)))

    say("")
    say("== histogram of b at +0x00 and +0x40, out-of-range draws ==")
    hist = {}
    for r in wr:
        for b in (r["b00"], r["b40"]):
            if b is None:
                continue
            hist[round(b, 4)] = hist.get(round(b, 4), 0) + 1
    for k, v in sorted(hist.items(), key=lambda kv: -kv[1])[:25]:
        say("   b=%-12s x %d" % (k, v))

    say("")
    say("== MARGIN: out-of-range b values nearest to 3.38806 ==")
    cand = []
    for r in wr:
        for off, b in (("+0x00", r["b00"]), ("+0x40", r["b40"])):
            if b is not None:
                cand.append((abs(b - 3.38806), b, r["eid"], r["ni"], off, r["dep"]))
    cand.sort()
    for d, b, eid, ni, off, dep in cand[:20]:
        say("   |b-3.38806|=%-10.5f b=%-10.5f eid %-6d %6d idx %s depth=%s"
            % (d, b, eid, ni, off, dep))

    say("")
    say("== how many draws share each b1 buffer (shadow-copy aliasing) ==")
    tops = sorted(b1_users.items(), key=lambda kv: -len(kv[1]))[:12]
    say("   %d distinct buffers ever bound at b1" % len(b1_users))
    for rid, eids in tops:
        say("   %-22s used by %4d draws  (eids %d..%d)"
            % (rid, len(eids), min(eids), max(eids)))
    vmres = sorted({r["b1res"] for r in vmr if r["b1res"]})
    say("   buffers at b1 during the viewmodel pass: %s" % vmres)
    for rid in vmres:
        eids = b1_users.get(rid, [])
        inr = [e for e in eids if VM_LO <= e <= VM_HI]
        say("     %-22s %d draws total, %d inside the viewmodel range"
            % (rid, len(eids), len(inr)))

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
