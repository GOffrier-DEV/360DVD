param(
    [ValidateSet('Debug', 'Release')] [string]$Configuration = 'Release',
    [switch]$VideoPerfLogging,
    [string]$RxdkRoot = 'C:\Program Files\RXDK-360',
    [string]$MSBuild = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
)

$ErrorActionPreference = 'Stop'
$RxdkRoot = [IO.Path]::GetFullPath($RxdkRoot)
$clangRoot = Join-Path $RxdkRoot 'bin\clang'
$clang = Join-Path $clangRoot 'bin\clang.exe'
$ar = Join-Path $clangRoot 'bin\llvm-ar.exe'
$rxdkLib = Join-Path $RxdkRoot 'lib\xbox'
$output = Join-Path $PSScriptRoot "build\$Configuration"
$libraryProject = Join-Path $PSScriptRoot '360DVD-libs.vcxproj'
$dvdLibrary = Join-Path $PSScriptRoot 'build\libdvd360.a'
$runtime = Join-Path $PSScriptRoot "build\runtime"
$helperObjects = Join-Path $runtime 'helper-objects'
$helperLib = Join-Path $runtime 'xdk-compiler-helpers.lib'
$project = Join-Path $PSScriptRoot '360DVD.vcxproj'
$deviceSource = Join-Path $PSScriptRoot 'build\obj\libs\device_xbox360.c'
$dvdDebug = if ($Configuration -eq 'Debug') { 'true' } else { 'false' }
$ffmpegBuild = Join-Path $PSScriptRoot 'build\ffmpeg-rxdk-altivec'
foreach ($lib in 'avformat', 'avcodec', 'avfilter', 'swresample', 'avutil') {
    if (-not (Test-Path "$ffmpegBuild\lib$lib\lib$lib.a")) {
        throw 'Build FFmpeg from WSL first; see docs/ffmpeg-rxdk.md.'
    }
}
$ffmpegProperties = @("/p:FfmpegBuildDir=$ffmpegBuild",
    "/p:FfmpegSourceDir=$(Join-Path $PSScriptRoot 'deps\ffmpeg')")
$videoPerfProperty = if ($VideoPerfLogging) { '/p:VideoPerfLogging=true' } else { '/p:VideoPerfLogging=false' }

foreach ($tool in @($MSBuild, $clang, $ar)) {
    if (-not (Test-Path $tool)) { throw "Required build tool not found: $tool" }
}

$null = New-Item -ItemType Directory -Force $runtime, $helperObjects, $output
$kernelObject = Join-Path $runtime 'kernel_import.o'
$kernelArchive = Join-Path $runtime 'kernel_import.a'
Copy-Item (Join-Path $rxdkLib 'libc.a'), (Join-Path $rxdkLib 'libcpp.a') $runtime -Force
& $clang --target=powerpc64-unknown-xbox360 -c (Join-Path $rxdkLib 'kernel_import.s') -o $kernelObject
if ($LASTEXITCODE -ne 0) { throw 'Could not compile RXDK kernel imports for Clang.' }
if (Test-Path $kernelArchive) { Remove-Item $kernelArchive -Force }
& $ar rcs $kernelArchive $kernelObject
if ($LASTEXITCODE -ne 0) { throw 'Could not create the RXDK Clang kernel-import archive.' }
$null = New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot 'build\obj\libs')

