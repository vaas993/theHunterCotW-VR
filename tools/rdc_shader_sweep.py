"""Ground truth pass 8: are the viewmodel-pass shaders exclusive to it?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_shader_sweep.py

Walks EVERY draw in the frame and records VS/PS CRCs.  If any draw OUTSIDE
eid 31900..32600 binds one of the late-pass shaders, a shift keyed on them
would bleed into that pass (worst case: the shadow cascades).  Slow (per-draw
replay) but this is the last offline question.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_shader_sweep.txt
"""
import os
import traceback
import zlib

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_shader_sweep.txt")

# The late viewmodel pass shaders (from pass 7):
#   color PS:  FBECF3C0 F616CE07 7A86206F
#   color VS:  11F22A82 DD360195 D2EBA766 040F62D1
#   depth VS:  F2F0B7D5 C3904B15 F2D11D69 (PS = null)
WATCH = {0xFBECF3C0, 0xF616CE07, 0x7A86206F,
         0x11F22A82, 0xDD360195, 0xD2EBA766, 0x040F62D1,
         0xF2F0B7D5, 0xC3904B15, 0xF2D11D69}
PASS_LO, PASS_HI = 31900, 32600

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

    n_draws = 0
    hits_outside = []
    trio_events = []
    for a in leaves:
        if not (a.flags & rd.ActionFlags.Drawcall):
            continue
        n_draws += 1
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        vs_crc = crc_of(pipe.GetShader(rd.ShaderStage.Vertex)) or 0
        ps_crc = crc_of(pipe.GetShader(rd.ShaderStage.Pixel)) or 0
        inside = PASS_LO <= a.eventId <= PASS_HI
        vs_hit = vs_crc in WATCH
        ps_hit = ps_crc in WATCH
        if (vs_hit or ps_hit):
            trio_events.append((a.eventId, vs_crc, ps_crc, inside))
            if not inside:
                hits_outside.append((a.eventId, vs_crc, ps_crc,
                                     a.GetName(sf)[:44]))
        if n_draws % 250 == 0:
            say("  ... %d draws scanned" % n_draws)

    say("")
    say("scanned %d draws" % n_draws)
    say("")
    say("== all events binding watched shaders ==")
    for eid, vs, ps, inside in trio_events:
        say("  %5d VS %08X PS %08X %s"
            % (eid, vs, ps, "in-pass" if inside else "**OUTSIDE**"))
    say("")
    if hits_outside:
        say("== %d draws OUTSIDE the viewmodel pass bind watched shaders =="
            % len(hits_outside))
        for eid, vs, ps, name in hits_outside:
            say("  %5d VS %08X PS %08X %s" % (eid, vs, ps, name))
    else:
        say("EXCLUSIVE: no draw outside eid %d..%d binds any watched shader."
            % (PASS_LO, PASS_HI))

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
