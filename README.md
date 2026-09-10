# theHunter: Call of the Wild — VR

Native OpenXR stereo VR mod by **Vaas993**. Explore the game in stereoscopic 3D with head tracking, 6DoF camera movement, and adjustable weapon depth.

## Download version 1.3

[**Download v1.3**](https://github.com/vaas993/theHunterCotW-VR/releases/tag/v1.3) — get `theHunterCotW-VR-v1.3.zip`.

Version 1.3 is a regular release. Neural Rendering remains an optional experimental feature with specific runtime/driver requirements.

## New in 1.3

- **Live DLSS source-size changes:** Quality, Balanced, Performance, Ultra Performance, DLAA and Custom. Supported changes no longer require restarting.
- **Improved native DLAA switching**, including NR with DLAA / Custom 100% on the tested configuration.
- **Optional Neural Rendering (NR)** before DLSS upscaling or native DLAA, tested with full-rate stereo and AER. NR enhances the image and costs GPU time; it is **not frame generation**.
- **NR working scales:** 50%, 75%, 85%, 90% and 100%. Lower values can reduce cost but soften detail, without changing the headset output size.
- **Clearer overlay:** grouped settings and a prominent **[ APPLY CHANGES ]** button directly below Quality/Custom. It turns amber when selected values have not been submitted.
- **Launcher 1.3:** VR & output, Picture & NR, and Setup & notes tabs. Includes NR settings and prevents competing launcher saves while the game runs.
- Hidden-overlay refresh optimization and a small full-output NR composition optimization. Performance gains depend on your configuration; no universal FPS increase is promised.

## Requirements

- theHunter: Call of the Wild, update **9.2 / Peru Hunting Reserve**.
- An OpenXR runtime such as Meta Quest Link, SteamVR or Virtual Desktop.
- **Single player only.**
- NVIDIA RTX hardware for optional DLSS. The mod also provides per-eye smoothing without DLSS.
- Main test configuration: Quest 3, RTX 5090. Other combinations are not broadly validated.

## Install or upgrade

1. Close the game and launcher. Back up existing mod files and `cotwvr.ini`.
2. Extract the release ZIP beside `theHunterCotW_F.exe`.
3. The ZIP contains fresh recommended settings. Keep your backed-up INI instead if you want to preserve your current configuration.
4. Open `VR Settings/theHunterCotW VR Settings.exe`. Launcher saves apply at the next launch.
5. Start your OpenXR runtime, then launch the game.
6. Check the game's video settings against the launcher's **Setup & notes** tab. **FXAA + TAA** is required for DLSS and the mod's replacement temporal pass.

## Applying changes in game

In **PICTURE**, choose a DLSS quality mode or Custom source scale. Select **[ APPLY CHANGES ]**, then press **Enter**, **controller A**, or **left/right**.

The amber **CHANGES WAITING** cue means your choices have not been submitted. After Apply, check **Live status** and **Resize status**: submitting a request is not proof that both eyes have activated the requested size.

Use that same Apply button after changing **NR working scale**. NR enable and the reconstruction model are separate controls.

**Headset/output resolution in VIEW is still fixed at launch and requires restart.** It is not the same as DLSS source scale or NR working scale.

## Optional Neural Rendering setup

NVIDIA's **`nvngx_dlssnr.dll` is not included**. Acquire it separately from a source you are entitled to use, under its provider's terms, and place it here:

```text
<game folder>/cotwvr-nr/nvngx_dlssnr.dll
```

Our companion bridge, `cotwvr-nr/nvngx.dll_dlssnr.dll`, is included. Do not rename an ordinary DLSS DLL or copy a driver-core DLL into the game folder.

This NR integration accepts only the tested runtime, bridge and driver-core SHA-256 fingerprints. **A driver update can make NR decline to start even if it might otherwise be compatible.** See the [release notes](https://github.com/vaas993/theHunterCotW-VR/releases/tag/v1.3) for exact hashes and the tested driver version. Broader compatibility is not yet validated.

NR is **off by default**. Ordinary DLSS works without the optional NR runtime. When enabling NR, check **NR status**—the switch alone does not prove it is running. Try 100% for full working detail, then 90% or 85% if you need lower GPU cost. Leave GPU timing off except when measuring.

## Controls

| Default control | Action |
| --- | --- |
| Insert / Ctrl+Alt+O | Open overlay |
| Pause | Recenter |
| Delete | Show flat game screen |
| Alt, held | Free look |
| Enter / controller A | Activate the selected Apply button |
| Left/right | Adjust a setting or activate Apply |

Bindings can be changed in the overlay.

## Known limitations and troubleshooting

- AER can produce temporal mismatch around moving objects. The separate hitch/frame-pacing investigation remains deferred.
- Lower NR scales can look softer. An unsafe NR/device fault requires restarting; toggling NR off does not clear it.
- Earlier DLAA cloud flicker was not reproduced in later testing; a dedicated cloud fix is not claimed.
- HUD placement controls remain unfinished. 6DoF moves the camera without collision, so you can lean through walls.
- Scope alignment can shift with head-based free look. View presets were tuned for the tested Quest 3 setup.
- A black headset image with a normal monitor does **not** by itself diagnose HDR or a driver problem. Include stereo/AER mode, DLSS quality, NR status and recent changes in your report.

Logs: `%LOCALAPPDATA%/theHunterCotWVR/cotwvr.log`, with the previous run beside it. The launcher includes **Open log folder**.

No new VirusTotal scan is claimed for v1.3. Scan results from older releases do not apply to these files. The mod uses injection/hooking and its binaries are unsigned.

## Source and credits

The v1.3 release includes a separate **Source ZIP** containing the corresponding current mod, bridge and launcher code plus build support. Use that asset for this build: this repository's main source tree and GitHub's automatically generated source archives have not yet been synchronized with the newer experimental candidate. See the source ZIP's `BUILD-THIS-CANDIDATE.txt` for its build entry point and external dependencies.

Built by Vaas993. Mod source is under GPL-3.0; third-party components retain their own licenses. Includes the NVIDIA DLSS SR runtime, the Khronos OpenXR loader, and MinHook. The optional NVIDIA NR runtime is not redistributed in the public package.

Unofficial project; not affiliated with NVIDIA, Expansive Worlds or Avalanche Studios.

## Uninstall

Close the game. Remove the mod's `cotwvr.dll`, `XINPUT9_1_0.dll`, `openxr_loader.dll`, `nvngx_dlss.dll`, `cotwvr.ini`, `cotwvr_launcher.cfg`, `VR Settings` and `cotwvr-nr` folders/files. Keep your backups if needed. Restore desired video settings in the game.
