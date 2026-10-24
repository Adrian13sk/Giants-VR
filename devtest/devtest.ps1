<#
  GiantsVR on-device test runner (Windows PowerShell 5.1+).

  Installs the APK on the Quest connected by USB, then runs each test script on the headset: the
  app plays the script (no one needs to wear the headset) and writes frame times, GPU times and
  screenshots; this pulls them, with the app's log, into devtest\results\<date-time>\.

  Usage (from the folder this script is in):
    .\devtest.ps1                      # all tests in .\tests
    .\devtest.ps1 -Tests perf_1a_village,kabuto_k1a
    .\devtest.ps1 -Sweep               # also every mission for 20 s (needs the game's Bin folder)
    .\devtest.ps1 -NoInstall           # keep the installed APK

  If Windows refuses to run scripts:  powershell -ExecutionPolicy Bypass -File .\devtest.ps1
#>
param(
    [string]$Apk = (Join-Path $PSScriptRoot "..\GiantsVR-dev.apk"),
    [string[]]$Tests = @(),
    [switch]$Sweep,
    [switch]$NoInstall,
    [string]$GameBin = "C:\GOG Games\Giants - Citizen Kabuto\Bin",
    [int]$SweepSeconds = 20
)
$ErrorActionPreference = 'Continue'
$pkg = 'io.github.giantsvr.dev'
$files = "/sdcard/Android/data/$pkg/files"

$adb = (Get-Command adb -ErrorAction SilentlyContinue).Source
if (-not $adb) { $adb = Join-Path $env:USERPROFILE 'Documents\platform-tools\adb.exe' }
if (-not (Test-Path $adb)) { Write-Host "adb not found (looked in PATH and $adb)"; exit 1 }
function Adb { & $adb @args 2>$null }

$state = (Adb get-state) -join ''
if ($state -ne 'device') { Write-Host "No Quest found by adb (state '$state'). Plug it in and allow USB debugging."; exit 1 }
$serial = (Adb get-serialno) -join ''

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$outRoot = Join-Path $PSScriptRoot "results\$stamp"
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null
$summary = Join-Path $outRoot 'summary.txt'
"GiantsVR device test $stamp, Quest $serial" | Set-Content $summary

if (-not $NoInstall) {
    if (-not (Test-Path $Apk)) { Write-Host "APK not found: $Apk"; exit 1 }
    $hash = (Get-FileHash $Apk -Algorithm SHA256).Hash.ToLower()
    Write-Host "Installing $Apk ($hash)"
    $r = (Adb install -r $Apk) -join ' '
    "install: $r (apk sha256 $hash)" | Add-Content $summary
    if ($r -notmatch 'Success') { Write-Host "Install failed: $r"; exit 1 }
}

# Keep the headset awake and "worn" while nobody wears it (undone at the end).
Adb shell input keyevent KEYCODE_WAKEUP | Out-Null
Adb shell am broadcast -a com.oculus.vrpowermanager.prox_close | Out-Null

# The test list.
$scripts = @()
$testDir = Join-Path $PSScriptRoot 'tests'
if ($Tests.Count -gt 0) { $scripts = @($Tests | ForEach-Object { Join-Path $testDir "$_.txt" }) }
else { $scripts = @(Get-ChildItem $testDir -Filter *.txt | Sort-Object Name | ForEach-Object { $_.FullName }) }
# Mission name prefix -> world (as scripts/mission_sweep.sh).
$worldOf = @(@('^1', 'Story1'), @('^2', 'Story2'), @('^story3', 'Story3'), @('^4', 'Story4'), @('^5', 'Story5'),
             @('^r1', 'Rstory1'), @('^r4', 'Rstory4'), @('^rstory2', 'Rstory2'), @('^rstory3', 'Rstory3'), @('^rstory5', 'Rstory5'),
             @('^k1', 'Kstory1'), @('^k2', 'Kstory2'), @('^kstory3', 'Kstory3'), @('^kstory4', 'Kstory4'), @('^kstory5', 'Kstory5'),
             @('^end', 'the_end'))
