# FFmpeg on RXDK

`deps/ffmpeg` pins FFmpeg 9.1 (`n9.1`). The LGPL-only build includes MPEG-2
video and the elementary-stream and MPEG program-stream (VOB) demuxers.
The VOB demuxer recognizes Dolby Digital (AC-3), MPEG Layer II (MP2),
DVD-Video LPCM, and DTS tracks. The build includes their decoders and
the AC-3, MPEG audio, and DTS parsers. The player uses FFmpeg for video
and embedded audio. DVD navigation is separate.

## Toolchain

Run `bash tools/configure-ffmpeg-rxdk.sh` from Ubuntu on WSL with the
checkout on a Windows drive. Install `gcc`, `libc6-dev`, `make`, `git`, and
Python 3.12 or newer in Ubuntu. The script uses `RXDK360` (default
`C:\Program Files\RXDK-360`) and stages the committed submodule sources
under `build/ffmpeg-rxdk` so that Windows Git line-ending conversion does
not affect configure.

After configure, run `make -C build/ffmpeg-rxdk -j8`. This produces
`libavformat/libavformat.a`, `libavcodec/libavcodec.a`,
`libswresample/libswresample.a`, and
`libavutil/libavutil.a` under the build directory. The scalar baseline has no
FFmpeg threading or built-in I/O protocols: keep FFmpeg calls on one
playback worker and use custom AVIO for input.

The shipped AltiVec build is configured with `--xbox-altivec`, which enables
w32threads and FFmpeg's native slice threading. The player still supplies
custom AVIO for disc input.

## Optical DVD input

`src/FfmpegFileIo.cpp` feeds libdvdnav-selected sectors to FFmpeg through a
non-seekable custom AVIO context without changing `deps/ffmpeg`. A dedicated
reader prefetches up to four 128 KiB chunks. AVIO waits only when no bytes
are ready. Navigation boundaries drain or replace queued data according to
the DVD VM event.

## AltiVec build

In WSL, run `bash tools/configure-ffmpeg-rxdk.sh --xbox-altivec` and
`make -C build/ffmpeg-rxdk-altivec -j8`. Then run
`.\build.ps1` or `.\build.ps1 -Configuration Debug` in PowerShell.
Both configurations link the AltiVec archives. Motion estimation remains
scalar. The IDCT replaces `VMHRADDSHS` with exact split-byte products,
rounding shifts, and a saturating pack, and Xenon IDCT is enabled by default
in this build. No files in the FFmpeg source tree are patched.

## Playback controls

During a DVD menu, the D-pad moves the disc highlight and **A** activates the
selected button or continues a still. **Start** or **Back** opens the
playback overlay.

In the overlay, D-pad Left/Right selects an item, **A** activates it, **B**
dismisses the overlay, and **Start** toggles pause directly. The overlay
hides after four seconds of inactivity while playing.

The first page provides **Play/Pause**, **Stop**, **DVD Menu**, and
**Title**, with a control to switch pages. The second page provides
**Subtitles**, **Sound** (next audio track), **Aspect** (display mode),
**Zoom**, and **Info**. Elapsed time follows the playback clock; remaining
time appears when FFmpeg reports a duration. Display cycles through Auto,
Letterbox, Fullscreen (crop to fill), Stretch, Native, and Letterbox Zoom
(for 4:3 transfers with baked-in widescreen bars). Auto and Letterbox both
preserve the source aspect ratio. Fullscreen and Stretch also look the same
as Auto when the source and output already match. The boot log reports both
ratios.

**Subtitles** cycles through the disc's bitmap subtitle streams and Off.
To select audio at startup, put a one-based supported audio track number in
`audio-track.txt` beside the XEX. Remove the file to use FFmpeg's preferred
track.

The overlay controls optical DVD playback. Menu buttons, chapter navigation,
10-second backward/forward skips, live audio-track switching, subtitles, and
the display/zoom modes are implemented. Seek skips are limited to the current
DVD title/PGC; authored menus and still frames do not support time seeking.
Resume bookmarks and CD playback are not implemented.

## Audio and pacing

The AltiVec build defaults to two-way MPEG-2 slice decoding using FFmpeg's
native RXDK thread adapter. The calling decoder runs on logical processor 2
and the helper runs on 4. Audio remains single-threaded and its refill policy
is unchanged.

libdvdnav supplies the program stream to FFmpeg, which selects a supported
audio track and converts it to 48 kHz, stereo, signed 16-bit PCM for
XAudio2. Playback starts on the first presented video frame and uses the
XAudio2 sample clock as master. During an audio underrun, video normally
holds. Startup targets six decoded frames, 16 XAudio2 buffers, and 750 ms of
compressed video estimated from the video bitrate (or the container bitrate,
then 6 Mb/s if neither is reported). It stops early at EOF, a read-ahead
limit, or 24 queued audio buffers. The boot log reports the queued, target,
and budget bytes.

During playback, the worker starts refilling audio below eight buffers when
at least three decoded frames are ready. Below three audio buffers, it
prioritizes audio regardless of decoded-video depth until six audio buffers
are queued or the read-ahead limit is reached. With a full decoded queue, it
can also read ahead toward 12 compressed video packets. Read-ahead is
limited to 96 packets and stops once queued video payload reaches the lesser
of 4 MiB and 1/32 of the available physical RAM sampled at startup. Clock
recovery remains a fallback. If video gets behind, expired frames are
skipped.

Pacing samples stay in RAM during playback. A background logger writes
progress to `360DVD.log` about every two seconds, including while paused.
The render thread never waits for a progress write. The full CSV and summary
are written when playback ends. The trace records `audio_recovery_ticks`,
and the summary records `video_packet_peak`.
