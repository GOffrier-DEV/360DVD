param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Output,
    [string]$RxdkRoot = 'C:\Program Files\RXDK-360'
)
$ErrorActionPreference = 'Stop'
$converter = Join-Path $RxdkRoot 'bin\win32\xui2bin.exe'
$outputDir = New-Item -ItemType Directory -Force -Path $Output
$dvd = Join-Path $Source 'dvd'
Copy-Item -LiteralPath (Join-Path $RxdkRoot 'redist\Fonts\XenonCLatin.ttf') -Destination $outputDir.FullName

# Compile the exported layouts with the SDK's built-in XuiScene. The
# dashboard-only classes must be replaced by title-side playback handlers.
foreach ($name in 'dvd_skin','DvdPadPlayback','DvdPadDisplay') {
    $sourceFile = if ($name -eq 'dvd_skin') {
        Join-Path $Source "$name.xui"
    } else {
        Join-Path $dvd "$name.xui"
    }
    $layout = New-Object System.Xml.XmlDocument
    $layout.PreserveWhitespace = $true
    $layout.Load((Resolve-Path -LiteralPath $sourceFile).Path)
    if ($name -eq 'dvd_skin') {
        $points = $layout.SelectSingleNode(
            '//XuiVisual[Properties/Id="osui_graphicTextBG"]/XuiFigure[Properties/Id="gr"]/Properties/Points')
        if (!$points) { throw 'Stock DVD action strip geometry was not found.' }
        # Replace only the outline. Retain the original fill, opacity and bounds.
        $radius = 12.0
        $culture = [Globalization.CultureInfo]::InvariantCulture
        $vertices = @(
            foreach ($corner in 0..3) {
                $cx = if ($corner -lt 2) { 420.0 - $radius } else { $radius }
                $cy = if ($corner -eq 1 -or $corner -eq 2) { 46.941750 - $radius } else { $radius }
                foreach ($step in 0..8) {
                    $angle = ($corner * 90 - 90 + $step * 11.25) * [Math]::PI / 180
                    $x = ($cx + $radius * [Math]::Cos($angle)).ToString('F6', $culture)
                    $y = ($cy + $radius * [Math]::Sin($angle)).ToString('F6', $culture)
                    "$x,$y,$x,$y,$x,$y,0"
                }
            }
        )
        $points.InnerText = "$($vertices.Count),$($vertices -join ','),"
    }
    foreach ($node in $layout.SelectNodes('/XuiCanvas/XuiScene/Properties/ClassOverride')) {
        if ($node.InnerText -in 'DvdPadPlayback','DvdPadDisplay') {
            [void]$node.ParentNode.RemoveChild($node)
        }
    }
    # Dashboard sound objects are not registered in a standalone title.
    # Drop only those objects and their tracks; keep the visual timelines.
    foreach ($sound in $layout.SelectNodes('//XuiSoundXAudio')) {
        $id = $sound.SelectSingleNode('Properties/Id').InnerText
        foreach ($track in $sound.ParentNode.SelectNodes('Timelines/Timeline')) {
            if ($track.SelectSingleNode('Id').InnerText -eq $id) {
                [void]$track.ParentNode.RemoveChild($track)
            }
        }
        [void]$sound.ParentNode.RemoveChild($sound)
    }
    # Resolve the dashboard's package-relative icons in our loose-file layout.
    foreach ($image in $layout.SelectNodes('//ImagePath')) {
        if ($image.InnerText -match '^[\w.-]+\.png$' -and
            (Test-Path -LiteralPath (Join-Path $dvd $image.InnerText) -PathType Leaf)) {
            $image.InnerText = 'dvd360:///' + $image.InnerText
        }
    }
    $xmlPath = Join-Path $outputDir.FullName "$name.xui"
    $binaryPath = Join-Path $outputDir.FullName "$name.xur"
    $layout.Save($xmlPath)
    & $converter $xmlPath $binaryPath
    if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
        throw "Failed to compile $xmlPath"
    }
    if ($name -eq 'DvdPadDisplay') {
        # Reuse the stock rounded strip and its Xenon label in a separate
        # scene so the notification can outlive the control bar.
        $scene = $layout.SelectSingleNode('/XuiCanvas/XuiScene')
        $background = $scene.SelectSingleNode('XuiLabel[Properties/Id="titleBackground"]').CloneNode($true)
        $label = $scene.SelectSingleNode('XuiLabel[Properties/Id="txt_Action"]').CloneNode($true)
        foreach ($child in @($scene.ChildNodes)) {
            if ($child.Name -ne 'Properties') { [void]$scene.RemoveChild($child) }
        }
        [void]$scene.Properties.RemoveChild($scene.Properties.SelectSingleNode('DefaultFocus'))
        $background.Properties.Position = '173.500000,303.000000,0.000000'
        $label.Properties.Position = '183.500000,305.000000,0.000000'
        $label.Properties.Text = ' '
        [void]$scene.AppendChild($background)
        [void]$scene.AppendChild($label)
        $xmlPath = Join-Path $outputDir.FullName 'DvdTrackPopup.xui'
        $binaryPath = Join-Path $outputDir.FullName 'DvdTrackPopup.xur'
        $layout.Save($xmlPath)
        & $converter $xmlPath $binaryPath
        if ($LASTEXITCODE -ne 0 -or !(Test-Path -LiteralPath $binaryPath -PathType Leaf)) {
            throw "Failed to compile $xmlPath"
        }
    }
}

# Keep relative image references from both scenes intact.
Get-ChildItem -LiteralPath $dvd -Filter '*.png' -File |
    Copy-Item -Destination $outputDir.FullName
