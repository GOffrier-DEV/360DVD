param(
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$FontFile,
    [string]$DashboardAssets = (Join-Path $env:TEMP '360dash-9199'),
    [string]$DvdAssets = (Join-Path $env:TEMP 'opencode\dash-download-resources\dvd-unpacked'),
    [string]$SymbolFont
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# 16 x 8 cells of 32 pixels. The XEX consumes the L8 coverage plane.
$bitmap = New-Object Drawing.Bitmap 512,256
$graphics = [Drawing.Graphics]::FromImage($bitmap)
$graphics.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
$white = [Drawing.Brushes]::White
$privateFonts = $null
if ($FontFile) {
    $privateFonts = New-Object Drawing.Text.PrivateFontCollection
    $privateFonts.AddFontFile((Resolve-Path -LiteralPath $FontFile).Path)
    $font = [Drawing.Font]::new($privateFonts.Families[0],22,
        [Drawing.FontStyle]::Regular,[Drawing.GraphicsUnit]::Pixel)
    Write-Host "OSD typeface: $($font.Name)"
} else {
    $font = New-Object Drawing.Font 'Segoe UI',22,([Drawing.FontStyle]::Regular),([Drawing.GraphicsUnit]::Pixel)
}
$symbols = $null
$symbolFonts = $null
if (!$SymbolFont) {
    $sdkRoot = if ($env:RXDK360) { $env:RXDK360 } else { 'C:\Program Files\RXDK-360' }
    $SymbolFont = Join-Path $sdkRoot 'xboxfont\Convsym.ttf'
}
if (Test-Path -LiteralPath $SymbolFont -PathType Leaf) {
    $symbolFonts = New-Object Drawing.Text.PrivateFontCollection
    $symbolFonts.AddFontFile((Resolve-Path -LiteralPath $SymbolFont).Path)
    $symbols = [Drawing.Font]::new($symbolFonts.Families[0],26,
        [Drawing.FontStyle]::Regular,[Drawing.GraphicsUnit]::Pixel)
    Write-Host "Start/D-pad glyphs: $($symbols.Name)"
}
$format = [Drawing.StringFormat]::GenericTypographic
$format.FormatFlags = $format.FormatFlags -bor [Drawing.StringFormatFlags]::MeasureTrailingSpaces
$widths = New-Object byte[] 128
$colorSlots = @(5,6,3,4,1,2)
$buttonPixels = @{}
for ($i = 32; $i -lt 127; $i++) {
    $x = ($i % 16) * 32
    $y = [Math]::Floor($i / 16) * 32
    $text = [string][char]$i
    $widths[$i] = [Math]::Ceiling($graphics.MeasureString($text,$font,512,$format).Width)
    $graphics.DrawString($text,$font,$white,[single]($x+2),[single]($y+1),$format)
}

# Use the Blades shared button art and the extracted DVD transport art; use the
# XDK Convection symbols for Start and D-pad, absent from the dashboard package.
$buttonImages = @{
    5 = Join-Path $DashboardAssets 'A-Button.png'
    6 = Join-Path $DashboardAssets 'B-Button.png'
    1 = Join-Path $DvdAssets 'ico_32x_play_black.png'
    2 = Join-Path $DvdAssets 'ico_32x_pause_black.png'
    3 = Join-Path $DvdAssets 'ico_32x_aspect.png'
    4 = Join-Path $DvdAssets 'ico_32x_sub.png'
}
foreach ($i in 1..8) {
    $widths[$i] = 31
    $state = $graphics.Save()
    $graphics.TranslateTransform(32 * $i,0)
    if ($symbols -and ($i -eq 7 -or $i -eq 8)) {
        $codepoint = if ($i -eq 7) { 0xF03E } else { 0xF039 }
        $text = [string][char]$codepoint
        $advance = $graphics.MeasureString($text,$symbols,512,$format).Width
        $graphics.DrawString($text,$symbols,$white,[single]((32-$advance)/2),[single]1,$format)
        $graphics.Restore($state)
        continue
    }
    if ($colorSlots -contains $i) {
        if (!(Test-Path -LiteralPath $buttonImages[$i] -PathType Leaf)) {
            throw "Required dashboard artwork is missing: $($buttonImages[$i])"
        }
        $image = [Drawing.Bitmap]::new((Resolve-Path -LiteralPath $buttonImages[$i]).Path)
        if ($image.Width -ne 32 -or $image.Height -ne 32) {
            throw "Expected original 32x32 dashboard artwork: $($buttonImages[$i])"
        }
        $buttonPixels[$i] = $image
        $graphics.Restore($state)
        continue
    }
    $pen = New-Object Drawing.Pen ([Drawing.Color]::White),2
    switch ($i) {
        7 { $graphics.DrawEllipse($pen,4,4,24,24); $graphics.FillPolygon($white,[Drawing.Point[]]@(
            [Drawing.Point]::new(13,10),[Drawing.Point]::new(22,16),[Drawing.Point]::new(13,22))) }
        8 { $graphics.FillRectangle($white,12,4,8,24); $graphics.FillRectangle($white,4,12,24,8) }
    }
    $pen.Dispose()
    $graphics.Restore($state)
}
$bitmap.SetPixel(0,0,[Drawing.Color]::White)
$bytes = New-Object byte[] (128 + 512*256)
[Array]::Copy($widths,$bytes,128)
for ($y = 0; $y -lt 256; $y++) {
    for ($x = 0; $x -lt 512; $x++) {
        $bytes[128 + $y*512 + $x] = $bitmap.GetPixel($x,$y).A
    }
}
[IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($Output))) | Out-Null
# Keep the legacy L8 file layout. Store RGB in unused row-one and row-zero cells;
# the original button cells hold alpha. No resizing or tinting at build time.
for ($button = 0; $button -lt $colorSlots.Length; $button++) {
    $slot = $colorSlots[$button]
    $image = $buttonPixels[$slot]
    for ($y = 0; $y -lt 32; $y++) {
        for ($x = 0; $x -lt 32; $x++) {
            $pixel = $image.GetPixel($x,$y)
            $bytes[128 + $y*512 + $slot*32 + $x] = $pixel.A
            $rgb = if ($button -lt 4) {
                128 + ($y+32)*512 + $button*96 + $x
            } else {
                128 + $y*512 + (10+($button-4)*3)*32 + $x
            }
            $bytes[$rgb] = $pixel.R
            $bytes[$rgb+32] = $pixel.G
            $bytes[$rgb+64] = $pixel.B
        }
    }
    $image.Dispose()
}
[IO.File]::WriteAllBytes($Output,$bytes)
$graphics.Dispose()
$bitmap.Dispose()
$font.Dispose()
$format.Dispose()
if ($privateFonts) { $privateFonts.Dispose() }
if ($symbols) { $symbols.Dispose() }
if ($symbolFonts) { $symbolFonts.Dispose() }
