# theHunter: Call of the Wild — VR

**Native OpenXR stereo VR for theHunter: Call of the Wild.** The world is drawn
from the game's own camera, once per eye — real depth, your head turns the view,
you can lean and step around inside it, and your weapon has depth.

Not a screen-in-a-void wrapper.

*a VR mod by Vaas993*

---

## Download

**[Download the latest release](../../releases/latest)** — one zip, drop it in the
game folder.

---

## Requirements

| | |
|---|---|
| **Game** | theHunter: Call of the Wild, update **9.2 (Peru Hunting Reserve)**, Steam |
| **Headset** | Anything with an OpenXR runtime — Meta Quest Link, SteamVR, Virtual Desktop |
| **DLSS** | Optional. Needs an NVIDIA **RTX** card; there is an alternative for every other card |
| **Mode** | **Single player only.** Do not use it in multiplayer sessions |

This is a work in progress. Expect rough edges — the known ones are listed below.

---

## Install

1. Download the zip and unpack it.
2. Copy **every file** into the game folder, beside `theHunterCotW_F.exe`:
   ```
   ...\steamapps\common\theHunterCotW\
   ```
3. Run **`VR Settings	heHunterCotW VR Settings.exe`** — the settings window
   lives in that subfolder. Make a desktop shortcut to it if you like.
4. Read the page it opens with, press **Use recommended**, then **Save**, then
   **Launch game**.
5. Put the headset on before the game finishes loading.

Nothing in the game is modified. The mod is only the added files, so uninstalling
is deleting them.

---

## Anti-aliasing — you do not need DLSS

The launcher and the in-game panel both offer three, and the mod sets the game's
own anti-aliasing to match whichever you pick:

| Choice | Needs | What you get |
|---|---|---|
| **NVIDIA DLSS** | RTX card | Best picture, and the only one that can buy frames back by upscaling |
| **Per-eye smoothing (mod)** | any card | The mod replaces the game's temporal pass with its own, which keeps each eye's history separate — smoothing without the cross-eye ghosting. **Use this if you have no RTX card.** |
| **Off — the game's own** | — | Nothing from the mod. Sharp, aliased, foliage shimmers |

---

## Controls

| Key | |
|---|---|
| **Insert** (or Ctrl+Alt+O) | open the settings panel in the headset |
| **Pause** | recentre the view — use it whenever forward stops being forward |
| **Delete** | show the flat game screen, for menus and the map |
| **Alt** (held) | free look — the view turns, the weapon stays put |

Everything can be rebound in the panel, and the panel works with a gamepad.

---

## Antivirus warnings

**2 of 65 engines** flag the download; 63 pass it clean. Scanned file by file,
it is narrower than that:

| File | Detections |
|---|---|
| The settings window, and every file it needs | **0 / 70** |
| `openxr_loader.dll`, `nvngx_dlss.dll` (NVIDIA-signed) | **0 / 70** |
| `cotwvr.dll` | 1 / 70 |
| `XINPUT9_1_0.dll` | 2 / 70 |

Both remaining detections are unnamed generic labels from two of the highest
false-positive engines in the set. No engine identifies a malware family.

The two that are flagged are the two that do the actual work. `XINPUT9_1_0.dll`
is a small DLL named after a system library that loads another DLL — which is
how the game loads the mod, and also how a certain kind of malware works.
`cotwvr.dll` then patches engine code in memory to render one eye at a time.
Heuristic scanners recognise the *technique*; they are not identifying anything
in particular, which is why every major engine passes it and only high
false-positive scanners do not. Nothing is code-signed.

[VirusTotal scan of the current release](https://www.virustotal.com/gui/file/a890351d521b9bcfddf5ed30cc88f84cfca2e8c52dde064d09eeb753b298453a) — and the entire source is in this
repository: read it, or build it yourself with [BUILDING.md](BUILDING.md).

---

## Known issues

- **Objects can shake or shimmer with DLSS on** — worst on close-up geometry and edges. Weapon depth makes it worse - it confuses the picture pass about which camera drew the frame, and the result is seen as the WORLD shaking rather than the weapon. Two things help: DLSS models M and L reduce the shaking noticeably, and turning weapon depth off on the WEAPON tab removes its share of it entirely.
- **The HUD is drawn flat across the whole view** — rather than at a comfortable distance. The switches that move it are in the in-game panel and currently break more than they fix, which is why they carry a warning.
- **Leaning does not collide with anything** — 6DoF moves the camera, not the character, so lean far enough and you will lean through a wall. The travel limit in the panel is the guard.
- **The scope's magnified picture is positioned from your head** — not from the gun - so holding free-look while aimed slides it off the scope.
- **Sizes and shapes are worked out for a Quest 3** — other headsets run fine but may lose some field or waste pixels. Use the CUSTOM resolution and raise "How much wider" until it fills the view.

---

## If something goes wrong

The mod writes a log to:

```
%LOCALAPPDATA%\theHunterCotWVR\cotwvr.log
```

The previous run is kept beside it, and the settings launcher has an **Open log
folder** button. It records what was detected, which hooks took and every setting
the mod changed. **Include it in any bug report — it usually contains the
answer.**

- **Black screen in the headset, game fine on the monitor** — HDR is on. Turn it
  off in Windows display settings.
- **The mod starts nothing at all** — the game has probably been updated past
  this build. The mod identifies the game by fingerprint and refuses to guess.
- **You want the game back to normal without uninstalling** — set **VR mod** to
  *off* in the launcher. Every file stays where it is.

---

## Uninstall

Delete `cotwvr.dll`, `XINPUT9_1_0.dll`, `openxr_loader.dll`, `nvngx_dlss.dll`,
`cotwvr.ini`, `cotwvr_launcher.cfg` and the `VR Settings` folder. Nothing else
is touched.

---

## Credits

Built by **Vaas993**.

Includes NVIDIA DLSS (`nvngx_dlss.dll`, redistributed under the NVIDIA DLSS SDK
licence), the Khronos **OpenXR** loader (Apache 2.0) and **MinHook** (BSD
2-clause). theHunter: Call of the Wild is a trademark of Expansive Worlds /
Avalanche Studios; this mod is unofficial and not affiliated with them.
