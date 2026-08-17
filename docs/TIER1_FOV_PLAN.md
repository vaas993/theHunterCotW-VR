# TIER 1 FOV FOR theHunter: Call of the Wild — IMPLEMENTATION PLAN

Build: `theHunterCotW_F.exe`, SizeOfImage 0x02A13000, timestamp 0x6A5A5133. All RVAs module-relative. Every number below is either measured in this session's captures/disassembly or derived from those; anything inferred is flagged in section 9.

---

## 0. THE ONE THING THAT CHANGED THE ANSWER

The brief's first question — "is the projection separable?" — is now settled, and the answer kills the obvious plan:

**There is no projection matrix anywhere in this renderer's constant buffers.** Every 4x4 in every constant block bound by every one of 1041 draws in `cotw_frame48688.rdc` was tested against the pure-projection shape; zero matches. The search was then widened to all 26,397 buffers <= 64 KB, whole-buffer, row-major and transposed, at 11 different events across the frame. Zero matches. World draws read either a shared premultiplied world->clip matrix (`GlobalConstants`, 704 B, four copies) or a CPU-baked `WorldViewProjection` per object.

So "hook the projection builder and write the anvilengine2vr literal" has no target, and "patch the shared projection CB" has no buffer.

That leaves two real routes, and the second one is better than it looks.

---

## 1. THE ROUTE

**Engine code — but not the route the brief expected. Do not patch matrices at all.**

The plan is two levers that together produce a result geometrically identical to Tier 1:

- **Lever A (engine, one float per frame):** widen the engine's own world FOV so the frame it renders is a strict *superset* of both eyes' frusta. One scalar, at a location proven by two independent readers of the same chain.
- **Lever B (submission, `vr.cpp`):** give each eye its *own off-centre sub-rectangle* of that frame, and submit the runtime's real per-eye `fov` from `xrLocateViews`. The pixels each eye receives then correspond exactly to the runtime's asymmetric tangents.

The user sees exactly what Tier 1 would show. The cost is that ~33% of rendered pixels are thrown away at the inner corners. That is the entire price, and it buys the following:

**Why not the constant-buffer route** (this is the justification, from evidence, not preference):

