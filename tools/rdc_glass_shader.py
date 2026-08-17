"""Disassemble the scope-glass vertex shader: which cb1 registers does it read?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_glass_shader.py

The point: the proof already showed SV_POSITION comes from the 4x4 at +0x40
(WorldViewProjection).  This shows whether the shader ALSO reads +0x00 (World,
cb1[0..3]) - because that is the field the mod is currently writing, and it
decides whether the visible symptom is the lens outline moving or the lens's
shaded content moving.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_glass_shader.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame28475.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_glass_shader.txt")

EIDS = [19702, 20439]

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

    for eid in EIDS:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        eps = controller.GetShaderEntryPoints(vsid)
        refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        say("")
        say("=" * 74)
        say("eid %d  VS %s" % (eid, str(vsid)))

        targets = controller.GetDisassemblyTargets(True)
        say("disassembly targets: %s" % [str(t) for t in targets])
        asm = ""
        for t in targets:
            try:
                asm = controller.DisassembleShader(
                    pipe.GetGraphicsPipelineObject(), refl, t)
                if asm:
                    say("(target %s)" % t)
                    break
            except Exception as e:
                say("DisassembleShader(%s) failed %s: %s"
                    % (t, type(e).__name__, e))
        if not asm:
            say("no disassembly")
            continue

        lines = asm.splitlines()
        say("%d lines; every line mentioning cb1 or o0.xyzw:" % len(lines))
        for i, ln in enumerate(lines):
            if "cb1[" in ln or "o0." in ln or ln.strip().startswith("dcl_"):
                say("  %4d  %s" % (i, ln.rstrip()))

        # which cb1 registers appear at all
        regs = set()
        for ln in lines:
            p = 0
            while True:
                p = ln.find("cb1[", p)
                if p < 0:
                    break
                q = ln.find("]", p)
                if q > 0:
                    regs.add(ln[p + 4:q])
                p = q + 1
        say("")
        say("cb1 registers referenced: %s"
            % sorted(regs, key=lambda s: (len(s), s)))
        say("  cb1[0..3]   = bytes +0x00..+0x3F  (World)")
        say("  cb1[4..7]   = bytes +0x40..+0x7F  (WorldViewProjection)")
        say("  cb1[8..11]  = bytes +0x80..+0xBF  (PrevWorldViewProjection)")

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
