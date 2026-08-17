# theHunter: Call of the Wild — native VR mod

Working notes. Follows the playbook in
`<your VR reference folder> Patches&Mods\VR APPS\VR documents\BUILDING_A_VR_MOD.md`, written after
the Far Cry 2 VR mod; section numbers below refer to it.

---

## 1. Game assessment (§1)

Measured 2026-08-01, not assumed.

| Check | Result | Consequence |
|---|---|---|
| **Anti-cheat** | None. No EAC/BattlEye/Denuvo-AC binary anywhere in the tree. EOSSDK is present for Epic Online Services (co-op), but that is a service SDK, not an anti-cheat. | Project is feasible. Keep to single-player anyway. |
| **Bitness** | **x64** (`Machine 0x8664`, PE32+) | 64-bit OpenXR loader, MinHook built with `hde64.c`. |
| **Graphics API** | **D3D11 only.** Imports `D3D11CreateDevice` + `CreateDXGIFactory/1/2`. No D3D12, no Vulkan strings in the exe. The `d3d9.dll` import is a single `D3DPERF_SetOptions` (a profiling no-op), not a renderer. | **The easy path.** OpenXR binds D3D11 natively — no private device, no shared textures, no DXVK. This is the thing Far Cry 2 did not have. |
| **Engine** | Avalanche **Apex Engine**, statically linked into the exe (no engine DLL). `archives_win64/*.arc`, ADF save format, 25 `Apex` strings. | All RVAs are relative to `theHunterCotW_F.exe`. |
| **Imports usable as a proxy** | `XINPUT9_1_0.dll` (XInputGetState, XInputSetState), `DINPUT8.dll`, `WINMM.dll`, `dxgi.dll`, `d3d11.dll` | See "Proxy choice" below. |
| **Threading** | Not yet established. Needed before full-rate stereo (§11). | Open question. |
| **FOV configurable** | Not yet established. | Open question. |

### Build fingerprint

Every RVA is worthless without this, and the game patches silently.

```
theHunterCotW_F.exe    41,850,880 bytes
ImageBase              0x140000000
SizeOfImage            0x02A13000
PE TimeDateStamp       0x6A5A5133   (2026-07-17 15:58:43 UTC)
Steam appid            518790
Steam buildid          24282372
versions.518791.txt    code_final 3304878 / archives 3296380
Sections               .text 0x1000 (vsz 0x1923DDF), .rdata 0x1925000, .data 0x25AF000
```

`apex::Init()` checks `SizeOfImage` and `TimeDateStamp` at runtime. On a
mismatch it logs loudly and `apex::Rva()` returns `nullptr` for everything, so a
stale address can never be called after a game update.

### Prior art (§3)

Searched 2026-08-01. **There is no native VR mod for this game.** Only vorpX
(a reprojection driver with an official G3D profile) — not stereo from the
engine's own camera, and not what this project is.