1. **It is not one matrix, it is 341.** Corrected census on `cotw_frame48688`: 341 of 361 main-pass draws carry a camera clip matrix, spread over at least 14 distinct (block, offset) sites — `GlobalConstants` +0x000/+0x1d0/+0x210/+0x260, `InstanceConsts` +0x040, `PerInstanceConstants` +0x000, `cbVertexConsts` +0x000, HS `cb1` +0x010 (14 draws), DS `cb1` +0x010 (11 draws), plus one-off PS/DS sites. Miss one and that geometry is drawn at the wrong angular position in one eye.
2. **~50 windows per frame are lookalikes.** Matrices at `InstanceConsts` +0x000 and `LocalConstants` +0x040 have a=3.661/b=3.388 with near *exactly* 0.01 — the camera's own near — so the structural detector proposed in the CB report does not separate them. Patching those draws geometry through a foreign frustum.
3. **+0x210/+0x260 are the PREVIOUS frame's view-projection, not duplicates.** In `cotw_frame14974` they carry a different camera basis (translation 2383.69 vs 2387.14). That is the TAA / motion-vector reprojection pair. Get those inconsistent with the current pair and you reproduce the AER ghosting failure from this project's own prior-art notes — temporal history crossing between eyes.
4. **The matrices are not the only consumers of the projection.** This is the decisive one. Vegetation is culled by a compute shader (eid 4257, CS `ResourceId::2336`) against `VegSysCtx`, a 304-byte block holding **explicit CPU-built plane equations** (`m_AABNormal`, float4 x12, e.g. +0x030 = (0, 0.955875, 0, 2382.412) — the same numbers as the view matrix's col3/row3) plus `m_LodFOV` = 0.863977 driving vegetation LOD. Those are planes and a scalar; no matrix patch touches them. The PS `GlobalConstants` additionally carries depth-linearisation constants. CotW is deferred (two RTs, second R16G16 motion vectors), so SSAO / fog / SSR / TAA reconstruct world position from projection parameters that live in *other* fields. A perfect matrix patch leaves every one of those on the old frustum.
5. **One scalar reaches all of it.** `camera+0x450` is the single committed home of the world FOV, and everything — GlobalConstants, every baked WVP, terrain `cb1.m_OffsetViewProjection`, the vegetation planes, the LOD FOV, the depth constants — is built downstream of it. Widen that and the whole renderer stays *self-consistent*. That is the same payoff anvilengine2vr gets by hooking `onCalcProjection` inside `onCalcFinalView` (the frustum planes are computed at the end of the same function), obtained here through the FOV scalar instead of a matrix.

**And the asymmetry does not need to live in the engine.** A single FOV scalar cannot express an off-centre frustum, but it does not have to: the asymmetry lives entirely in *which rectangle of the frame each eye is shown* and *what fov we tell the runtime that rectangle spans*. That part is exact.

Section 3 keeps the CB route documented as Stage 3, for reclaiming the wasted pixels later. It is optional and it is last.

---

## 2. THE EXACT CHANGE

### 2.1 The engine's convention, stated (measured, not assumed)

Storage is **row-major with the row-vector convention**: `float4 #i` in memory **is row i**, and `clip = x*row0 + y*row1 + z*row2 + row3`. Proven from the disassembly, VS `ResourceId::1752`:

```
69: mul r0.xyzw, r2.yyyy, WorldViewProj[1].xyzw
70: mad r0.xyzw, r2.xxxx, WorldViewProj[0].xyzw, r0.xyzw
71: mad r0.xyzw, r2.wwww, WorldViewProj[2].xyzw, r0.xyzw
72: add o0.xyzw, r0.xyzw, WorldViewProj[3].xyzw
```

The engine's projection P, decoded from `GlobalConstants` +0x000 (col2 == 0 exactly; |col3| == 1 and col3 is the camera forward; residuals against the camera position are float noise, so there is no off-centre term and no TAA jitter baked in):

```
row0 = (a, 0, 0, 0)        a = 1/tanH
row1 = (0, b, 0, 0)        b = 1/tanV
row2 = (0, 0, 0, +1)       <-- +1, NOT the -1 of the anvilengine2vr snippet
row3 = (0, 0, n, 0)        n = 0.009432 (world-space copies) / 0.010000 (camera-relative copies)
```

That is **reverse-Z with an INFINITE far plane**: z_ndc = n/depth, 1 at the near plane falling to 0 at infinity, and clip.w = +view depth. There is no far term at all. The reference form's `far/(near-far)` and `near*far/(near-far)` entries **do not exist in this engine and must never be written**. `s = m23 = +1`.

Measured, flat capture: a = 1.08056 (h 85.57 deg), b = 1.00000 (v 90.000 deg), tanH/tanV = 0.925446 against render aspect 2880/3112 = 0.925450. Exact.

**This settles a question the mod had left open:** with the game's slider on 90, b came out at exactly 1.00000, i.e. tanV = 1.0. If the slider were horizontal, a would be 1.0; a is 1.0806. **The slider is vertical. That is now measured, not inferred** (`gamesettings.cpp:78-83` asserted it from a one-degree visual match; this is the hard version).

The projection is also **dynamic**: aiming down a scope narrows the *whole scene's* projection (a 1.0806 -> 2.9763, b 1.0000 -> 2.7544, same aspect). Any design must decide what to do about that rather than stomp it; see 2.5.

### 2.2 Where near and far come from

**Nowhere. Nothing touches them.** Stages 1 and 2 write no matrix, so reverse-Z, the infinite far plane, the near value, the depth-linearisation constants and every depth-based effect are preserved by construction. This is not a mitigation, it is the absence of the risk.

### 2.3 How the per-eye tangents get from xrLocateViews to the game thread

`vr.cpp:1257` already fills `views[2]` from `xrLocateViews`. Add, immediately after the `posesValid` check:

```cpp
// tier1.h / tier1.cpp - new, tiny translation unit
struct Tier1Frustum {
    float l[2], r[2], t[2], b[2];   // TANGENTS, signs preserved (l,b negative)
    float coverTanH, coverTanV;     // the covering symmetric render frustum
    bool  latched;
};
```

`Tier1::NoteRuntimeViews(const XrView v[2])`: on the **first** valid locate, convert with `tanf(angle)` (no `fabs` — the sign convention is preserved end to end, as UEVR's `OpenXR.cpp:538` comment insists), compute the covering tangents, and **latch**. Latch once, do not recompute per frame: the runtime's fov is constant on real hardware and a per-frame recompute would make the engine's FOV jitter. On later frames, if any eye needs more than the latch by >1%, log once and re-latch.

```cpp
TH = max over both eyes of max(-l, r)        // 1.376382 for this headset (54.0 deg)
TV = max over both eyes of max(t, -b)        // 1.428148                  (55.0 deg)
```

Published through an `InterlockedExchange`-guarded flag and read racily by the game thread, exactly like `g_viewBasis` in `stereo.cpp:20` — a one-frame-stale constant is invisible, a lock in a hot hook is not.

### 2.4 LEVER A — the engine FOV (the missing 22 degrees, and the culling fix)

**Hook:** MinHook detour on **`0x00642E80`**, the first-person camera modifier's `Update`, vtable `0x01A5C4D8` slot +0x28. Signature read off the call site at `0x004B763C..0x004B7648` (`mov r9,rdi; lea r8,[rsp+0x20]; lea rdx,[rbp-0x40]; call [rax+0x28]`), independently confirmed:

```cpp
using PFN_CamModUpdate = void(__fastcall*)(void* self, void* pose, float* lensOut, void* cam);
```

`lensOut[0]` = world FOV in **radians**, `lensOut[1]` = viewmodel/foreground FOV in radians. Written at `0x006434C3` and `0x006434FC` (`deg2rad` = 0.0174532942 at RVA 0x01938138). The ADS/zoom path rewrites `lensOut[0]` later, at `0x00643AC0`, so a **return** hook sits after everything.

Do **not** detour inside the function. It is 8201 bytes and also owns recoil (0x643B26), the pitch clamp (0x644200), hold-breath reseed (0x643DE8/0x643DF9) and the aim consumer (0x64424E); `apex.h` records four crashes from getting argument counts wrong in this exact neighbourhood. A normal MinHook entry detour that calls the original and then edits `lensOut` is safe and is the whole of the change.

```cpp
void __fastcall Hook_CamModUpdate(void* self, void* pose, float* lensOut, void* cam) {
    o_CamModUpdate(self, pose, lensOut, cam);
    if (!Cfg().tier1_fov || !lensOut) return;
    if (!Tier1::Latched() || !VR().Ready() || !PlayerCameraActive()) return;

    const float fovStockDeg = Tier1::StockWorldFovDeg();   // see below
    const float tanBase = tanf(fovStockDeg * 0.5f * kDeg);
    if (!(tanBase > 0.05f) || !(tanBase < 20.0f)) { Tier1::NoteLeverSkip(); return; }

    // A CONSTANT MULTIPLIER ON THE TANGENT, not an absolute write.
    // This preserves the ADS/scope ratio exactly: whatever magnification the
    // engine decided, it survives, scaled by the same k as the base frame.
    const float k = Tier1::CoverTanV() / tanBase;

    for (int i = 0; i < 2; ++i) {          // [0] world, [1] viewmodel
        if (i == 1 && !Cfg().tier1_scale_viewmodel_fov) break;
        const float f = lensOut[i];
        if (!(f > 0.05f) || !(f < 3.0f)) { Tier1::NoteLeverSkip(); continue; }
        lensOut[i] = 2.0f * atanf(tanf(f * 0.5f) * k);
    }
    Tier1::NoteLeverApplied(lensOut[0], lensOut[1]);
}
```

`StockWorldFovDeg()` reads the live game setting, which the engine itself re-reads every frame at `0x00642F74..0x00642F8C` and never clamps afterwards:

```cpp
// settings -> +0xFD8 -> int[0x38].  Global RVA 0x028064D8.
// GetIntSetting (0x00AEA520) for idx not in {0x10,0x31} is exactly:
//     ((int*)*(void**)(settings + 0xFD8))[idx]
int v = ReadChainOrDefault(/*rva*/0x028064D8, /*+*/0xFD8, /*index*/0x38, /*default*/90);
return (v >= 30 && v <= 120) ? (float)v : 90.0f;
```

Every SEH-guard the mod already uses applies; a null anywhere in the chain falls back to 90 and logs once.

**Why a multiplier and not an absolute write:** `0x00643AAE` multiplies `(1/magnification)` by `params+0xA4`, so ADS is a *ratio* of the world FOV. An absolute write kills scope zoom; a tangent multiplier keeps it exactly, and keeps the crop rectangle (2.6) constant whether or not the player is aiming.

**Why `lensOut[1]` too:** `ForeGroundFOV` (params+0xA0, 33.0 in the vanilla `default_first_person.ctunec`) is a separate tuned scalar — that is why the FOV slider never moved the gun, and why the Nexus "Increased Weapon FOV" mod is an ADF asset edit. Widen the world without it and the held weapon becomes enormous relative to the world. Scaling both by the same k keeps the stock relationship. `weapon_view_scale` stays as the fine tuner.

**What k comes out as, for this headset:**

| backbuffer aspect A | TV_cmd = max(TV, TH/A) | engine vertical | k (slider 90) |
|---|---|---|---|
| 0.9254 (FULLVIEW 2880x3112) | 1.487256 | 112.2 deg | 1.4873 |
| 0.9638 (optimal, new preset) | 1.428148 | 110.0 deg | 1.4281 |
| 1.3767 (FOV90-WIDE 3304x2400) | 1.428148 | 110.0 deg | 1.4281 |

`TV_cmd = max(TV, TH/A) * 1.02` (the 2% margin absorbs rounding and any runtime fov drift). Command that; the horizontal follows from the aspect, `tanH_actual = TV_cmd * A`.

### 2.5 The scope, decided rather than left to chance

The engine narrows the whole scene's projection when scoped. With Lever A as written, that narrowing survives (scaled by k), so the rendered world magnifies exactly as it does flat. The crop rectangle is computed from the **base** frustum and stays fixed, and the submitted fov stays the runtime's own. Result in the headset: aiming down a scope magnifies the world inside an unchanged field — which is what the mod does today, and both eyes magnify about the same forward axis, so fusion holds.

That is dishonest geometry, deliberately, and it is the same compromise every shipped mod makes (KisakCOD uses physical scope panels; MCC uses a gun-mounted scope screen). A properly rendered scope picture is future work, not this plan. Ship a config `tier1_ads = follow | hold`; `follow` is the default described above, `hold` forces `lensOut[0]` to the base covering value always (no world zoom at all) for anyone who finds the magnification uncomfortable.

### 2.6 LEVER B — the per-eye crop, and the exact submitted frustum

The rendered image is a symmetric perspective frame with known half-tangents `(tanH_a, tanV_a)`. A sub-rectangle of it *is* an off-centre sub-frustum — exactly. For eye e with runtime tangents `l,r,t,b`:

```
u0 = 0.5 + 0.5 * l / tanH_a          // NDC x = -1 at image u = 0
u1 = 0.5 + 0.5 * r / tanH_a
v0 = 0.5 - 0.5 * t / tanV_a          // NDC y = +1 (up) at image v = 0 (top row)
v1 = 0.5 - 0.5 * b / tanV_a
```

Then clamp to [0,1] **and derive the submitted fov back from the clamped bounds**, so the two can never disagree:

```
sub_l = (2*u0 - 1) * tanH_a ;  sub_r = (2*u1 - 1) * tanH_a
sub_t = (1 - 2*v0) * tanV_a ;  sub_b = (1 - 2*v1) * tanV_a
projViews[eye].fov = { atanf(sub_l), atanf(sub_r), atanf(sub_t), atanf(sub_b) };
```

Unclamped, that returns the runtime's own fov bit-for-bit. Clamped (lever A off, or a preset too narrow), it returns the honest intersection of what the headset wants and what was actually drawn — the picture loses field at that edge but **still fuses**, because the fov submitted matches the pixels submitted. This is the graceful-degradation property that makes Stage 2 shippable before Stage 1 is trusted.

Numbers for this headset at FULLVIEW 2880x3112 with lever A engaged (tanH_a = 1.376382, tanV_a = 1.487256):

```
eye0:  u [0.00000, 0.80482]   v [0.17535, 0.98013]   ->  x 0..2318, y 546..3050  (2318 x 2505 px)
eye1:  u [0.19518, 1.00000]   v [0.17535, 0.98013]   ->  x 562..2880, y 546..3050
```

Eye 0 takes the left part of the frame, eye 1 the right (the outer field is the larger one); both crop the same 16% off the **top**, because the headset sees 55 deg down and only 44 up. Both rects are the same size, so both eye swapchains stay the same size — but do not rely on that: size each eye's swapchain from its own rect, since a non-mirrored headset is legal.

Pixels kept: 0.8048 x 0.8048 = 64.8%. To land on the runtime's recommended per-eye pixel count exactly, size the backbuffer `W = rec_w / 0.8048`, `H = rec_h / 0.8048` — this is UEVR's `should_grow_rectangle_for_projection_cropping` (`OpenXR.cpp:521-531`) in one line.

**`vr.cpp` changes:**
- `ComputeCrop(srcW, srcH, headsetFov)` -> `ComputeEyeCrops(srcW, srcH, views[2])`, filling `m_eyes[e].rect {x,y,w,h}` instead of the single `m_cropX/m_cropY/m_width/m_height`. Keep the old members and the old function body behind `tier1_fov == false`.
- `CreateSwapchains` takes per-eye sizes.
- `CropBox()` -> `CropBox(int eye)`; the `CopySubresourceRegion` at `vr.cpp:1474` and the stash paths use the eye's box.
- `projViews[eye].subImage.imageRect` stays `{0,0,w_e,h_e}` — because we copied the right region, there is no need for an offset rect, which is the part of the "UEVR submits imageRect offsets" reading that does not apply here.
- `projViews[eye].fov = perEyeFov[eye]` (2.6) instead of the shared `fov`.

The pose side is untouched: `m_eyeRenderPose[eye]` and `aer_per_eye_pose` already do the right thing and their comment at `vr.cpp:1491-1509` remains correct.

**AER and mono:** in mono, both eyes crop *different* rectangles from the *same* image and submit *different* fovs — that is correct (identical rays, zero disparity) and strictly better than today, because each eye's angular placement stops being a compromise. Under AER only the fresh eye's rect is uploaded; the stale eye keeps its own last rect, which was cropped for it. Under full-rate both renders exist and each gets its own rect.

### 2.7 The matrix, for the record (Stage 3 only)

If the wasted 33% is ever reclaimed, the correction is a **right-multiply in clip space**, not a projection replacement. For `WVP_new = WVP_old * C`, in this engine's row-vector/row-major layout:

```
C00 = (2/(r-l)) / a_base          C11 = (2/(t-b)) / b_base
C30 = -(r+l)/(r-l)                C31 = -(t+b)/(t-b)
C22 = C33 = 1, everything else 0
```

applied to a stored `float m[16]` as:

```cpp
for (int i = 0; i < 4; ++i) {
    m[4*i+0] = C00 * m[4*i+0] + C30 * m[4*i+3];
    m[4*i+1] = C11 * m[4*i+1] + C31 * m[4*i+3];
    // m[4*i+2] and m[4*i+3] UNTOUCHED -> reverse-Z, infinite far, near all preserved
}
```

Two things that are easy to get wrong and that cost a debugging round each:

- **There is no `s` on C30/C31.** Deriving from `(P_old*C) row2 = m20_old*C00 + s*C30` gives `C30 = s*(m20_new - m20_old*C00)`, and with `m20_old = 0`, `m20_new = -s*(r+l)/(r-l)` and `s^2 = 1`, the s cancels. Verified numerically to 1.1e-16 against `inv(P_old) @ P_new`; the s-carrying form is wrong by *double the shear in the wrong direction*, which in a headset reads as an IPD or head-tracking bug, not as a projection bug.
- **The "identity when target == game frustum" test has zero power against that error** (r+l = 0 kills the term either way). The test with power is asymmetric and numeric: build `P_new` directly for a deliberately off-centre target, build `P_old*C`, require max elementwise difference < 1e-6.
- Use `a_base`/`b_base` **read once per frame from the shared block**, never the per-matrix `|col0|/|col3|`: for `M = W*V*P` that ratio equals `a` only when W is rigid or uniformly scaled, and the 25-draw non-uniform group has a spread of 0.31..1.0. One C for the whole frame, per eye.

---

## 3. HOW THE MOD IDENTIFIES THE TARGET AT RUNTIME

The standard this mod holds to is the first-person stencil tag: exact, structural, no learned pointers. Both levers meet it, and the second one closes a loop between them.

**Lever A's target** is a function, not a heuristic: `0x00642E80` **is** the first-person camera modifier's `Update` (vtable `0x01A5C4D8`, ctor `0x006323C0`, slot +0x28), reached under the build fingerprint the mod already verifies at startup. Runtime gates, all cheap and all structural:
- `Tier1::Latched()` — a real `xrLocateViews` has happened.
- `VR().Ready() && PlayerCameraActive()` — keeps it out of the main menu, the same predicate `frame_hook.cpp:741` uses to keep full-rate out.
- `lensOut` non-null and `lensOut[0] ∈ [0.05, 3.0] rad` before and after. Out of range -> skip and count.

There are ~13 camera modifier descriptor rows at `0x01A5C120..0x01A5C7F0`; only instances whose vptr is `0x01A5C4D8` reach this function, so no vptr comparison is needed — but log the `self` pointer's vptr once per session so a second camera type shows up as a fact rather than a surprise.

**The exactness comes from closing the loop at the GPU.** Add a read-only measurement in `Hook_Unmap` (`cbscan.cpp:4357`), where the mod already sees every constant-buffer write. Identify the shared camera block **by content, never by resource id** — `ResourceId::2528` holds a *shadow* matrix at eid 4600 and the camera matrix at eid 9151, and the engine cycles a small pool:

```cpp
bool IsSharedCameraClip(const float* m, float* aOut, float* bOut, float* nOut) {
    const float c0 = len3(m[0],m[4],m[8]);       // col0
    const float c1 = len3(m[1],m[5],m[9]);       // col1
    const float c2 = len3(m[2],m[6],m[10]);      // col2 - must be ZERO
    const float c3 = len3(m[3],m[7],m[11]);      // col3 - camera forward, unit
    const float n  = m[14];
    if (!(c2 < 1e-4f * maxf(c0, maxf(c1, c3)))) return false;
    if (fabsf(c3 - 1.0f) > 1e-3f) return false;                 // shared, not baked
    if (!(n > 0.005f && n < 0.02f)) return false;               // the camera's near
    if (!(c0 > 0.1f && c0 < 40.0f && c1 > 0.1f && c1 < 40.0f)) return false;
    *aOut = c0; *bOut = c1; *nOut = n; return true;
}
```

Gate it further on `t_size == 704` and require **the same (a,b) from at least two of the four offsets** +0x000 / +0x1d0 / +0x210 / +0x260 before publishing. Shadow matrices fail structurally (col3 = (0,0,0,1), col2 non-zero — sampled shadow WVP rows `(-0.010571,-0.035069,-0.000393,0) (0,0.105378,-0.000132,0) (-0.110607,0.003352,0.000038,0) (0.396256,-0.345363,0.484324,1.0)`). Per-object baked WVPs fail on `|col3| != 1`.

Then assert, once a second:

```
b_measured  ==  1 / ( k * tan(fovStockDeg/2) )   within 0.5%
```

**That is the identification standard.** The mod writes a float into an engine argument and then reads the consequence out of the GPU's own constant buffer. If the loop does not close within N frames, the lever is not reaching the renderer: log the two numbers, disable the lever, fall back to the clamped submission (2.6), and the picture is narrower but still correct. No guessing.

While that instrumentation is being added, fix `NoteProjection` (`cbscan.cpp:1588`): it keys on `(bufferSize, offset)` only and records no resource id, which is why `cbscan.cpp:1627` can claim the 192-byte buffer at +0x0000 holds a projection while `cbscan.cpp:1690` says it holds an affine matrix — `LooksLikeProjection` needs `m[15]==0` and `LooksLikeCloseModel` needs `m[15]==1`, so those are provably two different buffers sharing a size. Add the resource id. Until then "the 192-byte buffer" is an equivalence class, not a buffer, and nothing should be built on it.

---

## 4. THE GATE — shadows, reflections, UI

**Under Stages 1 and 2 the gate is free, and that is the strongest single argument for this route.** No projection matrix is written, so nothing can leak into a projection that should have been left alone. The only engine state changed is two floats inside the first-person camera modifier.

Specifically:
- **UI / Scaleform** does not read `camera+0x450`. The modifier is the first-person camera's; the UI path is separate. Untouched.
- **Shadow cascades** (`0x00736624`, ~510/s, flagged "never write to it" in `apex.h:399`) are built from the camera frustum. They *widen with it*, which is what you want — a wider camera frustum with unwidened cascades would leave shadows missing at the new edges. Watch cascade quality at the edges after Stage 1 (section 5) because the same cascade count now covers more world.
- **The 256x256 Y-flipped offscreen pass** (eids 3354-3625) binds the same `ResourceId::2528` with the same a/b and near = 0.009899. It widens consistently with everything else, which is correct. It is only a *trap for a matrix patch*, and we are not doing one.
- **The 1536x1536 shadow pass, and the 1024/512/128 and 1x1..64x64 mip chains** produced zero camera-clip hits across 1041 draws. Nothing there to protect.

**If Stage 3 is ever built**, the gate it needs is the one Stages 1-2 avoid: main viewport only (`RSGetViewports`, matched against the configured render size, not a learned resource id), inside the scene pass, structural test as in section 3, all four `GlobalConstants` copies plus terrain `cb1` +0x010 plus every per-object site, the previous-frame pair patched consistently with the current pair, and a per-frame assertion that the 1536x1536 viewport's hit count is still exactly 0. If a shadow draw ever starts matching, the detector has been loosened too far.

---

## 5. CULLING

**What the engine culls to:** `camera+0x450`, the committed world FOV in radians. `0x004B73C0` commits the view matrix (`camera+0x3D0`, via `0x0049E8D0`, whose `0x0049E8F1` is the mod's known `kEulerToMatrixRetView`) and the lens block (`camera+0x450`, written by `movups [rdi+0x450],xmm0` at `0x004B76F0`) in the same function, before any render work runs. `lensOut[0]` -> `camera+0x450` is a confirmed store; `settings int[0x38]` -> `params+0xA4` -> `lensOut[0]` is a confirmed chain (both handles resolve through the same helper `0xD7FB60`, stashed at `[rbp+0x68]` and `[rbp+0x78]`).

**Why widening it is expected to fix culling rather than merely move the problem:**
- **Terrain: proven to follow.** Patch culling happens in the hull shader from the same matrix, block `cb1` = `ViewDir_BackPatchCull@0, m_OffsetViewProjection@16, m_CameraPosition@80, TessOrigin@96, TessellationFactor*@112..128, BackPatchCullThreshold@132, FrustumCull@136, HalfRes@140`. The HS disassembly transforms every patch corner through `m_OffsetViewProjection[0..3]` before deciding. `m_OffsetViewProjection` at eid 12551 is bit-identical to `GlobalConstants` +0x1d0. Widen the FOV upstream and terrain's own rejection widens with it.
- **Vegetation: follows only if its planes are FOV-derived, which is the open question.** `VegSysCtx` `m_AABNormal` holds explicit plane equations built CPU-side from the camera basis (the normal 0.955875 and distance 2382.41 are the same numbers as the view matrix's col3/row3), and `m_LodFOV` = 0.863977 sits right next to `m_CameraWorldPosition`. Built from the camera *basis* is established; built from the camera *FOV* is not. If they are FOV-derived they widen for free; if they are not, grass and bushes pop at the new edges and `m_LodFOV` picks too coarse an LOD out there. Both are in a constant buffer reachable from the same `Map`/`Unmap` machinery if a patch turns out to be needed.
- **General object culling** happens CPU-side before `DrawIndexedInstancedIndirect` and is invisible in a capture. Unproven either way.

**The size of the problem if culling does NOT follow:** the covering frustum is 1.487/1.000 = **1.49x wider vertically** and 1.376/0.9254 = **1.49x wider horizontally** than what the engine draws today — a ~49% band of newly visible world all round. Not a corner case; it would be unmissable.

**THE TEST, and it needs no headset.** Run flat, monitor only, from an identical save and a fixed viewpoint.

1. **A/B the lever against itself.** `tier1_fov_k_override = 1.0` vs `1.5`, game FOV slider untouched, everything else identical. If the wide run shows fully-populated terrain, trees, rocks and grass right out to the new edges, culling follows the scalar and the whole trap is closed. Take the two screenshots from the same position and diff the edge bands.
2. **The falsifiable prediction.** If culling does *not* follow, the failure has a signature: **terrain stays solid to the new edge** (proven to follow) **while grass, bushes and small props pop in and out along a rectangular border** (the compute-shader planes). Seeing exactly that pattern confirms finding 2 and tells you precisely which buffer to patch. Seeing pop-in on terrain too means the FOV scalar is not the culling source at all and the assumption in this section is wrong.
3. **The negative control.** Set the lever k so the render is *deliberately 15 deg narrower* than the crop assumes, with the submitted frustum held fixed. If that pops at the edges and the matched case does not, culling is confirmed to follow `camera+0x450` and the lever is confirmed to be the fix. Without this control, "no pop" could just mean "nothing was near the edge".
4. **Pop vs streaming.** Stand still, pan slowly. Pop that is stationary in *world* space but appears/disappears at a fixed *screen* position is culling; pop that moves with the world is streaming and unrelated.
5. **Shadows.** After (1), check the shadow-cascade boundaries at the new edges — same cascade budget over 49% more world.
6. **Cost.** Log frame time in both runs. 49% more world in the frustum is not free, and the answer belongs in the preset descriptions.

Run 1, 2 and 3 **before** any headset session. They are cheap, they are decisive, and every downstream decision depends on the answer.

---

## 6. THE SUBMISSION SIDE

The comment at `vr.cpp:1289-1298` is correct *for the code it currently guards* and becomes wrong the moment Lever B lands. It must be replaced in the same commit, not contradicted. Proposed replacement, to sit where the old one is:

```
// *** EACH EYE IS SHOWN ITS OWN OFF-CENTRE RECTANGLE, SO EACH EYE GETS ITS
//     OWN TRUE FRUSTUM. ***
//
// The old rule here was: the game renders ONE image through ONE symmetric
// frustum, so both eyes must be shown it through the SAME symmetric frustum -
// handing each eye the runtime's own mirrored asymmetric FOV would put the
// image at a different angular position per eye and it would not fuse.
//
// That was true, and it stopped being true when the engine started rendering a
// frustum that CONTAINS both eyes' frusta (the FOV lever at 0x00642E80). A
// sub-rectangle of a symmetric perspective frame IS an off-centre sub-frustum,
// exactly. So each eye is copied its own rectangle - eye 0 the left part and
// the lower 84% of the height, eye 1 the right part and the same rows - and
// the FOV submitted with it is the one those pixels actually span, which is
// the runtime's own, straight out of xrLocateViews.
//
// The bounds are clamped to the image and the submitted FOV is derived BACK
// from the clamped bounds, so the two can never disagree. If the lever is off
// or a preset is too narrow, this degrades to the honest intersection of "what
// the headset wants" and "what was drawn": less field at that edge, still
// correct, still fuses. It never claims a degree that was not rendered.
//
// What this does NOT do is change the projection matrix. Nothing downstream of
// the camera is lied to, so culling, LOD, shadows, vegetation, TAA and the
// deferred passes' depth reconstruction all stay self-consistent. The price is
// that ~35% of the rendered pixels fall outside both eye rectangles.
```

Also update the preset commentary in `gamesettings.cpp:73-128`, which currently states as fact that "the vertical is FIXED at 90 whatever we do" and that "~4.5 deg of black at top and bottom is unavoidable". That was true of the settings-file route; the in-memory lever is a different path and is not clamped after `0x00642F8C`. Add a `TIER1` preset family at aspect 0.9638 (`2400x2490`, `2880x2988`, `3200x3320`) — the shape that minimises waste for this frustum — and leave the existing families alone so nobody's saved preset number changes meaning.

---

## 7. STAGING, WITH A DIAGNOSTIC PER STAGE

**Stage 0 — instrumentation only, no visual change.** Add `IsSharedCameraClip` reading at `Hook_Unmap`, plus the resource id in `NoteProjection`. Log once a second: measured vfov/hfov, aspect, near, which of the four offsets agreed, and the count of camera-clip windows seen.
*Diagnostic:* it must report **90.0 vertical / 85.6 horizontal / aspect 0.9254 / near 0.0094** on the FULLVIEW preset with the slider at 90. If it does not, the reader is wrong and nothing after this is trustworthy. **This stage is the measuring instrument for every stage after it.**

**Stage 1 — Lever A, flat, no headset.** `tier1_fov_k_override` as a raw float: run 1.0, 1.25, 1.5.
*Diagnostic:* Stage 0's readout must show vfov = `2*atan(k*tan(90/2))` within 0.5%, and the picture must visibly widen. The two failure modes are separately readable: **log changed, picture did not** = the lever writes a value the renderer does not use (wrong scalar); **picture changed, log did not** = the CB reader is wrong. Also log lever hit rate per frame — expect ~1.0; well below that means some frames render on a camera the hook never saw.

**Stage 1b — the culling A/B of section 5.** Before touching VR. Terrain solid + vegetation popping is the expected worst case and names its own fix.

**Stage 2 — Lever B, headset, `tier1_fov` off by default.** Per-eye crops and per-eye submitted fov.
*Diagnostic:* per eye, log the UV bounds, the pixel rect, **which sides clamped**, and the submitted fov in degrees against the runtime's. Unclamped they must be identical to 0.01 deg. A clamp on a side is a one-line statement of exactly how much field that edge is short.

**Stage 2b — the sign check, before judging comfort.** The vertical bound is the most likely thing to be mirrored (render-target Y down vs NDC Y up). Force the crop to the *top half only* via a debug key and confirm the headset shows the top half of the world. That failure is readable in one second; a flipped `v` shipped as-is presents as unexplained vertical disparity and eye strain, and gets blamed on IPD. The existing `F7` eye-swap and a new "swap crop only" toggle isolate crop-mirrored from pose-mirrored.

**Stage 2c — comfort pass, owner in the headset.** Per the standing rule, ask the owner to launch and send screenshots plus a description; do not auto-capture. Specific things to ask about: does the world fuse at arm's length as well as at distance; is there any vertical offset between the eyes; does the bottom of the view now reach further down than it used to.

**Stage 3 — optional, only after 1+2 ship.** The C-matrix CB patch to reclaim the 35%. Gate as in section 4, apply one C per eye per frame, and run the asymmetric numeric self-test from 2.7 in a unit test before it ever reaches the game. Expect this to take longer than Stages 0-2 combined, and expect the deferred passes (SSAO, fog, SSR, TAA) to be the thing that fights back.

---

## 8. THE OFF SWITCH

**One bool: `tier1_fov`, default `false` at first ship.** When false, the build must be behaviourally *identical* to today's, not approximately so:

- `Hook_CamModUpdate` returns immediately after calling the original — `lensOut[0]` and `lensOut[1]` untouched, so the engine FOV, culling, LOD, shadows and ADS zoom are all stock.
- `ComputeEyeCrops` is not called; the original `ComputeCrop` (`vr.cpp:395-423`) runs, producing the single centred `m_cropX/m_cropY/m_width/m_height`.
- Both eye swapchains are created at that one size; `CropBox()` returns the one box; both eyes copy the same region.
- `projViews[eye].fov = fov` — the shared frustum derived from `Cfg().game_fov_deg` and the image aspect, exactly as at `vr.cpp:1299-1338`.
- `gamesettings.cpp` keeps writing the resolution and the capped FOV as it does now.

The Stage 0 instrumentation stays live in both states — it is read-only and it is what makes a bug report legible.

Per the standing master-off-switch rule: this must be one switch that restores the old behaviour exactly, in the same build. Do not ship `tier1_fov` as three independent sliders (lever, crop, submitted fov) that individually add up to "off" — those combinations are not all valid, and a half-engaged state is the most confusing possible bug report.

---

## 9. WHAT IS NOT ESTABLISHED

Honest list. The first three are the ones that could invalidate the plan.

1. **That culling follows `camera+0x450`.** This is the load-bearing assumption of the whole route and it is a *hypothesis*. What is proven: `lensOut[0]` -> `camera+0x450` is a real store; terrain culls from a matrix derived from it. What is not proven: that general object culling, occlusion culling and vegetation plane construction derive from it, or that no separate culling FOV exists. Section 5 test 1+3 settles it in one flat session and costs nothing. **Run it before building Stage 2.**
2. **That `params+0xA4` is never clamped or stomped downstream.** All 8201 bytes of `0x00642E80` were scanned for `minss`/`maxss` and every clamp found is on registers or other fields — but `0x006442A3 movups [rsi+0xA4], xmm1` is a 16-byte store covering +0xA4..+0xB3 in the same function. If `rsi` ever aliases the params object it stomps `WorldFOV`. Unresolved. The plan sidesteps it by writing `lensOut` rather than `params`, but it is worth resolving before anyone reaches for the cheaper `params+0xA4` poke.
3. **That the vegetation cull planes and `m_LodFOV` are FOV-derived.** Their derivation from the camera *basis* is measured; their dependence on the FOV *scalar* is not. This is the most likely place for edge pop, and section 5's test 2 identifies it by signature.
4. **The vertical sign of the crop's `v` bounds.** All measured shear in the captures is exactly 0.00000, so a symmetric capture cannot settle render-target Y direction versus NDC Y. Stage 2b settles it empirically; getting it wrong reads as eye strain, not as an obvious break.
5. **Performance cost.** 49% more world inside the frustum, ~35% of rendered pixels discarded. Unmeasured. It may force lower presets.
6. **The 192-byte and 64-byte "projection" writes in the mod's own logs** (`cotwvr.pid*.log`: "90.0 deg vertical | 64-byte buffer +0x0000 | 6966 total", 192-byte at 60.0-90.0). No sampled instant in any capture holds a pure projection, but a Map-hook sees every write and a capture sees only a few instants, and 90.0 vertical is exactly the `b = 1.00000` derived from V*P. This is unresolved, it does **not** affect the plan (world draws provably read premultiplied matrices, so a standalone P would not move world geometry even if it exists), and the instrumentation fix in section 3 is what would resolve it. Do not let either "there is no projection anywhere" or "the 192-byte buffer is the projection" be used as a premise until the resource id is recorded.
7. **That `0x00642E80` is the only camera modifier that matters.** ~13 descriptor rows exist at `0x01A5C120..0x01A5C7F0`. Vehicle, scripted and cutscene cameras were not checked. If one bypasses this modifier, the FOV lever will silently not apply during that camera and the crop will over-claim — which is exactly the case the clamp in 2.6 is there to make survivable and visible.
8. **Separability: settled, negatively.** There is no standalone projection matrix in any constant block. This is as strong a negative as this method can produce: 1041 draws, all bound blocks, all stages, two captures, plus all 26,397 buffers <= 64 KB at 11 events, row-major and transposed, using the mod's own `LooksLikeProjection` predicate. If a later capture ever finds one, the plan does not change — the world draws still read premultiplied matrices.

---

## FILES

- Mod source to change: `<this repo>\src\cotwvr\vr.cpp` (ComputeCrop 395-423, CreateSwapchains 425, the fov block 1289-1338, the crop copy 1474, the submit 1490-1523), `cbscan.cpp` (Hook_Unmap 4357 for the measurement, NoteProjection 1588 for the resource id), `gamesettings.cpp` (73-135 comments, preset table), `apex.h` (new constants), plus a new `tier1.h/.cpp`.
- New apex constants to add: `kCameraModUpdate = 0x00642E80`, `kCameraModVtable = 0x01A5C4D8`, `kSettingsPtr = 0x028064D8`, `kSettingsArrayOffset = 0xFD8`, `kSettingIndexFov = 0x38`, `kGetIntSetting = 0x00AEA520`, `kCameraLensFovOffset = 0x450`, `kCameraViewMatrixOffset = 0x3D0`, `kParamsForeGroundFov = 0xA0`, `kParamsWorldFov = 0xA4`, `kParamsUseGameSettingsWorldFov = 0xA8`.
- Evidence: RenderDoc scripts and raw output in a scratch folder (`scan3..scan8`, `vfy1..vfy5`); PE/disassembly in the same tree's `pe.py`, `cmat.py` (the C-matrix numeric proof), `vb.py`. Not kept in the repository - the conclusions below are what survived.
- Vanilla ADF cross-check: `<this repo>\modbuilder_2.7.2\modbuilder\_internal\org\editor\entities\cameras\default_first_person.ctunec` — file 0x100 = 33.0 (ForeGroundFOV), 0x104 = 60.0 (WorldFOV), 0x108 = 1 (UseGameSettingsWorldFov, so the game-settings path is the live one by default).