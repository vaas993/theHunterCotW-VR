# theHunter: Call of the Wild VR — features, findings, and what they cost

Companion to `ARCHITECTURE.md`. That file covers how the mod is built; this one
covers what was learned making each feature work, including the things that did
not work and why.

Everything marked **measured** was observed in the running game or read out of
the disassembly. Everything marked **inference** is reasoning from those
observations and is labelled as such, because most of the wasted effort in this
project came from confusing the two.

---

## 1. Head roll — tip your head, the horizon tips

**Config:** `head_write_roll`, `head_roll_field`, `head_roll_invert`,
`head_roll_scale` · **Overlay:** HEAD TRACKING → *"Tilt the world with my head"*

Roll was being computed from the headset the entire time (`AnglesFromMatrix` in
`headtrack.cpp`) and simply never written to the game, which is why the world
stayed stubbornly level however far you leaned your head.

The write mirrors the pitch write exactly, one field along. **Yaw is `+0x4C` and
pitch is `+0x50`, so roll was guessed at `+0x54`** — and confirmed in the headset
first try. It is still exposed as a *setting* rather than a constant, because it
was a guess: if it had been wrong, `head_roll_field` could be moved from the
overlay without a rebuild.

Roll is deliberately **not** part of the crouch-undo bookkeeping that yaw and
pitch use. That machinery exists to stop the engine adopting an outstanding
offset as the player's permanent aim, and roll is not part of the player's aim.
**(inference — if the horizon ever starts creeping after crouches, that
assumption was wrong.)**

---

## 2. Smoothness — the judder was an ordering bug

Covered in full in `ARCHITECTURE.md` §4b. In short: `SubmitFrame`
(`render_hook.cpp:109`) runs **before** `HeadTrackTick` (`:148`), so the angles
that rotated the camera for the frame being submitted came from the pose located
at the *previous* present — while the *freshly* located pose was being submitted
with it. A projection layer's pose is the origin the compositor reprojects
**from**, so declaring one the pixels never saw put it out by exactly one frame
of head motion: zero error while still, growing with head speed.

**Config:** `submit_rendered_pose` · **measured:** *"smoothness is better"*.

---

## 3. The flat menu screen

**Config:** `menu_screen`, `menu_screen_key`, `menu_screen_auto`,
`menu_screen_width_m`, `menu_screen_distance_m`, `menu_screen_hide_world`,
`menu_screen_use_clip`, `menu_screen_log_signals` · **Overlay:** GAME MENUS

The game draws its menus flat, at screen scale, and they were then stretched
across the headset's whole field of view — so the edges, and therefore most of
the options, sat outside where the eye can comfortably look. The settings screen
could not be read at all.

**The fix needs no scaling.** The backbuffer is copied verbatim onto an
`XrCompositionLayerQuad` — the same mechanism the settings panel already uses —
and it is the quad's **size in metres** that decides how big it looks. No blit
shader, and no need to identify which draw calls are UI, which is unreachable
anyway: the frame census measured only **~0.3% of the frame** reaching this
DLL's D3D hooks, because the game records command lists and replays them.

### Two mistakes worth not repeating

**The code went into a branch that never runs.** There are two
`if (back) back->Release();` sites in `VRSystem::SubmitFrame`. The other one is
inside the *"could not create eye swapchains"* failure branch, which only
executes while VR is shutting down. The first attempt attached to that one, so
the feature never ran once — and the log had no `[screen]` line at all while the
setting looked switched on. **Anchor on the `// Alternate-eye flips here`
comment that follows the correct release, not on the release itself.**

**Hiding the world only for the main menu looked broken.** The reasoning was
"keeping the world visible during an in-game menu is safer". It is not: what is
behind the panel *is the same image the panel is showing*, wrapped across the
entire field of view. The result was the game rendered twice at two sizes — the
owner's words were "one flat screen inside the other". The world is now hidden
whenever the panel is up.

### Detecting the menu — one signal solved, one not

| menu | signal | status |
|---|---|---|
| Main menu | no player camera | **works** — the detector full-rate stereo already relies on |
| ESC-opened in-game menu | mouse cursor visible | **works** |
| **START-opened in-game menu** | — | **UNSOLVED** |

