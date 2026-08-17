"""What does this RenderDoc build actually call a constant buffer?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_cb_api.py

rdc_viewmodel_cb.py reported "no camera-near transform in any VS cbuffer" at
every viewmodel draw - but it never printed a single buffer line either, which
means it found NO constant buffers at all, and a bare `except: continue` hid the
reason.  A negative result from a call that never ran is not a negative result.

So this asks the replay directly, at one known viewmodel draw:
  - which pipeline-state methods exist on this build (1.45 moved a lot of the
    resource-access API to descriptors)
  - what GetConstantBuffer raises, printed rather than swallowed
  - what the descriptor API reports for the vertex stage
  - the shader's own reflection: how many cbuffers it declares and how big

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_cb_api.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_cb_api.txt")

EID = 33851          # a FBECF3C0 colour draw, 5958 indices

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

    controller.SetFrameEvent(EID, True)
    pipe = controller.GetPipelineState()

    say("")
    say("== pipeline-state methods mentioning buffer/constant/descriptor ==")
    for n in sorted(dir(pipe)):
        if n.startswith("_"):
            continue
        low = n.lower()
        if any(k in low for k in ("buffer", "constant", "descriptor", "block",
                                  "resource", "shader")):
            say("   %s" % n)

    say("")
    say("== GetConstantBuffer, error printed not swallowed ==")
    for slot in (0, 1, 2):
        try:
            cb = pipe.GetConstantBuffer(rd.ShaderStage.Vertex, slot, 0)
            say("   slot %d -> %r" % (slot, cb))
            for a in dir(cb):
                if not a.startswith("_"):
                    try:
                        say("        .%s = %r" % (a, getattr(cb, a)))
                    except Exception as e:
                        say("        .%s ! %s" % (a, e))
        except Exception as e:
            say("   slot %d RAISED %s: %s" % (slot, type(e).__name__, e))

    say("")
    say("== the descriptor API ==")
    for meth in ("GetConstantBlocks", "GetConstantBuffers",
                 "GetReadOnlyResources", "GetReadWriteResources"):
        fn = getattr(pipe, meth, None)
        if fn is None:
            say("   %s: absent" % meth)
            continue
        try:
            got = fn(rd.ShaderStage.Vertex)
            say("   %s -> %d entries" % (meth, len(got)))
            if len(got):
                e0 = got[0]
                say("        entry type %s" % type(e0).__name__)
                say("        attrs %s"
                    % [a for a in dir(e0) if not a.startswith("_")])
                for i, e in enumerate(got[:8]):
                    d = getattr(e, "descriptor", None)
                    say("        [%d] descriptor=%r" % (i, d))
                    if d is not None:
                        for a in ("resource", "byteOffset", "byteSize",
                                  "elementByteSize", "format"):
                            if hasattr(d, a):
                                say("             .%s = %r" % (a, getattr(d, a)))
        except Exception as e:
            say("   %s RAISED %s: %s" % (meth, type(e).__name__, e))

    say("")
    say("== what the vertex shader itself declares ==")
    try:
        vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        eps = controller.GetShaderEntryPoints(vsid)
        refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        say("   %d constant block(s)" % len(refl.constantBlocks))
        for i, cb in enumerate(refl.constantBlocks):
            say("   [%d] %s  bindPoint=%s  %d bytes  %d variable(s)"
                % (i, cb.name, cb.fixedBindNumber, cb.byteSize,
                   len(cb.variables)))
            for v in cb.variables[:24]:
                say("        +0x%04X %-40s %s rows=%d cols=%d"
                    % (v.byteOffset, v.name, v.type.descriptor.name
                       if hasattr(v.type, "descriptor") else "?",
                       getattr(v.type.descriptor, "rows", 0)
                       if hasattr(v.type, "descriptor") else 0,
                       getattr(v.type.descriptor, "columns", 0)
                       if hasattr(v.type, "descriptor") else 0))
        say("   %d read-only resource(s)" % len(refl.readOnlyResources))
        for r in refl.readOnlyResources[:16]:
            say("        t%s %s" % (r.fixedBindNumber, r.name))
    except Exception as e:
        say("   reflection failed %s: %s" % (type(e).__name__, e))
        say(traceback.format_exc())

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
