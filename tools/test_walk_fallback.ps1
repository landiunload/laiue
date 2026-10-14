#Requires -Version 7.2
param(
    [Parameter(Mandatory = $true)][string]$GameExe,
    [Parameter(Mandatory = $true)][string]$OutputDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$GameExe = (Resolve-Path -LiteralPath $GameExe).Path
$source = Split-Path -Parent $GameExe
$OutputDir = [IO.Path]::GetFullPath($OutputDir)
if ((Test-Path -LiteralPath $OutputDir) -and
    @(Get-ChildItem -LiteralPath $OutputDir -Force).Count -ne 0) {
    throw 'Use an empty output directory for fallback checks.'
}
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
$results = foreach ($profile in @(
    @{ name = 'no_voxel'; omit = @('laiue_voxel.dll') },
    @{ name = 'no_physics'; omit = @('laiue_physics.dll') },
    @{ name = 'no_actor'; omit = @('laiue_physics.dll', 'laiue_character.dll') }
)) {
    $stage = Join-Path $OutputDir $profile.name
    New-Item -ItemType Directory -Path $stage | Out-Null
    foreach ($file in $profile.omit) {
        if (-not (Test-Path -LiteralPath (Join-Path $source $file) -PathType Leaf)) {
            throw "The original runtime must include $file."
        }
    }
    foreach ($entry in Get-ChildItem -LiteralPath $source -Force) {
        if ($entry.Name -notin $profile.omit -and
            ($entry.Extension -eq '.dll' -or $entry.Name -eq (Split-Path -Leaf $GameExe) -or
             ($entry.PSIsContainer -and $entry.Name -eq 'assets'))) {
            Copy-Item -LiteralPath $entry.FullName -Destination $stage -Recurse
        }
    }
    $process = [Diagnostics.Process]::new()
    $process.StartInfo.FileName = Join-Path $stage (Split-Path -Leaf $GameExe)
    $process.StartInfo.WorkingDirectory = $stage
    $process.StartInfo.UseShellExecute = $false
    $process.StartInfo.CreateNoWindow = $true
    $process.StartInfo.RedirectStandardOutput = $true
    $process.StartInfo.RedirectStandardError = $true
    $process.StartInfo.ArgumentList.Add('--smoke')
    $started = $false
    $finished = $false
    try {
        if (-not $process.Start()) { throw "Cannot start $($profile.name)." }
        $started = $true
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $finished = $process.WaitForExit(60000)
        if (-not $finished) {
            $process.Kill($true)
            if (-not $process.WaitForExit(5000)) { throw 'Owned smoke process did not stop.' }
        }
        $output = $stdout.GetAwaiter().GetResult()
        $errors = $stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $stage 'stdout.log'), $output)
        [IO.File]::WriteAllText((Join-Path $stage 'stderr.log'), $errors)
        if (-not $finished -or $process.ExitCode -ne 0 -or $errors.Length -ne 0 -or
            $output -notmatch 'laiue walk: windowed smoke32 passed') {
            throw "Fallback $($profile.name) failed; inspect $stage."
        }
        [pscustomobject]@{ profile = $profile.name; status = 'PASS'; frames = 32;
            missingModules = $profile.omit; exitCode = $process.ExitCode }
    } finally {
        if ($started -and -not $process.HasExited) { $process.Kill($true) }
        $process.Dispose()
    }
}
@($results) | ConvertTo-Json -Depth 4 |
    Set-Content -LiteralPath (Join-Path $OutputDir 'fallback-report.json') -Encoding utf8
Write-Host "Walk fallback profiles PASS: $OutputDir"
