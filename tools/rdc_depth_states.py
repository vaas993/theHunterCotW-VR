"""Ground truth pass 9 (final): depth states + index counts around the
viewmodel pass.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_depth_states.py

  1. For the prepass-tail depth-only cluster (eids 13152..13224): numIndices,
     depth target, viewport - are these the same weapon meshes (index counts
     matching the viewmodel rounds) or other dynamic objects?
  2. For the viewmodel rounds (31910..32568): depth-stencil state of each draw
     (depthFunc, depthWrite, stencil) - decides whether shifting only the
     color round is viable or the depth rounds must shift too.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_depth_states.txt
"""
import os
import traceback
import zlib

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_depth_states.txt")

PREPASS = range(13140, 13240)
VMPASS = range(31900, 32600)

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def main():
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(CAP, "", None)
    say("OpenFile -> %s" % res)
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open")

    sf = controller.GetStructuredFile()
    actions = controller.GetRootActions()
    leaves = []

    def walk(lst):
        for a in lst:
            kids = getattr(a, "children", [])
            if kids:
                walk(kids)
            else:
                leaves.append(a)

    walk(actions)

    refl_cache = {}

    def crc_of(rid):
        key = str(rid)
        if key in refl_cache:
            return refl_cache[key]
        crc = None
        try:
            entries = controller.GetShaderEntryPoints(rid)
            if entries:
                refl = controller.GetShader(rd.ResourceId.Null(), rid,
                                            entries[0])
                crc = zlib.crc32(bytes(refl.rawBytes)) & 0xFFFFFFFF
        except Exception:
            pass
        refl_cache[key] = crc
        return crc

    def dump(a, with_depth_state):
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        vs = crc_of(pipe.GetShader(rd.ShaderStage.Vertex)) or 0
        ps = crc_of(pipe.GetShader(rd.ShaderStage.Pixel)) or 0
        line = "  %5d idx=%-6d VS %08X PS %08X" % (a.eventId, a.numIndices,
                                                   vs, ps)
        if with_depth_state:
            try:
                d3d = controller.GetD3D11PipelineState()
                ds = d3d.outputMerger.depthState
                line += "  depthEnable=%d func=%s write=%d stencil=%d" % (
                    ds.depthEnable, ds.depthFunction, ds.depthWrites,
                    ds.stencilEnable)
                if ds.stencilEnable:
                    fs = ds.frontFace
                    line += " [front func=%s pass=%s ref=%02X]" % (
                        fs.function, fs.passOperation,
                        d3d.outputMerger.depthStencilState if False else
                        getattr(ds, "frontFace", fs).reference
                        if hasattr(fs, "reference") else 0)
            except Exception as e:
                line += "  depthstate? %s" % e
        say(line)

    say("")
    say("== prepass-tail depth-only cluster ==")
    for a in leaves:
        if a.eventId in PREPASS and (a.flags & rd.ActionFlags.Drawcall):
            dump(a, True)

    say("")
    say("== viewmodel pass draws with depth state ==")
    for a in leaves:
        if a.eventId in VMPASS and (a.flags & rd.ActionFlags.Drawcall):
            dump(a, True)

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
