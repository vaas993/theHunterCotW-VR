# Replacing the temporal pass instead of correcting it

Scope, 2026-08-11. Supersedes the approach in `TAA_PER_EYE_PLAN.md` §6 step 2b
onwards. That document's findings stand; its *method* does not.

---

## 0. Why the approach changes

Two days of work established, by measurement:

1. **Per-eye history pixels fix the ghosting.** Verified in the headset.
2. **They introduce blur and smear**, because TAA does not blend its history, it
   REPROJECTS it — and the reprojection is not per-eye.
3. **The resolve holds no camera matrix.** All 28 of its constant-buffer slots
   enumerated; the only camera among them is the current one, on `VS cb0 +0x000`.
4. **`+0x210` / `+0x260` are not the previous frame's view-projection.** Bitwise,
   0 matches out of 89 frames. `TAA_PER_EYE_PLAN.md` §4's founding sentence is
   false, and every version of the matrix half was patching the wrong bytes.

And then the decisive outside evidence. **Luma** (Filippo Tarpini,
`github.com/Filoppi/Luma-Framework`) ships DLSS/DLAA for **Just Cause 3** — same
engine family, same D3D11, open source. Its Just Cause 3 `main.cpp`:

- Names the identical TAA input layout we fingerprinted:
  `0 Source Color · 1 Previous Color · 2 Raw Motion Vectors (directly in UV space)`
- States outright: **"we don't have the proj matrix in any cbuffer in this game,
  it's only in the CPU"** — which is exactly why our searches came back empty.
- Records the engine's jitter as **two alternating offsets, `-0.25 +0.25` and
  `+0.25 -0.25`**, driven from CPU code (Luma patches machine code to redirect it).
- And, architecturally: **it does not fix the game's TAA. It replaces it.**
  `return DrawOrDispatchOverrideType::Replaced` — intercept the draw, skip it,
  run DLSS with its **own** history, copy the result back. The engine's previous-
  colour buffer is never read.

**Owning the pass makes per-eye trivial.** Two temporal contexts, one per eye —
the AC Black Flag VR pattern (`VR_PRIOR_ART_LESSONS.md` lesson 1). The cross-eye
contamination cannot occur, because the shared history is never used. There is no
ping-pong to outwit and no matrix to find.

It also collapses two goals into one: **"fix TAA" and "add DLSS/FSR 2" become the
same job**, not one before the other.

---

## 1. What already exists and carries over

Everything needed to reach the pass is built and proven:

| Piece | State |
|---|---|
| Resolve fingerprint | Unique 1-in-785 and 1-in-1225 in captures, live-verified at 3072×3320. Resolution-independent. |
| Interception point | `Hook_Draw`; the mod already skips draws (`weapon_hide`, `ShouldSkipCurrentDraw`) and substitutes SRVs around one. |
| Inputs identified | SRV0 colour, SRV1 history, SRV2 motion vectors (`R16G16_FLOAT`, UV space), SRV3 depth (`R32G8X24_TYPELESS`) |
| Output + consumers | RTV0 `R11G11B10_FLOAT`; read by 7 later draws, so the result must land in the engine's own texture |
| Per-eye textures | Allocation from the engine's own desc, seeding, `CopyResource` out — written and working |
| Eye attribution | Measured: immediate context, one thread, tags 0/1 correct on 100% of frames |
| Frame boundary | `CBScanReport`, once per real frame (pass 1 only under full-rate) |
| Falsification switches | `eye_swap` / `starve` pattern already in place |

The read-only probe (`taa_probe`) stays: it is how any new pass gets verified.

---

## 2. Stages

### Stage 1 — intercept and pass through. *Small.*

Skip the engine's resolve entirely and copy SRV0 (current colour) into RTV0, so
the downstream chain sees an image. No history, no reprojection.

Result: a sharp, aliased, ghost-free picture — and a **real TAA-off**, which the
in-game FXAA/none setting already provides, so its value here is as a *proof of
interception*, not a feature. If the picture is correct, the hard part of
plumbing is done.

Risk: low. Gate behind its own switch; the canary (weapon 3D slider) applies.

