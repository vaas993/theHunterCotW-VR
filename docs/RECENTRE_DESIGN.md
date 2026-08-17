# Recentre — the design

Written 2026-08-09. Companion to `RECENTRE_REQUIREMENT.md`, which is the
authority. Where this document differs from it, the difference is named out loud
in §0.2 with the reasoning, so the owner can overrule it in one line rather than
discovering it in a headset.

Read-only design pass. No source file was edited. §5 is the edit list for a
later run.

---

## 0. Conformance

### 0.1 The rule this design is built to satisfy

From `RECENTRE_REQUIREMENT.md`:

> pressing recentre must not move the picture. Whatever the player is looking at
> stays where it is, and that direction becomes the new "straight ahead".

and

> The correct rule is CONTINUITY, not zero. ... This applies to every axis,
> including yaw: the test is "the picture did not move", and that test does not
> care which axis the jump was on.

Everything below follows from that one sentence. It is not a comfort preference
that gets traded against other comfort preferences; it is a hard constraint, and
it turns out to determine almost every open question by itself.

### 0.2 Where this design differs from the requirement document

Two places. Both obey the rule above; both differ on the *means* the requirement
document proposed for reaching it.

| Requirement doc | This design | Why |
|---|---|---|
| Item 3: *"The aim smoother must be dropped on recentre. ... Set its `active = false` when the recentre flag is consumed."* | **Do not drop it.** Ships as `recentre_reset_smoother`, defaulting to **0**. | Recentring does not modify the head matrix, so there is no stale pose for the smoother to blend from. Resetting it *creates* a jump instead of removing one: the next published pose becomes raw instead of smoothed, and the offset moves by (raw − smoothed) — up to ω·τ, about 6° at τ=60 ms and 100 °/s, up to ~25° at the 250 ms ceiling. That is a picture jump, on exactly the press the rule forbids one on. Full argument in §4. |
| Item 4: *"Head roll ... `head_write_roll` ... is off by default, so `g_rollRef` rarely matters."* | Roll **is** live, and roll is the one axis that must **never** be recentred. | `backups\2026-08-09_TIER1-COMPLETE\cotwvr.ini:122` has `head_write_roll = 1`. The default in `config.h:369` is `false`, but the owner's tuned configuration turned it on, so the roll reference is in the picture every frame. §2.3 shows that roll is also the one axis with nothing to credit a recentre to — so leaving it alone is both the simplest treatment (which is what item 4 asked for) and the only one that passes the "picture did not move" test on that axis. |

Nothing else here contradicts the requirement document.

---

## 1. What is already there

**Recentre already exists and already works on HOME.** This is not a new
feature. Three things are wrong with it, and one of them is not the axis
question everybody has been arguing about.

| Where | What is there today |
|---|---|
| `headtrack.cpp:234` | `RecentreHead()` — sets `g_recentreRequested` with `InterlockedExchange`. Declared at `headtrack.h:62`. **One caller in the entire tree.** |
| `headtrack.cpp:40` | `volatile LONG g_recentreRequested = 1;` — so the first good pose auto-recentres at startup. That behaviour is kept. |
| `headtrack.cpp:238-245` | The trigger. `GetAsyncKeyState(VK_HOME)`, edge-triggered by a local `static bool wasHome`. **Hardcoded.** No config key, no ini entry, no panel row — unlike `freelook_key` (`config.h:1116`) and `menu_screen_key` (`config.h:488`), which are both fully bindable to a key *or* a pad button. |
| `headtrack.cpp:265-277` | The consume block. Latches all three world-frame angles as the reference (`AnglesFromMatrix(m, &g_yawRef, &g_pitchRef, &g_rollRef)`) and re-anchors position (`g_havePosRef = false`). |
| `headtrack.cpp:96-106` | `HeadAngles()` subtracts the three as scalars, `WrapPi` on yaw and roll, none on pitch (correct — `asinf` at `:82` is already bounded to ±π/2). |
| `headtrack.cpp:39`, `:267` | `g_recentre` (the quaternion) is written and **never read anywhere**. Dead since the design moved from quaternion difference to angles. |
| `overlay.cpp:261-263` | The head-tracking help text hardcodes *"Press HOME at any time to set your current head direction as 'straight ahead'"* — it will describe the wrong key the moment the key is bindable. |

The world-frame angle extraction at `headtrack.cpp:19-29` is not in question and
is not touched by anything below. Nothing here proposes going back to
`conj(recentre) * head`.

So the work is: **make the trigger bindable (keyboard and pad), and make the
press stop moving the picture.** In that order of visibility, and the reverse
order of difficulty.

---

## 2. The proper way

### 2.1 Why the current press moves the picture — the exact mechanism

Live configuration (`backups\2026-08-09_TIER1-COMPLETE\cotwvr.ini`):
`head_write_yaw = 1`, `head_write_pitch = 1`, `head_write_roll = 1`,
`head_early_write = 1`, `head_view_basis = 0`, `head_matrix_rotate = 0`. So the
active path is the aim-consumer write, `camera_probe.cpp:1702-1812`:

```cpp
*pYaw   = yawBefore   - yawDelta;     // camera_probe.cpp:1763
*pPitch = pitchBefore - pitchDelta;   // camera_probe.cpp:1775
```

Write it as an equation. Per frame, per axis:

```
V  =  B  -  O          V = what the player sees   (the camera field we leave behind)
                       B = the engine's own base  (yawBefore — mouse, stick, the aim accumulator)
                       O = our head offset        (head − ref, through the invert flags)
```

`B` is the engine's. It does not change because we pressed a key. So **any
discontinuity in `O` is a discontinuity in `V`** — a one-frame teleport of the
view by exactly the size of the outstanding offset.

