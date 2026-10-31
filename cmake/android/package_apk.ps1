# Собирает подписанный APK программы на laiue: манифест, нативные
# библиотеки (программа и модули движка), ресурсы. Библиотеки кладутся
# несжатыми и выровненными по 16 КиБ, чтобы система отображала их прямо из
# APK на устройствах с любым размером страницы.
param(
    [Parameter(Mandatory = $true)][string]$Aapt2,
    [Parameter(Mandatory = $true)][string]$Zipalign,
    [Parameter(Mandatory = $true)][string]$Apksigner,
    [Parameter(Mandatory = $true)][string]$FrameworkJar,
    [Parameter(Mandatory = $true)][string]$Manifest,
    # Пути к .so через «|».
    [Parameter(Mandatory = $true)][string]$Libraries,
    [Parameter(Mandatory = $true)][string]$Abi,
    # Имя библиотеки программы без lib и .so, если окно — отдельный модуль.
    [string]$MainLibrary = '',
    [string]$AssetsDirectory = '',
    # llvm-strip: в APK идут библиотеки без отладочных сведений и лишних
    # символов, а полные остаются в каталоге сборки для отладчика.
    [string]$Strip = '',
    [Parameter(Mandatory = $true)][string]$Keystore,
    [Parameter(Mandatory = $true)][string]$KeystorePassword,
    [Parameter(Mandatory = $true)][string]$Output,
    [Parameter(Mandatory = $true)][int]$MinSdk,
    [Parameter(Mandatory = $true)][int]$TargetSdk,
    [Parameter(Mandatory = $true)][string]$VersionName,
    [Parameter(Mandatory = $true)][int]$VersionCode
)

$ErrorActionPreference = 'Stop'
$libraryPaths = @($Libraries.Split('|') | Where-Object { $_ -ne '' })
foreach ($file in @($Aapt2, $Zipalign, $Apksigner, $FrameworkJar, $Manifest, $Keystore) + $libraryPaths) {
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "Android packaging input does not exist: $file"
    }
}

$directory = Split-Path -Parent $Output
New-Item -ItemType Directory -Force -Path $directory | Out-Null
$stem = [System.IO.Path]::GetFileNameWithoutExtension($Output)
$unsigned = Join-Path $directory "$stem-unsigned.apk"
$aligned = Join-Path $directory "$stem-aligned.apk"
Remove-Item -LiteralPath $unsigned, $aligned, $Output -Force -ErrorAction SilentlyContinue

& $Aapt2 link --manifest $Manifest -I $FrameworkJar --min-sdk-version $MinSdk `
    --target-sdk-version $TargetSdk --version-code $VersionCode --version-name $VersionName `
    --auto-add-overlay -o $unsigned
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed with exit code $LASTEXITCODE" }

# Windows PowerShell 5.1 держит ZipArchiveMode в базовой сборке сжатия, а
# PowerShell 7 может загрузить её косвенно; обе грузятся явно.
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function Add-Entry($archive, [string]$name, [string]$path, $level) {
    $entry = $archive.CreateEntry($name, $level)
    $input = [System.IO.File]::OpenRead($path)
    try {
        $stream = $entry.Open()
        try { $input.CopyTo($stream) } finally { $stream.Dispose() }
    } finally { $input.Dispose() }
}

$stripped = Join-Path $directory "$stem-stripped"
if ($Strip) {
    if (-not (Test-Path -LiteralPath $Strip -PathType Leaf)) { throw "llvm-strip does not exist: $Strip" }
    Remove-Item -LiteralPath $stripped -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force -Path $stripped | Out-Null
}
$archive = [System.IO.Compression.ZipFile]::Open($unsigned,
    [System.IO.Compression.ZipArchiveMode]::Update)
try {
    $seen = @{}
    foreach ($library in $libraryPaths) {
        $name = Split-Path -Leaf $library
        if ($seen.ContainsKey($name)) { throw "Two libraries share the name $name" }
        $seen[$name] = $true
        $packaged = $library
        if ($Strip) {
            # Таблица динамических символов остаётся: по ней модули и main
            # находятся при загрузке.
            $packaged = Join-Path $stripped $name
            & $Strip --strip-unneeded -o $packaged $library
            if ($LASTEXITCODE -ne 0) { throw "llvm-strip failed on $library with exit code $LASTEXITCODE" }
        }
        Add-Entry $archive "lib/$Abi/$name" $packaged ([System.IO.Compression.CompressionLevel]::NoCompression)
    }
    if ($MainLibrary) {
        $entry = $archive.CreateEntry('assets/laiue/main_library',
            [System.IO.Compression.CompressionLevel]::NoCompression)
        $writer = New-Object System.IO.StreamWriter($entry.Open(),
            (New-Object System.Text.UTF8Encoding($false)))
        try { $writer.Write($MainLibrary) } finally { $writer.Dispose() }
    }
    if ($AssetsDirectory) {
        if (-not (Test-Path -LiteralPath $AssetsDirectory -PathType Container)) {
            throw "Android asset directory does not exist: $AssetsDirectory"
        }
        $assetRoot = [System.IO.Path]::GetFullPath($AssetsDirectory).TrimEnd('\', '/')
        Get-ChildItem -LiteralPath $assetRoot -File -Recurse | ForEach-Object {
            $relative = $_.FullName.Substring($assetRoot.Length).TrimStart('\', '/')
            Add-Entry $archive ('assets/' + $relative.Replace('\', '/')) $_.FullName `
                ([System.IO.Compression.CompressionLevel]::Optimal)
        }
    }
} finally { $archive.Dispose() }
if ($Strip) { Remove-Item -LiteralPath $stripped -Recurse -Force }

& $Zipalign -f -P 16 4 $unsigned $aligned
if ($LASTEXITCODE -ne 0) { throw "zipalign failed with exit code $LASTEXITCODE" }
Remove-Item -LiteralPath $unsigned -Force
# apksigner грузит свою библиотеку Conscrypt: доступ к нативному коду
# разрешается явно через канал параметров JVM, а не прячется. Разборщик .bat
# в Windows SDK делит --option=value, поэтому параметры идут через
# JDK_JAVA_OPTIONS и восстанавливаются после двух дочерних процессов.
$previousJavaOptions = [Environment]::GetEnvironmentVariable('JDK_JAVA_OPTIONS', 'Process')
try {
    $env:JDK_JAVA_OPTIONS = "$previousJavaOptions --enable-native-access=ALL-UNNAMED".Trim()
    & $Apksigner sign --ks $Keystore --ks-pass "pass:$KeystorePassword" --out $Output $aligned
    if ($LASTEXITCODE -ne 0) { throw "apksigner sign failed with exit code $LASTEXITCODE" }
    & $Apksigner verify $Output
    if ($LASTEXITCODE -ne 0) { throw "apksigner verify failed with exit code $LASTEXITCODE" }
} finally {
    [Environment]::SetEnvironmentVariable('JDK_JAVA_OPTIONS', $previousJavaOptions, 'Process')
}
Remove-Item -LiteralPath $aligned -Force
Write-Host "Created $Output"
