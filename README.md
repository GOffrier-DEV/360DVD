# 360DVD

DVD player for modded Xbox 360 consoles (RGH/JTAG).

The initial target is DVD playback using `libdvdread` for disc access and
`libdvdnav` for DVD navigation. Audio output will use RXDK. The H.264 decoder
in `C:\Users\jason\Documents\Xbox360-GPU-Video-Decoding` is a reference for
video decoding and presentation; it is not copied or modified by this project.
CD playback is intentionally out of scope for the first milestone.

## Current status

The first milestone is an Xbox 360 Direct3D smoke test that renders the
DVD-Video logo bouncing against a black screen. This checks device creation,
shader/texture setup, presentation, and frame timing before disc playback is
added.

Disc I/O, DVD navigation, CSS handling, and audio output are not implemented
yet.

## Video API baseline

`docs/video-api-surface.json` records the required declared Xbox 360 API
surface. It is a declaration checklist, not a certification that each API is
available or works on target hardware. Verify individual calls against the
selected RXDK/XDK headers and on-console tests before relying on them.

Desktop-only D3D9 calls listed in `desktop_assumptions_absent` must not be
assumed available. The memexport APIs are reserved for a later experiment.

## Planned milestones

1. Build and run the DVD logo rendering smoke test on console.
2. Add the XEX application shell and controller-driven UI.
3. Integrate `libdvdread` and `libdvdnav`; validate disc detection and menu
   navigation independently of playback.
4. Implement DVD video presentation and RXDK audio output.
5. Add CSS support behind a separate, documented decryption interface.
6. Revisit CD playback after DVD playback is stable.

## Build

The initial project targets the Xbox 360 XDK/MSBuild project format used by
the H.264 reference project. With the Xbox 360 SDK and Visual Studio 2010
XDK tools installed, run:

```powershell
.\build.ps1
```

The expected output is `build\Release\360DVD.xex` and its `Media` folder.
The project builds successfully in both Debug and Release with the installed
Xbox 360 SDK. Copy the XEX and its `Media` folder to an RGH/JTAG console to
test. The app writes startup diagnostics to `game:\360DVD.log`. On-console
behavior is not yet verified.

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
restrictions. See `media/ATTRIBUTION.md`.
