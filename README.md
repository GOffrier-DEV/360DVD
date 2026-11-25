# 360DVD 1.0

DVD player for modded Xbox 360 consoles (RGH/JTAG).

360DVD uses `libdvdread` for disc access, `libdvdnav` for navigation, FFmpeg
for decoding, and XAudio2 for audio output. CD playback is out of scope.

## Current status

The player opens physical DVDs at First Play, follows libdvdnav menus, and
plays DVD video, audio, and subtitles with the native Blades overlay.

- Menu navigation with the D-pad, **A** to activate a button, and the Blades
  Root/Title/Continue actions.
- MPEG-2 video decoded by FFmpeg with audio-mastered pacing, buffered input,
  and two-way slice decoding by default.
- Dolby Digital (AC-3), MPEG Layer II (MP2), DVD-Video LPCM, and DTS audio
  through XAudio2, with live audio-track switching.
- DVD subtitle streams, six display modes, and a stock-style zoom toggle.
- The Blades scenes, icons, font, and DVD-Video logo are embedded in the XEX,
  so only the XEX needs to be deployed.

Scan/seek, resume bookmarks, and CD playback are not implemented. Xenon IDCT
is used for video; motion compensation remains scalar. See
[CHANGELOG.md](CHANGELOG.md) for the 1.0 release notes.

## Documentation

- [FFmpeg on RXDK](docs/ffmpeg-rxdk.md)
- [DVD dependencies and porting boundary](docs/dependencies.md)
- [DVD device I/O findings](docs/xbox-dvd-io.md)
- [DVD logo animation](docs/dvd-logo.md)
- [Main-disc country selection: France vs. United Kingdom](docs/dvd-country-selection.md)

## Video API baseline

`docs/video-api-surface.json` records the required declared Xbox 360 API
surface. Desktop-only D3D9 calls listed in `desktop_assumptions_absent` are
not part of the target API surface.

## Build

The project targets the Xbox 360 XDK/MSBuild project format used by the
H.264 reference project. Building requires RXDK 360 (Clang, headers, and
import libraries), Visual Studio 2022 with the XDK MSBuild integration, and
the FFmpeg archives described in [FFmpeg on RXDK](docs/ffmpeg-rxdk.md).
Build FFmpeg in WSL first, then run from PowerShell:

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

### DVD playback

Build the required FFmpeg archives in WSL first; see
[FFmpeg on RXDK](docs/ffmpeg-rxdk.md). Insert a physical DVD. Video plays
with the Blades overlay and audio plays through XAudio2.

Debug builds write `game:\360DVD.log`, `game:\360DVD-nav.log`, and
`game:\360DVD-pacing.csv` to the console for diagnostics.

## Licensing and dependencies

360DVD is released under the GNU General Public License, version 2 or later
(`GPL-2.0-or-later`). See [`LICENSE`](LICENSE).

The DVD libraries are Git submodules under `deps/`. `libdvdread` and
`libdvdnav` follow the Xbox port branches in this repository
(`libdvdread-xbox` and `libdvdnav-xbox`); `libdvdcss` and FFmpeg are pinned to
upstream revisions. The superproject records exact revisions; initialize them
after cloning:

```powershell
git submodule update --init --recursive
```

See `docs/dependencies.md` for pinned revisions, licenses, and the porting
boundary. Keep third-party sources and build outputs separate from
application code.

The Blades XUI layouts, icons, and Xenon font are Microsoft assets and are
not covered by the GPL. The DVD-Video logo artwork is documented in
[`assets/ATTRIBUTION.md`](assets/ATTRIBUTION.md); it comes from
[Wikimedia Commons](https://commons.wikimedia.org/wiki/File:DVD-Video_Logo.svg)
and may be subject to trademark restrictions. Review both before
redistributing a built XEX.
