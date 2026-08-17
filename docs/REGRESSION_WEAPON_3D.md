# Regression: weapon 3D strength and weapon FOV stopped working

## FOUND, 2026-08-10 evening

**Cause: the six HUD lines inserted into each of the four draw hooks in
`cbscan.cpp`.** Removing them restores the weapon shift; everything else from
that afternoon - the crop reorder, scope-mono, iron sights, the recentre ramp and
settle delay - is innocent and still in the build that works.

The lines were:

```
    HudProbeQuad(ctx, vh, 0);
    if (HudIsolateSkip(ctx, vh)) return;
    if (Cfg().hud_hide_sunflare && ctx && HudIsSunFlareDraw(ctx)) return;
    if (Cfg().hud_hide_glare && HudLooksLikeHud(ctx, vh) &&
        HudIsGlareDraw(ctx)) return;
    HudMove hudMove_(ctx, vh);
```

**Every one is gated on config that is off, so they should have been inert.**

## NARROWED, same evening - it is one of the two DIAGNOSTICS

Re-adding only three of the six lines - the two toggles and `HudMove` - restored
the HUD controls with the weapon shift still working, confirmed in the headset.

So the culprit is one of the two that were NOT re-added:

```
    HudProbeQuad(ctx, vh, 0);          // reads the vertex buffer via a staging copy
    if (HudIsolateSkip(ctx, vh)) return;
```

`HudProbeQuad` is the likelier of the two by a wide margin. Even gated, it is the
only one of the six that does a `CopyResource` and a `Map` on the context - and
these hooks run on DEFERRED contexts, where a `D3D11_MAP_READ` is not valid.
That is also why its own output was garbage when it ran: NaNs and absurd
magnitudes, one sane row in 400. A call that corrupts context state explains a
weapon shift that stops working two functions later, and it explains it whether
or not the gate let the body run this frame.

`HudIsolateSkip` is the fallback suspect: it queries depth-stencil and blend
state and a shader-resource view, and releases each. Less likely, but not ruled
out.

**Neither is needed again.** Both were diagnostics, both have already produced
their answers, and they are staying out. If either is ever wanted back, it must
not sit in the draw hooks - and `HudProbeQuad` must not use a staging read on a
deferred context at all.

## The insertion-point theory was WRONG

The first version of this note blamed where the lines were placed - the classic
statement-after-an-unbraced-`if`. All four sites were then read directly: every
one is a plain statement context with nothing above it that could capture or
displace a statement. Recorded so the wrong mechanism is not carried forward.

Prime suspect before the test was the settle delay in `HeadTrackTick`, on the
grounds that it could explain the weapon AND the aim-steadiness failure at once.
It was wrong: the diff shows no early return and no skipped pose publish. Recorded
so the same wrong lead is not followed twice.

## The original report (kept)


Found 2026-08-10 evening. Bisected to a window, not yet to a change.

## The symptom

The WEAPON 3D tab's **3D strength** and **weapon field of view** sliders have no
effect in the headset. The values save correctly - `weapon_3d = 1`,
`weapon_3d_amount = 0.1300`, `weapon_view_scale = 0.560` are all present and
correct in `cotwvr.ini` - so this is not a settings problem. The shift is either
not running or running with no effect.

**Aim steadiness was reported as also not working in the same session.** Two
independent features - the weapon shift lives in the draw hooks, the smoother
lives in `HeadTrackTick` - so a shared cause is more likely than two faults.

## What the bisect established

| build | weapon 3D |
|---|---|
| `backups\2026-08-10_BEFORE-CROP-ORDER-FIX` (10:42, 627,200 B) | **WORKS** |
| `backups\2026-08-10_WORKING-SCOPE-MONO-AND-HUD` (17:58, 644,608 B) | broken |

Owner confirmed both, in the headset, same ini, same launcher. The DLL is the
only thing that changed between the two tests.

Also ruled out by direct test: **`scope_mono`**. The owner turned it off and the
sliders were still dead, so the scoped-IPD change in `stereo.cpp` is not it.

## The suspect list - everything that landed in that window

1. **The crop-order fix** (`vr.cpp`) - the per-eye FOV is now derived from the
   final integer rectangle instead of from the fractions.
2. **The recentre ramp** - `config.h`, `headtrack.cpp` (`RecentreRampFraction`,
   `g_rampLeft`), `camera_probe.cpp` (the slice multiply in `TryRecentreCredit`).
3. **The settle delay** - `headtrack.cpp`, the speed watcher inserted into the
   tick just before `SteadyAim(m)`.
4. **The HUD work in `cbscan.cpp`** - four new calls at the top of all four draw
   hooks: `HudProbeQuad`, `HudIsolateSkip`, the glare and sun-flare skips, and
   the `HudMove` RAII object.
5. **`scope_mono` / iron sights** (`stereo.cpp`) - ruled out above, but the file
   was edited.
6. **The menu cursor debounce** (`vr.cpp`).
7. Config plumbing for all of the above - roughly a dozen new keys, several
   inserted as standalone blocks near the head of `Assign()`.

Launcher changes in the same window cannot be involved: they do not affect the
DLL.

## Where to look first

**Item 3 is the strongest lead, because it explains BOTH failures at once.** The
settle watcher was inserted into `HeadTrackTick` immediately before the
`SteadyAim(m)` call. If that block returns, continues, or otherwise short-cuts
the tick, the smoother stops running - and anything later in the same tick stops
with it. That single insertion is the only edit in the window that sits upstream
of both a head-tracking feature and the per-frame state the weapon shift reads.

Check in this order:

1. Read `HeadTrackTick` from the top of the settle block to the end of the
   function. Confirm nothing added returns early or skips the publish of
   `g_head` / `InterlockedExchange(&g_haveHead, 1)`.
2. Turn on `weapon_pass_diag = 1` on the BROKEN build and read the counters -
   they say whether the weapon pass is being found at all, which separates "the
   shift runs and does nothing" from "the shift never runs".
3. Set `aim_steady_scoped_only = 0` on the broken build. If the smoother works
   then, only the gate is wrong; if not, the smoother itself is not running.
4. If the log is silent, bisect the source: the two backups differ by a known,
   small set of edits, and items 1-4 above can be reverted one at a time.

## Restoring

The working build is `backups\2026-08-10_BEFORE-CROP-ORDER-FIX\cotwvr.dll`.
The broken one is kept at `backups\2026-08-10_CURRENT-BEFORE-ROLLBACK\` so the
comparison stays available.

What the working build does NOT have: HUD move/size, the glare and sun-flare
toggles, scope-mono and iron sights, the recentre ramp and settle delay, the
crop reorder. Everything up to and including recentre, aim steadiness and the
HUD probe is present.
