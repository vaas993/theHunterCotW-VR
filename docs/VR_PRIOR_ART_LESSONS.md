# What other VR mods do — smoothness, FOV, resolution

Research passes 2026-08-06, from the mod collection on this machine. Two questions:
how do shipped VR mods keep AER and true stereo smooth, and how do they implement
FOV and resolution so that DIFFERENT HEADSETS work.

## The sources, and how deeply each could be read

| Mod | Games | What it is | Read |
|---|---|---|---|
| anvilengine2vr | AC Odyssey / Valhalla / Mirage | native OpenXR, DXGI hook — same architecture as ours | full source (`Far Cry 2 VR\research\existing_vr_mods\anvilengine2vr`) |
| AC Black Flag VR v1.0.0 | AC Black Flag | the same author's framework, built | binary strings — unusually revealing |
| Halo MCC VR 0.3.0 | Halo 3 / ODST / Reach | native OpenXR | full config — every setting self-documented — + README |
| MELE1-VR V2 | Mass Effect LE (ME1) | native; FOUR modes: Stereo / AER / DIBR / Mono | full INI + the installer script (where the resolution logic lives) |
| bioshock-vr 0.2.0 | BioShock Remastered | native OpenXR, full-rate stereo | README |
| Luke Ross R.E.A.L. | Cyberpunk + ~30 AAA | THE AER reference | hub docs + per-game notes only — the installer itself is encrypted (dvrcache DRM); noted honestly |
| UEVR Engine.ini profiles (P40L0) | Cronos, Mafia: Old Country | UE tuning for UEVR | full INIs (~1,200 lines each) |
| PCVR-Mods-Hub | 178 games | installer hub | sampled READMEs: Alien Isolation (MotherVR), Crysis VR (fholger), AC family, UEVR Deluxe, REFramework, Forza Horizon 5/6, Ready or Not, DawnVR, EchoGeneration2, Doom3BFG |
| KisakCOD-VR v0.10.0-beta.2 | Call of Duty 4 | source-port VR (jplakon, GPLv3), D3D9 bridged to OpenXR | full release docs: README, CHANGELOG, KNOWN-ISSUES, INSTALL, VR-Settings.bat, launcher (`<your VR reference folder> Patches&Mods\VR APPS\Other VR creators\jplakon`) — see PART 2b |

More unread material in that same `Other VR creators` folder for later passes:
BioVRDev + mohamad-balouza (Bioshock), Halcyon (Mass Effect 1 & 2), Mastersell
(ME2 LE motion controls), tig3rmast3r (The Witcher 3).

Two known gaps: the Luke Ross binary is unreadable (settings come from its docs
only), and anvilengine2vr's `vrframework` submodule is an empty checkout — its
frame loop was read through the Black Flag binary's strings instead.

---

# PART 1 — SMOOTHNESS (AER and true stereo)

## AER-specific

**1. Per-eye temporal contexts — the single biggest AER finding.**
The Black Flag framework intercepts FSR 3.1's `ffxCreateContext` and creates
TWO upscaler contexts, one per eye, swapped per frame:

    [FSR3.1] ffxCreateContext intercepted for FSR Upscale - creating dual contexts for AER
    [FSR3.1] Successfully created right eye context for AER

Why: every temporal pass (FSR/DLSS/TAA/motion blur) keeps frame history. Under
AER that history alternates eyes, so left-eye data is blended into right-eye
frames — the classic AER ghost. For CotW: TAA history is the prime suspect for
AER shimmer; the fix direction is per-eye history, or TAA off in AER mode.

**2. Kill temporal/screen-space effects at the engine level, not in a menu.**
anvilengine2vr BYTE-PATCHES TAA off and chromatic aberration off (EngineTwicks.cpp,
one patch each). MCC VR defaults motion blur off and names the reason: "also
removes repeating stereo echo artifacts" — blur is computed from frame-to-frame
motion, which under AER includes the eye flip itself.

