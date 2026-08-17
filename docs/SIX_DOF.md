# 6DoF - moving the camera with the head

Built 2026-08-15. Build `509532349706`. **Not yet tested in the headset.**

## Why this is not the same problem as head rotation

The rotation half of head tracking does NOT rewrite matrices. It writes the
engine's own yaw and pitch FIELDS (+0x4C and +0x50 of the lever object), because
this engine recomputes its view matrix from those fields every frame - so a
matrix rewrite is undone before it is used, and every early head-tracking
attempt that "did nothing" was doing exactly that.

**Position has no such field.** The game has no concept of a player leaning: no
crouch-independent head offset, nothing to steer. So position is applied the
only other way - by offsetting the camera's world position directly.

## Where it goes, and why that site is trustworthy

`ApplySixDof()` in `stereo.cpp`, called from `ApplyEyeOffset()` - which is the
world-camera site in `camera_probe.cpp`, the same row of the same matrix the
**stereo eye separation** has been offsetting correctly for weeks. Identical
arithmetic, identical basis, one site:

    m[12..14] += right*dx + up*dy + forward*dz

Two ordering decisions worth recording:

- **6DoF is applied BEFORE the eye separation.** The separation is then measured
  about the head where the head now is, which is what a real pair of eyes does
  and keeps the two effects independent.
- **It is applied in `ApplyEyeOffset` only, never in `ApplyEyeOffsetScaled`.**
  The weapon calls the scaled version with a NEGATIVE scale to cancel the eye
  shift it inherits from the camera. A negative 6DoF would leave the gun
  standing in world space while the player leaned past it. The weapon is
  camera-relative, so it inherits the movement correctly by doing nothing at
  all.

## Axes

The offset arrives from `HeadPositionMetres()` in OpenXR's local space: +x
right, +y up, **-z forward**. The game's basis rows come from the same
`g_viewBasis` the eye separation uses: row 1 is up (settled by the axis test),
`eye_axis_row` names right, and forward is whichever row neither of those
claims - found by search rather than by subtracting from 3, so it stays correct
if `eye_axis_row` is ever 1.

**The signs of right and forward were never settled by measurement**, exactly as
the eye separation's own axis was unsettled until one headset session pinned it.
Each therefore gets a switch rather than a guess.

## A new accessor, because the old one is for a face tracker

`HeadPositionMM()` exists to feed Extended View's Tobii lever: quarter scale,
clamped into a 150x100 mm desk box, z re-based around 600. Feeding that to a
camera would turn a lean into a twitch. `HeadPositionMetres()` returns the real
distance the head really moved, in metres, relative to the last recentre, and
nothing else.

## The limitation that cannot be engineered away from here

**Nothing collides.** The mod moves a camera; the game does not know the camera
moved. Lean far enough and the view goes through a wall and renders the inside
of the world. No amount of work outside the engine fixes that - it would need
the game's own collision, which is not exposed.

`six_dof_limit_m` (default 0.60 m) is the guard: it is a seated lean, not a
room. It is applied AFTER the scale, deliberately - a limit on the raw movement
would let a high scale walk the camera through a wall while the reading still
looked reasonable.

## Testing order

1. `six_dof = 1`, sit still, **recentre** - that sets where "not leaning" is.
2. Lean LEFT while looking at something close (the rifle, a branch). The view
   should move left and the world should shift with parallax. If it moves the
   wrong way: `...swap left / right`.
3. Duck. Wrong way: `...swap up / down`.
4. Lean IN toward something close. Wrong way: `...swap forward / back`. Test
   this one on a NEAR object - at distance a forward lean barely shows either
   way, in any direction.
5. Only once all three are right, judge the feel with `...movement scale`.

## PRIOR ART: how Luke Ross does it (read 2026-08-15)