# RXDK's XDK-header mode is required by the renderer, but is not suitable for
# compiling upstream C sources. Compile those as pure RXDK C, then archive them.
& $MSBuild $libraryProject /t:Clean /nologo /v:minimal `
    '/p:Configuration=Release' '/p:Platform=Xbox 360' `
    "/p:RxdkClangRoot=$clangRoot" "/p:RxdkInstallDir=$RxdkRoot" `
    "/p:RxdkLibcDir=$runtime"
if ($LASTEXITCODE -ne 0) { throw 'Could not clean RXDK DVD library objects.' }

# RXDK off_t is 32-bit. Widen only the byte offset passed to the stream
# callback; do not alter libc seek types or the pinned upstream checkout.
$deviceText = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'deps\libdvdcss\src\device.c'))
$oldSeek = 'off_t i_seek = (off_t) i_blocks * (off_t) DVDCSS_BLOCK_SIZE;'
$newSeek = 'uint64_t i_seek = (uint64_t) i_blocks * DVDCSS_BLOCK_SIZE;'
if ([regex]::Matches($deviceText, [regex]::Escape($oldSeek)).Count -ne 1) {
    throw 'Recheck the libdvdcss stream seek adaptation against the pinned source.'
}
$oldStreamIoctls = "int dvdcss_use_ioctls( dvdcss_t dvdcss )`n{`n    if( dvdcss->p_stream )`n        return 0;"
$newStreamIoctls = @'
extern int dvd360_css_stream_fd(void *, const dvdcss_stream_cb *);

int dvdcss_use_ioctls( dvdcss_t dvdcss )
{
    if( dvdcss->p_stream )
    {
        dvdcss->i_fd = dvd360_css_stream_fd(dvdcss->p_stream, dvdcss->p_stream_cb);
        return dvdcss->i_fd != -1;
    }
'@
$deviceText = $deviceText.Replace("`r`n", "`n")
if ([regex]::Matches($deviceText, [regex]::Escape($oldStreamIoctls)).Count -ne 1) {
    throw 'Recheck the libdvdcss stream ioctl adaptation against the pinned source.'
}
$null = New-Item -ItemType Directory -Force (Split-Path $deviceSource)
[IO.File]::WriteAllText($deviceSource,
    $deviceText.Replace($oldSeek, $newSeek).Replace($oldStreamIoctls, $newStreamIoctls))

& $MSBuild $libraryProject /t:ClCompile /nologo /v:minimal `
    '/p:Configuration=Release' '/p:Platform=Xbox 360' `
    "/p:RxdkClangDebug=$dvdDebug" "/p:DvdDiagnosticBuild=$dvdDebug" `
    "/p:RxdkClangRoot=$clangRoot" "/p:RxdkInstallDir=$RxdkRoot" `
    "/p:RxdkLibcDir=$runtime"
if ($LASTEXITCODE -ne 0) { throw 'Compiling the DVD libraries with RXDK Clang failed.' }
$libraryObjects = @(Get-ChildItem (Join-Path $PSScriptRoot 'build\obj\libs') -Filter '*.o' | ForEach-Object FullName)
if ($libraryObjects.Count -eq 0) { throw 'RXDK Clang produced no DVD library objects.' }
if (Test-Path $dvdLibrary) { Remove-Item $dvdLibrary -Force }
& $ar rcs $dvdLibrary @libraryObjects
if ($LASTEXITCODE -ne 0) { throw 'Could not archive the DVD library objects.' }

$helperMembers = @('crtfpr.o', 'crtgpr.o', 'crtvmx.o', 'chkstk.o', 'u64tod.o')
& $ar x "--output=$helperObjects" (Join-Path $rxdkLib 'libcMT.a') @helperMembers
if ($LASTEXITCODE -ne 0) { throw 'Could not extract RXDK Clang ABI helper objects.' }
if (Test-Path $helperLib) { Remove-Item $helperLib -Force }
$objects = $helperMembers | ForEach-Object { Join-Path $helperObjects $_ }
& $ar rcs $helperLib @objects
if ($LASTEXITCODE -ne 0) { throw 'Could not create RXDK Clang ABI helper archive.' }

$resourceDir = Join-Path $PSScriptRoot "build\embedded\$Configuration"
& (Join-Path $PSScriptRoot 'tools\embed-resources.ps1') -Directory $resourceDir `
    -Assets (Join-Path $PSScriptRoot 'assets') -RxdkRoot $RxdkRoot

& $MSBuild $project /m:1 /t:Rebuild /nologo /v:minimal `
    "/p:Configuration=$Configuration" '/p:Platform=Xbox 360' `
    "/p:RxdkClangRoot=$clangRoot" "/p:RxdkInstallDir=$RxdkRoot" `
    "/p:RxdkLibcDir=$runtime" "/p:RxdkHelperLib=$helperLib" `
    "/p:DvdLibraries=$dvdLibrary" `
    "/p:EmbeddedResourceDir=$resourceDir" `
    $videoPerfProperty `
    @ffmpegProperties `
    "/p:OutDir=$output\" "/p:IntDir=$(Join-Path $PSScriptRoot "build\obj\$Configuration")\"
if ($LASTEXITCODE -ne 0) {
    throw "360DVD build failed with exit code $LASTEXITCODE"
}
if ($Configuration -eq 'Release') {
    # RXDK's packer can emit a symbol sidecar even without compiler debug info.
    $symbolFile = Join-Path $output '360DVD.xdb'
    if (Test-Path -LiteralPath $symbolFile) { Remove-Item -LiteralPath $symbolFile -Force }
}

Write-Host "Built: $(Join-Path $output '360DVD.xex')"
