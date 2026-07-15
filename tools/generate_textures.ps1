# Генерирует собственный набор текстур laiue: бесшовные пиксель-арт
# 32x32 (земля, верх травы, бок травы) плюс карты нормалей из карт высот
# (альфа нормали = ambient occlusion). Никаких внешних ассетов — всё
# процедурно и детерминировано по -Seed.
#
#   pwsh -File tools/generate_textures.ps1 -Output .\assets\texturepacks_src\laiue32
#
# Далее пак собирается build_texture_pack.ps1 с ключом -IncludeNormals.

[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [string]$Output,

    [ValidateSet(16, 32, 64)]
    [int]$Size = 32,

    [uint32]$Seed = 20260715
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# --- Детерминированный генератор случайных чисел ---

$script:Rng = [System.Random]::new([int]($Seed -band 0x7FFFFFFF))

function Get-Random01 {
    $script:Rng.NextDouble()
}

function Limit01 {
    param([double]$Value)
    [math]::Min(1.0, [math]::Max(0.0, $Value))
}

# --- Бесшовный value noise: периодическая решётка + smoothstep ---

function New-PeriodicNoise {
    param([int]$Pixels, [int]$Cells)

    $grid = [double[]]::new($Cells * $Cells)
    for ($i = 0; $i -lt $grid.Length; ++$i) { $grid[$i] = Get-Random01 }

    $map = [double[]]::new($Pixels * $Pixels)
    for ($y = 0; $y -lt $Pixels; ++$y) {
        $gy = $y / [double]$Pixels * $Cells
        $y0 = [int][math]::Floor($gy); $fy = $gy - $y0
        $sy = $fy * $fy * (3 - 2 * $fy)
        $y1 = ($y0 + 1) % $Cells; $y0 = $y0 % $Cells
        for ($x = 0; $x -lt $Pixels; ++$x) {
            $gx = $x / [double]$Pixels * $Cells
            $x0 = [int][math]::Floor($gx); $fx = $gx - $x0
            $sx = $fx * $fx * (3 - 2 * $fx)
            $x1 = ($x0 + 1) % $Cells; $x0 = $x0 % $Cells
            $a = $grid[$y0 * $Cells + $x0]; $b = $grid[$y0 * $Cells + $x1]
            $c = $grid[$y1 * $Cells + $x0]; $d = $grid[$y1 * $Cells + $x1]
            $top = $a + ($b - $a) * $sx
            $bottom = $c + ($d - $c) * $sx
            $map[$y * $Pixels + $x] = $top + ($bottom - $top) * $sy
        }
    }
    , $map
}

function Add-Octaves {
    param([int]$Pixels, [double[]]$Weights, [int[]]$Cells)

    $sum = [double[]]::new($Pixels * $Pixels)
    for ($octave = 0; $octave -lt $Weights.Length; ++$octave) {
        $noise = New-PeriodicNoise -Pixels $Pixels -Cells $Cells[$octave]
        for ($i = 0; $i -lt $sum.Length; ++$i) {
            $sum[$i] += $noise[$i] * $Weights[$octave]
        }
    }
    , $sum
}

# --- Утилиты изображений ---

function New-RgbaImage {
    param([int]$Pixels)
    [pscustomobject]@{
        Size = $Pixels
        Pixels = [byte[]]::new($Pixels * $Pixels * 4)
    }
}

function Set-ImagePixel {
    param($Image, [int]$X, [int]$Y, [int[]]$Rgb)
    $offset = ($Y * $Image.Size + $X) * 4
    $Image.Pixels[$offset + 0] = [byte]$Rgb[0]
    $Image.Pixels[$offset + 1] = [byte]$Rgb[1]
    $Image.Pixels[$offset + 2] = [byte]$Rgb[2]
    $Image.Pixels[$offset + 3] = 255
}

function Save-Png {
    param($Image, [string]$Path)

    $bitmap = [System.Drawing.Bitmap]::new($Image.Size, $Image.Size)
    try {
        for ($y = 0; $y -lt $Image.Size; ++$y) {
            for ($x = 0; $x -lt $Image.Size; ++$x) {
                $offset = ($y * $Image.Size + $x) * 4
                $color = [System.Drawing.Color]::FromArgb(
                    $Image.Pixels[$offset + 3], $Image.Pixels[$offset + 0],
                    $Image.Pixels[$offset + 1], $Image.Pixels[$offset + 2])
                $bitmap.SetPixel($x, $y, $color)
            }
        }
        $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $bitmap.Dispose()
    }
    Write-Output "  $Path"
}

# Нормаль из карты высот (оси wrap — бесшовность сохраняется).
# Конвенция: R = +U (вправо), G = вверх текстуры, B — из поверхности.
# Альфа — ambient occlusion от высоты.
function New-NormalImage {
    param([int]$Pixels, [double[]]$Height, [double]$Strength)

    $image = New-RgbaImage -Pixels $Pixels
    for ($y = 0; $y -lt $Pixels; ++$y) {
        $yPrev = ($y - 1 + $Pixels) % $Pixels
        $yNext = ($y + 1) % $Pixels
        for ($x = 0; $x -lt $Pixels; ++$x) {
            $xPrev = ($x - 1 + $Pixels) % $Pixels
            $xNext = ($x + 1) % $Pixels
            $dx = ($Height[$y * $Pixels + $xNext] - $Height[$y * $Pixels + $xPrev]) * $Strength
            $dyImage = ($Height[$yNext * $Pixels + $x] - $Height[$yPrev * $Pixels + $x]) * $Strength

            # Поверхность h(u, up): n ~ (-dh/du, -dh/dup, 1); up = -imageY.
            $nx = -$dx
            $ny = $dyImage
            $nz = 1.0
            $length = [math]::Sqrt($nx * $nx + $ny * $ny + $nz * $nz)
            $nx /= $length; $ny /= $length; $nz /= $length

            $h = $Height[$y * $Pixels + $x]
            $occlusion = Limit01 (0.62 + 0.38 * $h)

            $offset = ($y * $Pixels + $x) * 4
            $image.Pixels[$offset + 0] = [byte][math]::Round(($nx * 0.5 + 0.5) * 255)
            $image.Pixels[$offset + 1] = [byte][math]::Round(($ny * 0.5 + 0.5) * 255)
            $image.Pixels[$offset + 2] = [byte][math]::Round(($nz * 0.5 + 0.5) * 255)
            $image.Pixels[$offset + 3] = [byte][math]::Round($occlusion * 255)
        }
    }
    $image
}

function Get-PaletteColor {
    param([object[]]$Palette, [double]$Value)
    $index = [math]::Min($Palette.Count - 1,
        [math]::Max(0, [int][math]::Floor($Value * $Palette.Count)))
    $Palette[$index]
}

# --- Земля: коричневые комья + редкие камешки ---

$dirtPalette = @(
    @(86, 60, 42), @(104, 74, 50), @(121, 88, 60),
    @(137, 102, 70), @(150, 115, 80), @(162, 127, 90)
)
$stoneColors = @(@(134, 130, 124), @(152, 148, 141))

function New-DirtMaps {
    param([int]$Pixels)

    $base = Add-Octaves -Pixels $Pixels -Weights @(0.50, 0.32, 0.18) -Cells @(4, 8, 16)
    $stones = New-PeriodicNoise -Pixels $Pixels -Cells ([int]($Pixels / 4))

    $height = [double[]]::new($Pixels * $Pixels)
    $image = New-RgbaImage -Pixels $Pixels
    for ($y = 0; $y -lt $Pixels; ++$y) {
        for ($x = 0; $x -lt $Pixels; ++$x) {
            $i = $y * $Pixels + $x
            $value = $base[$i]
            $isStone = $stones[$i] -gt 0.86
            $height[$i] = Limit01 ($value + $(if ($isStone) { 0.30 } else { 0.0 }))

            if ($isStone) {
                $color = $stoneColors[[int]($stones[$i] * 977) % 2]
            }
            else {
                $color = Get-PaletteColor -Palette $dirtPalette -Value $value
            }
            Set-ImagePixel -Image $image -X $x -Y $y -Rgb $color
        }
    }
    [pscustomobject]@{ Albedo = $image; Height = $height }
}

# --- Верх травы: зелёный ковёр со штрихами-травинками ---

$grassPalette = @(
    @(70, 116, 44), @(82, 132, 51), @(94, 147, 58),
    @(106, 161, 66), @(117, 173, 74), @(129, 186, 83)
)

function New-GrassTopMaps {
    param([int]$Pixels)

    $base = Add-Octaves -Pixels $Pixels -Weights @(0.45, 0.35, 0.20) -Cells @(4, 8, 16)
    $blades = New-PeriodicNoise -Pixels $Pixels -Cells ([int]($Pixels / 2))

    $height = [double[]]::new($Pixels * $Pixels)
    $image = New-RgbaImage -Pixels $Pixels
    for ($y = 0; $y -lt $Pixels; ++$y) {
        for ($x = 0; $x -lt $Pixels; ++$x) {
            $i = $y * $Pixels + $x
            $value = $base[$i]
            $blade = $blades[$i] -gt 0.80
            if ($blade) { $value = [math]::Min(1.0, $value + 0.35) }
            $height[$i] = Limit01 $value
            $color = Get-PaletteColor -Palette $grassPalette -Value $value
            Set-ImagePixel -Image $image -X $x -Y $y -Rgb $color
        }
    }
    [pscustomobject]@{ Albedo = $image; Height = $height }
}

# --- Бок травы: земля + рваная зелёная кромка сверху ---

function New-GrassSideMaps {
    param([int]$Pixels, $Dirt)

    $edge = New-PeriodicNoise -Pixels $Pixels -Cells ([int]($Pixels / 4))
    $tone = New-PeriodicNoise -Pixels $Pixels -Cells ([int]($Pixels / 4))

    $height = [double[]]::new($Pixels * $Pixels)
    $image = New-RgbaImage -Pixels $Pixels
    $baseDepth = [math]::Max(2, [int]($Pixels / 8))

    for ($x = 0; $x -lt $Pixels; ++$x) {
        # Глубина кромки читается из первой строки шума: по x она
        # периодична, значит стык соседних блоков бесшовный.
        $depth = $baseDepth + [int]([math]::Round($edge[$x] * $baseDepth))
        for ($y = 0; $y -lt $Pixels; ++$y) {
            $i = $y * $Pixels + $x
            $isGrass = $y -lt $depth
            # Редкие свисающие «пряди» на пиксель ниже кромки.
            if (!$isGrass -and $y -eq $depth -and ($edge[$Pixels + $x] -gt 0.72)) {
                $isGrass = $true
            }

            if ($isGrass) {
                $value = Limit01 $tone[$i]
                $color = Get-PaletteColor -Palette $grassPalette -Value $value
                Set-ImagePixel -Image $image -X $x -Y $y -Rgb $color
                $height[$i] = Limit01 (0.55 + 0.45 * $value)
            }
            else {
                $offset = $i * 4
                $image.Pixels[$offset + 0] = $Dirt.Albedo.Pixels[$offset + 0]
                $image.Pixels[$offset + 1] = $Dirt.Albedo.Pixels[$offset + 1]
                $image.Pixels[$offset + 2] = $Dirt.Albedo.Pixels[$offset + 2]
                $image.Pixels[$offset + 3] = 255
                $height[$i] = $Dirt.Height[$i] * 0.9
            }
        }
    }
    [pscustomobject]@{ Albedo = $image; Height = $height }
}

# --- Генерация и сохранение ---

$outputDirectory = [System.IO.Path]::GetFullPath($Output)
[System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null

Write-Output "Генерация текстур ${Size}x${Size} (seed $Seed):"

$dirt = New-DirtMaps -Pixels $Size
$grassTop = New-GrassTopMaps -Pixels $Size
$grassSide = New-GrassSideMaps -Pixels $Size -Dirt $dirt

Save-Png -Image $dirt.Albedo -Path (Join-Path $outputDirectory 'dirt.png')
Save-Png -Image $grassTop.Albedo -Path (Join-Path $outputDirectory 'grass_top.png')
Save-Png -Image $grassSide.Albedo -Path (Join-Path $outputDirectory 'grass_side.png')

$dirtNormal = New-NormalImage -Pixels $Size -Height $dirt.Height -Strength 2.2
$grassTopNormal = New-NormalImage -Pixels $Size -Height $grassTop.Height -Strength 2.0
$grassSideNormal = New-NormalImage -Pixels $Size -Height $grassSide.Height -Strength 2.2

Save-Png -Image $dirtNormal -Path (Join-Path $outputDirectory 'dirt_n.png')
Save-Png -Image $grassTopNormal -Path (Join-Path $outputDirectory 'grass_top_n.png')
Save-Png -Image $grassSideNormal -Path (Join-Path $outputDirectory 'grass_side_n.png')

Write-Output 'Готово. Сборка пака: tools/build_texture_pack.ps1 -Directory <каталог> -IncludeNormals -Output <файл.ltp>'