Today's press latches `ref = head`, so `O` goes from whatever it was straight to
zero, and `V` jumps to `B`. `B` for a standing player is roughly the horizon.
That is precisely the owner's report — *"if i was looking up in the sky and
pressed center it will bring me down to look straight at the horizon"* — and it
is not a tuning problem or an axis problem. It is structural.

### 2.2 The trap: mod-side continuity alone is a no-op

The obvious repair is to carry the offset: keep a persistent `carry` per axis,
so `O = head − ref + carry`, and on a press set `ref ← head` and
`carry ← carry + (head − ref_old)`. `O` is then continuous by construction and
the picture never moves.

It is also completely useless, and this needs stating plainly so the next run
does not build it:

```
after the press:  O(t) = head(t) − head(t0) + carry_old + head(t0) − ref_old
                       = head(t) − ref_old + carry_old
                       = exactly the same function of head(t) as before the press
```

The recentre is a **pure re-parameterisation**. Not just at the press — forever.
The head-to-view mapping is byte-identical to what it was, so nothing has been
recentred at all. This is a general result, not an artefact of the algebra: with
`B` untouched, the only two possibilities are "the picture moves" (today) or
"nothing happens".

### 2.3 Therefore: a recentre must move the game's base, not only our reference

To have both `V` unchanged **and** `O = 0` afterwards, `B` must absorb the
offset:

```
before:   V = B − O
after:    V = B' − 0        with   B' = B − O
```

That means writing `−O` into the engine's own persistent look accumulator at the
moment of the press. Those fields are already located, disassembled and
measured in this project:

```
apex.h:142-149     the player's own look accumulators, on the SAME object the aim
                   consumer receives in rcx:
                        0x00643F22  addss xmm0, xmm8        ; this frame's look delta
                        0x00643F27  movss [rsi+0x1c], xmm0  ; accumulate, wrap to ±pi
                   constexpr uint32_t kAimInputYaw   = 0x1C;
                   constexpr uint32_t kAimInputPitch = 0x20;
```

`FEATURES_AND_FINDINGS.md` §8 measured the engine doing this *to us*, with a
live Cheat Engine watch: on crouch, stand, prone and jump the engine writes our
outstanding head offset into `aimObj+0x1C` / `+0x20`. **The crouch ratchet is an
uncommanded, uncompensated recentre.** What this design does is the same
operation, deliberately, once, with the reference advanced by the same amount so
the picture stays put.

This is also a named, sanctioned pattern in the user's own playbook —
`<your VR reference folder> Patches&Mods\VR APPS\VR documents\BUILDING_A_VR_MOD.md:118`: *"inject a
turn, then subtract exactly what landed from your recentre reference so the
rendered view does not move"*, shipped by BioShock VR. It is not the forbidden
Model C.

Two things fall out for free:

- **The pitch clamp is not a problem.** `B' = B − O = V`, and `V` is what was
  already sitting in the camera's pitch field, so it is by definition inside the
  range that field accepts (`config.h:351`: −1.099…+1.118 rad). We are writing
  back a value the engine has already been holding.
- **The ratchet loses its fuel.** After a compensated recentre the outstanding
  offset is genuinely zero, so the next crouch has nothing to absorb. Recentre
  stops being only a comfort key and becomes the manual reset for the one open
  defect in the mod.

### 2.4 Which axes — and why the judges' disagreement dissolves

Three independent verdicts were taken. One said all three axes, two said yaw
only. **They were all answering a question that only exists in a latching
design.** Every one of those arguments — the tilted horizon, the `Rx(−p₀)`
shear, the vestibular mismatch, the reclined player — is an argument about what
happens when you *snap* an axis to zero. Under compensation nothing snaps, so
none of them bite.

The question that actually decides it is mechanical, not a matter of taste:

> **An axis can be recentred exactly when there is a persistent game-side
> quantity to credit the offset to.**

| Axis | Persistent game-side quantity | Recentred? |
|---|---|---|
| Yaw | `aimObj+0x1C` — measured, disassembled (`apex.h:148`, §8) | **Yes** |
| Pitch | `aimObj+0x20` — measured, disassembled (`apex.h:149`, §8) | **Yes** |
| Roll | **None.** The roll write is a per-frame camera offset at `camera + head_roll_field` (`camera_probe.cpp:1791-1793`) with no accumulator behind it, and `FEATURES_AND_FINDINGS.md` §1 states roll is deliberately outside the aim bookkeeping because *"roll is not part of the player's aim"* | **No** |

So: **the current all-three-axes reset is wrong — but not because it includes
pitch and roll. It is wrong because it latches.** Latching is wrong on yaw too,
and yaw is where the requirement document's own test catches it.

- Judge 1 wins on **pitch**: leaving a standing pitch offset hands the ratchet
  its largest available meal, and the modal press in a hunting game happens with
  the head 20-35° down on the stock. Compensated pitch removes that fuel at zero
  visual cost.
- Judges 2 and 3 win on **roll**: latching roll bakes a permanent cant against
  felt gravity, above the 1-2° subjective-vertical threshold, with nothing in the
  engine to ever absorb it; and pressing again re-samples a different cant, so
  the horizon wanders instead of converging. With `head_write_roll = 1` in the
  owner's ini this is live today.
- Nobody wins the framing. "Yaw-only vs all three" was the wrong axis of
  argument.

**Roll gets one further change.** Roll has a gravity datum — OpenXR's Y really is
up — so a reference for it is not merely unnecessary, it is a corruption of a
real measurement. `g_rollRef` should be **fixed at 0 and never written**,
including at the startup auto-recentre. As it stands, whatever cant the player's
head happened to have on the very first pose of the session is baked into the
horizon for the rest of it, and no press can ever clear it. This is the one
deliberate behaviour change here that is not about the press, and it lives under
the same master switch.

