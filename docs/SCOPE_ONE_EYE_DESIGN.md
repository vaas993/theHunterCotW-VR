# ONE OPTICAL AXIS WHILE SCOPED

*Design decision, theHunter: Call of the Wild VR — 2026-08-10*

**Owner's report:** *"in real life when aiming with scope we have to close one eye and look with one. how can we implement the same in the game, because right now each eye looks at the scope crosshair from different angle and if i shoot based on one of them i will miss my shot."*

**Decision, in one line:** while the player is looking down an optic, render **both eyes from one camera** — the game's own, the one the bullet leaves from. Do not blank an eye. Do not render the scope to one eye. Do not touch the per-eye crop.

**Master off switch:** `scope_mono = 0`. Default. With it at 0 the build behaves exactly as it does today, byte for byte.

---

## 1. The problem is geometry, not a rendering defect

Do not let anyone re-diagnose this as a bug in the classifier, the flicker work, or the crop. It is not.

**A real telescopic sight has an exit pupil.** The optic projects the target image and the reticle into a small cone of space behind the eyepiece. Only one eye can occupy that cone. Inside it, the reticle and the target are collinear along **one** optical axis, and that axis is aligned to the bore. Closing the other eye is not a technique — it is the consequence of a physical constraint of glass. The shooter does not *want* one eye; the instrument only has room for one.

**In VR the scope has no exit pupil.** The magnified picture is a screen-space image on a virtual lens, and both eyes are shown it from their own position, `ipd_mm = 64` apart. Concretely, in this build:

- The eye offset is applied at `src\cotwvr\stereo.cpp:90-92`, and it writes **translation only** — `m[12..14]`. Rows 0-2 of the camera basis are never touched. So the two eyes render the same world along the **same axis**, from two points 32 mm either side of it.
- The reticle, the lens disc and the mask are all at **zero disparity**: `Weapon3dAmount()` (`cbscan.cpp:2179-2183`) returns `weapon_3d_amount_scoped = 0.0000` while scoped, so `ShiftWeaponClipX(m, 0)` is a no-op and `ApplyWeaponPose` carries no eye sign at all. Identical pixels, identical angle, both eyes.
- The **world** is not at zero disparity, and while scoped it is magnified. The engine narrows the whole scene's projection when you aim — measured `1.08 → 2.98` in reciprocal tangent (`vr.cpp:1230-1244`) — while the mod deliberately pins the submitted field to the base frustum so the headset's field of view does not collapse. Everything in the picture is therefore presented at an angular magnification of

  ```
  M = T_base / T_rendered  =  0.9254 / 0.3360  ≈  2.75×
  ```

  and **world disparity is multiplied by exactly the same M**, while the reticle's stays at 0.

So the eyes are asked to fuse a reticle at optical infinity against a target pulled to an apparent depth of `D/M`. The angular disagreement between reticle and target is `M · IPD / D`:

| target range | reticle-to-target disparity, as displayed |
|---|---|
| 20 m | 30 arcmin |
| 50 m | 12 arcmin |
| 100 m | 6 arcmin |
| 150 m | 4 arcmin |

Panum's fusional limit for fine foveal detail is roughly 6-10 arcmin. **At every range this game is played at, the crosshair and the animal cannot both be single at the same time.** One of them doubles. That is the owner's "each eye looks at the crosshair from a different angle."

**And this is what makes the shot un-placeable, not the ballistics.** Because the eye offset is a pure translation, a reticle centred using one eye alone puts the bullet **32 mm laterally off that point, at any range, at any magnification** — it does not grow with distance. 32 mm is inside the vitals of every animal in the game. The geometry says he should still be hitting. He misses because he cannot tell where the crosshair is, so he places it somewhere it is not. **The fix must remove the ambiguity, not correct an offset.**

---

## 2. The decision, and the argument that settles it

Four candidate approaches were assessed independently. Three of them are variants of "make one eye authoritative": render the scope to one eye only, blank/dim the other eye, or annotate which eye to trust. The fourth is "force zero disparity."

### 2.1 The verdict on each