Read while the first build was being tested. Source: the **GTA V R.E.A.L. mod
README**, which is public on GitHub (the binary is encrypted, the README is not),
plus his Patreon and press coverage. It is the single most relevant piece of
prior art there is, because R.E.A.L. is the AER reference and ships 6DoF in ~42
AAA titles.

**1. He does exactly what we just did - camera-only offset - and ships it.**
The offset is applied at the rendering/camera level and, in his own words,
*"the offset that you apply to the camera when you move your head in real life
does not directly carry over to your character's head, or body."* So the
limitation recorded above is not a shortcut we took; it is the state of the art
for engines that do not expose a way to move the player.

**2. And the exception names the upgrade path.** *"With NOLF2 I was able to
implement full position tracking because the game provided a way to move the
main character model accurately by an arbitrary, precise amount. GTA V does not,
or if it does, I couldn't find out how."* **That is the real 6DoF: move the
PLAYER, and the camera follows with the engine's own collision.** If a
player-position write can be found in Apex the way the yaw field was found, this
feature graduates from a lean to a walk, and walls start working. Until then,
camera-only is correct and it is what everybody else ships.

**3. Position tracking is a toggle, on by default** (NUMPAD 0). Ours is
`six_dof`, default OFF - his mods are shipped configured, ours is still being
proven.

**4. No published scale or limit.** His docs list no position scale, no world
scale, and no travel clamp. Our `six_dof_limit_m` is our own addition. It is
defensible - it is the only thing standing between a room-scale step and the
inside of a rock - but it IS a divergence from the reference, and if leaning
feels short, that limit is the first suspect rather than the scale.

**5. Recentring realigns position, not just direction.** His recentre gesture
exists partly *"to realign the position tracking with the character model"*.
Ours already re-anchors position on recentre (`recentre_position`), which is the
same idea; it is worth keeping that default on now that position matters.

**6. The engine WILL take the camera off you sometimes.** *"there are a few
circumstances when the game will just take control of the camera"* - entering a
vehicle, being thrown, sliding down a slope - and his advice is not to fight it.
He also ships a **"view matrix fix"** (hotkey O) that forces the camera angles
and position *"even when the in-game camera resists being changed"*, i.e. a
second, later correction stage for exactly those moments. If our 6DoF dies
during a cutscene or an animation, that is the shape of the fix, and the site
would be later than `ApplyEyeOffset`.

**7. Cutscenes get their own switches** - stereo mode in cutscenes (normal /
dynamic / flat), pitch mode (absolute / relative / cut-relative), full camera
tracking on/off. Cutscenes are not a corner case to him; they are a whole
sub-feature. Worth remembering before assuming ours will behave.

**8. The HUD is the known casualty, and he answers it directly.** His HUD is
drawn *"about 3 feet in front of your head"* and *"fixed in space"* rather than
head-locked, with a **HUD tracking mode** hotkey (normal / force fixed / force
headlocked / developer), and it becomes *"smaller and headlocked"* while aiming.
Ours is still the game's own screen-space HUD, moved and scaled. Expect the
world to slide under a HUD that does not move; if that reads badly, his answer -
put it in the world, let it stay put - is the one to copy.

**9. One thing he warns about that we should check on ourselves:** in first
person, GTA V's player model *"does not even include a head! Your character's
body ends at the neck with a beheaded stump"*, and leaning far enough shows it.
CotW has a first-person body too - leaning down or back may show the inside of
it.

## FIRST HEADSET RUN, 2026-08-15 - three axes right, the pivot wrong

Owner: *"6 dof working very good so far ... weapon follow me ... up and down
works .. also left and right .. and forward and backwards"* - so all three axes
and all three signs came out right first time, and the weapon inherits the
movement exactly as predicted. Backup of that build:
`2026-08-15_6DOF-WORKING_pivot-issue` (`509532349706`).

**The fault:** *"if i stand and then rotate myself in the real world, in the
game i will rotate, but it seems the player does not rotate on his center ...
i rotate on that center not the player center"*, and *"looking around with game
pad stick cause the rotation to be wrong"*.