The decisive clue came from the owner: *"while playing and press menu button, the
menu opens but the game stays live in the background"*. `PlayerCameraActive()`
stays **true** and the framerate does not move, so that detector cannot see the
in-game menu at all.

A signal trace (`menu_screen_log_signals`) then measured what does move:

```
gameplay : cursor showing=0  bitmap=0x0      clip=3088x2108 (game window)
menu open: cursor showing=1  bitmap=0x10003  clip=5760x2160 (whole desktop)
```

All three flip together — for a menu opened with **ESC**. Opened with **START**,
none of them move: a pad-navigated menu has no use for a cursor, and the game
does not release the clip either. **No Windows-level signal can see it.**

The remaining route is the game's own menu flag, found the way the aim field was
found — a Cheat Engine session watching what flips when START is pressed. Until
then the workaround needs no detection at all: **bind the flat screen to START**,
so one press opens the menu and raises the panel together.

---

## 4. Day length

**Config:** `cheat_time_scale` · **Overlay:** CHEATS → *"Day length"* ·
range 0.05–10.00, step 0.05

Not done by writing the engine's own multiplier. A cheat table claims it lives at
`+0xF4` on the time manager, but that table targets a different build and the
offset has never been checked against ours — `LogTimeLayoutOnce()` prints that
neighbourhood precisely so nobody trusts it on faith. Writing an unverified
offset into a live game is how you corrupt something unrelated and lose a day
finding it.

It does not need writing. The engine advances the hour itself every frame — the
same fact that made a single "set time" write useless — so the mod **measures the
engine's own step each frame and re-applies it scaled**. Works below 1 and above
it, needs no unknown field, and releases cleanly the moment the setting returns
to 1.

---

## 5. Input capture — a mod overlay must own the keyboard

**This is the most reusable thing in this document.**

There are **three** ways a game can read a key, and a keyboard hook only closes
two of them:

| path | blocked by |
|---|---|
| `WM_KEYDOWN` | `SetWindowsHookExW(WH_KEYBOARD_LL)` returning 1 |
| `GetAsyncKeyState` | the same hook — a swallowed key never reaches the async state |
| **Raw Input (`WM_INPUT`)** | **nothing.** Delivered by the raw input manager *before* the hook chain runs |

Raw Input is the one that leaks, and every modern title uses it. It is closed by
hooking **`GetRawInputData` and `GetRawInputBuffer`** and rewriting the keyboard
record to **VKey `0xFF`** — the documented "keyboard overrun" value that sane
readers skip. That is a gentler lie than failing the call, which would send the
game's input code down a path it may not expect. Both entry points are hooked
rather than guessing which one a given game uses.

`NEXTRAWINPUTBLOCK` is written in terms of `QWORD`, which is not always in scope;
it only means "advance by `dwSize`, rounded up to 8".

### The trap this creates

Once keys are swallowed, **`GetAsyncKeyState` cannot see them either — so the
overlay goes deaf to its own keys.** Every panel key read must go through a
helper that returns `async || hookTable`:

```cpp
bool PanelKeyDown(int vk) {
    if ((GetAsyncKeyState(vk) & 0x8000) != 0) return true;   // not swallowed
    if (g_kbReady) return g_kbDown[vk] != 0;                 // swallowed
    return false;
}
```

Reading only the hook table made the panel refuse to open at all. Reading only
the async state made the bind capture arm while ENTER was **still held**, so the
key that opened the bind immediately bound itself.

**Never swallow the open/close key.** Insert is deliberately excluded, so a bug
in the hook cannot lock the player out of their own settings. That already
happened once.

---

## 6. Bindings — keys *and* controller buttons

**Overlay:** any `RK::Key` row — Enter or A arms it, release, then the next key or
pad button is taken. ESC cancels.

A binding is stored as one int: `0` unbound, `1..0xFE` a virtual key code, or
`kPadBind | wButtons` for an XInput button. Gamepad buttons are **not** virtual
key codes, so a capture that only scanned VK codes could never see one.

`BindingDown()` asks the right hardware for whichever kind it is, so the same
binding works for the flat-screen hotkey and for free-look.

### Three bugs, one root cause: reading input from somewhere stale

Far Cry 2's mod gets this right and is the reference — its `BindingHandlePad` is
called *"from the XInput proxy's post-process"*, capturing on live state rather
than from a cached copy. Every bug here was a departure from that.

