# Changelog

All notable changes to 360DVD are recorded here. The project follows
[Semantic Versioning](https://semver.org/).

## [1.5.0] - 2026-11-25

- Replaced the DVD screensaver during audio CD playback with a textured 3D
  disc centered above the transport. The disc spins up on play and slows
  to a stop on pause.
- Enabled 2× MSAA for the CD scene only, leaving DVD and XUI rendering unchanged.

## [1.4.1] - 2026-11-25

- Restyled the CD transport as a WMP11-inspired XUI scene with Xbox-green
  focus, gray track controls, and a centered Play/Pause glyph.

## [1.4.0] - 2026-11-25

- Added audio CD playback from the optical drive through XAudio2. Data
  tracks are skipped on mixed-mode CDs.
- Added a compact Blades overlay with previous track, play/pause, and next
  track controls. D-pad focus starts on Play/Pause and stops at the ends.

## [1.3.1] - 2026-11-25

- Check the optical drive every 250 ms for disc removal, including during
  still DVD menus and paused playback.

## [1.3.0] - 2026-11-25

- Added a No Disc screen and automatic First Play when a DVD is reinserted.
- On the verified 17489-dev XAM build with XBDM, eject no longer sends
  360DVD back to the dashboard. Other XAM builds are left unchanged.
- Disabled on-disk libdvdcss key caching while retaining in-session keys.

## [1.2.0] - 2026-11-25

- Added field-rate FFmpeg `bwdif` deinterlacing with automatic field order
  and a playback notification. Progressive video is not deinterlaced.
- Enabled the packed Xenon VMX kernel by default for Xbox builds.

## [1.1.0] - 2026-11-25

### Added

- Chapter navigation and 10-second backward/forward skips from the Blades
  playback controls. Time seeking is available within the current DVD PGC;
  menus and still frames do not support seeking.

### Improved

- Lossless DVD bitmap subtitle batching merges identical adjacent row-runs,
  reducing geometry and draw batches without changing subtitle pixels.

### Fixed

- Hardened DVD navigation event handling and read-ahead allocation against
  malformed disc data.
- Added defensive checks for partial audio initialization and embedded XUI
  resource-reader calls.

## [1.0.0] - 2026-11-24

First release: a DVD player for RGH/JTAG Xbox 360 consoles.

### Added

- Physical DVD playback starting at First Play, with libdvdcss CSS
  authentication over the console's DVD device controls.
- libdvdnav navigation: authored menus and titles, D-pad menu focus, button
  activation, Root/Title, and Continue.
- FFmpeg MPEG-2 video decoding with buffered disc input, audio-mastered
  pacing, expired-frame skipping, and two-way slice decoding enabled by
  default.
- Dolby Digital (AC-3), MPEG Layer II (MP2), DVD-Video LPCM, and DTS audio,
  converted to 48 kHz stereo signed 16-bit PCM and played through XAudio2.
- Blades playback overlay with elapsed/remaining time, play state, language
  labels, and the stock Xbox 360 DVD icon set.
- DVD bitmap subtitle rendering and cycling, plus startup track selection
  through `audio-track.txt`.
- Live audio-track switching and six display modes, including a stock-style
  zoom toggle.
- DVD-Video logo idle animation with independent edge bouncing and tint
  changes.
- Embedded Blades XUI scenes, icons, Xenon font, and DVD logo, so the XEX is
  the only deployment file.
- AltiVec build with the Xenon IDCT path enabled by default.
- Debug diagnostics: asynchronous `360DVD.log` progress, navigation log, and
  pacing CSV.

### Known limitations

- Resume bookmarks and CD playback are not implemented. Time seeking is
  limited to the current DVD PGC and is unavailable in menus or still frames.
- Motion compensation remains scalar; only the IDCT uses VMX.

### Dependencies

- libdvdread 7.1.1 and libdvdnav 7.0.1 from this repository's
  `libdvdread-xbox` and `libdvdnav-xbox` branches.
- libdvdcss 1.6.0 pinned under `deps/`.
- FFmpeg 9.1 (`n9.1`), built as an LGPL-only static archive with MPEG-2,
  AC-3, MP2, LPCM, and DTS support.

[1.0.0]: https://github.com/GOffrier-DEV/360DVD/releases/tag/v1.0.0
[1.1.0]: https://github.com/GOffrier-DEV/360DVD/releases/tag/v1.1.0
[1.2.0]: https://github.com/GOffrier-DEV/360DVD/releases/tag/v1.2.0
[1.3.0]: https://github.com/GOffrier-DEV/360DVD/releases/tag/v1.3.0
[1.3.1]: https://github.com/GOffrier-DEV/360DVD/releases/tag/v1.3.1
[1.4.0]: https://github.com/GOffrier-DEV/360DVD/releases/tag/v1.4.0
[1.4.1]: https://github.com/GOffrier-DEV/360DVD/releases/tag/v1.4.1
[1.5.0]: https://github.com/GOffrier-DEV/360DVD/releases/tag/v1.5.0
