# DLSS in theHunter: Call of the Wild VR — what was built, and how

Written 2026-08-12. Companion to `TAA_REPLACEMENT_PLAN.md`, which covers the
temporal work DLSS sits on top of.

---

## 0. The one-paragraph version

The mod intercepts the engine's temporal resolve draw, skips it, and runs its own
pass instead. DLSS plugs into that same interception: one DLAA feature per eye,
fed the engine's colour and depth, motion vectors we compute ourselves, and
evaluated in place of our resolve. It works — NGX initialises inside the modded
process and evaluates every frame for both eyes — but it is **off by default**,
because this engine produces no motion vectors for anything that moves under its
own power, and DLSS depends on those more heavily than a short-history TAA does.

---

## 1. Prerequisites and where they came from

| Piece | Source | Lands in |
|---|---|---|
| SDK headers | `github.com/NVIDIA/DLSS` (public, no signup) | `thirdparty\dlss\include` |
| Static lib | same repo, `lib/Windows_x86_64/**x64**/nvsdk_ngx_s.lib` | `thirdparty\dlss\lib\x64` |
| Runtime | same repo, `lib/Windows_x86_64/rel/nvngx_dlss.dll` v310.7 | beside the game exe |

60 MB kept from a 270 MB download; the rest is other platforms.

### The link trap, which cost a build

The SDK ships `vs2010`, `vs2012`, `vs2013` **and a plain `x64` folder**. Only the
plain `x64` one builds against a modern toolchain. `vs2010/x64/nvsdk_ngx_s.lib`
fails with:

```
LNK2038: mismatch detected for '_MSC_VER': value '1600' doesn't match value '1900'
LNK2019: unresolved external symbol __iob_func / vsprintf_s / vswprintf_s
```

because it is built against MSVC 2010's CRT. Take
`lib/Windows_x86_64/x64/nvsdk_ngx_s.lib`. The `_s` suffix is the **static**-CRT
variant, which matches this project's `/MT`; `_d` is for `/MD` and will not link
here.

### build.bat

```
/I"%ROOT%thirdparty\dlss\include"
"%ROOT%thirdparty\dlss\lib\x64\nvsdk_ngx_s.lib"
```

plus `src\cotwvr\dlss.cpp` in the source list.

---

## 2. Initialisation — done first, deliberately

`DlssProbe(dev)` in `dlss.cpp`, called once from the frame boundary behind
`dlss_probe`. It initialises NGX against **the game's own device** and queries
capability, and does nothing else.

That order was chosen because NGX init inside a modded process is the step most
likely to fail outright — it must find the runtime, agree with the driver, and
coexist with our D3D hooks, an OpenXR session and an overlay. Everything after it
is wasted if it cannot, and the failure reasons are distinguishable:

```cpp
NVSDK_NGX_D3D11_Init(kAppId, gameFolder, dev);
NVSDK_NGX_D3D11_GetCapabilityParameters(&caps);
caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &available);
// and on failure: NeedsUpdatedDriver, MinDriverVersionMajor/Minor,
//                 FeatureInitResult
```

Result on this machine (RTX 5090):

```
[dlss] NGX initialised.
[dlss] *** DLSS IS AVAILABLE. ***
```

**API note:** this SDK's `NVSDK_NGX_D3D11_Shutdown()` is behind
`NGX_ENABLE_DEPRECATED_SHUTDOWN`. Use `NVSDK_NGX_D3D11_Shutdown1(device)` and
keep a reference to the device you initialised with.

---

## 3. The inputs, and where each comes from

At the intercepted resolve draw, the engine has already bound most of what DLSS
needs — that is the main reason this interception point was chosen.

| DLSS input | Where it comes from |
|---|---|
| Colour | the engine's, PS slot 0 at that draw |
| Depth | the engine's, PS slot 3 (`R32G8X24_TYPELESS`) |
| Motion vectors | **ours** — see below |
| Output | our own UAV texture, copied into the engine's target |
| Jitter | **not wired** — see §6 |

### Motion vectors

The engine's own vectors are unusable here: under full-rate stereo they describe
*the previous render*, which is the other eye. So the resolve shader computes its
own, and since it already works out where each pixel came from in order to fetch
history, writing them costs a second render target rather than a whole pass:

```hlsl
out float4 o0 : SV_Target0     // the resolved colour
out float2 o1 : SV_Target1     // (prevUV - uv) * resolution, in PIXELS
```

`R16G16_FLOAT` at render resolution — the same format the engine's own velocity
buffer uses. One texture, not one per eye: it is written by an eye's resolve and
consumed by that eye's evaluate immediately after, before the other eye runs.

The engine's colour target stays at slot 0 exactly as it was; ours goes beside it
at slot 1, and the render-target state is restored afterwards **including the
null slot** — leaving a stray target bound would have the engine's next draw
writing into our motion vectors.

### The output has to be a UAV

DLSS writes through an unordered-access view, and the engine's colour target is
bound `SRV | RENDER_TARGET` with no UAV — so it cannot be written directly. One
texture of ours with `BIND_UNORDERED_ACCESS`, then `CopyResource` into the
engine's target. Luma does the same for Just Cause 3 for the same reason. The
alternative — redirecting the seven downstream consumers of that target — is a
far larger surface.

---

## 4. One feature per eye

```cpp
NVSDK_NGX_Handle*    g_feature[2];
NVSDK_NGX_Parameter* g_params[2];
```

Created on first use, swapped by `CurrentRenderEye()`. This is the AC Black Flag
VR pattern (`VR_PRIOR_ART_LESSONS.md` lesson 1) — because each eye owns its own
feature it owns its own history, so the cross-eye contamination this entire
effort began with cannot occur by construction.

Create parameters:

```cpp
cp.Feature.InWidth  = cp.Feature.InTargetWidth  = w;   // DLAA: no upscaling
cp.Feature.InHeight = cp.Feature.InTargetHeight = h;
cp.Feature.InPerfQualityValue = NVSDK_NGX_PerfQuality_Value_DLAA;
cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                          NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
```

**DLAA first** so nothing in the VR submit path changes dimensions and the result
is comparable like-for-like with the mod's own resolve. `IsHDR` because the
target is `R11G11B10_FLOAT` and runs well past 1.0. A resolution change releases
**both** eyes' features, not just the one being asked for.

Evaluate:

```cpp
ep.Feature.pInColor = colour;  ep.Feature.pInOutput = ourUavTexture;
ep.pInDepth = depth;           ep.pInMotionVectors = ourMvTexture;
ep.InMVScaleX = ep.InMVScaleY = 1.0f;   // ours are already in pixels
ep.InRenderSubrectDimensions = { w, h };
```

**Fail closed everywhere.** Any failure — no feature, no output texture, evaluate
returns an error — and `DlssEvaluate` returns false, the caller issues its own
resolve exactly as before, and the log says why (rate-limited to three lines).
There is no path that produces a half-finished frame.

---

## 5. What it actually looks like