> **STAGE 1 DONE AND CONFIRMED IN THE HEADSET, 2026-08-11.** Owner: *"when
> enabled ghosting gone and the game became shimmery especially trees"* — which
> is the correct result exactly: the engine's smeared temporal pass no longer
> runs, and nothing has replaced its smoothing yet. `taa_replace_pass`, dll sha
> `27E11AD82DC2`.
>
> **It shipped broken once, and how the owner found it is worth recording.** With
> only step 1 on it did nothing; with "give each eye its own history" ALSO on it
> worked. That can only be a gate: `Hook_Draw`'s call site still read
> `taa_probe || per_eye_temporal_history` and had never had `taa_replace_pass`
> added, so the function was never called. The unrelated switch made the call
> site true, and the replace branch inside then ran correctly.
>
> **Fourth time a switch was added and one of its three gates missed** (install
> gate / call site / internal gate). Fixed structurally, not by resolve:
> `TaaWantsDraws()` is now the single expression, and all three gates call it.

### Stage 2 — our own temporal resolve, per eye. *Medium.*

A pixel shader: sample current colour, sample **our** per-eye history at
`uv + motionVector`, neighbourhood-clamp, blend, write to RTV0 and to that eye's
history. Two histories, one per eye — the whole point.

New infrastructure: the project has **no HLSL and no shader compilation** today.
Either compile at load with `D3DCompile` (d3dcompiler_47 is already shipped
alongside RenderDoc) or embed precompiled blobs.

Expected outcome: ghosting gone AND smear gone, because the history and the
reprojection now describe the same render — *except* for the motion vectors,
which is Stage 3.

### Stage 3 — motion vectors under full-rate stereo. *The real unknown.*

The engine's velocities describe "since the previous render", which under
full-rate is **the other eye**:

- eye 0's vectors point at eye 1 of the previous frame
- eye 1's vectors point at eye 0 of *this* frame

Neither points at that eye's own previous frame, which is what a per-eye history
needs. This is genuinely new ground — Luma is flat-screen, and the Black Flag
framework runs AER where each eye renders on alternate frames.

Correction is possible in principle because **we compute the stereo offset
ourselves** (`stereo.cpp`), so the difference between the two viewpoints is known.
It is depth-dependent, so it must be done per pixel — which is free once Stage 2
owns a shader that already reads depth.

Fallback if it resists: accept eye 0 (whose error is only the IPD, no time
difference) and treat eye 1 the same way, i.e. ship the smaller error. Measure
before assuming that is acceptable.

### Stage 4 — DLSS. **NGX IS UP, 2026-08-11.**