| candidate | verdict | why |
|---|---|---|
| Render the scope picture to one eye only | **rejected** | A bright monocular disc inside a fused binocular field is a binocular-rivalry generator, not a closed eye — it alternates and drops out at 0.5-2 Hz. It also needs a `return` inside the draw hooks, which puts the glass and the mask (measured to be **one disc issued twice**, `cbscan.cpp:3756-3761`) on two different code paths that differ *by eye*. That is the crescent bug re-entered by a new door. And it leaves the 2.75×-magnified doubling of the whole surround untouched. |
| Blank or dim the non-shooting eye | **rejected as primary, kept as an option** | Genuinely works and is cheap (`XR_EYE_VISIBILITY_LEFT/RIGHT` on a quad layer, template already at `vr.cpp:1813-1854`). But it costs binocular summation (the view goes visibly dimmer on every scope raise — bad in a dusk-hunting game), it drifts the covered eye to its phoria within seconds so the eyes must re-fuse every time the scope comes down, and Quest 3's LCD "black" is a dim grey field with mura, not an eyelid. It imports the limitation of glass without importing any benefit. |
| Force zero disparity on the scope's screen **position** | **rejected — it is a null edit** | This is what the tuned ini already does. `weapon_3d_amount_scoped = 0.0000`, `weapon_stereo = 0`, `weapon_cb_stereo = 0`, `weapon_screen_stereo = 0`, and the lens classified as `h.stencil` so `atRasterizer` is false. The reticle is already at exactly zero disparity in both eyes. Proposing it is proposing today, and today is the complaint. |
| **Force zero disparity on the scope's *content* — one camera for both eyes while scoped** | **ADOPTED** | One multiplication, at one site, that already exists. Puts both eyes on the bore. |

### 2.2 Where the judges disagreed, and the deciding argument

The real disagreement was **not** about which approach to build. Three of the four assessments converged on collapsing the eye separation. The disagreement was about **whether the owner's miss is geometric at all**:

- One assessment held that the geometry is already correct to within 32 mm and therefore the owner cannot be missing for the reason he thinks — the complaint must be perceptual (he places the reticle wrong because he cannot fuse it).
- Another held that at magnification the disparity conflict is large enough to make the sight picture unreadable, which is the same thing said from the other end.
- A third raised a live possibility that something *is* genuinely per-eye and broken: the crop's `rw > m_width` clamp at `vr.cpp:1286-1287` changes the rectangle **after** the FOV was derived from the unclamped fractions.

**The deciding argument is that the adopted fix is the only one that is simultaneously the cure and the instrument.**

With both eyes rendered from one camera, the two images are identical content. If the owner *still* sees two crosshairs, then the disagreement cannot be coming from the eye separation, and the only remaining source is the crop — which the log names in one line. Every other candidate leaves that question open. This one closes it.

Better still: **the experiment needs no build.** `CTRL+ALT+F4` already cycles `ipd_mm` through `0 / 16 / 32 / 64 / 100` (`stereo.cpp:131-141`), and the comment on that hotkey says the quiet part out loud — *"0 mm is the control: it must look exactly like mono."* Scope up, press it until the log reads `ipd -> 0 mm`, and the answer arrives in ten seconds. The whole design below is nothing more than **making that hotkey automatic, and scoped-only.**

### 2.3 Why "one camera" beats "one eye", stated plainly

The owner asked for the thing a real shooter does. The right move is to copy the **result** of closing an eye, not its **mechanism**.

| | closing an eye (real, and the one-eye candidates) | one camera (adopted) |
|---|---|---|
| optical axes | one | one |
| lateral error vs. the bullet | **32 mm**, constant | **0 mm** |
| binocular fusion | given up | kept |
| brightness | halved (summation lost) | unchanged |
| the magnified doubling of the whole surround, not just the tube | untouched | removed |
| code reaching `ClassifyViewmodelDraw` / the flicker paths | yes | **none** |

Flesh has to close an eye because the exit pupil only fits one. VR does not have that constraint, so it can do the thing a real scope physically cannot: **put both eyes on the same optical axis, and make that axis the bore.**

The sibling project already shipped this. Far Cry 2 VR's `g_adsFlatten = 1.0f` (`<your mods folder>\Far Cry 2 VR\src\fc2vr\camera_hook.cpp:120-148`) collapses the world's eye separation while aiming, defaulted to full, with the note: *"a sight held against the eye cannot be fused at any separation... down a scope you are looking at a distant magnified image where real disparity is nil anyway."* CotW has one global `ipd_mm` at `config.h:179` and no scoped variant. That is the entire gap.

---

## 3. The per-eye crop: NOT guilty, and it does not change while scoped

**Answer: `tier1_per_eye_crop` is not contributing to the problem, it stays ON while scoped, and this design proposes no change to it whatsoever.** There is nothing to reverse because nothing is being altered.

### 3.1 Why it is exonerated

`ComputeEyeCrops` (`vr.cpp:1226-1332`) cuts each eye its own rectangle **and derives the submitted FOV back from the same clamped fractions** (`vr.cpp:1273-1277`). Let `T = tanH`. Then

```
u0 = ½ + ½·tan(angleLeft)/T          angleLeft'  = atan((2u0−1)·T)
u1 = ½ + ½·tan(angleRight)/T         angleRight' = atan((2u1−1)·T)
```

An OpenXR projection layer maps the submitted `imageRect` linearly in **tangent** across `[angleLeft', angleRight']`. For `p ∈ [0,1]` across the extent, the source fraction is `u = u0 + p(u1−u0)`, so

```
tanθ(p) = (2u0−1)T + p·2(u1−u0)T = (2u − 1)·T
```

