param(
    [Parameter(Mandatory = $true)]
    [string]$RuntimeDirectory
)

$ErrorActionPreference = 'Stop'
$runtimePath = [System.IO.Path]::GetFullPath($RuntimeDirectory).
    TrimEnd([System.IO.Path]::DirectorySeparatorChar,
            [System.IO.Path]::AltDirectorySeparatorChar)

$blocking = [System.Collections.Generic.List[object]]::new()
Get-Process -Name 'laiue', 'laiue_server' -ErrorAction SilentlyContinue |
    ForEach-Object {
        try {
            $executablePath = $_.Path
            if (-not [string]::IsNullOrWhiteSpace($executablePath)) {
                $processDirectory = [System.IO.Path]::GetDirectoryName(
                    [System.IO.Path]::GetFullPath($executablePath)).
                    TrimEnd([System.IO.Path]::DirectorySeparatorChar,
                            [System.IO.Path]::AltDirectorySeparatorChar)
                if ($processDirectory -ieq $runtimePath) {
                    $blocking.Add([pscustomobject]@{
                        Name = $_.ProcessName
                        Id = $_.Id
                        Path = $executablePath
                    })
                }
            }
        }
        catch {
            # Чужой защищённый процесс не относится к этому build tree.
        }
    }

if ($blocking.Count -ne 0) {
    $details = ($blocking | ForEach-Object {
        "  $($_.Name).exe (PID $($_.Id)): $($_.Path)"
    }) -join [Environment]::NewLine
    Write-Error -ErrorAction Continue @"
Нельзя обновить DLL: запущен runtime из каталога текущей сборки.
$details
Закройте игру/сервер обычным способом и повторите сборку.
Если процесс завис: Stop-Process -Id <PID>
"@
    exit 2
}

Write-Host "Runtime lock check: OK ($runtimePath)"
