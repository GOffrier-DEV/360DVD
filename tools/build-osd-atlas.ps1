param(
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$FontFile,
    [string]$SymbolFont
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

# 16 x 8 cells of 32 pixels. The XEX consumes the alpha plane directly.
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
$badge = New-Object Drawing.Font 'Segoe UI',17,([Drawing.FontStyle]::Bold),([Drawing.GraphicsUnit]::Pixel)
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
    Write-Host "Controller glyphs: $($symbols.Name)"
}
$format = [Drawing.StringFormat]::GenericTypographic
$format.FormatFlags = $format.FormatFlags -bor [Drawing.StringFormatFlags]::MeasureTrailingSpaces
$widths = New-Object byte[] 128
for ($i = 32; $i -lt 127; $i++) {
    $x = ($i % 16) * 32
    $y = [Math]::Floor($i / 16) * 32
    $text = [string][char]$i
    $widths[$i] = [Math]::Ceiling($graphics.MeasureString($text,$font,512,$format).Width)
    $graphics.DrawString($text,$font,$white,[single]($x+2),[single]($y+1),$format)
}

# Icons in otherwise unused control-code cells.
$controllerGlyphs = @{
    5 = 0xF041 # A
    6 = 0xF042 # B
    7 = 0xF03E # Start, with original label
    8 = 0xF039 # Four-way navigation
}
for ($i = 1; $i -le 8; $i++) {
    $widths[$i] = 31
    $state = $graphics.Save()
    $graphics.TranslateTransform(32 * $i,0)
    if ($symbols -and $controllerGlyphs.ContainsKey($i)) {
        $text = [string][char]$controllerGlyphs[$i]
        $advance = $graphics.MeasureString($text,$symbols,512,$format).Width
        $graphics.DrawString($text,$symbols,$white,[single]((32-$advance)/2),[single]1,$format)
        $graphics.Restore($state)
        continue
    }
    $pen = New-Object Drawing.Pen ([Drawing.Color]::White),2
    switch ($i) {
        1 { $graphics.FillPolygon($white,[Drawing.Point[]]@(
            [Drawing.Point]::new(8,5),[Drawing.Point]::new(26,16),[Drawing.Point]::new(8,27))) }
        2 { $graphics.FillRectangle($white,8,6,5,20); $graphics.FillRectangle($white,19,6,5,20) }
        3 { $graphics.DrawRectangle($pen,4,6,24,17); $graphics.DrawLine($pen,11,27,21,27) }
        4 { $graphics.DrawRectangle($pen,3,6,26,19); $graphics.DrawLine($pen,8,15,24,15)
            $graphics.DrawLine($pen,8,20,20,20) }
        5 { $graphics.DrawEllipse($pen,4,4,24,24); $graphics.DrawString('A',$badge,$white,10,5,$format) }
        6 { $graphics.DrawEllipse($pen,4,4,24,24); $graphics.DrawString('B',$badge,$white,10,5,$format) }
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
[IO.File]::WriteAllBytes($Output,$bytes)
$graphics.Dispose()
$bitmap.Dispose()
$font.Dispose()
$badge.Dispose()
$format.Dispose()
if ($privateFonts) { $privateFonts.Dispose() }
if ($symbols) { $symbols.Dispose() }
if ($symbolFonts) { $symbolFonts.Dispose() }
