"""BINOCULAR GLASS, part 4: the exact bits the runtime test has to key on.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_glass_binos4.py

The proposed runtime rule is structural rather than magnitude-based:

    the matrix at +0x00 is a World/affine transform  <=>  its 4th COLUMN
    (m[3], m[7], m[11], m[15]) is exactly (0, 0, 0, 1)

    affine at +0x00 -> the clip matrix is at +0x40, otherwise +0x00

That only works if the engine really stores literal 0.0f / 1.0f there, so this
prints the raw hex and the exact float repr of those four words, for every
constant-block layout in the viewmodel pass - and checks whether the glass's
WorldViewProjection at +0x40 is bit-identical to the binocular body's
WorldViewProjection at +0x00, which would let the mod cross-check a blended draw
against the tagged draw it belongs to.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_glass_binos4.txt
"""
import binascii
import os
import struct
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame18022.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_glass_binos4.txt")

EIDS = [(31395, "GLASS lens         InstanceConsts 128"),
        (31246, "body colour        InstanceConsts 256"),
        (30978, "body depth         InstanceConsts 256"),
        (31316, "skinned colour     InstanceConsts 256"),
        (31167, "hands colour       LocalConstants 6944")]

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return

    keep = {}
    for eid, label in EIDS:
        controller.SetFrameEvent(eid, True)
        pipe = controller.GetPipelineState()
        vsid = pipe.GetShader(rd.ShaderStage.Vertex)
        eps = controller.GetShaderEntryPoints(vsid)
        refl = controller.GetShader(rd.ResourceId.Null(), vsid, eps[0])
        cbk = refl.constantBlocks[0]

        data = b""
        for ud in pipe.GetConstantBlocks(rd.ShaderStage.Vertex):
            acc = getattr(ud, "access", None)
            if acc is None or int(getattr(acc, "index", -1)) != 0:
                continue
            d = getattr(ud, "descriptor", None)
            if d is None or d.resource == rd.ResourceId.Null():
                continue
            data = bytes(controller.GetBufferData(
                d.resource, int(d.byteOffset), 0x80))
            break

        say("")
        say("=" * 78)
        say("eid %-6d %s   block %s (b%s) %d B"
            % (eid, label, cbk.name, cbk.fixedBindNumber, cbk.byteSize))
        vnames = {int(v.byteOffset): v.name for v in cbk.variables}
        for off in (0x00, 0x40):
            if off + 64 > len(data):
                say("  +0x%02X past end of read (%d B)" % (off, len(data)))
                continue
            m = struct.unpack_from("<16f", data, off)
            say("  +0x%02X  %-26s" % (off, vnames.get(off, "(unnamed)")))
            say("        raw  %s" % binascii.hexlify(
                data[off:off + 64]).decode())
            col3 = (m[3], m[7], m[11], m[15])
            say("        4th column m[3],m[7],m[11],m[15] = %r" % (col3,))
            say("        as bits  %s"
                % " ".join("%08X" % struct.unpack("<I", struct.pack("<f", x))[0]
                           for x in col3))
            exact = (m[3] == 0.0 and m[7] == 0.0 and m[11] == 0.0
                     and m[15] == 1.0)
            say("        EXACTLY (0,0,0,1)? %s   -> %s"
                % (exact, "AFFINE, clip matrix is at +0x40"
                   if exact else "this IS the clip matrix"))
            keep[(eid, off)] = data[off:off + 64]

    say("")
    say("=" * 78)
    say("== is the glass's WVP the same bytes as the body's WVP? ==")
    g = keep.get((31395, 0x40))
    b = keep.get((31246, 0x00))
    if g and b:
        say("  glass eid 31395 +0x40 : %s" % binascii.hexlify(g).decode())
        say("  body  eid 31246 +0x00 : %s" % binascii.hexlify(b).decode())
        say("  BIT-IDENTICAL: %s" % (g == b))
        if g != b:
            gm = struct.unpack("<16f", g)
            bm = struct.unpack("<16f", b)
            say("  max |diff| = %g" % max(abs(x - y) for x, y in zip(gm, bm)))
    else:
        say("  one of them was not read")

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
