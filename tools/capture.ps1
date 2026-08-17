# Fire a RenderDoc frame capture in the running game.
#
# The person playing just plays - they get to a useful in-game moment and the
# capture is taken from outside, with no hotkey to fumble for and no overlay in
# the captured backbuffer.
#
# Requires: the capture_mode marker existed when the game launched (see
# tools/capture_mode.ps1), so the mod loaded RenderDoc instead of its own hooks.

$dir = "$env:LOCALAPPDATA\theHunterCotWVR"

if (-not (Get-Process theHunterCotW_F -ErrorAction SilentlyContinue)) {
    Write-Host "[!] the game is not running" -ForegroundColor Red
    exit 1
}
if (-not (Test-Path "$dir\capture_mode")) {
    Write-Host "[!] capture_mode marker is absent - the game launched with VR hooks," -ForegroundColor Red
    Write-Host "    not RenderDoc. They cannot coexist. Create the marker and relaunch." -ForegroundColor Red
    exit 1
}

$before = @(Get-ChildItem "$dir\captures\*.rdc" -ErrorAction SilentlyContinue)
New-Item -ItemType File -Force -Path "$dir\capture.trigger" | Out-Null
Write-Host "trigger dropped - the next frame will be written" -ForegroundColor Cyan

for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Milliseconds 500
    $now = @(Get-ChildItem "$dir\captures\*.rdc" -ErrorAction SilentlyContinue)
    if ($now.Count -gt $before.Count) {
        $new = $now | Sort-Object LastWriteTime | Select-Object -Last 1
        Write-Host ("captured: {0}  ({1:N0} bytes)" -f $new.Name, $new.Length) -ForegroundColor Green
        Write-Host $new.FullName
        exit 0
    }
}

Write-Host "[!] no .rdc appeared within 30s. Check cotwvr.log for [rdoc] lines." -ForegroundColor Red
exit 1