### 2.5 Position

Unchanged. `g_havePosRef = false` (`headtrack.cpp:273`) stays part of the
recentre: 6DOF position is re-anchored so the body ends up back under the head.
Every prior implementation does this — OpenVR resets *"position and yaw"*,
Oculus *"the (x,y,z) positional components"*, FC2's `RecentreHead` stores
`m_recentrePos` explicitly *"so recentring puts the body back under the head in
position as well as in direction"*. Position is scaled by 0.25 and clamped
(`headtrack.cpp:301-313`) so there is no continuity risk worth engineering
around: the visible step is at most a few millimetres of Extended View parallax.

### 2.6 Approach not taken

- **Recreating the LOCAL reference space** with an accumulated
  `poseInReferenceSpace` (the Microsoft OpenXR cookbook recipe, what
  OpenComposite does). Correct, but solves the wrong half: it would move where
  OpenXR thinks forward is, and the mismatch that needs fixing is between the
  head and *the game's* base. It also has a real historical hazard — rotated
  `poseInReferenceSpace` breaking ATW reprojection windows on Quest runtimes
  before v23. `m_space` is created once at `vr.cpp:309-313` and stays that way.
- **A ramp** (ease the offset to zero over ~1 s instead of jumping). Violates the
  rule — the picture still moves, just politely — and the owner has already
  rejected the shape once: *"no difference how the rotation jumped … with the
  servo it is continuous and smooth, with it off it is a snap. Same total
  rotation"* (§8).

---

## 3. The design

### 3.1 The keyboard binding

```
config.h:   int recentre_key = 0x24;      // VK_HOME
```

Default `0x24` means **nothing changes for anyone who does not touch it**: HOME
keeps working exactly as it does now.

Two traps, both already paid for elsewhere in this codebase:

1. **It must be polled through `BindingDown()`** (`overlay.h:150`,
   `overlay.cpp:1363-1367`), not `GetAsyncKeyState`. §5 of
   `FEATURES_AND_FINDINGS.md`: the raw-input hook rewrites swallowed keys to
   VKey `0xFF`, and *"a swallowed key never reaches the async state"* — so the
   current poll at `headtrack.cpp:240` is one hook away from going deaf. Going
   through `BindingDown` also buys **pad buttons for free**, since a binding is
   `0`, a VK code, or `kPadBind | wButtons` (`overlay.h:133-146`).
2. **It must not fire while the panel is capturing a binding.** `Overlay::Tick`
   swallows input with `return;` at `overlay.cpp:1721`, but `HeadTrackTick`
   (`render_hook.cpp:184`) is a separate call that never checks `g_capture`.
   `freelook_key` gets away with it because it is momentary; a recentre is an
   event, so binding the recentre key would fire a recentre at the instant of
   binding. `g_capture` (`overlay.cpp:609`) needs a public accessor.

Edge-triggered, in the shape `menu_screen_key` already uses
(`overlay.cpp:1728-1735`): a local `static bool wasDown`, act on the rising edge.

### 3.2 The gamepad binding

**Reaching for a keyboard in a headset is the whole problem**, so this has to
work. It also has to respect a measured negative result from this very project
(`overlay.cpp:995-998`):

> NO pad button opens the panel — Insert on the keyboard is the only way in.
> **Every pad binding tried so far collided with something the game itself
> uses**, and a panel that steals a button mid-hunt is worse than one that needs
> a keypress to reach.

So a bare face button is out. The design is a **two-button chord, held**, with
the mod suppressing both buttons from the game for as long as the chord is
complete.

```
config.h:   bool recentre_pad_enable      = true;
            int  recentre_pad_chord       = 0x00C0;   // LEFT_THUMB | RIGHT_THUMB
            int  recentre_pad_hold_ms     = 600;
            bool recentre_pad_swallow     = true;
```

**Why both stick clicks, held for 600 ms:**

- In CotW the left stick click and the right stick click are separate, ordinary
  actions (sprint; hold breath / zoom). Pressing *both at once* is not a
  gameplay action, and *holding* both for over half a second is not something
  that happens by accident in play.
- Both are harmless if they do slip through for a poll or two: a moment of
  sprint and a moment of held breath, while standing still, which is what you
  are doing when you press recentre.
- Neither is Start (the panel needs it for the flat-screen route) nor Back.
- It matches the conventional VR gesture family — stick click is the standard
  recentre input across the Quest and SteamVR ecosystems — without single-click
  fragility.
- A hold is its own debounce and its own settling window. Luke Ross's mod, which
  is the owner's own reference for correct recentre behaviour, is triggered by a
  head-shake gesture and then *"will wait for a fraction of a second to allow
  your head position to stabilize"* before recentring. 600 ms of hold is that
  wait, for free.

**It fires once per chord.** Rising edge on "chord has now been held
`recentre_pad_hold_ms`", and it does not re-arm until the chord is **fully
released**. Holding both sticks down for ten seconds gives one recentre, not
sixteen.

**How it cannot fight the game.** `COTWVR_PadPostProcess` (`dllmain.cpp:172-196`)
already edits the pad state in place — it zeroes `sThumbRY` for the pitch lock at
`:193-195`. The same place clears the chord's bits from
`state->Gamepad.wButtons` **while, and only while, every bit of the chord is
down**:

```
if (recentre_pad_enable && recentre_pad_swallow && chord && (b & chord) == chord)
    state->Gamepad.wButtons &= ~WORD(chord);
```

Ordering inside that function matters and is already established: the swallow
goes **after** `g_overlay.ConsumePad(userIndex, state)` at `:185`, for the same
reason the pitch lock does — the panel must still see the pad.