if ($Sweep) {
    $sweepDir = Join-Path $outRoot 'sweep_scripts'
    New-Item -ItemType Directory -Force -Path $sweepDir | Out-Null
    foreach ($f in Get-ChildItem $GameBin -Filter 'wm_*.bin' | Sort-Object Name) {
        $m = $f.BaseName.Substring(3)
        $w = 'Story1'
        foreach ($pair in $worldOf) { if ($m -match $pair[0]) { $w = $pair[1]; break } }
        $s = @("name sweep_$m", "world $w", "mission $m", "env GVR_CUTSCENE none", "duration $SweepSeconds", "perf 4 $SweepSeconds",
               "hold trigger_r 6 10", "hold a 8 11", "move 0 1 11 15", "shot 5 start", "shot 14 later")
        $p = Join-Path $sweepDir "sweep_$m.txt"
        $s | Set-Content -Encoding ascii $p
        $scripts += $p
    }
}

foreach ($script in $scripts) {
    if (-not (Test-Path $script)) { Write-Host "No such test: $script"; continue }
    $name = [IO.Path]::GetFileNameWithoutExtension($script)
    $dur = 20.0
    foreach ($line in Get-Content $script) { if ($line -match '^\s*duration\s+([\d.]+)') { $dur = [double]$Matches[1] } }
    $dest = Join-Path $outRoot $name
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Write-Host ("[{0}] {1} ({2} s)" -f (Get-Date -Format 'HH:mm:ss'), $name, $dur)

    Adb shell am force-stop $pkg | Out-Null
    Adb shell rm -rf "$files/autotest_out" | Out-Null
    # Unix line endings for the app.
    $tmp = Join-Path $env:TEMP 'gvr_autotest.txt'
    [IO.File]::WriteAllText($tmp, ((Get-Content $script) -join "`n") + "`n")
    Adb push $tmp "$files/autotest.txt" | Out-Null
    Adb logcat -c | Out-Null
    Adb shell am broadcast -a com.oculus.vrpowermanager.prox_close | Out-Null
    Adb shell am start -n "$pkg/android.app.NativeActivity" | Out-Null

    $status = 'timeout'
    $deadline = (Get-Date).AddSeconds($dur + 120)
    $seenRunning = $false
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 3
        $done = (Adb shell "test -f $files/autotest_out/done && echo yes") -join ''
        if ($done -match 'yes') { $status = 'done'; break }
        $alive = (Adb shell pidof $pkg) -join ''
        if ($alive) { $seenRunning = $true }
        elseif ($seenRunning) { $status = 'crashed'; break }
    }
    Start-Sleep -Seconds 2
    Adb pull "$files/autotest_out" $dest | Out-Null
    Adb logcat -d -v time | Out-File -Encoding utf8 (Join-Path $dest 'logcat_full.txt')
    Adb logcat -d -v time 'GiantsVR:V' 'OpenXR:V' 'VrApi:V' 'AndroidRuntime:V' 'DEBUG:V' 'libc:V' '*:S' |
        Out-File -Encoding utf8 (Join-Path $dest 'logcat.txt')
    Adb shell am force-stop $pkg | Out-Null

    $res = Join-Path $dest 'autotest_out\result.txt'
    $fps = ''
    if (Test-Path $res) { $fps = ((Get-Content $res) | Where-Object { $_ -match '^(fps|gpu_ms|slow_frames)' }) -join '; ' }
    $errs = (Select-String -Path (Join-Path $dest 'logcat.txt') -Pattern ' E/GiantsVR' -SimpleMatch -ErrorAction SilentlyContinue | Measure-Object).Count
    $line = "{0}: {1}; {2}; {3} errors logged" -f $name, $status, $fps, $errs
    Write-Host "   $line"
    $line | Add-Content $summary
}

Adb shell am broadcast -a com.oculus.vrpowermanager.automation_disable | Out-Null
Write-Host "Results: $outRoot"