**The eye index cancels.** A given source pixel is presented at the same angle in both eyes, by construction. That is an exact off-centre sub-frustum, not an approximation, and it is what the comment at `vr.cpp:1214-1225` claims.

Two corollaries that matter here:

- **A wrong base `T` does not break it either.** If `game_fov_deg` / `tier1_fov_k_override` disagree with the real render frustum, both eyes get the same wrong `u ↔ θ` map. That is a uniform angular magnification of the whole picture, not a per-eye divergence. It cannot produce "the crosshair is somewhere else in each eye."
- **Rounding is a uniform sub-pixel shift.** `x` is rounded, `rw` truncated to even (`vr.cpp:1282-1285`), while the FOV comes from the unrounded fractions. At 3360 px and ±50°, one pixel is ~0.041°, so the worst case is a ~0.02-0.08° constant shift of one eye's whole image. It moves reticle and target *together* and cannot cause a disagreement between them.

### 3.2 The two residual risks, and the 30-second log check

There are exactly two places where the rectangle is adjusted **after** the FOV was derived, and both are per-eye:

- `vr.cpp:1286-1287` — `if (rw > m_width) rw = m_width;` shrinks the rectangle without recomputing the FOV. If this ever fires, that eye's image is angularly **stretched** relative to the other, anchored at its left edge, and the centre of the two images then maps to different angles. **This is the one mechanism that could genuinely produce the owner's symptom from the crop.** It can only fire when there is an aspect crop (`m_width < srcW`), so on the FULLVIEW presets it is inert.
- `vr.cpp:1289-1290` — `if (x + rw > srcW) x = srcW - rw;` slides the origin. Rounding can push it over by one or two pixels, i.e. ≤0.08°. Negligible, but per-eye.

**The check, from a log the owner already produces, no build required.** Grep for:

```
[tier1] eye%d crop u[...] v[...] -> WxH at (x,y); submitting ... deg  [CLAMPED-*]
```

printed once a second per eye at `vr.cpp:1320-1328`. Compare eye0 and eye1 while scoped:

- `rw` equal between the eyes, no `CLAMPED-LEFT` / `CLAMPED-RIGHT` → the crop is clean. Proceed with this design.
- `rw` **different** between eyes, or `rw` exactly equal to the aspect-crop width on one eye only → the clamp is firing. **Stop and fix that first**, because no stereo change can compensate for a per-eye angular scale error.

### 3.3 What must NOT happen to it while scoped

- **Do not let the crop follow the scoped narrowing.** The base tangent is computed, not measured, precisely so the submitted field does not shrink when you aim (`vr.cpp:1230-1244`). Following it collapsed the picture into a small square with black around it. That comment is a scar; leave it alone.
- **Do not disable `tier1_per_eye_crop` while scoped.** With mono content, per-eye crops are the *correct* case — identical rays, zero disparity, and each eye's angular placement stops being a compromise. Turning it off while scoped would take the owner's full field away exactly when he wants it and would gain nothing.

---

## 4. Settings

All new keys, with defaults. **`scope_mono = 0` is the single master off switch** — with it off, every other key below is inert and the build is bit-identical to today. This satisfies the standing rule: one switch that restores the old behaviour exactly, in the same build, not three sliders that individually add up to "off."

```ini
# --- one optical axis while scoped -------------------------------------
# While you are looking through a scope, render BOTH eyes from one camera:
# the game's own, which is the one the bullet leaves from. Both eyes then
# see the same picture from the same point, so the crosshair and the target
# stop disagreeing and the crosshair marks the bore exactly.
# 0 = off, exactly as before.
scope_mono = 0

# How much of the eye separation to give up while scoped. 1.0 = all of it
# (one camera, no depth in the scoped view). 0.5 keeps half the depth and
# half the disagreement. Only read when scope_mono = 1.
scope_mono_amount = 1.0

# Milliseconds to fade between normal separation and scoped. 0 = instant.
# The instant version pops nearby objects sideways when the scope comes up.
scope_mono_ramp_ms = 120

# Where the single viewpoint sits: 0 = centre (the bore - the only value
# with zero aiming error), 1 = your left eye, 2 = your right eye.
# Left/right are a preference for how the gun sits, NOT an aiming aid: they
# move the viewpoint 32 mm off the bullet's line. Only read when
# scope_mono = 1.
scope_eye = 0

# Flatten binoculars too. Off by default: binoculars are two lenses with no
# shot to place, and they SHOULD keep full stereo. See section 6.
scope_mono_binoculars = 0

# One log line each time the scoped state changes. Cheap, and it is the only
# way to tell "the feature did not help" from "the feature never fired."
scope_mono_log = 1
```

**Hotkey:** `CTRL+ALT+F3` toggles `scope_mono` live and logs the new state. (F4-F7 are taken by `stereo.cpp`, F8-F10 by `camera_probe.cpp`; F3 is free — verify before wiring.) This exists so the owner can A/B the feature in a single headset session without editing the ini.

