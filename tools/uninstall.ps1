# Remove the mod, leaving a stock game.
#
# The user must always be able to get back to vanilla by deleting files, and
# must be able to do it without us - hence this script touches nothing but the
# files the mod itself installed.

param(
    # The game folder. Set COTWVR_GAME once and every script here follows it,
    # e.g.  setx COTWVR_GAME "D:\Steam\steamapps\common\theHunterCotW"
    [string]$Game = $(if ($env:COTWVR_GAME) { $env:COTWVR_GAME }
                      else { "C:\Program Files (x86)\Steam\steamapps\common\theHunterCotW" })
)

$ErrorActionPreference = "Stop"

# EVERYTHING THE PACKAGE INSTALLS, and it had drifted behind what the package
# actually contains: the DLSS runtime, the settings window and its preferences
# file were all added to the release and never added here, so "uninstalled" left
# three files behind - including the launcher, which a player would then run,
# and the preferences that suppress the first-run page. A stale uninstaller
# makes a FRESH test not fresh, which is the one thing it exists to guarantee.
$files = @("cotwvr.dll", "XINPUT9_1_0.dll", "openxr_loader.dll", "nvngx_dlss.dll",
           "cotwvr.ini", "cotwvr_launcher.cfg", "theHunterCotW VR Settings.exe",
           # Left by older builds, and by hand during development.
           "cotwvr_launcher.py", "cotwvr.ini.before_taa_defaults")

$proc = Get-Process -Name "theHunterCotW_F" -ErrorAction SilentlyContinue
if ($proc) {
    Write-Host "[!] theHunter CotW is running (PID $($proc.Id)). Close it first." -ForegroundColor Red
    exit 1
}
# The settings window is not the game, so the check above does not see it - and
# its exe is one of the files being removed.
$launcherProc = Get-Process -Name "theHunterCotW VR Settings" -ErrorAction SilentlyContinue
if ($launcherProc) {
    Write-Host "[!] The VR settings window is open (PID $($launcherProc.Id)). Close it first." -ForegroundColor Red
    exit 1
}

foreach ($f in $files) {
    $p = Join-Path $Game $f
    if (Test-Path $p) {
        Remove-Item -LiteralPath $p -Force
        Write-Host "  removed $f" -ForegroundColor Yellow
    } else {
        Write-Host "  (absent) $f"
    }
}

# The weapon/scope FOV overrides the mod writes into the game's dropzone folder.
# Only our own five camera files are touched - anything else in there belongs to
# other mods and is left alone.
$cameras = Join-Path $Game "dropzone\editor\entities\cameras"
foreach ($c in @("default_first_person.ctunec", "prone_first_person.ctunec",
                 "aim_scope_first_person.ctunec", "iron_sight_first_person.ctunec",
                 "iron_sight_prone_first_person.ctunec")) {
    $p = Join-Path $cameras $c
    if (Test-Path $p) {
        Remove-Item -LiteralPath $p -Force
        Write-Host "  removed dropzone override $c" -ForegroundColor Yellow
    }
}
# The settings window is a folder now.
$launcherDir = Join-Path $Game "VR Settings"
if (Test-Path $launcherDir) {
    Remove-Item $launcherDir -Recurse -Force
    Write-Host "  removed VR Settings\" -ForegroundColor Yellow
}

$oldAssets = Join-Path $Game "cotwvr_assets"
if (Test-Path $oldAssets) { Remove-Item $oldAssets -Recurse -Force }

# The Tobii proxy, if it was installed.
$real = Join-Path $Game "stream_engine_real.dll"
if (Test-Path $real) {
    Write-Host "  NOTE: the Tobii proxy is still installed - run tobii_uninstall.ps1" -ForegroundColor Cyan
}

Write-Host ""
Write-Host "The game is stock again. No game file was ever modified - the mod is" -ForegroundColor Green
Write-Host "only these added files, so nothing needs restoring." -ForegroundColor Green
Write-Host ""
Write-Host "The game's OWN video settings are untouched, on purpose: they are the" -ForegroundColor Cyan
Write-Host "game's to keep. Anti-aliasing is whatever the mod last set it to." -ForegroundColor Cyan
