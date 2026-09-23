param(
    [Parameter(Mandatory = $true)][string]$Aapt2,
    [Parameter(Mandatory = $true)][string]$Zipalign,
    [Parameter(Mandatory = $true)][string]$Apksigner,
    [Parameter(Mandatory = $true)][string]$FrameworkJar,
    [Parameter(Mandatory = $true)][string]$Manifest,
    [Parameter(Mandatory = $true)][string]$NativeLib,
    [Parameter(Mandatory = $true)][string]$Abi,
    [Parameter(Mandatory = $true)][string]$Keystore,
    [Parameter(Mandatory = $true)][string]$KeystorePassword,
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'
foreach ($tool in @($Aapt2, $Zipalign, $Apksigner, $FrameworkJar, $Manifest, $NativeLib, $Keystore)) {
    if (-not (Test-Path -LiteralPath $tool -PathType Leaf)) {
        throw "Android packaging input does not exist: $tool"
    }
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$unsigned = Join-Path $OutputDirectory 'laiue-walk-unsigned.apk'
$aligned = Join-Path $OutputDirectory 'laiue-walk-aligned.apk'
$output = Join-Path $OutputDirectory 'laiue-walk.apk'
Remove-Item -LiteralPath $unsigned, $aligned, $output -Force -ErrorAction SilentlyContinue

& $Aapt2 link --manifest $Manifest -I $FrameworkJar --min-sdk-version 28 `
    --target-sdk-version 35 --version-code 1 --version-name 0.7.0 `
    --auto-add-overlay -o $unsigned
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed with exit code $LASTEXITCODE" }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [System.IO.Compression.ZipFile]::Open($unsigned,
    [System.IO.Compression.ZipArchiveMode]::Update)
try {
    $entry = $archive.CreateEntry("lib/$Abi/liblaiue_walk.so",
        [System.IO.Compression.CompressionLevel]::NoCompression)
    $input = [System.IO.File]::OpenRead($NativeLib)
    try {
        $stream = $entry.Open()
        try { $input.CopyTo($stream) } finally { $stream.Dispose() }
    } finally { $input.Dispose() }
} finally { $archive.Dispose() }

& $Zipalign -f -p 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw "zipalign failed with exit code $LASTEXITCODE" }
& $Apksigner sign --ks $Keystore --ks-pass "pass:$KeystorePassword" --out $output $aligned
if ($LASTEXITCODE -ne 0) { throw "apksigner sign failed with exit code $LASTEXITCODE" }
& $Apksigner verify --verbose $output
if ($LASTEXITCODE -ne 0) { throw "apksigner verify failed with exit code $LASTEXITCODE" }
Write-Host "Created $output"