**3. A steady rate beats a high rate.** Luke Ross's own docs: AER needs a
CONSISTENT frame rate; when it cannot be held, drop to Mono rather than let AER
stutter (the Cyberpunk notes say exactly this — "Use Mono for now"). Uneven
pacing under AER means one eye goes stale = perceived flicker, not lag.

**4. AER gets its own resolution.** MELE1 stores per-mode resolutions (AER
3328x3536 vs Stereo square vs Mono 16:9). AER renders one eye per frame, so it
can afford MORE pixels per frame than true stereo. Ours submits both modes at
the same size — free image quality on the AER path.

**5. Ghost-hunting diagnostics ship in the config.** MCC VR: `right_eye_first`
(render right eye first, purely to chase ghosting). MELE1: `aerSwapEyes`. Cheap.

**6. Costly insets refresh at a divisor.** MCC VR's scope zoom picture renders
every Nth frame (`scope_refresh_divisor = 3`, range 1-4).

## True stereo and general pacing

**7. Never let the desktop clock pace the headset.** MCC VR
`desktop_present_unlocked` — the VR frame is submitted inside the game's desktop
Present, so desktop V-Sync paces the headset at the MONITOR's refresh. Their fix:
present the mirror unlocked; the runtime's refresh becomes the only clock. "The
desktop window may tear; the headset never does." Every mod's required settings
agree: V-Sync OFF, frame cap OFF, HDR OFF (MELE1: HDR is "the #1 cause" of a
broken headset image — matches our own FC2 HDR finding).

**8. One frame in flight.** Both UEVR profiles: `D3D12.MaximumFrameLatency=1`,
`r.OneFrameThreadLag=1`, `D3D12.AFRUseFramePacing=1`, `r.FinishCurrentFrame=0`.
D3D11 equivalent for us: `IDXGIDevice1::SetMaximumFrameLatency(1)`.