The residue is one poll: if the two clicks land in different polls, the game sees
a single-poll sprint tap (~8 ms) before the chord completes. That is below
noticeability and it is the honest cost of not owning the input stack.

**Where the chord is evaluated.** The hold timer runs once per frame in
`HeadTrackTick`, reading `g_overlay.PadButtons()` (`overlay.h:112`) — the same
route `BindingDown` uses for pad bindings, already OR'd across slots and already
proof against the "second connected pad wipes the real one" bug that
`FEATURES_AND_FINDINGS.md` §6 documents three times over. The *swallow* is
independent of the timer and lives in the pad callback, so it cannot depend on
frame ordering.

**`recentre_pad_chord` is a config value, not a constant, and that is
deliberate** — the same reasoning as `head_roll_field` (`config.h:365-368`):
*"THE FIELD IS A GUESS AND THAT IS WHY IT IS A SETTING."* Which pad buttons CotW
leaves spare has not been measured. If both stick clicks turn out to collide,
the chord moves from the ini or the panel without a rebuild, and `0` switches it
off entirely.

### 3.3 Which axes reset — the summary

| Axis | Reference on a press | Credited to |
|---|---|---|
| Yaw | advanced by exactly the offset that was applied | `aimObj + kAimInputYaw` (0x1C), wrapped to ±π |
| Pitch | advanced by exactly the offset that was applied | `aimObj + kAimInputPitch` (0x20) |
| Roll | **never touched**; `g_rollRef` is fixed at 0 | — |
| Position | re-anchored, as today (`g_havePosRef = false`) | — |

### 3.4 The exact operation

The formulation matters, because the obvious version re-introduces a bug the
input analysis already found.

**Do not re-latch the reference from a fresh pose. Advance it by exactly the
amount credited.**

```
raw offset in use this frame  (what HeadYawRadians returned, before the invert):
    oRaw   = WrapPi(yHead − g_yawRef)
delta actually written to the camera:
    yawDelta = head_invert ? −oRaw : oRaw          (camera_probe.cpp:1708, :1763)

on recentre:
    aimObj+0x1C  =  WrapPi(aimObj+0x1C − yawDelta)     // credit the engine
    g_yawRef     =  WrapPi(g_yawRef + oRaw)            // advance the reference
```

and the same for pitch, without the wrap (pitch is bounded by `asinf`).

Why this and not `AnglesFromMatrix(m, &g_yawRef, …)`:

- **It cancels exactly, whatever the threads are doing.** `HeadTrackTick` runs on
  the render thread (`render_hook.cpp:184`); the aim-consumer hook runs on the
  game thread. Re-latching from a fresh sample means the credit and the
  reference come from two different head poses, and the difference is a jump.
  Advancing by the credited amount is exact by construction.
- **It removes the raw-vs-smoothed mismatch for free.** Today's reference is
  captured from the raw `m` at `headtrack.cpp:272`, while every reader consumes
  the *smoothed* `g_head` (`headtrack.cpp:99`) because `SteadyAim(m)` runs
  afterwards at `:279`. The residual after a press is therefore
  (smoothed − raw), which decays over τ — the "view slides after recentring"
  symptom, invisible when still and worst mid-turn. `oRaw` comes from
  `HeadYawRadians`, i.e. from the smoothed matrix, the same number that was
  written to the camera. Same quantity in, same quantity out, nothing left over.
- It also makes the invert flags a non-issue: the engine is credited in the
  units the engine was written in, the reference is advanced in head units.

**Where it runs, and when it defers.** The credit must equal the offset that is
actually reaching the picture *on that tick*. The apply block at
`camera_probe.cpp:1702` is gated by `latchArmed`, `engineIdle`, `stance`,
`head_tracking` and `CoupledNow()`. So:

| Situation on the tick | What the recentre does |
|---|---|
| The offset was applied at the aim consumer | Credit it, advance the reference, clear the request. This is the normal path. |
| The tick was gated (`latchArmed`, engine idle, stance changing) but the offset is live | **Defer.** Put the request back and try the next tick. Log once a second so a stuck request is not silent. |
| Free-look is held, so the offset goes in at `WriteYawField` (`camera_probe.cpp:1876-1893`) instead | Defer, same as above. The aim object pointer is cached at the aim-consumer hook (which runs every frame regardless of the gates) so the credit can still be applied on the next ungated tick. |
| Nothing is reaching the picture at all (`head_tracking` off, `head_write_yaw`/`head_write_pitch` off, no player camera, main menu) | **Plain latch, no credit.** With no offset in the picture there is nothing to preserve, so latching is already continuous. This is the path the startup auto-recentre (`g_recentreRequested = 1`, `headtrack.cpp:40`) takes, and it is why that still works. |

The "nothing is being applied" test must be a **positive** observation — a
timestamp written by the apply sites, checked for staleness (~250 ms) — not an
inference from the config flags, because the gates are dynamic. Getting this
backwards is the one way the design can produce the exact failure it exists to
prevent.

**Once per press.** The request is consumed with an interlocked
compare-exchange. There are **two camera objects alternating, one per eye pass**
(`camera_probe.cpp:1729-1736`), and a credit applied on both would move the view
by one whole offset in the wrong direction.

### 3.5 The master switch

Per this project's standing rule, one switch restores today's behaviour exactly:

```
config.h:   bool recentre_compensate = true;
```

`0` must restore **all** of it, not most of it: latch all three references from
the raw pose in the current pre-`SteadyAim` position, write nothing to
`aimObj+0x1C/0x20`, latch `g_rollRef` again rather than pinning it to 0, and
leave the smoother alone. The cleanest way to guarantee that is to keep the
existing block at `headtrack.cpp:265-277` **verbatim** as the `else` branch
rather than reconstructing it from flags. The servo failed *"precisely because it
rewired shared paths that were not behind its flag, so 'off' was not off"*
(§9.2).

