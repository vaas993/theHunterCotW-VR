"""Full DXBC of the scope-glass vertex shader (VS 2205) - all 67 lines."""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame28475.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_glass_shader2.txt")

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        return
    for eid, tag in ((19702, "GLASS VS 2205"), (19946, "SOLID VS 2030"),
                     (19751, "SOLID hands VS 1815")):
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        eps = controller.GetShaderEntryPoints(vsid)
        refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        asm = controller.DisassembleShader(
            pipe.GetGraphicsPipelineObject(), refl, "DXBC")
        say("")
        say("=" * 74)
        say("eid %d  %s  VS %s" % (eid, tag, str(vsid)))
        for i, ln in enumerate(asm.splitlines()):
            say("  %3d  %s" % (i, ln.rstrip()))
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
