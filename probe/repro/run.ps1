param([string]$OutDir = (Join-Path ([IO.Path]::GetTempPath()) 'repro'), [string]$Arch = '/arch:armv8.2', [switch]$Disassemble)
$ErrorActionPreference = 'Stop'
$source = $PSScriptRoot
New-Item -ItemType Directory -Force $OutDir | Out-Null
$engine = "/O2 /Ot /Oi /GF /Gy /Gw /volatile:iso /Zc:inline $Arch /GS- /Ob3"
$variants = [ordered]@{
    'engine /GL'              = "$engine /GL"
    'engine'                  = $engine
    'engine /GL no arch'      = ($engine -replace ([regex]::Escape($Arch) + ' '), '') + ' /GL'
    'engine /GL /Ob2'         = ($engine -replace '/Ob3', '/Ob2') + ' /GL'
    'engine /GL /d2SSAOptimizer-' = "$engine /GL /d2SSAOptimizer-"
    'engine /d2SSAOptimizer-' = "$engine /d2SSAOptimizer-"
    '/O2 /GL'                 = '/O2 /GL'
    '/O2'                     = '/O2'
    '/O1'                     = '/O1'
    '/Od'                     = '/Od'
}
$index = 0
foreach ($name in $variants.Keys) {
    $dir = Join-Path $OutDir "v$index"
    ++$index
    New-Item -ItemType Directory -Force $dir | Out-Null
    $flags = $variants[$name] -split ' ' | Where-Object { $_ }
    $compile = & cl /nologo /c /std:c17 /W4 /WX @flags "/Fo$dir\repro.obj" (Join-Path $source 'repro.c') 2>&1
    if ($LASTEXITCODE -ne 0) { "[$name] compile failed: $compile"; continue }
    $null = & cl /nologo /c /std:c17 /O2 /W4 "/Fo$dir\check.obj" (Join-Path $source 'check.c') 2>&1
    $ltcg = if ($flags -contains '/GL') { '/LTCG' } else { '/INCREMENTAL:NO' }
    $link = & link /nologo $ltcg /OPT:REF /MAP "/OUT:$dir\repro.exe" "$dir\repro.obj" "$dir\check.obj" 2>&1
    if ($LASTEXITCODE -ne 0) { "[$name] link failed: $link"; continue }
    $result = & "$dir\repro.exe"
    "[$name] exit $LASTEXITCODE`: $result   ($($variants[$name]))"
    if ($Disassemble -or $LASTEXITCODE -ne 0) {
        $map = Get-Content "$dir\repro.map"
        $base = [Convert]::ToUInt64((($map | Select-String 'Preferred load address is ([0-9a-fA-F]+)').Matches[0].Groups[1].Value), 16)
        $symbols = foreach ($line in $map) {
            if ($line -match '^\s+[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+f\s') {
                [pscustomobject]@{ Name = $Matches[1]; Va = [Convert]::ToUInt64($Matches[2], 16) }
            }
        }
        $symbols = @($symbols | Sort-Object Va)
        $asm = & dumpbin /nologo /disasm "$dir\repro.exe"
        foreach ($function in 'ReproInit', 'ReproInitSmall') {
            $at = [Array]::FindIndex($symbols, [Predicate[object]]{ param($s) $s.Name -eq $function })
            if ($at -lt 0) { "  $function not in map"; continue }
            $from = $symbols[$at].Va
            $next = @($symbols | Where-Object { $_.Va -gt $from } | Select-Object -First 1)
            $to = if ($next.Count -gt 0) { [Math]::Min($next[0].Va, $from + 0x400) } else { $from + 0x400 }
            "  --- $function"
            foreach ($line in $asm) {
                if ($line -match '^\s+([0-9A-F]{16}):') {
                    $va = [Convert]::ToUInt64($Matches[1], 16)
                    if ($va -ge $from -and $va -lt $to) { "  $line" }
                }
            }
        }
    }
}