Two more switches, both small and both there because this project's history says
signs and means are never known until they are tried in a headset:

```
bool recentre_compensate_invert = false;   // if the press doubles the jump
bool recentre_reset_smoother    = false;   // RECENTRE_REQUIREMENT item 3, see §4
```

### 3.6 Optional, and clearly marked as such: the runtime's own recentre

There is **no** `XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING` handler
anywhere in `src\cotwvr` — the event loop at `vr.cpp:~1161-1198` handles session
state and instance loss only. When the player long-presses the Meta button, or
uses SteamVR's "Reset Seated Position", or re-runs guardian setup, LOCAL moves
underneath us, all three references go stale by exactly that shift, and the
picture jumps and *stays* wrong until HOME is pressed. Players do press that
button.

Under this design the fix is one line in the right place: on that event, call
`RecentreHead()`. The compensated recentre then absorbs the runtime's shift
without moving the picture — which is exactly what UEVR does
(`wants_reset_origin`, deferred until `ready() && got_first_valid_poses`). Note
the known runtime inconsistency: Quest sends one event, SteamVR sends several,
and the event *arrives before the new tracking data*, so it must set a flag that
is consumed after the next good pose — which is precisely what
`g_recentreRequested` already is.

Config: `bool recentre_on_runtime_event = true;`. **Out of scope for the first
build** — it is a separate, independently testable change and it should not be
in the same diff as the thing the owner is going to judge in a headset.

---

## 4. Interaction with the aim-steadying smoother

**The smoother must NOT be dropped on recentre. Dropping it is what would move
the picture.**

The state is three function-local statics at `headtrack.cpp:122-125` —
`active`, `prev[9]`, `last` — cleared today only when smoothing is off (`:132`),
on a >250 ms hitch (`:157-160`), and on a degenerate matrix (`:182`, `:187`).
`RecentreHead()` touches none of them.

**What happens if it is not dropped: nothing.** A recentre does not modify `m`.
It changes scalar references (and, under this design, an engine accumulator).
`prev` therefore stays continuous with the live pose, there is no stale pose to
blend from, and the smoother has no way to know a recentre happened. This is
true of today's build and stays true under this design.

**What happens if it IS dropped:** `active = false` makes the very next frame
take the `if (!active)` branch at `headtrack.cpp:135-140`, which copies `m` into
`prev` and **returns without blending** — so the published `g_head` becomes the
**raw** pose for that frame instead of the smoothed one. Every reader's offset
therefore steps by (raw − smoothed) in a single frame. That difference is the
smoother's lag: roughly ω·τ, about **6° at τ=60 ms with a 100 °/s head turn**,
and up to **~25° at the 250 ms ceiling**. It is zero when the player is still and
grows with head speed — so it would be invisible in a careful test and would
show up in play as "sometimes it kicks when I press it".

That is a picture jump caused by the press, on a design whose one rule is that
the press must not move the picture.

The genuine defect in this area is a different one, and §3.4 already fixes it:
the reference is captured from raw `m` (`headtrack.cpp:272`) while readers
consume smoothed `g_head` (`:99`), because `SteadyAim(m)` runs afterwards at
`:279`. Advancing the reference by the offset the readers actually used removes
the mismatch without touching the smoother's state at all.

**Ships as a switch anyway.** `RECENTRE_REQUIREMENT.md` item 3 asks for the
reset explicitly, and it is the authority. `recentre_reset_smoother` defaults to
`0` for the reasons above, and the owner can set it to `1` and feel the
difference in one session without a rebuild. If it is turned on, the reference
capture must move **below** `SteadyAim(m)` (`headtrack.cpp:279`) in the same
change, or the two errors compound.

**If instead the implementer chooses the re-latch formulation** (`AnglesFromMatrix`
on a fresh matrix) rather than the advance-by-credit of §3.4, then moving the
capture below `SteadyAim(m)` is **mandatory**, not optional. That is the one-line
ordering fix; without it every press injects a decaying (smoothed − raw)
residual.

---

## 5. Implementation checklist

Every edit, with the line it anchors to. `freelook_key` is the pattern
throughout. **Nothing in this list has been applied.**

### 5.1 `config.h` — eight new fields

Insert after `config.h:1116` (`int freelook_key = 0xA4;`), inside the same
head-tracking block, so the ini writer's neighbourhood matches.

| # | Field | Default | Note |
|---|---|---|---|
| 1 | `int  recentre_key` | `0x24` | VK_HOME — existing behaviour unchanged |
| 2 | `bool recentre_pad_enable` | `true` | |
| 3 | `int  recentre_pad_chord` | `0x00C0` | `XINPUT_GAMEPAD_LEFT_THUMB \| XINPUT_GAMEPAD_RIGHT_THUMB`; `0` = off |
| 4 | `int  recentre_pad_hold_ms` | `600` | |
| 5 | `bool recentre_pad_swallow` | `true` | hide the chord's buttons from the game while complete |
| 6 | `bool recentre_compensate` | `true` | **the master switch**; `0` = today, exactly |
| 7 | `bool recentre_compensate_invert` | `false` | |
| 8 | `bool recentre_reset_smoother` | `false` | see §4 |

Optional ninth (§3.6, not in the first build): `bool recentre_on_runtime_event = true;`

### 5.2 `config.cpp` — parser

**Do not extend the else-if chain.** `config.cpp:35-37` states VS2019 hits the
C1061 parser-nesting limit on it. Use the early-return form at the top of
`Assign()`; `menu_screen_key` at `config.cpp:40-43` is the exact template.
Insert after `config.cpp:63`.

