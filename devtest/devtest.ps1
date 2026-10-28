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
    .\devtest.ps1 -Set experiments     # the scripts in .\experiments instead of .\tests
    .\devtest.ps1 -Set missions,weapons # several folders, one after another
    .\devtest.ps1 -All                 # THE WHOLE TEST (2026-10-27): tests, missions, weapons, npcs, ui, cutscenes,
                                       # the whole campaign, then the mission sweep - about 5 h 20 min. Keep the
                                       # headset on its charger (a USB cable from a PC charges too slowly).
    .\devtest.ps1 -All -Skip campaign  # ... without some sets

  If Windows refuses to run scripts:  powershell -ExecutionPolicy Bypass -File .\devtest.ps1
#>
param(
    [string]$Apk = (Join-Path $PSScriptRoot "..\GiantsVR-dev.apk"),
    [string[]]$Tests = @(),
    [string[]]$Set = @('tests'),
    [switch]$All,
    [string[]]$Skip = @(),
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
Adb logcat -G 16M | Out-Null   # a bigger log buffer: the system's chatter pushed the app's lines out
function Avg($a) { if ($a.Count) { [math]::Round((($a | Measure-Object -Average).Average), 2) } else { 'n/a' } }

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
# (-File passes "a,b" as one string: split on commas and spaces; a trailing .txt is fine too)
$Set = @($Set | ForEach-Object { $_ -split '[,\s]+' } | Where-Object { $_ })
$Skip = @($Skip | ForEach-Object { $_ -split '[,\s]+' } | Where-Object { $_ })
if ($All) {
    # The whole test: the short checks first, the long runs (cutscenes, the campaign) after, the sweep last.
    $Set = @('tests', 'missions', 'weapons', 'npcs', 'ui', 'cutscenes', 'campaign')
    $Sweep = $true
    if ($Skip -contains 'sweep') { $Sweep = $false }
}
$Set = @($Set | Where-Object { $Skip -notcontains $_ })
$Tests = @($Tests | ForEach-Object { $_ -split '[,\s]+' } | Where-Object { $_ } | ForEach-Object { $_ -replace '\.txt$', '' })
if ($Tests.Count -gt 0) {
    # Named tests: looked up in the chosen folders (the first that has it).
    foreach ($t in $Tests) {
        $found = $null
        foreach ($d in $Set) { $p = Join-Path (Join-Path $PSScriptRoot $d) "$t.txt"; if (Test-Path $p) { $found = $p; break } }
        if (-not $found) { $found = Join-Path (Join-Path $PSScriptRoot $Set[0]) "$t.txt" }
        $scripts += $found
    }
} else {
    foreach ($d in $Set) {
        $testDir = Join-Path $PSScriptRoot $d
        if (-not (Test-Path $testDir)) { Write-Host "No such test folder: $testDir"; continue }
        $scripts += @(Get-ChildItem $testDir -Filter *.txt | Sort-Object Name | ForEach-Object { $_.FullName })
    }
}
# Mission name prefix -> world (as scripts/mission_sweep.sh).
$worldOf = @(@('^1', 'Story1'), @('^2', 'Story2'), @('^story3', 'Story3'), @('^4', 'Story4'), @('^5', 'Story5'),
             @('^r1', 'Rstory1'), @('^r4', 'Rstory4'), @('^rstory2', 'Rstory2'), @('^rstory3_race_2', 'Rstory3_race_2'), @('^rstory3_race_3', 'Rstory3_race_3'), @('^rstory3', 'Rstory3_race_1'), @('^rstory5', 'Rstory5'),
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

# How long it will take: each script's duration, plus ~25 s a test to start, load and pull its results.
$planned = 0.0
foreach ($s in $scripts) {
    $d = 20.0
    if (Test-Path $s) { foreach ($line in Get-Content $s) { if ($line -match '^\s*duration\s+([\d.]+)') { $d = [double]$Matches[1] } } }
    $planned += $d + 25
}
$msg = "{0} tests in {1}{2}: about {3:N0} h {4:N0} min" -f $scripts.Count, ($Set -join ', '), $(if ($Sweep) { ' + sweep' } else { '' }), [math]::Floor($planned / 3600), [math]::Floor(($planned % 3600) / 60)
Write-Host $msg
$msg | Add-Content $summary
$startAll = Get-Date
function Battery { $b = (Adb shell dumpsys battery) -join "`n"; if ($b -match 'level:\s*(\d+)') { [int]$Matches[1] } else { -1 } }

# A test that ran but hardly drew (the headset dozing off as it started: 1-2 fps, "fps 0.0") runs once more.
$queue = [System.Collections.Generic.Queue[string]]::new()
foreach ($s in $scripts) { $queue.Enqueue($s) }
$retried = @{}
while ($queue.Count -gt 0) {
    $script = $queue.Dequeue()
    if (-not (Test-Path $script)) { Write-Host "No such test: $script"; continue }
    $name = [IO.Path]::GetFileNameWithoutExtension($script)
    $dur = 20.0
    foreach ($line in Get-Content $script) { if ($line -match '^\s*duration\s+([\d.]+)') { $dur = [double]$Matches[1] } }
    $dest = Join-Path $outRoot $name
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    $bat = Battery
    $elapsed = ((Get-Date) - $startAll).TotalMinutes
    Write-Host ("[{0}] {1} ({2} s) - battery {3}%, {4:N0} min in" -f (Get-Date -Format 'HH:mm:ss'), $name, $dur, $bat, $elapsed)
    if ($bat -ge 0 -and $bat -lt 15) {
        Write-Host "   Battery at $bat% - waiting for it to charge to 30% (plug the headset in)." -ForegroundColor Yellow
        "battery $bat% before $name - waited to charge" | Add-Content $summary
        while ((Battery) -lt 30) { Start-Sleep -Seconds 60; Adb shell input keyevent KEYCODE_WAKEUP | Out-Null }
    }

    Adb shell am force-stop $pkg | Out-Null
    Adb shell input keyevent KEYCODE_WAKEUP | Out-Null   # (each time: a long run or a pause lets the headset sleep)
    Adb shell rm -rf "$files/autotest_out" | Out-Null
    Adb shell rm -rf "$files/crash" | Out-Null   # (a crash report left by an earlier run)
    # Unix line endings for the app.
    $tmp = Join-Path $env:TEMP 'gvr_autotest.txt'
    [IO.File]::WriteAllText($tmp, ((Get-Content $script) -join "`n") + "`n")
    Adb push $tmp "$files/autotest.txt" | Out-Null
    Adb logcat -c | Out-Null
    # The app's log streamed to a file while the test runs: a long test (the campaign, 55 min) outgrows the
    # log buffer, which would drop its early lines before the end.
    $logFile = Join-Path $dest 'logcat.txt'
    $logProc = Start-Process -FilePath $adb -ArgumentList @('logcat', '-v', 'time', 'GiantsVR:V', 'OpenXR:V', 'VrApi:V', 'AndroidRuntime:V', 'DEBUG:V', 'libc:V', '*:S') `
        -RedirectStandardOutput $logFile -NoNewWindow -PassThru
    Adb shell am broadcast -a com.oculus.vrpowermanager.prox_close | Out-Null
    Adb shell am start -n "$pkg/android.app.NativeActivity" | Out-Null
    # A system dialog in front of the app (e.g. "Controllers required" from APKs built before 2026-10-25,
    # when the controllers lie still) holds the test until someone presses a trigger in the headset.
    Start-Sleep -Seconds 4
    $dialog = (Adb shell dumpsys activity activities) | Select-String -Pattern 'LaunchCheck\w*Activity|systemdialog\.\w+' | Select-Object -First 1
    if ($dialog) {
        $what = $dialog.Matches[0].Value
        Write-Host "   The headset is showing a system dialog ($what) - this APK needs a trigger press to start." -ForegroundColor Yellow
        Write-Host "   Install a newer APK (one that declares hand tracking), or put the headset on and press a trigger." -ForegroundColor Yellow
        "${name}: system dialog at start: $what" | Add-Content $summary
    }

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
    # A native crash report (APKs from 2026-10-28 on): the signal, a backtrace and the last log lines.
    $crashNote = ''
    $hasCrash = (Adb shell "ls $files/crash 2>/dev/null") -join ''
    if ($hasCrash -match 'crash_') {
        Adb pull "$files/crash" $dest | Out-Null
        $crashNote = '; CRASH REPORT in crash\'
        Write-Host "   A crash report was written (crash\ in its results)." -ForegroundColor Red
    }
    Adb logcat -d -v time | Out-File -Encoding utf8 (Join-Path $dest 'logcat_full.txt')
    if ($logProc -and -not $logProc.HasExited) { Stop-Process -Id $logProc.Id -Force -ErrorAction SilentlyContinue }
    Start-Sleep -Milliseconds 500
    if (-not (Test-Path $logFile) -or (Get-Item $logFile).Length -lt 100) {
        # (the stream failed: the buffer's copy instead)
        Adb logcat -d -v time 'GiantsVR:V' 'OpenXR:V' 'VrApi:V' 'AndroidRuntime:V' 'DEBUG:V' 'libc:V' '*:S' | Out-File -Encoding utf8 $logFile
    }
    Adb shell am force-stop $pkg | Out-Null

    $res = Join-Path $dest 'autotest_out\result.txt'
    $fps = ''
    if (Test-Path $res) { $fps = ((Get-Content $res) | Where-Object { $_ -match '^(fps|slow_frames|effects_quality_level)' }) -join '; ' }
    # The system's own once-a-second performance line (VrApi) for the app: GPU time and load.
    $log = @(Get-Content (Join-Path $dest 'logcat.txt'))
    $appPid = 'none'
    foreach ($l in $log) { if ($l -match 'GiantsVR\(\s*(\d+)\)') { $appPid = $Matches[1]; break } }
    $vr = @($log | Where-Object { $_ -match ('VrApi\s*\(\s*' + $appPid + '\):\s*FPS=') })
    $vr | Set-Content (Join-Path $dest 'vrapi.txt')
    $use = $vr
    if ($vr.Count -gt 6) { $use = $vr[4..($vr.Count - 2)] }
    $appMs = @(); $gpuPct = @(); $fpsV = @()
    foreach ($l in $use) {
        if ($l -match 'App=([\d.]+)ms') { $v = [double]$Matches[1]; if ($v -lt 1000) { $appMs += $v } }  # (now and then a garbage 1e5+ value)
        if ($l -match 'GPU%=([\d.]+)') { $gpuPct += [double]$Matches[1] }
        if ($l -match 'FPS=(\d+)/') { $fpsV += [double]$Matches[1] }
    }
    $vrs = 'vrapi fps {0}, app gpu {1} ms, gpu load {2} ({3} s)' -f (Avg $fpsV), (Avg $appMs), (Avg $gpuPct), $use.Count
    # Another app drawing while the test ran (the system shell behind a pop-up: on 2026-10-26 it took 12-36 ms of
    # GPU a frame in some tests): the GPU figures are then not the game's own.
    $first = -1; $last = -1
    for ($i = 0; $i -lt $log.Count; $i++) {
        if ($log[$i] -match ('VrApi\s*\(\s*' + $appPid + '\):\s*FPS=')) { if ($first -lt 0) { $first = $i }; $last = $i }
    }
    $shellN = 0; $shellMs = @(); $handOver = 0
    if ($first -ge 0) {
        for ($i = $first; $i -le $last; $i++) {
            if ($log[$i] -match 'VrApi\s*\(\s*(\d+)\):\s*FPS=(\d+)/.*App=([\d.]+)ms') {
                if ($Matches[1] -eq $appPid) { continue }
                $handOver++
                if ($handOver -le 2) { continue }   # (the hand-over as the app starts)
                if ([double]$Matches[2] -gt 5 -and [double]$Matches[3] -gt 3) { $shellN++; $shellMs += [double]$Matches[3] }
            }
        }
    }
    $shell = ''
    if ($shellN -ge 6) {   # (2-5 s: the system shell drawing round the hand-overs at the start and end - 2026-10-27 run)
        $shell = "; OTHER APP DRAWING for $shellN s (its gpu $(Avg $shellMs) ms): these GPU figures are not the game's own"
        Write-Host "   Another app drew alongside the game (a system pop-up in the headset?) - take it off and run this test again." -ForegroundColor Yellow
    }
    # The events the script's comment lists (Log: "...") seen in the app's log.
    $events = ''
    $head = ((Get-Content $script) | Where-Object { $_ -match '^#' } | ForEach-Object { $_.Substring(1).Trim() }) -join ' '
    if ($head -match '(?i)(\blog\b|pass:)(.*)') {
        $exp = @([regex]::Matches($Matches[2], '"([^"]+)"') | ForEach-Object { $_.Groups[1].Value })
        $text = (Get-Content (Join-Path $dest 'logcat.txt')) -join "`n"
        $missing = @()
        foreach ($e in $exp) {
            $pat = [regex]::Escape($e) -replace '\\\.\\\.\\\.', '.*' -replace '(?<![A-Za-z])N(?![A-Za-z])', '[\d.]+'
            if ($text -notmatch $pat) { $missing += $e }
        }
        if ($exp.Count -gt 0) {
            $events = "; events $($exp.Count - $missing.Count)/$($exp.Count)"
            if ($missing.Count -gt 0) { $events += ' (MISSING: ' + ($missing -join ' | ') + ')' }
        }
    }
    # The headset took the app out of focus during the test (Guardian / standby: on 2026-10-27 the Guardian
    # set-up came up 40 s into a race and the headset slept for 7 min): the session left FOCUSED (5) before the
    # script finished. Those results are not the game's - run it once more.
    $focusLost = $false
    foreach ($l in $log) {
        if ($l -match ('GiantsVR\(\s*' + $appPid + '\):\s*autotest: .* finished')) { break }
        if ($l -match ('GiantsVR\(\s*' + $appPid + '\):\s*Session state 5 -> 4')) { $focusLost = $true; break }
    }
    if ($focusLost) {
        $shell += "; FOCUS LOST (Guardian or standby took over the headset during the test): its results are not the game's"
        Write-Host "   The headset took the app out of focus (Guardian / standby) during this test." -ForegroundColor Yellow
    }
    $errs = (Select-String -Path (Join-Path $dest 'logcat.txt') -Pattern ' E/GiantsVR' -SimpleMatch -ErrorAction SilentlyContinue | Measure-Object).Count
    $line = "{0}: {1}; {2}; {3}{4}{5}{7}; {6} errors logged" -f $name, $status, $fps, $vrs, $shell, $events, $errs, $crashNote
    Write-Host "   $line"
    $line | Add-Content $summary
    $fpsNum = 99.0
    if ($fps -match 'fps ([\d.]+)') { $fpsNum = [double]$Matches[1] }
    if ($focusLost -and -not $retried.ContainsKey($script)) {
        $retried[$script] = $true
        Write-Host "   (running it again)" -ForegroundColor Yellow
        "${name}: focus lost - run again" | Add-Content $summary
        $queue.Enqueue($script)
    }
    elseif ($status -eq 'done' -and $fpsNum -lt 10 -and -not $retried.ContainsKey($script)) {
        $retried[$script] = $true
        Write-Host "   (it hardly drew - the headset was probably asleep; running it again)" -ForegroundColor Yellow
        "${name}: hardly drew ($fpsNum fps) - run again" | Add-Content $summary
        $queue.Enqueue($script)
    }
}

Adb shell am broadcast -a com.oculus.vrpowermanager.automation_disable | Out-Null
# One line per set: how many tests finished with every listed event seen (the last line per test: a test run
# again counts once, by its second run).
$lines = @(Get-Content $summary)
$lastOf = @{}
foreach ($l in $lines) { if ($l -match '^(\S+): (done|timeout|crashed);') { $lastOf[$Matches[1]] = $l } }
foreach ($d in $Set + $(if ($Sweep) { @('sweep') } else { @() })) {
    if ($d -eq 'sweep') { $names = @($lastOf.Keys | Where-Object { $_ -like 'sweep_*' }) }
    else { $names = @(Get-ChildItem (Join-Path $PSScriptRoot $d) -Filter *.txt -ErrorAction SilentlyContinue | ForEach-Object { $_.BaseName }) }
    $mine = @($names | Where-Object { $lastOf.ContainsKey($_) } | ForEach-Object { $lastOf[$_] })
    if ($mine.Count -eq 0) { continue }
    $good = @($mine | Where-Object { $_ -match ': done;' -and $_ -notmatch 'MISSING' -and $_ -notmatch 'FOCUS LOST' })
    $t = "{0}: {1}/{2} done with all their events" -f $d, $good.Count, $mine.Count
    Write-Host $t
    $t | Add-Content $summary
}
"total time {0:N0} min" -f ((Get-Date) - $startAll).TotalMinutes | Add-Content $summary
Write-Host "Results: $outRoot"
