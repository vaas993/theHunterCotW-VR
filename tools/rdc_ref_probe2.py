"""Probe 2: RenderDoc 1.45 exposes D3D11 context state as one flat descriptor
store (D3D11State.descriptorStore / descriptorCount / descriptorByteSize).
Find the descriptor index that corresponds to VERTEX STAGE constant-buffer
SLOT b1 - the thing VSGetConstantBuffers1(1,1,...) actually returns, including
stale bindings the current shader never declared.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_ref_probe2.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_ref_probe2.txt")

EIDS = [33313, 33851, 34005, 34061]

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
    say("replay open")

    controller.SetFrameEvent(EIDS[0], False)
    st = controller.GetD3D11PipelineState()
    say("descriptorStore=%s count=%s byteSize=%s"
        % (st.descriptorStore, st.descriptorCount, st.descriptorByteSize))

    say("")
    say("== GetDescriptorLocations over the whole store ==")
    rng = rd.DescriptorRange()
    rng.offset = 0
    rng.count = int(st.descriptorCount)
    try:
        locs = controller.GetDescriptorLocations(st.descriptorStore, [rng])
        say("   %d locations" % len(locs))
        say("   attrs %s" % [a for a in dir(locs[0]) if not a.startswith("_")])
        for i, L in enumerate(locs):
            try:
                cat = str(getattr(L, "category", "?"))
                stg = str(getattr(L, "stage", "?"))
                idx = getattr(L, "index", None)
                fixed = getattr(L, "fixedBindNumber", None)
                logical = getattr(L, "logicalBindName", None)
                if "Vertex" in stg and ("Constant" in cat or "constant" in cat):
                    say("   [%4d] cat=%s stage=%s index=%s fixed=%s logical=%s"
                        % (i, cat, stg, idx, fixed, logical))
            except Exception as e:
                say("   [%4d] ! %s" % (i, e))
    except Exception as e:
        say("   RAISED %s: %s" % (type(e).__name__, e))
        say(traceback.format_exc())

    say("")
    say("== every ConstantBlock location with its stageMask ==")
    try:
        for i, L in enumerate(locs):
            if str(getattr(L, "category", "")).find("ConstantBlock") < 0:
                continue
            say("   [%4d] stageMask=%r fixed=%s logical=%s"
                % (i, getattr(L, "stageMask", None),
                   getattr(L, "fixedBindNumber", None),
                   getattr(L, "logicalBindName", None)))
    except Exception as e:
        say("   ! %s" % e)

    say("")
    say("== per-eid: descriptor at VS constant-block slot 1 ==")
    # find the flat index once (layout is fixed for D3D11)
    vs_cb_idx = {}
    for i, L in enumerate(locs):
        cat = str(getattr(L, "category", ""))
        sm = str(getattr(L, "stageMask", ""))
        if "Vertex" in sm and "ConstantBlock" in cat:
            vs_cb_idx[int(getattr(L, "fixedBindNumber", -1))] = i
    say("   VS constant-block slot -> flat descriptor index: %s"
        % sorted(vs_cb_idx.items())[:16])

    for eid in EIDS:
        controller.SetFrameEvent(eid, False)
        st2 = controller.GetD3D11PipelineState()
        say("  --- eid %d (store=%s count=%s) ---"
            % (eid, st2.descriptorStore, st2.descriptorCount))
        for slot in (0, 1, 2, 3):
            fi = vs_cb_idx.get(slot)
            if fi is None:
                say("     b%d: no flat index" % slot)
                continue
            r = rd.DescriptorRange()
            r.offset = fi
            r.count = 1
            try:
                ds = controller.GetDescriptors(st2.descriptorStore, [r])
                d = ds[0]
                say("     b%d -> resource=%s byteOffset=%s byteSize=%s"
                    % (slot, getattr(d, "resource", None),
                       getattr(d, "byteOffset", None),
                       getattr(d, "byteSize", None)))
            except Exception as e:
                say("     b%d RAISED %s: %s" % (slot, type(e).__name__, e))

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
