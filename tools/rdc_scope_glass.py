"""SCOPE GLASS: where does the alpha-blended lens shader keep its clip matrix?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_scope_glass.py

The mod shifts clip-space X on the viewmodel's projection matrix per eye and it
works for the SOLID weapon (stencil tag: StencilEnable, compareMask == 0x40
exactly, reference & 0x40).  The scope/binocular GLASS is a separate
alpha-blended pass with NO stencil tag; shifting the same +0x00 / +0x40 offsets
there makes the lens fly up-and-right, which a clip-space X shift cannot do -
so the field being written is not the clip matrix.

This script:
  1. sweeps the frame recording depth-stencil + blend + shader ids
  2. finds the first-person pass by the stencil predicate
  3. finds alpha-blended draws next to it (the glass)
  4. dumps EVERY constant block the glass VS reads, BY SLOT (b#), every variable
     BY NAME at its reflected offset, with values
  5. classifies each 4x4: a row-vector clip matrix has 4th COLUMN != (0,0,0,1);
     an affine World/View/Texture matrix has 4th column == (0,0,0,1)
  6. proves it by transforming a real vertex: VSIn position through the
     candidate matrix must reproduce the VSOut SV_POSITION

Traps encoded here (1.45):
  * PipeState.GetConstantBuffer does not exist -> GetConstantBlocks
  * UsedDescriptor.access.index is descriptor POSITION not HLSL register, so
    slot mapping comes from the D3D11 state's constantBuffers[] array instead
  * descriptors are temporary SWIG proxies -> fields copied immediately
  * no bare except: every failure is printed

Outputs:
    %LOCALAPPDATA%\theHunterCotWVR\rdc_scope_glass.txt
    %LOCALAPPDATA%\theHunterCotWVR\rdc_scope_glass.csv
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame28475.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_scope_glass.txt")
CSV = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_scope_glass.csv")

MAXBLOCK = 65536          # not 16384 - a glass block may be bigger
NEAR = 4                  # the mod's own window: blended within 4 draws of tagged

out = open(OUT, "w")
csv = open(CSV, "w")


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


def classify(vals):
    """vals = 16 floats as stored (row-major memory order).

    HLSL default on D3D is column_major storage, but this engine's dumps show
    the projection's -1 in element [11] and the translation in [12..14], i.e.
    row-vector convention v*M laid out row-major.  Test BOTH so nothing is
    assumed."""
    col3 = (vals[3], vals[7], vals[11], vals[15])
    row3 = (vals[12], vals[13], vals[14], vals[15])

    def is0001(t):
        return (abs(t[0]) < 1e-5 and abs(t[1]) < 1e-5 and abs(t[2]) < 1e-5
                and abs(t[3] - 1.0) < 1e-5)

    tags = []
    if is0001(col3):
        tags.append("AFFINE(row-vec): 4th col == (0,0,0,1)")
    else:
        tags.append("PROJECTIVE(row-vec): 4th col = (%+.4f %+.4f %+.4f %+.4f)"
                    % col3)
    if is0001(row3):
        tags.append("AFFINE(col-vec): 4th row == (0,0,0,1)")
    return " | ".join(tags)


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open: %s" % CAP)

    sf = controller.GetStructuredFile()

    resname = {}
    try:
        for r in controller.GetResources():
            resname[str(r.resourceId)] = r.name
    except Exception as e:
        say("GetResources failed: %s" % e)

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
    say("%d drawcalls in the frame" % len(draws))
    by_eid = {a.eventId: a for a in draws}

    csv.write("i,eid,name,numIndices,numInstances,stencilEnable,readMask,"
              "writeMask,ref,tagged,blend0,blendSrc,blendDst,vs,ps,rt0\n")

    rows = []
    for i, a in enumerate(draws):
        eid = a.eventId
        controller.SetFrameEvent(eid, False)
        d3d = controller.GetD3D11PipelineState()
        om = d3d.outputMerger
        ds = om.depthStencilState
        f = ds.frontFace
        bs = om.blendState

        b0en, bsrc, bdst = 0, "-", "-"
        try:
            bl = list(bs.blends)
            if bl:
                b = bl[0]
                b0en = int(bool(b.enabled))
                bsrc = en(b.colorBlend.source)
                bdst = en(b.colorBlend.destination)
        except Exception as e:
            bsrc = "ERR:%s" % e

        r = {
            "i": i,
            "eid": eid,
            "name": a.GetName(sf).split("(")[0][:34],
            "numIndices": int(getattr(a, "numIndices", 0)),
            "numInstances": int(getattr(a, "numInstances", 0)),
            "stencilEnable": int(bool(ds.stencilEnable)),
            "readMask": int(f.compareMask),
            "writeMask": int(f.writeMask),
            "ref": int(f.reference),
            "blend0": b0en,
            "blendSrc": bsrc,
            "blendDst": bdst,
            "vs": str(d3d.vertexShader.resourceId),
            "ps": str(d3d.pixelShader.resourceId),
        }
        r["tagged"] = int(r["stencilEnable"] and r["readMask"] == 0x40
                          and bool(r["ref"] & 0x40))
        try:
            rt = list(om.renderTargets)
            r["rt0"] = str(rt[0].resource) if rt else "-"
        except Exception:
            r["rt0"] = "?"
        rows.append(r)
        csv.write(",".join(str(r[k]) for k in (
            "i", "eid", "name", "numIndices", "numInstances", "stencilEnable",
            "readMask", "writeMask", "ref", "tagged", "blend0", "blendSrc",
            "blendDst", "vs", "ps", "rt0")) + "\n")
        if i % 200 == 0:
            csv.flush()
            say("  ... swept %d/%d (eid %d)" % (i, len(draws), eid))
    csv.flush()

    # ---------------- 1. the first-person pass ---------------------------
    tagged = [r for r in rows if r["tagged"]]
    say("")
    say("=" * 78)
    say("== STEP 1: first-person stencil tag (sEnable, readMask==0x40, ref&0x40) ==")
    say("  %d tagged draws of %d" % (len(tagged), len(rows)))
    if not tagged:
        say("  NO TAGGED DRAWS - the predicate does not fire in this capture")
    else:
        say("  draw-index span %d..%d   eid span %d..%d"
            % (tagged[0]["i"], tagged[-1]["i"],
               tagged[0]["eid"], tagged[-1]["eid"]))
        say("  %6s %7s %8s %6s  %-10s %-6s %s"
            % ("idx", "eid", "indices", "inst", "rd/wr/ref", "blend", "vs"))
        for r in tagged:
            say("  %6d %7d %8d %6d  0x%02X/0x%02X/0x%02X %-6d %s"
                % (r["i"], r["eid"], r["numIndices"], r["numInstances"],
                   r["readMask"], r["writeMask"], r["ref"], r["blend0"],
                   r["vs"][:26]))

    # ---------------- 2. alpha-blended draws next to it ------------------
    tagidx = set(r["i"] for r in tagged)
    glass = []
    for r in rows:
        if not r["blend0"]:
            continue
        if any(abs(r["i"] - t) <= NEAR for t in tagidx):
            glass.append(r)
    say("")
    say("=" * 78)
    say("== STEP 2: alpha-blended draws within %d draws of a tagged draw ==" % NEAR)
    say("  %d candidates" % len(glass))
    say("  %6s %7s %8s %6s  %-14s %-10s %-6s %s"
        % ("idx", "eid", "indices", "inst", "blend src/dst", "rd/wr/ref",
           "sEn", "vs"))
    for r in glass:
        say("  %6d %7d %8d %6d  %-14s 0x%02X/0x%02X/0x%02X %-6d %s"
            % (r["i"], r["eid"], r["numIndices"], r["numInstances"],
               "%s/%s" % (r["blendSrc"][:6], r["blendDst"][:6]),
               r["readMask"], r["writeMask"], r["ref"], r["stencilEnable"],
               r["vs"][:26]))

    # context: everything in and around the pass
    if tagged:
        lo = max(0, tagged[0]["i"] - 25)
        hi = min(len(rows) - 1, tagged[-1]["i"] + 25)
        say("")
        say("== context: draws %d..%d (T=tagged, B=blend0) ==" % (lo, hi))
        say("  %6s %7s %2s %2s %8s %6s %-12s %-10s %s"
            % ("idx", "eid", "T", "B", "indices", "inst", "blend", "rd/wr/ref",
               "name"))
        for r in rows[lo:hi + 1]:
            say("  %6d %7d %2s %2s %8d %6d %-12s 0x%02X/0x%02X/0x%02X %s"
                % (r["i"], r["eid"], "T" if r["tagged"] else ".",
                   "B" if r["blend0"] else ".", r["numIndices"],
                   r["numInstances"],
                   "%s/%s" % (r["blendSrc"][:5], r["blendDst"][:5]),
                   r["readMask"], r["writeMask"], r["ref"], r["name"]))

    # ---------------- 3+4. constants of the glass draws ------------------
    probe_done = [False]

    def dump(eid, tag):
        a = by_eid.get(eid)
        say("")
        say("=" * 78)
        say("  eid %-7d [%s]  %d indices, %d instances"
            % (eid, tag, getattr(a, "numIndices", 0),
               getattr(a, "numInstances", 0)))
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        d3d = controller.GetD3D11PipelineState()

        try:
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        except Exception as e:
            say("  reflection FAILED %s: %s" % (type(e).__name__, e))
            say(traceback.format_exc())
            return
        say("  VS %s  entry %s" % (str(vsid), eps[0].name if eps else "?"))

        # --- slot -> buffer, straight off the D3D11 state (register index) ---
        slotbuf = {}
        try:
            cbs = list(d3d.vertexShader.constantBuffers)
            if not probe_done[0]:
                probe_done[0] = True
                say("  [probe] d3d.vertexShader.constantBuffers -> %d entries, "
                    "attrs %s" % (len(cbs),
                                  [x for x in dir(cbs[0])
                                   if not x.startswith("_")] if cbs else []))
            for slot, cb in enumerate(cbs):
                rid = getattr(cb, "resourceId", None)
                if rid is None or rid == rd.ResourceId.Null():
                    continue
                vo = int(getattr(cb, "vecOffset", 0))
                vc = int(getattr(cb, "vecCount", 0))
                slotbuf[slot] = (rid, vo * 16, vc * 16)
        except Exception as e:
            say("  d3d constantBuffers FAILED %s: %s" % (type(e).__name__, e))

        # --- cross-check with the descriptor API (the 1.45 way) -------------
        desc_list = []
        try:
            blocks = pipe.GetConstantBlocks(rd.ShaderStage.Vertex)
            for ud in blocks:
                d = getattr(ud, "descriptor", None)
                acc = getattr(ud, "access", None)
                if d is None:
                    continue
                # copy immediately - temporary SWIG proxy
                desc_list.append((int(getattr(acc, "index", -1)),
                                  d.resource, int(d.byteOffset),
                                  int(d.byteSize)))
        except Exception as e:
            say("  GetConstantBlocks FAILED %s: %s" % (type(e).__name__, e))
        say("  GetConstantBlocks: %s"
            % ", ".join("[pos %d] %s off=%d size=%d" % (p, str(r)[:22], o, s)
                        for p, r, o, s in desc_list))
        say("  D3D11 slots     : %s"
            % ", ".join("b%d=%s off=%d size=%d" % (s, str(v[0])[:22], v[1], v[2])
                        for s, v in sorted(slotbuf.items())))

        found = []
        for cb in refl.constantBlocks:
            slot = int(cb.fixedBindNumber)
            say("")
            say("  -- %s  (b%d, %d bytes declared, %d variables) --"
                % (cb.name, slot, cb.byteSize, len(cb.variables)))
            src = slotbuf.get(slot)
            data = b""
            if src is None:
                # fall back on the descriptor list, matched by declared size
                m = [d for d in desc_list if d[3] == cb.byteSize]
                if m:
                    src = (m[0][1], m[0][2], m[0][3])
                    say("     (slot not in D3D11 array; matched a descriptor "
                        "by size)")
            if src is None:
                say("     NO BUFFER BOUND at b%d" % slot)
                continue
            rid, boff, bsz = src
            want = min(max(bsz, cb.byteSize), MAXBLOCK)
            try:
                data = bytes(controller.GetBufferData(rid, boff, want))
            except Exception as e:
                say("     GetBufferData FAILED %s: %s" % (type(e).__name__, e))
                continue
            say("     buffer %s  off=%d  read %d bytes"
                % (resname.get(str(rid), str(rid))[:40], boff, len(data)))
            if cb.byteSize > MAXBLOCK:
                say("     (declared bigger than %d - printing first %d only)"
                    % (MAXBLOCK, MAXBLOCK))

            for v in cb.variables:
                off = int(v.byteOffset)
                nr, nc, tn = shape_of(v)
                n = max(nr * nc, 1)
                if off + n * 4 > len(data):
                    say("     +0x%04X %-30s %dx%d  (past end of read)"
                        % (off, v.name, nr, nc))
                    continue
                vals = struct.unpack_from("<%df" % n, data, off)
                if not all(map(math.isfinite, vals)):
                    say("     +0x%04X %-30s %dx%d  non-finite"
                        % (off, v.name, nr, nc))
                    continue
                if nr >= 3 and nc >= 4:
                    say("     +0x%04X %-30s %dx%d" % (off, v.name, nr, nc))
                    for rr in range(nr):
                        say("            [%s]" % " ".join(
                            "%+12.5f" % vals[rr * nc + c] for c in range(nc)))
                    if n >= 16:
                        say("            %s" % classify(vals))
                        found.append((cb.name, slot, off, v.name, vals))
                    else:
                        say("            (3x4 - affine only, cannot be clip)")
                else:
                    say("     +0x%04X %-30s %dx%d = %s"
                        % (off, v.name, nr, nc,
                           " ".join("%+.5f" % x for x in vals[:8])))

        # --- the decisive test: does the candidate reproduce SV_POSITION? ---
        try:
            vsout = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSOut)
            if vsout and vsout.vertexResourceId != rd.ResourceId.Null():
                nv = int(getattr(vsout, "numIndices", 0))
                stride = int(vsout.vertexByteStride)
                base = int(vsout.vertexByteOffset)
                say("")
                say("  VSOut: %d verts, stride %d, fmt %s"
                    % (nv, stride, en(vsout.format.compType)))
                raw = bytes(controller.GetBufferData(
                    vsout.vertexResourceId, base, stride * min(nv, 6)))
                outpos = []
                for k in range(min(nv, 6)):
                    if (k + 1) * stride <= len(raw):
                        outpos.append(struct.unpack_from("<4f", raw,
                                                         k * stride))
                for k, p in enumerate(outpos):
                    say("    SV_POSITION[%d] = (%+.4f %+.4f %+.4f %+.4f)"
                        % (k, p[0], p[1], p[2], p[3]))

                vsin = controller.GetPostVSData(0, 0, rd.MeshDataStage.VSIn)
                inpos = []
                if vsin and vsin.vertexResourceId != rd.ResourceId.Null():
                    istride = int(vsin.vertexByteStride)
                    ibase = int(vsin.vertexByteOffset)
                    say("  VSIn : stride %d fmt %s comps %d"
                        % (istride, en(vsin.format.compType),
                           int(vsin.format.compCount)))
                    iraw = bytes(controller.GetBufferData(
                        vsin.vertexResourceId, ibase,
                        istride * min(nv, 6)))
                    for k in range(min(nv, 6)):
                        if (k + 1) * istride <= len(iraw):
                            if int(vsin.format.compCount) >= 3 and \
                               int(vsin.format.compByteWidth) == 4:
                                inpos.append(struct.unpack_from(
                                    "<3f", iraw, k * istride))
                    for k, p in enumerate(inpos):
                        say("    VSIn POSITION[%d] = (%+.4f %+.4f %+.4f)"
                            % (k, p[0], p[1], p[2]))

                if inpos and outpos and found:
                    say("")
                    say("  == transform test: p_in * M vs SV_POSITION ==")
                    for bname, slot, off, vname, vals in found:
                        errs = []
                        for p, q in zip(inpos, outpos):
                            v4 = (p[0], p[1], p[2], 1.0)
                            # row-vector: r_j = sum_i v_i * M[i][j]
                            r = [sum(v4[i] * vals[i * 4 + j] for i in range(4))
                                 for j in range(4)]
                            e = max(abs(r[j] - q[j]) for j in range(4))
                            errs.append(e)
                            # column-vector alternative
                        best = min(errs) if errs else 9e9
                        worst = max(errs) if errs else 9e9
                        mark = "   <<<< THIS IS THE CLIP MATRIX" \
                            if worst < 0.05 else ""
                        say("    %-22s b%d +0x%04X  maxerr %.5f%s"
                            % (vname, slot, off, worst, mark))
            else:
                say("  VSOut: no post-VS data")
        except Exception as e:
            say("  post-VS test FAILED %s: %s" % (type(e).__name__, e))

        # --- what textures the pixel shader reads (identifies the lens) ----
        try:
            ros = pipe.GetReadOnlyResources(rd.ShaderStage.Pixel)
            names = []
            for ud in ros[:12]:
                d = getattr(ud, "descriptor", None)
                if d is None:
                    continue
                rid = d.resource
                if rid == rd.ResourceId.Null():
                    continue
                names.append(resname.get(str(rid), str(rid))[:44])
            say("")
            say("  PS textures: %s" % ("; ".join(names) if names else "(none)"))
        except Exception as e:
            say("  PS textures FAILED %s: %s" % (type(e).__name__, e))

    say("")
    say("=" * 78)
    say("== STEP 3/4: constants of every glass candidate ==")
    for r in glass:
        dump(r["eid"], "GLASS blend=%s/%s" % (r["blendSrc"], r["blendDst"]))

    # a couple of neighbouring SOLID tagged draws for comparison
    say("")
    say("=" * 78)
    say("== reference: SOLID tagged viewmodel draws (layout already known) ==")
    if tagged:
        picks = [tagged[0]["eid"], tagged[len(tagged) // 2]["eid"],
                 tagged[-1]["eid"]]
        for eid in sorted(set(picks)):
            dump(eid, "SOLID tagged")

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
csv.close()
os._exit(0)