**Panel:** add the rows to the existing group `"WHEN LOOKING THROUGH A SCOPE OR BINOCULARS"` at `overlay.cpp:574`, after the `weapon_3d_amount_scoped` row at `:584-589`.

**On the dominant-eye choice:** the design does **not** need one, and `scope_eye = 0` (centre) is the correct default and the one to ship. It is included because the brief asks for it and because some players want the sight to sit where their master eye is. Be honest in the panel help text: centre is the only setting with zero lateral error; left and right each cost a constant 32 mm.

---

## 5. Implementation checklist

Exact. File, anchor, change. Nothing in `cbscan.cpp` classification, nothing in `frame_hook.cpp`, nothing in the crop.

### 5.1 `src\cotwvr\config.h` — new keys

**Anchor:** the stereo block, immediately after `bool eye_swap = false;` (line 186).

```cpp
    // --- one optical axis while scoped ------------------------------------
    // MASTER OFF SWITCH. false = the build behaves exactly as before.
    bool  scope_mono            = false;
    float scope_mono_amount     = 1.0f;   // fraction of the separation given up
    int   scope_mono_ramp_ms    = 120;    // 0 = instant
    int   scope_eye             = 0;      // 0 centre (bore), 1 left, 2 right
    bool  scope_mono_binoculars = false;
    bool  scope_mono_log        = true;
```

### 5.2 `src\cotwvr\config.cpp` — parse and save

**Parse anchor:** the `else if` chain beside `ipd_mm`, `config.cpp:375-378`.

```cpp
    else if (!_stricmp(key, "scope_mono"))            c.scope_mono            = flag();
    else if (!_stricmp(key, "scope_mono_amount"))     c.scope_mono_amount     = (float)atof(value);
    else if (!_stricmp(key, "scope_mono_ramp_ms"))    c.scope_mono_ramp_ms    = num();
    else if (!_stricmp(key, "scope_eye"))             c.scope_eye             = num();
    else if (!_stricmp(key, "scope_mono_binoculars")) c.scope_mono_binoculars = flag();
    else if (!_stricmp(key, "scope_mono_log"))        c.scope_mono_log        = flag();
```

> MSVC caps `else if` chains (C1061). If this chain is near the limit, put the six keys in the earlier `if (!_stricmp(...)) { ... return; }` block that `aer_per_eye_pose` uses at `config.cpp:80-81` instead.

**Save anchor:** beside `ipd_mm` at `config.cpp:1138-1143`. Write the commented block from §4 verbatim, so a regenerated ini teaches the setting.

### 5.3 `src\cotwvr\stereo.h` — two declarations

**Anchor:** beside `ApplyEyeOffsetScaled` (`stereo.h:56`).

```cpp
// The surviving fraction of the eye separation, 1.0 normally and falling to
// (1 - scope_mono_amount) while an optic is up. Read by the eye-offset site.
float ScopeMonoScale();

// Advance the ramp. MUST be called exactly ONCE PER REAL FRAME, never once per
// eye pass: a value that changes between eye 0's build and eye 1's build hands
// the two eyes different separations, which is instant double vision and the
// project's signature "one object, two paths" failure.
void NoteScopeMonoFrame();
```

### 5.4 `src\cotwvr\stereo.cpp` — the whole feature

**(a)** `#include "cbscan.h"` for `PlayerIsScoped()`.

**(b)** File-scope state, near `g_eye` (line 16). Fixed point, not float, so the cross-thread read is a plain atomic `LONG`:

```cpp
volatile LONG g_scopeFlatQ = 0;   // 0..1000 = how MONO we are right now
volatile LONG g_scopeWasOn = 0;   // for the state-change log line
```

**(c)** The two exported functions:

```cpp
float ScopeMonoScale() { return 1.0f - (float)g_scopeFlatQ * 0.001f; }

void NoteScopeMonoFrame() {
    const Config& c = Cfg();
    float want = 0.0f;
    if (c.scope_mono && PlayerIsScoped()) {
        // Binoculars: two lenses, no shot to place. See section 6.
        want = c.scope_mono_amount;
        if (want < 0.0f) want = 0.0f;
        if (want > 1.0f) want = 1.0f;
    }
    LONG target = (LONG)(want * 1000.0f + 0.5f);
    LONG cur = g_scopeFlatQ;
    if (c.scope_mono_ramp_ms > 0) {
        // ~11 ms a frame at 90 Hz; a fixed per-frame step is enough here and
        // costs no clock read.
        const LONG step = 11000 / c.scope_mono_ramp_ms;   // per-mille per frame
        if (target > cur) cur = (cur + step > target) ? target : cur + step;
        else if (target < cur) cur = (cur - step < target) ? target : cur - step;
    } else {
        cur = target;
    }
    InterlockedExchange(&g_scopeFlatQ, cur);

    if (c.scope_mono_log) {
        const LONG on = (cur > 0) ? 1 : 0;
        if (on != g_scopeWasOn) {
            InterlockedExchange(&g_scopeWasOn, on);
            COTW_LOG("[scope] one optical axis: %s  (flatten %.2f, eye %s, ipd %.0f mm)",
                     on ? "ENGAGING" : "released", (double)cur * 0.001,
                     c.scope_eye == 1 ? "LEFT" : (c.scope_eye == 2 ? "RIGHT" : "centre/bore"),
                     Cfg().ipd_mm);
        }
    }
}
```