**The cause, and it is a frame-of-reference error rather than a sign or a
scale.** The head position arrives in ROOM space, where turning your head does
not change which way "right" points. The camera basis it was being projected
onto is the body's facing turned by the head's own yaw and pitch - the engine
builds it from the very fields this mod writes. So the offset was re-projected
through the head's rotation every time the head turned, and **an offset that
rotates about the camera is an orbit**: the player swings around a point off to
one side instead of turning on the spot. The stick made it worse for the same
reason - it turns the basis too.

**The fix** takes the head's own rotation back out before applying the offset:

    want    R_body . p
    have    R_body . R_headYaw . R_headPitch . X
    so      X = R_headPitch^-1 . R_headYaw^-1 . p

What is left is the body's facing alone, which is the frame a lean is actually
measured in. Turning the head then turns the view without moving the player, and
the stick - which turns the body - carries the lean round with the player, which
is what a real turn does.

The angles come from `HeadYawRadians`/`HeadPitchRadians`, i.e. **the angles the
camera was actually given** (inversion switches, axis test and all), not the raw
pose with the invert flag re-applied here - a second copy of that rule would
eventually disagree with the first. They return false when head tracking is off,
which is precisely when there is no head rotation to remove.

`six_dof_body_frame` (default ON) switches it, so the difference can be felt
directly; `six_dof_body_frame_invert` covers the one sign this argument cannot
settle from the armchair. Build `CFF4380ABBA3`.

## SECOND RUN - "turn on the spot" fixed the pivot, two faults left

Owner: *"turn on my spot works"* - so cancelling the head's yaw was right. Two
faults remained, and they had one cause between them:

- *"moving to right does not move me to the right, the same for left"*
- *"if i move my stick up and then bring my view down with my head then rotate,
  the rotation will be wrong ... i rotated with my body and feels i am going up
  and down with rotation"*

**Cause: the offset was applied along the CAMERA's axes, which are tilted.** A
leaning body does not tilt with where it is looking - lean forward while staring
at your boots and you move horizontally forward; duck and you go straight down
the WORLD's vertical, not the camera's. Applying the lean along a pitched basis
mixes forward into down, which is the up-and-down during turns.

It also explains the sideways fault, which looked unrelated: the old code
cancelled the head's yaw AND pitch and then applied the result along the camera
basis - but that basis still carried **the STICK's pitch**, which no amount of
cancelling head angles can remove. With the stick pitched and the head turned,
the sideways axis the lean landed on was neither horizontal nor sideways.

**The fix: apply the lean in a gravity-aligned frame.**

    up      = world up, +Y (this engine keeps camera height in row3.y)
    right   = the camera's right, vertical part dropped, renormalised
    forward = the camera's forward, vertical part dropped, renormalised

Renormalised deliberately: a camera looking 60 degrees down leaves a forward row
half as long once flattened, and a short axis would make the lean shrink the
further down the player looked. Looking straight up or down leaves forward with
no horizontal part at all, so it is rebuilt as `up x right` - right always
survives, because head roll is not applied to this camera.

That removes ALL pitch from the movement, the stick's included, which the
previous approach could not have reached by tuning.

**And the yaw cancellation now uses the PHYSICAL yaw.** `HeadYawRadians` carries
the mod's inversion switch, which exists because the game's field wanted the
opposite sign - but that sign belongs to the game's convention, not to the room,
and the room is where the head's position lives. `HeadPhysicalYaw()` returns the
pose's own yaw since recentre, no flags. Cancelling with the flipped one would
have cancelled a rotation the room never had.

Pitch is no longer cancelled at all: with the frame horizontal there is nothing
left for it to contaminate. One angle, one rotation, about the vertical.

**Build `A3B8DCE20F61`.**

## RESOLVED - and the quarter turn was never real

