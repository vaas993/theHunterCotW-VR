"""Probe the RenderDoc capture for the weapon's shaders - ground truth pass 1.

Run headlessly:
    qrenderdoc.exe --python tools\rdc_weapon_probe.py

Answers, in one run:
  1. Does the capture contain shaders whose bytecode CRC-32 matches the three
     weapon pixel shaders found with Shader Toggler (0xFBECF3C0, 0xF616CE07,
     0x7A86206F)?  Same CRC ReShade computes, so a hit is THE same shader.
  2. How many events actually use those shaders (GetUsage), and where in the
     frame do they sit?

Learned from run 1: the frame is FLAT - no marker regions at all - with 2,162
drawcalls; and SDFile.buffers is empty in this build, so bytecode comes from
ShaderReflection.rawBytes instead of creation chunks.

Output goes to a FILE (qrenderdoc stdout does not survive detached launch).
"""
import os
import sys
import traceback
import zlib

import renderdoc as rd

CAP = os.path.expandvars(
    r"%LOCALAPPDATA%\theHunterCotWVR\captures\cotw_frame4317.rdc")
OUT = os.path.expandvars(r"%LOCALAPPDATA%\theHunterCotWVR\rdc_weapon_probe.txt")

WEAPON_CRCS = (0xFBECF3C0, 0xF616CE07, 0x7A86206F)

out = open(OUT, "w")


def say(s):
    out.write(s + "\n")
    out.flush()


def main():
    say("renderdoc API: %s" % rd.GetVersionString())
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(CAP, "", None)
    say("OpenFile -> %s" % res)
    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        say("could not start replay: %s" % res)
        return
    say("replay open")

    sf = controller.GetStructuredFile()
    actions = controller.GetRootActions()

    # Flat action list with eventIds so usage hits can be located by name.
    leaves = []

    def walk(lst):
        for a in lst:
            kids = getattr(a, "children", [])
            if kids:
                walk(kids)
            else:
                leaves.append(a)

    walk(actions)
    by_eid = {}
    for a in leaves:
        by_eid[a.eventId] = a
    say("%d leaf actions" % len(leaves))

    # ---- enumerate all shader resources, CRC their bytecode -------------
    resources = controller.GetResources()
    say("%d resources total" % len(resources))

    shader_crc = {}        # ResourceId -> (crc, stage, size)
    n_fail = 0
    stage_counts = {}
    for r in resources:
        if r.type != rd.ResourceType.Shader:
            continue
        try:
            entries = controller.GetShaderEntryPoints(r.resourceId)
        except Exception:
            n_fail += 1
            continue
        if not entries:
            n_fail += 1
            continue
        try:
            refl = controller.GetShader(rd.ResourceId.Null(), r.resourceId,
                                        entries[0])
        except Exception:
            n_fail += 1
            continue
        raw = bytes(refl.rawBytes)
        if not raw:
            n_fail += 1
            continue
        crc = zlib.crc32(raw) & 0xFFFFFFFF
        stage = str(refl.stage)
        shader_crc[r.resourceId] = (crc, stage, len(raw))
        stage_counts[stage] = stage_counts.get(stage, 0) + 1

    say("reflected %d shaders (%d failed): %s"
        % (len(shader_crc), n_fail, stage_counts))

    # ---- weapon hits -----------------------------------------------------
    say("")
    say("== weapon CRC hits ==")
    hits = []
    for rid, (crc, stage, size) in shader_crc.items():
        if crc in WEAPON_CRCS:
            hits.append((crc, rid, stage, size))
            say("  crc %08X  rid %s  %s  %d bytes" % (crc, rid, stage, size))
    if not hits:
        say("  NONE - the three Shader Toggler CRCs are not in this capture.")

    # ---- usage -----------------------------------------------------------
    say("")
    say("== GetUsage on weapon shader objects ==")
    weapon_eids = set()
    for crc, rid, stage, size in sorted(hits):
        try:
            usage = controller.GetUsage(rid)
        except Exception as e:
            say("  rid %s: GetUsage failed: %s" % (rid, e))
            continue
        eids = [u.eventId for u in usage]
        kinds = sorted({str(u.usage) for u in usage})
        say("  crc %08X rid %s: %d usage events, kinds=%s"
            % (crc, rid, len(eids), kinds))
        if eids:
            say("     eids: %s%s" % (eids[:40], " ..." if len(eids) > 40 else ""))
        weapon_eids.update(eids)

    # Name the hit events so we can see what they are.
    say("")
    say("== the hit events, named ==")
    for eid in sorted(weapon_eids):
        a = by_eid.get(eid)
        say("  eid %-6d %s" % (eid, a.GetName(sf) if a else "(not a leaf action)"))

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