1. **`m_prevButtons` was one value across all four XInput slots.** The real pad
   reports on slot 0; a second "connected" pad — Steam Input's virtual device
   makes this the *normal* case, not an exotic one — reports all zeros on the
   next slot and wipes it. A held button therefore read as a fresh press on every
   poll: **one press acted like ten.**
2. **`m_padButtons` had exactly the same bug**, which produced a baffling
   symptom: a binding could be *captured* but then did nothing. Capture runs
   inside the pad callback on that slot's live state, so it worked; the hotkey
   reads the cache, which was always `0x0000`.
3. **The capture scan asked `GetAsyncKeyState`** for keys the new hook swallows
   — see §5.

**Per device, one owner, live state.**

### And the prompt nobody could see

The capture block `return`s early to swallow input, and the redraw check lives at
the *bottom* of `Tick()`. So while it was listening the panel never repainted:
the row looked exactly as it had a second earlier, which reads as "nothing
happened" rather than "waiting for you". It now repaints inside that block, and
the help area is replaced with the instruction while listening.

---

## 7. The overlay panel

- **Columns must not share pixels.** The label box ran `62 → 582` while the value
  box started at `562` — twenty pixels of overlap, so any long label ran
  underneath its own value. Both columns now derive from one width with a real
  gap, and both are ellipsised: a label clipped mid-letter reads as a rendering
  fault, an ellipsis reads as "there is more to this name".
- **The help text is the point of a setting.** The box was 96 px — about four
  lines — and the longer descriptions were losing their last sentence, which is
  usually the *why*. The panel is 960 px tall (was 860) and the help box 156 px,
  with the vertical budget in one place so the list and the help cannot grow into
  each other again.
- **The stick is edge-triggered with hysteresis** (20000 to act, 12000 to re-arm)
  plus the d-pad's 400 ms grace and acceleration. It used to fire on a bare
  170 ms timer with no notion of returning to centre, so one flick scored several
  steps and its speed depended on how often the game happened to poll us.

---

## 8. The crouch ratchet — open, but *located*

Crouch, stand, prone and jump each convert part of the player's head offset into
permanent aim. Slopes do not. Repeat it with the head turned and the view walks
around, past full turns.

**Measured** with a live Cheat Engine watch (injection at `0x006440EF` capturing
`rsi`), hands off the mouse, head held ~30° right, ten crouch/stand cycles:

```
aimObj+0x1C (yaw accumulator)   +0.3327127099 -> -2.390461445
aimObj+0x20 (pitch accumulator) +0.0091840848 -> -0.1309515089
aimObj+0x5C/+0x60/+0x64         0 -> 0   NEVER FIRED
aimObj+0xBC (the save latch)    0 -> 0   NEVER SET
```

The owner had separately confirmed those fields move with the **mouse** but not
with the headset. The mouse was untouched. **So the engine itself writes our head
offset into the player's permanent look accumulator.** The scripted-camera save
is innocent — it never fired.

The endpoint is wrapped and the view went round several full turns, so the true
total is much larger and the per-event absorption may **exceed** the outstanding
offset. No design may assume a bounded gain.

**What is ruled out:** an open-loop servo. One was built and shipped
(`head_yaw_servo`); the owner's verdict was *"no difference how the rotation
jumped … with the servo it is continuous and smooth, with it off it is a snap"*.
Same total rotation. It tracked `g_servoApplied` — its own tally of what it
**wrote** — so when the engine baked into the field the tally did not move, the
error did not move, and it never opposed anything. **A correction that reads only
its own bookkeeping cannot reject a disturbance; it must read the actual field.**

**What is needed:** a closed loop on `aimObj+0x1C` itself. The hard part is that
the **player's mouse writes the same field**, so cancelling changes
indiscriminately kills mouse-look. The candidate discriminators are in
`apex.h`; the harness that found the field is
`cheat engine\WATCH_the_ratchet.md`, and it is reusable for any
"which number persists" question — it beat eleven hook-based probes because it
watched every candidate at once instead of one at a time.

---

## 9. How this project actually makes progress

Written down because the pattern was consistent enough to be worth a rule.

**Every fix that stuck came from a measurement. Every fix shipped on reasoning
alone broke.**

- The aim field: eleven hook probes over days, then one 20-minute Cheat Engine
  session found it.
