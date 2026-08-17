# Per-eye TAA history — capture analysis and implementation plan

Status: analysis complete, **no mod source edited**. Read-only on the captures.
Date: 2026-08-09.

Captures used (opened `rb` / via headless `renderdoccmd convert` only; mtimes unchanged):

- `%USERPROFILE%\AppData\Local\theHunterCotWVR\captures\cotw_frame28218.rdc` (2.41 GiB)
- `%USERPROFILE%\AppData\Local\theHunterCotWVR\captures\cotw_frame56340.rdc` (2.78 GiB)

Derived data: `<your capture analysis folder>\` (XML exports + `f28218.json` / `f56340.json` timeline indexes).

---

## 0. Bottom line

1. **The TAA resolve pass is found, in both captures, and is uniquely identifiable
   by resource topology alone — no shader hash, no guessed state.** One draw out
   of 785 (and one out of 1225) matches. The fingerprint is resolution-independent:
   it was validated at 2880×3112 *and* 4130×2124.
2. **The history is a texture, not a matrix.** It is a two-texture ping-pong,
   `res9817` ↔ `res9821`, full-res `R11G11B10_FLOAT`, with **no `CopyResource`
   between them** — the engine alternates which one it binds.
3. ~~**There is exactly ONE history pair**, so both eyes necessarily share it.~~
   **SUPERSEDED BY MEASUREMENT, 2026-08-10 — see §6 step 1's result.** There is
   one pair, but the eyes do not *share* it, they **cross-feed** it: eye 0 reads
   the texture eye 1 wrote last frame, eye 1 reads the texture eye 0 wrote this
   frame. Neither eye ever sees itself, and the two are contaminated
   *differently*. The fix is the same; the artefact is asymmetric.
4. **The old `taa_prev_from_last_render` result eliminates nothing** — and now
   there is a code-level reason, not a suspicion. See §4. Do not treat that line
   of enquiry as closed by those two headset runs.
5. **The captures are MONO.** They cannot directly show two eye passes. §3 states
   exactly what they can and cannot settle, and §8 says what capture would.

---

## 1. The TAA pass, exactly as captured

### frame 28218 — draw #746, chunkIndex 105269, `Draw(VertexCount=3)`, viewport 2880×3112 at (0,0)

| Role | Resource | Size | Format |
|---|---|---|---|
| **RTV0 (written)** | `res9817` | 2880×3112 | `R11G11B10_FLOAT` |
| PS SRV0 | `res622` | 2880×3112 | `R11G11B10_FLOAT` — current scene colour |
| **PS SRV1** | `res9821` | 2880×3112 | `R11G11B10_FLOAT` — **HISTORY** |
| PS SRV2 | `res9330` | 2880×3112 | `R16G16_FLOAT` — motion vectors |
| PS SRV3 | `res619` | 2880×3112 | `R32G8X24_TYPELESS` — depth |
| DSV | *none bound* | | |

### frame 56340 — draw #1186, chunkIndex 110517, `Draw(VertexCount=3)`, viewport 4130×2124 at (0,0)

Identical topology: out `res904223`, colour `res904062`, **history `res904227`**,
motion vectors `res904195`, depth `res904059`, no DSV.

Both history textures:

```
bind  = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET
usage = D3D11_USAGE_DEFAULT     mips=1   arr=1   samples=1
```

### The ping-pong is real, and it is not a copy

Role histogram for the pair in frame 28218:

| | RTV binds | SRV binds | UAV binds | copy DST | copy SRC |
|---|---|---|---|---|---|
| `res9817` (written this frame) | 1 | 3 | 0 | 0 | 0 |
| `res9821` (history) | **0** | 1 | 0 | **0** | 0 |

`res9821` is never written and never copied into during the frame, yet it is read.
Its content can only have come from a previous frame. Next frame the two swap
roles. **A per-eye fix therefore has to control which texture the resolve reads —
there is no copy to intercept.**

### The bind is a clean single-slot call

```
ci=105261   PSSetShaderResources(StartSlot=1, NumViews=1, {srv9823 -> res9821})
ci=105269   Draw(3, 0)
```

Eight chunks apart, one slot, one view. This is the easiest possible substitution
target: save slot 1, set ours, draw, restore.

### Which context, which thread

The resolve is issued on **`Context 7`, the IMMEDIATE context** (proven: `Context 7`
is the one that calls `ExecuteCommandList`, which only an immediate context can),
on **thread 6560**. It is *not* recorded into a deferred command list — the frame's
only two `ExecuteCommandList` calls are at ci 105678 and 105727, both *after* the
resolve.

This matters a great deal: it means the eye tag at this draw is read on the
render thread, not on one of the deferred recording workers (thread 27200 records
69,363 of the frame's 105,741 chunks). The `cbscan.cpp:3201` "two questions, two
moments" hazard does **not** apply to this pass the way it applied to the scope glass.

### Downstream consumers of the resolved output

`res9817` is read by 7 later draws (five 320×180 probes, one 1440×1556 bloom/DOF
entry, one full-res composite into `res629`). **This is why the plan below
substitutes the READ and leaves the RTV alone** — redirecting the write would
require redirecting all seven, and "a fallback for one path and not the other"
is precisely the shape of the crescent bug.

---

## 2. The identification rule to implement

Validated as **unique** — 1 match / 785 draws and 1 match / 1225 draws — and
**self-referential**, so it needs no knowledge of the render resolution:

```
call is Draw with VertexCount == 3
exactly one RTV bound, no DSV bound
RTV0 resource: format R11G11B10_FLOAT, dims W×H
viewport == (0, 0, W, H)                     (exact match to RTV0 dims)
PS SRV0 resource: W×H, R11G11B10_FLOAT
PS SRV1 resource: W×H, R11G11B10_FLOAT, and NOT the same resource as RTV0
PS SRV2 resource: W×H, R16G16_FLOAT
```

**Discrimination margin** (frame 28218 funnel — draws surviving each added clause):

```
  192   one full-res RTV
   39   + RTV is R11G11B10_FLOAT
    7   + SRV0 full-res R11G11B10_FLOAT
    1   + SRV1 full-res R11G11B10_FLOAT      <-- already unique here
    1   + SRV1 != RTV0
    1   + SRV2 full-res R16G16_FLOAT
    1   + viewport == render res
