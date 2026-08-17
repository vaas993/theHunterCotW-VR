"""Probe: what does this RenderDoc build call the D3D11 depth-stencil state?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_ds_probe.py

rdc_depth_states.py used `d3d.outputMerger.depthState` with a fallback
expression that would silently produce 0 for the stencil reference.  Before
sweeping 2200 draws we spell out exactly which attributes exist, so the sweep
never guesses.  Probed at one viewmodel draw and one world draw.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ds_probe.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ds_probe.txt")

EIDS = [33851, 33313, 20000]

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def dump_obj(o, name, depth=0, seen=None):
    pad = "   " * (depth + 1)
    say("%s%s : %s" % (pad, name, type(o).__name__))
    if depth > 2:
        return
    for a in sorted(dir(o)):
        if a.startswith("_"):
            continue
        try:
            v = getattr(o, a)
        except Exception as e:
            say("%s  .%s ! %s" % (pad, a, e))
            continue
        if callable(v):
            continue
        say("%s  .%s = %r" % (pad, a, v))


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open")

    for eid in EIDS:
        say("")
        say("=" * 70)
        say("== eid %d ==" % eid)
        controller.SetFrameEvent(eid, True)

        d3d = controller.GetD3D11PipelineState()
        say(" D3D11 state attrs: %s"
            % [a for a in dir(d3d) if not a.startswith("_")])

        om = d3d.outputMerger
        say(" outputMerger attrs: %s"
            % [a for a in dir(om) if not a.startswith("_")])
        for cand in ("depthStencilState", "depthState", "depthStencil"):
            o = getattr(om, cand, None)
            if o is not None:
                dump_obj(o, "outputMerger.%s" % cand)
                for face in ("frontFace", "backFace"):
                    f = getattr(o, face, None)
                    if f is not None:
                        dump_obj(f, "  .%s" % face, 1)
        for cand in ("depthReadOnly", "stencilReadOnly", "depthTarget"):
            say("  outputMerger.%s = %r" % (cand, getattr(om, cand, "ABSENT")))

        rs = d3d.rasterizer
        say(" rasterizer attrs: %s"
            % [a for a in dir(rs) if not a.startswith("_")])
        st = getattr(rs, "state", None)
        if st is not None:
            dump_obj(st, "rasterizer.state")
        vps = getattr(rs, "viewports", None)
        if vps is not None:
            say("  %d viewport(s)" % len(vps))
            for i, vp in enumerate(vps[:4]):
                dump_obj(vp, "  viewport[%d]" % i, 1)

        pipe = controller.GetPipelineState()
        vp = pipe.GetViewport(0)
        say("  PipeState.GetViewport(0): x=%r y=%r w=%r h=%r min=%r max=%r"
            % (vp.x, vp.y, vp.width, vp.height, vp.minDepth, vp.maxDepth))
        try:
            say("  PipeState.GetDepthTarget -> %r" % pipe.GetDepthTarget())
        except Exception as e:
            say("  GetDepthTarget raised %s: %s" % (type(e).__name__, e))

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