- The judder: reading the actual call order in `render_hook.cpp`, not theorising
  about latency.
- The pad bugs: log lines, not argument.
- The two features shipped on reasoning alone — the servo and the first flat
  screen — were both broken, and one of them broke head tracking as well.

**Three habits that came out of it:**

1. **Reset to the last known-good source before adding anything**, and rebuild it
   to confirm it produces the **same byte size** as the known-good DLL. The
   servo's damage happened because it was built on a tree that already contained
   failed work.
2. **Diff the finished change and require zero lines removed outside the
   feature.** Roll came out +45/−0 with every render file untouched. The servo
   failed precisely because it rewired shared paths that were not behind its
   flag, so "off" was not off.
3. **Read the deployed file back and compare its hash to the build.** The copy is
   refused while the game holds the DLL open — that silently cost a full test
   session, with a feature reported missing that had never been installed.

**And instrument for the difference between "did not run" and "ran and did not
help".** Those look identical from inside a headset and cost a session each to
tell apart. Every feature here now says which, once, in the log.

## Full-rate stereo could only be switched ON at launch (fixed 2026-08-15)

Owner: *"if launching the game with stereo mode, in game i can switch between
stereo and aer, but the opposite is not true"*.

**Cause, and it was a deliberate decision that had expired.** `dllmain` installed
the scene-draw hook only when full-rate was ALREADY on:

> "An unproven hook does not get installed just in case; toggling full-rate is
> worth a relaunch."

That was right when it was written - the hook had once been installed with the
wrong signature and broke the known-good alternate-eye path. But full-rate has
been verified in the headset since 2026-08-02 (locked 90 fps, zero stash
failures), so the premise is gone, while the cost stayed: **a control that works
in one direction only, with nothing on screen saying which case you are in.**
That is worse than one that plainly needs a restart.

**Fix:** the hook is installed whenever stereo is, and left INERT by its own
per-frame gate (`want` in frame_hook.cpp reads `full_rate_stereo` every frame).
`full_rate_hook_always = 0` restores the old caution for anyone who suspects the
detour.

**Not a bug, worth knowing:** switching INTO full-rate still needs the world on
screen (`PlayerCameraActive()`) and the warm-up counter passed - so it will not
engage while sitting in a menu. The panel row says so now.

**Build `A6668868A6A8`.**

## Sharpening and saturation did nothing in the mode the mod ships in (fixed 2026-08-16)

Both sliders worked when tested, and both did nothing in normal play. The
difference was the rendering mode, and the reason is a fast path that had been
optimised past the pass.

`Present` picks one of four routes. Two of them build the eye image in a **hold
texture** and then move it into the OpenXR swapchain image — that move is what
`PostProcessApply` replaces, so sharpening and saturation ride along for free.
The third, **AER fast (1 blit)**, exists precisely to avoid the hold: it crops
the backbuffer straight into the swapchain image with one
`CopySubresourceRegion`, saving two blits per frame. The post-process pass reads
a whole texture and writes a whole texture, so on that route there was nothing
for it to read — and it was never called. No failure, no log line, no fallback
fired, because nothing had gone wrong. The feature was simply not on the path.

That was the shipping path. Full-rate stereo has a hold, so the sliders worked
there; alternate-eye is the default, so for the owner they did not.

**Fix:** asking for either slider buys the hold back on that route —
`StashInto` then `PostProcessApply`, with the existing copy as the fallback.
The two blits the fast path saves are only spent when a slider is off its
default, so the default configuration still costs exactly one blit.

**The general shape:** *a feature that hangs off an operation disappears when
an optimisation removes the operation.* The pass was written as "it REPLACES the
CopyResource" — correct, documented in `post.h`, and quietly conditional on
there being a CopyResource to replace. Worth checking the same way for anything
else bolted onto the hold path: it is present on three routes out of four.

## The log: what it costs, and the 100 MB nobody saw (2026-08-16)

Asked what the log costs before shipping, so it was measured rather than
guessed. From real session logs:

* **Normal play: ~2.4 lines a second** - the once-per-five-second counters and
  nothing else. About 1 MB an hour.
* **With the DLSS diagnostics on: ~16 lines a second.** The longest such session
  here was 8.7 MB in 53 minutes.

