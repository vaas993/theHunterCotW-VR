"""How big is the actual D3D buffer the glass draw binds at b1?

Matters because cbscan.cpp's OTHER patch path, PatchWeaponMatrix, selects its
target with IsWeaponBuffer(ByteWidth == weapon_cb_size(192) && offset == 0).
If the glass's constant buffer really is a 192-byte D3D buffer then that path
is writing the glass block at +0x00 too.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_glass_bufsize.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame28475.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_glass_bufsize.txt")

WANT = ("144", "160", "320", "116")

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def main():
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        return

    names = {}
    for r in controller.GetResources():
        names[str(r.resourceId)] = r.name

    say("")
    say("== every constant buffer of 512 bytes or less ==")
    small = []
    for b in controller.GetBuffers():
        rid = str(b.resourceId)
        ln = int(b.length)
        if ln <= 512:
            small.append((ln, rid, names.get(rid, "")))
    small.sort()
    for ln, rid, nm in small:
        say("   %6d B  %-22s %s" % (ln, rid, nm))
    say("   (%d buffers of <=512 B)" % len(small))

    say("")
    say("== the buffers the viewmodel pass binds at b1 ==")
    for b in controller.GetBuffers():
        rid = str(b.resourceId)
        tail = rid.split("::")[-1]
        if tail in WANT:
            say("   %-22s length=%d  name=%s  flags=%s"
                % (rid, int(b.length), names.get(rid, ""),
                   str(getattr(b, "creationFlags", ""))))

    say("")
    say("== how many 192-byte constant buffers exist ==")
    n192 = [s for s in small if s[0] == 192]
    say("   %d" % len(n192))
    for ln, rid, nm in n192:
        say("     %s  %s" % (rid, nm))

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