> `[dlss] *** DLSS IS AVAILABLE. ***` — NGX initialised against the game's own
> device in 1.2 s, inside the mod's process, with our D3D hooks, the OpenXR
> session and the overlay all live. `cotwvr.dll` 764,416 bytes, sha
> `B66A04F69E75`. Runtime `nvngx_dlss.dll` v310.7 deployed beside the game.
>
> SDK in `thirdparty\dlss` (headers + x64 static lib + runtime, 60 MB), taken
> from the public `github.com/NVIDIA/DLSS`.
>
> **LINK TRAP:** the SDK ships `vs2010`, `vs2012`, `vs2013` **and** a plain `x64`
> folder. Only the plain `x64` one builds against a modern toolchain -
> `vs2010/x64/nvsdk_ngx_s.lib` fails with
> `LNK2038: _MSC_VER mismatch 1600 vs 1900` plus unresolved `__iob_func`,
> `vsprintf_s` and friends, because it is built against MSVC 2010's CRT. Use
> `lib/Windows_x86_64/x64/nvsdk_ngx_s.lib` (the `_s` variant matches this
> project's `/MT`).
>
> Deliberately probe-only: init and capability query, nothing rendered. The
> failure modes it distinguishes - runtime missing, driver too old, feature
> unsupported - are the ones that would otherwise look like "DLSS just doesn't
> work".

**Remaining for a working DLSS:**

1. **Motion vectors into a texture.** The resolve shader already computes
   `prevUV`; DLSS wants `(prevUV - uv) * resolution` in an `R16G16_FLOAT` target.
   Add it as a second render target on the pass we already own (MRT), restoring
   OM state after.
2. **One feature per eye**, created at the render resolution, swapped by
   `CurrentRenderEye()` - the AC Black Flag pattern.
3. **Evaluate** with colour, depth, motion vectors, and the **jitter we can now
   read** (Stage 5) - DLAA at native resolution first, so nothing in the VR
   submit path has to change size.

### Stage 4 (original scoping, kept for context)

Same interception, better resolve. **Two contexts, one per eye**, swapped by
`CurrentRenderEye()`.

API facts that decide this:

- **DLSS (NGX) supports D3D11 natively.** Luma proves it on this engine family.
  NVIDIA-only. The capture says this machine is an **RTX 5090**.
- **FSR 2's official SDK is D3D12 and Vulkan only.** D3D11 needs the community
  port (`optiscaler/FidelityFX-FSR2-DX11`) or a DX11-on-12 interop layer. Works
  on any GPU — the right choice if the mod ships to other people.

### Stage 5 — jitter. **MEASURED 2026-08-11. THE GATE IS PASSED.**

> **The engine jitters `m[12]` of the world→clip matrix, two-phase, amplitude
> ~1.478 clip units.** Divided by a typical `w`, that is about **a quarter of a
> pixel** at 3072 wide — exactly Just Cause 3's `±0.25`. **And both eyes
> alternate under full-rate**, so there is genuine sub-pixel variation per eye.
>
> **It can be READ, not patched.** Luma had to inject 20 bytes of shellcode at a
> fixed address to get this in Just Cause 3. We do not: the jitter falls out of
> the matrices we already capture, via the **third difference**
>
> ```
> x[n] - 3x[n-1] + 3x[n-2] - x[n-3]  =  8j
> ```
>
> which annihilates any smooth motion up to quadratic — position, velocity AND
> acceleration — leaving only the alternating part. A three-frame estimator
> assumes steady speed, and head acceleration leaks straight into the answer:
> it read 0.07–0.92 where the third difference reads ~1.0–1.8.
>
> **Also corrected here:** these matrices are **not** camera-relative. `m[15]` is
> ~−8487, not 0. The whole "add the distance walked" correction was built on
> misreading that, and `taa_camera_relative` is now off.

#### Measurement traps this cost — all instrument faults, none the game's

1. **A value compared against a copy of itself.** The history roll ran before the
   report, so "now vs last frame" printed `0.0000000` every time, by construction.
2. **A diagnostic upstream of the feature.** Moving the roll to the end put it
   after the probe's early returns, so the previous matrix stopped ageing the
   moment the probe disarmed — frozen for the rest of the session.
3. **Taking the camera matrix without checking whose it is.** The engine writes
   ~90 different cameras into that block per frame. Validate against
   `MeasuredCameraTangents` — and *never* against the first sample that passed,
   which latched the baseline onto a shadow camera and then rejected every real
   one (accepted 0, rejected 227/s).
4. **Reading inside the warmup.** `full_rate_warmup_frames = 1800` is twenty
   seconds of single-eye rendering; samples from there say nothing about
   full-rate, and I wrongly concluded it was not driving. The owner corrected it.

### Stage 5 (original scoping, kept for context)

DLSS/FSR2 need the sub-pixel jitter of the current frame. This engine's is CPU-
side — Luma patches machine code at a fixed offset to redirect it, which is
version-specific and brittle.

Without jitter, an upscaler degrades to TAAU: anti-aliasing and stability, no
real super-resolution. **DLAA at native resolution is the sensible first target**
and needs no resolution change anywhere in the VR submit path.

Note for later: under full-rate stereo the engine advances its 2-entry jitter per
RENDER, so eye 0 and eye 1 likely get *different* jitters and each eye gets the
*same* jitter every frame — worth measuring, because a per-eye history with a
constant jitter resolves no extra detail.

---

## 2b. OPEN, AND THE BIGGEST THING LEFT: the head rotation is not in the matrix

Owner, 2026-08-11: with the gamepad stick, trees behave; with head movement they
displace and drag as though glued to the head. Clouds too. Measured, not judged -
the `w` row `(m[3], m[7], m[11])` is the unit view forward, so the angle between
consecutive frames IS the camera's turn that frame:

```
head only    samples 1-45    0.0000 deg   (42 of 45 exactly zero)
stick only   samples 46-60   0.47 .. 1.52 deg
```

**The camera matrix we reproject with does not rotate when the head does.** And
it is not a case of picking the wrong one of the four cameras in the block -
all four were then measured over 60 samples of head-only movement:

```
+0x000 0.0000 | +0x1D0 0.0000 | +0x210 0.0000 | +0x260 0.0000
```

None of them. The head rotation is applied downstream of this constant buffer,
consistent with this project's own earlier finding that the view basis is built
at `kViewCommit` **from angles**, not from this matrix.

**Consequence:** every reprojection is wrong by exactly the head rotation, on
every frame the head turns. It degrades BOTH paths - our own resolve and DLSS -
and it is invisible under a two-frame history while glaring under DLSS's
eight-to-sixteen.

**ATTEMPTED AND FAILED, 2026-08-12.** `taa_head_rotation_fix` added back the
frame-to-frame head DELTA as a screen-space shift, `0.5 * dAngle / tan(fov/2)`
per axis, from `HeadYawRadians`/`HeadPitchRadians` and `MeasuredCameraTangents`.
Owner tried it at +1.0, -1.0 and either side: **no improvement at any scale.**
Defaulted OFF.

**REOPENED THE SAME EVENING - the failed test was not a fair test.** Two
reasons, found by the DLSS research sweep (`DLSS_RESEARCH.md`):
1. It ran while DLSS was ALSO misregistered every frame by the unreported
   ±0.25 px jitter and missing exposure. Fixing those was "night and day"
   (owner) - easily enough noise to mask a head-delta shift.
2. Both axes shared ONE scale, while yaw and pitch reach the screen through
   independently unknown sign conventions (the head-invert flags calibrate to
   the ENGINE's fields, not UV; camera_probe.cpp warns sharing a sign "would
   mean fixing pitch by breaking yaw"). If the two need opposite signs, ±1.0
   fails on every setting - exactly what was observed.
Retry build `4E27E9575681`: separate `taa_head_rotation_scale` (yaw) and
`taa_head_rotation_scale_y` (pitch), both live in the panel. Protocol: settle
yaw first (head level, turn left-right), then pitch (nod).

**RESOLVED 2026-08-12, BY MEASUREMENT: THE MATRIX CARRIES THE HEAD ROTATION -
THE FIX WAS DOUBLE-COUNTING - AND THE REAL BUG WAS ELSEWHERE ALL ALONG.**

Two instruments settled it in one evening: NVIDIA's dev DLL overlay (the MV
dot-grid), and a centre-texel readback probe (`taa_mv_probe`) logging the MV
texture against the uploaded head shift, the matrix's per-frame turn, and the
head's per-frame turn.

1. **The real bug: the motion vectors never reached DLSS at all.** The dot
   grid sat frozen in full motion - the MV texture was ZERO. Our MRT draw ran
   with the ENGINE's blend state, and the engine's resolve only ever writes
   one target, so RT1 was silently discarded while the "written 83/83" counter
   counted binds. Fix: our own blend state (blend off, all channels, both
   targets) around the resolve draw. THIS is why `dlss_mv_object_blend` read
   as "indistinguishable", and why the 10x head-shift amplification changed
   nothing - every MV experiment before this date was run against a dead
   texture and proves nothing.
2. **With MVs alive, the probe read the truth:** head-only turns show
   `matrix turned` tracking `head turned` ~1:1 (0.48 deg head -> 0.51 deg
   matrix, every line) - the mod writes head angles into the engine's aim, so
   the engine's camera and therefore the captured matrix FOLLOW THE HEAD. The
   founding measurement of this section (0.0000 deg during head turns) does
   not reproduce and was wrong or stale. The reprojection needs no head
   correction; `taa_head_rotation_fix` was ADDING ~11 px on top of a correct
   ~11 px native component - the measured MV was 2x truth during head turns,
   which reads as exactly "shimmers with head, fine with stick".
3. So: `taa_head_rotation_fix` **defaulted OFF permanently** - off IS the
   correct reprojection. Kept only as an experiment lever. Also observed: a
   recentre/headset-don jumps yaw ~90 deg in one frame; with the fix on, that
   injected a -2201 px vector for one frame - any future use of head deltas
   must clamp against pose discontinuities.
4. The stick phase validated the conversion math end to end: matrix turn
   0.33 deg/frame -> MV +8.0 px measured, formula 0.5*d/tanH*w predicts 7.5.

So the error is NOT a simple screen-space shift by the head delta, and the model
behind that attempt is wrong rather than mistuned. **Resolve the contradiction
before trying again:**

- If the matrices were wholly blind to the head, the depth-to-world
  reconstruction would be wrong by the FULL head angle every frame, not just
  when it changes - and stick-look and walking would not reproject as cleanly
  as they demonstrably do.
- The delta model assumed the absolute error cancels between `inverse(VP_now)`
  and `VP_prev`, leaving only the change. Trying it and getting nothing at any
  scale says that framing is wrong somewhere.

Worth checking first, cheaply: whether head tracking was actually driving the
view during the 0.0000 measurement (free look held, a recentre, or the head
credit going somewhere else would all produce a still matrix honestly), and
whether the depth buffer is rendered with the head-rotated view at all.
Establish where the head rotation enters the pipeline before modelling its
effect - assuming its form is what this attempt did, and it cost a build and a
headset run.

**The original idea, still unbuilt:** compose the head rotation into the
reprojection from the angles the mod already tracks each frame. Note the apparent contradiction to
resolve first: if the matrix were wholly blind to the head, the depth-to-world
reconstruction would be wrong too and stick movement would not work as well as it
does. Establish where the head rotation enters before assuming its form -
that assumption is what cost six runs today.

## 2c. THE OBJECT SHAKE - SOLVED 2026-08-13: IT IS `weapon_3d`

**The owner found it: turning OFF `weapon_3d` eliminates the shake completely.**
Backup of the state that proves it:
`backups\2026-08-13_SHAKE-FIXED_weapon3d-is-the-cause` (dll `4E69122F1C34`).

**Mechanism.** `weapon_3d` gives the held weapon stereo depth by PATCHING CLIP
MATRICES IN CONSTANT BUFFERS (`weapon_3d_via_constants`, the glass paths, the
WVP shift). The temporal resolve captures its camera from those same uploads -
`NoteCameraCandidate` sees every 704-byte camera-block write at Hook_Unmap, and
a weapon-shifted matrix passes the tangent test because the FOV is unchanged.
So the reprojection sometimes runs on a camera displaced by the weapon offset
(`weapon_3d_amount` 0.13). Centimetres of camera error is enormous at arm's
length and small at distance - which is exactly the observed profile.

**Why it defeated a full day of hunting:** every measurement of the INPUTS was
correct, because the inputs *were* correct. The corruption entered from a
different feature entirely, and only on some frames. The eliminated list is
long and now provably irrelevant to it: 16-phase jitter (the 2-phase model was
fiction), exact jitter reporting, de-jitter, camera-candidate selection,
double-precision reprojection, head-pose smoothing and per-frame latching,
IPD/stereo offset, closest-depth dilation, the viewmodel stencil exclusion,
and the fetch-in-pixel-space correction. Several of those are genuine
improvements and are kept; none was the shake.

**Two lessons paid for in full:**
1. **Baseline before blaming the mod** - the standing rule. Vanilla was tested
   at the END of the day and came back clean in one minute; it should have been
   the first move, and would have pointed at "a mod feature" immediately.
2. **When an artefact resists every fix to subsystem A, test whether subsystem
   B is feeding A bad data.** The resolve was never wrong; something else was
   writing into what it read.

**Next (to keep BOTH features):** flag the uploads that carry the weapon shift
and skip them in `NoteCameraCandidate`, so the TAA never selects a
weapon-displaced camera. The weapon_3d code applies its shift at known hook
sites, so a thread-local "shift applied" marker around those writes is enough.
Until that lands, `weapon_3d = 0` is the shipping setting.

## 2d. THE SECOND SHAKE - HEADSET ONLY, STILL OPEN (2026-08-13 night)

**Discovered by the owner after 2c, and it is a DIFFERENT fault:** with the TAA
fix off, DLSS off and weapon_3d off - i.e. every temporal feature disabled -
**the desktop mirror is perfectly clean and the headset still vibrates.**
Turning HEAD TRACKING off stops it. It is present in both eyes individually.

Eliminated tonight, each by test:
- the compositor's positional reprojection (`submit_frozen_position`)
- the compositor's rotational reprojection (`submit_frozen_orientation` -
  froze the whole submitted pose; VR became unusable, as expected, and the
  vibration REMAINED, which exonerates timewarp entirely)
