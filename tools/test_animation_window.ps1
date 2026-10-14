param(
    [Parameter(Mandatory = $true)][string]$DemoExe,
    [Parameter(Mandatory = $true)][string]$OutputDir,
    [int]$TimeoutSeconds = 60
)
$ErrorActionPreference = 'Stop'
$taskExe = (Resolve-Path -LiteralPath $DemoExe).Path
$taskOutput = [IO.Path]::GetFullPath($OutputDir)
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
$taskEnvironment = @{
    LAIUE_ANIMATION_DEMO_MODE = 'window'
    LAIUE_ANIMATION_REQUIRE_FULL = '1'
    LAIUE_ANIMATION_REQUIRE_WINDOW = '1'
    LAIUE_ANIMATION_WINDOW_FRAMES = '12'
}
$taskPrevious = @{}
try {
    foreach ($taskName in $taskEnvironment.Keys) {
        $taskPrevious[$taskName] = [Environment]::GetEnvironmentVariable($taskName)
        [Environment]::SetEnvironmentVariable($taskName, $taskEnvironment[$taskName])
    }
    $taskProcess = Start-Process -FilePath $taskExe -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput "$taskOutput/stdout.log" `
        -RedirectStandardError "$taskOutput/stderr.log"
    if (!$taskProcess.WaitForExit($TimeoutSeconds * 1000)) {
        $taskProcess.Kill()
        $taskProcess.WaitForExit()
        throw 'Animation window did not finish its 12 frames'
    }
    $taskProcess.WaitForExit()
    if ($taskProcess.ExitCode -ne 0) {
        throw "Animation window failed: exit $($taskProcess.ExitCode)"
    }
    $taskText = Get-Content -LiteralPath "$taskOutput/stdout.log" -Raw
    if ($taskText -notmatch 'animation: window presentation passed') {
        throw 'Animation did not confirm actual window presentation'
    }
    $taskDiagnostics = Get-Content -LiteralPath "$taskOutput/stderr.log" -Raw
    if ($taskDiagnostics -match 'Validation Error|VUID-') {
        throw 'Animation window reported Vulkan validation errors'
    }
    Write-Output 'Animation: 12 actual window frames passed'
} finally {
    foreach ($taskName in $taskPrevious.Keys) {
        [Environment]::SetEnvironmentVariable($taskName, $taskPrevious[$taskName])
    }
}
