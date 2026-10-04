#Requires -Version 7.2
param(
    [Parameter(Mandatory = $true)][string]$AdbPath,
    [string]$Serial = '',
    [Parameter(Mandatory = $true)][string]$Apk,
    [Parameter(Mandatory = $true)][string]$FrameCheckExe,
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [ValidateRange(60, 1800)][int]$TimeoutSeconds = 180
)

# A real Activity/swapchain test: NDK build success cannot replace runtime.
# Physics and camera remain frozen at each checkpoint until capture validation
# succeeds and this harness sends its test-only F12 acknowledgement.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$component = 'dev.laiue.walk/android.app.NativeActivity'
$package = 'dev.laiue.walk'
$runId = [System.Security.Cryptography.RandomNumberGenerator]::GetInt32(1, [int]::MaxValue)
$remoteRun = [guid]::NewGuid().ToString('N')
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$deadlineSeconds = $TimeoutSeconds
$adbPrefix = @()
$appProcessId = $null
$oldAutoRotation = $null
$oldUserRotation = $null
$oldFixedRotation = $null
$logSince = $null
$captures = [System.Collections.Generic.List[object]]::new()
$restoreErrors = [System.Collections.Generic.List[string]]::new()
$testIssue = $null
$summary = $null
$rotationVerified = $false
$homeResumeVerified = $false
$reportPath = Join-Path $OutputDir 'report.json'
[pscustomobject]@{ status = 'RUNNING'; run = $runId } | ConvertTo-Json |
    Set-Content -LiteralPath $reportPath -Encoding utf8

function Invoke-BoundedProcess {
    param([string]$Executable, [string[]]$Arguments, [int]$CommandTimeoutSeconds = 15)
    if ($clock.Elapsed.TotalSeconds -ge $deadlineSeconds) {
        throw "Android smoke exceeded its $TimeoutSeconds second deadline."
    }
    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo.FileName = $Executable
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $commandClock = [System.Diagnostics.Stopwatch]::StartNew()
    $started = $false
    foreach ($argument in $Arguments) { $process.StartInfo.ArgumentList.Add($argument) }
    try {
        if (-not $process.Start()) { throw "Cannot start $Executable." }
        $started = $true
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $remaining = [Math]::Max(1, $deadlineSeconds - $clock.Elapsed.TotalSeconds)
        $waitMs = [int](1000 * [Math]::Min($CommandTimeoutSeconds, $remaining))
        if (-not $process.WaitForExit($waitMs)) {
            $process.Kill($true)
            throw "Process timed out: $Executable $($Arguments -join ' ')"
        }
        $io = [System.Threading.Tasks.Task]::WhenAll([System.Threading.Tasks.Task[]]@($stdout, $stderr))
        $ioWaitMs = [int][Math]::Max(1, $waitMs - $commandClock.Elapsed.TotalMilliseconds)
        if (-not $io.Wait($ioWaitMs)) { throw "Process output timed out: $Executable" }
        $output = $stdout.GetAwaiter().GetResult()
        $errors = $stderr.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            throw "Process exited with $($process.ExitCode): $Executable $($Arguments -join ' ')`n$output`n$errors"
        }
        return $output.Trim()
    } finally {
        if ($started -and -not $process.HasExited) { $process.Kill($true) }
        $process.Dispose()
    }
}