```cpp
if (!_stricmp(key, "recentre_key")) { c.recentre_key = (int)strtol(value, nullptr, 0); return; }
if (!_stricmp(key, "recentre_pad_enable")) { c.recentre_pad_enable = flag(); return; }
if (!_stricmp(key, "recentre_pad_chord")) { c.recentre_pad_chord = (int)strtol(value, nullptr, 0); return; }
if (!_stricmp(key, "recentre_pad_hold_ms")) { c.recentre_pad_hold_ms = num(); return; }
if (!_stricmp(key, "recentre_pad_swallow")) { c.recentre_pad_swallow = flag(); return; }
if (!_stricmp(key, "recentre_compensate")) { c.recentre_compensate = flag(); return; }
if (!_stricmp(key, "recentre_compensate_invert")) { c.recentre_compensate_invert = flag(); return; }
if (!_stricmp(key, "recentre_reset_smoother")) { c.recentre_reset_smoother = flag(); return; }
```

`strtol(value, nullptr, 0)` for the two hex-written values, **not** `atoi` —
`config.cpp:27-33` and `tools\checkconfig.py` both enforce this, and getting it
wrong silently zeroes the key.

### 5.3 `config.cpp` — ini writer

Insert after `config.cpp:877` (`fprintf(f, "freelook_key = 0x%X\n", freelook_key);`).
The literal `"<name> = "` prefix is load-bearing — `tools\checkconfig.py` matches
`fprintf\(f, "([a-z_0-9]+) = `.

```cpp
fprintf(f, "# Recentre: takes your current head direction as straight ahead\n");
fprintf(f, "# WITHOUT moving the picture. Virtual-key code, 0x24 = HOME.\n");
fprintf(f, "recentre_key = 0x%X\n", recentre_key);
fprintf(f, "# Or click BOTH sticks and hold. The buttons are hidden from the\n");
fprintf(f, "# game while both are down, so nothing sprints or holds breath.\n");
fprintf(f, "recentre_pad_enable = %d\n", recentre_pad_enable ? 1 : 0);
fprintf(f, "recentre_pad_chord = 0x%X\n", recentre_pad_chord);
fprintf(f, "recentre_pad_hold_ms = %d\n", recentre_pad_hold_ms);
fprintf(f, "recentre_pad_swallow = %d\n", recentre_pad_swallow ? 1 : 0);
fprintf(f, "# THE MASTER SWITCH. 0 restores the old recentre exactly: the view\n");
fprintf(f, "# snaps to wherever the game was aiming.\n");
fprintf(f, "recentre_compensate = %d\n", recentre_compensate ? 1 : 0);
fprintf(f, "recentre_compensate_invert = %d\n", recentre_compensate_invert ? 1 : 0);
fprintf(f, "recentre_reset_smoother = %d\n", recentre_reset_smoother ? 1 : 0);
```

### 5.4 `config.cpp:1228` — `kKeysUnderstood`

```
constexpr int kKeysUnderstood = 202;   ->   210
```

**202 + 8 = 210.** With the optional ninth key of §3.6 it is **211**.
`tools\checkconfig.py` fails the build if this does not equal the count of
`_stricmp(key, "...")` matches, and separately if any key can be read but not
written. Run it before building.

### 5.5 `overlay.h` / `overlay.cpp` — expose the capture flag

- `overlay.h:150` — add beside `BindingDown`:
  `// True while a key row is listening for its new binding.`
  `bool BindingCaptureActive();`
- `overlay.cpp` — define it next to `BindingDown` at `:1363-1367`:
  `bool BindingCaptureActive() { return g_capture != 0; }` (`g_capture` is the
  file-scope int at `overlay.cpp:609`).

No other binding plumbing is needed: display (`overlay.cpp:694-697` via `KeyName`
at `:635`), arming (`Activate()`, `:950-955`), keyboard capture (`:1696-1708`)
and pad capture (`:1028-1030`) are all generic over `RK::Key`.

### 5.6 `overlay.cpp` — panel rows

Insert after the `"Head tracking"` row that ends at `overlay.cpp:264`, before the
`RK::Group, "STEADINESS"` at `:266`. The `Row` struct is at `:50-61`, `enum class
RK` at `:46`. `lo/hi/step` are cosmetic on `RK::Key` — `Adjust()` refuses to
nudge them (`:912-914`).

```cpp
{RK::Group, "RECENTRING", "", nullptr, 0, 0, 0, nullptr, 0, ""},

{RK::Key, "Recentre key",
 "Takes whatever direction you are facing right now as 'straight ahead'. The "
 "picture does not move when you press it - if you are looking down at the "
 "rifle, you stay looking down at the rifle, and that becomes your new "
 "neutral. Press Enter or A to change it, then the key or pad button you want.",
 &c.recentre_key, 0, 255, 1, nullptr, 0, "36 (Home)"},

{RK::Bool, "  ...or click both sticks",
 "Click the left and right sticks together and hold them for a moment. Both "
 "buttons are hidden from the game while they are both down, so you will not "
 "sprint or hold your breath by accident. Nothing happens until you have held "
 "them long enough, and it fires once however long you hold.",
 &c.recentre_pad_enable, 0, 0, 0, nullptr, 0, "on"},

{RK::Int, "  ...how long to hold",
 "Milliseconds both sticks must be held before it recentres. Longer is harder "
 "to trigger by accident; shorter feels more immediate.",
 &c.recentre_pad_hold_ms, 200, 2000, 100, nullptr, 0, "600 ms"},

{RK::Bool, "Recentring keeps the picture still",
 "ON: recentring hands your current offset to the game, so the view does not "
 "move at all - only what counts as 'neutral' changes. OFF restores the old "
 "behaviour, where the view snaps round to wherever the game was aiming. Leave "
 "it on unless you are comparing the two.",
 &c.recentre_compensate, 0, 0, 0, nullptr, 0, "on"},

{RK::Bool, "  ...recentring goes the wrong way",
 "Turn on if pressing recentre throws the view TWICE as far instead of leaving "
 "it still. Which way the game counts this is not known until it is tried.",
 &c.recentre_compensate_invert, 0, 0, 0, nullptr, 0, "off"},
```

