"""Every buffer's size + creation flags, so IA state can be joined to buffer size.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_bufdesc.py

rdc_ia_sweep.py showed the nine viewmodel meshes each own a private index
buffer that no world draw touches.  A learned pointer set is fine, but a rule
that needs no learning would be better: if a viewmodel index buffer is exactly
the size of its mesh (IndexCount*2 == ByteWidth) while world geometry indexes
into big shared pools, the hook can test that with one GetDesc call and no
prior knowledge.  This dumps every buffer so that join can be made offline.

No SetFrameEvent, no per-draw replay - GetBuffers() is a single cheap call.

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_bufdesc.csv
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
CSV = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_bufdesc.csv")
LOG = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_bufdesc.txt")

out = open(LOG, "w")


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

    bufs = controller.GetBuffers()
    say("%d buffers" % len(bufs))
    if len(bufs):
        say("BufferDescription attrs: %s"
            % [n for n in dir(bufs[0]) if not n.startswith("_")])
        b = bufs[0]
        say("sample: %s" % " ".join(
            "%s=%r" % (k, getattr(b, k)) for k in dir(b)
            if not k.startswith("_") and not callable(getattr(b, k))))

    # resource names, where the capture kept them
    names = {}
    for r in controller.GetResources():
        names[str(r.resourceId)] = str(getattr(r, "name", ""))

    csv = open(CSV, "w")
    csv.write("id,length,flags,gpuAddr,name\n")
    for b in bufs:
        rid = str(b.resourceId)
        digits = "".join(ch for ch in rid if ch.isdigit())
        csv.write("%s,%d,%s,%s,%s\n"
                  % (digits, int(b.length), str(b.creationFlags).replace(",", ";"),
                     getattr(b, "gpuAddress", 0),
                     names.get(rid, "").replace(",", ";")))
    csv.close()
    say("wrote %s" % CSV)

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
