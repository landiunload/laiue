param(
    [string]$SourceRoot = (Join-Path $PSScriptRoot '..\src')
)

$ErrorActionPreference = 'Stop'
$sourceRootPath = [System.IO.Path]::GetFullPath($SourceRoot)

# Разрешённые направления include-зависимостей. core — composition root;
# остальные модули видят только себя и явно перечисленные нижние слои.
$allowed = @{
    content     = @('content')
    core        = @('content', 'core', 'game', 'gameplay', 'input',
                    'interaction', 'mesh', 'network', 'physics', 'platform', 'render', 'world')
    game        = @('game')
    gameplay    = @('game', 'gameplay', 'physics')
    input       = @('input')
    interaction = @('interaction', 'physics', 'world')
    launcher    = @('launcher')
    mesh        = @('mesh', 'render', 'world')
    network     = @('network')
    physics     = @('physics')
    platform    = @('platform')
    render      = @('content', 'render')
    runtime     = @('runtime')
    server      = @('game', 'gameplay', 'interaction', 'network', 'physics', 'server', 'world')
    world       = @('world')
}

$violations = [System.Collections.Generic.List[string]]::new()
$files = Get-ChildItem -LiteralPath $sourceRootPath -Recurse -File |
    Where-Object { $_.Extension -eq '.c' -or $_.Extension -eq '.h' } |
    Where-Object { $_.FullName -notmatch '[\\/]generated[\\/]' }

foreach ($file in $files) {
    $relative = [System.IO.Path]::GetRelativePath(
        $sourceRootPath, $file.FullName).Replace('\', '/')
    if (-not $relative.Contains('/')) { continue }
    $owner = $relative.Split('/')[0]
    if (-not $allowed.ContainsKey($owner)) {
        $violations.Add("${relative}: неизвестный модуль '$owner'")
        continue
    }

    $lineNumber = 0
    foreach ($line in Get-Content -LiteralPath $file.FullName) {
        $lineNumber++
        if ($line -notmatch '^\s*#include\s+"([^"]+)"') { continue }
        $include = $Matches[1].Replace('\', '/')
        if ($include.Contains('../')) {
            $violations.Add("${relative}:${lineNumber}: запрещён относительный include '$include'")
            continue
        }
        if (-not $include.Contains('/')) { continue }

        $dependency = $include.Split('/')[0]
        if ($dependency -notin $allowed[$owner]) {
            $violations.Add(
                "${relative}:${lineNumber}: модуль '$owner' не может включать '$include'")
        }
    }
}

if ($violations.Count -gt 0) {
    $violations | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Host "Architecture boundaries: OK ($($files.Count) files)"