- frame queue depth (`frame_latency = 1`)
- the per-eye crop sliding (`tier1_crop_latch` - the rectangle is recomputed
  every frame from the runtime's fov; latching it is CORRECT and is kept, but
  it did not fix this)
- stale-eye alternation under full-rate (log: 28,200 frames double-rendered,
  0 stash failures)
- one-pose-per-frame (`head_latch_per_frame`), pose smoothing (`aim_steady`),
  the reseed code cave, IPD

**What the evidence now says.** The image the engine renders is clean (desktop
proves it). The compositor is not warping it wrongly (frozen pose proves it).
So the remaining candidate is that the ENGINE'S OWN head rotation is stepping:
the mod writes yaw/pitch/roll floats into the engine's camera every frame and
the engine accumulates/wraps/clamps them. A fraction of a degree of stepping
is invisible on a small desktop window and very visible in a headset, where
the frame of reference is head-locked and the image fills 100+ degrees. That
also explains why head tracking off stops it and why nothing in the render or
submit paths touched it.

**BISECTED 2026-08-13 night, and this is the important part: THE HEADSET
VIBRATION PREDATES ALL OF IT.** The owner's point was sharp - switching a
feature off does not remove the code it added, so "off" was never a baseline.
So the pre-TAA build was deployed and tested: `2026-08-10_BEFORE-TAA-PROBE`
(dll `A8ED48DCA844`, with its own ini - weapon 3D, Tier-1 FOV, HUD, scope,
full-rate stereo, and NOT ONE LINE of TAA or DLSS work). **It vibrates too.**
So nothing from the temporal work causes it; the fault lives in the older VR
machinery. It very likely felt new because the ghosting and smearing this
week's work removed were masking a fine vibration that was always there.

