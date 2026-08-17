# Copy the built mod into the game folder, and prove the copy actually landed.
#
# "Verify the build artefact changed before deploying" is a rule paid for in
# wasted headset sessions: testing a stale DLL and drawing conclusions from it
# is indistinguishable from the mod not working.

param(
    # The game folder. Set COTWVR_GAME once and every script here follows it,
    # e.g.  setx COTWVR_GAME "D:\Steam\steamapps\common\theHunterCotW"
    [string]$Game = $(if ($env:COTWVR_GAME) { $env:COTWVR_GAME }
                      else { "C:\Program Files (x86)\Steam\steamapps\common\theHunterCotW" }),
    [string]$Build = (Join-Path (Split-Path $PSScriptRoot -Parent) "build")
)

$ErrorActionPreference = "Stop"

$files = @("cotwvr.dll", "XINPUT9_1_0.dll", "openxr_loader.dll")

# *** THE LAUNCHER IS PART OF THE MOD AND WAS NOT BEING DEPLOYED. ***
#
# It lives in launcher\dist rather than build\, so it sat outside this list and
# the copy in the game folder quietly stayed six days behind - long enough to
# test the wrong window and believe a change had not landed. It is only rebuilt
# when PyInstaller runs, so it is deployed only when the built one differs from
# the deployed one, and skipped in silence otherwise.
$launcherName = "theHunterCotW VR Settings.exe"
$launcherSrc = Join-Path (Split-Path $PSScriptRoot -Parent) "launcher\dist\$launcherName"

if (-not (Test-Path (Join-Path $Game "theHunterCotW_F.exe"))) {
    Write-Host "[!] theHunterCotW_F.exe not found in $Game" -ForegroundColor Red
    exit 1
}

# A running game holds the DLLs open; the copy fails and the old build stays.
$proc = Get-Process -Name "theHunterCotW_F" -ErrorAction SilentlyContinue
if ($proc) {
    Write-Host "[!] theHunter CotW is running (PID $($proc.Id)). Close it first." -ForegroundColor Red
    exit 1
}

Write-Host "Deploying to $Game" -ForegroundColor Cyan
$fail = $false
foreach ($f in $files) {
    $src = Join-Path $Build $f
    $dst = Join-Path $Game  $f
    if (-not (Test-Path $src)) {
        Write-Host "  [!] missing from build: $f" -ForegroundColor Red
        $fail = $true
        continue
    }
    Copy-Item -LiteralPath $src -Destination $dst -Force

    $hs = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
    $hd = (Get-FileHash -LiteralPath $dst -Algorithm SHA256).Hash
    $len = (Get-Item -LiteralPath $dst).Length
    $stamp = (Get-Item -LiteralPath $src).LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss")
    if ($hs -eq $hd) {
        Write-Host ("  OK  {0,-20} {1,10:N0} bytes  built {2}  sha {3}" -f $f, $len, $stamp, $hs.Substring(0,12)) -ForegroundColor Green
    } else {
        Write-Host "  [!] $f readback MISMATCH - the file on disk is not what we built" -ForegroundColor Red
        $fail = $true
    }
}

if (Test-Path -LiteralPath $launcherSrc) {
    $ldst = Join-Path $Game $launcherName
    $lhs = (Get-FileHash -LiteralPath $launcherSrc -Algorithm SHA256).Hash
    $lhd = if (Test-Path -LiteralPath $ldst) {
        (Get-FileHash -LiteralPath $ldst -Algorithm SHA256).Hash } else { "" }
    if ($lhs -ne $lhd) {
        # The settings window may be open - it is not the game, so nothing above
        # catches it, and a locked copy would otherwise fail the whole deploy.
        try {
            Copy-Item -LiteralPath $launcherSrc -Destination $ldst -Force
            $len = (Get-Item -LiteralPath $ldst).Length
            $stamp = (Get-Item -LiteralPath $launcherSrc).LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss")
            Write-Host ("  OK  {0,-20} {1,10:N0} bytes  built {2}  sha {3}" -f "VR Settings.exe", $len, $stamp, $lhs.Substring(0,12)) -ForegroundColor Green
        } catch {
            Write-Host "  [!] could not replace $launcherName - close the settings window" -ForegroundColor Red
            $fail = $true
        }
    } else {
        Write-Host ("  --  {0,-20} already current" -f "VR Settings.exe") -ForegroundColor DarkGray
    }
}

if ($fail) { exit 1 }

# The camera templates are embedded in cotwvr.dll (see camera_assets.h), so
# nothing needs installing beside the game. Clean up the folder an earlier
# version of this script created.
$oldAssets = Join-Path $Game "cotwvr_assets"
if (Test-Path $oldAssets) {
    Remove-Item $oldAssets -Recurse -Force
    Write-Host "  removed cotwvr_assets (templates now live inside the DLL)" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Log for the next run: $env:LOCALAPPDATA\theHunterCotWVR\cotwvr.log" -ForegroundColor Cyan
Write-Host "(previous run is kept as cotwvr.prev.log)"
