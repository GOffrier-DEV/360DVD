param([ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release')

$ErrorActionPreference = 'Stop'
$msbuild = "$env:WINDIR\Microsoft.NET\Framework\v4.0.30319\MSBuild.exe"
$project = Join-Path $PSScriptRoot '360DVD.vcxproj'

if (-not (Test-Path $msbuild)) {
    throw "MSBuild not found: $msbuild"
}

& $msbuild $project /nologo /v:minimal "/p:Configuration=$Configuration" '/p:Platform=Xbox 360'
if ($LASTEXITCODE -ne 0) {
    throw "360DVD build failed with exit code $LASTEXITCODE"
}

Write-Host "Built: build\$Configuration\360DVD.xex"