Owner, after the dial shipped: **"0 is the correct one"**. So the axis mapping
was right all along, and the "leaning left moves me forward" report came from an
ambiguous instruction rather than an ambiguous axis: **"lean left" can mean
step sideways, tilt the head onto the shoulder, or turn - and only the first is
a position change at all.** Tilting is roll, which 6DoF ignores by design;
turning is yaw, which the body-frame code deliberately cancels. Either of those
tested as "leaning" would produce exactly the confusing answer that was
reported.

**The lesson is about the instruction, not the code.** A test asked of a person
in a headset has to name the motion unambiguously - "move your whole head
sideways to your left, do not turn, do not tilt, do not lean forward at the same
time" - because the tester cannot see which of the three the code is watching.
The panel text and the test steps here now say it that way.

`six_dof_yaw_offset_deg` stays at 0 and stays in the build: the room-versus-chair
misalignment it fixes is real for anyone whose play boundary faces a different
way from their chair, even though it turned out not to be this machine's
problem. It costs nothing at 0 - the rotation block is skipped outright.

**State: 6DoF works.** All three axes, correct signs, correct pivot on the
player, weapon follows, stick and physical turning both behave. Build
`7A4C890CE08D`, plus a text-only rebuild waiting for the game to close.

## THE REAL FAULT, FOUND BY READING F.E.A.R. VR (2026-08-15)

Owner, after the dial: *"leaning left does not ALWAYS make the character lean
left ... moving the stick and playing mess up the direction"*, and: go read
other mods.

**F.E.A.R. VR** (DR-89, open source, OpenXR, `src/common/head_tracking_math.h`)
does the whole conversion in one line:

    centerDelta = Rotate(inverseRecenter,
                         {current.px - recenter.px, ...});

The room offset is rotated by **the inverse of the RECENTRE pose** - and that
recentre is stored yaw-only, for the stated reason that *"a VR recenter must
only choose which horizontal direction is 'forward'. Treating the current pitch
and roll as neutral tilts the room."* Its offsets also drop pitch and roll
because *"looking up must not reduce forward speed, and head roll must never
introduce strafing"* - the horizontal frame this project arrived at
independently two builds ago, confirmed.

**What we were missing was that inverse-recentre rotation.** Our basis is the
CAMERA's, which already carries the head's recentre-RELATIVE yaw, so two
rotations are owed:

    inverse recentre  (a0)  +  head-relative yaw  (a - a0)  =  absolute yaw (a)

They sum to the head's **absolute room yaw**, so it is one rotation by the whole
angle. The last three builds cancelled only `a - a0` and never applied `a0`,
leaving the mapping out by **the direction the player happened to be facing when
they last recentred**:

- recentre square to the room and `a0 = 0` - everything looks perfect;
- recentre at any other angle and every lean lands rotated by a constant;
- recentre again facing differently and the constant changes.

Which is precisely "does not ALWAYS lean left", and why a fixed quarter-turn
dial could not fix it: the error is not a property of the room, it is a property
of the last recentre.

`HeadPhysicalYaw()` now returns the ABSOLUTE room yaw - deliberately not minus
`g_yawRef`, with the reasoning written at both the definition and the header,
since it is the one angle in that file measured differently from all the others.

**Build `645665F65F95`.**

**Also found, for later:** F.E.A.R. VR ships `camera_collision.h`,
`lean_collision.h` and `lean_body.h` - it solves the leaning-through-walls
problem that both we and Luke Ross document as unsolvable from outside. Worth
reading properly before anyone attempts the "move the player, not the camera"
upgrade.

## "TWO OPPOSITE DIRECTIONS" NAMED THE SIGN, 2026-08-15

Owner: *"leaning left works only in two opposite directions, the other two will
make lean left move in the wrong direction ... rotating feels like i am rotating
on a different axis or center"*.

**That symptom is a diagnosis.** R(+a) and R(-a) agree in exactly two places,
0 and 180 degrees apart, and disagree everywhere else. A lean that is right when
facing two opposite ways and wrong the other two is a rotation applied the wrong
way round - not a scale, not an axis, not a pivot.

