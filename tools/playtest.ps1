param(
    [string[]]$Scenario = @(),
    [string]$Save = "",
    [string]$Exe = "",
    [switch]$Keep,
    [switch]$NoPrivacy,
    [switch]$List
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $Exe) { $Exe = Join-Path $root 'build\host\gtracing.exe' }
if (-not $Save) { $Save = Join-Path $root 'build\host\home' }

$common_taps_play = '1500:512,300'
$drag_route = '1500:512,300;1800:730,480;2000:120,486,20,120,376;2350:438,500;2600:954,690;3000:940,690;4000:512,480;4400:512,480'
$scenarios = [ordered]@{
    'menu' = @{
        what   = 'start, the title screen, the main menu'
        quit   = 2000
        env    = @{ GT_TAPS = $common_taps_play; GT_BENCH = '600-1400' }
        expect = @('menus take the pad .* MainMenu', 'menus take the pad .* NavigationMenu')
        also   = @('\[bench\] frames 600-1400: [0-9.]+ fps')
    }
    'display' = @{
        what   = 'Options > Display (its seven rows), PostFX and Motion Blur changed, back to the main menu'
        quit   = 3100
        env    = @{ GT_TAPS = '1500:512,300;1950:120,730;2350:209,300;2500:757,535;2525:232,535;2550:757,477;2575:757,477;2600:232,477;2625:232,477'
                    GT_KEYNAV = '1700:X;1800:X;2700:X;2850:X'; GT_BENCH = '2420-2480'; GT_FULL_BLUR = 'auto' }
        expect = @('menus take the pad .* NavigationMenu', 'menus take the pad .* ControlsMenu', '\[postfx\] off \(Options > Display\)',
                   '\[postfx\] Enhanced \(Options > Display\)', '\[display\] motion blur Full', '\[display\] motion blur Light',
                   '\[display\] motion blur Full', '\[display\] motion blur Auto', 'menus take the pad .* NavigationMenu:[0-9]+$')
    }
    'livefx' = @{
        what   = 'Practice > Simple Race on the U.S. Speed Ring (autopilot) started with PostFX Off: PostFX and Motion Blur switched while it runs (GT_TEST_DISPLAY)'
        quit   = 10900
        env    = @{ GT_TAPS = '1500:512,300;3000:829,225;3300:229,260;3600:960,356;3850:960,356;4100:960,356;4350:960,356;4600:925,696;4900:950,666;5250:930,676;6250:512,480;6600:512,480'
                    GT_KEYNAV = '1800:X;2000:L;2150:L;2300:D;2500:E'; GT_TEST_AUTOPILOT = '1'
                    GT_POSTFX = '0'; GT_FULL_BLUR = '1'; GT_BENCH = '7600-10400'
                    GT_TEST_DISPLAY = '7600:postfx=4,8000:postfx=2,8400:postfx=3,8800:postfx=0,9200:postfx=4,9600:blur=0,10200:blur=1'
                    GT_SHOTS = '7550,8300,9100,10500' }
        expect = @('\[postfx\] off \(POSTFX=0\)', 'race on state', '\[postfx\] Enhanced \(Options > Display\)',
                   '\[postfx\] a bloom shader given gt_balance', '\[postfx\] Classic \(Options > Display\)',
                   '\[postfx\] Classic Natural \(Options > Display\)', '\[postfx\] off \(Options > Display\)',
                   '\[postfx\] Enhanced \(Options > Display\)', '\[blur\] the blur target [0-9]+ x [0-9]+ -> [0-9]+ x [0-9]+',
                   '\[blur\] the blur target [0-9]+ x [0-9]+ -> [0-9]+ x [0-9]+')
        also   = @('\[bench\] frames 7600-10400: [0-9.]+ fps', '\[blur\] the motion blur shader given gt_blur_light')
    }
    'mycars' = @{
        what   = 'the Garage (My Cars) with the add-on cars'
        quit   = 3400
        env    = @{ GT_TAPS = $common_taps_play; GT_KEYNAV = '1700:X;1800:X'; GT_TEST_MENU = '2200:GarageMenu'; GT_BENCH = '2700-3300' }
        expect = @('menus take the pad .* NavigationMenu', '\[menu3d\] test: GarageMenu opened', 'menus take the pad .* GarageMenu')
    }
    'race' = @{
        what   = 'Practice > Simple Race: driven, then the pause menu, Leave, YES (the Simple Race menu), the main menu'
        quit   = 6400
        env    = @{ GT_TAPS = '1500:512,300;1800:730,480;2000:120,486,20,120,376;2350:829,225;2650:950,666;2950:930,676;4000:512,480;4400:512,480;5300:799,327;5600:368,409'
                    GT_PEDALS = '4050:5150:g'; GT_KEYNAV = '5200:X;6000:X'; GT_BENCH = '4500-5100' }
        expect = @('menus take the pad .* NavigationMenu', 'race on state', 'menus take the pad .* PauseMenu',
                   'race off state -1.* menus QR_MainMenu', 'menus take the pad .* NavigationMenu:[0-9]+$')
        also   = @('\[bench\] frames 4500-5100: [0-9.]+ fps')
    }
    'replay' = @{
        what   = 'Practice > Test Center drag race to the end, its replay: zoom, turn and 10 s back with the pad'
        quit   = 8900
        env    = @{ GT_TAPS = "$drag_route;7700:644,696"; GT_PEDALS = '4050:7400:g'; GT_BENCH = '4500-7000'
                    GT_REPLAY_PADTEST = '8650:8700:i;8710:8760:R;8770:8775:b' }
        also   = @('\[bench\] frames 4500-7000: [0-9.]+ fps')
        expect = @('race on state', 'menus take the pad .* EndMenu:1',
                   'menus take the pad .* ReplayMenu', '\[replay\] LowBarMenu.btnGreyOverlay -> Hide', '\[replay\] pad zoom in',
                   '\[replay\] pad: right stick turns the camera', '\[replay\] pad back 10 s')
    }
}
if ($List) {
    $scenarios.Keys | ForEach-Object { '{0,-8} {1}' -f $_, $scenarios[$_].what }
    'selftest --selftest (always first), then layers: tools\layer_check.py'
    'privacy  a forced crash''s minidump and log, both exes: tools\privacy_scan.py (always last; -NoPrivacy skips it)'
    return
}
$Scenario = @($Scenario | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
if (-not $Scenario.Count) { $Scenario = @($scenarios.Keys) }
foreach ($s in $Scenario) { if (-not $scenarios.Contains($s)) { throw "no scenario '$s' (-List shows them)" } }
if (-not (Test-Path $Exe)) { throw "no exe at $Exe" }
if (-not (Test-Path (Join-Path $Save 'Documents'))) { throw "no save at $Save (a folder with Documents\ and Library\)" }
if (Get-Process gtracing -ErrorAction SilentlyContinue) { throw 'gtracing.exe is running: close the game first' }

$out = Join-Path $root ('build\playtest\' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $out | Out-Null
$tmp = Join-Path $env:TEMP ('gt-playtest-' + [guid]::NewGuid().ToString('N').Substring(0, 8))
New-Item -ItemType Directory $tmp | Out-Null

$private = @($env:USERNAME, $env:COMPUTERNAME, $env:USERPROFILE) | Where-Object { $_ -and $_.Length -ge 3 }
$bad = '^\[FAULT\]|^\[FATAL\]|^\[crash\] wrote|uncaught (C\+\+|Objective-C) exception|did not (compile|link)|GL error'

$ini = Join-Path $tmp 'gtracing.ini'
@('# playtest.ps1: a window, the cloud leaderboard off', 'GT_FULLSCREEN=0', 'GT_WIDE=1024', 'GT_RESOLUTION=768',
  'GT_CLOUD_BOARD=0', 'GT_BACKGROUND_THROTTLE=0', 'GT_MENU_CAR_RANDOM=0') | Out-File $ini -Encoding ascii
$switches = 'GT_TAPS', 'GT_KEYNAV', 'GT_PADNAV', 'GT_PEDALS', 'GT_TEST_MENU', 'GT_BENCH', 'GT_REPLAY_PADTEST', 'GT_PAUSE',
            'GT_CAM', 'GT_WHEEL', 'GT_SHOTS', 'GT_POSTFX', 'GT_FULL_BLUR', 'GT_TEST_DISPLAY', 'GT_TEST_AUTOPILOT'
$base = @{ GT_INI = $ini; GT_CLOUD_BOARD = '0'; GT_SPIKE_MS = '18'; GT_SHOT_DIR = $out; GT_PADMENU_LOG = '0' }

function Invoke-GameRun([string]$name, [hashtable]$envs, [string[]]$arguments, [int]$timeout) {
    $log = Join-Path $out "$name.log"
    $set = @{} + $base
    foreach ($k in $switches) { $set[$k] = '' }
    foreach ($k in $envs.Keys) { $set[$k] = $envs[$k] }
    $set['GT_LOG'] = $log
    $saved = @{}
    foreach ($k in $set.Keys) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $set[$k]) }
    try {
        $sp = @{ FilePath = $Exe; WorkingDirectory = (Split-Path $Exe); PassThru = $true }
        if ($arguments.Count) { $sp.ArgumentList = $arguments }
        $p = Start-Process @sp
        if (-not $p.WaitForExit($timeout * 1000)) { $p.Kill(); $p.WaitForExit(); return @{ log = $log; code = 'timeout' } }
        return @{ log = $log; code = $p.ExitCode }
    } finally {
        foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
    }
}

