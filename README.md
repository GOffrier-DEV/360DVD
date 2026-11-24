# 360DVD

DVD player for modded Xbox 360 consoles (RGH/JTAG).

360DVD uses `libdvdread` for disc access, `libdvdnav` for navigation, FFmpeg
for decoding, and XAudio2 for audio output. CD playback is out of scope.

## Current status

The player now opens physical DVDs at First Play, follows libdvdnav menus,
and plays DVD video, audio, and subtitles with the native Blades overlay.

## DVD navigation notes

- [Main-disc country selection: France vs. United Kingdom](docs/dvd-country-selection.md)

## Video API baseline

`docs/video-api-surface.json` records the required declared Xbox 360 API
surface. It is a declaration checklist, not a certification that each API is
available or works on target hardware. Verify individual calls against the
selected RXDK/XDK headers and on-console tests before relying on them.

Desktop-only D3D9 calls listed in `desktop_assumptions_absent` must not be
assumed available. The memexport APIs are reserved for a later experiment.

## Build

The initial project targets the Xbox 360 XDK/MSBuild project format used by
the H.264 reference project. With the Xbox 360 SDK and Visual Studio 2010
XDK tools installed, run:

```powershell
.\build.ps1
```

The build has two configurations. Both use AltiVec and embed the Blades
scenes, icons, font, and DVD logo in the XEX.

- `.\build.ps1` → `build\Release\360DVD.xex`, without app logging or symbols.
- `.\build.ps1 -Configuration Debug` → `build\Debug\360DVD.xex`, with symbols
  and app logs.

Deploy only the XEX. The source resources are under [`assets/`](assets/);
the build compiles the XUI layouts and embeds them with the images and font.

### FFmpeg playback

Build the required FFmpeg archives in WSL first; see
[FFmpeg on RXDK](docs/ffmpeg-rxdk.md). Insert a physical DVD. Audio plays
through XAudio2.

In Debug, retrieve `game:\360DVD-pacing.csv` after playback ends. If
playback stalls, retrieve `game:\360DVD.log` and `game:\360DVD-nav.log`.
The trace includes XAudio2's sample clock and queue depth, expired video
frames, and separate video codec, demux, and `ReadFile` measurements.
Demux time includes `ReadFile` time; don't add them together.
Compare `decode_cpu_ms` (decode-thread CPU time) with `decode_frame_ms`
(elapsed time for the same work) across multiple frames. CPU accounting
may be coarse; `-1` indicates an unavailable reading. Rows without a new
frame report zero.

## Licensing and dependencies

The current VideoLAN `master` branches are tracked as Git submodules under
`deps/`. The superproject pins exact revisions; initialize them after cloning:

```powershell
git submodule update --init --recursive
```

See `docs/dependencies.md` for pinned revisions, licenses, and the porting
boundary. Keep third-party sources and build outputs separate from application
code.

The rendering test uses the DVD-Video logo artwork from [Wikimedia
Commons](https://commons.wikimedia.org/wiki/File:DVD-Video_Logo.svg), sourced
to DVD Format / Logo Licensing Company. The logo may be subject to trademark
restrictions. See `assets/ATTRIBUTION.md`.