Measured in the headset, 3072×3320 per eye, full-rate stereo:

- **Standing still: excellent.** No shimmer at all. Better than the mod's resolve.
- **Moving: foliage and snow smear badly**, rocks/houses/interiors much less.

The cause is not the integration. **This engine has no motion vectors for
animated meshes.** Just Cause 3's decompiled resolve says so outright for the
same engine — *"the game has no motion vectors (not for moving/animated meshes),
nor has jitters"*. Wind-blown trees and falling snow therefore have no velocity
data anywhere: not in ours, not in the engine's.

Proof it is not a blend problem: `dlss_mv_object_blend` at 0.00 / 0.50 / 1.00 is
indistinguishable in the headset, because there is no object motion in the
engine's vectors to blend in. And trees smear from their own wind movement **even
while standing perfectly still**.

Why the mod's own resolve handles this better: its variance clamp rejects history
that does not match the neighbourhood, which is exactly what compensates for
missing motion vectors. Moving foliage shimmers a little instead of smearing.
DLSS accumulates 8–16 frames guided by vectors this engine cannot supply.

**Hence: off by default, honestly described in the panel, a situational option
for static scenery rather than a general win.**

---

## 6. Known gaps

1. **Jitter is wired to zero.** The alternation was measured — two-phase, ~1.478
   in `m[12]`, both eyes — but converting it to the sub-pixel offset in pixels
   NGX wants is unresolved: a term in `m[12]` divides by `w`, so its screen
   effect depends on depth, which a projection jitter should not.
   `dlss_jitter_x/y` exist for when the real value is known. Without jitter DLSS
   resolves no *extra* detail; it behaves as a very good temporal filter.
2. **Exposure is not supplied.** Neither an exposure texture nor the auto-exposure
   flag is set — see the next build.
3. **No per-object motion.** Engine limitation, §5. Would need velocity injected
   at the source, which means the vertex shaders that animate foliage.
4. **The head-rotation error** (`TAA_REPLACEMENT_PLAN.md` §2b) degrades DLSS more
   than the mod's own resolve, because its longer history magnifies it.

## 7. Settings

| Key | Default | What |
|---|---|---|
| `dlss_probe` | 0 | init + capability query, logs availability |
| `dlss_enable` | 0 | run DLAA instead of the mod's resolve |
| `dlss_jitter_x/y` | 0 | see §6 |
| `dlss_mv_object_blend` | 0 | measured irrelevant, §5 |
| `taa_write_motion_vectors` | 1 | write the MV target |

Panel: **STEREO → GHOSTING (TAA) → "...use DLSS instead (experimental)"**.

## 7b. PHASE A CORRECTNESS BUILD, 2026-08-12 (after the research sweep)

`DLSS_RESEARCH.md` supersedes §6's framing: the smear was mostly OUR
registration bugs, not the missing vegetation MVs. This build (dll 790,016
bytes, sha `A8101974A830`) fixes all four:

1. **Jitter is reported now** (`dlss_jitter_mode = 1`, auto). The conversion
   that stalled §6.1 is solved: the engine's jitter is an NDC offset j landing
   in the combined matrix as `m[12] += j*m[15]`, so `j_ndc = Δ/m[15]` -
   depth-INdependent on screen. Per-frame signed value via the probe's own
   third difference, magnitude locked to a running mean after 8 sane samples,
   per-axis, clamped ±0.5 px. `dlss_jitter_scale = -1` flips the sign if the
   headset says the convention is backwards. Expect ~±0.26 px on x in the log
   (`[taa] jitter told to DLSS`).
2. **`MVJittered` create flag** (`dlss_mv_jittered = 1`): our MVs are built
   from jittered matrices; DLSS now subtracts the reported offsets itself.
3. **`DepthInverted` create flag** (`dlss_depth_inverted = 1`): Apex is
   reversed-Z (Luma sets this for JC3).
4. **Reset discipline**: `InReset` on the first frame AND after any >500 ms
   evaluation gap (menu, loading) - history never bridges a discontinuity.

Plus the **render preset selector** (`dlss_preset` 0-4 = default/J/K/L/M, set
on the DLAA slot before creation). Changing preset or any create flag rebuilds
both eyes' features live (create-signature check in `EnsureFeature`).
Panel: seven new rows under GHOSTING (TAA).

> **OWNER VERDICT, SAME DAY: "night and day difference ... the smearing is
> gone and I can see the snow dropping and the trees behind it."** At default
> settings, preset untouched. The research thesis is CONFIRMED: the foliage/
> snow smear was the mod's registration bugs (jitter above all), not the
> engine's missing vegetation vectors. Remaining: slight object shift/shimmer
> with HEAD movement only (stick is clean, standing still is clean) - that is
> §2b of TAA_REPLACEMENT_PLAN.md, reopened the same day with per-axis scales
> (dll `4E27E9575681`), since the original "no improvement at any scale"
> verdict was reached under the jitter bug's masking AND with one shared sign
> for two independently-signed axes.

## 7c. THE MOTION VECTORS WERE NEVER ARRIVING - found with the dev DLL, 2026-08-12

