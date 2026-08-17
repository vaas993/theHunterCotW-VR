"""Does the PROPOSED runtime rule ever pick the wrong offset - anywhere?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_wvp_rule_sweep.py
    set COTW_CAP=...\cotw_frame28475.rdc  for the other capture

The rule under test, as claimed:
    IsAffine(m)  = m[3]==0 && m[7]==0 && m[11]==0 && m[15]==1   (4th COLUMN)
    WvpOffsetIn(block, bytes):
        bytes < 0x80            -> 0
        !finite(+0x00)          -> 0
        !IsAffine(+0x00)        -> 0        // +0x00 is itself the clip matrix
        !finite(+0x40)          -> 0
        IsAffine(+0x40)         -> 0        // refuse rather than guess
        else                    -> 0x40

Ground truth per draw comes from the shader's OWN reflection: the offset of the
4x4 in the b1 block whose name begins "WorldViewProj".  That is checked against
what the DXBC actually feeds into o0 (SV_POSITION) for every distinct layout, so
the name is not trusted on its own either.

This runs over EVERY drawcall in the frame, not just the viewmodel pass, because
the question is whether the rule could misfire - and the mod's glass catcher
takes untagged blended draws, which in another scene could be world geometry.

Also reports, per distinct b1 layout: the real D3D buffer size behind the
binding (the mod refuses to snapshot buffers smaller than 256 bytes, so a
128-byte block is invisible to it), and the firstConstant of the binding.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_wvp_rule_sweep_<capname>.txt
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
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_wvp_rule_sweep_" + TAG + ".txt")

MAX_BLOCK = 65536

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
            nm = getattr(holder, "name", None) or getattr(holder, "baseType", "")
            return int(r), int(c or 1), str(nm)
    return 0, 0, "?"


def is_affine(m):
    return m[3] == 0.0 and m[7] == 0.0 and m[11] == 0.0 and m[15] == 1.0


def finite(m):
    return all(map(math.isfinite, m))


def rule_proposed(data):
    if len(data) < 0x80:
        return 0
    m0 = struct.unpack_from("<16f", data, 0)
    if not finite(m0):
        return 0
    if not is_affine(m0):
        return 0
    m1 = struct.unpack_from("<16f", data, 0x40)
    if not finite(m1):
        return 0
    if is_affine(m1):
        return 0
    return 0x40


def rule_current(data):
    if len(data) < 0x80:
        return 0
    m = struct.unpack_from("<16f", data, 0)
    if not (math.isfinite(m[12]) and math.isfinite(m[13])
            and math.isfinite(m[14])):
        return 0
    return 0x40 if (abs(m[12]) > 100.0 or abs(m[13]) > 100.0
                    or abs(m[14]) > 100.0) else 0


def main():
    cap = rd.OpenCaptureFile()
    say("capture: %s" % CAP)
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open")

    # real buffer sizes, by resource id
    bufsize = {}
    try:
        for b in controller.GetBuffers():
            bufsize[str(b.resourceId)] = int(b.length)
    except Exception as e:
        say("GetBuffers failed: %s" % e)

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

    # layout cache per vertex shader: (blockName, declBytes, [(off,name)], truth)
    layout = {}
    disasm_note = {}
    stats = {}
    mismatch_prop = []
    mismatch_cur = []
    no_b1 = 0
    seen_layouts = {}

    for i, a in enumerate(draws):
        eid = a.eventId
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        d3d = controller.GetD3D11PipelineState()
        om = d3d.outputMerger
        ds = om.depthStencilState
        f = ds.frontFace
        bs = om.blendState
        b0 = bs.blends[0] if len(bs.blends) else None
        tagged = bool(ds.stencilEnable) and int(f.compareMask) == 0x40 and \
            (int(f.reference) & 0x40) != 0
        blended = bool(b0.enabled) if b0 is not None else False

        try:
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        except Exception:
            continue
        if vsid == rd.ResourceId.Null():
            continue
        key = str(vsid)

        if key not in layout:
            try:
                eps = controller.GetShaderEntryPoints(vsid)
                refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
            except Exception as e:
                layout[key] = None
                continue
            cbk = None
            for cb in refl.constantBlocks:
                if int(cb.fixedBindNumber) == 1:
                    cbk = cb
                    break
            if cbk is None:
                layout[key] = None
            else:
                mats = []
                for v in cbk.variables:
                    r_, c_, _t = shape_of(v)
                    if r_ == 4 and c_ == 4:
                        mats.append((int(v.byteOffset), v.name))
                truth = None
                tname = None
                for off, nm in mats:
                    if nm.lower().startswith("worldviewproj"):
                        truth = off
                        tname = nm
                        break
                # what the DXBC actually feeds into o0
                o0 = ""
                try:
                    dis = controller.DisassembleShader(rd.ResourceId.Null(),
                                                       refl, "DXBC")
                    hits = []
                    for ln in dis.splitlines():
                        s = ln.strip()
                        if " o0" in s or s.startswith("dcl_output_siv o0"):
                            hits.append(s)
                    o0 = " | ".join(hits[:6])
                except Exception:
                    pass
                layout[key] = (cbk.name, int(cbk.byteSize), mats, truth, tname,
                               o0)

        lay = layout.get(key)
        if lay is None:
            no_b1 += 1
            continue
        bname, bsize, mats, truth, tname, o0 = lay

        # the bound descriptor + data
        data = b""
        boff = 0
        rid = None
        try:
            for ud in pipe.GetConstantBlocks(rd.ShaderStage.Vertex):
                acc = getattr(ud, "access", None)
                aidx = int(getattr(acc, "index", -1)) if acc is not None else -1
                d = getattr(ud, "descriptor", None)
                if d is None:
                    continue
                r_ = d.resource
                o_ = int(getattr(d, "byteOffset", 0))
                s_ = int(getattr(d, "byteSize", 0))
                # find the descriptor position of the b1 block
                try:
                    refl2 = None
                except Exception:
                    pass
                if aidx < 0:
                    continue
                # position of b1 in refl.constantBlocks: recompute cheaply
                if aidx == lay_index(layout, key, controller, vsid):
                    rid = r_
                    boff = o_
                    if r_ != rd.ResourceId.Null():
                        data = bytes(controller.GetBufferData(
                            r_, o_, min(s_, MAX_BLOCK) if s_ else MAX_BLOCK))
                    break
        except Exception as e:
            say("eid %d block read failed: %s" % (eid, e))
            continue

        sig = (bname, bsize, tuple(mats), key)
        if sig not in seen_layouts:
            seen_layouts[sig] = {
                "vs": key, "block": bname, "size": bsize, "mats": mats,
                "truth": truth, "tname": tname, "o0": o0,
                "n": 0, "tagged": 0, "blended": 0,
                "bufbytes": bufsize.get(str(rid), -1),
                "firstConstant": boff // 16,
                "propWrong": 0, "curWrong": 0, "example": eid,
            }
        L = seen_layouts[sig]
        L["n"] += 1
        if tagged:
            L["tagged"] += 1
        if blended:
            L["blended"] += 1

        p = rule_proposed(data)
        c = rule_current(data)
        if truth is not None:
            if p != truth:
                L["propWrong"] += 1
                if len(mismatch_prop) < 40:
                    mismatch_prop.append((eid, key, bname, bsize, truth, p,
                                          tagged, blended))
            if c != truth:
                L["curWrong"] += 1
                if len(mismatch_cur) < 40:
                    mismatch_cur.append((eid, key, bname, bsize, truth, c,
                                         tagged, blended))
        if i % 200 == 0:
            say("  ... %d/%d" % (i, len(draws)))

    say("")
    say("=" * 100)
    say("== distinct b1 layouts in the frame ==")
    say("  %-16s %-18s %-6s %-7s %-6s %-8s %-9s %-7s %-7s %s"
        % ("vs", "block", "decl", "buffer", "first", "draws", "tag/blend",
           "truth", "propBad", "curBad"))
    for sig, L in sorted(seen_layouts.items(), key=lambda kv: -kv[1]["n"]):
        say("  %-16s %-18s %-6d %-7d %-6d %-8d %-9s %-7s %-7d %d"
            % (L["vs"], L["block"], L["size"], L["bufbytes"],
               L["firstConstant"], L["n"],
               "%d/%d" % (L["tagged"], L["blended"]),
               ("0x%02X" % L["truth"]) if L["truth"] is not None else "none",
               L["propWrong"], L["curWrong"]))
        say("        4x4s: %s"
            % ", ".join("+0x%04X %s" % (o, n) for o, n in L["mats"]))
        if L["o0"]:
            say("        o0 <- %s" % L["o0"][:220])

    say("")
    say("== draws where the PROPOSED rule disagrees with the reflected name ==")
    say("  %d (first 40 shown)" % len(mismatch_prop))
    for m in mismatch_prop:
        say("     eid %-7d vs %-16s %-18s %-5d truth 0x%02X rule 0x%02X "
            "tagged=%s blended=%s" % m)
    say("")
    say("== draws where the CURRENT rule disagrees ==")
    say("  %d (first 40 shown)" % len(mismatch_cur))
    for m in mismatch_cur:
        say("     eid %-7d vs %-16s %-18s %-5d truth 0x%02X rule 0x%02X "
            "tagged=%s blended=%s" % m)
    say("")
    say("  draws with no b1 block at all: %d" % no_b1)
    say("")
    say("DONE")
    controller.Shutdown()
    cap.Shutdown()


_lay_idx = {}


def lay_index(layout, key, controller, vsid):
    """Descriptor position of the b1 block = its index in refl.constantBlocks."""
    if key in _lay_idx:
        return _lay_idx[key]
    try:
        eps = controller.GetShaderEntryPoints(vsid)
        refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        for i, cb in enumerate(refl.constantBlocks):
            if int(cb.fixedBindNumber) == 1:
                _lay_idx[key] = i
                return i
    except Exception:
        pass
    _lay_idx[key] = -1
    return -1


try:
    main()
except Exception:
    say("EXCEPTION:")
    say(traceback.format_exc())

out.close()
os._exit(0)
