# Deploy Atlas to a connected Android device or Meta Quest and forward it to
# a local port, so the page opens in an ordinary desktop browser while every
# search runs on the device's own Arm core.
#
#   .\demo\run_on_android.ps1
#   .\demo\run_on_android.ps1 -Serial <id> -Port 8080
#
# Then open http://localhost:8080 (or, on the headset itself, the in-VR
# browser pointed at http://localhost:8080 — the forward makes the loopback
# address work identically inside the headset).
#
# Copyright 2026 The Vane Authors. Apache License 2.0.
param(
    [string]$Serial = "",
    [int]$Port = 8080
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sdk  = "$env:LOCALAPPDATA\Android\Sdk"
$adb  = "$sdk\platform-tools\adb.exe"
$cm   = "$sdk\cmake\3.22.1\bin\cmake.exe"
$nj   = "$sdk\cmake\3.22.1\bin\ninja.exe"
$ndk  = Get-ChildItem "$sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1

$adbArgs = @(); if ($Serial) { $adbArgs = @("-s", $Serial) }
if (-not (& $adb devices | Select-String "\tdevice$")) {
    Write-Host "No device detected. Enable USB debugging and accept the RSA prompt." -ForegroundColor Red
    Write-Host "For Quest: enable developer mode in the Meta Quest phone app first."
    exit 1
}

if (-not (Test-Path "$root\demo\atlas.bin")) {
    throw "demo/atlas.bin missing. Run: python demo/atlas_pack.py <glove.txt> demo/atlas.bin"
}

$bd = "$root\build_android"
if (-not (Test-Path $bd)) { New-Item -ItemType Directory -Force -Path $bd | Out-Null }
Push-Location $bd
try {
    & $cm -G Ninja -DCMAKE_MAKE_PROGRAM="$nj" `
        -DCMAKE_TOOLCHAIN_FILE="$($ndk.FullName)\build\cmake\android.toolchain.cmake" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 `
        -DCMAKE_BUILD_TYPE=Release $root
    if ($LASTEXITCODE -ne 0) { throw "configure failed" }
    & $cm --build .
    if ($LASTEXITCODE -ne 0) { throw "build failed" }
} finally { Pop-Location }

$dev = "/data/local/tmp/atlas"
& $adb @adbArgs shell "mkdir -p $dev"
foreach ($f in @("vane.so", "atlas_server")) {
    & $adb @adbArgs push "$bd\$f" "$dev/$f" | Out-Null
}
& $adb @adbArgs push "$root\demo\atlas.bin"  "$dev/atlas.bin"  | Out-Null
& $adb @adbArgs push "$root\demo\atlas.html" "$dev/atlas.html" | Out-Null
& $adb @adbArgs shell "chmod 755 $dev/atlas_server"

# Kill a stale instance from a previous run, if any, then launch fresh.
#
# 'pkill atlas_server', NOT 'pkill -f atlas_server'. Android's toolbox pkill
# does not implement -f the way procps does: on a Redmi K20 Pro (Android 11)
# the -f form matched far more than intended and took down the adb daemon
# along with it, which silently dropped the port forward. The symptom was a
# server that ran perfectly on-device while localhost refused to connect.
& $adb @adbArgs shell "pkill atlas_server" 2>$null | Out-Null
Start-Sleep -Seconds 1

Write-Host "Starting atlas_server on-device (port $Port, forwarded to this machine)..." -ForegroundColor Cyan
# Build the argument array element by element. Joining $adbArgs into a string
# yields "" when no -Serial was given, and Start-Process rejects an
# ArgumentList whose first element is empty — which broke the default
# invocation, the one both READMEs document.
$srvArgs = @()
if ($Serial) { $srvArgs += @("-s", $Serial) }
$srvArgs += @("shell", "cd $dev && LD_LIBRARY_PATH=$dev ./atlas_server --port $Port")
Start-Process -NoNewWindow -FilePath $adb -ArgumentList $srvArgs | Out-Null

# Forward AFTER the server is up, and verify it took. Establishing it before
# launch is legal but leaves no way to tell a lost forward from a dead server.
Start-Sleep -Seconds 5
& $adb @adbArgs forward "tcp:$Port" "tcp:$Port" | Out-Null
if (-not (& $adb @adbArgs forward --list | Select-String "tcp:$Port")) {
    Write-Host "Port forward did not take. Retry: adb forward tcp:$Port tcp:$Port" -ForegroundColor Red
    exit 1
}

Start-Sleep -Seconds 2
try {
    $caps = (Invoke-WebRequest -Uri "http://localhost:$Port/api/caps" -UseBasicParsing -TimeoutSec 10).Content
    Write-Host ""
    Write-Host "Serving. Device reports: $caps" -ForegroundColor DarkGray
} catch {
    Write-Host "Server not answering on localhost:$Port yet - give it a moment and reload." -ForegroundColor Yellow
}

Write-Host ""
Write-Host "Open: http://localhost:$Port" -ForegroundColor Green
Write-Host "(On the headset's own browser, the same localhost URL reaches this forward.)"