**(d)** The one line that is the whole fix. `stereo.cpp:86`:

```cpp
    const float half = Cfg().ipd_mm * 0.001f * 0.5f;
```

becomes

```cpp
    const float flat = 1.0f - ScopeMonoScale();          // 0 normally, 1 fully mono
    const float half = Cfg().ipd_mm * 0.001f * 0.5f * ScopeMonoScale();
    // scope_eye: put the single viewpoint AT one eye instead of at the bore.
    // This is COMMON MODE - the same value for both eyes, no CurrentRenderEye()
    // in it - which is exactly what makes it a viewpoint choice and not a new
    // disparity.
    float bias = 0.0f;
    if (Cfg().scope_eye == 1 || Cfg().scope_eye == 2) {
        bias = (Cfg().scope_eye == 1 ? -1.0f : 1.0f) *
               flat * Cfg().ipd_mm * 0.001f * 0.5f;
        if (Cfg().eye_swap) bias = -bias;
    }
```

and lines 90-92:

```cpp
    m[12] += r[0] * (half * sign + bias) * scale;
    m[13] += r[1] * (half * sign + bias) * scale;
    m[14] += r[2] * (half * sign + bias) * scale;
```

> `scale` may be negative — the viewmodel-cancel path at `camera_probe.cpp:2117` calls with `-weapon_depth`. Multiplying `bias` by `scale` too keeps the weapon's inherited-cancel consistent with whatever the camera did. That path is inert today (`weapon_stereo = 0`) but must not become a second rule.

**(e)** Advance the ramp. `StereoTick()` at `stereo.cpp:95` is called once per real frame from `render_hook.cpp:183`, inside `if (ownFrame)`. Add as the **first statement of the function**, above `++frame` and crucially above the `if (everyFrames <= 0) return;` early-out at line 144 — `stereo_log_every` can be 0 and the ramp must still advance.

```cpp
void StereoTick(int everyFrames) {
    NoteScopeMonoFrame();     // once per real frame - see stereo.h
    static uint64_t frame = 0;
```

**(f)** The hotkey, in the existing `pressed()` block beside F4-F7:

```cpp
        if (pressed(VK_F3)) {
            Cfg().scope_mono = !Cfg().scope_mono;
            COTW_LOG("[scope] F3: one optical axis while scoped %s",
                     Cfg().scope_mono ? "ON" : "OFF (as before)");
        }
```

### 5.5 `src\cotwvr\render_hook.cpp` — the gate. **Do not skip this.**

`PlayerIsScoped()` reads `g_scopedNow`, which is written in exactly one place: `cbscan.cpp:5594`, **inside `CBScanReport()`**. And `CBScanReport()` — plus the D3D hook installation that produces the mask draws in the first place — is gated on:

```cpp
    Cfg().weapon_cb_scan || Cfg().shader_list || Cfg().shader_hide_index >= 0 ||
    Cfg().weapon_screen_stereo || Cfg().weapon_hide || Cfg().weapon_pass_diag ||
    Cfg().hud_probe
```

at **`render_hook.cpp:107-110`** (hook install) and **`render_hook.cpp:171-173`** (report). It works today only because `shader_list = 1` in both the tuned and released ini. Set that purely diagnostic switch to 0 and `PlayerIsScoped()` becomes **permanently false with no message anywhere** — `weapon_3d_amount_scoped`, `aim_steady_scoped_only` and this feature all die silently.

**Required change: add `|| Cfg().scope_mono` to BOTH gates.** Both, not one: the second gate runs the latch, the first one installs the hooks that feed it. This is the same precedent the comment at `render_hook.cpp:162-170` records for `hud_probe`.

*Optional follow-up, better but larger:* move the latch block (`cbscan.cpp:5577-5595`) into a new `void CBScanFrameBoundary();` exported from `cbscan.h`, call it unconditionally from `render_hook.cpp` right after `FrameScanReport();` (line 152), and delete it from `CBScanReport()`. Do **not** leave it in both places — `g_maskSeenThisFrame` is zeroed by the latch, so a second call would clear the evidence. The hook-install gate at line 107 still needs `scope_mono` naming even after this.