**9. Validate xrWaitFrame's outputs.** The Black Flag framework explicitly
checks predicted display time for garbage ("is 11111111!", "less than predicted
display period", "less than 1000!"). Bad runtime predictions are a real observed
failure mode; guard them in SubmitFrame.

**10. Pose micro-smoothing is a seasoning, not a fix.** MCC VR
`headset_smoothing` default 0.03, max 0.10 ("0 = raw; try 0.05 only for
micro-jitter"). MELE1 `headLookSmoothing 0.4` on look; `aim_stabilization`
smooths the RETICLE only — "bullets stay raw."

**11. VR stutter is usually CPU/streaming, not GPU.** MCC VR's draw-distance
slider: "VR is usually CPU-limited, not GPU." Crysis VR: reprojection is
CPU-bound. The UEVR profiles spend hundreds of lines on PSO precaching
(`r.pso.precaching=1`) and texture-streaming pacing (background threads,
amortised CPU->GPU copies) — shader-compile and streaming hitches read as judder.

**12. Sharpen the OUTPUT, not the render.** MCC VR: bicubic upscale filter +
RCAS sharpening + optional FXAA/SMAA on the finished image.

**13. Mirror ONE eye on the desktop.** bioshock-vr mirrors the left eye instead
of the raw alternating view (`vrmirror`) — directly applicable to our AER
desktop window, which currently shows the flip.

**14. Keep pacing when the headset is idle.** bioshock-vr `vrpace`: game
continues full speed on the monitor with the headset off — the exact state that
crashes ours.

---

# PART 2 — FOV AND RESOLUTION (multi-headset)

## FOV: three tiers, best first

**Tier 1 — replace the projection matrix from the runtime's tangents
(anvilengine2vr — the right way).** Hook the engine's projection builder and
write an off-centre matrix straight from xrLocateViews' per-eye fov tangents:

```cpp
auto frustum = vr->get_runtime()->frustums[eye];   // l, r, t, b tangents
float sum_rl = frustum[1] + frustum[0], sum_tb = frustum[2] + frustum[3];
float inv_rl = 1.f / (frustum[1] - frustum[0]);
float inv_tb = 1.f / (frustum[2] - frustum[3]);
proj = { 2*inv_rl, 0,         0,                          0,
         0,        2*inv_tb,  0,                          0,
         sum_rl*inv_rl, sum_tb*inv_tb, far/(near-far),   -1,
         0,        0,         near*far/(near-far),        0 };
```

The game renders each eye with the headset's EXACT asymmetric frustum. Any
headset works automatically — Quest's lopsided -54/+40, Index, Pimax — because
the runtime supplies the numbers. There is NO FOV setting at all. They gate the
replacement on "is this the main 3D view" (`inside_calc_mvp || farPlane > 1201`)
so shadow/UI projections stay stock.

**Tier 2 — max the game's own FOV and project into it (MCC VR).** Game keeps its
symmetric projection; the mod REQUIRES the in-game slider at 120 and maps the
headset frustum into the image. Their warning names the trap exactly:

> "FOV 120 is the one that visibly breaks the game if wrong: at a lower FOV the
> engine stops drawing geometry at the edges, so scenery pops in and out in the
> headset."

**CULLING FOLLOWS THE GAME'S FOV, NOT YOURS.** If the submitted frustum is wider
than the game's render FOV, the edges pop. MELE1 adds `questFovMatch` and
`vrFillH/vrFillV` (how much of the headset view the flat frame fills, separate
values for cutscenes).

**Tier 3 — FOV as user sliders (the praydog-derived framework).** "Horizontal
FOV Scale", "Vertical FOV Scale", and "Extended FOV Scale Range" — a switch that
exists PURELY for wide-FOV headsets (Pimax) where the normal slider range is not
enough. Logs `Diagonal FOV: %f` at startup as a per-headset diagnostic.
"World-Space Scale" is a SEPARATE slider — world scale and FOV are different
quantities.

Small FOV lessons: games that fake ZOOM by changing FOV (EchoGeneration2 docs:
"impossible in VR") need a camera-offset compromise instead — relevant to CotW's
scope/hold-breath zoom. DawnVR gives the monitor/spectator view its OWN FOV
(default 90), independent of the headset.

## Resolution: four patterns

**1. Scale-of-recommended, not absolute pixels (MCC VR, framework, SteamVR).**
MCC's only knob is `resolution_scale` 0.35-2.75 multiplying a base ("your value
scales both numbers together, so the picture keeps its shape"; out-of-range
values clamp, so a typo cannot brick the launch). The framework defers to the
runtime: reads `maxRecommendedResolution`, honours SteamVR's per-headset
supersampling ("Resolution can be changed in SteamVR"). A scale adapts to any
headset automatically; absolute pixels are right for exactly one.

**2. Pixels-per-degree (Luke Ross — most headset-independent).** "Target Pixels
Per Degree" + an "Optimize Resolution" button that recomputes the render size
from it. PPD x the headset's actual FOV = pixels: one number means the same
sharpness on every headset.

**3. Lie to the game about the monitor (MELE1).** Verbatim from its installer:

> "The mod makes the game render ABOVE the monitor's resolution: it reports a
> larger primary display to the game (InstallDisplayQueryHooks in
> d3d_capture.cpp), so 6144x3456 works on a 1080p panel exactly as it does on a
> 4K one. Your MONITOR no longer limits the image. Your GPU does."

Plus a RESOLUTION WATCHDOG (`expectedResX/Y`): the mod verifies at runtime that
the game actually took the resolution, catching silent reverts.

**4. Per-mode SHAPE, measured tiers (MELE1's hardest-won lesson).** Each VR mode
gets its own resolution AND aspect: Stereo renders SQUARE (3072^2..6144^2), Mono
stays 16:9 wide, AER gets 3328x3536 — because "16:9 resolution squashed the UI +
blurred the image. Square fixes both." Per-eye render shape should be
near-square, matching real per-eye frusta. Tiers are measured, not guessed
("4090, Stereo: 3072 = locked 60fps; 5120 = ~50; 6144 = ~40; 10240 = a gamble"),
and mode+resolution are picked TOGETHER every run — a mode/resolution desync
caused an actual GPU-crash investigation.

**Universal negative: in-game upscalers/dynamic res OFF.** MCC: "Do not enable
FSR - it breaks the VR image scale." Cyberpunk R.E.A.L.: "Resolution Scaling:
Off." Mods provide their own final-image upscale instead (MCC: bicubic + RCAS)
so render size and submit size can differ cleanly. The exception proves it: the
framework supports FSR only by managing dual per-eye contexts itself (Part 1 #1).

---

# PART 2b — KisakCOD-VR (Call of Duty 4, jplakon) — read 2026-08-06

Source-port VR (GPLv3, github jplakon/CallOfDuty4_VR), v0.10.0-beta.2, at
`<your VR reference folder> Patches&Mods\VR APPS\Other VR creators\jplakon\`. Tested config is
the owner's exact setup: Quest 3 + Virtual Desktop OpenXR. D3D9 engine bridged
to OpenXR ("GPU bridge"). Its beta.2 changelog is practically a frame-pacing
case study.

## Pacing lessons (their beta.2 was exactly this)

**15. Pose-stamp every captured frame.** "Associates each captured stereo image
with the exact OpenXR render pose used to create it, preventing a delayed GPU
capture from using a newer head pose." A frame submitted with a NEWER pose than
it was rendered with is head-turn judder. Ours keeps `m_headPose` before
overwrite for the same reason — theirs generalises it: the pose travels WITH the
image, always, through any queue depth.

**16. A real capture queue with GPU fences, not a two-slot flip.** They replaced
"alternating capture reuse/drop behavior" with an ORDERED FOUR-SLOT queue and
retirement-fence polling — "substantially improves fresh-frame delivery and
reduces head-turn judder, consumer skips, and reused-frame streaks." Reused-
frame streaks are precisely an AER smoothness killer.

**17. Per-frame engine work must run once per FRAME, not once per VIEW.** Their
sound update ran once per eye and scope view — three times per frame — producing
"scratchy or corrupted looping audio." Fixed by running it once per game frame.
Directly relevant to our full-rate stereo, which REPLAYS the engine's
build+submit pair: anything inside that path with per-frame semantics (audio,
timers, particles) runs twice. We found the clock; audio is the same class.

**18. Yaw-only game camera base.** "Reduces the game-authored VR camera base to
yaw only, preventing sprint, rappel, and scripted pitch/roll from banking the
horizon while preserving full physical HMD motion." The game contributes yaw;
pitch/roll come from the head alone, so scripted animation can never bank the
horizon. A cleaner formulation of our decoupled-pitch goal.

## Resolution/FOV lessons

**19. Mode and scale are one setting, and the launcher REJECTS invalid combos.**
`VR_CUSTOM_MODE=6016x2688` + `OUTPUT_SCALE=1.0`, or the supported lower preset
`4768x2016` + `0.75` — documented as pairs, picked together. The obsolete
3072x1536 is refused at launch "because it cannot hold two rectangular eyes plus
the dedicated scope panel." Same lesson as MELE1 (mode+resolution together, a
desync caused a GPU-crash hunt), enforced mechanically.

**20. Packed layout sized for its contents.** One render target holds both eyes
PLUS a dedicated scope panel; the mode's shape exists to fit that packing.
`KISAK_VR_ALLOW_OVERSIZED_WINDOW=1` = window bigger than the monitor (MELE's
display-lie, MCC's fit_desktop_window, third implementation of the same idea).

**21. The scope is a separate physical panel, placed in METRES.**
`SCOPE_FORWARD/LEFT/UP_METERS`, `SCOPE_RADIUS_METERS=0.024`,
`SCOPE_CAPTURE_SIZE=1024` — a small dedicated capture shown on a physical disc
on the rifle, with headset-specific calibration expected. Convergent with MCC's
gun-mounted scope screen (which adds refresh-divisor). This is the shipped
answer to "zoom is impossible in VR" (Part 2): a panel, not an FOV change.

**22. Stereo-synchronised shadows are a FEATURE with an off switch.**
`KISAK_VR_SHADOWS=1`, "synchronized dynamic shadow maps", documented as
significant-cost with "set 0 if corruption or poor performance." Per-eye shadow
mismatch shimmers in stereo; synchronising costs; both facts acknowledged, user
decides. (Their FSR path exists and defaults OFF — consistent with every other
mod here.)

**23. Diagnostics discipline.** Verbose per-frame traces are RETIRED for normal
play; bug reports ask for `[VR][PERF]` lines; a separate diagnostics launcher
restores developer messages; the normal launcher hides engine warnings and the
FPS marker from the headset. Two launchers, one policy.

---

# PART 3 — WHERE OURS STANDS, AND THE UPGRADE PATH

Ours today: swapchains sized to the GAME'S BACKBUFFER (runtime recommends
3072x3264 per eye; we use 3304x2400 — landscape where the headset wants
near-portrait: MELE's "wrong shape" mistake). Submitted FOV is SYMMETRIC,
built to match the game's aspect (we log the Quest's real -54/+40 frustum, then
discard the asymmetry). The launcher's presets are ABSOLUTE pixels — correct for
this headset only. It works because it was tuned on one headset.

In order of value:

1. **Per-eye swapchains at the runtime's recommended size** (shape included),
   one scale knob on top (MCC/framework pattern).
2. **Game FOV >= the headset's widest angle, derived per headset** — not a
   preset. Watch for MCC's culling trap: CotW pop-in at the view edges would be
   exactly that.
> **2026-08-12 addendum — DLSS prior art read in a dedicated pass:** Luma JC3
> (full source — jitter patch, MV dejitter, DepthInverted, AutoExposure),
> PureDark Skyrim/FO4 VR Upscaler (NGX Halton helpers, MV clear-after-evaluate,
> mip bias 0 at DLAA), MSFS/DCS preset lore (J/M kill VR smear), UEVR AFR MV
> fix, OptiScaler override catalog, official Programming Guide v310.5.0 in
> full. All lessons + ranked plan live in `DLSS_RESEARCH.md` — read that first
> for anything DLSS.

3. **Projection replacement from the tangents** (anvilengine2vr) — the real
   prize. We already hook the projection path (kEulerToMatrixRetView) and hold
   WorldViewProjection per draw; the machinery is close.
4. **Launcher resolution table becomes scale-of-recommended (or PPD)** so the
   presets mean the same thing on any headset.
5. AER-path items from Part 1: unlocked desktop present (#7), AER-specific
   resolution (#4), per-eye TAA handling (#1/#2), frame latency 1 (#8).

---

# PART 3 — 6DoF / POSITION TRACKING (Luke Ross R.E.A.L., read 2026-08-15)

Read while our own first 6DoF build was being tested. Full write-up, with the
decisions it feeds, is in `SIX_DOF.md`; the headlines:

**1. Camera-only offset IS the state of the art.** R.E.A.L. applies the head
offset at the rendering/camera level and states plainly that it *"does not
directly carry over to your character's head, or body"*. Ours does the same
thing at the same kind of site. Nobody is doing better in engines that do not
expose the player.

**2. The exception names the upgrade path.** *"With NOLF2 I was able to
implement full position tracking because the game provided a way to move the
main character model accurately by an arbitrary, precise amount."* Real 6DoF =
move the PLAYER and let the engine's own collision carry the camera. Finding a
player-position write in Apex would graduate our lean into a walk.

**3. Expect the engine to seize the camera** (vehicles, being thrown, slope
slides). His answer is a second, later correction stage - the "view matrix fix"
- that forces angles and position *"even when the in-game camera resists"*.

**4. Cutscenes are a sub-feature, not a corner case**: separate switches for
stereo mode, pitch mode and whether camera tracking applies at all.

**5. The HUD is the known casualty of a moving head.** His is drawn in the
world about three feet out, fixed in space, with a HUD tracking mode switch and
a headlocked-while-aiming state. A screen-space HUD over a moving world is the
thing to watch for.

**6. No published position scale or travel limit** - his is 1:1 and unclamped.
Our `six_dof_limit_m` is our own guard against leaning into geometry.
