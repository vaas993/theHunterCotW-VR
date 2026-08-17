# DLSS research — everything found, and the plan to make it right

Research sweep 2026-08-12, five parallel passes: the official Programming Guide
(v310.5.0, 84 pages, read in full), Luma-Framework's Just Cause 3 source (same
engine, the only DLSS prior art on Apex), the VR DLSS mod ecosystem (PureDark,
Luke Ross, UEVR, MSFS/DCS), OptiScaler's source (the catalog of common
integration bugs), and Apex-engine material. Companion to
`DLSS_IMPLEMENTATION.md` (what was built) and `TAA_REPLACEMENT_PLAN.md`.

Local reference copies: `thirdparty\dlss\doc\DLSS_Programming_Guide_Release.pdf`
(+ extracted text `dlss_guide.txt`).

---

## 0. The verdict in one paragraph

Our DLSS is not failing because the engine lacks vegetation motion vectors —
**Luma ships DLSS on this exact engine with zero foliage/skinned MVs and calls
the result "fine (a bit smudged)"**. Ours looks far worse than that because
three registration inputs are wrong or missing: we report **zero jitter while
the frame is actually jittered ±0.25 px** (a direct spec violation — guide
3.7.3 rule 3), our **motion vectors contain that jitter** and we neither cancel
it nor declare it (`MVJittered`), and until the pending fix we supplied **no
exposure** for an HDR input (guide 3.9 lists "ghosting of moving objects" as
symptom #1 of exactly that). On top sit two cheap wins we never touched: the
**DepthInverted flag** (Apex is reversed-Z; JC3 sets it — wrong flag corrupts
DLSS's closest-depth MV dilation, i.e. moving edges) and the **render preset**
(310.x transformer presets; MSFS's identical high-contrast VR smear was largely
fixed by preset choice alone). Fix registration first, then tune presets, and
only then judge whether vegetation velocity still needs real work.

---

## 1. What the Programming Guide says we got wrong (v310.5.0, read in full)

### 1a. Jitter is REQUIRED — and misreported jitter is worse than none

- Hard requirement (§2.2): "at least 16 jitter phases (32 or more preferred)"
  for DLSS generally; the phase formula (§3.7.1.1) is
  `phases = 8 × (target/render)²` → **minimum 8 phases for DLAA**. Sequence:
  **Halton** (what the network was trained on).
- Where it goes (§3.7.2): `M[2][0] += jx; M[2][1] += jy;` — exactly the
  `m[12]/m[13]` slot our engine already perturbs (two-phase, ±0.25 px, measured
  via the third difference — `TAA_REPLACEMENT_PLAN.md` Stage 5).
- The reporting contract (§3.7.3): offsets are **pixels at render resolution**,
  in **[-0.5, +0.5]**, in the **same axis convention as motion vectors**
  (origin top-left, Y down — so a +Y clip-space jitter is a −Y pixel offset),
  and — the rule we violate — they represent the jitter applied to the
  projection **"irrespective of whether the offsets were also applied to the
  motion vectors or not"**. Zero may only be passed when no jitter was applied.
  Ours IS jittered; we pass zero. Every other frame is misregistered by ~0.5 px
  at the accumulator.
- `MVJittered` create flag (§3.6.2): set when the MV texture *includes* jitter —
  DLSS then subtracts the per-frame offsets itself. Our MVs are computed from
  jittered matrices, so today they do include jitter deltas.
- Zero/wrong-jitter symptoms (§3.7.4, §8.1): shimmer, "screen door", fuzz on
  fine static detail, degraded reconstruction — stacking with our smear.

### 1b. Motion vectors — convention CORRECT, foliage gap is the documented worst case

- Direction: pixel + MV = that pixel's position in the **previous** frame;
  units = **pixels at render resolution**, RG16F, origin top-left. Our
  `(prevUV − uv) × resolution`, `MVScale = 1.0`, `MVLowRes` is exactly right,
  and render-res MVs need **no manual dilation** ("DLSS dilates them
  internally" — preferred path).
- §8.1 names our symptom: "Common places to miss out motion vectors include
  **animated foliage, the sky**"; §3.15 adds snow/dust particles. There is no
  sanctioned "no-MV fallback value".
- **Bias-current-color mask is DEAD**: §3.15 (310.4.0+ note) — "has not been
  introduced for use with latest models and **should not be used**; only Preset
  F supports this buffer." Transparency/particle/animated-texture masks are in
  the research-only block. So masks are NOT the foliage answer on transformer
  presets — contrary to what older community lore says.

### 1c. Exposure — our bug, verbatim

§3.9: for HDR, DLSS **needs** the frame's exposure ("the value which when
multiplied to the input brings middle gray to expected level";
`MidGray/(AvgLuma×(1−MidGray))`, MidGray ≈ 0.18), as a **1×1 2D texture**
(R16F+, first channel). Missing/wrong exposure symptoms: "1. **Ghosting of
moving objects** … 3. **Aliasing especially of moving objects**. 4. Exposure
lag." AutoExposure flag (creation-time, ~0.02 ms) is the sanctioned fallback.
**Note: the exposure texture is only honored by presets J and K; preset L
always auto-exposes.**

### 1d. The rest

- **DepthInverted** (§3.8): DLSS assumes near=0/far=1; set the flag for
  reversed-Z. **Luma sets it for JC3** — Apex is reversed-Z. We never set it.
  Wrong flag corrupts closest-depth dilation → moving-edge quality.
- **InReset** (§3.13): first frame after a cut/teleport ONLY (never per-eye,
  never per-frame). Luma also resets on "frame didn't draw the 3D scene"
  (menus/loading) and on SR toggle / output change.
- **Sharpness deprecated**; tonemapper-type / frame-time-delta / 3D-MVs /
  particle masks are research-only. Ignore.
- **Mip bias** (§3.5): DLAA sanctioned range (−1, 0]. Luma uses −1 at native;
  PureDark uses 0 at DLAA. Optional sharpness experiment, not correctness.
- **VR (§3.17)**: "DLSS natively supports … VR by creating multiple DLSS
  instances … one per view." Our one-feature-per-eye full-rate design is the
  documented pattern. Never evaluate ONE feature twice per frame.
- **D3D11**: NGX is not thread-safe; immediate context only (Luma asserts it);
  on D3D11 NGX **preserves** context state (unlike DX12/VK) — Luma still
  saves/restores everything. Output must be UAV-capable (ours is);
  wrong bind flags ⇒ "output may be black without further indication".

---

## 2. What Luma does on this engine (JC3, full source read)

- **Jitter**: patches the exe (20 stolen bytes at the jitter writer), replaces
  the game's 2-phase ±0.25 with **8-phase Halton(2,3) in [−0.5,0.5] px**, fed
  per frame from the CPU; DLSS gets the same pixel value raw. When SR is off it
  writes the vanilla two-phase back.
- **MVs**: rewrote the game's MV pass; camera MVs from depth reprojection
  (`uvNonJittered − prevUV`), **current-frame jitter subtracted in the shader**
  (`MVJittered=false`); removing the *previous* frame's jitter was tried and
  reverted — "works fine without". Dynamic rigid objects (vehicles) decoded
  from the engine's velocity buffer with a ×0.125 scale + depth threshold;
  `MVScale = −render_res` (their MVs are curr−prev in UV; the sign flip makes
  them prev-pointing pixels — equivalent to our convention).
- **Foliage/skinned**: NOTHING. No mask, no dilation. "DLSS looks fine with
  them!" / "a bit smudged". FSR was the one that looked terrible and shipped
  disabled.
- **Exposure**: `AutoExposure` create-flag for DLSS ("DLSS looks fine with
  it"); static 1×1 exposure texture only for FSR. **"DLSS fails if we pass in a
  1D texture"** — must be 2D. Prey variant: per-frame exposure fed as
  `InPreExposure` from a GPU readback, because updating the exposure *texture*
  every frame made DLSS "act weird".
- **Flags**: `MVLowRes | DepthInverted | AutoExposure | IsHDR`, MVJittered off.
- **Preset**: Default (0); user-facing option for E/F/J/K/L/M with the note
  "F might offer less ghosting than J/K" (CNN-era lore).
- **Reset**: on output change, SR toggle, and any frame with no 3D scene drawn.
- Known side effect of extending jitter phases: passes tuned for 2-phase
  (sun shadows, AO) can shimmer — their open TODO. Watch for it in CotW.

## 3. What the VR mods teach

- **PureDark (Skyrim/FO4 VR Upscaler)** — real Halton jitter from NGX's own
  helpers (`GetJitterPhaseCount`/`GetJitterOffset`) with a debug kill switch;
  MVs declared un-jittered; AutoExposure on; **mip bias 0 at DLAA**;
  sub-rect per-eye evaluation on a double-wide (ours: separate textures, also
  fine); **clears the MV texture after each evaluate by copying an
  always-empty texture** so skipped passes feed zeros, not stale vectors;
  "anything that blurs the input must be off" (TAA/DoF/FXAA force-disabled).
- **MSFS (the instructive official title)** — identical symptom class
  (high-contrast detail smearing in VR at DLAA): community fix was DLL 310.1 +
  **force preset J** ("no more ghosting"); official fix was DLSS 4.5
  **preset M ("Ghost Killer")**, trained in linear space for high-contrast
  smear. Also: right-eye-only shimmer was root-caused to a **broken right-eye
  velocity buffer** — when one eye misbehaves, audit that eye's inputs.
- **DCS VR** — forced **preset J** "much better for DCS VR"; K sometimes reads
  oversharpened.
- **Luke Ross / R.E.A.L.** — standing config: DLL 310.5.x, force preset L or M;
  motion blur / DoF / grain / CA off. Transformer model "fixed" their DLSS
  ghosting even under AER.
- **UEVR** — native stereo (both eyes every frame — our mode): "DLSS/FSR2
  usually work completely fine with no ghosting"; AER is what fights temporal
  accumulators. Their AFR MV fix (same-eye residual reconstruction) is not
  needed in our mode.
- **Skyrim Community Shaders** — per-eye stochastic effects converging
  differently per eye ⇒ binocular shimmer. Keep **both eyes on the same jitter
  phase** each frame.
- **fholger's verdict** (why vrperfkit never got DLSS): "needs deep engine
  integration" — input hygiene IS the work; there is no generic shortcut.

## 4. What OptiScaler's source teaches (catalog of integration bugs)

No auto-detection anywhere — its whole model is per-flag manual overrides that
humans toggle until the artifact dies. The flag list = the checklist of common
integration bugs: `DepthInverted`, `AutoExposure`, `HDR`, `JitterCancellation`
(=MVJittered), `DisplayResolution` (=high-res MVs), `DisableReactiveMask`.
Its missing-exposure fallback is NOT a fake texture — it flips to auto-exposure.
Preset override = `NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA` (+ per-mode
variants) before feature creation. Copy this pattern: ship live per-flag
overrides in our panel so any future misdiagnosis is a toggle, not a build.

## 5. Presets and DLL versions (310.x reality, from headers + community)

- Enum today: A–D removed, E/F deprecated, G/H/I/N/O reserved, **J=10, K=11,
  L=12, M=13** (all transformer).
- **K**: default for DLAA/Quality/Balanced since 310.2.1 — "best image quality
  at higher performance cost".
- **J**: "slightly less ghosting at the cost of extra flickering" (header) —
  the proven VR winner in MSFS/DCS lore.
- **L**: UltraPerf default, "sharper, more stable, **less ghosting than J, K**,
  more expensive" — always auto-exposes; peak-performant on RTX 40+.
- **M**: Perf default, "similar improvements to L but closer in speed to J/K" —
  MSFS's official "Ghost Killer".
- DLL: we ship **310.7.0** (current). Known regression: 310.4.0 ghosted more
  than 310.3.0 — pin and A/B, don't chase newest blindly.
- Set BEFORE feature creation, per quality slot:
  `NVSDK_NGX_Parameter_SetUI(params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, value)`;
  preset persists for the feature's lifetime; DLAA hint only applies when
  in==out size (our case).

## 6. Instrumentation we now know exists (deploy before tuning)

1. **Dev DLL** (`lib/Windows_x86_64/dev/nvngx_dlss.dll` in the NVIDIA/DLSS
   repo; local copy in `thirdparty\dlss\dev\`) — drop-in, watermarked, unlocks
   the debug overlays.
2. **Registry** (`HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore`):
   `ShowDlssIndicator` (1 dev / 1024 release), `LogLevel` 2, `EnableConsoleLogging` 1.
   NGX logs to the path given at Init — **must be writable or NGX can silently
   fail** (audit our `InApplicationDataPath`!). Repo `utils\*.reg` has ready files.
3. **Dev-DLL hotkeys**: CTRL+ALT+F12 cycle input overlays (color / **MVs** /
   depth / **jitter scatter plot** — red dots = out-of-range jitter, yellow =
   too few phases / exposure); CTRL+ALT+F6 **accumulation mode** (static scene
   must converge to perfect native — the definitive jitter+MV correctness
   test); CTRL+ALT+F9 cycles 20 jitter sign/scale transforms live (finds our
   sign convention in minutes, no rebuilds); CTRL+ALT+F10 swap jitter axes;
   CTRL+ALT+Y toggle auto-exposure live; CTRL+ALT+O NaN sweep.
4. **RenderDoc does NOT support NGX** (guide, Known Tooling Issues). Fallback:
   our own SRV dumps, or route inputs to FSR2's DX11 lib for a capture.

## 7. Vegetation velocity — the residual problem, if it still matters

After registration is fixed, expectation from Luma: foliage drops to "slightly
smudged". If that's not good enough:

- **Cheap probe first**: capture with the game's motion-blur toggle ON while an
  animal runs past — if skinned draws write real velocity for the blur pass,
  force that path and consume it (free engine vectors for the fastest movers).
  If not, the velocity buffer is pure camera reprojection — settled.
- **The real fix** (big, precedented in-engine, never done by a mod): UE-style
  `VertexDeformationOutputsVelocity` — if CotW's wind is analytic vertex sway
  (function of time + wind constants, likely for this era), tag vegetation
  draws, evaluate the sway at t and t−1 (cbuffer history we can snapshot), and
  write the delta into the velocity target before our resolve. All ingredients
  (draw tagging, shader replacement, cbuffer snapshots) exist in this project.
- **Masks are NOT the answer** on transformer presets (§1b). Optical-flow MV
  substitution (ReShade-style) is below DLAA quality bar — non-avenue.
- Preset J/L/M tuning is the legitimate dial for whatever smear remains.

---

## 8. THE PLAN — ranked, with expected effect

Phase A — correctness (all four before the next verdict; they compound):
1. **Report the true jitter.** Read the engine's actual per-frame jitter from
   the captured matrix (`m[12]/m[13]` delta — we already measure it via the
   third difference), convert clip→pixels (`jx_px = 0.5 × m12_delta × w_px`,
   sign per MV convention, Y down), pass per evaluate. Set `MVJittered=1`
   (our MVs contain jitter) — OR dejitter in the MV shader (Luma's way,
   current frame only) and keep `MVJittered=0`. Same phase both eyes.
2. **Set `DepthInverted`** after confirming Apex reversed-Z (one probe: read
   depth at a known near/far pair, or copy Luma's JC3 assumption).
3. **Exposure**: keep the AutoExposure build (already deployed, untested).
4. **Reset discipline**: `InReset=1` on menu/loading frames (no scene draw),
   teleports, feature recreation.

Phase B — quality (one variable at a time, in-headset A/B):
5. **Preset matrix** at DLAA: K (explicit default) → **J** → **M** → L.
   Panel-selectable so the owner can flip live. Watch: L forces auto-exposure;
   J may flicker.
6. **8-phase Halton jitter** replacing the engine's 2-phase: write our own
   sequence into the engine's jitter source each frame (find the CPU writer —
   Luma's PatchJitters is the template; we may instead be able to compose it
   into the matrices we already reproject with, since we own the resolve).
   Watch for shadow/AO shimmer (Luma's TODO).
7. **Mip bias experiment**: 0 vs −1 on scene samplers at DLAA.
8. **PureDark's MV clear** after evaluate (stale-vector insurance).

Phase C — instrumentation (parallel with A, before any deep tuning):
9. Dev DLL + registry + hotkey pass: accumulation-mode convergence test,
   jitter scatter, per-eye MV overlay (right-eye audit per MSFS lesson),
   NaN sweep, exposure test pattern. Audit our NGX Init log path is writable.
10. Panel: OptiScaler-style live per-flag overrides (AutoExposure / HDR /
    DepthInverted / MVJittered / preset) + the master DLSS off switch (rule).

Phase D — only if foliage still unacceptable after A+B:
11. Motion-blur velocity probe → force skinned velocity if it exists.
12. Vertex-shader wind velocity reconstruction (the big one, §7).

Success criteria: accumulation mode converges to native on a static scene
(proves registration); snow-area smear at or below the mod's own resolve;
foliage at Luma's "slightly smudged" or better; no new shimmer on shadows/AO;
both eyes visually symmetric.
