# Build for arm64-v8a, push to a connected Android device, run the verifier
# and the benchmark, and save the UNEDITED output to results/.
#
# The output files this produces are the submission's evidence. Do not edit
# them by hand — if a number looks wrong, fix the code and re-run.
#
#   .\scripts\run_on_android.ps1 -Label s23-ultra
#   .\scripts\run_on_android.ps1 -Label quest2 -Seconds 300   # sustained run
#
# Copyright 2026 The Vane Authors. Apache License 2.0.

param(
    [Parameter(Mandatory = $true)][string]$Label,
    [double]$Seconds = 5.0,
    [int]$Rows = 512,
    [int]$Cols = 4096,
    [string]$Serial = "",
    [ValidateSet("", "dequant_gemv_int4", "gemv_int8", "quantize_int4")]
    [string]$Op = "",
    # Skip the verifier. Only for repeat runs on a device that already
    # captured a passing verify in the same session.
    [switch]$BenchOnly
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$sdk  = "$env:LOCALAPPDATA\Android\Sdk"
$adb  = "$sdk\platform-tools\adb.exe"
$cm   = "$sdk\cmake\3.22.1\bin\cmake.exe"
$nj   = "$sdk\cmake\3.22.1\bin\ninja.exe"

$ndk = Get-ChildItem "$sdk\ndk" -Directory | Sort-Object Name | Select-Object -Last 1
if (-not $ndk) { throw "No Android NDK found under $sdk\ndk" }

foreach ($t in @($adb, $cm, $nj)) {
    if (-not (Test-Path $t)) { throw "Missing required tool: $t" }
}

$adbArgs = @()
if ($Serial) { $adbArgs = @("-s", $Serial) }

$devices = & $adb devices | Select-String -Pattern "\tdevice$"
if (-not $devices) {
    Write-Host "No device detected." -ForegroundColor Red
    Write-Host "  1. Enable Developer Options, then USB debugging."
    Write-Host "  2. Connect by USB and accept the RSA prompt on the device."
    Write-Host "     Quest: enable developer mode in the Meta Quest phone app first."
    Write-Host "  3. Re-run. Use -Serial <id> if more than one device is attached."
    exit 1
}

# ---- build -----------------------------------------------------------------
$bd = Join-Path $root "build_android"
if (Test-Path $bd) { Remove-Item -Recurse -Force $bd }
New-Item -ItemType Directory -Force -Path $bd | Out-Null

Push-Location $bd
try {
    & $cm -G Ninja -DCMAKE_MAKE_PROGRAM="$nj" `
        -DCMAKE_TOOLCHAIN_FILE="$($ndk.FullName)\build\cmake\android.toolchain.cmake" `
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 `
        -DCMAKE_BUILD_TYPE=Release $root
    if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

    & $cm --build .
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
} finally { Pop-Location }

# ---- push ------------------------------------------------------------------
# /data/local/tmp is executable on stock Android and on Quest.
$dev = "/data/local/tmp/vane"
& $adb @adbArgs shell "mkdir -p $dev"
foreach ($f in @("vane.so", "vane_probe", "vane_verify", "vane_bench")) {
    & $adb @adbArgs push "$bd\$f" "$dev/$f" | Out-Null
}
& $adb @adbArgs shell "chmod 755 $dev/vane_probe $dev/vane_verify $dev/vane_bench"

# ---- run -------------------------------------------------------------------
$results = Join-Path $root "results"
New-Item -ItemType Directory -Force -Path $results | Out-Null
$out = Join-Path $results "$Label.txt"
$env_prefix = "cd $dev && LD_LIBRARY_PATH=$dev"

$header = @(
    "vane device run",
    "label      : $Label",
    "captured   : $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')",
    "ndk        : $($ndk.Name)",
    "abi        : arm64-v8a",
    "adb model  : $((& $adb @adbArgs shell getprop ro.product.model).Trim())",
    "adb device : $((& $adb @adbArgs shell getprop ro.product.device).Trim())",
    "android    : $((& $adb @adbArgs shell getprop ro.build.version.release).Trim())",
    "soc        : $((& $adb @adbArgs shell getprop ro.soc.model).Trim())",
    "args       : --seconds $Seconds --rows $Rows --cols $Cols$(if ($Op) { " --op $Op" })",
    "",
    "This file is captured verbatim from the device. Nothing in it is edited.",
    ("=" * 78), ""
)
$header | Set-Content -Encoding utf8 $out

"--- vane_probe ---"                        | Add-Content -Encoding utf8 $out
(& $adb @adbArgs shell "$env_prefix ./vane_probe")  | Add-Content -Encoding utf8 $out
""                                            | Add-Content -Encoding utf8 $out

$verifyRc = "skipped"
if (-not $BenchOnly) {
    "--- vane_verify ---"                       | Add-Content -Encoding utf8 $out
    (& $adb @adbArgs shell "$env_prefix ./vane_verify") | Add-Content -Encoding utf8 $out
    $verifyRc = (& $adb @adbArgs shell "$env_prefix ./vane_verify >/dev/null 2>&1; echo `$?").Trim()
    "verify exit code: $verifyRc"                 | Add-Content -Encoding utf8 $out
    ""                                            | Add-Content -Encoding utf8 $out
}

$opArg = if ($Op) { " --op $Op" } else { "" }
"--- vane_bench ---"                        | Add-Content -Encoding utf8 $out
(& $adb @adbArgs shell "$env_prefix ./vane_bench --seconds $Seconds --rows $Rows --cols $Cols$opArg --report $dev/report.json") | Add-Content -Encoding utf8 $out

# No 2>&1 here. adb writes its progress line to stderr, and Windows PowerShell
# wraps redirected native stderr in ErrorRecords — which $ErrorActionPreference
# = "Stop" then treats as fatal, aborting the script after every capture and
# skipping the verify summary below.
& $adb @adbArgs pull "$dev/report.json" (Join-Path $results "$Label.json") | Out-Null

Write-Host ""
if ($verifyRc -eq "0") {
    Write-Host "VERIFY PASSED - all available paths agree with the scalar oracle" -ForegroundColor Green
} elseif ($verifyRc -eq "skipped") {
    Write-Host "VERIFY SKIPPED (-BenchOnly) - these numbers are only trustworthy if a" -ForegroundColor Yellow
    Write-Host "passing verify was captured for this device in results/" -ForegroundColor Yellow
} else {
    Write-Host "VERIFY FAILED (exit $verifyRc) - do not publish these results until fixed" -ForegroundColor Red
}
Write-Host "results/$Label.txt"
Write-Host "results/$Label.json"