No VR mod exists for any Apex Engine title, so nothing transfers wholesale.
The transferable references are architectural, from the mods studied for
Far Cry 2 (all in `K:\...\VR documents\other vr framworks\`):

- **BioShock VR** — D3D11 x64, `dxgi.dll` proxy, ships an 18 KB ini. Closest
  technical shape to this game.
- **Halo MCC VR** — the Model B reference: floating true-aim reticle, native
  crosshair hidden, comfort turning.
- **FEAR VR**, **MELE1 VR** — packaging and install/uninstall shapes.

---

## 2. Architecture decisions

### Proxy choice: `XINPUT9_1_0.dll`

The game statically imports `XInputGetState`/`XInputSetState` from it, so it
loads at process start, before the renderer.

Chosen over the more usual `dxgi.dll` because **this machine has several other
tools that all want to be `dxgi.dll`** — ReShade, geo-11, vorpX, and the
NOMOREFLAT mod collection. XInput collides with none of them, loads just as
early, and hands us the gamepad for the settings panel for free.

The proxy forwards all four real exports (ordinals 2–5 preserved) and its only
addition is `LoadLibrary("cotwvr.dll")`. If that fails it writes
`%LOCALAPPDATA%\theHunterCotWVR\proxy_error.log`, because "no log file at all"
is otherwise indistinguishable between *never loaded*, *deleted by antivirus*
and *failed to resolve an import*.

### Head-tracking model (§2) — NOT YET COMMITTED

The decision everything depends on. Deliberately deferred until the camera is
found (checkpoint 5), because the choice is only real once we know whether the
engine's camera orientation is a writable **input** or a derived **output**.

Leaning **Model A** (the head *is* the game's camera). CotW is a game about
aiming a scoped rifle at a distant animal: aim precision is the whole loop, and
Model B's "head looks, stick aims + floating reticle" compromise is a much worse
fit here than it is in an arcade shooter. If the camera basis turns out to be
derived output, Model B is the fallback.

**Model C is forbidden** — driving the game's view to *equal* the head via
injected input. It is a target-seeking loop that fights any other input device
on the same axis. Far Cry 2 spent days there; see the playbook §2.

---

## 3. Layout

```
src/cotwvr/
  dllmain.cpp      entry; startup thread; COTWVR_PadPostProcess export
  log.cpp/.h       timestamped log -> %LOCALAPPDATA%\theHunterCotWVR\cotwvr.log
  config.cpp/.h    cotwvr.ini beside the DLL; unknown keys ignored
  apex.cpp/.h      ALL engine addresses + the build fingerprint check
  render_hook.cpp/.h   DXGI vtable hooks: Present (slot 8), ResizeBuffers (13)
  vr.cpp/.h        OpenXR: instance, session, swapchains, frame loop
src/proxy/
  proxy.cpp, xinput9_1_0.def
thirdparty/        minhook, openxr (copied from the Far Cry 2 VR project)
tools/             deploy.ps1, uninstall.ps1
build.bat          x64, VS2019 BuildTools
```

Deliberate rules:

- **Every engine address lives in `apex.h`**, as an RVA, with a comment saying
  how it was found and what its calling convention is.
- **Log from the first line of code**, rotating the previous run to
  `cotwvr.prev.log`.
- **Nothing heavy in `DllMain`** — it spawns a thread and returns.
- The Present hook **always calls the original**, headset or not, so the desktop
  window never freezes.

---

## 4. Build order (§20) — progress

| # | Step | Proves | State |
|---|---|---|---|
| 1 | Proxy loads, writes a log | injection | **VERIFIED 2026-08-01** |
| 2 | Present hook fires | we own the frame | **VERIFIED 2026-08-01** |
| 3 | OpenXR session, blank layer | the runtime path | **built** (`blank_layer_test = 1`) |
| 4 | Backbuffer copied to both eyes | the pipeline | **built** (mono in VR) |
| 5 | Find the camera; offset position per eye | **stereo** | **camera FOUND, position lever proven; alternate-eye rendering next** |
| 6 | Find the orientation input; apply head rotation | head tracking | not started — the Model A/B fork |
| 7 | **Commit to Model A or Model B** | whether it feels right | not started |
| 8 | 6DOF, FOV, comfort, settings panel | playability | not started |
| 9 | Full-rate stereo | smoothness | not started |
| 10 | Reticle, HUD, weapon separation | polish | not started |

---

### First run, measured (flat, `submit_to_headset = 0`)

```
[  0.004] log opened                      <- injection works
[  0.004] apex fingerprint OK             base 0x7FF7B8580000, stamp 0x6A5A5133
[  0.279] DXGI hooks installed
[  0.394] first Present - we own the frame
[  0.395] backbuffer 3840x2880 format 28 (R8G8B8A8_UNORM) windowed=1 buffers=1
[  0.395] backbuffer is 8-bit -> CopyResource into the XR swapchain will work
```

Facts worth keeping:

- **Injection is fast and clean** — hooks in place 279 ms after process start,
  frame ownership at 394 ms, well before the renderer does anything interesting.
- **HDR is off.** The backbuffer is `R8G8B8A8_UNORM`, so the single most common
  cause of "black headset, perfect monitor" is ruled out *before* anyone put a
  headset on.
- **The backbuffer is 3840x2880, windowed.** Two consequences: eye swapchains
  made at that size cost ~44 MB per image (≈265 MB for two triple-buffered
  eyes), and the runtime's own recommended per-eye size is likely much smaller.
  Matching the backbuffer is right for now because it makes the copy a straight
  `CopyResource`; switching to the recommended size later needs a scaling blit.
- **Frame pacing**: ~187 fps at the menu once loaded, first 600 frames spread
  over 15 s of loading. Do not kill a test run before ~20 s.

### Runtime environment (this machine)

- Active OpenXR runtime: **VDXR** —
  `C:\Program Files\Virtual Desktop Streamer\OpenXR\virtualdesktop-openxr.json`.
  It returns `XR_ERROR_FILE_ACCESS_ERROR` when the streamer is not connected to
  a headset; that is not our bug.
- Implicit API layers registered: `ReShade64_XR.json` and Virtual Desktop's
  Oculus compatibility layer. ReShade's manifest declares
  `"disable_environment": "DISABLE_XR_APILAYER_reshade_1"` — which is exactly
  the variable `StartupThread` sets, verified against the JSON rather than
  assumed.

### The camera, found by measurement (2026-08-01)

Static RE got as far as the engine's two generic transform builders (see
`apex.h`). It could not say which of their 43 call sites was the camera, so that
was **measured**: `camera_probe.cpp` records per call site the calls/frame,
distance travelled and basis swing, then displaces one site at a time by 5 m
while a human looks.

| caller | calls/frame | displaced 5 m → | verdict |
|---|---|---|---|
| `0x006445CC` | 0.65 | **picture clearly moves** | **the position lever** |
| `0x006DBDC8` | 0.98 | picture moves slightly | tracks the view; weak lever |
| `0x004B6F11` | 0.65 | nothing | body transform |
| `0x00736624` | ~3 | nothing (swing 128k) | shadow cascades — the §9.4 trap |
| `0x006FD788`, `0x00C26398` | <2 | nothing | rotation-only transforms |

Two facts fell out of the positions, and both are gifts:

- The camera sits **exactly 1.72 above** the body transform at the same X/Z ⇒
  the world is **Y-up, in metres**. OpenXR is also Y-up, so the basis change is
  far cheaper than Far Cry 2's Z-up Dunia, and IPD/6DOF need no scale factor.
- The axis test confirmed **row 1 is up** ("definitely straight up").

**The trap that the axis test exposed.** Offsetting along the mover's *own*
row 0 was reported as "forward-**left**" — a diagonal, which a camera-relative
axis can never be. Measured in the same frame:

```
mover  0x006445CC  row0 = ( 0.792,  0.000, -0.610)
camera 0x006DBDC8  row0 = (-1.000, -0.005,  0.000)
```

The mover is **body-aligned**, and the body faces a different way from the view.
So stereo must **read the world basis from `0x006DBDC8`** (which tracks the view
perfectly) and **write the position at `0x006445CC`** (which is what actually
steers the picture) — one object for orientation, a different one for position.
Had the axis test returned a clean "right", that mismatch would have gone
unnoticed until the two eyes refused to fuse.

---

## 4b. Smoothness: submit the pose the frame was *actually* drawn from

**Symptom.** Head tracking looked correct but felt slightly juddery — and only
while the head was *moving*. Holding still was clean.

**Cause — an ordering bug, not a tuning problem.** Both of these run inside the
Present hook, in this order:

| line | call | what it does |
|---|---|---|
| `render_hook.cpp:109` | `VR().SubmitFrame(sc)` | locates the head pose, submits the finished image with it, then caches that pose in `m_headPose` |
| `render_hook.cpp:148` | `HeadTrackTick()` | reads `m_headPose` and computes the angles written into the game camera |

So the angles that rotate the camera for frame *N* come from the pose located at
present *N−1*. But `SubmitFrame` was submitting the *freshly located* pose with
that image. The runtime was being told "this was rendered from here" about a
viewpoint the pixels never saw.

A projection layer's pose is not decoration: the compositor reprojects the image
from the pose you declare to the pose actually reached at scan-out. Declare the
wrong one and it reprojects from the wrong origin — off by exactly **one frame of
head motion**. Zero error while the head is still, growing with angular velocity.
That is precisely a judder you only notice while turning.

**Fix** (`vr.cpp`, `submit_rendered_pose`, overlay: *"Smoother head motion"*).
Keep `m_headPose` before it is overwritten — that value *is* the pose the
outgoing frame was rendered from — and put it in `projViews[eye].pose`:

```cpp
const XrPosef renderedFrom = m_headPose;      // what these pixels really saw
const bool haveRenderedFrom = InterlockedCompareExchange(&m_headValid, 1, 1) != 0;
m_headPose = views[0].pose;                   // now update it for the NEXT frame
...
projViews[eye].pose =
    (Cfg().submit_rendered_pose && haveRenderedFrom) ? renderedFrom : monoPose;
```

Falls back to `monoPose` on the first frame, when there is no previous pose yet.

**Bonus.** Correcting the declaration also lets the runtime cancel the one-frame
staleness, so this reduces effective latency as well as judder — reprojection
could always do that; it was simply being aimed from the wrong origin.

**Confirmed in the headset 2026-08-04**: *"smoothness is better"*. Off by default
(`submit_rendered_pose`) so it can be A/B'd live from the overlay.

**The general lesson.** Whenever this mod hands the runtime a pose, the question
is never "where is the head now" but "what did the image in my hand actually
see". Those differ by a frame here because rendering is driven by a hook that
runs *after* submission.

---

## 5. Traps already guarded against

- **HDR** — a 10/16-bit backbuffer cannot `CopyResource` into an 8-bit XR
  swapchain: the headset goes black while the monitor looks perfect.
  `VRSystem::Init` detects the format family and says so explicitly in the log.
- **Format mismatch** — the eye swapchain format is *chosen* to match the
  backbuffer (exact match first, then same typeless family), never assumed.
- **`xrPollEvent`** — pumped every frame; skipping it strands the session in
  `IDLE`, rendering nothing, with no error anywhere.
- **Begin/End balance** — `xrEndFrame` runs on every path, including
  `shouldRender == false`, with `layerCount = 0`.
- **`ResizeBuffers`** — hooked; drops the eye swapchains so they rebuild at the
  new size.
- **MinHook `ALREADY_CREATED`** — treated as success, not failure, so a hook can
  be toggled off and back on.
- **ReShade's OpenXR layer** — `DISABLE_XR_APILAYER_reshade_1` is set for this
  process only, so ReShade keeps working everywhere else.
- **Re-entrant Present** — only the outermost call owns the frame.
- **Two `back->Release()` sites in `SubmitFrame`** — one of them is inside the
  "could not create eye swapchains" failure branch and only runs while VR is
  shutting down. Code attached to that one never executes. Anchor on the
  `// Alternate-eye flips here` comment that follows the *correct* release.
- **Per-XInput-slot state** — `COTWVR_PadPostProcess` is called once per
  controller slot, not once per frame. Any button state kept as a single value is
  wiped by the next slot: Steam Input's virtual pad reports all zeros and makes
  a held button read as a fresh press every poll.
- **Raw Input ignores keyboard hooks** — `WM_INPUT` is delivered before the hook
  chain, so a low-level hook cannot stop a modern game reading keys. Hook
  `GetRawInputData`/`GetRawInputBuffer` as well.
- **Swallowed keys are invisible to `GetAsyncKeyState`** — including to the mod
  itself. Panel key reads go through `PanelKeyDown()` (`async || hookTable`).
- **The deploy copy is refused while the game runs** — read the deployed file's
  hash back and compare it to the build, or you will test a stale DLL.

---

## 6. Where the rest is written down

`FEATURES_AND_FINDINGS.md`, alongside this file, covers every feature the mod
grew and what each one cost to get right:

| section | subject |
|---|---|
| 1 | Head roll — and why `+0x54` was a guess worth exposing as a setting |
| 2 | Smoothness (detail in §4b above) |
| 3 | The flat menu screen, and the menu-detection signals that do and do not work |
| 4 | Day length, without writing an unverified multiplier field |
| 5 | **Input capture — the three ways a game reads a key, and the one no hook can block** |
| 6 | Bindings for keys and pad buttons, and three bugs with one root cause |
| 7 | The overlay panel's layout rules |
| 8 | The crouch ratchet: located by measurement, still open |
| 9 | How this project actually makes progress — measure, diff, verify the deploy |

Section 5 is the most reusable outside this project; section 9 is the one most
worth reading before changing anything.