function Test-Log([string]$log, [string[]]$expect, [string[]]$also = @()) {
    $why = @()
    if (-not (Test-Path $log)) { return @('no log written') }
    $lines = Get-Content $log
    foreach ($l in $lines) { if ($l -match $bad) { $why += "log: $l" } }
    foreach ($p in $private) {
        $hit = $lines | Where-Object { $_.IndexOf($p, [StringComparison]::OrdinalIgnoreCase) -ge 0 } | Select-Object -First 1
        if ($hit) { $why += 'log holds a private name or folder (line: ' + $hit.Substring(0, [Math]::Min(60, $hit.Length)) + ' ...)' }
    }
    $at = 0
    foreach ($e in $expect) {
        $found = $false
        for ($i = $at; $i -lt $lines.Count; $i++) { if ($lines[$i] -match $e) { $found = $true; $at = $i + 1; break } }
        if (-not $found) { $why += "expected (in order) and not found: $e" }
    }
    foreach ($e in $also) { if (-not ($lines -match $e)) { $why += "expected and not found: $e" } }
    return $why
}

$results = @()
try {
    $h = Join-Path $tmp 'home_selftest'
    New-Item -ItemType Directory $h | Out-Null
    $r = Invoke-GameRun 'selftest' @{ GT_HOME = $h } @('--selftest') 120
    $why = Test-Log $r.log @('\[selftest\] 0 of [0-9]+ checks failed')
    if ($r.code -ne 0) { $why += "exit code $($r.code)" }
    $sum = (Select-String -Path $r.log -Pattern '\[selftest\] [0-9]+ of [0-9]+ checks failed' | Select-Object -Last 1).Line
    $results += [pscustomobject]@{ scenario = 'selftest'; result = $(if ($why) { 'FAIL' } else { 'PASS' }); note = $sum; why = $why }
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($py -and (Test-Path (Join-Path $root 'build\host\obj'))) {
        $lines = @(& $py -I (Join-Path $root 'tools\layer_check.py'))
        $ok = $LASTEXITCODE -eq 0
        $results += [pscustomobject]@{ scenario = 'layers'; result = $(if ($ok) { 'PASS' } else { 'FAIL' })
                                       note = $lines[-1]; why = $(if ($ok) { @() } else { $lines }) }
    }

    foreach ($s in $Scenario) {
        $sc = $scenarios[$s]
        $h = Join-Path $tmp "home_$s"
        robocopy $Save $h /MIR /NFL /NDL /NJH /NJS /NP | Out-Null
        $envs = @{ GT_HOME = $h; GT_QUIT_AT = "$($sc.quit)" } + $sc.env
        Write-Host "running $s ($($sc.what)) ..."
        $r = Invoke-GameRun $s $envs @() ([int]($sc.quit / 30) + 120)
        $why = Test-Log $r.log $sc.expect $(if ($sc.also) { $sc.also } else { @() })
        if ($r.code -ne 0) { $why += "exit code $($r.code)" }
        $bench = (Select-String -Path $r.log -Pattern '\[bench\] frames [0-9-]+: ([0-9.]+) fps' -ErrorAction SilentlyContinue | ForEach-Object { $_.Matches[0].Groups[1].Value + ' fps' }) -join ', '
        $spikes = @(Select-String -Path $r.log -Pattern '^\[spike\]' -ErrorAction SilentlyContinue).Count
        $results += [pscustomobject]@{ scenario = $s; result = $(if ($why) { 'FAIL' } else { 'PASS' })
                                       note = "bench $(if ($bench) { $bench } else { '-' }); frames over 18 ms: $spikes"; why = $why }
        if (-not $Keep) { Remove-Item $h -Recurse -Force -ErrorAction SilentlyContinue }
    }

    if (-not $NoPrivacy) {
        $h = Join-Path $tmp 'home_crash'
        New-Item -ItemType Directory $h | Out-Null
        Write-Host 'running privacy (a forced crash, then tools\privacy_scan.py) ...'
        $r = Invoke-GameRun 'crash' @{ GT_HOME = $h; GT_CRASH_TEST = '200'; GT_QUIT_AT = '3000' } @() 120
        $dmp = Join-Path $out 'crash.dmp'
        $why = @()
        if (-not (Test-Path $r.log)) { $why += 'no log written' }
        elseif (-not (Select-String -Path $r.log -Pattern '^\[crash\] wrote' -Quiet)) { $why += 'the crash wrote no dump' }
        if (-not (Test-Path $dmp)) { $why += 'no crash.dmp' }
        $scan = @($Exe, $dmp, $r.log) + @(Join-Path (Split-Path $Exe) 'release\gtracing.exe' | Where-Object { Test-Path $_ })
        $scan = @($scan | Where-Object { Test-Path $_ })
        $py = (Get-Command python -ErrorAction SilentlyContinue).Source
        if (-not $py) { $why += 'no python for tools\privacy_scan.py' }
        else {
            $lines = & $py -I (Join-Path $root 'tools\privacy_scan.py') --release @scan
            if ($LASTEXITCODE -ne 0) { $why += @($lines) }
        }
        $names = (@($scan | ForEach-Object { Split-Path $_ -Leaf }) + 'tools\release\' + 'build\release\ (shipped part)') -join ', '
        $results += [pscustomobject]@{ scenario = 'privacy'; result = $(if ($why) { 'FAIL' } else { 'PASS' })
                                       note = "scanned $names"; why = $why }
    }
} finally {
    if (-not $Keep) { Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue }
}

'' ; 'results in ' + ($out -replace [regex]::Escape($root + '\'), '')
foreach ($r in $results) {
    '{0,-4} {1,-9} {2}' -f $r.result, $r.scenario, $r.note
    foreach ($w in $r.why) { '            ' + $w }
}
if ($Keep) { "temporary saves kept in $tmp" }
if ($results | Where-Object { $_.result -ne 'PASS' }) { exit 1 }
exit 0