```

Uniqueness is reached at the SRV1 clause; SRV2, the viewport, the vertex count
and the no-DSV clause are all **margin**. Keep them — they cost nothing and they
make the rule fail loudly rather than drift onto a neighbouring pass after a game
patch. Frame 56340's funnel is the same shape (274 → 42 → 7 → 1).

Note this rule is *description of resources*, not a learned pointer and not a
guessed state fingerprint — the discipline `THE_FLICKER_POSTMORTEM.md:139-152`
and `:186-190` demand.

---

## 3. What the captures CANNOT tell you — stated plainly

**Both captures are single-view (mono).** One `Present`, one G-buffer pass, every
viewport at x=0,y=0 covering the whole target. `tools\capture.ps1` says why:
RenderDoc and the VR hooks cannot coexist, so a capture session runs the flat
renderer.

Therefore the captures **cannot** directly answer:

- **"Is the same texture used by both eye passes?"** There is only one eye pass
  in them. The answer below is an *inference*, and it is labelled as one.
- **Whether the engine's ping-pong parity flips between eye 0's pass and eye 1's
  replay.** This changes the *shape* of the contamination (see §5) though not the fix.
- The TAA blend weight, the jitter sequence, or anything in a constant buffer —
  `renderdoccmd convert -c xml` omits buffer contents.

**The inference, and its basis.** Full-rate stereo replays `BUILD + SUBMIT +
PRESENT-PATH` for the second eye (`frame_hook.cpp:467-473`, and `:51` — "Post-processing
+ resolve to the backbuffer + present"). The resolve lives inside the present path.
Nothing in `ReplaySecondEye` allocates or rebinds textures; it re-runs engine code
against engine resources. The engine has exactly one history pair. **So under
full-rate the resolve runs twice per `Present` against one shared pair, and each
eye's history contains the other eye.** That is consistent with the capture and
with the source, but it has not been *observed*, and §6 step 1 is the cheap
observation that would make it a fact.

---

## 4. Re-reading the failed `taa_prev_from_last_render` attempt

Two headset runs, "changed nothing". The brief lists four candidate explanations.
Evidence now available:

**Candidate 4 — TAA was not enabled — is ELIMINATED.**
`GraphicsAA already 3` appears in the logs; per `launcher\cotwvr_launcher.py:428-429`,
3 = "FXAA + TAA". TAA was on. The capture agrees — the resolve pass is present
and running.

**Candidate 3 — eye attribution taken on the wrong thread — is much WEAKER than feared.**
`SetRenderEye(1)` at `frame_hook.cpp:596` wraps the *entire* second-eye replay and
`SetRenderEye(0)` at `:805` closes it, both on the present-path thread. And §1
shows the resolve is issued on the immediate context, not on a deferred worker.
The window is coarse enough to cover the pass. (It is *not* eliminated — the old
patch ran in `Hook_Unmap`, which the deferred workers do hit; see candidate 2.)

**Candidate 1 — the instrument was never read — is the strongest surviving one,
and it is now a concrete finding rather than a suspicion.**

The counter is logged at `cbscan.cpp:5559-5564`, but that block is **nested inside
`if (Cfg().tier1_measure_fov)`** (`cbscan.cpp:5522`) *and* inside a once-per-second
gate. More importantly:

> **Searched all 113 mod logs in `%LOCALAPPDATA%\theHunterCotWVR\`. There is not a
> single `[taa]` line in any of them.** 80 of those logs contain
> `[fullrate] engaging`. The mod does not echo `taa_prev_from_last_render` at
> startup either, so no log records whether the feature was ever switched on.

So there is **no surviving evidence that `PatchPreviousViewProjection` ever executed
its `memcpy` even once.** By the postmortem's own rule — *a null result from an
unverified counter eliminates nothing* — those two headset sessions did not test
the hypothesis they were believed to test. This is the same failure mode that
voided four eliminations before.

**Candidate 2 — delivery gap — remains open and is checkable.**
`PatchPreviousViewProjection` is called only from `Hook_Unmap` (`cbscan.cpp:5267`).
`Hook_UpdateSubresource` snapshots constant buffers but does not patch. The frame
contains **438 `UpdateSubresource` calls on the immediate context** — so if the
resolve's copy of the camera block arrives by that route, the patch never touched
the bytes that mattered.

**None of this changes the conclusion that outranks all four:** history is pixels.
A previous-view-projection matrix only re-aims *where* last frame's sample is
fetched from; it cannot repair a texture whose contents are the other eye,
rendered from a camera 64 mm away with different disocclusion and different
view-dependent shading. Re-aiming the fetch relocates the ghost at best. Build on
the texture.

---

## 5. Every temporal history in the frame

The generalised ping-pong detector (renderable + read + never written + never a
copy destination, this frame) found **four full-resolution histories, and the same
four in both captures**:

| 28218 | 56340 | Format | Read at | What it is |
|---|---|---|---|---|
| `res9821` | `res904227` | `R11G11B10_FLOAT` | 1 draw (#746 / #1186) | **TAA colour history** — the target |
| `res2983` | `res904343` | `R11G11B10_FLOAT` | 1 draw (#664 / #1113) | second temporal pass, colour half |
| `res2978` | `res904338` | `R16_FLOAT` | same draw | second temporal pass, other half |
| `res685` | `res904125` | `R32_FLOAT` | 1 draw (#663 / #1112) | depth history |

Each is read at **exactly one draw**, and each has an identically-described
sibling written the same frame (`res9817`, `res2993`+`res2988`, `res682`). That
one-draw scope is what makes "fail closed" cheap: there is exactly one bind to
substitute per history, or none.

Smaller read-only-history textures exist too (128×128 and 32×32
`R16G16B16A16_FLOAT`, bound at 80–112 draws each — likely probe/adaptation data).
**Leave them alone for now.** They are bound across the whole G-buffer pass, so
their substitution surface is two orders of magnitude larger, and none of them is
the thing that produces a ghost trail. Widening before the narrow case is proven
is the mistake `THE_FLICKER_POSTMORTEM.md:110-122` records.

Also, so it is not mistaken for a history: **`res622` → `res2522` is copied 4×
per frame** (full-res `R11G11B10_FLOAT`). That is the scene-colour copy for
refraction reads. It is not TAA history.

**Memory cost of duplication at 2880×3112** (8,962,560 px):

| Format | Per texture | Per eye pair (2 textures) |
|---|---|---|
| `R11G11B10_FLOAT` | 34.19 MB | 68.4 MB |
| `R32_FLOAT` | 34.19 MB | 68.4 MB |
| `R16_FLOAT` | 17.09 MB | 34.2 MB |

TAA colour alone: **68.4 MB** of extra VRAM. All four: ~171 MB.

---

## 6. Implementation plan

### Step 0 — the cheap answers, before any code (owner, minutes)

These are one launcher row or one hotkey each, and the prior art says they work
(`VR_PRIOR_ART_LESSONS.md:45-57`).

- **`GraphicsAA` 3 → 1 (FXAA only).** Lesson 1's own honest fallback is *"per-eye
  history, or TAA off in AER mode"*. If this removes the artefact, that is the
  answer *and* it confirms the diagnosis for free. No byte-patch needed here —
  it is a settings value.
- **IPD 0 mm (CTRL+ALT+F4 to the 0 mm step, `stereo.cpp:131-141`).** If the
  shimmer survives at 0 mm, both eyes are the same camera, cross-eye contamination
  cannot be the cause, and this whole line closes in one minute.
- **Eliminate pacing** (lesson 3) before blaming history: uneven pacing under AER
  makes one eye go stale and reads as flicker with no temporal pass involved.

### *** READ THIS BEFORE STEP 1 - learned 2026-08-10, the hard way ***

**Do not put a diagnostic in the draw hooks casually, and never one that maps a
resource.** A HUD probe added to all four draw hooks in `cbscan.cpp` silently
killed the weapon 3D shift and the weapon FOV - features that had worked for
days. Every added line was gated on config that was OFF, and it broke anyway.
Removing them restored it; re-adding only the non-diagnostic half kept it
working. Full account in `REGRESSION_WEAPON_3D.md`.

The suspect is `HudProbeQuad`, which did a `CopyResource` into a staging buffer
and a `Map(D3D11_MAP_READ)` on the context it was handed. **These hooks run on
DEFERRED contexts** - `cbscan.cpp` says so in its own notes, and `g_diDeferred`
counts them - and a read-map on a deferred context is not valid D3D11. Its own
output proved it: NaNs and absurd magnitudes, one sane row in 400.

So for the TAA probe:

* **Read STATE and DESCRIPTIONS only** - `OMGetRenderTargets`, `PSGetShaderResources`,
  `GetDesc`, pointer values. Never `Map`, never `CopyResource`, never a readback.
* **Release every COM pointer on every path.** The §2 fingerprint needs an RTV
  and three SRVs; that is eight `Release()` calls to get right, and a leak in a
  per-draw hook kills the game within minutes.
* **Test the weapon 3D strength slider after the probe build, before trusting any
  number it prints.** It is the canary: it exercises the same hooks and it fails
  visibly and immediately. Two minutes, and it would have caught this one the
  day it was introduced.
* Prefer doing the work in `Present` where possible - one call per frame, on the
  immediate context, where a readback would actually be legal.

### Step 1 — the probe. One logging build. Zero behaviour change. **Do this before writing the feature.**

> **BUILT 2026-08-10.** `src\cotwvr\taa.cpp` + `taa.h`, `cotwvr.dll` 652,288 bytes,
> sha256 `60BCEB176A55…`, deployed and byte-identical across `build\`, the game
> folder and `release\theHunterCotW-VR-v1.0\`.
>
> - Config: `taa_probe` (default 0), `taa_probe_seconds` (default 15, self-disarms).
>   `kKeysUnderstood` 227 → 229.
> - Panel: **STEREO → IF SOMETHING LOOKS WRONG → "Find the smearing (diagnostic)"**.
> - Call sites: **two lines**. `TaaOnDraw` at the top of `Hook_Draw` only — the
>   resolve is a `Draw(3)`, so the other three draw hooks cannot ever see it, and
>   after the HUD-probe regression the surface is deliberately a quarter of what
>   that one took. `TaaOnFrame` next to `HudProbeReport()` in `CBScanReport`.
> - `taa_probe` is in the `render_hook.cpp` install gate, so it cannot depend on
>   an unrelated switch being on.
> - **Counted per REAL FRAME, not per Present.** `CBScanReport` runs inside
>   `if (ownFrame)`, which under full-rate is pass 1 only, so one window spans
>   both eye passes. Getting this backwards would have made the correct answer
>   (2) read as the model-is-wrong answer (1).
> - Read-only: `OMGetRenderTargets`, `PSGetShaderResources`, `RSGetViewports`,
>   `GetDesc`, `QueryInterface`. No `Map`, no `CopyResource`, no readback. Every
>   COM pointer released by RAII on all ten exits; stored resource pointers are
>   identity tokens and are never dereferenced.
> - It also records **PS slot 3 (depth)**, which is not part of the rule. It is
>   free at that point and FSR 2 / DLSS both need the depth resource, so the same
>   run that answers the TAA question also inventories the upscaler inputs.

`taa_probe = 1`, default 0. In `Hook_Draw`, evaluate the §2 fingerprint and log
**once per Present**:

- **How many times the fingerprint matched this Present.** Expect **2** under
  full-rate, **1** under AER. *If it is 1 under full-rate, the model in §3 is
  wrong and nothing below should be built.*
- Per match: `CurrentRenderEye()`, `GetCurrentThreadId()`, context pointer,
  immediate-vs-deferred, RTV0 resource pointer, **SRV1 resource pointer**,
  viewport, and `GetDesc` of RTV0 and SRV1 (w, h, format, mips, usage, bind).
- **A rejection funnel, in the §2 clause order**, with the counts reconciling to
  total draws — copy the shape of `g_bindExit[7]` / `kBindExitName`
  (`cbscan.cpp:3054-3063`). A fingerprint that stops matching after a game patch
  must say so, not go quiet.

#### RESULT — run 2026-08-10, ~1350 frames at 90 fps, FULLVIEW 3072×3320

Every one-second window for fifteen seconds, without a single exception:

| Question | Answer |
|---|---|
| Resolves per real frame under full-rate | **2** (85–90 of every ~90 frames; ~1% of frames carry a third) |
| Eye tags on the pair | **{0, 1}**, always |
| Same history texture? | **NO — different, every frame** |
| Same output target? | **NO — different, every frame** |
| Context / thread | **immediate context, one thread, both eyes** |
| Frame-to-frame history pointer | **never changes** |

Measured pointers, with the two engine textures called A and B:

```
eye 0:   reads B (…A52E0)    writes A (…A4520)
eye 1:   reads A (…A4520)    writes B (…A52E0)
```

That fully determines it — **a strict cross-feed, not a shared buffer**:

```
eye 0's history = what eye 1 wrote LAST frame    (other eye + a frame of motion)
eye 1's history = what eye 0 wrote THIS frame    (other eye, same instant)
```

Two consequences the plan did not anticipate:

- **The artefact is asymmetric.** Eye 1 gets a clean 64 mm parallax double; eye 0
  gets parallax *plus* a frame of motion. Confirmable by closing one eye.
- **Each eye already owns a fixed output texture** (eye 0 always resolves into A,
  eye 1 into B). That looks like a free fix — just swap which one each eye reads
  — and it is not reachable: eye 0's own previous image lives in A, and A is what
  it overwrites in the same draw. One texture cannot be read and written by one
  draw. So the private-texture-plus-copy of step 2 stands.

Also settled in passing: the resolve is on the **immediate context on one
thread**, so `SetRenderEye`'s window covers it and the §4b keying question is now
a measurement rather than an argument. And the funnel's rejection counts
reconcile to ~184 `Draw()` calls per real frame — 92 per eye pass against the
capture's 94 — so the probe is looking at the frame the capture describes.

**The three numbers this yields are the whole point:**

1. Does the resolve run **twice** per Present under full-rate?
2. Do the two runs carry eye tags **0 and 1**? — this directly settles the
   attribution question the old patch failed on.
3. **Do the two runs read the SAME `SRV1` pointer, or do they alternate?** This
   is the crux the mono capture cannot answer. Same pointer ⇒ each eye blends
   against the other, exactly as theorised. Alternating ⇒ parity flips between
   eyes and one eye is self-consistent while the other is not — which would show
   up as an *asymmetric* artefact, and is worth knowing because the owner can
   confirm it by closing one eye.

Either outcome is fixed by the same duplication. The log just says which.

### Step 2 — the feature: substitute the read, copy out the write

> **BUILT 2026-08-10**, on the step-1 result above. `cotwvr.dll` 659,456 bytes,
> sha256 `8521327C65C7…`, deployed and identical across `build\`, the game folder
> and `release\`. **Not yet judged in the headset.**
>
> - Keys: `per_eye_temporal_history` (master off switch, 0),
>   `per_eye_temporal_eye_swap` (0), `per_eye_temporal_starve` (0).
>   `kKeysUnderstood` 229 → 232.
> - Panel: **STEREO → IF SOMETHING LOOKS WRONG → "Give each eye its own
>   history"**, with the two test switches indented under it.
> - Allocation from the engine's own `D3D11_TEXTURE2D_DESC`, verbatim, on first
>   match; both seeded with a `CopyResource` from the live history so no eye
>   starts black. Re-created only on a desc change, **from the drawing thread**,
>   so allocation and release never cross threads. Turning the feature off stops
>   the substitution but does not free the textures — freeing from the frame
>   boundary would be a race for no benefit.
> - Substitution wraps the `orig()` call in `Hook_Draw` and nothing else:
>   save slot 1 → bind ours → draw → **exact** restore (including the null case)
>   → `CopyResource` this eye's resolved image into its own texture.
> - Fail closed: no textures, no device, nothing in the history slot → the whole
>   frame runs stock. Never one eye substituted and the other not.
> - Reports once a second whenever it is on, whether or not the probe is armed:
>   substitutions per eye, fail-closed exits by reason, and a loud line if the
>   two eye counts differ or if either test switch is on.

#### RESULT — headset, 2026-08-10

> **"it does fix the ghosting but it makes the world blurry and smearing and
> shaky a little."**
>
> The ghost is gone, so the pixel half is right. The blur is the missing half,
> and it was predictable: TAA does not merely mix the history in, it
> **reprojects** it — every sample is fetched from where that pixel sat in the
> render the history came from, using the matrix at `+0x210` / `+0x260`, of
> which the engine keeps exactly **one**, set from the last render. Substituting
> pixels without it leaves the two halves describing different renders:
>
> | | history pixels now are | matrix still describes | error |
> |---|---|---|---|
> | eye 0 | eye 0, one frame ago | eye 1, one frame ago | the IPD |
> | eye 1 | eye 1, one frame ago | eye 0, **this** frame | **a whole frame of motion** |
>
> A sample fetched from the wrong place is blur when still and smear when
> moving — and it should be markedly worse in eye 1, which is checkable by
> closing one eye.

### Step 2b — the reprojection, per eye. **BUILT 2026-08-10**

`cotwvr.dll` 662,016 bytes, sha256 `ABB6A8F7C20A…`, deployed and synced.

`per_eye_temporal_matrix` (default **1**, only acts while
`per_eye_temporal_history` is on). It is routing, not mathematics: keep each
eye's camera matrix, roll it into that eye's previous-frame slot **once per real
frame** in `CBScanReport`, and hand each eye back its own. Pixels and
reprojection then describe the same render.

This reuses `PatchPreviousViewProjection`, which already had the block located
and the structural `IsSharedCameraClip` guard. The old `taa_prev_from_last_render`
handed back the *other* eye's matrix — correct when the history was the other
eye, exactly wrong now — so it is ignored while the per-eye route is on.

It reports per second: blocks patched per eye per frame, how many arrived on a
deferred context, and **how far the write actually moved the reprojection**. Two
zeros there mean the matrix written was the one already present and the picture
cannot have changed — the null-result trap §4 records, closed in advance.

**Reverting to the pixels-only build needs no rebuild:**
`per_eye_temporal_matrix = 0` reproduces it exactly.

#### RESULT — 2026-08-11. It lands, and it lands in the wrong place.

Owner: *"same as before … still blurry and smeary and shaky … the re-aim does
not fix that, it only sharpens and adds detail to some objects."* And with the
whole feature **off**: *"no smearing"*, *"no motion blur"* — so both artefacts
are caused by the substitution, not pre-existing.

The instrument says the patch is not the problem:

```
eye 0 patched 94.0 of 94.0 camera blocks, eye 1 94.0 of 94.0   (0.0 deferred)
it MOVED the reprojection by 40 … 1640 world units
```

Every block, both eyes, immediate context, and the write genuinely changes the
data. **So the delivery is perfect and the target is wrong.** The next
measurement said so outright — the constant buffers bound to the resolve draw:

```
PS cb0  1584 bytes    PS cb1  384    PS cb2  192    PS cb3  160    PS cb4  64
*** no 704-byte block bound to this draw ***
```

**The TAA resolve never reads the shared 704-byte camera block.** The matrix
patched at Unmap cannot reach it however many blocks it patches — and "it only
sharpened *some objects*" is exactly what a patch landing on the velocity pass
and nothing else looks like.

Two things this also settles, both worth keeping:

- Blur that grows with distance and vanishes up close is a **reprojection**
  error, not a blending one: the history sample is located by reconstructing a
  world position from depth, so a small matrix error throws distant points a long
  way and near ones hardly at all.
- The owner's *"left eye is less smooth than the right"* is present with the
  feature **off**, so it is NOT this. Under full-rate the left eye is rendered
  first, then the whole second pass runs, then both are submitted against one
  pose — the left eye's pixels are a full pass older at submission. Its own
  problem, its own fix; the ghosting was masking it.

### Step 2c — find the resolve's OWN reprojection. **BUILT 2026-08-11**

`cotwvr.dll` 666,112 bytes, sha256 `70BC698E6031…`.

`CBScanResolveConstants` searches each of those five buffers for a 4x4 this file
already tracks — this eye's current matrix, this eye's previous frame, and the
other eye's of each — **straight and transposed** (HLSL is column-major by
default and engines transpose on upload; not checking would produce a confident
"not found" for a matrix sitting right there). Anything unrecognised that passes
the structural clip-matrix test is logged too, with its `a`, `b` and near plane.

It reuses the existing snapshot table, so `taa_probe` was added to the snapshot
gates in both `Hook_Unmap` and `Hook_UpdateSubresource` — without that the
resolve's buffers are simply not in the table and every answer would be "no
snapshot".

Runs at most once a second while the probe is armed. A **"PREVIOUS frame"** hit
names the matrix the per-eye fix has to write; a **"CURRENT"** hit names its
neighbour.

#### RESULT — 2026-08-11, and the first reading of it was VOID

Fifteen passes, all five buffers, every one "nothing recognised". **That result
meant nothing**, and the reason is the exact trap §4 of this document is about:
the reference matrices `g_eyeVPCur` / `g_eyeVPPrev` were only collected inside
the *feature's* branch of `PatchPreviousViewProjection`, and that run had the
probe armed with the feature **off**. So every candidate was compared against
four null pointers. *A search with no reference cannot fail to find nothing.*
Fixed: the matrices are now tracked whenever `taa_probe` **or** the feature is
on, and only the patching still requires the feature.

**What IS valid from that run:** the independent structural test
(`IsSharedCameraClip`, which needs no reference) found **no clip-like matrix
anywhere in those five buffers**. So the resolve almost certainly reprojects from
the **motion-vector texture**, not from a matrix — which relocates the whole
problem to the pass that WRITES those vectors.

### Step 2d — sweep every constant buffer in the frame. **BUILT 2026-08-11**

`cotwvr.dll` 667,136 bytes, sha256 `1E5620907B4E…`.

If the resolve carries no matrix, the thing to correct is whichever camera the
**velocity pass** used. Rather than hunt that pass draw by draw — which would
mean putting a probe back into the busy draw hooks, the surface that cost a day
already — the sweep walks the existing snapshot table: 512 slots that already
hold every constant buffer the engine wrote this frame. A camera matrix is
recognisable on sight, straight or transposed.

**A buffer holding the OTHER eye's CURRENT matrix is the engine's "previous
render" camera.** That is what the velocity vectors are computed against, and
therefore what the per-eye fix has to replace.

### Step 2e — REFUTED: +0x210 is not the previous frame. 2026-08-11

Two things had to be found before this could be asked properly, and both were
instrument faults of mine:

1. **The resolve reads no matrix at all.** All 28 of its constant-buffer slots
   were enumerated: `PS cb0..cb4` (1584/384/192/160/64) and `VS cb0` (704) plus
   `VS cb1..3` (96) and `VS cb12` (208). The only camera among them is the
   CURRENT one, at `VS cb0 +0x000`. It reprojects from the motion-vector texture.
   The earlier "no 704-byte block bound to this draw" was wrong — the probe read
   six PIXEL slots out of twenty-three bindings, and the camera block is on the
   VERTEX shader.
2. **The velocity pass is a single full-screen `Draw(3)`**, two RTVs, no depth,
   second target full-res `R16G16_FLOAT` — not a per-object G-buffer pass. Its
   `VS cb0` is the same 704-byte block and carries **four** cameras:

   | offset | what the scan says |
   |---|---|
   | `+0x000` | the CURRENT camera, matched bitwise |
   | `+0x1D0` | a camera, `a=0.9005 b=0.8332 near=0.01000` |
   | `+0x210` | a camera, `a=0.9005 b=0.8332 near=**0.01240**` |
   | `+0x260` | a camera, `a=0.9005 b=0.8332 near=0.01000` |

Then the apples-to-apples test — at the velocity draw, remember that block's
`+0x000`; one frame later ask whether the three are bitwise equal to it:

```
velocity pass seen 89/90 times (eye 0 / eye 1)
  +0x1D0 : 0 / 0
  +0x210 : 0 / 0
  +0x260 : 0 / 0
