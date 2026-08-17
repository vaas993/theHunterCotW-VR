"""REFUTATION PASS: is "index-buffer resource identity" a real discriminator?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_ib_refute.py

A previous pass claimed the 9 viewmodel meshes each own a private index buffer,
bound by exactly 3 draws each, none of which the world touches -> 27/27 hits,
0/2187 false positives.  This re-measures that from scratch and then asks the
question the previous pass did not:

  Is "owns a private, whole-buffer, 3-times-bound, 16-bit index buffer" a
  property of the VIEWMODEL, or is it just how this engine stores every mesh?

If world meshes have the identical structural profile, then the index buffer
carries no intrinsic viewmodel signal at all: the rule is a memorised pointer
list whose 0 FP / 0 FN score is a tautology of how the list was built, and the
real accuracy lives entirely in whatever oracle populates the list.

Everything is measured over ALL drawcalls, never a sample.  Nothing is
swallowed: every per-draw failure is counted and printed.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ib_refute.txt
        %LOCALAPPDATA%\theHunterCotWVR\rdc_ib_refute.csv
"""
import os
import time
import traceback
from collections import defaultdict

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ib_refute.txt")
CSV = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ib_refute.csv")

VM_LO, VM_HI = 33313, 33956

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


def main():
    t0 = time.time()
    cap = rd.OpenCaptureFile()
    say("OpenFile -> %s" % cap.OpenFile(CAP, "", None))
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open (%.1fs)" % (time.time() - t0))

    # ---- buffer sizes, one cheap call --------------------------------------
    blen = {}
    try:
        for b in controller.GetBuffers():
            blen[rid(b.resourceId)] = int(b.length)
        say("buffer table: %d buffers" % len(blen))
    except Exception as e:
        say("GetBuffers FAILED %s: %s" % (type(e).__name__, e))

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
    say("%d leaf actions, %d drawcalls, eids %d..%d"
        % (len(leaves), len(draws), draws[0].eventId, draws[-1].eventId))

    csv = open(CSV, "w")
    csv.write("eid,inpass,api,n,inst,topo,layout,ib,ibstride,iboff,idxoff,"
              "basev,psnull,nvb,vb0,strides,vbids,iblen\n")

    recs = []
    errors = 0
    t1 = time.time()
    for i, a in enumerate(draws):
        eid = a.eventId
        try:
            controller.SetFrameEvent(eid, False)
            pipe = controller.GetPipelineState()
            d3d = controller.GetD3D11PipelineState()
            ia = d3d.inputAssembly

            layout = rid(getattr(ia, "resourceId", None))
            topo = str(getattr(ia, "topology", "?"))

            ibid, ibstride, iboff = "ERR", -1, -1
            ib = ia.indexBuffer
            ibid = rid(getattr(ib, "resourceId", None))
            ibstride = int(getattr(ib, "byteStride", 0))
            iboff = int(getattr(ib, "byteOffset", 0))

            vbids, strides, offs = [], [], []
            for vb in ia.vertexBuffers:
                r = rid(getattr(vb, "resourceId", None))
                if r == "0":
                    continue
                vbids.append(r)
                strides.append(int(getattr(vb, "byteStride", 0)))
                offs.append(int(getattr(vb, "byteOffset", 0)))

            # null pixel shader == depth-only round
            psnull = 1
            try:
                psid = pipe.GetShader(rd.ShaderStage.Pixel)
                psnull = 1 if psid == rd.ResourceId.Null() else 0
            except Exception as e:
                psnull = -1

            name = a.GetName(sf)
            api = name.split("(")[0] if name else "?"
            rec = dict(eid=eid, inpass=(VM_LO <= eid <= VM_HI), api=api,
                       n=int(getattr(a, "numIndices", 0)),
                       inst=int(getattr(a, "numInstances", 0)),
                       topo=topo, layout=layout, ib=ibid, ibstride=ibstride,
                       iboff=iboff,
                       idxoff=int(getattr(a, "indexOffset", 0)),
                       basev=int(getattr(a, "baseVertex", 0)),
                       psnull=psnull, vbids=vbids, strides=strides, offs=offs,
                       iblen=blen.get(ibid, -1))
            recs.append(rec)
            csv.write("%d,%d,%s,%d,%d,%s,%s,%s,%d,%d,%d,%d,%d,%d,%s,%s,%s,%d\n"
                      % (eid, 1 if rec["inpass"] else 0, api, rec["n"],
                         rec["inst"], topo, layout, ibid, ibstride, iboff,
                         rec["idxoff"], rec["basev"], psnull, len(vbids),
                         vbids[0] if vbids else "-",
                         "+".join(str(s) for s in strides),
                         "+".join(vbids), rec["iblen"]))
        except Exception as e:
            errors += 1
            if errors <= 12:
                say("  eid %d FAILED %s: %s" % (eid, type(e).__name__, e))
        if i % 300 == 0:
            say("  ...%d/%d (%.1fs)" % (i, len(draws), time.time() - t1))
            csv.flush()
    csv.close()
    say("swept %d draws, %d errors, %.1fs"
        % (len(recs), errors, time.time() - t1))

    inp = [r for r in recs if r["inpass"]]
    outp = [r for r in recs if not r["inpass"]]
    say("")
    say("POPULATION: %d drawcalls, %d in %d..%d, %d outside"
        % (len(recs), len(inp), VM_LO, VM_HI, len(outp)))
    api_in = defaultdict(int)
    api_out = defaultdict(int)
    for r in inp:
        api_in[r["api"]] += 1
    for r in outp:
        api_out[r["api"]] += 1
    say("  entry points inside : %s" % dict(api_in))
    say("  entry points outside: %s" % dict(api_out))
    DI = "ID3D11DeviceContext::DrawIndexed"
    say("  plain DrawIndexed outside the pass: %d" % api_out.get(DI, 0))

    # ---------------- TEST 1: re-count the claim over EVERY draw -----------
    say("")
    say("=" * 74)
    say("TEST 1 - re-count the IB rule over every non-viewmodel drawcall")
    S = sorted(set(r["ib"] for r in inp))
    say("  index buffers bound inside the pass: %d -> %s" % (len(S), S))
    hits = [r for r in inp if r["ib"] in S]
    fps = [r for r in outp if r["ib"] in S]
    say("  in-pass draws matched      : %d/%d" % (len(hits), len(inp)))
    say("  OUTSIDE draws matched (FP) : %d/%d" % (len(fps), len(outp)))
    for r in fps[:40]:
        say("      FP eid %d %s n=%d ib=%s iboff=%d" %
            (r["eid"], r["api"], r["n"], r["ib"], r["iboff"]))
    fp_di = [r for r in fps if r["api"] == DI]
    say("  of those, plain DrawIndexed (what the hook sees): %d" % len(fp_di))

    binds = defaultdict(list)
    for r in recs:
        binds[r["ib"]].append(r)
    say("")
    say("  frame-wide bind count of each viewmodel IB:")
    for s in S:
        rs = binds[s]
        ns = sorted(set(x["n"] for x in rs))
        say("      IB %-7s bound by %d draw(s), %d in-pass, counts=%s, len=%d"
            % (s, len(rs), sum(1 for x in rs if x["inpass"]), ns,
               blen.get(s, -1)))

    # ---------------- TEST 2: the depth-only rounds ------------------------
    say("")
    say("=" * 74)
    say("TEST 2 - do all three rounds of each mesh match identically?")
    permesh = defaultdict(list)
    for r in inp:
        permesh[r["ib"]].append(r)
    allgood = True
    for s in sorted(permesh, key=lambda k: -len(permesh[k])):
        rs = sorted(permesh[s], key=lambda x: x["eid"])
        nn = sorted(set(x["n"] for x in rs))
        pn = [x["psnull"] for x in rs]
        say("      IB %-7s n=%s rounds=%d psnull=%s eids=%s"
            % (s, nn, len(rs), pn, [x["eid"] for x in rs]))
        if len(rs) != 3:
            allgood = False
    say("  every mesh matched in all its rounds: %s" % allgood)
    say("  depth-only (null PS) in-pass draws: %d ; colour: %d ; unknown: %d"
        % (sum(1 for r in inp if r["psnull"] == 1),
           sum(1 for r in inp if r["psnull"] == 0),
           sum(1 for r in inp if r["psnull"] == -1)))

    # ---------------- TEST 3: is "private IB" a viewmodel property? --------
    say("")
    say("=" * 74)
    say("TEST 3 - structural twins: how many WORLD index buffers look exactly")
    say("         like a viewmodel index buffer?")

    def profile(rs):
        """structural profile of one index buffer's usage, viewmodel-blind"""
        n = sorted(set(x["n"] for x in rs))
        return dict(
            nbinds=len(rs),
            one_mesh=(len(n) == 1),
            n=n[0] if len(n) == 1 else -1,
            all_plain=all(x["api"] == DI for x in rs),
            all_16=all(x["ibstride"] == 2 for x in rs),
            all_tri=all("TriangleList" in x["topo"] for x in rs),
            all_zero=all(x["idxoff"] == 0 and x["basev"] == 0 and
                         x["iboff"] == 0 for x in rs),
            whole=(len(n) == 1 and blen.get(rs[0]["ib"], -1) == n[0] * 2),
        )

    vm_ibs = set(S)
    prof = {k: profile(v) for k, v in binds.items() if k not in ("0", "ERR")}
    vmprof = {k: prof[k] for k in vm_ibs if k in prof}
    say("  viewmodel IB profiles:")
    for k in sorted(vmprof):
        say("      %-7s %s" % (k, vmprof[k]))

    world_ibs = [k for k in prof if k not in vm_ibs]
    say("  total distinct index buffers bound in the frame: %d "
        "(%d viewmodel, %d world)" % (len(prof), len(vmprof), len(world_ibs)))

    def count(pred, label):
        w = [k for k in world_ibs if pred(prof[k])]
        v = [k for k in vmprof if pred(vmprof[k])]
        say("      %-58s world %4d/%4d   viewmodel %d/%d"
            % (label, len(w), len(world_ibs), len(v), len(vmprof)))
        return w

    say("  how common is each 'viewmodel-like' property among WORLD buffers?")
    count(lambda p: p["one_mesh"], "one mesh only (all binds same index count)")
    count(lambda p: p["whole"], "whole-buffer draw (IndexCount*2 == ByteWidth)")
    count(lambda p: p["nbinds"] == 3, "bound exactly 3 times in the frame")
    count(lambda p: p["all_plain"], "only ever plain DrawIndexed")
    count(lambda p: p["all_16"], "16-bit indices")
    count(lambda p: p["all_tri"], "TriangleList")
    count(lambda p: p["all_zero"], "idxoff/basev/iboff all zero")
    twins = count(lambda p: (p["one_mesh"] and p["whole"] and p["nbinds"] == 3
                             and p["all_plain"] and p["all_16"]
                             and p["all_tri"] and p["all_zero"]),
                  "ALL OF THE ABOVE (full viewmodel structural profile)")
    say("  -> world index buffers structurally INDISTINGUISHABLE from the "
        "viewmodel's: %d" % len(twins))
    if twins:
        say("     examples (ib, indexCount, eids):")
        for k in twins[:25]:
            rs = binds[k]
            say("        ib %-7s n=%-6d eids %s"
                % (k, rs[0]["n"], [x["eid"] for x in rs]))
    say("  draws living on those twin buffers: %d"
        % sum(len(binds[k]) for k in twins))

    # ---------------- TEST 4: what happens if the set goes stale -----------
    say("")
    say("=" * 74)
    say("TEST 4 - sensitivity of the rule to the learned set being wrong")
    say("  Recall as a function of how many of the %d meshes are in the set:"
        % len(S))
    for miss in range(0, len(S) + 1):
        kept = S[miss:]
        h = sum(1 for r in inp if r["ib"] in kept)
        say("      %d mesh(es) missing -> %d/%d in-pass draws matched (%.0f%%)"
            % (miss, h, len(inp), 100.0 * h / len(inp)))
    say("  NOTE: one missing mesh = 3 unmatched draws = that mesh renders at")
    say("        world depth while the rest of the gun does not.")

    # ---------------- TEST 5: boundary / other first-person geometry -------
    say("")
    say("=" * 74)
    say("TEST 5 - the neighbourhood of the pass (is 33313..33956 the whole")
    say("         first-person geometry, or only part of it?)")
    near = [r for r in recs if 32900 <= r["eid"] <= 34400]
    for r in near:
        say("      %s eid %-6d %-42s n=%-6d inst=%-4d ib=%-7s lay=%-6s "
            "psnull=%d str=%s"
            % ("VM " if r["inpass"] else "   ", r["eid"], r["api"], r["n"],
               r["inst"], r["ib"], r["layout"], r["psnull"],
               "+".join(str(x) for x in r["strides"])))

    # ---------------- TEST 6: pointer-free alternatives --------------------
    say("")
    say("=" * 74)
    say("TEST 6 - anything in the IA that needs NO learned pointer?")
    di_out = [r for r in outp if r["api"] == DI]

    def rule(name, pred):
        h = sum(1 for r in inp if pred(r))
        f_all = sum(1 for r in outp if pred(r))
        f_di = sum(1 for r in di_out if pred(r))
        say("      %-52s %2d/%d hits, FP %4d/%d all, %4d/%d DrawIndexed"
            % (name, h, len(inp), f_all, len(outp), f_di, len(di_out)))

    rule("whole-buffer IB (n*2 == ByteWidth)",
         lambda r: r["iblen"] == r["n"] * 2)
    rule("16-bit + TriangleList + idxoff/basev 0",
         lambda r: r["ibstride"] == 2 and "TriangleList" in r["topo"]
         and r["idxoff"] == 0 and r["basev"] == 0)
    rule("<=2 VBs and VB0 stride in {8,16,28}",
         lambda r: len(r["vbids"]) <= 2 and r["strides"]
         and r["strides"][0] in (8, 16, 28))
    rule("all three combined",
         lambda r: r["iblen"] == r["n"] * 2 and r["ibstride"] == 2
         and "TriangleList" in r["topo"] and r["idxoff"] == 0
         and r["basev"] == 0 and len(r["vbids"]) <= 2 and r["strides"]
         and r["strides"][0] in (8, 16, 28))
    lays = sorted(set(r["layout"] for r in inp))
    rule("input layout in the pass's set %s" % lays,
         lambda r: r["layout"] in lays)
    vb0s = set(r["vbids"][0] for r in inp if r["vbids"])
    rule("VB0 in the pass's set (%d ids)" % len(vb0s),
         lambda r: r["vbids"] and r["vbids"][0] in vb0s)

    say("")
    say("DONE total %.1fs" % (time.time() - t0))
    controller.Shutdown()
    cap.Shutdown()


try:
    main()
except Exception:
    say("EXCEPTION:")
    say(traceback.format_exc())

out.close()
os._exit(0)