function Invoke-Adb {
    param([string[]]$Arguments, [int]$CommandTimeoutSeconds = 15)
    return Invoke-BoundedProcess -Executable $AdbPath -Arguments (@($adbPrefix) + @($Arguments)) `
        -CommandTimeoutSeconds $CommandTimeoutSeconds
}

function Read-SmokeLog {
    $arguments = @('logcat', '-d', '-v', 'threadtime')
    if ($logSince) { $arguments += @('-T', $logSince) }
    if ($appProcessId) { $arguments += "--pid=$appProcessId" }
    $arguments += @('laiue.walk:I', 'AndroidRuntime:E', 'libc:F', '*:S')
    $raw = Invoke-Adb -Arguments $arguments
    [System.IO.File]::WriteAllText((Join-Path $OutputDir 'logcat.txt'), $raw)
    # App errors are scoped by PID; scenario messages additionally by run ID.
    if ($appProcessId -and $raw -match 'FATAL EXCEPTION|Fatal signal|(?:^|\n)[^\n]*\sE\s+laiue[.]walk\s*:') {
        throw "Android walk process $appProcessId reported an error; inspect $(Join-Path $OutputDir 'logcat.txt')."
    }
    $lines = @($raw -split '\r?\n' | Where-Object {
        $_ -match 'LAIUE_SCENARIO ' -and $_ -match "(?:^|\s)run=$runId(?:\s|$)"
    })
    $log = $lines -join "`n"
    [System.IO.File]::WriteAllText((Join-Path $OutputDir 'scenario.log'), $log)
    if ($log -match 'event=failure|event=summary[^\r\n]*status=FAIL') {
        throw "Android scenario run $runId failed; inspect $(Join-Path $OutputDir 'scenario.log')."
    }
    return $log
}

function Wait-Log {
    param([scriptblock]$Condition, [string]$Description, [int]$WaitSeconds = 25)
    $until = $clock.Elapsed.TotalSeconds + $WaitSeconds
    do {
        $log = Read-SmokeLog
        if (& $Condition $log) { return $log }
        if ($clock.Elapsed.TotalSeconds -ge $until) { throw "Timed out waiting for $Description." }
        Start-Sleep -Milliseconds 200
    } while ($true)
}

