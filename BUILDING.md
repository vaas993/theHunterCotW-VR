# Building theHunter: Call of the Wild — VR

You do **not** need any of this to play the mod. Download the release, copy the
files into the game folder, done. This is for reading the code, changing it, or
building it yourself.

## What you need

| | |
|---|---|
| **Visual Studio 2019 Build Tools** | with the C++ x64 toolchain. The build script expects `vcvarsall.bat` at the default path; edit `build.bat` if yours differs |
| **Python 3** | on `PATH` — the build runs three checks written in Python |
| **PyInstaller** | only if you want to rebuild the settings launcher: `pip install pyinstaller` |

## The three SDKs

These are **not in this repository** — they belong to other people, and
redistributing them is not mine to do. Download each one and put it where the
build expects it:

```
thirdparty/
  openxr/     Khronos OpenXR SDK (Apache 2.0)
              https://github.com/KhronosGroup/OpenXR-SDK/releases
              needs: include/, native/x64/release/{lib,bin}
  minhook/    MinHook (BSD 2-clause)
              https://github.com/TsudaKageyu/minhook
              needs: include/, src/
  dlss/       NVIDIA DLSS SDK
              https://github.com/NVIDIA/DLSS
              needs: include/, lib/x64/, bin/nvngx_dlss.dll
```

Only `nvngx_dlss.dll` ships in a release, which NVIDIA's licence allows. The
SDK itself stays on your machine.

## Build

```
build.bat
```

Artifacts land in `build/`: `cotwvr.dll`, `XINPUT9_1_0.dll`, `openxr_loader.dll`.

The build runs three checks first, and **fails** on any of them. They are not
ceremony — each one exists because the mistake it catches has already cost a
day:

- **`tools/checkconfig.py`** — every setting must be both read from and written
  to `cotwvr.ini`. A setting that can be read but never saved comes back off
  after every exit, which is indistinguishable from a broken feature.
- **`tools/checkrecommend.py`** — the in-game panel and the launcher each have a
  "recommended settings" button, written in different languages in different
  files. This fails the build if they disagree, because two recommendations that
  disagree put a player on a configuration nobody has ever run.
- **The `/utf-8` flag** — the sources *are* UTF-8, and without telling MSVC so,
  every non-ASCII character in a string becomes mojibake in the headset.

## The rest of the toolchain

```
tools/deploy.ps1              copy the build into the game folder, verified by hash
tools/uninstall.ps1           remove every file the mod installs
tools/make_release.py         assemble the release folder and zip
tools/audit_release.py        check a package before it is uploaded
tools/compare_builds.py       are two builds of the game the same code at the same addresses?
launcher/cotwvr_launcher.py   the settings window (plain Python + tkinter)
```

## Where to start reading

`docs/` is the honest history of the project — what was tried, what failed, and
why. Start with `ARCHITECTURE.md`, then `FEATURES_AND_FINDINGS.md`.

The engine addresses all live in **`src/cotwvr/apex.h`**, and nowhere else. Each
one carries how it was found and what it does. They are RVAs into
`theHunterCotW_F.exe` for one specific build, verified at runtime by a
fingerprint (SizeOfImage + PE timestamp); on any other build every one of them
is refused and the mod installs nothing. That is why a game patch — or a
different store's build — needs the addresses found again rather than a
recompile.
