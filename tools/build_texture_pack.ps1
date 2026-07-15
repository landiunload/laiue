[CmdletBinding(DefaultParameterSetName = 'Directory')]
param(
    [Parameter(Mandatory, ParameterSetName = 'Directory')]
    [string]$Directory,

    # LTP2: дополнительно упаковать карты нормалей <имя>_n.png
    # (RGB — нормаль, A — ambient occlusion).
    [Parameter(ParameterSetName = 'Directory')]
    [switch]$IncludeNormals,

    [Parameter(Mandatory, ParameterSetName = 'PatrixZip')]
    [string]$PatrixZip,

    [Parameter(ParameterSetName = 'PatrixZip')]
    [ValidateRange(1, 36)]
    [int]$Tile = 1,

    [Parameter(ParameterSetName = 'PatrixZip')]
    [ValidatePattern('^#[0-9A-Fa-f]{6}$')]
    [string]$GrassTint = '#75AD55',

    [Parameter(Mandatory)]
    [string]$Output
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Compression.FileSystem

$LtpMagic = [uint32]0x3150544C
$LtpVersion = [uint16]1
$LtpVersionNormals = [uint16]2
$LtpHeaderSize = [uint16]24
$LtpFormatRgba8 = [uint32]1
$LtpFormatRgba8Normals = [uint32]2
$LayerCount = 3
$MaximumDimension = 4096

function Convert-BitmapToRgba {
    param([Parameter(Mandatory)][System.Drawing.Bitmap]$Bitmap)

    $pixels = [byte[]]::new($Bitmap.Width * $Bitmap.Height * 4)
    for ($y = 0; $y -lt $Bitmap.Height; ++$y) {
        for ($x = 0; $x -lt $Bitmap.Width; ++$x) {
            $color = $Bitmap.GetPixel($x, $y)
            $offset = ($y * $Bitmap.Width + $x) * 4
            $pixels[$offset + 0] = $color.R
            $pixels[$offset + 1] = $color.G
            $pixels[$offset + 2] = $color.B
            $pixels[$offset + 3] = $color.A
        }
    }

    [pscustomobject]@{
        Width = [int]$Bitmap.Width
        Height = [int]$Bitmap.Height
        Pixels = $pixels
    }
}

function Read-PngFile {
    param([Parameter(Mandatory)][string]$Path)

    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $bitmap = [System.Drawing.Bitmap]::new($resolved)
    try {
        Convert-BitmapToRgba -Bitmap $bitmap
    }
    finally {
        $bitmap.Dispose()
    }
}

function Read-ZipPng {
    param(
        [Parameter(Mandatory)]$Zip,
        [Parameter(Mandatory)][string]$EntryPath
    )

    $entry = $Zip.GetEntry($EntryPath)
    if ($null -eq $entry) {
        throw "В архиве отсутствует $EntryPath"
    }

    $stream = $entry.Open()
    try {
        $source = [System.Drawing.Image]::FromStream($stream)
        try {
            $bitmap = [System.Drawing.Bitmap]::new($source)
            try {
                Convert-BitmapToRgba -Bitmap $bitmap
            }
            finally {
                $bitmap.Dispose()
            }
        }
        finally {
            $source.Dispose()
        }
    }
    finally {
        $stream.Dispose()
    }
}

function Convert-HexColor {
    param([Parameter(Mandatory)][string]$Color)

    [pscustomobject]@{
        R = [Convert]::ToInt32($Color.Substring(1, 2), 16)
        G = [Convert]::ToInt32($Color.Substring(3, 2), 16)
        B = [Convert]::ToInt32($Color.Substring(5, 2), 16)
    }
}

function New-TintedImage {
    param(
        [Parameter(Mandatory)]$Image,
        [Parameter(Mandatory)]$Tint,
        [switch]$ForceOpaque
    )

    $pixels = [byte[]]::new($Image.Pixels.Length)
    for ($offset = 0; $offset -lt $pixels.Length; $offset += 4) {
        $pixels[$offset + 0] = [byte][math]::Floor(($Image.Pixels[$offset + 0] * $Tint.R + 127) / 255)
        $pixels[$offset + 1] = [byte][math]::Floor(($Image.Pixels[$offset + 1] * $Tint.G + 127) / 255)
        $pixels[$offset + 2] = [byte][math]::Floor(($Image.Pixels[$offset + 2] * $Tint.B + 127) / 255)
        $pixels[$offset + 3] = if ($ForceOpaque) { 255 } else { $Image.Pixels[$offset + 3] }
    }

    [pscustomobject]@{ Width = $Image.Width; Height = $Image.Height; Pixels = $pixels }
}

function Merge-GrassSide {
    param(
        [Parameter(Mandatory)]$Base,
        [Parameter(Mandatory)]$Overlay,
        [Parameter(Mandatory)]$Tint
    )

    if ($Base.Width -ne $Overlay.Width -or $Base.Height -ne $Overlay.Height) {
        throw 'Размеры grass side base и overlay не совпадают'
    }

    $tintedOverlay = New-TintedImage -Image $Overlay -Tint $Tint
    $pixels = [byte[]]::new($Base.Pixels.Length)
    for ($offset = 0; $offset -lt $pixels.Length; $offset += 4) {
        $alpha = [int]$tintedOverlay.Pixels[$offset + 3]
        $inverseAlpha = 255 - $alpha
        for ($channel = 0; $channel -lt 3; ++$channel) {
            $mixed = $tintedOverlay.Pixels[$offset + $channel] * $alpha +
                $Base.Pixels[$offset + $channel] * $inverseAlpha
            $pixels[$offset + $channel] = [byte][math]::Floor(($mixed + 127) / 255)
        }
        $pixels[$offset + 3] = 255
    }

    [pscustomobject]@{ Width = $Base.Width; Height = $Base.Height; Pixels = $pixels }
}

function Test-PowerOfTwo {
    param([int]$Value)
    $Value -gt 0 -and (($Value -band ($Value - 1)) -eq 0)
}

function Assert-CompatibleLayers {
    param([Parameter(Mandatory)][object[]]$Layers)

    if ($Layers.Count -ne $LayerCount) {
        throw "LTP1 требует ровно $LayerCount слоя"
    }

    $width = [int]$Layers[0].Width
    $height = [int]$Layers[0].Height
    if ($width -ne $height -or !(Test-PowerOfTwo $width) -or
        $width -gt $MaximumDimension) {
        throw "Текстуры должны быть квадратными power-of-two, от 1 до $MaximumDimension пикселей"
    }

    foreach ($layer in $Layers) {
        if ($layer.Width -ne $width -or $layer.Height -ne $height) {
            throw 'Все слои texture pack должны иметь одинаковый размер'
        }
        if ($layer.Pixels.Length -ne $width * $height * 4) {
            throw 'Некорректный размер RGBA8-слоя'
        }
    }
}

function New-NextMip {
    param([Parameter(Mandatory)]$Source)

    $width = [math]::Max(1, [int]($Source.Width / 2))
    $height = [math]::Max(1, [int]($Source.Height / 2))
    $pixels = [byte[]]::new($width * $height * 4)

    for ($y = 0; $y -lt $height; ++$y) {
        $sourceY0 = [math]::Min($Source.Height - 1, $y * 2)
        $sourceY1 = [math]::Min($Source.Height - 1, $sourceY0 + 1)
        for ($x = 0; $x -lt $width; ++$x) {
            $sourceX0 = [math]::Min($Source.Width - 1, $x * 2)
            $sourceX1 = [math]::Min($Source.Width - 1, $sourceX0 + 1)
            $destination = ($y * $width + $x) * 4
            $indices = @(
                ($sourceY0 * $Source.Width + $sourceX0) * 4
                ($sourceY0 * $Source.Width + $sourceX1) * 4
                ($sourceY1 * $Source.Width + $sourceX0) * 4
                ($sourceY1 * $Source.Width + $sourceX1) * 4
            )
            for ($channel = 0; $channel -lt 4; ++$channel) {
                $sum = 0
                foreach ($index in $indices) {
                    $sum += $Source.Pixels[$index + $channel]
                }
                $pixels[$destination + $channel] = [byte][math]::Floor(($sum + 2) / 4)
            }
        }
    }

    [pscustomobject]@{ Width = $width; Height = $height; Pixels = $pixels }
}

function New-MipChain {
    param([Parameter(Mandatory)]$Base)

    $current = $Base
    while ($true) {
        $current
        if ($current.Width -eq 1 -and $current.Height -eq 1) {
            break
        }
        $current = New-NextMip -Source $current
    }
}

# Мип-уровень карты нормалей: после усреднения RGB перенормируется
# (иначе дальние мипы «выцветают» к плоской нормали неравномерно).
function Restore-NormalLength {
    param([Parameter(Mandatory)]$Image)

    for ($offset = 0; $offset -lt $Image.Pixels.Length; $offset += 4) {
        $nx = $Image.Pixels[$offset + 0] / 127.5 - 1.0
        $ny = $Image.Pixels[$offset + 1] / 127.5 - 1.0
        $nz = $Image.Pixels[$offset + 2] / 127.5 - 1.0
        $length = [math]::Sqrt($nx * $nx + $ny * $ny + $nz * $nz)
        if ($length -lt 1e-5) { $nx = 0.0; $ny = 0.0; $nz = 1.0; $length = 1.0 }
        $Image.Pixels[$offset + 0] = [byte][math]::Round(($nx / $length * 0.5 + 0.5) * 255)
        $Image.Pixels[$offset + 1] = [byte][math]::Round(($ny / $length * 0.5 + 0.5) * 255)
        $Image.Pixels[$offset + 2] = [byte][math]::Round(($nz / $length * 0.5 + 0.5) * 255)
    }
    $Image
}

function New-NormalMipChain {
    param([Parameter(Mandatory)]$Base)

    $current = $Base
    while ($true) {
        $current
        if ($current.Width -eq 1 -and $current.Height -eq 1) {
            break
        }
        $current = Restore-NormalLength -Image (New-NextMip -Source $current)
    }
}

$normalLayers = $null
if ($PSCmdlet.ParameterSetName -eq 'Directory') {
    $sourceDirectory = (Resolve-Path -LiteralPath $Directory).Path
    # Stable LTP layer order: dirt, grass top, grass side.
    $layers = @(
        Read-PngFile -Path (Join-Path $sourceDirectory 'dirt.png')
        Read-PngFile -Path (Join-Path $sourceDirectory 'grass_top.png')
        Read-PngFile -Path (Join-Path $sourceDirectory 'grass_side.png')
    )
    if ($IncludeNormals) {
        $normalLayers = @(
            Read-PngFile -Path (Join-Path $sourceDirectory 'dirt_n.png')
            Read-PngFile -Path (Join-Path $sourceDirectory 'grass_top_n.png')
            Read-PngFile -Path (Join-Path $sourceDirectory 'grass_side_n.png')
        )
    }
}
else {
    $zipPath = (Resolve-Path -LiteralPath $PatrixZip).Path
    $zip = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
    try {
        $root = 'assets/minecraft/optifine/ctm/patrix'
        $dirt = Read-ZipPng -Zip $zip -EntryPath "$root/dirt/default/$Tile.png"
        $grassTopSource = Read-ZipPng -Zip $zip -EntryPath "$root/grass/block/top/$Tile.png"
        $grassSideBase = Read-ZipPng -Zip $zip -EntryPath "$root/grass/block/side_default/$Tile.png"
        $grassSideOverlay = Read-ZipPng -Zip $zip -EntryPath "$root/grass/block/side_overlay/$Tile.png"
        $tint = Convert-HexColor -Color $GrassTint
        $grassTop = New-TintedImage -Image $grassTopSource -Tint $tint -ForceOpaque
        $grassSide = Merge-GrassSide -Base $grassSideBase -Overlay $grassSideOverlay -Tint $tint
        $layers = @($dirt, $grassTop, $grassSide)
    }
    finally {
        $zip.Dispose()
    }
}

Assert-CompatibleLayers -Layers $layers
if ($null -ne $normalLayers) {
    Assert-CompatibleLayers -Layers $normalLayers
    if ($normalLayers[0].Width -ne $layers[0].Width) {
        throw 'Размер карт нормалей должен совпадать с albedo'
    }
}

$chains = [object[]]::new($LayerCount)
for ($layer = 0; $layer -lt $LayerCount; ++$layer) {
    $chains[$layer] = @(New-MipChain -Base $layers[$layer])
}

$normalChains = $null
if ($null -ne $normalLayers) {
    $normalChains = [object[]]::new($LayerCount)
    for ($layer = 0; $layer -lt $LayerCount; ++$layer) {
        $normalChains[$layer] = @(New-NormalMipChain -Base $normalLayers[$layer])
    }
}

$mipCount = $chains[0].Count
[uint64]$dataBytes64 = 0
foreach ($chain in $chains) {
    if ($chain.Count -ne $mipCount) {
        throw 'Число mip-уровней слоёв не совпадает'
    }
    foreach ($mip in $chain) {
        $dataBytes64 += $mip.Pixels.Length
    }
}
if ($null -ne $normalChains) {
    # LTP2: payload нормалей идентичен по размеру и раскладке albedo.
    $dataBytes64 *= 2
}
if ($dataBytes64 -gt [uint32]::MaxValue) {
    throw 'Payload LTP превышает 4 GiB'
}

$outputPath = [System.IO.Path]::GetFullPath($Output)
$outputDirectory = [System.IO.Path]::GetDirectoryName($outputPath)
if (![string]::IsNullOrEmpty($outputDirectory)) {
    [System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null
}

$stream = [System.IO.File]::Open(
    $outputPath, [System.IO.FileMode]::Create,
    [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
$writer = [System.IO.BinaryWriter]::new($stream)
try {
    if ($null -ne $normalChains) {
        $writer.Write($LtpMagic)
        $writer.Write($LtpVersionNormals)
        $writer.Write($LtpHeaderSize)
        $writer.Write([uint16]$layers[0].Width)
        $writer.Write([uint16]$layers[0].Height)
        $writer.Write([uint16]$LayerCount)
        $writer.Write([uint16]$mipCount)
        $writer.Write($LtpFormatRgba8Normals)
        $writer.Write([uint32]$dataBytes64)
    }
    else {
        $writer.Write($LtpMagic)
        $writer.Write($LtpVersion)
        $writer.Write($LtpHeaderSize)
        $writer.Write([uint16]$layers[0].Width)
        $writer.Write([uint16]$layers[0].Height)
        $writer.Write([uint16]$LayerCount)
        $writer.Write([uint16]$mipCount)
        $writer.Write($LtpFormatRgba8)
        $writer.Write([uint32]$dataBytes64)
    }
    foreach ($chain in $chains) {
        foreach ($mip in $chain) {
            $writer.Write([byte[]]$mip.Pixels)
        }
    }
    if ($null -ne $normalChains) {
        foreach ($chain in $normalChains) {
            foreach ($mip in $chain) {
                $writer.Write([byte[]]$mip.Pixels)
            }
        }
    }
}
finally {
    $writer.Dispose()
}

$versionLabel = if ($null -ne $normalChains) { 'LTP2 (albedo+normal)' } else { 'LTP1' }
$fileBytes = (Get-Item -LiteralPath $outputPath).Length
Write-Output ("{0}: {1}x{2}, layers={3}, mips={4}, payload={5} B, file={6} B -> {7}" -f
    $versionLabel, $layers[0].Width, $layers[0].Height, $LayerCount, $mipCount,
    $dataBytes64, $fileBytes, $outputPath)
