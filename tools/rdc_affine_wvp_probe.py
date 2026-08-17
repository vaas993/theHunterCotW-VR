"""COUNTER-EXAMPLE PROBE: clip-space matrices whose 4th column IS (0,0,0,1).

Run headlessly:
    qrenderdoc.exe --python tools\rdc_affine_wvp_probe.py
    set COTW_CAP=... for the other capture

The proposed runtime rule keys on "a matrix that projects to clip space never
has a 4th column of exactly (0,0,0,1), because w must come from the vertex".
rdc_wvp_rule_sweep reported layouts where it does anyway.  This dumps them:

  * every distinct b1 layout that owns a 4x4 named WorldViewProj* at +0x00
  * for each, a draw where that matrix's 4th column is bit-exactly (0,0,0,1)
  * the raw bits, so "exactly" is not a figure of speech
  * what actually sits at +0x40 in that layout, i.e. what the rule would shift
  * an arithmetic check that the +0x00 matrix really is the object-to-clip one
  * the index counts those draws use, against the viewmodel's mesh counts - the
    mod's OffsetForMesh() decides the layout ONCE per index count and keeps it,
    so one such draw arriving first poisons that mesh for the whole session

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_affine_wvp_probe_<capname>.txt
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
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_affine_wvp_probe_" + TAG + ".txt")

VIEWMODEL_COUNTS = (77484, 26178, 9594, 5007, 162, 6087, 5958, 10272, 3252,
                    2484, 2208, 1584)

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


def bits(f):
    return "%08X" % struct.unpack("<I", struct.pack("<f", f))[0]


def is_affine(m):
    return m[3] == 0.0 and m[7] == 0.0 and m[11] == 0.0 and m[15] == 1.0


def finite(m):
    return all(map(math.isfinite, m))


def b1_of(controller, pipe):
    vsid = pipe.GetShader(rd.ShaderStage.Vertex)
    if vsid == rd.ResourceId.Null():
        return None
    eps = controller.GetShaderEntryPoints(vsid)
    refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
    bi = None
    cbk = None
    for k, cb in enumerate(refl.constantBlocks):
        if int(cb.fixedBindNumber) == 1:
            bi, cbk = k, cb
            break
    if bi is None:
        return None
    data = b""
    rid = None
    for ud in pipe.GetConstantBlocks(rd.ShaderStage.Vertex):
        acc = getattr(ud, "access", None)
        if acc is None or int(getattr(acc, "index", -1)) != bi:
            continue
        d = getattr(ud, "descriptor", None)
        if d is None:
            continue
        rid = d.resource
        o_ = int(getattr(d, "byteOffset", 0))
        s_ = int(getattr(d, "byteSize", 0))
        if rid != rd.ResourceId.Null():
            data = bytes(controller.GetBufferData(rid, o_,
                                                  min(s_, 65536) or 65536))
        break
    return (str(vsid), cbk, data, rid, refl)


def main():
    cap = rd.OpenCaptureFile()
    say("capture: %s" % CAP)
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return

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

    # find every draw whose b1 has a 4x4 at +0x00 that IS affine but whose
    # ground-truth WVP is at +0x00 as well - i.e. the rule's failure case
    victims = {}
    counts = {}
    for i, a in enumerate(draws):
        controller.SetFrameEvent(a.eventId, True)
        pipe = controller.GetPipelineState()
        try:
            got = b1_of(controller, pipe)
        except Exception:
            continue
        if got is None:
            continue
        vs, cbk, data, rid, refl = got
        if len(data) < 0x80:
            continue
        mats = []
        for v in cbk.variables:
            r_, c_, _ = shape_of(v)
            if r_ == 4 and c_ == 4:
                mats.append((int(v.byteOffset), v.name))
        truth = None
        for off, nm in mats:
            if nm.lower().startswith("worldviewproj"):
                truth = off
                break
        if truth != 0:
            continue
        m0 = struct.unpack_from("<16f", data, 0)
        if not finite(m0) or not is_affine(m0):
            continue
        m1 = struct.unpack_from("<16f", data, 0x40)
        if not finite(m1) or is_affine(m1):
            continue
        # the rule would return 0x40 here, and the truth is 0x00
        key = vs
        counts.setdefault(key, set()).add(int(getattr(a, "numIndices", 0)))
        if key not in victims:
            victims[key] = (a.eventId, cbk, data, mats, rid)
        if i % 400 == 0:
            say("  ... %d/%d" % (i, len(draws)))

    say("")
    say("=" * 96)
    say("== layouts where the PROPOSED rule returns 0x40 but the clip matrix is "
        "at +0x00 ==")
    if not victims:
        say("  none in this capture")
    for vs, (eid, cbk, data, mats, rid) in sorted(victims.items()):
        say("")
        say("  vs %s   block %s (%d B declared)   example eid %d   resource %s"
            % (vs, cbk.name, int(cbk.byteSize), eid, str(rid)))
        say("    index counts seen with this shader: %s"
            % sorted(counts.get(vs, ())))
        clash = sorted(set(counts.get(vs, ())) & set(VIEWMODEL_COUNTS))
        say("    counts shared with the viewmodel meshes: %s"
            % (clash if clash else "none"))
        say("    4x4 variables: %s"
            % ", ".join("+0x%04X %s" % (o, n) for o, n in mats))
        for off, nm in mats:
            if off + 64 > len(data):
                continue
            m = struct.unpack_from("<16f", data, off)
            say("    +0x%04X %-26s affine=%-5s  4th column bits %s %s %s %s"
                % (off, nm, is_affine(m), bits(m[3]), bits(m[7]), bits(m[11]),
                   bits(m[15])))
            for r in range(4):
                say("            [%s]" % " ".join("%+12.5f" % m[r * 4 + c]
                                                  for c in range(4)))
        m1 = struct.unpack_from("<16f", data, 0x40)
        say("    WHAT THE RULE WOULD SHIFT, +0x40 as 16 floats:")
        for r in range(4):
            say("            [%s]" % " ".join("%+12.5f" % m1[r * 4 + c]
                                              for c in range(4)))
        named = [(int(v.byteOffset), v.name, shape_of(v)[0], shape_of(v)[1])
                 for v in cbk.variables
                 if 0x40 <= int(v.byteOffset) < 0x80]
        say("    declared variables inside +0x40..+0x80: %s"
            % ", ".join("+0x%04X %s (%dx%d)" % t for t in named))

        # prove +0x00 really is the object-to-clip matrix
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        try:
            proof(controller, pipe, data, mats)
        except Exception as e:
            say("    proof failed: %s" % e)

    say("")
    say("DONE")
    controller.Shutdown()
    cap.Shutdown()


def proof(controller, pipe, data, mats):
    attrs = list(pipe.GetVertexInputs())
    vbs = list(pipe.GetVBuffers())
    pos = None
    for at in attrs:
        if str(getattr(at, "name", "")).upper().startswith("POSITION"):
            pos = at
            break
    if pos is None:
        say("    (no POSITION attribute)")
        return
    slot = int(getattr(pos, "vertexBuffer", 0))
    if slot >= len(vbs) or vbs[slot].resourceId == rd.ResourceId.Null():
        say("    (POSITION vb unbound)")
        return
    vb = vbs[slot]
    stride = int(vb.byteStride)
    base = int(vb.byteOffset) + int(getattr(pos, "byteOffset", 0))
    fmt = pos.format
    ctype = en(fmt.compType)
    cw = int(fmt.compByteWidth)
    raw = bytes(controller.GetBufferData(vb.resourceId, base, stride * 400))

    def dec(k):
        o = k * stride
        if o + cw * 3 > len(raw):
            return None
        if cw == 4 and ctype in ("Float", "SFloat"):
            return struct.unpack_from("<3f", raw, o)
        if cw == 2 and ctype in ("Float", "SFloat"):
            return tuple(float(x) for x in struct.unpack_from("<3e", raw, o))
        if cw == 2 and ctype == "SNorm":
            return tuple(x / 32767.0 for x in struct.unpack_from("<3h", raw, o))
        if cw == 2 and ctype == "UNorm":
            return tuple(x / 65535.0 for x in struct.unpack_from("<3H", raw, o))
        return None

    pv = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
    if pv.vertexResourceId == rd.ResourceId.Null():
        say("    (no post-VS)")
        return
    pstride = int(pv.vertexByteStride)
    n = min(int(getattr(pv, "numIndices", 0)), 400)
    pvraw = bytes(controller.GetBufferData(pv.vertexResourceId,
                                           int(pv.vertexByteOffset),
                                           pstride * (n + 8)))
    pidx = oidx = None
    try:
        if pv.indexResourceId != rd.ResourceId.Null():
            istr = int(pv.indexByteStride)
            ir = bytes(controller.GetBufferData(pv.indexResourceId,
                                                int(pv.indexByteOffset),
                                                istr * n))
            c = len(ir) // istr
            pidx = list(struct.unpack(("<%dI" % c) if istr == 4
                                      else ("<%dH" % c), ir[:c * istr]))
        ib = pipe.GetIBuffer()
        if ib is not None and ib.resourceId != rd.ResourceId.Null():
            istr = int(ib.byteStride)
            ir = bytes(controller.GetBufferData(ib.resourceId,
                                                int(ib.byteOffset), istr * n))
            c = len(ir) // istr
            oidx = list(struct.unpack(("<%dI" % c) if istr == 4
                                      else ("<%dH" % c), ir[:c * istr]))
    except Exception:
        pass

    pairs = []
    if pidx and oidx:
        seen = set()
        for k in range(min(len(pidx), len(oidx))):
            key = (oidx[k], pidx[k])
            if key in seen:
                continue
            seen.add(key)
            p = dec(oidx[k])
            o = pidx[k] * pstride
            if p is None or o + 16 > len(pvraw):
                continue
            c = struct.unpack_from("<4f", pvraw, o)
            if all(map(math.isfinite, c)) and all(map(math.isfinite, p)):
                pairs.append((p, c))
            if len(pairs) >= 200:
                break
    if not pairs:
        say("    (no vertex pairs)")
        return

    def rowa(m, p):
        return tuple(p[0] * m[j] + p[1] * m[4 + j] + p[2] * m[8 + j] + m[12 + j]
                     for j in range(4))

    def cola(m, p):
        return tuple(p[0] * m[j * 4] + p[1] * m[j * 4 + 1]
                     + p[2] * m[j * 4 + 2] + m[j * 4 + 3] for j in range(4))

    say("    arithmetic against %d real vertex pairs:" % len(pairs))
    for off, nm in mats:
        if off + 64 > len(data):
            continue
        m = struct.unpack_from("<16f", data, off)
        for lbl, fn in (("row v.M", rowa), ("col M.v", cola)):
            worst = max(max(abs(c[i] - fn(m, p)[i]) for i in range(4))
                        for p, c in pairs)
            say("       +0x%04X %-26s %-8s worst %.6f%s"
                % (off, nm, lbl, worst, "   <<< MATCH" if worst < 1e-3 else ""))


try:
    main()
except Exception:
    say("EXCEPTION:")
    say(traceback.format_exc())

out.close()
os._exit(0)
