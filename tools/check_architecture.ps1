param(
    [string]$SourceRoot = (Join-Path $PSScriptRoot '..\src')
)

$ErrorActionPreference = 'Stop'
$sourceRootPath = [System.IO.Path]::GetFullPath($SourceRoot)
$checkerPath = Join-Path $PSScriptRoot 'check_architecture.cmake'

# Keep a single dependency map in the CMake checker used by configure and CI.
& cmake "-DSOURCE_ROOT=$sourceRootPath" -P $checkerPath
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
