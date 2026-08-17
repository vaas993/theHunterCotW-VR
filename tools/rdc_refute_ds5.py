"""ds4 with a working scalar accessor: every DS state the process created.

SDObject.AsUInt64() returned nothing for the integer fields, so this probes the
raw data union first and prints what it found, then does the census. No replay
is started - GetStructuredData is enough and takes seconds instead of minutes.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_refute5_<tag>.txt
"""
import os
import traceback

import renderdoc as rd

CAPNAME = os.environ.get("REFUTE_CAP", "cotw_frame4317.rdc")
TAG = CAPNAME.replace("cotw_frame", "").replace(".rdc", "")
CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\\" + CAPNAME)
OUT = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\rdc_refute5_" + TAG + ".txt")

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def kids(o):
    try:
        return [o.GetChild(i) for i in range(o.NumChildren())]
    except Exception:
        return []


def scalar(o):
    """Whatever this leaf actually holds."""
    for path in ("data.basic.u", "data.basic.i", "data.basic.d",
                 "data.basic.b", "data.str"):
        cur = o
        try:
            for part in path.split("."):
                cur = getattr(cur, part)
            if cur is not None and cur != "":
                return cur
        except Exception:
            continue
    for meth in ("AsUInt64", "AsInt64", "AsString"):
        try:
            v = getattr(o, meth)()
            if v not in (None, ""):
                return v
        except Exception:
            continue
    return None


def find(o, wanted, depth=0):
    if depth > 6:
        return None
    for c in kids(o):
        if str(getattr(c, "name", "")) == wanted:
            return c
        r = find(c, wanted, depth + 1)
        if r is not None:
            return r
    return None


def main():
    cap = rd.OpenCaptureFile()
    say("capture: %s" % CAP)
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    sf = cap.GetStructuredData()
    say("%d chunks" % len(sf.chunks))

    dss = [c for c in sf.chunks if "CreateDepthStencilState" in c.name]
    say("%d CreateDepthStencilState chunks" % len(dss))
    if not dss:
        return

    # probe the accessor on the first chunk
    say("")
    say("-- accessor probe on chunk 0 --")
    for fld in ("StencilReadMask", "StencilWriteMask", "StencilEnable",
                "DepthEnable"):
        n = find(dss[0], fld)
        if n is None:
            say("   %-18s NOT FOUND" % fld)
            continue
        say("   %-18s type=%s  scalar=%r"
            % (fld, str(getattr(n, "type", "?")), scalar(n)))

    say("")
    say("=" * 78)
    say("== every depth-stencil state created by the process ==")
    say("  %-4s %-8s %-9s %-24s %-9s %-9s %s"
        % ("#", "dEnable", "dWrite", "depthFunc", "sEnable", "readMask", "writeMask"))
    hist = {}
    n40 = 0
    for i, c in enumerate(dss):
        def g(name):
            n = find(c, name)
            return scalar(n) if n is not None else None
        de, dw = g("DepthEnable"), g("DepthWriteMask")
        df, se = g("DepthFunc"), g("StencilEnable")
        rm, wm = g("StencilReadMask"), g("StencilWriteMask")
        ff = find(c, "FrontFace")
        sfunc = None
        if ff is not None:
            sn = find(ff, "StencilFunc")
            sfunc = scalar(sn) if sn is not None else None
        try:
            rmi = int(rm)
        except Exception:
            rmi = -1
        hist[rmi] = hist.get(rmi, 0) + 1
        if rmi == 0x40:
            n40 += 1
        say("  %-4d %-8s %-9s %-24s %-9s %-9s %-9s %s%s"
            % (i, de, str(dw).replace("D3D11_DEPTH_WRITE_MASK_", ""),
               str(df).replace("D3D11_COMPARISON_", ""), se,
               ("0x%02X" % rmi) if rmi >= 0 else str(rm), wm,
               str(sfunc).replace("D3D11_COMPARISON_", ""),
               "   <<< readMask 0x40" if rmi == 0x40 else ""))

    say("")
    say("states with StencilReadMask == 0x40: %d of %d" % (n40, len(dss)))
    say("read-mask histogram over EVERY state the process created:")
    for k in sorted(hist, key=lambda k: -hist[k]):
        say("   %-6s : %d" % (("0x%02X" % k) if k >= 0 else "?", hist[k]))

    say("")
    say("DONE")
    cap.Shutdown()


try:
    main()
except Exception:
    say("EXCEPTION:")
    say(traceback.format_exc())

out.close()
os._exit(0)
