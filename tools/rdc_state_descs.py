"""Are the block-exclusive state OBJECTS distinguishable by their DESC?

Run headlessly:
    qrenderdoc.exe --python rdc_state_descs.py

rdc_rs_sweep found three depth-stencil state objects (98509/98510/98511) and one
blend state (98512) used ONLY by the viewmodel pass.  A pointer the mod has
never seen created is useless on its own - but unlike shaders, state objects ARE
introspectable at runtime: ID3D11DepthStencilState::GetDesc() works on any
pointer.  So the question that decides whether this lead is real is: do those
objects carry a DESC that no world state object carries?

This dumps the FULL desc (including every stencil field) of every distinct
rasterizer / blend / depth-stencil state used anywhere in the frame, with how
many draws inside and outside 33313..33956 use it.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_state_descs.txt
"""
import os
import traceback
from collections import defaultdict

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_state_descs.txt")

BLOCK_LO, BLOCK_HI = 33313, 33956

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def en(v):
    s = str(v)
    if "." in s and ":" in s:
        return s.split(".", 1)[1].split(":", 1)[0]
    return s


def face(f):
    return "(fn=%s fail=%s depthFail=%s pass=%s readMask=%02X writeMask=%02X " \
           "ref=%02X)" % (en(f.function), en(f.failOperation),
                          en(f.depthFailOperation), en(f.passOperation),
                          int(getattr(f, "compareMask", 0)),
                          int(getattr(f, "writeMask", 0)),
                          int(getattr(f, "reference", 0)))


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

    ds_desc, rs_desc, bs_desc = {}, {}, {}
    ds_use = defaultdict(lambda: [0, 0])
    rs_use = defaultdict(lambda: [0, 0])
    bs_use = defaultdict(lambda: [0, 0])

    for a in draws:
        eid = a.eventId
        i = 0 if BLOCK_LO <= eid <= BLOCK_HI else 1
        controller.SetFrameEvent(eid, True)
        d3d = controller.GetD3D11PipelineState()

        st = d3d.rasterizer.state
        k = str(st.resourceId)
        rs_use[k][i] += 1
        if k not in rs_desc:
            rs_desc[k] = ("fill=%s cull=%s frontCCW=%d bias=%d biasClamp=%g "
                          "slope=%g depthClip=%d scissor=%d msaa=%d aaLines=%d "
                          "forcedSamples=%d cons=%s"
                          % (en(st.fillMode), en(st.cullMode),
                             st.frontCCW, st.depthBias, st.depthBiasClamp,
                             st.slopeScaledDepthBias, st.depthClip,
                             st.scissorEnable, st.multisampleEnable,
                             st.antialiasedLines, st.forcedSampleCount,
                             en(st.conservativeRasterization)))

        ds = d3d.outputMerger.depthStencilState
        k = str(ds.resourceId)
        ds_use[k][i] += 1
        if k not in ds_desc:
            ds_desc[k] = ("depthEnable=%d depthWrite=%d func=%s stencilEnable=%d"
                          "\n           front %s\n           back  %s"
                          % (ds.depthEnable, ds.depthWrites,
                             en(ds.depthFunction), ds.stencilEnable,
                             face(ds.frontFace), face(ds.backFace)))

        bs = d3d.outputMerger.blendState
        k = str(bs.resourceId)
        bs_use[k][i] += 1
        if k not in bs_desc:
            parts = []
            for j, b in enumerate(list(bs.blends)[:8]):
                cb, ab = b.colorBlend, b.alphaBlend
                parts.append("RT%d[en=%d wm=%X c:%s/%s/%s a:%s/%s/%s]"
                             % (j, b.enabled, b.writeMask,
                                en(cb.source), en(cb.destination),
                                en(cb.operation),
                                en(ab.source), en(ab.destination),
                                en(ab.operation)))
            bs_desc[k] = ("a2c=%d indep=%d sampleMask=%08X\n           %s"
                          % (bs.alphaToCoverage, bs.independentBlend,
                             bs.sampleMask, "\n           ".join(parts[:4])))

    for label, desc, use in (("RASTERIZER", rs_desc, rs_use),
                             ("BLEND", bs_desc, bs_use),
                             ("DEPTH-STENCIL", ds_desc, ds_use)):
        say("")
        say("=" * 78)
        say("== every distinct %s state used in the frame ==" % label)
        # group by desc text so identical descs are visible as collisions
        by_desc = defaultdict(list)
        for k, d in desc.items():
            by_desc[d].append(k)
        for k, cnt in sorted(use.items(), key=lambda kv: -kv[1][0]):
            twins = [x for x in by_desc[desc[k]] if x != k]
            mark = ""
            if cnt[0] and not cnt[1]:
                mark = "   <<< used ONLY inside the viewmodel block"
            say("")
            say("  %-20s IN %3d  OUT %5d%s" % (k, cnt[0], cnt[1], mark))
            say("      desc: %s" % desc[k])
            if twins:
                tw_in = sum(use[t][0] for t in twins)
                tw_out = sum(use[t][1] for t in twins)
                say("      DESC-TWINS (byte-identical desc, different object): "
                    "%s -> IN %d OUT %d" % (twins, tw_in, tw_out))
            else:
                say("      desc is UNIQUE among all %s states in the frame"
                    % label)

    say("")
    say("=" * 78)
    say("== desc-level verdict ==")
    for label, desc, use in (("RASTERIZER", rs_desc, rs_use),
                             ("BLEND", bs_desc, bs_use),
                             ("DEPTH-STENCIL", ds_desc, ds_use)):
        by_desc = defaultdict(lambda: [0, 0, []])
        for k, d in desc.items():
            by_desc[d][0] += use[k][0]
            by_desc[d][1] += use[k][1]
            by_desc[d][2].append(k)
        say("")
        say("  -- %s, collapsed to DESC value --" % label)
        for d, (ci, co, ks) in sorted(by_desc.items(), key=lambda kv: -kv[1][0]):
            if ci == 0:
                continue
            say("     IN %3d  OUT %5d  objects=%s" % (ci, co, ks))
            say("        %s" % d)

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
