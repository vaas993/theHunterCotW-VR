"""Is index count alone enough to identify the weapon?

Run headlessly:
    qrenderdoc.exe --python tools\rdc_idx_unique.py

The viewmodel draws in this capture carry exactly the index counts recorded from
the August 2 capture - stable across two sessions.  If those counts are also
UNIQUE within the frame, the mod can match the weapon on index count alone and
never needs a shader fingerprint, which sidesteps the whole
"only 101 of 2,039 shaders are ever fingerprinted" problem.

So: count how many draws in the frame use each of the ten, and where they sit.
A count that only ever appears inside the late viewmodel block is safe to match
on.  One that also appears at event 900 in the middle of the world pass is not,
and needs a second condition (the mod already has one - the pass runs after
post, so a frame-position gate costs nothing).

Output: %LOCALAPPDATA%\theHunterCotWVR\rdc_idx_unique.txt
"""
import os
import traceback

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_idx_unique.txt")

# cbscan.cpp's ground truth, from the August 2 capture.
WEAPON_COUNTS = [77484, 5007, 162, 6087, 3252, 2484, 4992, 5748, 5958, 9594]

# The viewmodel block in THIS capture, from rdc_shader_sweep.txt.
BLOCK_LO, BLOCK_HI = 33300, 34000

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
    say("%d drawcalls in the frame" % len(draws))

    want = set(WEAPON_COUNTS)
    by_count = {}
    for a in draws:
        n = getattr(a, "numIndices", 0)
        if n in want:
            by_count.setdefault(n, []).append(a.eventId)

    say("")
    say("== each ground-truth count, and every draw in the frame using it ==")
    safe, unsafe, missing = [], [], []
    for n in WEAPON_COUNTS:
        eids = sorted(by_count.get(n, []))
        if not eids:
            missing.append(n)
            say("  %-6d : NOT PRESENT in this capture" % n)
            continue
        outside = [e for e in eids if not (BLOCK_LO <= e <= BLOCK_HI)]
        inside = [e for e in eids if BLOCK_LO <= e <= BLOCK_HI]
        verdict = "SAFE - only in the viewmodel block" if not outside else \
                  "COLLIDES with %d draw(s) elsewhere" % len(outside)
        (safe if not outside else unsafe).append(n)
        say("  %-6d : %2d draw(s)  in-block %s  outside %s   %s"
            % (n, len(eids), inside[:6],
               outside[:8] if outside else "-", verdict))

    say("")
    say("== verdict ==")
    say("  safe counts     : %s" % safe)
    say("  colliding counts: %s" % unsafe)
    say("  absent counts   : %s" % missing)
    if unsafe:
        say("  -> index count alone is NOT sufficient; pair it with a "
            "frame-position gate (the pass runs after post) or with the "
            "render target.")
    else:
        say("  -> index count alone IS sufficient in this frame. The mod can "
            "seed its table with these and drop the shader dependency.")

    # What else lives in the block, so nothing in the pass is missed.
    say("")
    say("== every draw inside the viewmodel block ==")
    for a in draws:
        if BLOCK_LO <= a.eventId <= BLOCK_HI:
            n = getattr(a, "numIndices", 0)
            say("  eid %-6d %7d indices %s"
                % (a.eventId, n, "<-- ground truth" if n in want else ""))

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
