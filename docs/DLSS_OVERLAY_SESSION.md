# The DLSS dev-overlay session — what to run and what to look for

Set up 2026-08-15, to answer the one question eight eliminated suspects did not:
**why does giving each eye its own sub-rectangle upset presets K and J, while L
and M are unaffected?**

## The state before this session

| | |
|---|---|
| Symptom | crop ON + K/J = shimmer on head movement. crop OFF = clean on every preset. crop ON + M/L = clean. |
| Eliminated by measurement | jitter scale (was a real bug, fixed, not this), jitter source (exact vs estimator), per-frame pose latch, 6DoF, AER history age (full-rate made no difference), head-rotation compensation, the reconstruction rectangle moving (latched - fixed a double-vision bug of its own, did not fix this) |
| Known good | crop ON + M or L: full vertical field AND clean image. This is what ships regardless of what the session finds. |

## What was set up

1. **The DEV DLSS DLL is now deployed.** The game folder had the RELEASE build
   (58,977,904 bytes) - the dev build (69,725,808) was sitting unused in
   `thirdparty\dlss\dev\`. That is why no overlay ever appeared. The release
   build is kept beside it as `nvngx_dlss.release.dll`.
2. **The debug registry key was already on**: `ShowDlssIndicator = 1`,
   `LogLevel = 1` under `HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore`.
3. **`ENABLE_DLSS_VERBOSE_LOG.reg`** added beside the game - it raises
   `LogLevel` to 2, which records the parameters of every EVALUATE (subrect,
   jitter, MV scale, flags) rather than only feature creation. Needs
   administrator (it writes HKLM).

## Already confirmed from the level-1 log

The preset hint now lands on **all five** quality levels, which was a real bug
fixed earlier today:

    Info: (DLAA) Using App hint Preset K
    Info: (Quality) Using App hint Preset K
    Info: (Balanced) Using App hint Preset K
    Info: (Perf) Using App hint Preset K
    Info: (UltraPerf) Using App hint Preset K

So the preset row genuinely reaches DLSS at every level. Whatever the fault is,
it is not the preset failing to apply.

## Running it

1. Double-click **ENABLE_DLSS_VERBOSE_LOG.reg**, accept the prompt.
2. Launch. Set **preset K** and **"cut each eye its own view" ON** - the failing
   combination, deliberately.
3. In the world, standing still, press **CTRL+ALT+F12** to bring up NVIDIA's own
   overlay. It draws into the DLSS output, so it appears in the headset.
4. Look around until the shimmer is obvious, then read the overlay.

## What to read off it

The overlay reports what NGX believes about its own inputs. The four that matter
here, in the order they would explain the fault:

- **Render subrect / resolution.** Should be each eye's crop, identical from
  frame to frame. Anything varying is the rectangle moving under the history.
- **Jitter.** Should be a sub-pixel value changing every frame in a 16-phase
  pattern. A constant, a zero, or a value larger than one pixel is wrong.
- **Motion vector scale and format.** Ours are in PIXELS, current -> previous,
  with `MVLowRes` set. If NGX believes they are UV-space or full-res, every
  vector is wrong by a factor of the resolution.
- **Exposure.** `AutoExposure` is set; the overlay says whether it agrees.

`F6` runs the accumulation-convergence test and `F9` shows the jitter
transforms, per the dev DLL's own hotkeys.

## Afterwards

- Run **DISABLE_DLSS_DEBUG.reg** to stop the verbose log (it grows fast).
- Put the release DLL back:
  `copy nvngx_dlss.release.dll nvngx_dlss.dll` in the game folder. The dev build
  is slower and draws an indicator; it is not what to play on.

## If the overlay says everything is correct

Then our inputs are right and preset K simply dislikes this arrangement, which
is a legitimate finding and the end of the hunt: ship crop ON with M or L. The
alternative - moving the upscale inside the resolve, onto the HDR image before
the HUD is drawn - is a large change to the post chain and would be justified
only by a fault this session actually finds.