### 5.6 `src\cotwvr\overlay.cpp` — panel rows

**Anchor:** after the `weapon_3d_amount_scoped` row (`overlay.cpp:584-589`), inside the existing `RK::Group` at line 574.

```cpp
        {RK::Bool, "One optical axis while scoped",
         "In real life a scope has room for one eye, which is why shooters close the other. "
         "This does the same thing the other way round: while you are looking through a "
         "scope, both eyes are shown the view from ONE camera - the game's own, which is "
         "where the bullet comes from. The crosshair then means the same thing to both eyes "
         "and marks exactly where the shot goes. The scoped view loses its depth in exchange. "
         "OFF leaves everything as it was.",
         &c.scope_mono, 0, 1, 1, nullptr, 0, "off"},

        {RK::Percent, "How much depth to give up while scoped",
         "100% is one camera and no depth at all inside the scope, which is what makes the "
         "crosshair and the target agree. Lower values keep some depth and some of the "
         "disagreement. Only used when 'One optical axis while scoped' is on.",
         &c.scope_mono_amount, 0.0f, 1.0f, 0.05f, nullptr, 0, "100%"},

        {RK::Int, "Fade time while scoping (ms)",
         "How long to blend between normal depth and the scoped view. 0 snaps, which makes "
         "close-by things pop sideways as the scope comes up.",
         &c.scope_mono_ramp_ms, 0, 400, 10, nullptr, 0, "120"},

        {RK::Int, "Which eye the scope sits at",
         "Centre is correct: it puts the view exactly on the barrel's line, so the crosshair "
         "and the bullet agree perfectly. Left or right move the view onto that eye, which "
         "feels more like a real cheek weld but puts the shot 32 mm to the side. "
         "0 = centre, 1 = left, 2 = right.",
         &c.scope_eye, 0, 2, 1, nullptr, 0, "centre"},
```

### 5.7 Files that must NOT change

- **`cbscan.cpp`** — no new rule in `ClassifyViewmodelDraw`, no draw suppressed, no change to the glass/mask routing. The glass and the mask are one disc issued twice; anything that gives them different treatment *by eye* reopens the split. This is the safety case for the whole design.
- **`vr.cpp`** — no change to `ComputeEyeCrops`, the submit path, or `tier1_per_eye_crop`.
- **`frame_hook.cpp`** — no change. Do not try to skip `ReplaySecondEye` while mono in v1 (see §8).

### 5.8 One standing warning for whoever touches the submit path next

Today both projection views carry the **same** pose: `m_eyeRenderPose[eye]` is assigned the identical `renderedFrom` (`vr.cpp:1694-1702`), and `monoPose` is the midpoint of the runtime's two eye poses (`vr.cpp:1611-1614`). So the runtime is already told "both eyes were drawn from the same point," which is exactly what this design wants — nothing to change.

**If anyone later makes those poses genuinely per-eye positional**, they must scale that separation by `ScopeMonoScale()` as well. Otherwise the runtime's reprojection will put back, in the compositor, precisely the offset the camera hook just removed — and it will look like the feature silently stopped working.

---

## 6. Binoculars

`cbscan.h:53` claims the latch covers *"a scope or a pair of binoculars."* **The measurement contradicts the header, twice:**

- `cbscan.cpp:3638-3639` — *"'Scoped' is detected from the mask stamp, and only scopes stamp one - so binoculars never triggered it and never got the scoped strength."*
- `cbscan.cpp:3810-3812` — *"the measurement found the optic drawing in frames with no 0x40 stamper at all, and binocular frames are exactly those."*
- `config.h:694` — *"Binoculars stamp no mask, so they cannot split this way."*

So `PlayerIsScoped()` is **false** while binoculars are raised, and binoculars keep full stereo for free. **That is the right behaviour and it should stay:** binoculars are two lenses with two exit pupils, no reticle and no shot to place. There is nothing to align to a bore, and flattening them would throw away the one place in the game where magnified stereo is a pure gain.

**But do not ship on a header comment and a measurement from another build.** Verify in one minute:

1. Raise binoculars.
2. Read the log line at `cbscan.cpp:6004`: `[weapon3d]   scoped right now: YES/no`.
3. Expected: **`no`** with binoculars up, **`YES`** with a rifle scope up.

If it reads `YES` with binoculars — the game changed, or a scope model stamps differently — then the latch has stopped discriminating and an explicit exemption is needed. The discriminator already exists in the measurements: **the binocular lens is 2208 indices, the scope lens is 1584** (`cbscan.cpp:2059-2060`, `:2352`, `:4754`). Latch a `g_binocularsNow` from a 2208-index lens draw the same way `g_scopedNow` is latched, and gate `NoteScopeMonoFrame()` on `!g_binocularsNow || Cfg().scope_mono_binoculars`. Do not invent a new signal; use the count that is already measured.

