"""Probe: what does this RenderDoc build call rasterizer / blend / viewport?

Run headlessly:
    qrenderdoc.exe --python rdc_rs_probe.py

No guessing at attribute names.  Dump dir() of the D3D11 rasterizer state, the
blend state and one viewport at a known viewmodel draw and at a known world
draw, so the real sweep can hardcode the right spellings.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_rs_probe.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_rs_probe.txt")

EIDS = [33851, 33313, 20000]

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def dump_obj(label, o, depth=0):
    pad = "   " * (depth + 1)
    if o is None:
        say("%s%s = None" % (pad, label))
        return
    say("%s%s : %s" % (pad, label, type(o).__name__))
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
        say("=" * 74)
        say("EID %d" % eid)
        controller.SetFrameEvent(eid, True)
        d3d = controller.GetD3D11PipelineState()

        say("-- d3d top-level attrs --")
        for a in sorted(dir(d3d)):
            if not a.startswith("_"):
                say("     %s" % a)

        rs = getattr(d3d, "rasterizer", None)
        say("-- rasterizer container --")
        if rs is not None:
            for a in sorted(dir(rs)):
                if a.startswith("_"):
                    continue
                try:
                    v = getattr(rs, a)
                except Exception as e:
                    say("     .%s ! %s" % (a, e))
                    continue
                if callable(v):
                    continue
                say("     .%s = %r" % (a, v))
            dump_obj("rasterizer.state", getattr(rs, "state", None))
            vps = getattr(rs, "viewports", None)
            if vps is not None:
                say("     viewports: %d" % len(vps))
                for i, vp in enumerate(list(vps)[:8]):
                    dump_obj("viewport[%d]" % i, vp)
            scs = getattr(rs, "scissors", None)
            if scs is not None:
                say("     scissors: %d" % len(scs))
                for i, sc in enumerate(list(scs)[:8]):
                    dump_obj("scissor[%d]" % i, sc)

        om = getattr(d3d, "outputMerger", None)
        say("-- outputMerger container --")
        if om is not None:
            for a in sorted(dir(om)):
                if a.startswith("_"):
                    continue
                try:
                    v = getattr(om, a)
                except Exception as e:
                    say("     .%s ! %s" % (a, e))
                    continue
                if callable(v):
                    continue
                say("     .%s = %r" % (a, v))
            bs = getattr(om, "blendState", None)
            dump_obj("outputMerger.blendState", bs)
            if bs is not None:
                bl = getattr(bs, "blends", None)
                if bl is not None:
                    say("     blends: %d" % len(bl))
                    for i, b in enumerate(list(bl)[:4]):
                        dump_obj("blend[%d]" % i, b)
                        for sub in ("colorBlend", "alphaBlend"):
                            dump_obj("blend[%d].%s" % (i, sub),
                                     getattr(b, sub, None), 1)

        # generic PipeState viewport, as used elsewhere in tools/
        pipe = controller.GetPipelineState()
        try:
            vp = pipe.GetViewport(0)
            say("-- PipeState.GetViewport(0) --")
            dump_obj("vp", vp)
        except Exception as e:
            say("PipeState.GetViewport RAISED %s: %s" % (type(e).__name__, e))

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