```

**Zero, on every frame.** So `+0x1D0` / `+0x210` / `+0x260` are **not** the
previous frame's view-projection, and §4's founding sentence — which every
version of the matrix half was built on, and which nobody had ever tested — is
false. `taa_prev_from_last_render` and `per_eye_temporal_matrix` were both
writing bytes that are not the reprojection. That is why the headset saw the
re-aim "only sharpen some objects" and never fix the smear.

Same field of view, three different near planes, all present alongside the
current camera. Whatever they are, they are not a frame-delayed copy of it.

**Still open:** where the velocity pass gets the previous camera. It may be
derived in the shader from something else in the block, or live in a buffer this
mod has not seen written. Step 2f (built, sha `1FB211648012`) widens the sample
ring from 12 entries to 384 — about four frames, where twelve covered a fraction
of one — and asks whether those three offsets match **any** main-view matrix
seen in that window, at any point in the frame. If that is also zero, the
previous camera is not a copy of anything observable at the Unmap boundary and
the substitute-a-matrix route is finished.

`per_eye_temporal_history = 0` (default off, restores today's behaviour exactly).

**Allocation.** On first match, `GetDesc` the SRV1 resource and create two private
textures from **that desc, unmodified** — never a hand-written desc (the
format-family rule, `ARCHITECTURE.md:277-279`). Seed both with one
`CopyResource` from the engine's current history so the first frame per eye is
not black. Re-create if the desc ever changes (resolution change) and go stock
until they are ready.

**At the matched draw**, with `E = CurrentRenderEye()`:

```
if (!ready[E] || !enabled)            -> do nothing at all, run stock
PSGetShaderResources(1, 1, &saved)    // AddRefs
PSSetShaderResources(1, 1, &ourSRV[E])
orig(ctx, 3, 0)
PSSetShaderResources(1, 1, &saved)    // exact restore
if (saved) saved->Release()
CopyResource(ourTex[E], rtv0Resource) // this eye keeps its own resolved image
```

**Why substitute the read and not the write.** The engine's output texture still
receives the resolve, so all seven downstream consumers (§1) see exactly what
they always saw. Redirecting the RTV would mean redirecting those seven too, and
one resource reached by a substituted path and a stock path is the crescent bug
by another door (`THE_FLICKER_POSTMORTEM.md:56-58`).

**Fail closed, both halves together.** If the substitution cannot be made — no
private texture, desc mismatch, `PSGetShaderResources` failure, eye tag not in
{0,1} — leave the **whole frame** stock. Never one eye substituted and the other
not; that is worse than the bug.

**Cost.** One `CopyResource` of 34.19 MB per eye per frame = 2 per frame under
full-rate ≈ 137 MB of copy traffic per frame, ~12.3 GB/s at 90 Hz. A few percent
of a modern card's bandwidth. Acceptable for a correct first implementation; §7
notes the zero-copy variant if it ever shows in frametimes.

**Keying — the §4b principle.** The state must be keyed to *the render that
produced the pixels*, not to "which eye are we on now". Here those coincide,
because the resolve is issued on the immediate context inside the present path
that `SetRenderEye` wraps (§1, §4) — but step 1's log is what turns that from an
argument into a measurement. Do not skip it.

### Step 3 — only then, the other three histories

Generalise to a small table: `{ fingerprint, ourTex[2], enabled }`, one row per
history from §5, each behind **its own** switch
(`per_eye_temporal_depth`, `per_eye_temporal_second`). Ship them **one at a time**.
Do not enable all four in the build that first proves the colour history.

### Step 4 — switches and diagnostics

Per the master-off-switch rule, in the same build:

| Key | Default | Effect |
|---|---|---|
| `per_eye_temporal_history` | 0 | **The master off switch.** 0 = today's behaviour, exactly. |
| `taa_probe` | 0 | Step 1's logging. Read-only `Get*` calls, no behaviour change. |
| `per_eye_temporal_eye_swap` | 0 | Deliberately feed the **wrong** eye's history. |
| `per_eye_temporal_starve` | 0 | Bind the *current colour* as history — should visibly kill all temporal accumulation. |

The last two are the controls the postmortem insists on: a switch that can only
express "better" cannot test anything. If turning the feature on changes nothing
**but `eye_swap` or `starve` visibly changes the image**, the plumbing is proven
live and the hypothesis is genuinely refuted. If *none* of the three changes
anything, the substitution is not reaching the shader and the null result again
means nothing — which is exactly the trap the last attempt fell into.

Report once per second: matches per Present, substitutions applied per eye,
fail-closed exits by reason.

### Step 5 — build constraints

- New config keys go in `config.cpp` as **standalone**
  `if (!_stricmp(key, "...")) { ...; return; }` blocks near the other standalone
  ones — the else-if chain is at MSVC's C1061 limit.
- `kKeysUnderstood` is currently **202** (`config.cpp:1228`). Bump by exactly the
  number of keys added. Trust the build's error message.
- Source is CRLF. Use the Edit tool for exact edits.
- Deploy only with `theHunterCotW_F.exe` not running; verify by SHA256. **Do not
  launch the game.**

---

## 7. Alternatives considered and rejected (for now)

- **Redirect the RTV as well (true zero-copy, the closest FSR analogue).** Needs
  the seven downstream consumers redirected too. More surface, more fail-closed
  paths, same visual result. Revisit only if the copy shows up in frametimes.
- **Copy-swap around the pass** (`CopyResource` in *and* out). Needs no knowledge
  of which slot holds the history — but costs two full-res copies per eye instead
  of one, and the capture already gave us the slot with certainty, so the extra
  copy buys nothing.
- **Patching the ping-pong parity so each eye gets its own pair.** Requires
  finding and trusting an engine-internal counter. Guessing engine state is the
  thing this project has paid for twice.

---

## 8. Confidence

**High confidence (directly observed, twice, at two resolutions):**

- The TAA resolve pass exists, its exact resources, formats, sizes and slots.
- The history is a two-texture ping-pong with no copy between the textures.
- The identification fingerprint is unique and resolution-independent.
- The history SRV is set by a clean single-slot `PSSetShaderResources(1,1,…)`.
- The pass runs on the immediate context, not a deferred command list.
- Four full-res temporal histories exist, each read at exactly one draw.
- `GraphicsAA = 3`, so TAA was genuinely enabled.
- No `[taa]` counter line exists in any of 113 mod logs.

**Inference, not observation (labelled as such):**

- That both eyes share the one history pair under full-rate stereo. This follows
  from the source — the replay re-runs engine code over engine resources and
  allocates nothing — and from the capture showing exactly one pair. It is
  strongly supported and it is *not* the same as having seen it.

**Not answerable from these captures:**

- Whether the ping-pong parity flips between the two eye passes.
- The TAA blend weight and jitter sequence (buffer contents are omitted by the
  XML conversion path).

**Therefore: do not build §6 step 2 before §6 step 1 has been run.** The single
most likely reason the last attempt "changed nothing, twice" is that its
instrument was never read — and building a second, larger feature on the same
unverified footing would repeat the exact mistake, at greater cost.

### The capture that would settle the remaining questions

A mono RenderDoc capture cannot show two eye passes, and `capture.ps1` says the
VR hooks and RenderDoc cannot coexist. So:

1. **Preferred, and far cheaper: the step-1 log.** It answers all three open
   questions (twice per Present? eye tags 0/1? same SRV1 pointer?) from a live VR
   session with no capture at all, and with zero behaviour change. Run it in the
   headset for ten seconds and read four lines.
2. **If a capture is still wanted**, it must be taken *with the VR hooks live* —
   which today means making them coexist, not swapping one for the other. The
   value would be seeing the two resolves in one frame with their bindings. That
   is a tooling project in its own right and it is **not** on the critical path;
   item 1 gets the same three answers today.
3. **For the blend weight or jitter** (only needed if per-eye history lands and
   still ghosts), use the replay API — `qrenderdoc.exe --python <script>` against
   an existing `.rdc`, modelled on the ~80 scripts already in `tools\rdc_*.py`.
   That path *can* read shader disassembly and constant-buffer values, which
   `renderdoccmd convert` cannot.