**Iron sights and red dots** stamp no lens mask either, so they are not covered by this design. They will show the same class of problem at 1× magnification, where it is roughly 3× smaller and near the fusion limit rather than past it. Expect that as the next report; it is a separate design.

---

## 7. The five-minute test

The test has to separate two different claims, because only the second one matters:

- **Claim A — "the reticle looks right."** The two eyes agree about where the crosshair is. Necessary, not sufficient.
- **Claim B — "the bullet lands there."** The impact is on the crosshair. This is the claim.

The design's key property: **horizontal is the only axis eye separation can move.** Bullet drop is vertical and expected. So the pass condition is about left/right, never up/down.

### Step 0 — the no-build control (30 seconds, do this first)

In game, scope up on something distant. Press `CTRL+ALT+F4` until the log reads `[stereo] F4: ipd -> 0 mm  (CONTROL: must look identical to mono)`.

- **The crosshair snaps to one place and sits still** → the eye separation is the cause, the design is right, build it.
- **You still see two crosshairs at 0 mm** → the eye separation is *not* the cause. Stop. Grep the log for `[tier1] eye0 crop` / `[tier1] eye1 crop` and compare `rw` and any `CLAMPED-` markers (§3.2). Fix the crop first; nothing in this design will help.

Press `CTRL+ALT+F4` back round to 64 mm when done.

### Step 1 — the bullet test (three minutes)

**Target:** a thin, hard-edged, *vertical* world feature at roughly 80-100 m that shows an impact mark — a fence post, a signpost, a lone dead trunk, a rock spire. **Not an animal.** An animal is too wide to prove anything and moves.

