param(
    [Parameter(Mandatory)][ValidateSet('map', 'stress')][string]$Mode,
    [string]$BuildDir = 'build/windows-msvc-arm64',
    [string]$Config = 'Release',
    [int]$Runs = 1000,
    [int]$Parallel = 4,
    [string]$Label = 'stress',
    [int]$ShowReports = 4,
    [string[]]$Frames = @()
)
$ErrorActionPreference = 'Stop'
$bin = Join-Path $BuildDir "bin/$Config"
$exe = Join-Path $bin 'laiue_physics_determinism_test.exe'
if (-not (Test-Path $exe)) { throw "missing $exe" }

$maps = @{}
function Get-Map([string]$module) {
    $key = $module.ToLowerInvariant()
    if ($maps.ContainsKey($key)) { return $maps[$key] }
    $path = Join-Path $bin ([IO.Path]::ChangeExtension($module, '.map'))
    $map = $null
    if (Test-Path $path) {
        $lines = [IO.File]::ReadAllLines((Resolve-Path $path))
        $base = 0
        $symbols = [Collections.Generic.List[object]]::new()
        foreach ($line in $lines) {
            if ($line -match 'Preferred load address is ([0-9a-fA-F]+)') {
                $base = [Convert]::ToUInt64($Matches[1], 16)
            }
            elseif ($line -match '^\s+[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+f\s') {
                $symbols.Add([pscustomobject]@{ Name = $Matches[1]; Va = [Convert]::ToUInt64($Matches[2], 16) })
            }
        }
        $map = [pscustomobject]@{ Base = $base; Symbols = @($symbols | Sort-Object Va) }
    }
    $maps[$key] = $map
    return $map
}

function Resolve-Address([string]$module, [uint64]$rva) {
    $map = Get-Map $module
    if ($null -eq $map) { return $null }
    $va = $map.Base + $rva
    $best = $null
    $next = $null
    foreach ($symbol in $map.Symbols) {
        if ($symbol.Va -le $va) { $best = $symbol }
        elseif ($null -eq $next) { $next = $symbol; break }
    }
    if ($null -eq $best) { return $null }
    [pscustomobject]@{
        Text = '{0}+0x{1:x}' -f $best.Name, ($va - $best.Va)
        Start = $best.Va
        End = if ($null -ne $next) { $next.Va } else { $best.Va + 0x400 }
        Va = $va
    }
}

function Add-Symbols([string]$text) {
    $lines = $text -split "`r?`n"
    $out = foreach ($line in $lines) {
        if ($line -match '([A-Za-z0-9_.]+\.(?:exe|dll))\+0x([0-9a-f]+)') {
            $resolved = Resolve-Address $Matches[1] ([Convert]::ToUInt64($Matches[2], 16))
            if ($null -ne $resolved) { "$line  [$($resolved.Text)]" } else { $line }
        }
        else { $line }
    }
    $out -join "`n"
}

if ($Mode -eq 'map') {
    $disassembly = @{}
    foreach ($frame in $Frames) {
        if ($frame -notmatch '^([A-Za-z0-9_.]+\.(?:exe|dll))\+0x([0-9a-f]+)$') { throw "bad frame $frame" }
        $module = $Matches[1]
        $rva = [Convert]::ToUInt64($Matches[2], 16)
        $resolved = Resolve-Address $module $rva
        "=== $frame -> $(if ($resolved) { $resolved.Text } else { 'unresolved' })"
        if ($null -eq $resolved) { continue }
        if (-not $disassembly.ContainsKey($module)) {
            $disassembly[$module] = & dumpbin /nologo /disasm (Join-Path $bin $module)
        }
        # The whole function when it is short, otherwise a window around the address.
        $from = $resolved.Start
        $to = $resolved.End
        if ($to - $from -gt 0x600) {
            $from = [Math]::Max($resolved.Start, $resolved.Va - 0x300)
            $to = [Math]::Min($resolved.End, $resolved.Va + 0x80)
        }
        foreach ($line in $disassembly[$module]) {
            if ($line -match '^\s+([0-9A-F]{16}):') {
                $va = [Convert]::ToUInt64($Matches[1], 16)
                if ($va -ge $from -and $va -lt $to) {
                    $marker = if ($va -eq $resolved.Va) { '>>' } else { '  ' }
                    "$marker$line"
                }
            }
        }
    }
    foreach ($module in 'laiue_physics_determinism_test.exe', 'laiue_world.dll') {
        "=== functions of $module"
        $map = Get-Map $module
        foreach ($symbol in $map.Symbols) { '  0x{0:x} {1}' -f ($symbol.Va - $map.Base), $symbol.Name }
    }
    exit 0
}

$outDir = Join-Path ([IO.Path]::GetTempPath()) "probe-$Label"
New-Item -ItemType Directory -Force $outDir | Out-Null
$codes = @{}
$signatures = @{}
$reports = [Collections.Generic.List[string]]::new()
$running = [Collections.Generic.List[object]]::new()
$started = 0
$clock = [Diagnostics.Stopwatch]::StartNew()
while ($started -lt $Runs -or $running.Count -gt 0) {
    while ($started -lt $Runs -and $running.Count -lt $Parallel) {
        $output = Join-Path $outDir "$started.txt"
        $process = Start-Process -FilePath (Resolve-Path $exe) -WorkingDirectory (Resolve-Path $bin) `
            -NoNewWindow -PassThru -RedirectStandardOutput $output -RedirectStandardError "$output.err"
        $null = $process.Handle
        $running.Add([pscustomobject]@{ Process = $process; Output = $output; Index = $started })
        ++$started
    }
    $finished = @($running | Where-Object { $_.Process.HasExited })
    if ($finished.Count -eq 0) {
        Start-Sleep -Milliseconds 10
        continue
    }
    foreach ($entry in $finished) {
        $entry.Process.WaitForExit()
        $code = '0x{0:x8}' -f $entry.Process.ExitCode
        $codes[$code] = 1 + [int]$codes[$code]
        $text = [IO.File]::ReadAllText($entry.Output)
        if ($entry.Process.ExitCode -ne 0) {
            $lines = @($text -split "`r?`n" | Where-Object { $_ -match 'Unhandled exception|^\s+\S+\+0x|check failed' })
            $signature = Add-Symbols (($lines | Select-Object -First 6) -join "`n")
            $signatures[$signature] = 1 + [int]$signatures[$signature]
            if ($reports.Count -lt $ShowReports) {
                $reports.Add("--- run $($entry.Index) exit $code after $($clock.Elapsed.TotalSeconds.ToString('0.0')) s`n" + (Add-Symbols $text))
            }
        }
        Remove-Item $entry.Output, "$($entry.Output).err" -ErrorAction SilentlyContinue
        [void]$running.Remove($entry)
    }
}
"=== $Label ($Config, $Runs runs, $Parallel at once, $($clock.Elapsed.TotalSeconds.ToString('0')) s)"
foreach ($code in $codes.Keys | Sort-Object) { "  exit $code x $($codes[$code])" }
foreach ($signature in $signatures.Keys) { "--- $($signatures[$signature]) runs:`n$signature" }
foreach ($report in $reports) { $report }
exit 0
