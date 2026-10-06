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
$smokeLogLines = [System.Collections.Generic.List[string]]::new()
$smokeLogSeen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
$captures = [System.Collections.Generic.List[object]]::new()
$restoreErrors = [System.Collections.Generic.List[string]]::new()
$restoredSettings = @{}
$originalSettings = $null
$rotationSettingsModified = $false
$baselineSamples = [System.Collections.Generic.List[object]]::new()
$restoreAttempts = [System.Collections.Generic.List[object]]::new()
$testIssue = $null
$summary = $null
$rotationVerified = $false
$homeResumeVerified = $false
$gameplayStatePreserved = $false
$gameplayState = $null
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
    # Logcat is a bounded ring: retain each observed line once so later
    # checkpoint polls cannot discard earlier presentation/lifecycle evidence.
    foreach ($line in $raw -split '\r?\n') {
        if ($line.Length -ne 0 -and $smokeLogSeen.Add($line)) { $smokeLogLines.Add($line) }
    }
    $history = $smokeLogLines -join "`n"
    [System.IO.File]::WriteAllText((Join-Path $OutputDir 'logcat.txt'), $history)
    # App errors are scoped by PID; scenario messages additionally by run ID.
    if ($appProcessId -and $history -match 'FATAL EXCEPTION|Fatal signal|(?:^|\n)[^\n]*\sE\s+laiue[.]walk\s*:') {
        throw "Android walk process $appProcessId reported an error; inspect $(Join-Path $OutputDir 'logcat.txt')."
    }
    $lines = @($history -split '\r?\n' | Where-Object {
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

function Gameplay-State {
    param([string]$Log, [string]$Event)
    $lines = [regex]::Matches($Log, "event=$Event run=$runId [^\r\n]*")
    if ($lines.Count -eq 0) { throw "No $Event gameplay state was reported." }
    $state = [regex]::Match($lines[$lines.Count - 1].Value,
        'game_tick=\d+ game_revision=\d+ yaw=-?\d+[.]\d+ pitch=-?\d+[.]\d+ first_person=[01] material=[123] root_block_x=-?\d+ root_block_y=-?\d+ root_block_z=-?\d+ root_fraction_x=-?\d+[.]\d+ root_fraction_y=-?\d+[.]\d+ root_fraction_z=-?\d+[.]\d+ provider_frame_x=-?\d+ provider_frame_y=-?\d+ provider_frame_z=-?\d+')
    if (-not $state.Success) { throw "Incomplete $Event gameplay state." }
    return $state.Value
}

function Get-RotationSettings {
    $auto = Invoke-Adb -Arguments @('shell', 'settings', 'get', 'system', 'accelerometer_rotation')
    $rotation = Invoke-Adb -Arguments @('shell', 'settings', 'get', 'system', 'user_rotation')
    $fixed = Invoke-Adb -Arguments @('shell', 'wm', 'fixed-to-user-rotation')
    $settings = [ordered]@{
        user_rotation = $rotation; accelerometer_rotation = $auto
        fixed_to_user_rotation = $fixed
    }
    Assert-RotationSettings -Settings $settings -Description 'observed'
    return $settings
}

function Assert-RotationSettings {
    param([AllowNull()][System.Collections.IDictionary]$Settings, [string]$Description = 'original')
    if ($null -eq $Settings -or $Settings['accelerometer_rotation'] -notmatch '^(0|1|null)$' -or
        $Settings['user_rotation'] -notmatch '^(0|1|2|3|null)$' -or
        $Settings['fixed_to_user_rotation'] -notmatch '^(enabled|disabled|default|enabled_if_no_auto_rotation)$') {
        $settingsText = [pscustomobject]$Settings | ConvertTo-Json -Compress
        throw "No complete, valid $Description rotation settings are available: $settingsText"
    }
}

function Test-RotationSettingsEqual {
    param([System.Collections.IDictionary]$Left, [System.Collections.IDictionary]$Right)
    foreach ($name in @('user_rotation', 'accelerometer_rotation', 'fixed_to_user_rotation')) {
        if ([string]$Left[$name] -cne [string]$Right[$name]) { return $false }
    }
    return $true
}

function Wait-RotationSettings {
    param([AllowNull()][System.Collections.IDictionary]$Expected = $null,
          [System.Collections.Generic.List[object]]$Samples, [int]$WaitSeconds = 8)
    $until = [Math]::Min($deadlineSeconds, $clock.Elapsed.TotalSeconds + $WaitSeconds)
    $previous = $null
    $observed = $null
    $matching = 0
    do {
        $observed = Get-RotationSettings
        foreach ($name in $observed.Keys) { $restoredSettings[$name] = $observed[$name] }
        $Samples.Add([pscustomobject]@{
            elapsedSeconds = $clock.Elapsed.TotalSeconds; settings = [pscustomobject]$observed
        })
        if ($null -ne $Expected) {
            $matching = if (Test-RotationSettingsEqual -Left $Expected -Right $observed) { $matching + 1 } else { 0 }
        } elseif ($null -ne $previous -and (Test-RotationSettingsEqual -Left $previous -Right $observed)) {
            $matching++
        } else {
            $matching = 1
        }
        if ($matching -ge 3) { return [pscustomobject]@{ stable = $true; observed = $observed } }
        $previous = $observed
        if ($clock.Elapsed.TotalSeconds -ge $until) { break }
        Start-Sleep -Milliseconds 200
    } while ($clock.Elapsed.TotalSeconds -lt $until)
    return [pscustomobject]@{ stable = $false; observed = $observed }
}

function Restore-Setting {
    param([string]$Name, [string]$Original)
    if ($Original -ceq 'null') {
        Invoke-Adb -Arguments @('shell', 'settings', 'delete', 'system', $Name) | Out-Null
    } else {
        Invoke-Adb -Arguments @('shell', 'settings', 'put', 'system', $Name, $Original) | Out-Null
    }
}

function Restore-RotationSettings {
    param([AllowNull()][System.Collections.IDictionary]$Original, [bool]$Modified)
    # A failed/partial baseline never authorizes writes during cleanup.
    if (-not $Modified) { return }
    Assert-RotationSettings -Settings $Original
    for ($number = 1; $number -le 3; $number++) {
        $attempt = [pscustomobject]@{
            number = $number; expected = [pscustomobject]$Original; status = 'RUNNING'
            samples = [System.Collections.Generic.List[object]]::new()
            errors = [System.Collections.Generic.List[string]]::new(); observed = $null
        }
        $restoreAttempts.Add($attempt)
        # Fixed mode may trigger policy work: restore it before the final pair.
        # Try every write even if an earlier command fails. Command failures
        # remain fatal; only a readable, mismatched tuple permits a retry.
        foreach ($write in @(
            { Invoke-Adb -Arguments @('shell', 'wm', 'fixed-to-user-rotation', $Original['fixed_to_user_rotation']) | Out-Null },
            { Restore-Setting -Name 'user_rotation' -Original $Original['user_rotation'] },
            { Restore-Setting -Name 'accelerometer_rotation' -Original $Original['accelerometer_rotation'] }
        )) {
            try { & $write } catch { $attempt.errors.Add($_.Exception.Message) }
        }
        try {
            $verified = Wait-RotationSettings -Expected $Original -Samples $attempt.samples
            $attempt.observed = [pscustomobject]$verified.observed
        } catch {
            $attempt.errors.Add($_.Exception.Message)
        }
        if ($attempt.errors.Count -ne 0) {
            $attempt.status = 'ERROR'
            throw "Rotation restoration command failed: $($attempt.errors -join '; ')."
        }
        if ($verified.stable) {
            $attempt.status = 'PASS'
            return
        }
        $attempt.status = 'MISMATCH'
    }
    $expectedText = [pscustomobject]$Original | ConvertTo-Json -Compress
    $observedText = $attempt.observed | ConvertTo-Json -Compress
    throw "Rotation restoration expected $expectedText, observed $observedText after 3 attempts."
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
    Invoke-Adb -Arguments @('install', '-r', $Apk) -CommandTimeoutSeconds 45 | Out-Null
    Invoke-Adb -Arguments @('shell', 'am', 'force-stop', $package) | Out-Null
    # Installation and a fresh emulator's policy initialization can take time.
    # Capture a stable complete tuple after both, before the first mutation.
    $baseline = Wait-RotationSettings -Samples $baselineSamples
    if (-not $baseline.stable) { throw 'Original rotation settings did not reach a stable baseline.' }
    $originalSettings = $baseline.observed
    $oldAutoRotation = $originalSettings['accelerometer_rotation']
    $oldUserRotation = $originalSettings['user_rotation']
    $oldFixedRotation = $originalSettings['fixed_to_user_rotation']
    $logSince = Invoke-Adb -Arguments @('shell', 'date', "'+%m-%d %H:%M:%S.000'")
    $rotationSettingsModified = $true
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
    $gameplayState = Gameplay-State -Log $log -Event 'summary'

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
    if ((Gameplay-State -Log $landscapeLog -Event 'presented') -ne $gameplayState) {
        throw 'Rotation changed the held gameplay pose, world frame, camera or session revision.'
    }
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
    if ((Gameplay-State -Log $resumedLog -Event 'presented') -ne $gameplayState) {
        throw 'HOME/resume changed the held gameplay pose, world frame, camera or session revision.'
    }
    $gameplayStatePreserved = $true
    $homeResumeVerified = $true
    Read-SmokeLog | Out-Null
} catch {
    $testIssue = $_.Exception.Message
} finally {
    # Restore under a separate bounded deadline, attempting every setting.
    # PASS is written only after all these calls succeed.
    $deadlineSeconds = $clock.Elapsed.TotalSeconds + 60
    try {
        Restore-RotationSettings -Original $originalSettings -Modified $rotationSettingsModified
    } catch {
        $restoreErrors.Add($_.Exception.Message)
    }
}

$passed = $null -eq $testIssue -and $restoreErrors.Count -eq 0 -and
          $rotationVerified -and $homeResumeVerified -and $gameplayStatePreserved -and
          $captures.Count -eq 12
$result = [pscustomobject]@{
    status = $(if ($passed) { 'PASS' } else { 'FAIL' })
    run = $runId; serial = $Serial; processId = $appProcessId; apk = $Apk
    elapsedSeconds = $clock.Elapsed.TotalSeconds; scenarioSummary = $summary
    captures = $captures.ToArray(); rotationVerified = $rotationVerified
    homeResumeVerified = $homeResumeVerified; settingsRestored = $restoreErrors.Count -eq 0
    gameplayStatePreserved = $gameplayStatePreserved; gameplayState = $gameplayState
    originalSettings = [pscustomobject]@{
        user_rotation = $oldUserRotation; accelerometer_rotation = $oldAutoRotation
        fixed_to_user_rotation = $oldFixedRotation
    }
    restoredSettings = $restoredSettings
    settingsModified = $rotationSettingsModified; baselineSamples = $baselineSamples.ToArray()
    restorationAttempts = $restoreAttempts.ToArray()
    originalIssue = $testIssue; restorationIssues = $restoreErrors.ToArray()
    limits = 'External adb captures inspect a held native checkpoint and the shared-decoder viewport. Visual correctness and frame-exact GPU readback are not certified. Frame wall time includes presentation/vsync and excludes ACK waits. Memory counters are scoped geometry/CPU-shadow bytes, not total VRAM or RSS.'
}
$result | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $reportPath -Encoding utf8
if (-not $passed) {
    throw "Android walk runtime smoke FAIL: $testIssue $($restoreErrors -join '; '). Report: $reportPath"
}
Write-Host "Android walk runtime smoke PASS: $OutputDir"
