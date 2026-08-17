"""Probe: can we read the RAW D3D11 VS constant-buffer SLOT array (context
state, including bindings the current shader does not declare)?

The runtime check does VSGetConstantBuffers1(1,1,...) which returns whatever is
bound at slot b1 on the context - a leftover from an earlier draw if this
shader never sets b1.  rdc_vm_proj2.py instead asked the SHADER REFLECTION for
"which descriptor position has fixedBindNumber==1" and skipped the draw when
the shader declared no b1 (559 world draws skipped).  That is not what the mod
does.  This probe finds the API that gives per-slot context state.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ref_probe.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ref_probe.txt")

EIDS = [33313, 33851, 34005]

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
    say("replay open")
    say("API: %s" % controller.GetAPIProperties().pipelineType)

    controller.SetFrameEvent(EIDS[0], False)

    say("")
    say("== controller methods with 'Pipeline' or 'Descriptor' ==")
    for n in sorted(dir(controller)):
        if n.startswith("_"):
            continue
        if "ipeline" in n or "escriptor" in n:
            say("   %s" % n)

    say("")
    say("== GetD3D11PipelineState ==")
    fn = getattr(controller, "GetD3D11PipelineState", None)
    if fn is None:
        say("   ABSENT")
    else:
        st = fn()
        say("   type %s" % type(st).__name__)
        say("   attrs %s" % [a for a in dir(st) if not a.startswith("_")])
        vs = getattr(st, "vertexShader", None)
        if vs is not None:
            say("   vertexShader attrs %s"
                % [a for a in dir(vs) if not a.startswith("_")])
            cbs = getattr(vs, "constantBuffers", None)
            say("   constantBuffers = %r" % (type(cbs).__name__ if cbs is not None else None))
            if cbs is not None:
                say("   len = %d" % len(cbs))
                for i, cb in enumerate(cbs[:6]):
                    say("     [%d] attrs %s" % (i, [a for a in dir(cb) if not a.startswith("_")]))
                    for a in ("resourceId", "resource", "vecOffset", "vecCount",
                              "byteOffset", "byteSize"):
                        if hasattr(cb, a):
                            say("          .%s = %r" % (a, getattr(cb, a)))

    say("")
    say("== per-eid: raw slot array for the vertex stage ==")
    for eid in EIDS:
        controller.SetFrameEvent(eid, False)
        say("  --- eid %d ---" % eid)
        st = controller.GetD3D11PipelineState()
        vs = st.vertexShader
        for i, cb in enumerate(vs.constantBuffers):
            rid = getattr(cb, "resourceId", None)
            if rid is None:
                rid = getattr(cb, "resource", None)
            vo = getattr(cb, "vecOffset", None)
            vc = getattr(cb, "vecCount", None)
            say("     b%-2d %s vecOffset=%s vecCount=%s" % (i, rid, vo, vc))

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