**And the sign was in the angle, not in my arithmetic.** `AnglesFromMatrix`
computes `yaw = atan2f(fx, -fz)`, so a head turned to the player's RIGHT (facing
+X) reports **+90 degrees**. A right-handed rotation about +Y by +90 degrees
faces **-X**. The two conventions are opposite, so "undo the yaw" written with
the textbook matrix DOUBLED it. That trigonometry has now been written wrong
twice in this file, in two different builds.

**So it no longer uses an angle.** `HeadRoomToBody()` (headtrack.cpp) projects
the room vector onto the head matrix's OWN basis vectors - forward is the -Z
column, right is `forward x up`, both flattened to the horizontal first - and a
projection onto real basis vectors cannot pick up a sign convention, because
there is no convention left to pick up. It is also the only place that needs to
know the pose's layout, which is where that knowledge belongs.

`six_dof_body_frame_invert` is repurposed as a MIRROR (negates the right
component) rather than a negated angle: a reflection is what produces the
two-right-two-wrong symptom, so if it ever returns, that is the switch - but it
should not, now that no angle is involved.

**Build `58EDCCB59C04`.**

## THE BASIS FOLLOWS THE STICK, NOT THE HEAD - measured 2026-08-15

Owner: *"stepping left with stick rotation works with all directions ... but if
i rotate only my head to right (with no stick) and then step left my character
will go forward"*.

**That pair of facts identifies the frame.** Stick turns correct in all four
directions means the body half of the mapping is right. A head turn on its own
breaking it, by exactly the head's own yaw, means the basis the offset is
applied along **does not turn with the head** - so cancelling the head's current
yaw put that yaw back into the answer instead of taking it out.

It fits the site: `NoteViewBasis(m)` captures the lever matrix BEFORE this mod
touches it, and `head_rotate_lever` is off in the owner's config. Whatever the
engine does downstream to build the view from its yaw field, the basis this file
captured is the body's.

**So the rule is:**

    a frame that turns with the HEAD  -> cancel the head's CURRENT facing
    a frame that turns with the BODY  -> cancel the facing at the RECENTRE

Ours is the second. `HeadRoomToRecentre()` projects the room offset onto the
facing the player had at the last recentre - which is exactly F.E.A.R. VR's
`Rotate(inverseRecenter, current - recenter)`, arrived at the long way round,
and the constant between the room's axes and the character's.

The recentre facing is stored as a flattened forward VECTOR, not an angle, for
the same reason the rest of this stopped using angles: two numbers, no
trigonometry, no sign convention to get backwards.

**And it had to be stored at BOTH recentre sites.** The first patch reached only
one of the two `AnglesFromMatrix(m, &g_yawRef, ...)` calls - this file's own
version of the one-object-two-paths trap that has now cost this project a bug in
cbscan, in taa, and here.

**Build `A4FB4498FBA4`.**

## AND THE REFERENCE HAD THREE OWNERS - 2026-08-15

Owner: *"this time is a mess ... after centering stepping left moves forward"*.

The model was right; the bookkeeping was not. `HeadRoomToRecentre` kept **its own
copy** of the recentre facing, stamped at the two places that LATCH the
reference with `AnglesFromMatrix(m, &g_yawRef, ...)`. It missed a third:

    ApplyPendingCommit():   g_yawRef = WrapPi(g_yawRef + ry);

The reference is also **ADVANCED incrementally**, one frame after a recentre,
when the offset is credited to the engine's own accumulator - which is the
normal path with `recentre_compensate` on. So the copy went stale exactly when
the player recentred, the transform fell back to the room's raw axes, and every
lean came out rotated by the true reference angle. Recentring made it WORSE,
which is the tell: a copy that is only wrong after the event it was meant to
track is a copy that missed one of its writers.

