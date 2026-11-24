# FFmpeg migration

`deps/ffmpeg` pins FFmpeg 9.1 (`n9.1`). The LGPL-only build includes MPEG-2
video and the elementary-stream and MPEG program-stream (VOB) demuxers.
The VOB demuxer recognizes Dolby Digital (AC-3), MPEG Layer II (MP2),
DVD-Video LPCM, and DTS tracks. The build includes their decoders and
the AC-3, MPEG audio, and DTS parsers. The player uses FFmpeg for video
and embedded audio. DVD navigation is separate.

## Toolchain checkpoint

Run `bash tools/configure-ffmpeg-rxdk.sh` from Ubuntu on WSL with the
checkout on a Windows drive. Install `gcc`, `libc6-dev`, `make`, `git`, and
Python 3.12 or newer in Ubuntu. The script uses `RXDK360` (default
`C:\Program Files\RXDK-360`) and puts its staged sources and probe logs
under `build/ffmpeg-rxdk`. It stages the committed submodule sources so
that Windows Git line-ending conversion does not affect configure.

The script compiles probes with RXDK's Picolibc headers and links with
RXDK's direct linker, libc, and kernel imports. These are link-only
probes, not runnable titles. Do not substitute a relocatable link that
accepts undefined symbols: that can make FFmpeg incorrectly detect
unavailable libc APIs.

After configure, run `make -C build/ffmpeg-rxdk -j8`. This produces
`libavformat/libavformat.a`, `libavcodec/libavcodec.a`,
`libswresample/libswresample.a`, and
`libavutil/libavutil.a` under the build directory. The baseline has no
FFmpeg threading or built-in I/O protocols: keep FFmpeg calls on one
playback worker and use custom AVIO for input. Building the archives does
not verify a title link or on-console playback.

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
Both configurations link the AltiVec archives.
Motion estimation remains scalar. The experimental IDCT replaces
`VMHRADDSHS` with exact split-byte products, rounding shifts, and a
saturating pack. Xenon IDCT is enabled by default in this build. Place
an empty `ffmpeg-scalar-idct.enable` beside a Debug XEX to test scalar IDCT
with the same build. No files in the FFmpeg source tree are patched.
Verify visual output and frame counts on console
before comparing `video_codec_ms`, underruns, and total playback time
with the scalar build using the same VOB.

## Playback acceptance

### On-screen playback controls

Press a controller button during file playback to show the overlay.
Use D-pad Left/Right to select **Play/Pause** or **Display**, **A** to
activate, **B** to dismiss, or **Start** to toggle pause directly.
The overlay hides after four seconds of inactivity while playing.
Elapsed time follows the playback clock; remaining time appears when
FFmpeg reports a duration. Display cycles through Auto, Letterbox,
Fullscreen (crop to fill), Stretch, Native, and Letterbox Zoom (for 4:3
transfers with baked-in widescreen bars). Auto and Letterbox both preserve
the source aspect ratio. Fullscreen and Stretch also look the same as Auto
when the source and output already match. The boot log reports both ratios.

Select **Subtitles** to cycle through discovered DVD bitmap streams and
Off. Standalone VOB files may lack disc-provided palettes and language
labels. To select audio at startup, put a one-based supported audio
track number in `audio-track.txt` beside the XEX. Remove the file to
use FFmpeg's preferred track.

This overlay controls the MPEG-2/VOB fixture player. DVD menus, chapter
navigation, scan/seek, live audio switching, and resume bookmarks are
not implemented by this playback path.

For an affinity A/B test, run the same XEX and VOB with and without an
empty `playback-affinity.enable` beside the XEX. The marker requests
hardware threads 5 (render), 2 (decode), and 0 (reader), with unchanged
thread priorities and audio refill policy. This does not enable FFmpeg
slice threading.

The AltiVec build defaults to two-way MPEG-2 slice decoding using
FFmpeg's native RXDK thread adapter. The calling decoder runs on logical
processor 2 and the helper runs on 4. Verify `slice_threads=2` in the
startup log. Place an empty `ffmpeg-single-thread.enable` beside the XEX
for a single-thread comparison.
Use the same affinity marker setting in both runs. Audio remains
single-threaded and its refill policy is unchanged.

- Build normally with `.\build.ps1`; libmpeg2 is no longer built or linked.
  Insert a physical DVD. libdvdnav supplies the program stream to FFmpeg,
  which selects a supported audio track and converts it to 48 kHz, stereo,
  signed 16-bit PCM for
  XAudio2. Playback starts on the first presented video frame and uses
  the XAudio2 sample clock as master. During an audio underrun, video
  normally holds. Startup targets six decoded frames, 16 XAudio2 buffers,
  and 750 ms of compressed video estimated from the video bitrate (or the
  container bitrate, then 6 Mb/s if neither is reported). It stops early
  at EOF, a read-ahead limit, or 24 queued audio buffers. The boot log
  reports the queued, target, and budget bytes. During playback, the worker starts refilling audio below eight
  buffers when at least three decoded frames are ready. Below three audio
  buffers, it prioritizes audio regardless of decoded-video depth until
  six audio buffers are queued or the read-ahead limit is reached. With a full
  decoded queue, it can also read ahead toward 12 compressed video
  packets. Read-ahead is limited to 96 packets and stops once queued
  video payload reaches the lesser of 4 MiB and 1/32 of the available
  physical RAM sampled at startup. Clock recovery remains a fallback.
  Pacing samples stay in RAM during playback. A background logger writes
  progress to `360DVD.log` about every two seconds, including while paused.
  The render thread never waits for a progress write. The full CSV and
  summary are written when playback ends.
  Check `audio_recovery_ticks` in the trace and `video_packet_peak` in
  the summary log. If video gets behind, it skips expired frames. Initial
  A/V timestamp offsets and discontinuities are not yet compensated.
- Compare `video_codec_ms` and `decode_frame_ms` against the previous
  run with the same VOB. `audio_work_ms` covers audio decode, conversion,
  and waits for an XAudio2 buffer slot. The RXDK wrapper compiles the
  MPEG-2 decode, motion, and scalar IDCT sources at `-O3`; measured
  performance on console is still pending.
- On console, check display order, frame count including final delayed
  frames, cadence, and queue underruns for both input formats. The
  scalar FFmpeg decoder runs on the existing frame queue's decode worker.
- Test AC-3, MP2, LPCM, and DTS VOB tracks separately. Verify stream IDs,
  packet timestamps, and decoded sample counts, then test audio track
  selection and A/V sync through XAudio2.
