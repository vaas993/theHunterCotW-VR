"""Radial profile of the stencil disc: exactly what each mask draw writes.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_ads_disc_profile.py

rdc_ads_mask_proof showed bit 0x40 appearing only at eid 17030 - the 13-point
line through the centre saw nothing from 17027.  That is either "17027 writes
nothing visible" or "17027 writes a ring thinner than the probe spacing".  A
dense radial sweep settles which, and gives the exact radius of each ring in
NDC and pixels so it can be matched against the shroud's inner edge.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ads_disc_profile_<cap>.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.environ.get("COTW_CAP") or os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame13783.rdc")
TAG = os.path.splitext(os.path.basename(CAP))[0]
OUT = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ads_disc_profile_" + TAG + ".txt")

EIDS = [int(x) for x in os.environ.get(
    "COTW_EIDS", "16972,17027,17030,17627,17707").split(",")]
N = 240

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def rid_of(o):
    for attr in ("resourceId", "resource"):
        v = getattr(o, attr, None)
        if v is not None:
            s = str(v)
            return None if s.endswith("::0") else v
    return None


def main():
    cap = rd.OpenCaptureFile()
    say("capture: %s" % CAP)
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return

    controller.SetFrameEvent(EIDS[-1], True)
    d3d = controller.GetD3D11PipelineState()
    dsv = rid_of(d3d.outputMerger.depthTarget)
    if dsv is None:
        say("no depth target at eid %d" % EIDS[-1])
        return
    t = next((x for x in controller.GetTextures() if x.resourceId == dsv), None)
    W, H = int(t.width), int(t.height)
    say("depth target %s  %dx%d  %s" % (dsv, W, H, t.format.Name()))
    cx, cy = W // 2, H // 2

    def stencil(x, y):
        pv = controller.PickPixel(dsv, x, y, rd.Subresource(0, 0, 0),
                                  rd.CompType.Typeless)
        f = list(pv.floatValue)
        return int(round(f[1] * 255.0)) if len(f) > 1 else -1

    for eid in EIDS:
        controller.SetFrameEvent(eid, True)
        say("")
        say("=" * 74)
        say("== stencil along +x from the centre, after eid %d ==" % eid)
        prev = None
        runs = []
        for i in range(N + 1):
            f = i / float(N)                 # NDC x in 0..1
            x = min(W - 1, int(cx + f * cx))
            s = stencil(x, cy) & 0xFF
            b = 1 if (s & 0x40) else 0
            if prev is None or b != prev[0]:
                runs.append([b, f, f, s])
                prev = (b, f)
            else:
                runs[-1][2] = f
                runs[-1][3] = s
        for b, f0, f1, s in runs:
            say("   bit0x40=%s   NDC x %.4f .. %.4f   px %d .. %d   "
                "last stencil 0x%02X"
                % ("SET" if b else " . ", f0, f1,
                   int(cx + f0 * cx), int(cx + f1 * cx), s))

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