function Save-Capture {
    param([string]$Name, [int]$NativeWidth, [int]$NativeHeight, [int]$Checkpoint = 0)
    if ($Name -notmatch '^[a-zA-Z0-9_-]+$' -or $NativeWidth -le 0 -or $NativeHeight -le 0) {
        throw 'Invalid screenshot checkpoint metadata.'
    }
    # Only this run's GUID-scoped device file is created and removed.
    $remote = "/sdcard/laiue-walk-$remoteRun-$Name.png"
    $local = Join-Path $OutputDir "$Name.png"
    try {
        Invoke-Adb -Arguments @('shell', 'screencap', '-p', $remote) -CommandTimeoutSeconds 7 | Out-Null
        Invoke-Adb -Arguments @('pull', $remote, $local) -CommandTimeoutSeconds 7 | Out-Null
    } finally {
        Invoke-Adb -Arguments @('shell', 'rm', '-f', $remote) -CommandTimeoutSeconds 3 | Out-Null
    }
    # The engine PNG decoder is the sole parser; it inspects a viewport without UI.
    $imageText = Invoke-BoundedProcess -Executable $FrameCheckExe -Arguments @($local) `
        -CommandTimeoutSeconds 10
    $image = $imageText | ConvertFrom-Json
    if ($image.status -ne 'PASS' -or -not $image.useful -or
        $image.width -le 0 -or $image.height -le 0) {
        throw "Shared frame checker rejected $Name."
    }
    # System bars can sit below portrait surfaces or beside landscape surfaces.
    # Accept only a bounded inset on either axis and the same orientation.
    if ($image.width -lt $NativeWidth -or $image.width -gt $NativeWidth * 1.25 -or
        $image.height -lt $NativeHeight -or $image.height -gt $NativeHeight * 1.25 -or
        (($image.width -gt $image.height) -ne ($NativeWidth -gt $NativeHeight))) {
        throw "Native surface and screenshot dimensions disagree for $Name."
    }
    $capture = [pscustomobject]@{
        name = $Name; checkpoint = $Checkpoint; path = $local
        nativeWidth = $NativeWidth; nativeHeight = $NativeHeight
        width = $image.width; height = $image.height; imageCheck = $image
        sha256 = (Get-FileHash -LiteralPath $local -Algorithm SHA256).Hash
        source = 'external_adb_screencap'; run = $runId
    }
    $captures.Add($capture)
    return $capture
}

function Latest-Presentation {
    param([string]$Log)
    $items = [regex]::Matches($Log, 'event=presented run=\d+ generation=\d+ frame=\d+ width=(\d+) height=(\d+)')
    if ($items.Count -eq 0) { throw 'No presented native frame was reported.' }
    return $items[$items.Count - 1]
}

function Restore-Setting {
    param([string]$Name, [AllowNull()][object]$Original)
    if ($null -eq $Original) { return }
    if ($Original -eq 'null' -or $Original -eq '') {
        Invoke-Adb -Arguments @('shell', 'settings', 'delete', 'system', $Name) | Out-Null
    } else {
        Invoke-Adb -Arguments @('shell', 'settings', 'put', 'system', $Name, $Original) | Out-Null
    }
    $restored = Invoke-Adb -Arguments @('shell', 'settings', 'get', 'system', $Name)
    $expected = if ($Original -eq '') { 'null' } else { [string]$Original }
    if ($restored -ne $expected) { throw "Restoring $Name did not recover its original value." }
}

try {
    foreach ($inputFile in @($AdbPath, $Apk, $FrameCheckExe)) {
        if (-not (Test-Path -LiteralPath $inputFile -PathType Leaf)) {
            throw "Android smoke input does not exist: $inputFile"
        }
    }
    $AdbPath = (Resolve-Path -LiteralPath $AdbPath).Path
    $Apk = (Resolve-Path -LiteralPath $Apk).Path
    $FrameCheckExe = (Resolve-Path -LiteralPath $FrameCheckExe).Path
    if (-not $Serial) {
        $devices = Invoke-Adb -Arguments @('devices')
        $connected = @([regex]::Matches($devices, '(?m)^([^\s]+)\s+device\s*$') |
            ForEach-Object { $_.Groups[1].Value })
        if ($connected.Count -ne 1) {
            throw 'Pass -Serial for one authorized device. A build is not a runtime test.'
        }
        $Serial = $connected[0]
    }
    $adbPrefix = @('-s', $Serial)
    if ((Invoke-Adb -Arguments @('get-state')) -ne 'device') { throw 'Android device is not ready.' }
    $auto = Invoke-Adb -Arguments @('shell', 'settings', 'get', 'system', 'accelerometer_rotation')
    if ($auto -notmatch '^(0|1|null)$') { throw 'Invalid original accelerometer rotation setting.' }
    $oldAutoRotation = $auto
    $rotation = Invoke-Adb -Arguments @('shell', 'settings', 'get', 'system', 'user_rotation')
    if ($rotation -notmatch '^(0|1|2|3|null)$') { throw 'Invalid original user rotation setting.' }
    $oldUserRotation = $rotation
    $fixed = Invoke-Adb -Arguments @('shell', 'wm', 'fixed-to-user-rotation')
    if ($fixed -notmatch '^(enabled|disabled|default|enabled_if_no_auto_rotation)$') {
        throw 'Device does not expose a restorable fixed-to-user-rotation mode.'
    }
    $oldFixedRotation = $fixed
    Invoke-Adb -Arguments @('install', '-r', $Apk) -CommandTimeoutSeconds 45 | Out-Null
    Invoke-Adb -Arguments @('shell', 'am', 'force-stop', $package) | Out-Null
    $logSince = Invoke-Adb -Arguments @('shell', 'date', "'+%m-%d %H:%M:%S.000'")
    Invoke-Adb -Arguments @('shell', 'settings', 'put', 'system', 'accelerometer_rotation', '0') | Out-Null
    Invoke-Adb -Arguments @('shell', 'wm', 'fixed-to-user-rotation', 'enabled') | Out-Null
    Invoke-Adb -Arguments @('shell', 'settings', 'put', 'system', 'user_rotation', '0') | Out-Null
    Invoke-Adb -Arguments @('shell', 'am', 'start', '-W', '-n', $component,
        '--ez', 'laiueScenario', 'true', '--ei', 'laiueScenarioRunId', "$runId") | Out-Null
    $appProcessId = Invoke-Adb -Arguments @('shell', 'pidof', $package)
    if ($appProcessId -notmatch '^\d+$') { throw 'NativeActivity did not retain a running application process.' }

    foreach ($checkpoint in 1..9) {
        $checkpointPattern = "event=checkpoint run=$runId phase=([A-Za-z_]+) checkpoint=$checkpoint tick=\d+ frame=\d+ width=(\d+) height=(\d+)"
        $log = Wait-Log -Description "checkpoint $checkpoint" -Condition {
            param($text)
            return $text -match $checkpointPattern
        }
        $match = [regex]::Match($log, $checkpointPattern)
        $phase = $match.Groups[1].Value
        Save-Capture -Name $phase -NativeWidth ([int]$match.Groups[2].Value) `
            -NativeHeight ([int]$match.Groups[3].Value) -Checkpoint $checkpoint | Out-Null
        $captureLog = Read-SmokeLog
        $latest = [regex]::Matches($captureLog, 'event=checkpoint run=\d+ phase=[A-Za-z_]+ checkpoint=(\d+)')
        if ($latest.Count -eq 0 -or [int]$latest[$latest.Count - 1].Groups[1].Value -ne $checkpoint -or
            $captureLog -match "event=capture_ack run=$runId checkpoint=$checkpoint(?:\s|$)") {
            throw "Checkpoint $checkpoint was not held throughout capture."
        }
        Invoke-Adb -Arguments @('shell', 'input', 'keyevent', 'KEYCODE_F12') | Out-Null
        Wait-Log -Description "capture acknowledgement $checkpoint" -Condition {
            param($text)
            return $text -match "event=capture_ack run=$runId checkpoint=$checkpoint(?:\s|$)"
        } -WaitSeconds 10 | Out-Null
    }
    $log = Wait-Log -Description 'successful scenario summary' -Condition {
        param($text)
        return $text -match "event=summary run=$runId status=PASS"
    }
    $summary = [regex]::Match($log, "LAIUE_SCENARIO event=summary run=$runId status=PASS[^\r\n]*").Value

    # Require real resize/presentation, not merely changed Android settings.
    $portraitFrame = Latest-Presentation -Log $log
    $portrait = Save-Capture -Name 'portrait' -NativeWidth ([int]$portraitFrame.Groups[1].Value) `
        -NativeHeight ([int]$portraitFrame.Groups[2].Value)
    if ($portrait.width -ge $portrait.height) { throw 'Portrait display rotation did not occur.' }
    $priorPresented = ([regex]::Matches($log, 'event=presented ')).Count
    Invoke-Adb -Arguments @('shell', 'settings', 'put', 'system', 'user_rotation', '1') | Out-Null
    $landscapeLog = Wait-Log -Description 'landscape resize and presentation' -Condition {
        param($text)
        $presented = [regex]::Matches($text, 'event=presented run=\d+ generation=\d+ frame=\d+ width=(\d+) height=(\d+)')
        return $presented.Count -gt $priorPresented -and
               [int]$presented[$presented.Count - 1].Groups[1].Value -gt
               [int]$presented[$presented.Count - 1].Groups[2].Value
    }
    $landscapeFrame = Latest-Presentation -Log $landscapeLog
    $landscape = Save-Capture -Name 'landscape' -NativeWidth ([int]$landscapeFrame.Groups[1].Value) `
        -NativeHeight ([int]$landscapeFrame.Groups[2].Value)
    if ($landscape.width -le $landscape.height) { throw 'Landscape display rotation did not occur.' }
    $rotationVerified = $true

    $beforePause = Read-SmokeLog
    $lostCount = ([regex]::Matches($beforePause, 'event=lifecycle run=\d+ command=lost_focus ')).Count
    Invoke-Adb -Arguments @('shell', 'input', 'keyevent', 'KEYCODE_HOME') | Out-Null
    Wait-Log -Description 'HOME and lost focus' -Condition {
        param($text)
        return ([regex]::Matches($text, 'event=lifecycle run=\d+ command=lost_focus ')).Count -gt $lostCount
    } | Out-Null
    # Take baselines after confirmed loss of focus: a pre-HOME frame cannot
    # count as proof of successful resumed presentation.
    $pausedLog = Read-SmokeLog
    $presentedCount = ([regex]::Matches($pausedLog, 'event=presented ')).Count
    $gainedCount = ([regex]::Matches($pausedLog, 'event=lifecycle run=\d+ command=gained_focus ')).Count
    Invoke-Adb -Arguments @('shell', 'am', 'start', '-W', '-n', $component, '-f', '0x20020000') | Out-Null
    $resumedProcess = Invoke-Adb -Arguments @('shell', 'pidof', $package)
    if ($resumedProcess -ne $appProcessId) { throw 'HOME/resume restarted the application process.' }
    $resumedLog = Wait-Log -Description 'gained focus and new presented frame' -Condition {
        param($text)
        $gained = [regex]::Matches($text, 'event=lifecycle run=\d+ command=gained_focus ')
        $presented = [regex]::Matches($text, 'event=presented ')
        return $gained.Count -gt $gainedCount -and $presented.Count -gt $presentedCount -and
               $presented[$presented.Count - 1].Index -gt $gained[$gained.Count - 1].Index
    }
    $resumedFrame = Latest-Presentation -Log $resumedLog
    Save-Capture -Name 'resumed' -NativeWidth ([int]$resumedFrame.Groups[1].Value) `
        -NativeHeight ([int]$resumedFrame.Groups[2].Value) | Out-Null
    $homeResumeVerified = $true
    Read-SmokeLog | Out-Null
} catch {
    $testIssue = $_.Exception.Message
} finally {
    # Restore under a separate bounded deadline, attempting every setting.
    # PASS is written only after all these calls succeed.
    $deadlineSeconds = $clock.Elapsed.TotalSeconds + 60
    foreach ($restore in @(
        { Restore-Setting -Name 'user_rotation' -Original $oldUserRotation },
        { Restore-Setting -Name 'accelerometer_rotation' -Original $oldAutoRotation },
        { if ($null -ne $oldFixedRotation) {
            Invoke-Adb -Arguments @('shell', 'wm', 'fixed-to-user-rotation', $oldFixedRotation) | Out-Null
            $restoredFixed = Invoke-Adb -Arguments @('shell', 'wm', 'fixed-to-user-rotation')
            if ($restoredFixed -ne $oldFixedRotation) { throw 'Fixed rotation restoration did not recover its original value.' }
        } }
    )) {
        try { & $restore } catch { $restoreErrors.Add($_.Exception.Message) }
    }
}

$passed = $null -eq $testIssue -and $restoreErrors.Count -eq 0 -and
          $rotationVerified -and $homeResumeVerified -and $captures.Count -eq 12
$result = [pscustomobject]@{
    status = $(if ($passed) { 'PASS' } else { 'FAIL' })
    run = $runId; serial = $Serial; processId = $appProcessId; apk = $Apk
    elapsedSeconds = $clock.Elapsed.TotalSeconds; scenarioSummary = $summary
    captures = $captures.ToArray(); rotationVerified = $rotationVerified
    homeResumeVerified = $homeResumeVerified; settingsRestored = $restoreErrors.Count -eq 0
    originalIssue = $testIssue; restorationIssues = $restoreErrors.ToArray()
    limits = 'External adb captures inspect a held native checkpoint and the shared-decoder viewport. Visual correctness and frame-exact GPU readback are not certified. Frame wall time includes presentation/vsync and excludes ACK waits. Memory counters are scoped geometry/CPU-shadow bytes, not total VRAM or RSS.'
}
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding utf8
if (-not $passed) {
    throw "Android walk runtime smoke FAIL: $testIssue $($restoreErrors -join '; '). Report: $reportPath"
}
Write-Host "Android walk runtime smoke PASS: $OutputDir"