**Fixed by deleting the copy.** The facing is now DERIVED from `g_yawRef` at the
point of use, in the same convention `AnglesFromMatrix` defines it:

    yaw = atan2(fx, -fz)   =>   fx = sin(yaw),  fz = -cos(yaw)

Self-consistent by construction: the sign cannot disagree with the angle it came
from, which is what every hand-written rotation in this feature has managed to
do at least once. Three sites maintain that reference; a fourth copy of it was
never going to hold.

**Build `2A760B405353`.**

**The through-line of this whole feature, worth reading before touching it
again:** every single fault has been a frame-of-reference or convention error,
never a formula. Room versus body versus camera; the recentre's facing versus
the head's current facing; an angle whose sign convention is not the textbook
one; and finally a reference with three owners and a private copy. The parts
that were derived from real basis vectors, or from the same expression that
defines the convention, have all been right first time.

## SETTLED: MODE 1, ALL DIRECTIONS (2026-08-15)

Owner: *"mode 1 works in all directions"*. So the basis this offset is applied
along **does turn with the head**, and the facing to cancel is the head's
CURRENT one. `six_dof_frame_mode = 1`, build `6543E72F7F15`, backup
`2026-08-15_6DOF-WORKING_mode1-all-directions`.

The other three modes stay in the build. Which one is right is a property of
engine behaviour that cannot be read from outside, so it is a measured setting,
not a constant - and a different Apex title, or a different config of this one
(`head_rotate_lever`, `stereo_basis_from_lever`), could land on another.

## THE REMAINING LIMITATION, FOUND BY THE OWNER FROM THE FOLIAGE

Owner: *"if i move in the real world it seems my player location in the game
stays in its place - i know that because when the player moves it moves the
foliage under it ... if i rotate with the stick, the rotation will be anchored
to that location that stayed"*.

**Correct, and it is the documented limitation arriving in two visible forms.**
The mod moves the CAMERA; the character entity never moves. So:

- **the foliage does not part under a lean** - grass responds to the PLAYER's
  position, and that has not changed. A better tell than anything in the log,
  because it is the game's own opinion of where the player is;
- **a stick turn pivots on the character, not on the head.** The lean offset is
  expressed in the character's frame, so turning the character sweeps the camera
  around it on an arc of the lean's own length. Physically that is what happens
  if a body pivots under a leaning head - but VR players expect a stick turn to
  rotate the world about their own head, and it is only the character's failure
  to move that makes the two differ.

Both disappear the moment the PLAYER moves rather than the camera, which is the
upgrade path already recorded here from Luke Ross's NOLF2 exception and F.E.A.R.
VR's `lean_body.h` / `lean_collision.h`.

**A concrete lead, from this project's own cheat tables** (v1.22 table,
`Update_Player_on_Map_Position`): its AOB opens

    F3 0F 10 9B 8C 27 00 00     movss xmm3,[rbx+0x278C]
    F3 0F 10 93 84 27 00 00     movss xmm2,[rbx+0x2784]

so a player object carries coordinates at **+0x2784 and +0x278C**, and the same
table's `playerstop` script freezes player movement, which means it has already
found the movement writer. That is where a "move the player, not the camera"
attempt starts - not from scratch.

**Risks to weigh before starting:** writing a position directly may bypass the
collision it is meant to gain, fight the movement code frame by frame, or trip
whatever the engine does about teleporting. The safer shape is to feed the lean
as a small displacement through the engine's OWN movement path, if one can be
found, rather than writing coordinates.

## Open questions for the first run

- Does the weapon follow the head correctly, or does it swim? It should
  inherit the movement for free, being camera-relative.
- Does the temporal resolve reproject correctly through a positional move?
  `EyeWorldPosition()` reads the camera position AFTER both offsets, so it
  should - but walking already exercised that path and leaning is the same
  thing at a different scale.
- Does the HUD, which is drawn in screen space, react badly to a camera that
  moves without the game knowing? It should not move at all, which may itself
  look wrong when the world does.
