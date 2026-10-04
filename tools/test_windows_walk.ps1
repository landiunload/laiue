#Requires -Version 7.2
param(
    [Parameter(Mandatory = $true)][string]$GameExe,
    [Parameter(Mandatory = $true)][string]$FrameCheckExe,
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [ValidateRange(30, 600)][int]$TimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$GameExe = (Resolve-Path -LiteralPath $GameExe).Path
$FrameCheckExe = (Resolve-Path -LiteralPath $FrameCheckExe).Path
$OutputDir = [System.IO.Path]::GetFullPath($OutputDir)
if ((Test-Path -LiteralPath $OutputDir) -and
    @(Get-ChildItem -LiteralPath $OutputDir -Force).Count -ne 0) {
    throw 'Use an empty output directory for the Windows scenario.'
}
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

$info = [System.Diagnostics.ProcessStartInfo]::new($GameExe)
$info.UseShellExecute = $false
$info.CreateNoWindow = $true
$info.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden
$info.RedirectStandardOutput = $true
$info.RedirectStandardError = $true
$info.WorkingDirectory = Split-Path -Parent $GameExe
$info.ArgumentList.Add('--scenario')
$info.Environment['LAIUE_SCENARIO_OUTPUT'] = $OutputDir
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $info
$clock = [System.Diagnostics.Stopwatch]::StartNew()
$peakWorkingSet = 0L
$started = $false
try {
    if (-not $process.Start()) { throw 'Could not start the walk game.' }
    $started = $true
    $outputTask = $process.StandardOutput.ReadToEndAsync()
    $errorTask = $process.StandardError.ReadToEndAsync()
    while (-not $process.WaitForExit(100)) {
        $process.Refresh()
        $peakWorkingSet = [Math]::Max($peakWorkingSet, $process.PeakWorkingSet64)
        if ($clock.Elapsed.TotalSeconds -ge $TimeoutSeconds) { throw 'Windows scenario timed out.' }
    }
    $output = $outputTask.GetAwaiter().GetResult()
    $errors = $errorTask.GetAwaiter().GetResult()
    [System.IO.File]::WriteAllText((Join-Path $OutputDir 'stdout.log'), $output)
    [System.IO.File]::WriteAllText((Join-Path $OutputDir 'stderr.log'), $errors)
    if ($process.ExitCode -ne 0 -or $errors.Length -ne 0 -or
        $output -notmatch 'LAIUE_SCENARIO event=summary status=PASS\b' -or
        $output -match 'LAIUE_SCENARIO event=failure') {
        throw 'Windows walk scenario failed; inspect stdout.log and stderr.log.'
    }
    Add-Type -AssemblyName System.Drawing
    $captures = foreach ($phase in 'stand','walk','sprint','jump','turn','first_person','edit','rebase','settle') {
        if ($output -notmatch "event=checkpoint phase=$phase\b") { throw "Missing phase: $phase" }
        $bitmapPath = Join-Path $OutputDir "$phase.bmp"
        $pngPath = Join-Path $OutputDir "$phase.png"
        # File conversion only: the BMP comes directly from the requested GPU
        # frame before presentation, never from the desktop or another app.
        $bitmap = [System.Drawing.Bitmap]::new($bitmapPath)
        try { $bitmap.Save($pngPath, [System.Drawing.Imaging.ImageFormat]::Png) }
        finally { $bitmap.Dispose() }
        $checkJson = & $FrameCheckExe $pngPath
        if ($LASTEXITCODE -ne 0) { throw "Viewport pixel validation failed: $phase" }
        $check = $checkJson | ConvertFrom-Json
        if ($check.status -ne 'PASS' -or -not $check.useful) { throw "Empty viewport: $phase" }
        [pscustomobject]@{
            phase = $phase; path = $pngPath; pixels = $check
            sha256 = (Get-FileHash -LiteralPath $pngPath -Algorithm SHA256).Hash
            source = 'requested_gpu_frame_before_present'
        }
    }
    [pscustomobject]@{
        status = 'PASS'; executable = $GameExe; elapsedSeconds = $clock.Elapsed.TotalSeconds
        processPeakWorkingSetBytes = $peakWorkingSet
        scenarioSummary = [regex]::Match($output, 'LAIUE_SCENARIO event=summary status=PASS[^\r\n]*').Value
        captures = @($captures)
        limits = 'Useful viewport pixels do not certify anatomy or UV correctness. Working set is process memory, not total GPU memory. Frame wall time includes presentation/vsync; explicit capture and screenshot holds are outside its samples.'
    } | ConvertTo-Json -Depth 7 | Set-Content -LiteralPath (Join-Path $OutputDir 'report.json') -Encoding utf8
    Write-Host "Windows walk runtime smoke PASS: $OutputDir"
} finally {
    if ($started -and -not $process.HasExited) { $process.Kill($true) }
    $process.Dispose()
}
