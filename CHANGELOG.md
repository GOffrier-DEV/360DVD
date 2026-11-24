# Changelog

All notable changes to 360DVD are recorded here. The project follows
[Semantic Versioning](https://semver.org/).

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
- Debug diagnostics: asynchronous `360DVD.log` progress, navigation log,
  pacing CSV, and VMX/IDCT counters.

### Known limitations

- Scan/seek, resume bookmarks, and CD playback are not implemented.
- Motion compensation remains scalar; only the IDCT uses VMX.

### Dependencies

- libdvdread 7.1.1 and libdvdnav 7.0.1 from this repository's
  `libdvdread-xbox` and `libdvdnav-xbox` branches.
- libdvdcss 1.6.0 and libmpeg2 upstream revisions pinned under `deps/`.
  libmpeg2 is retained for reference and is not built.
- FFmpeg 9.1 (`n9.1`), built as an LGPL-only static archive with MPEG-2,
  AC-3, MP2, LPCM, and DTS support.

[1.0.0]: https://github.com/GOffrier-DEV/360DVD/releases/tag/v1.0.0