Each line took a mutex, an `fprintf`, an `fflush`, and **two
`OutputDebugStringA` calls**. The flush is the honest cost and it stays - a log
that loses its last page never records the thing that went wrong - and at a
couple of lines a second it is not measurable against a frame. `OutputDebugString`
was pure waste: it takes a machine-wide named mutex and signals two events for a
debugger that is almost never attached, and it broadcast the mod's internals to
any DebugView window on the system. It is now behind `IsDebuggerPresent()`.

**The real finding was not the cost.** `LogInit` falls back to
`cotwvr.pidNNNN.log` when another process already owns the main file - and the
game reliably starts a short-lived process before the one that renders, so
**one orphan file was left behind on every single launch**. This machine had
**187 of them, about 100 MB**, accumulating silently since the project started.
On a shared release that is a slow leak into every player's AppData.

Three changes, all in `log.cpp`:

1. `PruneProcessLogs` keeps the newest 3 pid logs and deletes the rest at every
   launch. `cotwvr.log` and `cotwvr.prev.log` are never touched.
2. A 64 MB cap, which writes one final line saying why it stopped.
3. A `logging` switch (default on), applied **after** the opening lines. Off
   means "stop the commentary", not "write nothing": what the game build is,
   which hooks took and what was applied are what a bug report cannot do
   without, and a log that can be silenced before it says anything is a support
   channel that fails exactly when it is needed.

**The shape worth remembering:** the per-file cost was fine and the per-launch
*count* was the problem. Nobody looks at a log directory listing; the cost
showed up as a number of files, not as a number of milliseconds.

## Two buttons that both said "recommended" (2026-08-16)

The in-game panel and the launcher each have a button that puts you on the
tested configuration. They are written in different languages, in different
files, by hand - and they had drifted: the panel set `weapon_3d = 0` and
`head_write_roll = 0` for reasons found weeks after the launcher's list was
captured, and the launcher put both back to 1. Thirteen further settings the
panel had learned about were missing from the launcher altogether.

Press one, then the other, and you land on a configuration nobody has ever run -
and neither button is doing anything wrong, so nothing reports it.

`tools/checkrecommend.py` now parses `RecommendAct` out of `overlay.cpp`,
compares it against the launcher's tables, and fails the build on a conflict or
an omission. It runs beside `checkconfig.py` in `build.bat`. The first run found
all fifteen.

**The general form:** *the same claim, restated in two places, is a claim that
will eventually be false in one of them.* Where it cannot be derived once, it
has to be compared automatically - a comment saying "keep these in sync" is not
a mechanism.

## The weapon-depth gate, a fourth time — and why fixing the gate did not fix it (2026-08-16)

Reported again: *"disabling weapon 3d is causing shimmering and blurriness ... I
thought we fixed that previously."* It had been fixed, in `Hook_Map`, on
2026-08-14 — and that gate was still correct. This was a different gate, of
exactly the same kind.

The constant-buffer **snapshot table** is written on two hooks and read by four
things. Each writer carried its own hand-written list of who might want it:

```cpp
// Hook_Unmap
if ((Cfg().weapon_3d || Cfg().hud_probe || Cfg().taa_probe) && ...)
// UpdateSubresource
if (src && res && (Cfg().weapon_3d || Cfg().taa_probe))
```

Both name the weapon and the probes and stop there. But `taa.cpp` calls
`CBNoteVelocityPass` and `CBScanResolveConstants` **every frame**, and both read
that table through `FindSnapshot`. With weapon depth off and no probe running,
the table was empty, the resolve identified its own pass without the constants
that describe it, and the picture went soft — the moment an unrelated switch was
turned off.

**Why the previous fix did not prevent this.** It was applied to the *gate*, not
to the *pattern*. There were three hand-maintained lists of dependants, and
correcting one of them left the other two to drift. A comment saying "this file
keeps paying for it" had been written three times above one of them.

**Fix:** one predicate, `SnapshotsWanted()`, named for the RESOURCE rather than
for any feature, next to the table it guards. Both writers call it; a new reader
adds itself in one place and every writer follows.

**The rule, generalised:** *when a resource has several writers and several
readers, the question "does anyone need this?" must exist once.* Any version of
it that is restated per writer is a set of lists that agree only until someone
adds a reader — and the failure is silent, because every switch involved does
exactly what it says.
