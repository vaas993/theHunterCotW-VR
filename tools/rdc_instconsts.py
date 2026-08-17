"""InstanceConsts at the viewmodel draws - the actual numbers.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_instconsts.py

rdc_cb_api.py found that the viewmodel's vertex shader declares a 256-byte
constant block with NAMED fields, so nothing here has to be guessed:

    InstanceConsts (b1)
      +0x0000 WorldViewProjection    +0x00B0 Mirrored
      +0x0040 TextureMatrix          +0x00B8 WindConst
      +0x0070 World                  +0x00BC Alpha
      +0x00C0 RotWorld               +0x00F0 LocalWindDir
      +0x00FC PrevSkinningBoneOffset

TWO TRAPS, both already paid for:

  * `PipeState.GetConstantBuffer` does not exist on 1.45 - it is
    `GetConstantBlocks`, returning UsedDescriptor.  A bare `except: continue`
    around the old call turned an AttributeError into a confident and completely
    false "no transform in any cbuffer".
  * The `Descriptor` handed out by `.descriptor` is a temporary SWIG proxy.
    Holding one across loop iterations gets you a recycled object pointing at a
    different buffer - which is how a 256-byte block came back as 96 bytes.
    Every field is copied out, and the buffer read, before moving on.

So this reads EVERY bound VS constant buffer at each draw and identifies the
block by the shader's own reflection, then prints WorldViewProjection and World
and checks the two things the mod depends on: whether World's translation is
camera-near (a runtime test needing no shader identity), and whether the WVP is
per-object (it must be, for a per-eye shift applied per draw).

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_instconsts.txt
"""
import math
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_instconsts.txt")

EIDS = [33313, 33338, 33373, 33406,               # depth rounds
        33482, 33632, 33714, 33749, 33784,        # colour
        33851, 33876, 33901, 33956]

# Ordinary world draws for contrast: if World's translation is small for the
# weapon and large for these, the near-translation test discriminates.
CONTROL_EIDS = [22526, 23120, 24445]

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def mat4(data, off):
    if len(data) < off + 64:
        return None
    m = struct.unpack_from("<16f", data, off)
    return None if not all(map(math.isfinite, m)) else m


def show(label, m):
    say("       %s" % label)
    for r in range(4):
        say("          [%+11.4f %+11.4f %+11.4f %+11.4f]"
            % (m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3]))


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

    wvp_seen = {"weapon": [], "control": []}

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
            decl = list(refl.constantBlocks)
        except Exception as e:
            say("  eid %d: reflection failed: %s" % (eid, e))
            return

        say("")
        say("  == eid %-6d [%s] %d indices ==  declares: %s"
            % (eid, tag, getattr(a, "numIndices", 0),
               ", ".join("%s(b%s,%dB)" % (c.name, c.fixedBindNumber, c.byteSize)
                         for c in decl)))

        blocks = pipe.GetConstantBlocks(rd.ShaderStage.Vertex)
        # Copy every field out IMMEDIATELY - the descriptor proxy is temporary.
        reads = []
        for i, ud in enumerate(blocks):
            acc = getattr(ud, "access", None)
            idx = getattr(acc, "index", None) if acc is not None else None
            d = getattr(ud, "descriptor", None)
            if d is None:
                continue
            rid = d.resource
            boff = int(getattr(d, "byteOffset", 0))
            bsz = int(getattr(d, "byteSize", 0))
            if rid == rd.ResourceId.Null():
                continue
            want = min(bsz, 4096) if bsz else 4096
            try:
                data = bytes(controller.GetBufferData(rid, boff, want))
            except Exception as e:
                say("     [%d] %s read failed: %s" % (i, rid, e))
                continue
            reads.append((i, idx, rid, boff, bsz, data))
            say("     [%d] bind=%s %s off 0x%X declared %dB, got %dB"
                % (i, idx, rid, boff, bsz, len(data)))

        # Which read is InstanceConsts?
        #
        # NOT by fixedBindNumber.  `access.index` is the descriptor's position in
        # the list (0,1,2), while fixedBindNumber is the HLSL register (b1,b2,b3)
        # - so matching them lands exactly one entry too far along, which is how
        # a 256-byte block first came back as 96 bytes of MaterialConsts.
        #
        # RenderDoc clamps each read to the block's used range, so the returned
        # LENGTH identifies it outright: InstanceConsts is the read that came
        # back 256 bytes.  Declaration order is the fallback.
        inst = next((c for c in decl if c.name == "InstanceConsts"), None)
        if inst is None:
            say("     (this shader has no InstanceConsts - different pass)")
            return
        pick = next((r for r in reads if len(r[5]) == inst.byteSize), None)
        if pick is None:
            order = [c.name for c in decl].index("InstanceConsts")
            pick = reads[order] if order < len(reads) else None
        if pick is None:
            pick = next((r for r in reads if len(r[5]) >= 0xB0), None)
        if pick is None:
            say("     InstanceConsts declared but no bound buffer is big enough")
            return

        data = pick[5]
        say("     -> using [%d] %s (%d bytes)" % (pick[0], pick[2], len(data)))

        wvp = mat4(data, 0x00)
        wld = mat4(data, 0x70)
        if wvp is None or wld is None:
            say("     buffer too short for +0x70 (%d bytes)" % len(data))
            return

        show("WorldViewProjection +0x00", wvp)
        show("World +0x70", wld)
        rowp = (wld[12], wld[13], wld[14])
        colp = (wld[3], wld[7], wld[11])
        say("       World translation  row (%+.4f %+.4f %+.4f) |p|=%.4f"
            % (rowp[0], rowp[1], rowp[2], math.sqrt(sum(v * v for v in rowp))))
        say("                          col (%+.4f %+.4f %+.4f) |p|=%.4f"
            % (colp[0], colp[1], colp[2], math.sqrt(sum(v * v for v in colp))))
        wvp_seen[tag].append((eid, wvp))

    for eid in EIDS:
        dump(eid, "weapon")
    for eid in CONTROL_EIDS:
        dump(eid, "control")

    say("")
    say("== is WorldViewProjection per-object? ==")
    for tag, lst in wvp_seen.items():
        uniq = {tuple(round(v, 5) for v in m) for _, m in lst}
        say("  %-8s %d draw(s), %d distinct WVP -> %s"
            % (tag, len(lst), len(uniq),
               "PER-OBJECT" if len(uniq) > 1 else "shared across the pass"))

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
