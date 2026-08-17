"""How exclusive is StencilReadMask==0x40 across the WHOLE SESSION?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_refute_ds4.py

"It works in two frames" is weak evidence about other weapons and other places.
A stronger bound: a capture carries every CreateDepthStencilState the process
made since it started, including states for passes this frame never ran. If the
engine created 40 depth-stencil states and only the viewmodel's three ask for
StencilReadMask == 0x40, the predicate is exclusive over far more of the
renderer than one frame's draws can show. If a dozen unrelated states also use
0x40, the two clean frames are luck.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_refute4_<tag>.txt
"""
import os
import traceback

import renderdoc as rd

CAPNAME = os.environ.get("REFUTE_CAP", "cotw_frame4317.rdc")
TAG = CAPNAME.replace("cotw_frame", "").replace(".rdc", "")
CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\\" + CAPNAME)
OUT = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute4_" + TAG + ".txt")

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def kids(o):
    try:
        return [o.GetChild(i) for i in range(o.NumChildren())]
    except Exception:
        try:
            return list(o)
        except Exception:
            return []


def flat(o, prefix="", depth=0, acc=None):
    """name -> scalar, flattened over the whole SDObject tree."""
    if acc is None:
        acc = {}
    if depth > 6:
        return acc
    ch = kids(o)
    nm = prefix + ("." if prefix else "") + str(getattr(o, "name", ""))
    if not ch:
        val = None
        for meth in ("AsUInt64", "AsInt64", "AsString", "AsDouble"):
            try:
                val = getattr(o, meth)()
                break
            except Exception:
                continue
        acc[nm] = val
        return acc
    for c in ch:
        flat(c, nm, depth + 1, acc)
    return acc


def main():
    cap = rd.OpenCaptureFile()
    say("capture: %s" % CAP)
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))

    sf = None
    controller = None
    try:
        sf = cap.GetStructuredData()
        say("structured data read WITHOUT starting a replay")
    except Exception:
        say("GetStructuredData failed, falling back to a replay:")
        say(traceback.format_exc())
    if sf is None or not len(sf.chunks):
        res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
        if controller is None:
            say("could not start replay: %s" % res)
            return
        sf = controller.GetStructuredFile()

    chunks = sf.chunks
    say("%d chunks total" % len(chunks))

    dss = [c for c in chunks if "CreateDepthStencilState" in c.name]
    say("%d CreateDepthStencilState chunks" % len(dss))

    if dss:
        say("")
        say("-- flattened structure of the first one, so nothing is guessed --")
        f0 = flat(dss[0])
        for k in sorted(f0):
            say("   %-70s = %s" % (k, f0[k]))

    say("")
    say("=" * 78)
    say("== every depth-stencil state the process created ==")
    say("  %-5s %-7s %-6s %-14s %-8s %-8s %s"
        % ("#", "dEnable", "dWrite", "depthFunc", "sEnable", "readMask", "writeMask"))
    n40 = 0
    rows = []
    for i, c in enumerate(dss):
        f = flat(c)

        def get(suffix, default=None):
            for k in f:
                if k.endswith(suffix):
                    return f[k]
            return default

        de = get("DepthEnable")
        dw = get("DepthWriteMask")
        df = get("DepthFunc")
        se = get("StencilEnable")
        rm = get("StencilReadMask")
        wm = get("StencilWriteMask")
        try:
            rmi = int(rm)
        except Exception:
            rmi = -1
        if rmi == 0x40:
            n40 += 1
        rows.append((i, de, dw, df, se, rmi, wm))
        say("  %-5d %-7s %-6s %-14s %-8s %-8s %-8s%s"
            % (i, de, dw, df, se,
               ("0x%02X" % rmi) if rmi >= 0 else str(rm), wm,
               "   <<< readMask 0x40" if rmi == 0x40 else ""))

    say("")
    say("states with StencilReadMask == 0x40: %d of %d" % (n40, len(dss)))
    hist = {}
    for r in rows:
        hist[r[5]] = hist.get(r[5], 0) + 1
    say("read-mask histogram over ALL created states:")
    for k in sorted(hist, key=lambda k: -hist[k]):
        say("   0x%02X : %d" % (k, hist[k]) if k >= 0 else "   ?    : %d" % hist[k])

    # ---- the same for blend/raster, as a sanity check on how many states exist
    for other in ("CreateBlendState", "CreateRasterizerState"):
        say("   (%s chunks: %d)"
            % (other, len([c for c in chunks if other in c.name])))

    say("")
    say("DONE")
    if controller:
        controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    say("EXCEPTION:")
    say(traceback.format_exc())

out.close()
os._exit(0)
