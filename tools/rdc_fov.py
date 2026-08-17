"""Read the game's ACTUAL projection matrix out of a RenderDoc capture and
report its true horizontal and vertical field of view.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_fov.py

Why this exists: the whole FOV question - is the game's "GameFOV" setting
horizontal or vertical? - has been guessed at repeatedly and got it wrong every
time. The projection matrix answers it outright:

    P[0][0] = 1 / tan(hFov/2)        (before any aspect handling)
    P[1][1] = 1 / tan(vFov/2)

so hFov = 2*atan(1/P00) and vFov = 2*atan(1/P11). Compare those against the
GameFOV the capture was taken at and the convention is settled.

Output goes to a FILE, because qrenderdoc's stdout does not survive being
launched detached.
"""
import math
import os
import struct

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\fov_report.txt")

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def looks_like_projection(m):
    """A D3D perspective matrix: [2][3] == +/-1, [3][3] == 0, P00/P11 > 0."""
    if not all(map(math.isfinite, m)):
        return None
    for name, p00, p11, z1, z2 in (
        ("row-major", m[0], m[5], m[11], m[15]),
        ("col-major", m[0], m[5], m[14], m[15]),
    ):
        if abs(abs(z1) - 1.0) > 1e-3 or abs(z2) > 1e-6:
            continue
        if not (0.05 < p00 < 50) or not (0.05 < p11 < 50):
            continue
        h = 2 * math.atan(1.0 / p00) * 180 / math.pi
        v = 2 * math.atan(1.0 / p11) * 180 / math.pi
        if 20 < h < 175 and 20 < v < 175:
            return (name, h, v, p00, p11)
    return None


say("capture: %s" % CAP)
cap = rd.OpenCaptureFile()
res = cap.OpenFile(CAP, "", None)
say("OpenFile -> %s" % res)

res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
if ctrl is None:
    say("could not open for replay: %s" % res)
    out.close()
else:
    try:
        actions = ctrl.GetRootActions()
    except AttributeError:
        actions = ctrl.GetDrawcalls()

    def leaves(lst):
        for a in lst:
            kids = getattr(a, "children", [])
            if kids:
                for k in leaves(kids):
                    yield k
            else:
                yield a

    all_leaves = list(leaves(actions))
    say("leaf actions: %d" % len(all_leaves))

    # Sample a few points through the frame; the main scene pass is somewhere in
    # the middle, shadow passes at the start.
    found = {}
    for frac in (0.25, 0.4, 0.55, 0.7):
        idx = int(len(all_leaves) * frac)
        if idx >= len(all_leaves):
            continue
        ctrl.SetFrameEvent(all_leaves[idx].eventId, True)
        for b in ctrl.GetBuffers():
            if b.length < 64 or b.length > 65536:
                continue
            try:
                data = ctrl.GetBufferData(b.resourceId, 0, 0)
            except Exception:
                continue
            if not data or len(data) < 64:
                continue
            for off in range(0, len(data) - 63, 16):
                m = struct.unpack_from("<16f", data, off)
                r = looks_like_projection(m)
                if r:
                    key = (round(r[1], 1), round(r[2], 1))
                    found.setdefault(key, r)

    say("")
    say("=== projection matrices found ===")
    for (h, v), (name, hh, vv, p00, p11) in sorted(found.items()):
        say("  %-10s  hFov %6.2f deg   vFov %6.2f deg   aspect(tan) %.4f"
            % (name, hh, vv, math.tan(math.radians(hh / 2)) /
               math.tan(math.radians(vv / 2))))
    if not found:
        say("  none found")
    say("")
    say("The game's settings.json had GameFOV = 90 when this was captured.")
    say("Whichever of hFov/vFov is ~90 tells you what that setting means.")

    ctrl.Shutdown()
    cap.Shutdown()
    out.close()