1. Set `scope_mono = 0` (today's build). Scope up, put the crosshair centre on a named mark on the post. Hold still. Fire. Note where the impact lands relative to the crosshair. Repeat three times.
2. Press `CTRL+ALT+F3` to turn `scope_mono` on. Confirm the log prints `[scope] one optical axis: ENGAGING`. Repeat the same three shots on the same mark.

**Reading it:**

| what you see | what it means |
|---|---|
| Impact directly **below** the crosshair, on the same vertical line, three shots stacked | **PASS.** Vertical offset is bullet drop and has nothing to do with this. |
| Impact repeatably **left or right** of the crosshair, and that offset **shrinks or vanishes** between step 1 and step 2 | **PASS**, and it quantifies the bug that was fixed. |
| Impact repeatably left or right by the **same** amount with `scope_mono` on and off, three shots stacked | The crosshair is not on the camera axis. This is a **different bug** and no stereo change will fix it. Suspects, in order: `weapon_view_scale = 0.500` and `weapon_view_offset_x` (set them to `1.00` and `0.00` and repeat). |
| Shots **scattered** horizontally rather than offset | You are still not able to place the crosshair — check `scope_mono_amount` really is 1.0 and that step 3 below passes. |

### Step 2 — the discriminator between "looks right" and "lands there" (one minute)

With `scope_mono = 1`, physically close your **left** eye, place the crosshair on the mark, fire. Then close your **right** eye, place, fire.

**Both impacts must land on the same vertical line.** If they do not, the two eyes are still being given different angles — which after step 0 can only be the crop, not the eye separation.

This is the step that separates the claims. "The reticle looks right" is what your eyes report. "The two eyes' shots land in the same place" is what the game reports, and it cannot be fooled by a comfortable-looking image.

### Step 3 — prove the feature fired at all (fifteen seconds)

The log must contain, while scoped:

- `[scope] one optical axis: ENGAGING (flatten 1.00, eye centre/bore, ipd 64 mm)` on the scope raise, and `released` on the lower.
- `[weapon3d]   scoped right now: YES` at least once while aimed.

If `scoped right now` reads `no` while you are scoped, the mask signal is not reaching the feature. Check §5.5 — `Cfg().scope_mono` must be named in **both** gates in `render_hook.cpp`. A feature that silently does nothing has cost this project two whole test sessions before.

---

## 8. Risks, and how each one shows up in the headset

| risk | how it looks | mitigation |
|---|---|---|
| **The scoped view goes flat.** Both eyes see one image, so there is no depth inside the scope. | Glassing a hillside looks like a photograph rather than a scene. | This is the trade, and it is a smaller loss than it sounds: at M ≈ 2.75 the depth you lose was exaggerated ~3× and wrong anyway. `scope_mono_amount = 0.5` keeps half of it for players who want it, at half the disagreement. |
| **Transition lurch.** 64 → 0 mm moves each eye's camera 32 mm sideways. | Nearby objects — grass, the gun, a branch — pop sideways as the scope comes up. Distant targets barely move (the shift is `32 mm / D`, magnified). | `scope_mono_ramp_ms = 120`, which also hides it inside the ADS raise animation. |
| **The ramp evaluated per eye pass instead of per frame.** | A brief but unmistakable **double image every single time you scope**, worse the longer the ramp. This is the project's signature "one object, two paths" failure. | `NoteScopeMonoFrame()` is called from `StereoTick()` only, which `render_hook.cpp:183` calls inside `if (ownFrame)` — once per real frame. Under full-rate the order is [eye0 build][eye1 build][advance], so both eyes read the same value. **Never call it from a draw hook or a camera hook.** |
| **Alternate-eye mode (full-rate off, or off for the session after a replay fault at `frame_hook.cpp:807-813`).** One eye is drawn per frame, so during the ramp the two eyes are one ramp-step apart. | A faint doubling for the ~11 frames of the ramp, then clean. | One step at 120 ms is ~3 mm of separation difference. Acceptable. If it reads badly, set `scope_mono_ramp_ms = 0` — the pop is the lesser evil in AER. |
| **`PlayerIsScoped()` dies silently.** It is latched inside a diagnostic (`cbscan.cpp:5594` inside `CBScanReport()`), which only runs behind the gate at `render_hook.cpp:171-173`. | The feature does nothing, forever, with no message. The player reports "it didn't help." | §5.5, both gates. Plus the `[scope] ... ENGAGING` line, which is absent exactly when this has happened. |
| **The latch is one frame behind** (deliberately — `cbscan.cpp:2165-2169`). | At the raise/lower boundary, one frame carries the wrong separation. | Invisible behind the ramp. Do not "fix" the latch to be same-frame: a strength that changes halfway through a frame tears the gun in two. |
| **Binoculars flatten** if a future build makes them stamp a mask. | Binoculars lose all depth and look like a flat zoomed photo. | `scope_mono_binoculars = 0` plus the 2208-index discriminator in §6. Verify with the `scoped right now:` line before shipping. |
| **The head-anchored scope picture is NOT fixed.** The magnified picture is a screen-space quad positioned from the camera, not the weapon (`config.h`, the `weapon_3d_skip_scene_picture` block). | Hold free-look while aimed and the picture still slides off the tube. | Out of scope here — but note it now becomes **symmetric**: both eyes see it slide the same way, so it is cosmetic rather than an aiming error. Under any one-eye design it would have become asymmetric, which is worse. |
| **The whole world goes mono while scoped**, not just the tube. | Peripheral scenery flattens too. | This is correct and is a feature, not a side effect: while scoped the engine narrows the *whole* scene, so the whole field carries `M`-magnified disparity, not only the circle. Flattening the frame fixes more of the discomfort than fixing the tube alone ever could. |
| **`weapon_3d_amount_scoped` interaction.** | None expected — it is already `0.0000`, so the weapon carries no per-eye shift while scoped. | If the owner ever raises it, the weapon regains disparity while the world has none, which is the current bug inverted. State that in the panel help: it should stay at 0 whenever `scope_mono` is on. |

### Deliberately deferred

- **Skipping `ReplaySecondEye()` while mono** and copying one render into both eye swapchains. Both eyes are identical content, so this would halve the scoped GPU cost for free. Deferred because the replay also freezes and restores the engine clock (`frame_hook.cpp:566-693`), and changing when it runs changes game timing — a much larger blast radius than one multiplication in `stereo.cpp`. Revisit only after §7 passes.
- **Blanking or dimming the non-shooting eye** (`XR_EYE_VISIBILITY_LEFT/RIGHT`, template at `vr.cpp:1813-1854`). This stays available as a **third** state, behind `scope_mono` working, and if it is ever built it must be a ~70% **dim with a 120 ms fade**, never a hard cut to black. Only worth starting if `scope_mono = 1` fails in the headset.
- **Iron sights and red dots.** No lens mask, no signal, separate design.

---

## 9. Summary

- The owner's report is **correct physics**, not a bug. A VR scope has no exit pupil, so both eyes see the reticle from their own position, and while scoped the whole scene's disparity is magnified ~2.75× while the reticle's stays at zero. Reticle and target cannot both be single.
- The answer is not to close an eye — that copies the *mechanism* of a limitation of glass. The answer is to copy its *result*: **one optical axis, and make it the bore.**
- The fix is `ScopeMonoScale()` multiplied into `half` at **`stereo.cpp:86`**, latched once per frame, gated on `PlayerIsScoped()`. It touches no draw classification, no crop, no submit path, and nothing the flicker work stabilised.
- **`tier1_per_eye_crop` is not the cause, stays on while scoped, and is not modified.** The eye index cancels out of the crop's tangent map by construction. Two per-eye clamps at `vr.cpp:1286-1290` are the only residual suspects and one log line settles them.
- **`scope_mono = 0` is the single master off switch**, default, and leaves today's build exactly as it is.
- The test that matters is **where the bullet lands, horizontally, relative to the crosshair.** Drop is vertical and irrelevant. Three shots at a fence post, with the setting off and then on, answers it in five minutes.
