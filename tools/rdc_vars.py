"""Every named constant, at its own reflected offset, at the viewmodel draws.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_vars.py

WHY THIS EXISTS: rdc_instconsts.py read WorldViewProjection at +0x00 and World
at +0x70 because that is what ONE shader (11F22A82) declares.  The viewmodel
pass uses seven different vertex shaders and each packs InstanceConsts its own
way, so +0x70 in the others is some unrelated variable - it came back holding
2560 and 1637, which are render-target dimensions, at every single draw.

The reflection knows every offset by name, so nothing here is hardcoded: for
each draw we walk the shader's own variable table and print each constant at ITS
declared offset, with its declared shape.

What we are looking for is the viewmodel's placement: a transform whose
translation is within about a metre of the origin (the renderer is
camera-relative), differing per mesh, and present at the weapon draws but not at
the control draws out in the world.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_vars.txt
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_vars.txt")

# 33313 and 33632 are the 77484-index mesh - the hands. Its shader declares
# LocalConstants (6944 B) instead of InstanceConsts (256 B), so its transform is
# NOT at +0x00, and shifting +0x00 there hits some unrelated constant: the hands
# get no depth and a faint ghost instead. These two eids are here to find out
# what LocalConstants actually contains and where its matrix lives.
WEAPON_EIDS = [33632]
CONTROL_EIDS = []

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def shape_of(v):
    """(rows, cols, typename) however this build spells it."""
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


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open")

    leaves = []

    def walk(lst):
        for a in lst:
            kids = getattr(a, "children", [])
            if kids:
                walk(kids)
            else:
                leaves.append(a)

    walk(controller.GetRootActions())
    by_eid = {a.eventId: a for a in leaves}

    # name -> list of (eid, tag, translation) for the summary at the end
    near = {}

    def dump(eid, tag):
        a = by_eid.get(eid)
        if a is None:
            say("  eid %d: not a leaf action" % eid)
            return
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        try:
            vsid = pipe.GetShader(rd.ShaderStage.Vertex)
            eps = controller.GetShaderEntryPoints(vsid)
            refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        except Exception as e:
            say("  eid %d reflection failed: %s" % (eid, e))
            return

        say("")
        say("=" * 74)
        say("  eid %-6d [%s]  %d indices" % (eid, tag, getattr(a, "numIndices", 0)))

        blocks = pipe.GetConstantBlocks(rd.ShaderStage.Vertex)
        reads = []
        for ud in blocks:
            d = getattr(ud, "descriptor", None)
            if d is None:
                continue
            rid, boff, bsz = d.resource, int(d.byteOffset), int(d.byteSize)
            if rid == rd.ResourceId.Null():
                continue
            try:
                reads.append(bytes(controller.GetBufferData(
                    rid, boff, min(bsz, 16384) if bsz else 16384)))
            except Exception:
                reads.append(b"")

        for bi, cb in enumerate(refl.constantBlocks):
            # Match the declared block to a read by its size, then by order.
            data = next((d for d in reads if len(d) == cb.byteSize), None)
            if data is None:
                data = reads[bi] if bi < len(reads) else b""
            say("  -- %s (b%s, %d bytes declared, %d read) --"
                % (cb.name, cb.fixedBindNumber, cb.byteSize, len(data)))
            # 16384, not 4096. LocalConstants is 6944 bytes and was being skipped
            # as a "bulk array" - but it is exactly the block the hands' shader
            # reads, and where ITS matrix sits is the whole question.
            if cb.byteSize > 16384:
                say("     (skipping - bulk array, not placement data)")
                continue
            for v in cb.variables:
                off = int(v.byteOffset)
                rows, cols, tn = shape_of(v)
                n = max(rows * cols, 1)
                if off + n * 4 > len(data):
                    say("     +0x%04X %-28s %dx%d  (past end of read)"
                        % (off, v.name, rows, cols))
                    continue
                vals = struct.unpack_from("<%df" % n, data, off)
                if not all(map(math.isfinite, vals)):
                    say("     +0x%04X %-28s %dx%d  non-finite"
                        % (off, v.name, rows, cols))
                    continue
                if rows >= 3 and cols >= 4:
                    say("     +0x%04X %-28s %dx%d" % (off, v.name, rows, cols))
                    for r in range(rows):
                        say("            [%s]" % " ".join(
                            "%+10.4f" % vals[r * cols + c] for c in range(cols)))
                    # Translation in both conventions.
                    rowp = (vals[12], vals[13], vals[14]) if n >= 16 else None
                    colp = (vals[3], vals[7], vals[11])
                    for lbl, p in (("row", rowp), ("col", colp)):
                        if p is None:
                            continue
                        d = math.sqrt(sum(x * x for x in p))
                        mark = ""
                        if 0.02 < d < 2.0:
                            mark = "   <<< CAMERA-NEAR"
                            near.setdefault(v.name, []).append((eid, tag, lbl, p, d))
                        say("            %s-translation (%+.4f %+.4f %+.4f) |p|=%.4f%s"
                            % (lbl, p[0], p[1], p[2], d, mark))
                else:
                    say("     +0x%04X %-28s %dx%d = %s"
                        % (off, v.name, rows, cols,
                           " ".join("%+.4f" % x for x in vals[:8])))

    for eid in WEAPON_EIDS:
        dump(eid, "weapon")
    for eid in CONTROL_EIDS:
        dump(eid, "control")

    say("")
    say("=" * 74)
    say("== camera-near transforms, by variable name ==")
    if not near:
        say("  none at any draw")
    for name, lst in sorted(near.items(), key=lambda kv: -len(kv[1])):
        w = [e for e in lst if e[1] == "weapon"]
        c = [e for e in lst if e[1] == "control"]
        say("  %-28s %d weapon draw(s), %d control draw(s)  %s"
            % (name, len(w), len(c),
               "DISCRIMINATES" if w and not c else "also fires in the world"))
        for eid, tag, lbl, p, d in lst[:10]:
            say("        eid %-6d %-7s %s (%+.4f %+.4f %+.4f) |p|=%.4f"
                % (eid, tag, lbl, p[0], p[1], p[2], d))

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
