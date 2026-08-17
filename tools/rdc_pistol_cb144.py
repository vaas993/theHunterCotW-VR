"""Which draws in the whole frame bind the 192-byte InstanceConsts buffer?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_pistol_cb144.py

rdc_pistol_rtt.py part (c) reported "0 draws bind ResourceId::144" by reading
d3d.vertexShader.constantBuffers - an attribute that yields nothing on this
build.  That is precisely the 1.45 trap: a wrong attribute name is a confident
false negative.  rdc_pistol_lens.py, using the descriptor API, showed eids
15852 / 15855 / 16549 all bind ResourceId::144 with a REAL length of 192 bytes.

This redoes the frame-wide question through GetConstantBlocks -> UsedDescriptor
(access.index is the descriptor POSITION, matched against the reflection index,
never against the HLSL register), and reports for every draw:
    the VS b-register 1 block name, the buffer resource, its REAL D3D length

Two things ride on the answer:
  * cbscan.cpp refuses any constant buffer smaller than 256 bytes, so a 192-byte
    block cannot be patched today at all
  * if ResourceId::144 is bound ONLY by the scope-lens draws it is a second,
    independent discriminator

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_pistol_cb144.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.environ.get("COTW_CAP") or os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame33848.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_pistol_cb144.txt")

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def main():
    cap = rd.OpenCaptureFile()
    say("capture: %s" % CAP)
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return

    bufl = {}
    for b in controller.GetBuffers():
        bufl[str(b.resourceId)] = int(b.length)

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
    draws.sort(key=lambda a: a.eventId)
    say("%d drawcalls" % len(draws))

    refl_cache = {}

    def reflect(vsid):
        k = str(vsid)
        if k not in refl_cache:
            try:
                eps = controller.GetShaderEntryPoints(vsid)
                refl_cache[k] = controller.GetShader(rd.ResourceId.Null(),
                                                     vsid, eps[0]) if eps else None
            except Exception as e:
                say("   reflect(%s) RAISED %s: %s" % (k, type(e).__name__, e))
                refl_cache[k] = None
        return refl_cache[k]

    rows = []
    nerr = 0
    for i, a in enumerate(draws):
        controller.SetFrameEvent(a.eventId, False)
        pipe = controller.GetPipelineState()
        vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        refl = reflect(vsid)
        if refl is None:
            nerr += 1
            continue
        bi = None
        cbname, cbdecl = "-", 0
        for k, cb in enumerate(refl.constantBlocks):
            if int(cb.fixedBindNumber) == 1:
                bi, cbname, cbdecl = k, cb.name, int(cb.byteSize)
                break
        rid, real, boff = "NULL", -1, 0
        if bi is not None:
            for ud in pipe.GetConstantBlocks(rd.ShaderStage.Vertex):
                acc = getattr(ud, "access", None)
                if acc is None or int(getattr(acc, "index", -1)) != bi:
                    continue
                d = getattr(ud, "descriptor", None)
                if d is None:
                    continue
                rid = str(getattr(d, "resource", "NULL"))
                boff = int(getattr(d, "byteOffset", 0))
                break
            real = bufl.get(rid, -1)
        rows.append((i, a.eventId, int(getattr(a, "numIndices", 0)),
                     str(vsid), cbname, cbdecl, rid, real, boff))
        if i % 200 == 0:
            say("  ... %d/%d" % (i, len(draws)))
    say("  %d draws with no usable VS reflection" % nerr)

    say("")
    say("== every distinct VS b1 buffer resource in the frame ==")
    g = {}
    for r in rows:
        g.setdefault((r[6], r[7], r[4]), []).append(r[1])
    say("   %-18s %10s  %-24s %6s  eids" % ("resource", "real bytes",
                                            "block name", "draws"))
    for k in sorted(g, key=lambda k: -len(g[k])):
        v = g[k]
        say("   %-18s %10d  %-24s %6d  %s%s"
            % (k[0], k[1], k[2], len(v), v[:12], " ..." if len(v) > 12 else ""))

    say("")
    say("== draws whose VS b1 buffer is SMALLER than cbscan's 256-byte floor ==")
    small = [r for r in rows if 0 <= r[7] < 256]
    say("   %d draws" % len(small))
    for r in small:
        say("   pos %-5d eid %-7d idx %-7d vs %-16s %-20s buffer %-16s %d B "
            "(byteOffset %d)" % (r[0], r[1], r[2], r[3], r[4], r[6], r[7], r[8]))

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
