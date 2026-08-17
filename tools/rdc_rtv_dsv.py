"""Which render targets / depth-stencil are bound at every draw in the frame?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_rtv_dsv.py

THE QUESTION: is there an RTV, a DSV, or a combination of the two that is bound
for essentially every viewmodel draw in eid 33313..33956 and for essentially
nothing else in the frame?  If so a DrawIndexed hook can call
OMGetRenderTargets, compare one pointer, and be done - no shader hash, no index
count, no global counter.

Two levels matter and they are NOT the same thing:
  * RESOURCE level - which ID3D11Texture2D the view points at.  This is what
    ActionDescription.outputs / .depthOut report, and it is free (no replay).
  * VIEW level - which ID3D11RenderTargetView / ID3D11DepthStencilView object is
    bound.  This is what OMGetRenderTargets actually hands the hook.  Two views
    onto the same texture are different pointers, so the view level can
    discriminate where the resource level cannot (and vice versa: a view can be
    recreated, a texture cannot).  Only the pipeline state knows this, which
    costs a SetFrameEvent per draw.

So: pass 1 walks every action for free and buckets by (outputs, depthOut).
Pass 2 replays every draw and records the actual view descriptors, plus the
viewport, so the report can say whether the hook should compare a view pointer
or a resource pointer.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_rtv_dsv.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_rtv_dsv.txt")

VM_LO, VM_HI = 33313, 33956

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def inrange(eid):
    return VM_LO <= eid <= VM_HI


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
    draws = [a for a in leaves if a.flags & rd.ActionFlags.Drawcall]
    say("%d leaf actions, %d drawcalls" % (len(leaves), len(draws)))

    vm_draws = [a for a in draws if inrange(a.eventId)]
    ot_draws = [a for a in draws if not inrange(a.eventId)]
    say("viewmodel range %d..%d : %d draws" % (VM_LO, VM_HI, len(vm_draws)))
    say("outside the range      : %d draws" % len(ot_draws))
    say("")
    say("viewmodel draws, eid + index count:")
    for a in vm_draws:
        say("   %6d  %7d indices" % (a.eventId, getattr(a, "numIndices", 0)))

    textures = {t.resourceId: t for t in controller.GetTextures()}
    resnames = {}
    for r in controller.GetResources():
        resnames[r.resourceId] = r.name

    def texname(rid):
        if rid == rd.ResourceId.Null():
            return "NULL"
        t = textures.get(rid)
        nm = resnames.get(rid, "")
        if t is None:
            return "%s %s" % (rid, nm)
        return "%s %dx%d %s%s%s" % (
            rid, t.width, t.height, t.format.Name(),
            " arr%d" % t.arraysize if t.arraysize > 1 else "",
            (" '%s'" % nm) if nm else "")

    # ---------------- pass 1: resource level, free ------------------------
    say("")
    say("=" * 78)
    say("== PASS 1: resource level (ActionDescription.outputs / .depthOut) ==")

    combos = {}         # (outs, depth) -> [in_eids, out_eids]
    rtv_use = {}        # rid -> [in, out]
    dsv_use = {}
    for a in draws:
        outs = tuple(o for o in a.outputs if o != rd.ResourceId.Null())
        depth = a.depthOut
        key = (outs, depth)
        slot = combos.setdefault(key, [[], []])
        slot[0 if inrange(a.eventId) else 1].append(a.eventId)
        for o in outs:
            s = rtv_use.setdefault(o, [0, 0])
            s[0 if inrange(a.eventId) else 1] += 1
        if depth != rd.ResourceId.Null():
            s = dsv_use.setdefault(depth, [0, 0])
            s[0 if inrange(a.eventId) else 1] += 1

    say("")
    say("-- every distinct (render targets, depth) combination --")
    say("   %5s %5s  %s" % ("IN", "OUT", "targets"))
    ordered = sorted(combos.items(), key=lambda kv: -len(kv[1][0]))
    for (outs, depth), (ins, outs_e) in ordered:
        mark = ""
        if ins and not outs_e:
            mark = "   <<<< EXCLUSIVE TO THE VIEWMODEL RANGE"
        elif ins:
            mark = "   (shared)"
        say("")
        say("   %5d %5d%s" % (len(ins), len(outs_e), mark))
        if not outs:
            say("        rtv  (none bound)")
        for o in outs:
            say("        rtv  %s" % texname(o))
        say("        dsv  %s" % texname(depth))
        if ins:
            say("        in-range eids  : %s%s"
                % (ins[:14], " ..." if len(ins) > 14 else ""))
        if outs_e:
            say("        out-range eids : %s%s"
                % (outs_e[:14], " ..." if len(outs_e) > 14 else ""))

    say("")
    say("-- each RTV resource on its own --")
    for rid, (i, o) in sorted(rtv_use.items(), key=lambda kv: -kv[1][0]):
        say("   in=%-5d out=%-5d  %s%s" % (i, o, texname(rid),
                                           "   <<<< IN ONLY" if i and not o else ""))
    say("")
    say("-- each DSV resource on its own --")
    for rid, (i, o) in sorted(dsv_use.items(), key=lambda kv: -kv[1][0]):
        say("   in=%-5d out=%-5d  %s%s" % (i, o, texname(rid),
                                           "   <<<< IN ONLY" if i and not o else ""))

    # ---------------- pass 2: view level, needs replay --------------------
    say("")
    say("=" * 78)
    say("== PASS 2: view level (what OMGetRenderTargets would hand the hook) ==")

    controller.SetFrameEvent(draws[0].eventId, True)
    pipe = controller.GetPipelineState()
    say("")
    say("-- output-merger methods on this build --")
    for n in sorted(dir(pipe)):
        if n.startswith("_"):
            continue
        low = n.lower()
        if any(k in low for k in ("output", "depth", "target", "stencil",
                                  "viewport", "scissor")):
            say("     %s" % n)

    say("")
    say("-- shape of one GetOutputTargets entry --")
    try:
        ots = pipe.GetOutputTargets()
        say("   GetOutputTargets -> %d entries, type %s"
            % (len(ots), type(ots[0]).__name__ if len(ots) else "-"))
        if len(ots):
            say("   attrs %s" % [a for a in dir(ots[0])
                                 if not a.startswith("_")])
            d = getattr(ots[0], "descriptor", None)
            if d is not None:
                say("   descriptor attrs %s"
                    % [a for a in dir(d) if not a.startswith("_")])
    except Exception as e:
        say("   GetOutputTargets RAISED %s: %s" % (type(e).__name__, e))
    try:
        dt = pipe.GetDepthTarget()
        say("   GetDepthTarget -> type %s" % type(dt).__name__)
        say("   attrs %s" % [a for a in dir(dt) if not a.startswith("_")])
        d = getattr(dt, "descriptor", None)
        if d is not None:
            say("   descriptor attrs %s"
                % [a for a in dir(d) if not a.startswith("_")])
    except Exception as e:
        say("   GetDepthTarget RAISED %s: %s" % (type(e).__name__, e))

    def om_state():
        """(rtv view ids, rtv resource ids, dsv view id, dsv res id, dsv flags,
            viewport tuple) -- every field COPIED out of the SWIG proxies."""
        p = controller.GetPipelineState()
        rtv_v, rtv_r = [], []
        try:
            for ud in p.GetOutputTargets():
                d = getattr(ud, "descriptor", ud)
                vw = getattr(d, "view", None)
                rs = getattr(d, "resource", None)
                if rs is None or rs == rd.ResourceId.Null():
                    continue
                rtv_v.append(str(vw) if vw is not None else "?")
                rtv_r.append(str(rs))
        except Exception as e:
            rtv_v = ["ERR %s" % e]
        dv = dr = "?"
        dflags = ""
        try:
            ud = p.GetDepthTarget()
            d = getattr(ud, "descriptor", ud)
            vw = getattr(d, "view", None)
            rs = getattr(d, "resource", None)
            dv = str(vw) if vw is not None else "?"
            dr = str(rs) if rs is not None else "?"
            for f in ("flags", "textureType", "firstMip", "numMips",
                      "firstSlice", "numSlices"):
                if hasattr(d, f):
                    dflags += " %s=%s" % (f, getattr(d, f))
        except Exception as e:
            dv = "ERR %s" % e
        vp = ""
        try:
            v = p.GetViewport(0)
            vp = "%.0fx%.0f@%.0f,%.0f z%.3f..%.3f" % (
                v.width, v.height, v.x, v.y, v.minDepth, v.maxDepth)
        except Exception:
            pass
        return tuple(rtv_v), tuple(rtv_r), dv, dr, dflags, vp

    vcombos = {}        # (rtv_v, dsv_v) -> [in, out]
    vrtv = {}
    vdsv = {}
    vport = {}
    detail_in = []
    detail_out_samples = []
    for n, a in enumerate(draws):
        controller.SetFrameEvent(a.eventId, True)
        rtv_v, rtv_r, dv, dr, dflags, vp = om_state()
        isin = inrange(a.eventId)
        k = (rtv_v, dv)
        s = vcombos.setdefault(k, [[], []])
        s[0 if isin else 1].append(a.eventId)
        for v in rtv_v:
            t = vrtv.setdefault(v, [0, 0])
            t[0 if isin else 1] += 1
        t = vdsv.setdefault(dv, [0, 0])
        t[0 if isin else 1] += 1
        t = vport.setdefault(vp, [0, 0])
        t[0 if isin else 1] += 1
        if isin:
            detail_in.append((a.eventId, getattr(a, "numIndices", 0),
                              rtv_v, rtv_r, dv, dr, dflags, vp))
        elif n % 97 == 0:
            detail_out_samples.append((a.eventId, getattr(a, "numIndices", 0),
                                       rtv_v, rtv_r, dv, dr, dflags, vp))

    say("")
    say("-- every distinct (RTV views, DSV view) combination --")
    for (rtv_v, dv), (ins, outs_e) in sorted(vcombos.items(),
                                             key=lambda kv: -len(kv[1][0])):
        mark = ""
        if ins and not outs_e:
            mark = "   <<<< EXCLUSIVE TO THE VIEWMODEL RANGE"
        elif ins:
            mark = "   (shared)"
        say("")
        say("   in=%-5d out=%-5d%s" % (len(ins), len(outs_e), mark))
        say("        rtv views %s" % (list(rtv_v) if rtv_v else "(none)"))
        say("        dsv view  %s" % dv)
        if ins:
            say("        in eids   %s%s" % (ins[:14],
                                            " ..." if len(ins) > 14 else ""))
        if outs_e:
            say("        out eids  %s%s" % (outs_e[:14],
                                            " ..." if len(outs_e) > 14 else ""))

    say("")
    say("-- each RTV VIEW on its own --")
    for v, (i, o) in sorted(vrtv.items(), key=lambda kv: -kv[1][0]):
        say("   in=%-5d out=%-5d  view %s%s"
            % (i, o, v, "   <<<< IN ONLY" if i and not o else ""))
    say("")
    say("-- each DSV VIEW on its own --")
    for v, (i, o) in sorted(vdsv.items(), key=lambda kv: -kv[1][0]):
        say("   in=%-5d out=%-5d  view %s%s"
            % (i, o, v, "   <<<< IN ONLY" if i and not o else ""))
    say("")
    say("-- viewport, in case the viewmodel gets its own --")
    for v, (i, o) in sorted(vport.items(), key=lambda kv: -kv[1][0]):
        say("   in=%-5d out=%-5d  %s" % (i, o, v))

    say("")
    say("-- every viewmodel draw in full --")
    for eid, ni, rtv_v, rtv_r, dv, dr, dflags, vp in detail_in:
        say("   eid %-6d %7d idx  vp %s" % (eid, ni, vp))
        for i, (v, r) in enumerate(zip(rtv_v, rtv_r)):
            say("        rtv%d view %s  res %s" % (i, v, texname_str(r, textures,
                                                                    resnames)))
        say("        dsv  view %s  res %s%s"
            % (dv, texname_str(dr, textures, resnames), dflags))

    say("")
    say("-- sampled world draws for contrast --")
    for eid, ni, rtv_v, rtv_r, dv, dr, dflags, vp in detail_out_samples:
        say("   eid %-6d %7d idx  vp %s  rtvs %s  dsv %s"
            % (eid, ni, vp, list(rtv_v), dv))

    say("")
    say("DONE")
    controller.Shutdown()
    cap.Shutdown()


def texname_str(rid_str, textures, resnames):
    for rid, t in textures.items():
        if str(rid) == rid_str:
            nm = resnames.get(rid, "")
            return "%s %dx%d %s%s" % (rid_str, t.width, t.height,
                                      t.format.Name(),
                                      (" '%s'" % nm) if nm else "")
    return rid_str


try:
    main()
except Exception:
    say("EXCEPTION:")
    say(traceback.format_exc())

out.close()
os._exit(0)
