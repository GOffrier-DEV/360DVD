param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [Parameter(Mandatory=$true)][string]$Assets,
    [Parameter(Mandatory=$true)][string]$RxdkRoot
)
$ErrorActionPreference = 'Stop'
$null = New-Item -ItemType Directory -Force -Path $Directory
$blades = Join-Path $Assets 'Blades'
$converter = Join-Path $RxdkRoot 'bin\win32\xui2bin.exe'
$files = @(
    foreach ($layout in Get-ChildItem -LiteralPath $blades -Filter '*.xui' -File | Sort-Object Name) {
        $compiled = Join-Path $Directory ($layout.BaseName + '.xur')
        & $converter $layout.FullName $compiled | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Failed to compile $($layout.FullName)" }
        Get-Item -LiteralPath $compiled
    }
    Get-ChildItem -LiteralPath $blades -File |
        Where-Object { $_.Extension -in '.png', '.ttf' } | Sort-Object Name
    Get-Item -LiteralPath (Join-Path $Assets 'dvd-video-logo.bmp')
)
$header = Join-Path $Directory 'DvdEmbeddedData.inc'
$writer = New-Object IO.StreamWriter($header, $false, [Text.Encoding]::ASCII)
try {
    for ($i = 0; $i -lt $files.Count; ++$i) {
        $bytes = [IO.File]::ReadAllBytes($files[$i].FullName)
        $writer.WriteLine("static const BYTE resource$i[] = {")
        for ($j = 0; $j -lt $bytes.Length; $j += 32) {
            $last = [Math]::Min($j + 31, $bytes.Length - 1)
            $writer.WriteLine(($bytes[$j..$last] -join ',') + ',')
        }
        $writer.WriteLine('};')
    }
    $writer.WriteLine('static const EmbeddedResource resources[] = {')
    for ($i = 0; $i -lt $files.Count; ++$i) {
        $writer.WriteLine(('    {{ L"{0}", resource{1}, sizeof(resource{1}) }},' -f $files[$i].Name, $i))
    }
    $writer.WriteLine('};')
} finally { $writer.Dispose() }
