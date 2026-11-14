param([string]$Bin, [string]$Include = '*.dll', [int]$Show = 60, [string]$DisasmFile)
# Finds the MSVC 14.52 ARM64 store-order pattern in built binaries: inside one
# basic block a store of zeros overwrites bytes that an earlier store of other
# data wrote through the same, unchanged base register, with no load through
# that register in between. A correct compiler never leaves such an earlier
# store dead, so every hit is either this miscompile or a scanner gap.
$ErrorActionPreference = 'Stop'

function Get-Symbols([string]$module) {
    $map = [IO.Path]::ChangeExtension($module, '.map')
    if (-not (Test-Path $map)) { return @() }
    $symbols = foreach ($line in [IO.File]::ReadAllLines($map)) {
        if ($line -match '^\s+[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\s+f\s') {
            [pscustomobject]@{ Name = $Matches[1]; Va = [Convert]::ToUInt64($Matches[2], 16) }
        }
    }
    @($symbols | Sort-Object Va)
}

function Get-Function($symbols, [uint64]$va) {
    $best = $null
    foreach ($symbol in $symbols) { if ($symbol.Va -le $va) { $best = $symbol } else { break } }
    if ($null -eq $best) { return '?' }
    '{0}+0x{1:x}' -f $best.Name, ($va - $best.Va)
}

$lineRegex = [regex]'^\s+([0-9A-F]{16}):\s+[0-9A-F]{8}\s+(\S+)\s*(.*)$'
$memoryRegex = [regex]'\[(\w+)(?:,#(-?0x[0-9A-F]+|-?\d+))?\](!?)(?:,#(-?0x[0-9A-F]+|-?\d+))?'
$branchRegex = [regex]'^(b|bl|blr|br|ret|cbz|cbnz|tbz|tbnz|beq|bne|bcs|bhs|bcc|blo|bmi|bpl|bvs|bvc|bhi|bls|bge|blt|bgt|ble|bal|brk|svc|udf)$'
$width = @{ x = 8; w = 4; q = 16; d = 8; s = 4; h = 2; b = 1 }
$hits = [Collections.Generic.List[string]]::new()
$scanned = 0
$files = if ($DisasmFile) { @(Get-Item $DisasmFile) } else { Get-ChildItem $Bin -File -Filter $Include }
foreach ($file in $files) {
    ++$scanned
    $lines = if ($DisasmFile) { [IO.File]::ReadAllLines($DisasmFile) } else { & dumpbin /nologo /disasm $file.FullName }
    $targets = [Collections.Generic.HashSet[uint64]]::new()
    foreach ($line in $lines) {
        $match = $lineRegex.Match($line)
        if ($match.Success -and $branchRegex.IsMatch($match.Groups[2].Value) -and
            $match.Groups[3].Value -match '([0-9A-F]{16})') {
            [void]$targets.Add([Convert]::ToUInt64($Matches[1], 16))
        }
    }
    $symbols = $null
    $zero = [Collections.Generic.HashSet[string]]::new()
    $stores = [Collections.Generic.List[object]]::new()
    foreach ($line in $lines) {
        $match = $lineRegex.Match($line)
        if (-not $match.Success) { continue }
        $va = [Convert]::ToUInt64($match.Groups[1].Value, 16)
        $op = $match.Groups[2].Value
        $operandText = $match.Groups[3].Value.Trim()
        if ($targets.Contains($va)) { $zero.Clear(); $stores.Clear() }
        if ($branchRegex.IsMatch($op)) { $zero.Clear(); $stores.Clear(); continue }
        $operands = @($operandText -split ',')
        if ($op -eq 'movi' -and $operandText -match '^v(\d+)\.\w+,#0$') { [void]$zero.Add($Matches[1]); continue }
        if ($op -eq 'fmov' -and $operandText -match '^[dsh](\d+),[xw]zr$') { [void]$zero.Add($Matches[1]); continue }
        $memory = $memoryRegex.Match($operandText)
        $writeback = $memory.Success -and ($memory.Groups[3].Value -eq '!' -or $memory.Groups[4].Success)
        if ($op -match '^(stp|stnp|str|stur|strb|strh|sturb|sturh)$' -and $memory.Success) {
            $registers = @($operandText.Substring(0, $memory.Index).TrimEnd(',') -split ',')
            $base = $memory.Groups[1].Value
            $offset = if ($memory.Groups[2].Success) { [int64]$memory.Groups[2].Value } else { 0 }
            if ($memory.Groups[4].Success) { $offset = 0 }
            $size = if ($op -in 'strb', 'sturb') { 1 } elseif ($op -in 'strh', 'sturh') { 2 } else { $width[$registers[0].Substring(0, 1)] }
            $size *= $registers.Count
            $isZero = $true
            foreach ($register in $registers) {
                if ($register -in 'xzr', 'wzr') { continue }
                if ($register -match '^[qdshb](\d+)$' -and $zero.Contains($Matches[1])) { continue }
                $isZero = $false
            }
            if ($isZero) {
                foreach ($earlier in $stores) {
                    if ($earlier.Base -eq $base -and -not $earlier.Zero -and
                        $offset -lt $earlier.Offset + $earlier.Size -and $earlier.Offset -lt $offset + $size) {
                        if ($null -eq $symbols) { $symbols = Get-Symbols $file.FullName }
                        $hits.Add(('{0}: {1} {2} @0x{3:x} [{4}] overwrites {5} {6} @0x{7:x}' -f
                            $file.Name, $op, $operandText, $va, (Get-Function $symbols $va),
                            $earlier.Op, $earlier.Args, $earlier.Va))
                    }
                }
            }
            if ($writeback) { $stores.Clear() }
            else {
                $stores.Add([pscustomobject]@{ Base = $base; Offset = $offset; Size = $size; Zero = $isZero; Op = $op; Args = $operandText; Va = $va })
            }
            continue
        }
        if ($memory.Success) {
            # A load through this base may read the earlier stores.
            $base = $memory.Groups[1].Value
            [void]$stores.RemoveAll([Predicate[object]]{ param($s) $s.Base -eq $base })
            if ($writeback) { $stores.Clear() }
        }
        # Registers this instruction writes: the first operand, both for pairs.
        $written = @($operands[0])
        if ($op -match '^ld[an]?p' -or $op -match '^ld\w*xp$') { $written += $operands[1] }
        foreach ($register in $written) {
            $register = $register.Trim()
            if ($register -match '^[qdshbv](\d+)') { [void]$zero.Remove($Matches[1]) }
            elseif ($register -match '^[xw](\d+)$') {
                $name = "x$($Matches[1])"
                [void]$stores.RemoveAll([Predicate[object]]{ param($s) $s.Base -eq $name })
            }
            elseif ($register -eq 'sp') {
                [void]$stores.RemoveAll([Predicate[object]]{ param($s) $s.Base -eq 'sp' })
            }
        }
    }
}
"=== scanned $scanned binaries ($Include) in $Bin, $($hits.Count) store-order hits"
$hits | Select-Object -First $Show
