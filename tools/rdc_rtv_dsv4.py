"""Verification: is "no RTV bound + main-scene DSV" really exclusive, and is the
DSV read-only flag a second discriminator?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_rtv_dsv4.py

rdc_rtv_dsv3.py found that among the 778 ID3D11DeviceContext::DrawIndexed calls
in the frame, the binding (zero render targets, depth 18561) is used by the 9
viewmodel depth-prepass draws and by nothing else - the 80 world draws that share
it all arrive via DrawInstancedIndirect, which the mod's hook never sees.  Before
that is reported as a discriminator it has to survive three checks:

  1. "zero RTVs" must mean all EIGHT OM slots null, not just slot 0, and no UAV
     smuggled in through OMSetRenderTargetsAndUnorderedAccessViews - otherwise
     the hook's OMGetRenderTargets test does not mean what this analysis means.
  2. The 80 indirect sharers must be indirect for a STRUCTURAL reason (the
     engine's depth prepass is GPU-driven) and not by luck of this scene.  If
     the prepass is 100% indirect over its whole 2,300-event span that is an
     engine property; if it is mixed, the zero false positives are scene luck
     and the discriminator is worthless in the next clearing.
  3. Whether the DSV is bound READ-ONLY anywhere - D3D11_DSV_READ_ONLY_DEPTH is
     one bit in the view's desc and would be a second, independent handle on the
     119 DrawIndexed false positives that share the colour round's targets.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_rtv_dsv4.txt
"""
import os
import re
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_rtv_dsv4.txt")

VM_LO, VM_HI = 33313, 33956
VM_ROUND1 = [33313, 33338, 33344, 33352, 33373, 33380, 33387, 33394, 33406]
PREPASS_LO, PREPASS_HI = 11200, 13690

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
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
    draws = [a for a in leaves if a.flags & rd.ActionFlags.Drawcall]

    def entry(a):
        m = re.search(r"::(\w+)\s*\(", a.GetName(sf))
        return m.group(1) if m else "?"

    # ---- check 1: all eight OM slots + UAVs at the round-1 draws ----------
    say("")
    say("== check 1: the full output-merger state at each round-1 draw ==")
    for eid in VM_ROUND1:
        controller.SetFrameEvent(eid, True)
        p = controller.GetPipelineState()
        slots = []
        for i, ud in enumerate(p.GetOutputTargets()):
            d = getattr(ud, "descriptor", ud)
            r = getattr(d, "resource", None)
            slots.append("null" if (r is None or r == rd.ResourceId.Null())
                         else str(r))
        dt = p.GetDepthTarget()
        dd = getattr(dt, "descriptor", dt)
        dres = str(getattr(dd, "resource", "?"))
        dflags = str(getattr(dd, "flags", "?"))
        uavs = []
        try:
            for ud in p.GetReadWriteResources(rd.ShaderStage.Pixel):
                d = getattr(ud, "descriptor", None)
                if d is None:
                    continue
                r = getattr(d, "resource", None)
                if r is not None and r != rd.ResourceId.Null():
                    uavs.append(str(r))
        except Exception as e:
            uavs = ["ERR %s" % e]
        psid = p.GetShader(rd.ShaderStage.Pixel)
        say("   eid %-6d rtv slots %s" % (eid, slots))
        say("            dsv %s flags=%s   PS=%s   PS UAVs=%s"
            % (dres, dflags, "NULL" if psid == rd.ResourceId.Null() else psid,
               uavs or "none"))

    # ---- check 2: is the depth prepass structurally indirect? -------------
    say("")
    say("== check 2: entry points of every draw in the depth prepass ==")
    pre = [a for a in draws if PREPASS_LO <= a.eventId <= PREPASS_HI]
    fam = {}
    for a in pre:
        e = entry(a)
        base = ("DrawIndexed" if e == "DrawIndexed" else
                "DrawIndexedInstanced" if e == "DrawIndexedInstanced" else
                "indirect" if "Indirect" in e else e)
        fam[base] = fam.get(base, 0) + 1
    say("   %d draws in eid %d..%d" % (len(pre), PREPASS_LO, PREPASS_HI))
    for k, v in sorted(fam.items(), key=lambda kv: -kv[1]):
        say("      %-26s %4d" % (k, v))
    nodirect = [a for a in pre if entry(a) == "DrawIndexed"]
    say("   plain DrawIndexed inside the prepass: %d  %s"
        % (len(nodirect), [a.eventId for a in nodirect][:20]))

    # every draw in the whole frame with zero RTVs, by entry point + dsv
    say("")
    say("== every draw in the FRAME with zero render targets bound ==")
    zero = {}
    for a in draws:
        outs = [o for o in a.outputs if o != rd.ResourceId.Null()]
        if outs:
            continue
        k = (str(a.depthOut), entry(a) == "DrawIndexed")
        s = zero.setdefault(k, [[], []])
        s[0 if VM_LO <= a.eventId <= VM_HI else 1].append(a.eventId)
    textures = {t.resourceId: t for t in controller.GetTextures()}

    def tinfo(rid_str):
        for rid, t in textures.items():
            if str(rid) == rid_str:
                return "%dx%d %s%s" % (t.width, t.height, t.format.Name(),
                                       " arr%d" % t.arraysize
                                       if t.arraysize > 1 else "")
        return "?"
    for (d, is_di), (i, o) in sorted(zero.items(), key=lambda kv: -len(kv[1][0])):
        say("   dsv %-20s %-22s DrawIndexed=%-5s in=%-4d out=%-4d %s"
            % (d, tinfo(d), is_di, len(i), len(o),
               "<<<< EXCLUSIVE" if i and not o else ""))

    # ---- check 3: DSV read-only flags across the frame --------------------
    say("")
    say("== check 3: DSV descriptor flags, over every DrawIndexed in the frame ==")
    flagmap = {}
    di = [a for a in draws if entry(a) == "DrawIndexed"]
    say("   %d DrawIndexed calls to sample" % len(di))
    for a in di:
        controller.SetFrameEvent(a.eventId, True)
        p = controller.GetPipelineState()
        dt = p.GetDepthTarget()
        dd = getattr(dt, "descriptor", dt)
        r = str(getattr(dd, "resource", "?"))
        f = str(getattr(dd, "flags", "?"))
        nrtv = 0
        for ud in p.GetOutputTargets():
            d2 = getattr(ud, "descriptor", ud)
            rr = getattr(d2, "resource", None)
            if rr is not None and rr != rd.ResourceId.Null():
                nrtv += 1
        k = (r, f, nrtv)
        s = flagmap.setdefault(k, [[], []])
        s[0 if VM_LO <= a.eventId <= VM_HI else 1].append(a.eventId)
    say("")
    say("   (depth resource, dsv flags, #RTVs bound) -> in / out")
    for (r, f, n), (i, o) in sorted(flagmap.items(), key=lambda kv: -len(kv[1][0])):
        say("   %-20s %-32s rtvs=%d  in=%-4d out=%-5d %s"
            % (r, f, n, len(i), len(o),
               "<<<< EXCLUSIVE" if i and not o else ""))

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
