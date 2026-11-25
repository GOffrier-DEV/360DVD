# DVD library dependencies

The libraries are Git submodules under `deps/`. `libdvdread` and `libdvdnav`
use the Xbox port branches in this repository; the other submodules track
upstream. The parent repository records a specific commit for each library, so
builds remain reproducible until the submodules are explicitly advanced.

| Path | Upstream | Pinned revision | License | Built |
|---|---|---|---|---|
| `deps/libdvdread` | this repository, `libdvdread-xbox` | `7.1.1-126-gf0e5736` | GPL-2.0-or-later | yes |
| `deps/libdvdnav` | this repository, `libdvdnav-xbox` | `7.0.0-21-gabc995d` (reports 7.0.1) | GPL-2.0-or-later | yes |
| `deps/libdvdcss` | https://code.videolan.org/videolan/libdvdcss | `1.6.0-7-g811ce97` | GPL-2.0-or-later | yes |
| `deps/ffmpeg` | https://github.com/FFmpeg/FFmpeg | `n9.1` (`74e4393`) | LGPL-2.1-or-later | yes |

Initialize after cloning with `git submodule update --init --recursive`.
Advance intentionally with `git submodule update --remote`; review the changed
gitlinks and library changelogs before committing. Because the two DVD
navigation libraries point at this repository, those branch names must remain
published for a fresh checkout to initialize.

## Porting boundary

- `libdvdread` and `libdvdnav` are built as static C libraries for the Xbox
  target. `libdvdnav` depends on the matching `libdvdread` submodule.
- CSS is supplied by `libdvdcss`; do not add a separate hard-coded CSS key
  path. It is linked into the DVD input layer rather than loaded dynamically.
- The console's optical-drive I/O is platform-specific. Isolate it behind the
  `libdvdread` filesystem/stream callbacks and the `libdvdcss` device backend;
  do not assume desktop Win32 device handles or POSIX `/dev` paths work on
  Xbox 360.
- Keep upstream source pristine where possible. Put Xbox-specific config,
  compatibility shims, and project files under `port/` or in the top-level
  build integration, and document any necessary upstream patches.
- The player links upstream code under the licenses above. The application as
  a whole is distributed under GPL-2.0-or-later; see `LICENSE`.

## RXDK build integration

`.\build.ps1 -Configuration Release` builds with RXDK Clang and the installed
Visual Studio 2022 MSBuild integration. The upstream C files are compiled in
RXDK's pure-Clang mode, then archived as `build\libdvd360.a`; the renderer is
compiled in XDK-header mode and links that archive together with the FFmpeg
archives. RXDK's `Lib` target is not available for Clang, so the build script
assembles the C objects with `llvm-ar`. Both Debug and Release XEX
configurations are supported.

Xbox compatibility configuration and header shims live under `port\xbox360\`.
The library build uses libdvdread's POSIX file backend on top of the Xbox
stream callbacks and includes libdvdcss's stream support. `port/xbox360/dvdcss_ioctl.c`
implements the DVD device controls used for CSS authentication; key material
is not logged. The renderer calls libdvdnav/libdvdread for disc navigation and
feeds the selected sectors to FFmpeg.

FFmpeg is built separately in WSL as described in `docs/ffmpeg-rxdk.md`.
`build.ps1` expects the AltiVec static archives under
`build\ffmpeg-rxdk-altivec`.

RXDK Clang runtime/import files are taken from the installed RXDK. Kernel PDB
symbol stores are present under `C:\symsrv` and
`C:\Program Files\RXDK-360\bin\xbox\symsrv`; use the matching PDB for the
console kernel revision when investigating native DVD-drive calls.
