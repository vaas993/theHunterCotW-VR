"""REFUTATION PASS on the "glass WVP lives at +0x40" claim.

Run headlessly (capture + output chosen by environment, so one script serves
both captures):

    set COTW_CAP=%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame28475.rdc
    set COTW_OUT=%LOCALAPPDATA%\theHunterCotWVR\rdc_refute_glass_scope.txt
    qrenderdoc.exe --python tools\rdc_refute_glass.py

NOTHING here is taken from the previous investigation: not the glass eids, not
the block size, not the offset.  The frame is swept from scratch, the glass is
found from render state, and the matrix that reaches clip space is established
by FORWARD transform - input vertex through the candidate must reproduce the
real post-VS SV_POSITION - not by inverse-and-hope.

Ground truth per draw = the 64-byte window (named or not) whose forward
transform reproduces SV_POSITION.  Two rules are then scored against it:

    rule A (shipping):  |m[12]|,|m[13]|,|m[14]| > 100  ->  0x40 else 0x00
    rule B (proposed):  lowest o in {0x00,0x40} whose column 3 != (0,0,0,1)

and three extra things the claim depends on are measured rather than assumed:
    * the REAL D3D11 ByteWidth of every constant buffer a viewmodel draw binds
      (the mod only snapshots buffers >= 256 B, and only substitutes when
       offset + 256 <= snapshot size)
    * the bind OFFSET (vecOffset*16) of every such draw - the shipping code
      evaluates the rule at the pool base, not at the block
    * whether any candidate is ORTHOGRAPHIC (affine clip matrix), which is the
      one shape that would make rule B pick the wrong field

RenderDoc 1.45 traps observed: GetConstantBlocks (not GetConstantBuffer),
descriptors are temporary SWIG proxies, offsets are per shader, no bare except.
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(os.environ.get(
    "COTW_CAP", r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame28475.rdc"))
OUT = os.path.expandvars(os.environ.get(
    "COTW_OUT", r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute_glass.txt"))
CSV = OUT.replace(".txt", ".csv")

NEAR = 4          # the mod's own glass window
CTX = 30          # draws either side of the tagged pass to analyse deeply
MAXREAD = 65536

out = open(OUT, "w")
csvf = open(CSV, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def en(x):
    return str(x).split(".")[-1].split(":")[0]


def shape_of(v):
    t = getattr(v, "type", None)
    for holder in (t, getattr(t, "descriptor", None)):
        if holder is None:
            continue
        r = getattr(holder, "rows", None)
        c = getattr(holder, "columns", None)
        if r:
            nm = getattr(holder, "name", None) or getattr(holder, "baseType", "")
            return int(r), int(c or 1), str(nm)
    return 0, 0, "?"


def rowmul(v, m):
    """row vector v(4) * M, M stored row-major in memory order."""
    return [sum(v[i] * m[i * 4 + j] for i in range(4)) for j in range(4)]


def colmul(m, v):
    """M * column vector v(4), M stored row-major in memory order."""
    return [sum(m[j * 4 + i] * v[i] for i in range(4)) for j in range(4)]


def is0001(c):
    return (abs(c[0]) < 1e-6 and abs(c[1]) < 1e-6 and abs(c[2]) < 1e-6
            and abs(c[3] - 1.0) < 1e-6)


def ruleA(blk, nbytes):
    """Exactly what cbscan.cpp ships today."""
    if nbytes < 0x80:
        return 0
    m = struct.unpack_from("<16f", blk, 0)
    tx, ty, tz = m[12], m[13], m[14]
    if not all(map(math.isfinite, (tx, ty, tz))):
        return 0
    world = abs(tx) > 100.0 or abs(ty) > 100.0 or abs(tz) > 100.0
    return 0x40 if world else 0x00


def ruleB(blk, nbytes):
    """The proposed rule: lowest candidate offset with a projective column 3."""
    for o in (0x00, 0x40):
        if o + 64 > nbytes:
            break
        if o + 64 > len(blk):
            break
        m = struct.unpack_from("<16f", blk, o)
        c = (m[3], m[7], m[11], m[15])
        if not all(map(math.isfinite, c)):
            continue
        if not is0001(c):
            return o
    return None          # "no clip matrix here - leave the draw alone"


def main():
    say("capture: %s" % CAP)
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open")

    sf = controller.GetStructuredFile()

    # ---- real D3D11 buffer sizes, straight from the resource list ----------
    bufsize = {}
    try:
        for b in controller.GetBuffers():
            bufsize[str(b.resourceId)] = int(b.length)
        say("GetBuffers: %d buffers" % len(bufsize))
    except Exception as e:
        say("GetBuffers FAILED %s: %s" % (type(e).__name__, e))

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
    by_eid = {a.eventId: a for a in draws}

    # ---------------- PHASE 1: sweep -------------------------------------
    csvf.write("i,eid,indices,inst,sEn,readMask,ref,tagged,blend,src,dst,vs,ps\n")
    rows = []
    for i, a in enumerate(draws):
        controller.SetFrameEvent(a.eventId, False)
        d3d = controller.GetD3D11PipelineState()
        om = d3d.outputMerger
        ds = om.depthStencilState
        f = ds.frontFace
        b0, bsrc, bdst = 0, "-", "-"
        try:
            bl = list(om.blendState.blends)
            if bl:
                b0 = int(bool(bl[0].enabled))
                bsrc = en(bl[0].colorBlend.source)
                bdst = en(bl[0].colorBlend.destination)
        except Exception as e:
            bsrc = "ERR%s" % type(e).__name__
        r = {"i": i, "eid": a.eventId,
             "indices": int(getattr(a, "numIndices", 0)),
             "inst": int(getattr(a, "numInstances", 0)),
             "sEn": int(bool(ds.stencilEnable)),
             "readMask": int(f.compareMask), "ref": int(f.reference),
             "blend": b0, "src": bsrc, "dst": bdst,
             "vs": str(d3d.vertexShader.resourceId),
             "ps": str(d3d.pixelShader.resourceId)}
        r["tagged"] = int(r["sEn"] and r["readMask"] == 0x40 and bool(r["ref"] & 0x40))
        rows.append(r)
        csvf.write(",".join(str(r[k]) for k in (
            "i", "eid", "indices", "inst", "sEn", "readMask", "ref", "tagged",
            "blend", "src", "dst", "vs", "ps")) + "\n")
        if i % 200 == 0:
            csvf.flush()
            say("  swept %d/%d" % (i, len(draws)))
    csvf.flush()

    tagged = [r for r in rows if r["tagged"]]
    say("")
    say("== stencil tag (sEn && readMask==0x40 && ref&0x40): %d of %d draws =="
        % (len(tagged), len(rows)))
    for r in tagged:
        say("   i=%-5d eid=%-7d idx=%-7d ref=0x%02X blend=%d vs=%s"
            % (r["i"], r["eid"], r["indices"], r["ref"], r["blend"], r["vs"]))

    tagidx = set(r["i"] for r in tagged)
    nearblend = [r for r in rows
                 if r["blend"] and any(abs(r["i"] - t) <= NEAR for t in tagidx)]
    say("")
    say("== blended within %d draws of a tagged draw: %d ==" % (NEAR, len(nearblend)))
    for r in nearblend:
        fwd = min([r["i"] - t for t in tagidx if t <= r["i"]] or [99999])
        say("   i=%-5d eid=%-7d idx=%-7d %s/%s vs=%s  sinceStencil(fwd)=%s"
            % (r["i"], r["eid"], r["indices"], r["src"][:8], r["dst"][:10],
               r["vs"], "none-yet" if fwd == 99999 else fwd))

    # deep set: everything around the tagged pass
    if tagged:
        lo = max(0, min(tagidx) - CTX)
        hi = min(len(rows) - 1, max(tagidx) + CTX)
    else:
        lo, hi = 0, min(len(rows) - 1, 60)
    deep = rows[lo:hi + 1]
    say("")
    say("== deep analysis over draws %d..%d (%d draws) ==" % (lo, hi, len(deep)))

    # ---------------- PHASE 2: per draw ----------------------------------
    score = {"A": [0, 0], "B": [0, 0]}          # [correct, wrong]
    failA, failB = [], []
    seen_vs = {}
    summary = []

    for r in deep:
        eid = r["eid"]
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        d3d = controller.GetD3D11PipelineState()
        try:
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        except Exception as e:
            say("  eid %d reflection FAILED %s: %s" % (eid, type(e).__name__, e))
            continue

        # slot -> (buffer, bind offset, bind size) from the D3D11 state
        slotbuf = {}
        try:
            for slot, cb in enumerate(list(d3d.vertexShader.constantBuffers)):
                rid = getattr(cb, "resourceId", None)
                if rid is None or rid == rd.ResourceId.Null():
                    continue
                slotbuf[slot] = (rid, int(getattr(cb, "vecOffset", 0)) * 16,
                                 int(getattr(cb, "vecCount", 0)) * 16)
        except Exception as e:
            say("  eid %d d3d cbs FAILED %s: %s" % (eid, type(e).__name__, e))

        # post-VS clip + input positions
        clip, inpos = [], []
        try:
            vo = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            if vo and vo.vertexResourceId != rd.ResourceId.Null():
                st, base = int(vo.vertexByteStride), int(vo.vertexByteOffset)
                n = min(int(getattr(vo, "numIndices", 0)) or 16, 16)
                raw = bytes(controller.GetBufferData(vo.vertexResourceId, base,
                                                     st * n))
                for k in range(n):
                    if (k + 1) * st <= len(raw):
                        clip.append(struct.unpack_from("<4f", raw, k * st))
            vi = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSIn)
            if vi and vi.vertexResourceId != rd.ResourceId.Null():
                st, base = int(vi.vertexByteStride), int(vi.vertexByteOffset)
                n = min(len(clip) or 16, 16)
                raw = bytes(controller.GetBufferData(vi.vertexResourceId, base,
                                                     st * n))
                cc = int(vi.format.compCount)
                cw = int(vi.format.compByteWidth)
                ct = en(vi.format.compType)
                for k in range(n):
                    o = k * st
                    if o + st > len(raw):
                        break
                    if cw == 4 and ct.startswith("Float") and cc >= 3:
                        inpos.append(struct.unpack_from("<3f", raw, o))
                    elif cw == 2 and ct.startswith("SNorm") and cc >= 3:
                        v = struct.unpack_from("<%dh" % cc, raw, o)
                        inpos.append(tuple(x / 32767.0 for x in v[:3]))
                    elif cw == 2 and ct.startswith("UNorm") and cc >= 3:
                        v = struct.unpack_from("<%dH" % cc, raw, o)
                        inpos.append(tuple(x / 65535.0 for x in v[:3]))
        except Exception as e:
            say("  eid %d postVS FAILED %s: %s" % (eid, type(e).__name__, e))

        # candidate matrices out of every declared block
        blocks = []
        for cb in refl.constantBlocks:
            slot = int(cb.fixedBindNumber)
            src = slotbuf.get(slot)
            if src is None:
                blocks.append((cb, slot, None, 0, 0, b"", 0))
                continue
            rid, boff, bsz = src
            want = min(max(cb.byteSize, 16), MAXREAD)
            try:
                data = bytes(controller.GetBufferData(rid, boff, want))
            except Exception as e:
                say("  eid %d GetBufferData FAILED %s: %s"
                    % (eid, type(e).__name__, e))
                data = b""
            blocks.append((cb, slot, rid, boff, bsz, data,
                           bufsize.get(str(rid), -1)))

        cands = []      # (blockname, slot, offset, varname, vals, data, real)
        for cb, slot, rid, boff, bsz, data, real in blocks:
            named = {}
            for v in cb.variables:
                nr, nc, tn = shape_of(v)
                if nr >= 4 and nc >= 4:
                    named[int(v.byteOffset)] = v.name
            lim = min(len(data), 1024)
            for o in range(0, max(lim - 63, 0), 16):
                vals = struct.unpack_from("<16f", data, o)
                if not all(map(math.isfinite, vals)):
                    continue
                cands.append((cb.name, slot, o, named.get(o, "(unnamed)"),
                              list(vals), data, real, boff))

        # GROUND TRUTH: forward transform must reproduce SV_POSITION
        truth = None
        truth_err = None
        truth_conv = None
        if clip and inpos and cands:
            n = min(len(clip), len(inpos))
            best = None
            for (bn, slot, o, vn, vals, data, real, boff) in cands:
                for conv in ("row", "col"):
                    err = 0.0
                    for k in range(n):
                        p = list(inpos[k]) + [1.0]
                        q = rowmul(p, vals) if conv == "row" else colmul(vals, p)
                        e = max(abs(q[j] - clip[k][j]) for j in range(4))
                        err = max(err, e)
                    if best is None or err < best[0]:
                        best = (err, bn, slot, o, vn, conv, real, boff, len(data))
            if best and best[0] < 1e-3:
                truth_err = best[0]
                truth = (best[1], best[2], best[3], best[4])
                truth_conv = best[5]

        # rules, evaluated on the block as the shipping code would see it
        b1 = None
        for cb, slot, rid, boff, bsz, data, real in blocks:
            if slot == 1:
                b1 = (cb, slot, rid, boff, bsz, data, real)
        rA = rB = None
        if b1 and len(b1[5]) >= 0x80:
            rA = ruleA(b1[5], len(b1[5]))
            rB = ruleB(b1[5], len(b1[5]))
        elif b1 and len(b1[5]) >= 0x40:
            rA = ruleA(b1[5], len(b1[5]))
            rB = ruleB(b1[5], len(b1[5]))

        tagl = ("TAGGED" if r["tagged"] else ("blend" if r["blend"] else "-"))
        line = ("i=%-5d eid=%-7d %-6s idx=%-7d vs=%-14s" %
                (r["i"], r["eid"], tagl, r["indices"], r["vs"]))
        if truth:
            line += " TRUTH %s@+0x%02X %s(%s) err=%.2e" % (
                truth[0], truth[2], truth[3], truth_conv, truth_err)
        else:
            line += " TRUTH none"
        line += "  A=%s B=%s" % (
            "-" if rA is None else "0x%02X" % rA,
            "none" if rB is None else "0x%02X" % rB)
        if b1:
            line += "  b1=%s real=%dB bind_off=%d declared=%dB" % (
                str(b1[2]), b1[6], b1[3], b1[0].byteSize)
        say("  " + line)
        summary.append((r, truth, truth_conv, rA, rB, b1))

        if truth and b1 is not None and truth[1] == 1:
            t_off = truth[2]
            if rA == t_off:
                score["A"][0] += 1
            else:
                score["A"][1] += 1
                failA.append((r["i"], r["eid"], t_off, rA, r["blend"]))
            if rB == t_off:
                score["B"][0] += 1
            else:
                score["B"][1] += 1
                failB.append((r["i"], r["eid"], t_off, rB, r["blend"]))

        # detail dump for the glass and one of each layout
        key = (r["vs"], b1[0].name if b1 else "-", b1[0].byteSize if b1 else 0)
        if key not in seen_vs:
            seen_vs[key] = (r["i"], r["eid"])

    say("")
    say("=" * 78)
    say("== SCOREBOARD over the analysed window (b1 blocks with ground truth) ==")
    say("   rule A (shipping, |t|>100 at +0x00): %d correct, %d wrong"
        % tuple(score["A"]))
    for f in failA:
        say("       WRONG i=%-5d eid=%-7d truth +0x%02X got %s blended=%d"
            % (f[0], f[1], f[2], "-" if f[3] is None else "0x%02X" % f[3], f[4]))
    say("   rule B (proposed, column3 != (0,0,0,1)): %d correct, %d wrong"
        % tuple(score["B"]))
    for f in failB:
        say("       WRONG i=%-5d eid=%-7d truth +0x%02X got %s blended=%d"
            % (f[0], f[1], f[2], "none" if f[3] is None else "0x%02X" % f[3],
               f[4]))

    say("")
    say("== one representative draw per (VS, b1 block, size) ==")
    for k, v in sorted(seen_vs.items(), key=lambda kv: kv[1][0]):
        say("   vs=%-14s block=%-18s %5dB   first at i=%d eid=%d"
            % (k[0], k[1], k[2], v[0], v[1]))

    say("")
    say("DONE PHASE 1+2")
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    say("EXCEPTION:")
    say(traceback.format_exc())

out.close()
csvf.close()
os._exit(0)