The dev `nvngx_dlss.dll` (repo `lib/Windows_x86_64/dev/`, local
`thirdparty\dlss\dev\`) + `HKLM\...\NGXCore\ShowDlssIndicator=1` unlock input
overlays (CTRL+ALT+F12) - and the MV view was a FROZEN DOT GRID in full
motion: **the texture DLSS received was zero, always.** §3's second render
target was silently discarded because the draw ran with the ENGINE's blend
state (its resolve writes one target; whatever its state says about RT1 wins,
and D3D11 raises no error). Fix: an explicit blend state - blending off, all
channels writable, both targets - around our resolve draw.

Consequences worth engraving:
- Every MV-content experiment before this date (object blend "indistinguishable",
  head-shift amplification "no change") ran against a dead texture. Void.
- DLSS's "night and day" improvement earlier the same day came from jitter +
  exposure + preset flags ALONE, with zero MVs - which shows how much
  registration matters, and how robust the transformer model is without
  vectors when jitter is honest.
- A centre-texel readback probe (`taa_mv_probe`, three-deep staging ring -
  one staging texture starves under headset load) now logs MV vs head shift
  vs matrix turn vs head turn, and it is what proved the head-rotation story
  (see `TAA_REPLACEMENT_PLAN.md` §2b resolution): the matrix follows the
  head, the head fix double-counted, off is correct.
- RenderDoc cannot see NGX; this overlay + probe pair is the instrument set
  for this integration. Registry helpers: game folder `ENABLE_DLSS_DEBUG.reg`
  / `DISABLE_DLSS_DEBUG.reg`; release DLL kept as `nvngx_dlss.release.dll`.

## 7d. Preset A/B in the headset, 2026-08-12 — and what it points at

Owner's ranking on the residual instability (a big rock, staring then gentle
head turns), worst to best: **J → K → L → M.** Two distinct artifacts named:
TEXTURE shake (sub-pixel, on the surface) and OBJECT/EDGE shake (the geometry
itself). L eliminated the texture shake but the rock still shook; **M
eliminated the texture shake and mostly suppressed the edge shake.** Shipped
default now `dlss_preset = 4` (M).

Diagnosis this supports: the remaining error is JITTER-SIDE, not MV-side —
texture shake = the estimator's per-frame phase errors in what we report;
edge shake = the engine's 2-phase sequence itself (below DLAA's 8-phase
requirement; edges limit-cycle between two sample positions). Preset
dependence = how hard each model leans on history despite it.

**Phase B next: own the jitter.** Inject a 4-entry overlay into the camera
matrices at upload (the mod has patched all 94 camera-block writes per frame
before — machinery proven), turning 2 effective sample positions into 8, and
report the EXACT total (estimated engine part + exact injected part) per
evaluate. Gate behind its own switch; watch sun shadows/AO (Luma's warning);
fall back = `2026-08-12_DLSS-MVs-ALIVE_no-shimmer_BEST`.

## 7e. The jitter-overlay campaign, 2026-08-12 evening — approach falsified, source located by elimination

`taa_jitter_inject` added an 8-entry Halton overlay to every main-view camera
in the shared 704-byte block at upload (`InjectJitterOverlay`, Hook_Unmap),
reported exactly (estimator subtracts the known overlay; totals clamped after
summing - clamping before summing put red out-of-range dots on the dev DLL's
scatter plot, since fixed: the plot now shows a clean legal 8+ position cloud).

What the headset runs established, in order:
1. First form (`m[12] += j*m[15]`, world-scale shorthand): rock/texture shake
   largely fixed on all presets (M/L best) BUT far-tree flicker appeared, worst
   at the tree-LOD boundary (scene-complexity setting moves it), plus a
   HEADING-DEPENDENT stability (one yaw stable, others flickery).
2. The m15 stats named the flaw: TWO of the four cameras per block are
   CAMERA-RELATIVE (|m15| ~ 0) and were skipped - rendered without the overlay
   while DLSS was told it was there. 24,440 skips/s vs 12,220 blocks/s.
3. Exact form (column0 += j*column3, valid for every convention, no skips):
   flicker GONE everywhere, heading-dependence gone - and the anti-shake
   benefit gone with it.
4. The 40x amplification test settled why: a 9 px overlay produced NO
   whole-image vibration - only flicker/blur in depth-reconstructing passes.
   **The geometry raster does not consume the shared block's cameras.** It
   draws with the per-object baked WVPs (the matrices IsSharedCameraClip
   deliberately rejects), which are composed on the CPU with the engine's own
   jitter already inside. No amount of shared-block patching can change the
   raster's jitter.

**Conclusion: the 8-phase jitter must be injected at the CPU SOURCE - the code
that generates the per-frame jitter value before it is baked into per-object
WVPs, the shared block, and every pass's compensation. Luma's PatchJitters is
the template. Entry point for the hunt: the projection-build path the Tier-1
FOV work already hooks (`TIER1_FOV_PLAN.md`), and Cheat-Engine write-watches on
the shared block's source struct.** `taa_jitter_inject` stays in the build,
default OFF, as the delivery mechanism is wrong, not the idea. Interim state 1
(the accidental shake fix) is NOT shippable - its benefit was a side effect of
inconsistent matrices and came bundled with the flicker.

## 7f. The shake hunt, 2026-08-12 late - the true engine facts, one measured
##     root cause, two failed cures, and the revert

State on disk after this session: **deployed = the BEST backup dll
(`AC07273AAD3F`) + its working ini + preset M** - the "clean, almost perfect"
state. The src tree carries everything below behind opt-in switches
(defaults restored to BEST-equivalent behaviour); re-introduce ONE at a time.

**Engine facts, now measured ground truth:**
- The jitter generator is `apex::kJitterGenerator` (+0x12DFB0):
  `bool(obj, float* m16, w, h)`, reads AA mode at [obj+0x3D8], scales a table
  by [0x25AFB8C], indexes by the frame counter [0x25AFB30], divides by w/h,
  emits identity + NDC offset in row3. Found by the hunt ladder
  (`jitterhunt.cpp`: memory scan -> hwbp x3 -> disassembly; VP composer at
  +0xCF3D0, its true home [[0x2718A78]+0x5C8] matrix +0x194, pending +0x314,
  P at +0x294 with jitter row +0x2B4).
- **The hot path INLINES the generator** - a detour catches only a cold
  caller (33 calls/s vs 90 fps). `jitter_take` therefore cannot work as
  built; `jitter_mode3` is a no-op because...
- **The game runs AA mode 3 - the SIXTEEN-phase table - and always did.**
  The 2-phase model (and the third-difference estimator built on it) was
  fiction; the estimator's confident ±0.26 was averaged noise.
- **Exact jitter extraction needs no hooks**: with VP = V*(P*J) and V's
  rotation orthonormal, jx = m0*m3 + m4*m7 + m8*m11 (jy from the y column) -
  `JitterFromMatrix`, exact per eye per frame from the captured matrix.
  `StripJitter` is its inverse (col0 -= jx*col3).

**The measured root cause of the universal object shake** (rock, houses,
tables; survives DLSS on AND off; grows with history weight - the resolve's
own strength slider made it WORSE at 2.00, which is what pinned it to the
history fetch): **the captured reprojection matrix jumps by up to ~3 world
units in its translation row on 87 OF 90 FRAMES while standing perfectly
still** (per-frame probe; the 4 Hz fingerprint aliased over it). Several
same-tangent cameras (main eye, reflection cameras meters away) plus
pool-buffer rename races from deferred contexts mean the snapshot the resolve
reads is a per-frame-random member of the camera family. The resolve has
reprojected through a wandering camera since the day it was built.

**Failed cure #1 - `taa_dejitter`** (strip jitter from both matrices,
un-jittered sample position, clean MVs, MVJittered off): correct textbook
step but the wander dwarfs the jitter; shake unchanged, and in the same
builds DLSS lost its clean look ("world looks shimmery, before it was
clean") - one of the evening's changes regressed it; unvalidated, default
OFF.
**Failed cure #2 - `taa_camera_lock` v1** (reject candidates >0.25 from last
frame's camera, re-seed after 15): rejection of the ONLY candidate produced
15-frame stale cycles - trees flickered on look movement, shake not cured.
Default OFF. **The v2 design that fits the measurement: candidate SELECTION -
collect every tangent-passing camera per frame per eye at Hook_Unmap (cbscan
sees every write), and at the resolve pick the one nearest last frame's
choice.** Rejection can starve; selection cannot.

**Also in src, unvalidated:** exact-extraction reporting as the DLSS jitter
source (replaced the estimator; part of the possibly-regressing set),
`jitter_take`/`jitter_mode3` (inert for the reasons above), the mvprobe's
per-frame row3-diff stats (the instrument that caught the 87/90).

**Next session, in order:** (1) verify the restored BEST state still looks
clean; (2) build selection-v2 for the camera race and prove it with the
mvprobe at rest (MV -> ~0.0 is the pass); (3) from a clean baseline,
re-introduce exact-jitter reporting and taa_dejitter one at a time with an
in-headset A/B each; (4) only then the upscaling design work.

## 7g. 2026-08-14 - three faults closed, and the heartbeat

**The shimmer with weapon depth OFF was a gating bug, not a trade-off.**
`Hook_Map` returned early unless `weapon_3d` / `weapon_cb_scan` / `hud_probe`
was on; when it returns early `t_mapped` is never set, so **Hook_Unmap does
nothing at all** - no snapshots, no camera candidates for the resolve, no
jitter overlay. Turning weapon depth off silently cut the temporal resolve off
from its data. The TAA/DLSS switches are now in that gate, and the
UpdateSubresource snapshot path (also gated on `weapon_3d`) now runs for them
too and offers its cameras to the selection. Owner: **"the shimmer is gone
with weapon depth off"**. *Third* instance of this exact bug class in that
file - the comment above the gate already documented the first two.

**State that fixes all three known faults** (backup
`2026-08-14_CLEAN_no-shake_no-shimmer_no-vibration`):
`weapon_3d = 0` (object shake), `head_write_roll = 0` (headset vibration),
TAA + DLSS on, preset M. Remaining: weapon depth and the resolve still cannot
coexist - the weapon's shifted matrices reach the resolve's camera choice.
Owner's decisive detail: with weapon depth on, **the weapon is stable and the
WORLD shakes**, which is the contamination direction confirmed.

**The heartbeat, and a wrong first attempt worth recording.** The owner's
pulse tremors his head and the view beats with it. The first fix was a
rate-gated exponential smoother (tau 70 ms, full below 4 deg/s, none above
18). It felt floaty and it could never have worked:

- Cardiac head motion is **0.75-5 Hz - the same band as voluntary head
  motion**, so no frequency filter can separate them. At tau 70 ms only ~10%
  of a 1.2 Hz pulse is removed; 90% would need tau = 1.3 s. The "70 ms is
  enough for a pulse" comment was arithmetic borrowed from a ~10 Hz HAND
  tremor, where the same tau does remove 74%.
- Scale from shipped mods: **Halo MCC VR caps head smoothing at 4.8 ms**
  (`headset_smoothing` 0.03 default, 0.10 max), HaloCEVR applies **none** to
  the view (0.4-0.6 to the *weapon*, scaled by zoom), UEVR's camera lerp is
  **off by default** and its fastest is tau 100 ms. 70 ms was ~15x the
  strictest ceiling.
- The floatiness has a closed form: lag = `move_deg_s x frame time`, and the
  rate gate was therefore frame-rate dependent (double at 45 fps).
- **Luke Ross's `ReduceShaking = 0.6` is engine camera-shake damping, not
  tremor** (it sits next to "Camera shake detected: turning off gaze-based
  aiming"). R.E.A.L. has no head-tremor filter; its stability comes from
  `ForceMatchedEyePoses`, extrapolation/blending and a yaw prediction buffer.

**What replaced it**, both shipped and both prior-art-backed:
1. **A dead-band** (`head_tremor_level`, off/light/medium/strong =
   0/0.03/0.05/0.10 deg): pulse and looking separate by AMPLITUDE, not
   frequency. Shape from opentrack's Accela (subtractive dead-band, 0.03 deg
   default, max 0.2), smoothstep release from the Oculus prediction patent
   (US 9,063,330 - which also validates rate-gating at 1-5 deg/s and 0.3
   rad/s), a 15 ms ema inside Casiez's 10-20 ms filter budget, and a hard
   leash (0.30 deg) so lag is bounded at any frame rate. Holding costs zero
   latency, which is why this cannot feel floaty the way a filter does.
2. **Prediction dampening** (`prediction_damp_pct`): the runtime EXTRAPOLATES
   the pose to display time and extrapolation amplifies velocity, so a small
   sharp tremor arrives bigger than it is. Asking for a pose closer to now
   attacks that at source. This is OpenXR Toolkit's "over-prediction
   reduction" (ex "prediction dampening", ex "shaking reduction") in our own
   `xrLocateViews`; its published range is unverifiable (docs site dead), so
   it is a plain 0-100 dial.

**Still untested for the heartbeat:** cardiac head motion is dominantly
TRANSLATIONAL, and `submit_frozen_position` targets exactly the channel a
rotation dead-band cannot reach. It was tried once during the vibration hunt
and did not help, but that was before the roll fault was found - worth a retry.

## 7h. UPSCALING, 2026-08-14 - and why it could not live where DLAA lives

Everything above is DLAA: render size == output size, DLSS as an anti-aliaser
only. Upscaling is the other half of what DLSS is for, and it is where the
frame rate comes from - at 50% per axis the game draws a QUARTER of the pixels.

**The obvious place is the wrong place.** The DLAA path sits inside the
engine's temporal resolve (`TaaReplacePass`). Producing a larger image there is
not a matter of passing bigger numbers:

- the colour at that point is **pre-tonemap R11G11B10 HDR**, and
- **every pass after it expects render resolution** - bloom, tonemap, the
  whole post chain, and the entire UI. Changing the size there means
  redirecting all of them, which is a bigger and far more fragile change than
  the upscale itself.

**So the upscale happens at the capture instead** - `VRSystem::StashInto`, the
one place an eye's finished image is taken for the headset. There the colour is
the final backbuffer: tonemapped, 8-bit, HUD included. Three consequences, all
deliberate:

1. **No `IsHDR` flag on this feature.** Declaring HDR on an LDR image is the
   same class of mistake as declaring nothing on an HDR one, which cost this
   project a week. The upscale is therefore a **second, entirely separate
   feature set** (`g_upFeature[2]`) from the DLAA one - different sizes,
   different flags, different histories. They never share.
2. **The HUD goes through the upscaler.** DLSS has no transparency mask here,
   so HUD edges are reconstructed like anything else. Watch the compass and
   the ammo counter for wobble; if it shows, the fix is the mask input, not
   the ratio.
3. **Depth and motion vectors come from the resolve**, published by it for
   exactly this: `TaaDepthForEye`, `TaaMotionVectors`, `TaaJitterForEye`. That
   is the last moment all three are provably current for the eye being
   captured. **The resolve must be on for upscaling to work** - with it off the
   upscale declines every frame and the headset gets the plain copy.

**One subrect addresses all three inputs.** Colour, depth and vectors are all
full-frame render-resolution surfaces, and the headset is shown a centred crop
of them, so `InColorSubrectBase` / `InDepthSubrectBase` / `InMVSubrectBase` all
carry the same crop origin and `InRenderSubrectDimensions` the crop size. That
is only meaningful if the three share a grid, so `DlssUpscale` reads all three
descriptions and **declines with a named log line** if they differ - a
half-size depth buffer would otherwise present as a mystery smear rather than a
mistake.

**Sizes are per eye.** With `tier1_per_eye_crop` on, the two eyes' crops differ
by a few dozen pixels. A single shared size cache would see "the size changed"
on every eye swap and rebuild both NGX features twice a frame, at NGX's build
cost. Each eye caches its own in/out/preset triple.

**The submitted rectangle is what was WRITTEN, not what was intended.**
`m_holdW/m_holdH` are set by the capture: upscaled size when DLSS ran, render
size when it declined. `xrEndFrame`'s `imageRect.extent` reads those. Without
it, a single declined frame would submit the upscaled extent over a
render-sized image and show a magnified top-left corner - a spectacular-looking
bug out of a one-frame fallback.

**What is fixed at launch and what is live.** `dlss_upscale_pct` sizes the
OpenXR swapchains and the hold textures, so it **takes a game restart**; the
overlay row says so. `m_width/m_height` still mean the crop - every FOV, crop
and projection calculation stays in render pixels - and only the new
`m_outW/m_outH` describe what the headset is handed. Hold textures gain
`D3D11_BIND_UNORDERED_ACCESS` because NGX writes its output through a UAV,
which is also why even the mono debug path must go via a hold texture: OpenXR
swapchain images have no UAV bind.

**ONE KNOB, TWO HALVES - and the first build shipped only one of them.**
Rebuilding the eye larger buys nothing by itself: the game still draws
everything, so the frame rate is unchanged and the extra pixels are resampled
away by the compositor. Owner, immediately, with `50` set: *"but didn't see
performance uplift .. the fps should go higher"* - correct, and the design was
wrong, not the observation. Expecting the user to also pick a smaller
`render_preset` by hand, and to pick one that happens to match the percentage,
is two settings that must agree and no way to see that they do.

So `dlss_upscale_pct` now drives **both** halves, in the two files that own
them:

- `gamesettings.cpp` writes `render_preset x pct` into the game's
  `settings.json` (from `DllMain`, every launch, which is the only write that
  survives - the game rewrites that file from memory when it quits). **This is
  where the frame rate comes from.** The auto-FOV is computed from the reduced
  size, since that is the aspect the game will actually render at - the same
  ratio to within a pixel, but the number the FOV has to agree with.
- `vr.cpp` sizes the swapchains and hold textures at `render / pct`, landing
  back on the preset's size.

`3072x3320` at 58% is rendered `1782x1926` and reconstructed to `3072x3320`:
the preset stays the size the headset is HANDED, and the percentage is what the
game pays for it. The log states both halves in one line.

There is still a ceiling - **1.2x the runtime's recommended per-eye size** -
which now only matters for custom resolutions, since the output lands on the
preset by construction. Everything above recommended is resampled back down by the compositor
before it reaches the panel, and DLSS time and video memory scale with OUTPUT
pixels. Measured on this machine: render `3072x3320`, runtime recommends
`3072x3264`, `scale-of-recommended 1.00 x 1.02` - so an uncapped 50% would ask
DLSS to build **6042x6528 per eye, 39 MP an eye**, four fifths of which the
compositor immediately discards. 1.2 per axis (1.44x the pixels) keeps the usual
supersampling headroom and stops.

Both axes are scaled by the same factor when it bites: an eye whose aspect no
longer matches its crop is a stretched image, not a smaller one.

**Settings.** 100 = off (DLAA, behaviour unchanged). 67 Quality, 58 Balanced,
50 Performance; below 40 is refused - Ultra Performance is not a sensible VR
setting. The number is per axis, so 50 means a quarter of the pixels.

### First upscaling run, 2026-08-14 - it worked, and looked bad for two reasons

Owner: *"i think there was no upscaling .. and the game ran at 1536x1660 .. the
image was so low"*. The log says it DID work - `[dlss] eye 0: UPSCALE feature
1348x1498 -> 2696x2996 ... preset M` on both eyes, no declines after. Three
faults, in the order they mattered:

1. **The log lied about the swapchain.** `[vr] eye 0 swapchain 1536x1660` printed
   the FUNCTION PARAMETER, not the field the swapchain was actually made at
   (`m_outW/m_outH`, 3072x3320). A run that was upscaling read as a run that was
   not.
2. **The failure counter went quiet at three.** The first three "upscale
   unavailable" lines all landed in the menu, before the resolve had ever run -
   then the log said nothing, including nothing about the success 70 seconds
   later. Replaced with a once-a-second count of BOTH outcomes plus the reason.
3. **DLSS was running TWICE per eye per frame** - the DLAA evaluate inside the
   resolve, then the reconstruction at the capture, each with its own history.
   Two temporal accumulations stacked: double the cost, and softer than either
   alone, because accumulating an already-accumulated image is how detail is
   smoothed away. `dlss_upscale_solo` (default ON) makes the in-resolve DLAA
   step aside when the upscale is active; our own cheap shader resolve still
   runs, because that is what writes the motion vectors the upscale needs.

And a judgement, not a bug: **50% from a 3072-wide preset is 1536 - too low a
base for VR.** 1348x1498 per eye across 94 degrees is about 14 pixels per
degree before reconstruction. 67 (Quality, render 2058x2224) is the sane first
setting here; 50 is for when the frame rate matters more than the image.

**Build `30AA236B94F7`.** Owner's ini set to `dlss_upscale_pct = 67`,
`dlss_upscale_solo = 1` for the next run. Owner confirmed: **"the upscaling
works"**.

### The quality ladder, and what "live" can honestly mean, 2026-08-15

`dlss_quality`: DLAA / Quality 67 / Balanced 58 / Performance 50 / Ultra
Performance 33 / Custom (`dlss_upscale_pct`, floor lowered 40 -> 33). NVIDIA's
published per-axis ratios, so the quality value NGX keys its preset tables on
matches what we actually ask for. One function, `DlssRenderPct()`, is the only
place that answers "what percentage" - three files needed it, and any file
reading `dlss_upscale_pct` directly would silently ignore the ladder.

**The output is now the PRESET, not render / pct.** The player picks a render
resolution on the VIEW tab; that is what the headset is handed, always, and the
quality level decides only how much of it the game draws. The old formula made
the headset picture change SIZE when the quality changed, which is not what a
DLSS setting does in any game that ships one. (The preset is a full-frame size
and the input is a crop of the frame, so it is applied as a ratio against
`m_srcWidth/m_srcHeight` - never as an absolute, which would undo the crop.)

**What can and cannot be live, stated honestly.** The game reads its own
resolution once, at startup, so the mod's quality row cannot change what the
game draws until the next launch - no mod-side trick changes that. What CAN be
live is everything on our side, and that is now rebuilt on the fly:
`BuildEyeSwapchains()` was split out of `CreateSwapchains` and can re-create
both eye swapchains while running; `EnsureOutputSize()` runs once per frame
before anything is acquired and rebuilds when the numbers disagree.

That turns the GAME's own video menu into the live lever: lower the resolution
there, the game applies it live, `ResizeBuffers` lands, the output stays pinned
to the preset, and DLSS reconstructs across the bigger gap - a real comparison
of a quality level, frame rate included, with no restart. The panel row says
exactly this rather than implying the row itself is live.

**The panel tells the truth about sizes.** `FormatValue` now lets any row
supply live text (it was Action-only), so the quality row reads
`Quality 67%  -  2058x2224  ->  3072x3320` and the render-resolution row appends
what the game is actually drawing. "Performance" alone is a true answer and a
useless one; the two sizes are the answer the player can check.

**Menu rework.** The DLSS rows had grown as a flat list of `...` continuations
hanging off one TAA switch inside the STEREO tab. They are now their own
**PICTURE** tab, grouped in the order a player meets them: SMOOTHING - START
HERE (the ghosting fix, strength, sharpen, then DLSS on/off), DLSS - HOW MANY
PIXELS YOU PAY FOR (quality mode, custom %, run-once, model), DLSS FINE TUNING -
SETTLED, LEAVE ALONE (jitter mode and scale, exposure, reversed depth, vectors
carry the shake), EXPERIMENTS - OFF UNLESS ASKED (16-position, 8-position and
its amount, head-turn correction and its two signs), DIAGNOSTIC.

**Build `ED8C9B15C9DB`**, owner's ini set to `dlss_quality = 1` (Quality).

### Two panel faults the owner found immediately, 2026-08-15

**The value column was cut off mid-number.** `Quality 67%  -  2058x2224 ->
3072x...` - it was a fixed 340 px, sized once for "PRESS ANY KEY OR BUTTON".
Rows that report two resolutions need more, so the column now measures its own
text and takes up to 62% of the row; the label, which is ellipsised anyway and
short on exactly those rows, gets the rest.

**"Do I have to restart?" should not be a question the player has to ask.**
The row now says **RESTART TO APPLY** whenever the frame the game is actually
drawing differs from the frame the current quality level would have asked for -
measured against `m_srcWidth`, not remembered, so it is right after a profile
load or a hand-edited ini too, and it stops saying it the moment the game comes
back at the right size. Same indicator on the render-resolution row.

### THE HEARTBEAT FILTER IS WRONG IN PRINCIPLE - and the owner's test proved it

Owner, testing the dead-band: *"why steady out my heartbeat .. it goes worse
with higher values and off option is the best and eliminates the heartbeat
shaking .. with off setting the world stops to shake with heartbeat but the
weapon in hand does not"*.

That is not a tuning failure, it is the correct result, and it retires the
whole approach: **in VR the world looks still precisely when the view follows
the head EXACTLY.** The vestibular system is predicting the motion the head
really made; a view that matches it is perceived as a stationary world. Hold
the view still through a movement the head genuinely made and you have
manufactured a mismatch - which is exactly what reads as the world shaking. So
every level above off HAS to be worse, and off has to be perfect.

The second half of the observation names the real target. With no filtering at
all the world is stable and **the weapon still beats with the pulse**, because
the weapon is not stabilised by the vestibular match - it is drawn at a
head-derived position and any tremor in that position is visible against a
world that is not moving. That is the same split the prior art already
described and that this project cited when the filter was first built:
**HaloCEVR smooths the WEAPON (0.4-0.6, scaled by zoom) and applies NOTHING to
the view.** The filter was aimed at the one channel that must never be
filtered.

`head_tremor_level` stays in the build at 0 - it is the control that proved
this - with a help text that now says LEAVE THIS OFF and why.

### Weapon-only steadying: BUILT, TESTED, REMOVED (2026-08-15)

Owner: *"remove these last weapon setting .. they do not affect the weapon
beat"*. Reverted in full - six files restored from
`2026-08-15_QUALITY-LADDER_panel-rework_tremor-off`, which was the state
immediately before it, so the revert is exact rather than hand-unpicked.

**What it did, and why it not working is informative.** A follower over the raw
head angles published the residue (raw minus follower = the tremor), and the
weapon's clip matrix was offset by the opposite of it, through the same two
paths the eye shift uses. It ran - the gates were opened for it, the tick was
moved to the pose publish where it actually fires - and it did not change the
beat. So **the weapon's pulse is not a head-angle residue reaching the weapon's
transform.** Two candidates left, both untested: the weapon's position comes
from the game's own camera (which the mod steers, and which lags the pose by
its own amount), or it is the compositor's timewarp - which corrects the whole
layer as if it were at infinity, and therefore mis-corrects anything held at
arm's length by exactly the pose difference. The second would explain why the
world is stable and only the near object beats, and it cannot be fixed by
offsetting the weapon in NDC after the fact.

Kept for the record; anything that goes at this next should start from that
second explanation rather than re-deriving the first.

### The removed design, for reference

`WeaponSteadyTickImpl` (headtrack.cpp) runs a slow follower over the raw head
angles and publishes the RESIDUE - raw minus follower, scaled by strength -
which is the tremor itself, in radians. `WeaponViewOffsets` (cbscan.cpp) turns
that into the opposite NDC offset and adds it to the weapon's own position
offsets, so the gun sits where a hand without a pulse would have held it. The
view is not touched anywhere in the path.

Four things worth keeping:

1. **The tick had to go where the pose is published, not in
   `HeadLatchFrame()`** - which is a public function that NOTHING CALLS. The
   real per-frame path is `HeadLatchFrameImpl()` inside the pose publish.
   Shipping the tick in the dead function would have been a feature that
   silently never ran, which is this project's most-repeated failure.
2. **Real time, not frames.** dt from `GetTickCount64`, clamped to 100 ms so a
   hitch cannot snap the follower. A per-frame constant would smooth twice as
   hard at 90 fps as at 45 - the same trap the heartbeat filter's rate gate
   fell into.
3. **One function, both paths.** The body moves through its constant buffer and
   the lens moves at the rasterizer; both now read `WeaponViewOffsets()`. A
   lens left behind is the crescent bug, in a new coat.
4. **It cannot be gated on `weapon_3d`,** which the owner runs OFF (it
   contaminates the resolve's camera choice). Every weapon route was gated on
   it, so the fix would have been unreachable for the person who asked for it.
   `WeaponPoseWanted()` - true when the position, size or steadying is
   configured - now opens those gates, and `Weapon3dShiftAmount()` forces the
   per-eye shift to zero when weapon depth is off, so entering the path cannot
   smuggle in a shift nobody asked for. Default-false, so with nothing
   configured not one instruction changes.

Tuning order: `weapon_steady_gain` first (the viewmodel has a projection of its
own that cannot be read from outside, so the radians-to-NDC constant is a dial,
not a derivation), then `weapon_steady_ms`, then the amount. Signs have their
own switches per axis.

Built as `7BE5FCB8F254`; removed in `597F365DA5AF`.

### The resolution row was acting live, and must not (2026-08-15)

Owner: *"why when changing the resolution in the view tab .. there is an effect
on the real world ? it should not affect"*. Correct, and it was the upscaler's
fault. `UpscaleTarget` read the **live** panel row to decide what the headset
should be handed, so scrolling a row that plainly says TAKES EFFECT NEXT TIME
YOU START THE GAME rebuilt both eye swapchains on every nudge and changed the
picture there and then - 37 rebuilds if you scroll the list.

The target is now **stamped once**, in `ApplyGameSettings`, where the
resolution for this launch is actually written (`LaunchRenderTarget()`). The
row is inert again until the next launch, as it says it is.

The one wanted kind of live survives untouched, because it works the other way
round: the target is applied as a RATIO against the frame the game is drawing,
so when the game's own video menu changes the resolution, `m_srcWidth` moves,
the output stays where the player put it, and DLSS reconstructs across the new
gap.

**Build `597F365DA5AF`.**

Same fault, second door: **switching the quality level to DLAA mid-game also
changed the picture.** `UpscaleTarget` still had an early return on the
percentage, so DLAA (100%) fell through to "no upscaling" and the output
collapsed from the launch target to the render size. The quality level is a
statement about what the GAME will draw at the next launch and says nothing
about how big the picture handed to the headset should be, so the function no
longer reads it at all: the target comes from the launch stamp and the frame
being drawn, and the percentage survives only as the fallback for when there is
no launch target (preset "leave the game alone"). Moving the quality row is now
inert until a restart, exactly as the row says.

**Build `E88332D1BBBD`.**

**Third door, and the actual culprit: `taa.cpp` was reading the percentage
too.** With the output size finally pinned, switching to DLAA still blurred the
picture and switching back still shimmered - because `dlss_upscale_solo` asked
"are we upscaling?" as `DlssRenderPct() < 100`. Moving the quality row therefore
switched the **in-resolve DLAA pass** on and off underneath a headset-side
upscale that had not changed at all: DLAA stacked two temporal passes (blur),
going back removed one (shimmer). Neither had anything to do with the level
chosen; both were the same setting read from the wrong place.

It now asks `VRUpscalingNow()` - true when the output in force this frame is
larger than the frame being drawn - which is a fact about what is happening
rather than a statement about the next launch. The fallback percentage inside
`UpscaleTarget` is latched at swapchain creation for the same reason.

**The rule this cost three builds to learn, worth stating once: a setting that
takes effect at the next launch must be read ONLY at launch. Every place that
reads it live is a place it will half-apply mid-game.** Three such places
existed - the output size, the DLAA gate, and the fallback percentage - and each
one produced a different symptom that looked like a separate bug.

**Build `87F4090BE529`.**

### Rows that only exist sometimes, 2026-08-15

The custom percentage is the Custom slot's value and nothing else's; on Quality
or Performance it is inert, and an inert row that can still be nudged is one
that gets blamed for doing nothing. `Row` gained an optional `show()`
predicate - null on every row but this one - and `IsLabelRow()` (the one
expression the cursor, the drawing and the scroll arithmetic all consult) now
counts a hidden row alongside headings and group labels, so there is no way for
those three to disagree about what is on screen. Two details that would
otherwise have bitten:

- **The cursor can be left standing on a row that vanished** - by a change
  elsewhere, or a profile load. The draw walks it back to the nearest row that
  is still there.
- **The panel's redraw signature had to include visibility.** It hashes the
  VALUES; a row appearing or disappearing changes neither, so the list would
  not have been rebuilt and the row would have lingered until something else
  forced a redraw.

**Build `FB84C253C0FA`.**

### The DLSS switch did not come back, 2026-08-15

Owner: *"toggling off and on dlss does not bring it back .. if switching off and
on the give each eye it's view, this will bring back dlss"*. That second half is
the diagnosis, handed over complete: **"give each eye its own view" is
`tier1_per_eye_crop`, and toggling it CHANGES THE PER-EYE CROP SIZES** - which
is the one thing `EnsureUpscaleFeature` keys on, so it tears the NGX feature
down and builds a new one. Toggling DLSS never did, because the sizes either
side of the toggle are identical.

Meanwhile the output textures the upscaler writes into ARE destroyed and remade
on that toggle (the swapchain rebuild). So the feature survived, carrying
history accumulated against textures that no longer existed - the only
explanation consistent with the other half of the evidence, which is that every
counter said it was still running (`upscale eye 0: 90 ran, 0 declined`).
`DlssUpscaleInvalidate()` now drops both features whenever the output size
changes, at the cost of one soft frame.

**And a second reporting bug of the exact kind already recorded here**: the hold
textures logged `m_width/m_height` (what the game renders) rather than the size
they were made at, so the log read `hold textures created (2058x2224)` while
they were 3072x3320. That sent this investigation down a false trail for a
while. Both size logs now print what was actually made, next to what the game
draws.

### Panel rework, 2026-08-15

Every tab passed over: settings given names that say what they are rather than
what they do to you (`Stereo mode` -> `Rendering mode`, `Smoother head motion`
-> `Submit the rendered viewpoint`, `Hold steady while aiming` -> `Aim
stabilisation`, `Hide one HUD piece` -> `Isolate one HUD element`), groups
renamed to the thing they configure (`HOW MANY PIXELS` -> `RESOLUTION`, `WHO
LOOKS UP AND DOWN` -> `PITCH`, `IF IT GOES THE WRONG WAY` -> `AXIS DIRECTION`),
and every tab that has rows nobody should be moving now ends with **ADVANCED -
DO NOT TOUCH UNLESS NECESSARY**, with EXPERIMENTAL and DIAGNOSTICS below that
where they exist.

**The two dependencies are now stated where they are needed, not just here.**
DLSS needs the per-eye anti-aliasing switch on (that pass is where it is handed
depth, motion vectors and jitter) AND the GAME's own anti-aliasing on (the whole
mechanism is taking over the engine's temporal resolve - with the game's AA off
there is no such pass to take over). Both are spelled out on the two rows
concerned, and the log's "NOTHING was replaced" line now names the game's AA
setting as the first thing to check.

**Build `70A5327229A5`.**

### Presets J and K went shimmery in motion - the jitter was under-reported

Owner: *"using DLSS M or L preset is fine, but switching to K or J the game will
be shimmery and blurry; if standing still it's clean"*.

**Cause: the upscale was told a jitter about 12% too small.** The jitter lives
in the camera matrix as an NDC offset, and NDC spans the WHOLE render target -
so converting it to pixels means multiplying by the FULL width. `StashInto` was
passing the CROP width (1806 of 2058 here), because that is what it had to hand
for the subrect. The result is still a legal sub-pixel value, so nothing
rejected it and nothing logged it.

The in-resolve DLAA path never had this - it asks with the resolve target's own
size, which IS the full frame - which is exactly why the fault only ever
appeared with upscaling on.

**Why it looked like a preset problem.** A wrong jitter means DLSS accumulates
samples at positions that do not match where the frame was actually drawn. At
REST the error is a fixed sub-pixel bias and the history still converges, so the
picture is clean - the owner's "standing still it's clean" is the tell. In
MOTION the mismatch changes every frame and the accumulation smears. The models
differ in how hard they lean on the reported jitter: NVIDIA's own header says K
is "best image quality at a higher performance cost" and the default for
DLAA/Balanced/Quality, while L and M are the defaults for Ultra Performance and
Performance - the low-input-resolution models, built to be robust when their
inputs are poor. So L and M tolerated a bad jitter and J and K did not. **The
preset ranking was measuring OUR error, not the models.**

**Second fault found while there:** the preset hint is set per PerfQuality
level, and only Quality/Balanced/Performance were being set - so on DLAA or
Ultra Performance the preset row did nothing at all, silently. All five levels
are set now.

**Build `231A318AB9C1`.** Worth re-running the J/K/L/M comparison after this: the
earlier ranking was taken with the bias present, and every preset may now behave
differently.

### It was NOT the jitter - and the elimination has a shape

Tested by the owner, all with preset K:

| Suspect | Result |
|---|---|
| crop-width jitter (fixed above) | no change |
| `dlss_jitter_exact = 1` (exact instead of the disproven estimator) | no change |
| `head_latch_per_frame = 1` (one pose per frame) | no change |
| 6DoF off | no change - **6DoF is innocent** |
| **full-rate stereo instead of AER** | **no change - history AGE is innocent** |

Full-rate was the important one: both eyes then come from the same instant, so
"the history is 22 ms old under AER" cannot be the explanation, and neither can
anything else about how stale the previous image is.

**What that leaves is written in this project's own source, in `taa.cpp`, above
the head-shift constant:**

> "Measured: neither +0x000 nor any of the block's other three cameras rotates
> when the head does - **0.0000 deg across 60 samples of head-only movement**,
> against 0.47..1.52 for the stick. The head is applied downstream of this
> buffer, at the view commit, from angles."

**The camera matrices the resolve reprojects through do not contain the head
rotation at all.** The stick's rotation is in them; the head's is applied later.
The reprojection is `inverse(VP_now) * VP_prev`, so when BOTH matrices lack the
head equally the error cancels - and it fails by exactly the amount the head
rotation CHANGED between the two frames. Which is:

- zero standing still,
- zero with the stick,
- proportional to head speed when looking around,
- and preset-dependent, because it is a history-alignment error and the models
  differ in how hard they lean on history.

That is the owner's symptom, item for item, including "the trees move a little
with my head movement".

**And the compensation for it already exists and is OFF**:
`taa_head_rotation_fix` adds that delta to the history lookup, with a per-axis
scale each. It was defaulted off after §2b concluded the matrix DOES follow the
head - a probe that measured **a different matrix** (the lever at the view
commit, which does) from the one the resolve captures (the shared block, which
does not). Both measurements are real; they disagree because they looked at
different things.

**Tested: it is not that either.** One sign made it worse, the other returned it
to where it started - neutral at best. The head-rotation compensation stays off.

### THE OWNER FOUND IT: the per-eye crop

*"i found the culprit .. cut each eye it own view .. disabling it stopped the
shimmering"* - `tier1_per_eye_crop`.

**Why that is the answer, and why it is preset-dependent.** DLSS accumulates
into a history keyed to the RECTANGLE it is given. The per-eye crop is that
rectangle: it is computed per eye from the runtime's own per-eye frustum, and
`ComputeEyeCrops` says of it, in the source, *"it is submitted as a
sub-rectangle of a full-size swapchain, so it may change size freely from frame
to frame."* Free to change is exactly what an accumulator cannot tolerate - move
or resize the rectangle and every pixel of the new frame lands somewhere else in
the history. Standing still that is one static misregistration; the moment
anything moves it is a shimmer, and the models that lean hardest on history
(K, J) show it while the robust ones (L, M) absorb it.

**Two causes, both closed:**

1. **An odd crop ORIGIN.** The size had always been rounded even; the origin
   never was. For a plain copy that is harmless - for a reconstruction it puts
   the eye half a texel off the lattice its samples were drawn on, and the two
   eyes, landing on different parities, disagree with each other as well. Origin
   is now even too.
2. **The rectangle moving under the accumulation.** `tier1_crop_latch` guards
   the crop, but nothing guarded what DLSS was HANDED. `dlss_upscale_rect_latch`
   (default on) latches the reconstruction rectangle per eye for as long as the
   render size holds. The player's crop may still evolve - it is submitted as an
   image rect, which costs nothing - while DLSS keeps a fixed frame of
   reference.

**Build `A10A4BFB71FF`.** The test that settles it: turn "cut each eye its own
view" back ON with preset K. If it stays clean, both features work together for
the first time.

**And the lesson, which is the owner's not mine:** five suspects were eliminated
by measurement from inside the mod (jitter scale, jitter source, pose latch,
6DoF, history age, head-rotation compensation) and none of them was it. The
answer came from switching one FEATURE off at a time in the panel. When
component-level instrumentation keeps coming back clean, stop instrumenting
components and start bisecting features.

### The rectangle latch was NOT the mechanism - result, 2026-08-15

Tested with the crop back on: **double vision gone, shimmer back.** So the
rectangle moving frame-to-frame was not what the per-eye crop was doing to DLSS,
and the two fixes shipped for it are worth exactly what they are:

- the **even crop origin** is correct regardless and stays (a reconstruction
  lattice should not sit half a texel off the grid its samples were drawn on);
- the **rectangle latch** costs nothing, is right in principle, and demonstrably
  does not cure this. It stays on but is no longer claimed as the fix.

**What is actually known now, and it is a clean statement:**

| per-eye crop | preset | result |
|---|---|---|
| off | any, incl. K/J | clean |
| on | M or L | clean |
| on | K or J | shimmer on head movement |

Both features work. They do not work TOGETHER on the two history-hungry models.
Since M and L are NVIDIA's own defaults for Performance and Ultra Performance -
the models built to stay stable when their inputs are imperfect - and since the
picture on them is what the owner has repeatedly called clean, **crop ON with M
or L is the configuration to ship**: the full vertical field AND a clean image.

**What has NOT been explained**, and should be stated rather than papered over:
why a different sub-rectangle per eye upsets K and J specifically. Everything
measurable about that rectangle has been checked - it is even, it is stable, it
carries its own frustum, its size and origin are within the frame, and the
jitter, depth and motion vectors are all addressed through the same subrect.
The next real step is not another switch: it is to capture a frame with the dev
DLSS overlay on (CTRL+ALT+F12) under crop+K and read what NGX itself says about
its inputs, which is the one instrument this project has not yet pointed at this
question.

## 8. Shipping note

`nvngx_dlss.dll` is 59 MB and must sit beside the game executable. That is a real
addition to any release download, and NVIDIA-only — decide whether it is bundled
or fetched before shipping.
