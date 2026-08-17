"""Every round the GLASS mesh is drawn in, and whether the mod can reach it.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_glass_rounds.py
    set COTW_CAP=... for the other capture

The glass draws found by the mod's forward-only window are only the ones AFTER a
tagged draw.  In the scope capture the same vertex shader and the same index
count also appear a couple of draws BEFORE the tagged pass (pixel shader null =
a depth/velocity round).  If a mesh is shifted in one round and not another it
depth-fights itself, so this finds every draw in the frame that shares the glass
vertex shader, dumps its b1 block, and checks whether its clip matrix is the
same one.

It also prints the REAL D3D buffer length behind each binding.  The mod refuses
to snapshot any constant buffer smaller than 256 bytes
(cbscan.cpp: `t_size >= kInstanceConstsSize`), so a 128- or 192-byte glass block
is invisible to it and the draw is skipped entirely - which changes what the
observed symptom can possibly be.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_glass_rounds_<capname>.txt
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.environ.get("COTW_CAP") or os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame18022.rdc")
TAG = os.path.splitext(os.path.basename(CAP))[0]
OUT = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_glass_rounds_" + TAG + ".txt")

# vertex shaders that draw glass, measured in rdc_glass_wvp_audit
GLASS_VS = ("ResourceId::2302", "ResourceId::2205")

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def en(v):
    return str(v).split(".")[-1].split(":")[0]


def shape_of(v):
    t = getattr(v, "type", None)
    for holder in (t, getattr(t, "descriptor", None)):
        if holder is None:
            continue
        r = getattr(holder, "rows", None)
        c = getattr(holder, "columns", None)
        if r:
            return int(r), int(c or 1), ""
    return 0, 0, ""


def is_affine(m):
    return m[3] == 0.0 and m[7] == 0.0 and m[11] == 0.0 and m[15] == 1.0


def main():
    cap = rd.OpenCaptureFile()
    say("capture: %s" % CAP)
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return

    bufsize = {}
    for b in controller.GetBuffers():
        bufsize[str(b.resourceId)] = int(b.length)

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

    hits = []
    for i, a in enumerate(draws):
        controller.SetFrameEvent(a.eventId, False)
        d3d = controller.GetD3D11PipelineState()
        vs = str(d3d.vertexShader.resourceId)
        if vs in GLASS_VS:
            ds = d3d.outputMerger.depthStencilState
            f = ds.frontFace
            b0 = d3d.outputMerger.blendState.blends[0] \
                if len(d3d.outputMerger.blendState.blends) else None
            hits.append((i, a.eventId, int(getattr(a, "numIndices", 0)), vs,
                         str(d3d.pixelShader.resourceId),
                         int(bool(ds.stencilEnable)), int(f.compareMask),
                         int(f.writeMask), int(f.reference),
                         int(bool(b0.enabled)) if b0 is not None else 0,
                         int(bool(ds.depthEnable)), int(bool(ds.depthWrites))))
        if i % 400 == 0:
            say("  ... %d/%d" % (i, len(draws)))

    say("")
    say("== every draw using a glass vertex shader ==")
    say("  %-6s %-7s %-7s %-16s %-16s %-6s %-16s %-6s %s"
        % ("pos", "eid", "idx", "vs", "ps", "sten", "rd/wr/ref", "blend",
           "depth(en/wr)"))
    for (pos, eid, idx, vs, ps, sen, rdm, wrm, ref, bl, de, dw) in hits:
        say("  %-6d %-7d %-7d %-16s %-16s %-6d 0x%02X/0x%02X/0x%02X    %-6d %d/%d"
            % (pos, eid, idx, vs, ps, sen, rdm, wrm, ref, bl, de, dw))

    say("")
    say("== the b1 block at each of those draws ==")
    mats = {}
    for (pos, eid, idx, vs, ps, sen, rdm, wrm, ref, bl, de, dw) in hits:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        eps = controller.GetShaderEntryPoints(vsid)
        refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        bi = None
        for k, cb in enumerate(refl.constantBlocks):
            if int(cb.fixedBindNumber) == 1:
                bi = k
                cbk = cb
                break
        if bi is None:
            say("  eid %d: no b1" % eid)
            continue
        data = b""
        rid = None
        boff = 0
        for ud in pipe.GetConstantBlocks(rd.ShaderStage.Vertex):
            acc = getattr(ud, "access", None)
            if acc is None or int(getattr(acc, "index", -1)) != bi:
                continue
            d = getattr(ud, "descriptor", None)
            if d is None:
                continue
            rid = d.resource
            boff = int(getattr(d, "byteOffset", 0))
            bsz = int(getattr(d, "byteSize", 0))
            if rid != rd.ResourceId.Null():
                data = bytes(controller.GetBufferData(rid, boff,
                                                      min(bsz, 65536) or 65536))
            break
        say("  eid %-7d pos %-5d idx %-6d  block %s declared %d B, resource %s "
            "real length %s B, firstConstant %d, read %d B"
            % (eid, pos, idx, cbk.name, int(cbk.byteSize), str(rid),
               bufsize.get(str(rid), "?"), boff // 16, len(data)))
        say("        MOD CAN SNAPSHOT IT: %s  (cbscan.cpp requires the buffer to "
            "be >= 256 bytes)"
            % ("yes" if bufsize.get(str(rid), 0) >= 256 else
               "NO - draw is skipped, nothing is shifted"))
        for v in cbk.variables:
            r_, c_, _ = shape_of(v)
            off = int(v.byteOffset)
            if r_ == 4 and c_ == 4 and off + 64 <= len(data):
                m = struct.unpack_from("<16f", data, off)
                say("        +0x%04X %-26s affine=%-5s  %s"
                    % (off, v.name, is_affine(m),
                       " ".join("%+.4f" % x for x in m[:4])))
                mats.setdefault(eid, {})[off] = data[off:off + 64]

    say("")
    say("== are the rounds the same object? (byte-compare the clip matrix) ==")
    eids = sorted(mats)
    for i in range(len(eids)):
        for j in range(i + 1, len(eids)):
            a, b = mats[eids[i]], mats[eids[j]]
            common = set(a) & set(b)
            same = [o for o in sorted(common) if a[o] == b[o]]
            diff = [o for o in sorted(common) if a[o] != b[o]]
            say("  eid %-7d vs eid %-7d : identical at %s ; different at %s"
                % (eids[i], eids[j],
                   ", ".join("+0x%02X" % o for o in same) or "-",
                   ", ".join("+0x%02X" % o for o in diff) or "-"))

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
