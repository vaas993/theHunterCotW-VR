"""Head-to-head: learned index-buffer pointer set vs an intrinsic pipeline tag.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_ib_vs_stencil.py

The IB claim says index-buffer resource identity is "THE ONE THAT SEPARATES".
That is only interesting if nothing INTRINSIC separates - because the IB rule
needs an oracle to populate its pointer list, while an intrinsic tag does not.
This sweeps every drawcall once and scores both on the same population, and
also classifies the two draws immediately after the pass (34005/34014) which the
IB claim counted as world draws while admitting they share the viewmodel's
vertex buffer.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ib_vs_stencil.txt
"""
import os
import sys
import time
import traceback
from collections import defaultdict

import renderdoc as rd

CAPS = [
    (os.path.expandvars(
        r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc"),
     33313, 33956, "4317"),
]
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ib_vs_stencil.txt")

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def rid(r):
    if r is None:
        return "0"
    s = str(r)
    d = "".join(ch for ch in s if ch.isdigit())
    return d if d else s


def sweep(path, lo, hi, tag):
    t0 = time.time()
    cap = rd.OpenCaptureFile()
    say("")
    say("#" * 74)
    say("# capture %s  pass = %d..%d" % (tag, lo, hi))
    say("OpenFile -> %s" % cap.OpenFile(path, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open (%.1fs)" % (time.time() - t0))

    sf = controller.GetStructuredFile()
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
    say("%d drawcalls" % len(draws))

    recs = []
    errors = []
    for i, a in enumerate(draws):
        eid = a.eventId
        try:
            controller.SetFrameEvent(eid, False)
            d3d = controller.GetD3D11PipelineState()
            ia = d3d.inputAssembly
            ib = rid(getattr(ia.indexBuffer, "resourceId", None))

            om = d3d.outputMerger
            dss = om.depthStencilState
            se = bool(getattr(dss, "stencilEnable", False))
            ff = dss.frontFace
            bf = dss.backFace
            ref = int(getattr(ff, "reference", 0))
            cm = int(getattr(ff, "compareMask", 0))
            wm = int(getattr(ff, "writeMask", 0))
            bref = int(getattr(bf, "reference", 0))
            bcm = int(getattr(bf, "compareMask", 0))
            dsobj = rid(getattr(dss, "resourceId", None)) \
                if hasattr(dss, "resourceId") else "-"

            name = a.GetName(sf)
            api = name.split("(")[0] if name else "?"
            recs.append(dict(eid=eid, inpass=(lo <= eid <= hi), api=api,
                             n=int(getattr(a, "numIndices", 0)), ib=ib,
                             se=se, ref=ref, cm=cm, wm=wm,
                             bref=bref, bcm=bcm, ds=dsobj))
        except Exception as e:
            errors.append((eid, type(e).__name__, str(e)))
        if i % 400 == 0:
            say("  ...%d/%d (%.1fs)" % (i, len(draws), time.time() - t0))
    say("swept %d, errors %d" % (len(recs), len(errors)))
    for e in errors[:10]:
        say("   ERR eid %d %s %s" % e)

    inp = [r for r in recs if r["inpass"]]
    outp = [r for r in recs if not r["inpass"]]
    DI = "ID3D11DeviceContext::DrawIndexed"
    di_out = [r for r in outp if r["api"] == DI]
    say("population: %d total, %d in-pass, %d outside (%d of them DrawIndexed)"
        % (len(recs), len(inp), len(outp), len(di_out)))

    S = set(r["ib"] for r in inp)

    def score(label, pred):
        h = sum(1 for r in inp if pred(r))
        f = [r for r in outp if pred(r)]
        fdi = [r for r in f if r["api"] == DI]
        say("  %-46s hits %2d/%-3d  FP %4d/%-5d (%3d DrawIndexed)"
            % (label, h, len(inp), len(f), len(outp), len(fdi)))
        return h, f

    say("")
    say("== head to head, same population ==")
    score("LEARNED index-buffer pointer set (%d ptrs)" % len(S),
          lambda r: r["ib"] in S)
    score("INTRINSIC stencil ref bit 0x40", lambda r: bool(r["ref"] & 0x40))
    score("INTRINSIC stencil compareMask == 0x40", lambda r: r["cm"] == 0x40)
    score("INTRINSIC both (enable & cm==0x40 & ref&0x40)",
          lambda r: r["se"] and r["cm"] == 0x40 and (r["ref"] & 0x40))
    score("(control) stencil compareMask & 0x40 - bit test",
          lambda r: bool(r["cm"] & 0x40))

    say("")
    say("== stencil state of every in-pass draw ==")
    for r in inp:
        say("   eid %-6d n=%-6d ib=%-7s se=%d ref=0x%02X cm=0x%02X wm=0x%02X "
            "back(ref=0x%02X cm=0x%02X)"
            % (r["eid"], r["n"], r["ib"], r["se"], r["ref"], r["cm"], r["wm"],
               r["bref"], r["bcm"]))

    say("")
    say("== world stencil distribution (compareMask -> count) ==")
    d = defaultdict(int)
    for r in outp:
        d[r["cm"]] += 1
    for k in sorted(d, key=lambda k: -d[k]):
        say("   cm=0x%02X : %d" % (k, d[k]))
    d = defaultdict(int)
    for r in outp:
        d[r["ref"]] += 1
    say("   world stencil refs: %s"
        % ", ".join("0x%02X:%d" % (k, d[k])
                    for k in sorted(d, key=lambda k: -d[k])))
    say("   world draws with ref bit 0x40 set: %d"
        % sum(1 for r in outp if r["ref"] & 0x40))

    say("")
    say("== the draws just outside the pass: world, or missed viewmodel? ==")
    for r in recs:
        if 33940 <= r["eid"] <= 34130:
            flag = "VM-RANGE" if r["inpass"] else "outside "
            vm_ib = "IBrule=%s" % ("HIT " if r["ib"] in S else "miss")
            vm_st = "STENCILrule=%s" % ("HIT " if (r["ref"] & 0x40) else "miss")
            say("   %s eid %-6d %-40s n=%-6d ib=%-7s ref=0x%02X cm=0x%02X  "
                "%s %s"
                % (flag, r["eid"], r["api"], r["n"], r["ib"], r["ref"],
                   r["cm"], vm_ib, vm_st))

    say("")
    say("== does anything outside the pass carry a first-person stencil tag? ==")
    tagged = [r for r in outp if (r["ref"] & 0x40) or r["cm"] == 0x40]
    say("   %d outside draws carry ref bit 0x40 or compareMask 0x40" % len(tagged))
    for r in tagged[:40]:
        say("      eid %-6d %-40s n=%-6d ref=0x%02X cm=0x%02X"
            % (r["eid"], r["api"], r["n"], r["ref"], r["cm"]))

    say("")
    say("capture %s done %.1fs" % (tag, time.time() - t0))
    controller.Shutdown()
    cap.Shutdown()


try:
    for p, lo, hi, tg in CAPS:
        sweep(p, lo, hi, tg)
    say("")
    say("ALL DONE")
except Exception:
    say("EXCEPTION:")
    say(traceback.format_exc())

out.close()
os._exit(0)
