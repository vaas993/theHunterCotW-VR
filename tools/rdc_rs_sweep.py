"""Rasterizer + blend + viewport state of EVERY drawcall in the frame.

Run headlessly:
    qrenderdoc.exe --python rdc_rs_sweep.py

Question: is there any rasterizer state (cull, depth bias, depth clip, scissor),
blend state (enable, write mask) or viewport value that is TRUE for essentially
all viewmodel draws in eid 33313..33956 and FALSE for essentially all ~2180
other draws in the frame?

Everything is read out of the SWIG proxies into plain tuples immediately - the
proxies are temporaries and must not be held across iterations.

Outputs:
    %LOCALAPPDATA%\theHunterCotWVR\rdc_rs_sweep.txt   (report)
    %LOCALAPPDATA%\theHunterCotWVR\rdc_rs_sweep.csv   (per-draw raw)
"""
import os
import traceback
from collections import Counter, defaultdict

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_rs_sweep.txt")
CSV = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_rs_sweep.csv")

BLOCK_LO, BLOCK_HI = 33313, 33956

out = open(OUT, "w")
csv = open(CSV, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def en(v):
    """Enum -> short readable name."""
    s = str(v)
    if "." in s and ":" in s:
        return s.split(".", 1)[1].split(":", 1)[0]
    return s


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
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
    draws = [a for a in leaves if a.flags & rd.ActionFlags.Drawcall]
    say("%d drawcalls in the frame" % len(draws))

    inside = [a for a in draws if BLOCK_LO <= a.eventId <= BLOCK_HI]
    outside = [a for a in draws if not (BLOCK_LO <= a.eventId <= BLOCK_HI)]
    say("  in  %d..%d : %d draws" % (BLOCK_LO, BLOCK_HI, len(inside)))
    say("  outside    : %d draws" % len(outside))

    csv.write("eid,inblock,numIndices,numInstances,name,"
              "rsId,cull,fill,frontCCW,depthBias,depthBiasClamp,slopeBias,"
              "depthClip,scissorEnable,multisample,aaLines,forcedSampleCount,"
              "consRaster,"
              "bsId,alphaToCoverage,independentBlend,sampleMask,blendFactor,"
              "rt0enabled,rt0writeMask,rt0src,rt0dst,rt0op,writeMasks,"
              "enabledMask,"
              "vpCount,vp0x,vp0y,vp0w,vp0h,vp0min,vp0max,vpAllSame,"
              "sc0enabled,sc0x,sc0y,sc0w,sc0h,"
              "dsId,depthEnable,depthWrite,depthFunc\n")

    # dimension -> value -> [in_count, out_count, sample_eids_in, sample_eids_out]
    stats = defaultdict(lambda: defaultdict(lambda: [0, 0, [], []]))
    rows = []

    for a in draws:
        eid = a.eventId
        inb = BLOCK_LO <= eid <= BLOCK_HI
        controller.SetFrameEvent(eid, True)
        d3d = controller.GetD3D11PipelineState()

        rs = d3d.rasterizer
        st = rs.state
        rsId = str(st.resourceId)
        cull = en(st.cullMode)
        fill = en(st.fillMode)
        frontCCW = bool(st.frontCCW)
        dbias = int(st.depthBias)
        dbclamp = float(st.depthBiasClamp)
        sbias = float(st.slopeScaledDepthBias)
        dclip = bool(st.depthClip)
        scen = bool(st.scissorEnable)
        msaa = bool(st.multisampleEnable)
        aal = bool(st.antialiasedLines)
        fsc = int(st.forcedSampleCount)
        cons = en(st.conservativeRasterization)

        vps = []
        for vp in rs.viewports:
            if not vp.enabled:
                continue
            vps.append((round(float(vp.x), 3), round(float(vp.y), 3),
                        round(float(vp.width), 3), round(float(vp.height), 3),
                        round(float(vp.minDepth), 6),
                        round(float(vp.maxDepth), 6)))
        vp0 = vps[0] if vps else (0, 0, 0, 0, 0, 0)
        vp_all_same = len(set(vps)) <= 1

        scs = []
        for sc in rs.scissors:
            scs.append((bool(sc.enabled), int(sc.x), int(sc.y),
                        int(sc.width), int(sc.height)))
        sc0 = scs[0] if scs else (False, 0, 0, 0, 0)

        om = d3d.outputMerger
        bs = om.blendState
        bsId = str(bs.resourceId)
        a2c = bool(bs.alphaToCoverage)
        indep = bool(bs.independentBlend)
        smask = int(bs.sampleMask)
        bf = tuple(round(float(x), 3) for x in bs.blendFactor)

        # How many RTs are actually bound - masks past that are meaningless.
        nrt = 0
        for i, d in enumerate(om.renderTargets):
            if d.resource != rd.ResourceId.Null():
                nrt = i + 1

        wmasks, enmask = [], []
        rt0 = (False, 0, "?", "?", "?")
        for i, b in enumerate(bs.blends):
            wmasks.append(int(b.writeMask))
            enmask.append(1 if b.enabled else 0)
            if i == 0:
                cb = b.colorBlend
                rt0 = (bool(b.enabled), int(b.writeMask),
                       en(cb.source), en(cb.destination), en(cb.operation))
        wm_bound = tuple(wmasks[:max(nrt, 1)])
        en_bound = tuple(enmask[:max(nrt, 1)])

        ds = om.depthStencilState
        dsId = str(ds.resourceId)
        dEn = bool(ds.depthEnable)
        dWr = bool(ds.depthWrites)
        dFn = en(ds.depthFunction)

        ps_bound = d3d.pixelShader.resourceId != rd.ResourceId.Null()

        name = a.GetName(sf).replace(",", ";")[:60]
        ni = getattr(a, "numIndices", 0)
        nins = getattr(a, "numInstances", 0)

        csv.write("%d,%d,%d,%d,%s,%s,%s,%s,%d,%d,%g,%g,%d,%d,%d,%d,%d,%s,"
                  "%s,%d,%d,%d,%s,%d,%d,%s,%s,%s,%s,%s,"
                  "%d,%g,%g,%g,%g,%g,%g,%d,%d,%d,%d,%d,%d,%s,%d,%d,%s\n"
                  % (eid, 1 if inb else 0, ni, nins, name,
                     rsId, cull, fill, 1 if frontCCW else 0, dbias, dbclamp,
                     sbias, 1 if dclip else 0, 1 if scen else 0,
                     1 if msaa else 0, 1 if aal else 0, fsc, cons,
                     bsId, 1 if a2c else 0, 1 if indep else 0, smask,
                     "|".join(str(x) for x in bf),
                     1 if rt0[0] else 0, rt0[1], rt0[2], rt0[3], rt0[4],
                     "|".join(str(x) for x in wm_bound),
                     "|".join(str(x) for x in en_bound),
                     len(vps), vp0[0], vp0[1], vp0[2], vp0[3], vp0[4], vp0[5],
                     1 if vp_all_same else 0,
                     1 if sc0[0] else 0, sc0[1], sc0[2], sc0[3], sc0[4],
                     dsId, 1 if dEn else 0, 1 if dWr else 0, dFn))

        rows.append((eid, inb, ni, name, ps_bound))

        def rec(dim, val):
            s = stats[dim][val]
            s[0 if inb else 1] += 1
            (s[2] if inb else s[3]).append(eid)

        rec("RS.resourceId", rsId)
        rec("RS.cullMode", cull)
        rec("RS.fillMode", fill)
        rec("RS.frontCCW", frontCCW)
        rec("RS.depthBias", dbias)
        rec("RS.depthBiasClamp", dbclamp)
        rec("RS.slopeScaledDepthBias", sbias)
        rec("RS.depthClip", dclip)
        rec("RS.scissorEnable", scen)
        rec("RS.multisampleEnable", msaa)
        rec("RS.antialiasedLines", aal)
        rec("RS.forcedSampleCount", fsc)
        rec("RS.conservativeRaster", cons)
        rec("RS.(bias,slope,clip)", (dbias, sbias, dclip))

        rec("BS.resourceId", bsId)
        rec("BS.alphaToCoverage", a2c)
        rec("BS.independentBlend", indep)
        rec("BS.sampleMask", smask)
        rec("BS.blendFactor", bf)
        rec("BS.rt0.enabled", rt0[0])
        rec("BS.rt0.writeMask", rt0[1])
        rec("BS.rt0.equation", (rt0[2], rt0[3], rt0[4]))
        rec("BS.writeMasks(bound RTs)", wm_bound)
        rec("BS.enabled(bound RTs)", en_bound)
        rec("BS.numBoundRTs", nrt)
        rec("BS.anyBlendEnabled", any(enmask[:max(nrt, 1)]))

        rec("VP.count", len(vps))
        rec("VP[0]", vp0)
        rec("VP[0].xy", (vp0[0], vp0[1]))
        rec("VP[0].wh", (vp0[2], vp0[3]))
        rec("VP[0].depthRange", (vp0[4], vp0[5]))
        rec("VP.allSame", vp_all_same)
        rec("SC[0]", sc0)
        rec("SC.enabledRect", (scen, sc0))

        rec("DS.resourceId", dsId)
        rec("DS.(enable,write,func)", (dEn, dWr, dFn))
        rec("PS.bound", ps_bound)

    csv.close()

    N_IN = len(inside)
    N_OUT = len(outside)

    say("")
    say("=" * 78)
    say("== per-dimension value tables ==")
    say("   IN  = draws inside %d..%d (total %d)" % (BLOCK_LO, BLOCK_HI, N_IN))
    say("   OUT = every other drawcall     (total %d)" % N_OUT)

    verdicts = []
    for dim in sorted(stats):
        vals = stats[dim]
        say("")
        say("-- %s  (%d distinct value(s)) --" % (dim, len(vals)))
        for val, s in sorted(vals.items(), key=lambda kv: -kv[1][0]):
            frac_in = 100.0 * s[0] / N_IN if N_IN else 0.0
            frac_out = 100.0 * s[1] / N_OUT if N_OUT else 0.0
            flag = ""
            if s[0] and not s[1]:
                flag = "   <<< EXCLUSIVE to the viewmodel block"
            elif s[0] == N_IN and s[1] <= 5:
                flag = "   <<< all IN, only %d OUT" % s[1]
            say("   %-46s IN %4d (%5.1f%%)  OUT %5d (%5.1f%%)%s"
                % (str(val)[:46], s[0], frac_in, s[1], frac_out, flag))
            if s[0] and s[3]:
                say("        first OUT eids: %s" % s[3][:10])
            if s[0] and not s[1]:
                verdicts.append((dim, val, s[0], s[1]))
            elif s[0] == N_IN and s[1] > 0:
                verdicts.append((dim, val, s[0], s[1]))

    say("")
    say("=" * 78)
    say("== candidate discriminators (value covers ALL or nearly all IN draws) ==")
    if not verdicts:
        say("   NONE. No single value in this dimension covers the block.")
    for dim, val, ci, co in sorted(verdicts, key=lambda v: (v[3], -v[2])):
        prec = 100.0 * ci / (ci + co) if (ci + co) else 0.0
        rec = 100.0 * ci / N_IN if N_IN else 0.0
        say("   %-28s = %-34s recall %5.1f%%  precision %5.1f%%  "
            "(FP %d, FN %d)"
            % (dim, str(val)[:34], rec, prec, co, N_IN - ci))

    say("")
    say("=" * 78)
    say("== every draw in the block, for reference ==")
    for eid, inb, ni, name, psb in rows:
        if inb:
            say("   %-6d %7d idx  ps=%s  %s" % (eid, ni, "Y" if psb else "-",
                                                name))

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