### 5.7 `overlay.cpp:261-263` — the help text that names the key

Currently: *"Turn your head and the game world turns with it. **Press HOME at any
time** to set your current head direction as 'straight ahead'. …"*

Must stop naming a key that is now rebindable. Replace the middle sentence with a
pointer to the RECENTRING group below it.

### 5.8 `headtrack.h` — new declarations

Around `headtrack.h:62` (`void RecentreHead();`):

```cpp
// True once, on the tick the request is taken. Interlocked - there are two
// camera objects alternating and only one of them may perform the recentre.
bool TakeRecentreRequest();
// Hand it back: this tick could not credit the offset (gated, or free-look).
void PutBackRecentreRequest();
// The raw head offset the readers are using right now, before the invert flags.
bool CurrentRawOffset(float* yaw, float* pitch);
// Advance the reference by exactly what was credited to the engine.
void CommitRecentre(float rawYaw, float rawPitch);
// The apply sites stamp this whenever an offset actually reaches the picture,
// so "nothing is being applied" is an observation and not an inference.
void NoteHeadOffsetApplied();
```

### 5.9 `headtrack.cpp` — the trigger

Replace `headtrack.cpp:238-245` (the `wasHome` / `GetAsyncKeyState(VK_HOME)`
block) with:

- keyboard: `const bool down = BindingDown(Cfg().recentre_key);` edge-triggered
  on a `static bool wasDown`, in the shape of `overlay.cpp:1728-1735`;
- pad chord: `const WORD pads = g_overlay.PadButtons();` and, if
  `recentre_pad_enable` and `recentre_pad_chord` and
  `(pads & chord) == chord`, start/continue a `static DWORD chordSince`; fire
  once when `GetTickCount() - chordSince >= recentre_pad_hold_ms`; re-arm only
  when `(pads & chord) == 0`;
- both gated on `!BindingCaptureActive()`;
- both call `RecentreHead()` and log which route fired.

`headtrack.cpp` already includes `overlay.h` (`:6`), so `BindingDown`,
`g_overlay.PadButtons()` and `BindingCaptureActive()` are all in scope.

### 5.10 `headtrack.cpp` — the consume block

`headtrack.cpp:265-277`:

- Keep the existing block **verbatim** as the `else` branch of
  `if (Cfg().recentre_compensate)`, so "off" is provably off.
- The compensating branch does **not** consume the request here. It leaves it
  pending for `camera_probe.cpp` to perform (§5.11), except for the
  nothing-is-being-applied fallback, which latches yaw and pitch, sets
  `g_havePosRef = false`, and logs `[head] recentred with no offset outstanding`.
- `g_rollRef` is pinned to `0.0f` and never written in the compensating branch
  (§2.4).
- `g_recentre` (`headtrack.cpp:39`, written at `:267`) is dead — never read
  anywhere in the tree. Remove it with this change; it is inside the feature, so
  the "zero lines removed outside the feature" rule (§9.2) is not broken.
- If `recentre_reset_smoother` is on, the recentre needs to reach `SteadyAim`'s
  function-local `active` (`headtrack.cpp:122`). Promote it to a file-scope flag
  or give `SteadyAim` a `bool resetNow` parameter — there is no way to reach the
  statics from outside today.

### 5.11 `camera_probe.cpp` — the credit

- Cache the aim object at the top of the aim-consumer hook (the function that
  reads `a + apex::kAimLatchByte` at `:1676`), before any gate, so the pointer is
  available even on gated ticks.
- Call `NoteHeadOffsetApplied()` from both apply sites — the aim-consumer block
  (`:1708` and `:1771`) and `WriteYawField` (`:1876`, `:1891`).
- Immediately after the pitch write at `camera_probe.cpp:1781`, inside the same
  `__try`, and before `NoteAppliedOffset(...)` at `:1811`:

```cpp
if (Cfg().recentre_compensate && TakeRecentreRequest()) {
    float rawYaw = 0.0f, rawPitch = 0.0f;
    if (CurrentRawOffset(&rawYaw, &rawPitch) && a && (pYaw || pPitch)) {
        const float s = Cfg().recentre_compensate_invert ? -1.0f : 1.0f;
        char* aim = static_cast<char*>(a);
        if (pYaw) {
            float* acc = reinterpret_cast<float*>(aim + apex::kAimInputYaw);
            *acc = WrapPi(*acc - s * yawDelta);
        }
        if (pPitch) {
            float* acc = reinterpret_cast<float*>(aim + apex::kAimInputPitch);
            *acc = *acc - s * pitchDelta;
        }
        CommitRecentre(pYaw ? rawYaw : 0.0f, pPitch ? rawPitch : 0.0f);
        COTW_LOG("[head] recentred: credited yaw %+.2f pitch %+.2f deg to the "
                 "player's own aim, so the picture did not move",
                 yawDelta * 57.2957795f, pitchDelta * 57.2957795f);
    } else {
        PutBackRecentreRequest();      // gated tick - try again next frame
    }
}
```

`apex::kAimInputYaw` / `kAimInputPitch` are `apex.h:148-149`. `WrapPi` is
currently file-local in `headtrack.cpp:89`; either export it or duplicate three
lines locally.

