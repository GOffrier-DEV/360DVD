# DVD library dependencies

The libraries are upstream VideoLAN Git submodules on `master`. Each parent
repository records a specific commit, so builds remain reproducible until the
submodules are explicitly advanced.

| Path | Upstream | Version at imported revision | License |
|---|---|---:|---|
| `deps/libdvdread` | https://code.videolan.org/videolan/libdvdread | 7.1.1 | GPL-2.0-or-later |
| `deps/libdvdnav` | https://code.videolan.org/videolan/libdvdnav | 7.0.1 | GPL-2.0-or-later |
| `deps/libdvdcss` | https://code.videolan.org/videolan/libdvdcss | 1.6.0 | GPL-2.0-or-later |

Initialize after cloning with `git submodule update --init --recursive`.
Advance intentionally with `git submodule update --remote`; review the changed
gitlinks and library changelogs before committing.

## Porting boundary

- `libdvdread` and `libdvdnav` are built as static C libraries for the Xbox
  target. `libdvdnav` depends on the matching `libdvdread` submodule.
- CSS is supplied by `libdvdcss`; do not add a separate hard-coded CSS key
  path. Link it into the DVD input layer rather than relying on desktop dynamic
  library loading.
- The console's optical-drive I/O is platform-specific. Isolate it behind the
  `libdvdread` filesystem/stream callbacks and the `libdvdcss` device backend;
  do not assume desktop Win32 device handles or POSIX `/dev` paths work on
  Xbox 360.
- Keep upstream source pristine where possible. Put Xbox-specific config,
  compatibility shims, and project files under `port/` or in the top-level
  build integration, and document any necessary upstream patches.

## RXDK build integration

`.\build.ps1 -Configuration Release` builds with RXDK Clang and the installed
Visual Studio 2022 MSBuild integration. The upstream C files are compiled in
RXDK's pure-Clang mode, then archived as `build\libdvd360.a`; the renderer is
compiled in XDK-header mode and links that archive. RXDK's `Lib` target is not
available for Clang, so the build script assembles the C objects with
`llvm-ar`. Both Debug and Release XEX builds have been verified.

Xbox compatibility configuration and header shims live under
`port\xbox360\`. The library build currently selects libdvdread's POSIX file
backend for filesystem access and includes libdvdcss's stream support. This is
only build integration: the renderer does not yet call libdvdnav/libdvdread,
and no Xbox optical-drive stream or CSS authentication backend has been
implemented. The ioctl stubs intentionally fail rather than report successful
authentication.

RXDK Clang runtime/import files are taken from the installed RXDK. Kernel PDB
symbol stores are present under `C:\symsrv` and
`C:\Program Files\RXDK-360\bin\xbox\symsrv`; use the matching PDB for the
console kernel revision when investigating native DVD-drive calls.