**THE NEXT MEASUREMENT (do this first, do not guess):** log, per frame, the
head yaw we WRITE and the engine camera's yaw READ BACK from its own field,
and print the difference. If the read-back steps while the written value moves
smoothly, the injection path is quantising and that is the fault. `apex.h` has
the field offsets (camera +0x4C yaw, +0x50 pitch, +0x54 roll) and the mod
already hooks the sites that write them.

## 3. What to do with the current build

- `per_eye_temporal_history` — keep, it is the proof the ghost is fixable, but it
  is superseded by Stage 2.
- `per_eye_temporal_matrix` — **default it to 0 and mark it disproven.** It
  patches bytes now measured not to be the previous camera.
- `taa_prev_from_last_render` — same, and it predates all of this.
- `taa_probe` — keep. It is the instrument for every stage above.

---

## 4. Discipline carried forward, paid for today

- **A search with no reference must say so, not report zero.** Three runs were
  voided by that; the probe now refuses to print a null result as a finding.
- **Gates must include the new switch in EVERY place**, not the one you edited.
- **Do not sample a quarter of the state and conclude.** Six of 23 constant-buffer
  slots produced a confident, wrong "the camera block is not bound".
- **A ring 12 entries long cannot hold a matrix a frame old** when the buffer is
  written 94 times a frame. Size the instrument to the question.
- **A probe must arm on evidence the world is rendering**, not at process start —
  an ini-persisted switch armed it on the main menu and it reported nothing.