### 5.12 `dllmain.cpp` — the chord swallow

In `COTWVR_PadPostProcess`, after `g_overlay.ConsumePad(userIndex, state);`
(`dllmain.cpp:185`) and beside the existing `lock_pitch_pad` block (`:193-195`):
clear `recentre_pad_chord`'s bits from `state->Gamepad.wButtons` while, and only
while, every bit of the chord is down. Same ordering reason as the pitch lock —
after the panel, before the game.

### 5.13 Build gates

1. `python tools\checkconfig.py` — passes only if all eight keys are readable and
   writable and `kKeysUnderstood == 210`.
2. Diff the finished change and require **zero lines removed outside the
   feature** (§9.2). Expected removals: the `VK_HOME` poll, the old consume
   block (moved into the `else`), and dead `g_recentre`.
3. Read the deployed DLL back and compare its hash to the build (§9.3). The copy
   is refused while the game holds the DLL open, and that has already cost one
   full test session.

---

## 6. Risks, and how each shows up in the headset

| # | Risk | What the player sees | Instrument / mitigation |
|---|---|---|---|
| 1 | **Credit sign inverted** — the accumulator is written the wrong way | The press throws the view **twice as far** as it does today, same direction. Unmistakable. | `recentre_compensate_invert`. The log line prints the credited degrees; compare to the jump. |
| 2 | **Credit applied twice** (both eye passes; there are two alternating camera objects, `camera_probe.cpp:1729-1736`) | The press swings the view by one whole offset the **wrong** way. | Interlocked consume in `TakeRecentreRequest()`; the log line must appear exactly once per press. |
| 3 | **Recentre performed on a gated tick** (`latchArmed`, engine idle, stance changing) | Normally fine, but pressing it during a crouch, or just as the engine bakes the aim, jumps the view. | Defer instead of performing; log the deferral once a second so a stuck request is visible. |
| 4 | **The fallback fires while an offset is live** — e.g. free-look held, so the apply site moved to `WriteYawField` | A jump on the press, but only when free-look was held. | The "nothing applied" test must read a timestamp stamped by **both** apply sites, not the config flags. |
| 5 | **Pitch accumulator clamps** at a tighter range than the camera field | Recentring while looking far up or down moves the view **a little, in pitch only, near the extremes**. Normal presses are clean. | Log the accumulator before and after; if they differ from the intended delta the clamp bit. |
| 6 | **The undo / hold-base machinery reverts the credit** (`HoldBaseIfEngineIdle`, `NoteAppliedOffset`, `head_undo_mode`) | The view is still at the press, then **springs back** a frame or two later. | `head_undo_mode = 0` in the live ini and the credit targets a different object (`aimObj+0x1C`, not `camera+0x4C`), so this should not fire — but it is the first suspect if the recentre "half works". |
| 7 | **`recentre_reset_smoother = 1`** | A kick of up to ~6° (τ=60 ms) when the key is pressed **mid-turn**; nothing at all when standing still. Reads as "it sometimes jolts". | Defaults off. §4. |
| 8 | **The pad chord collides with a CotW action** | A sprint or held-breath twitch at the instant of recentring; or, with the swallow on, sprint and hold-breath stop working while both sticks are clicked. | `recentre_pad_chord` is a config value — move it or set it to `0` without a rebuild. |
| 9 | **The chord fires by accident** | An unexplained recentre mid-hunt. **Under this design that is now harmless** — the picture does not move — which is itself an argument for the compensated version. | 600 ms hold; fires once per chord; re-arms only on full release. |
| 10 | **The recentre key is swallowed by the raw-input hook** | HOME worked before the change and does nothing after it. | `BindingDown` reads `async \|\| hookTable` (`overlay.cpp:1355-1367`). Never `GetAsyncKeyState` directly. |
| 11 | **A recentre fires while the panel is capturing a binding** | Binding the recentre key immediately recentres, on the press that bound it. | Gate on `BindingCaptureActive()`. `HeadTrackTick` does not go through `Overlay::Tick`'s swallow at `overlay.cpp:1721`. |
| 12 | **Roll left latched from the startup sample** (i.e. §2.4's change not made) | The horizon sits permanently canted by whatever the head was doing on the first pose of the session, and no press ever clears it. With `head_write_roll = 1` this is live. | `g_rollRef` pinned to 0. |
| 13 | **A runtime-side recentre** (Meta button long-press, SteamVR reset, guardian re-setup) | The picture jumps and stays off-centre until the key is pressed. Nothing in the mod reacts. | §3.6, deliberately out of the first build. |
| 14 | **Config key added to `Load()` and forgotten in `Save()`** | The setting works until the game exits, then comes back off — reads exactly like a broken feature. Has happened four times. | `tools\checkconfig.py`, plus `kKeysUnderstood = 210`. |

### The test

From `RECENTRE_REQUIREMENT.md`, unchanged:

> Look down at the weapon, press recentre. The picture must not move at all.
> Then turn the head level: the view should follow from there, with the
> down-look now reading as neutral.

Three more that catch the failures above and cost nothing:

- **Press it ten times while standing still.** The view must not walk. Any drift
  is risk 1 or 2, and the direction says which.
- **Look up at the sky and press.** This is the owner's original report; it must
  now do nothing visible.
- **Recentre, then crouch immediately.** With the offset genuinely at zero the
  ratchet has nothing to absorb, so the crouch should be clean. If it still
  ratchets, the credit did not land.

And per §9's standing rule: every path logs **once**, saying which one it took —
credited, deferred, or latched-with-nothing-outstanding. "Did not run" and "ran
and did not help" look identical from inside a headset, and each has cost this
project a session.
