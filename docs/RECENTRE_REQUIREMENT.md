# Recentre — the owner's requirement, which overrides the research

Stated 2026-08-09. This file is the authority. If `RECENTRE_DESIGN.md`
disagrees with it, this file wins and that doc is wrong.

## The requirement, in the owner's words

> usually recenter should not bring me to the center of the horizon .. it should
> only center my view whatever where i am looking .. so if i am looking down it's
> should center my view while i am looking down ect .. that how luke ross mods do

So: **pressing recentre must not move the picture.** Whatever the player is
looking at stays where it is, and that direction becomes the new "straight
ahead". Looking down at the rifle and recentring must leave the view looking
down at the rifle — not snap up to the horizon.

## This means YAW-ONLY IS WRONG for this mod

The standard advice, and the framing the research run was given, is that recentre
should reset yaw only. **Do not implement that here.** A yaw-only reset leaves
`g_pitchRef` at its old value, so an existing downward look keeps driving the
game's camera down after the press — the picture moves. That is exactly the
behaviour the owner is ruling out.

## THE CURRENT IMPLEMENTATION IS WRONG — observed in the headset

Owner, 2026-08-09, after the analysis below was written:

> the current behavior is wrong because if i was looking up in the sky and
> pressed center it will bring me down to look straight at the horizon

That is the ground truth and it beats the reading of the code that follows.

**Why the reading below was wrong.** It assumed that zeroing the head angles
means "the mod applies no rotation, so nothing moves". It does not. The mod
WRITES AN ABSOLUTE ANGLE into the game's camera, so a head angle of zero is not
"leave it alone" — it is "point the camera at the game's own zero", which is the
horizon. Recentre therefore drives the view to the horizon by construction, and
the further from level the player was looking, the bigger the jump.

**The correct rule is CONTINUITY, not zero.** At the instant of the press the
OUTPUT must be unchanged. The reference must be chosen so that the angle the mod
applies immediately after the press equals the angle it was applying immediately
before it — not so that the head angle reads zero. From that moment on, head
movement moves the view onward from where it already was.

Mechanically that means recentre must ACCUMULATE an offset rather than latch the
raw pose: remember what the mod was applying, and pick the new reference so that
value is preserved. Zeroing is only correct if the mod's output is a delta added
to whatever the game already has, and it is not.

This applies to every axis, including yaw: the test is "the picture did not
move", and that test does not care which axis the jump was on.

## The reading that was wrong (kept so the mistake is not repeated)

`headtrack.cpp` on recentre stores the live pose as the reference:

    AnglesFromMatrix(m, &g_yawRef, &g_pitchRef, &g_rollRef);

and `HeadAngles()` subtracts it:

    *yaw   = WrapPi(y - g_yawRef);
    *pitch = p - g_pitchRef;
    *roll  = WrapPi(r - g_rollRef);

Immediately after a recentre all three read zero, so the mod applies no rotation
of its own and the game's camera is left exactly as it was. The player's current
gaze has become neutral without the picture moving. That is the requirement.

**So the axis question is settled and needs no change.** Anyone reading the
judge panel's verdicts should treat a "yaw-only" recommendation as answered by
the owner from experience with shipped Luke Ross mods, not as an open question.

## What IS still missing — this is the actual work

1. **The key is hardcoded.** `headtrack.cpp:240` polls `VK_HOME` with
   `GetAsyncKeyState`. There is no `recentre_key`, no ini entry and no panel row,
   although `freelook_key` and `menu_screen_key` are both bindable. Add
   `recentre_key`, defaulting to `VK_HOME` (0x24) so nothing changes for anyone
   who leaves it alone.
2. **No gamepad binding.** Reaching for a keyboard while wearing a headset is the
   whole problem. Needs a pad route that cannot fight the game's own use of that
   button — see `COTWVR_PadPostProcess` in `dllmain.cpp` and `ConsumePad` in
   `overlay.cpp`. A hold-for-N-ms combination is safer than a bare face button.
3. **The aim smoother must be dropped on recentre.** `SteadyAim()` in
   `headtrack.cpp` holds the previous pose in a static. If it is still active
   across a recentre it will blend from the stale pose towards the new one, which
   would slide the view — precisely the "picture moves" failure this requirement
   forbids. Set its `active = false` when the recentre flag is consumed.
4. **Roll is a separate question and is NOT urgent.** Head roll is only applied
   when `head_write_roll` is on, which is off by default, so `g_rollRef` rarely
   matters. Do not let roll complicate the design.

## Test

Look down at the weapon, press recentre. The picture must not move at all. Then
turn the head level: the view should follow from there, with the down-look now
reading as neutral.
