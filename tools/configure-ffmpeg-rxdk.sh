#!/usr/bin/env bash
# Run from WSL with the checkout on a Windows drive.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
build="$root/build/ffmpeg-rxdk"
simd=(--disable-altivec)
altivec=0
if [[ ${1:-} == --xbox-altivec ]]; then
    shift
    build="$build-altivec"
    altivec=1
    simd=(--enable-asm --enable-altivec --disable-vsx --disable-power8 --disable-runtime-cpudetect)
fi
bwdif_vmx=0
if [[ ${1:-} == --experimental-bwdif-vmx ]]; then
    if (( ! altivec )); then
        echo '--experimental-bwdif-vmx requires --xbox-altivec' >&2
        exit 1
    fi
    shift
    bwdif_vmx=1
fi
mkdir -p "$build"
cd "$build"

# Windows Git may have checked out shell scripts and makefiles as CRLF.
# Stage the committed sources with their original line endings.
revision=$(git -C "$root/deps/ffmpeg" rev-parse HEAD)
src="$build/source-$revision"
mkdir -p "$src"
git -C "$root/deps/ffmpeg" archive "$revision" |
    python3 -c 'import sys, tarfile; tarfile.open(fileobj=sys.stdin.buffer, mode="r|").extractall(sys.argv[1], filter="data")' "$src"

# Force a rebuild even when git archive restored an older source timestamp.
if (( bwdif_vmx )); then
    cp "$root/port/xbox360/ffmpeg-bwdif-vmx.h" "$src/libavfilter/rxdk-bwdif-vmx.h"
    sed -i '/^av_cold void ff_bwdif_init_filter_line/i #include "rxdk-bwdif-vmx.h"' "$src/libavfilter/bwdifdsp.c"
    sed -i '/s->filter_line  = ff_bwdif_filter_line_c;/s/ff_bwdif_filter_line_c/dvd360_bwdif_line/' "$src/libavfilter/bwdifdsp.c"
fi
touch "$src/libavfilter/bwdifdsp.c"

sdk=$(wslpath -u "${RXDK360:-C:\\Program Files\\RXDK-360}")
printf 'sdk=%q\naltivec=%d\n' "$sdk" "$altivec" > rxdk-env
if (( altivec )); then
    cp "$root/port/xbox360/ffmpeg-vmx-idct.h" rxdk-vmx-idct.h
    cp "$root/port/xbox360/ffmpeg-slice-atomics.h" rxdk-slice-atomics.h
    cp "$root/port/xbox360/ffmpeg-native-threads.h" "$src/compat/w32pthreads.h"
    # Our native adapter supplies the synchronization APIs missing from RXDK.
    sed -i '/check_func_headers "windows.h process.h" _beginthreadex &&/c\    true \&\&' "$src/configure"
    sed -i '/check_type "windows.h" \(CONDITION_VARIABLE\|INIT_ONCE\) &&/d' "$src/configure"
fi
cat > rxdk-scalar-dsp.h <<'EOF'
#include "config.h"
#undef HAVE_ALTIVEC
#define HAVE_ALTIVEC 0
EOF
cat > rxdk-error-config.h <<'EOF'
#include "config.h"
/* RXDK exposes GNU strerror_r; FFmpeg expects the POSIX int return. */
#undef HAVE_STRERROR_R
#define HAVE_STRERROR_R 0
EOF
export TMPDIR=.

# Configure splits CC on spaces. Keep the tool name space-free and quote the
# actual SDK path inside the shim.
cat > rxdk-cc <<'EOF'
#!/usr/bin/env bash
source ./rxdk-env
args=()
depfile=
previous=
for arg in "$@"; do
    if [[ $previous == -MF ]]; then
        depfile=$arg
    fi
    previous=$arg
    case "$arg" in
        */libavcodec/ppc/idctdsp.c)
            if (( altivec )); then args+=(-include rxdk-vmx-idct.h); fi
            ;;
         */libavcodec/ppc/me_cmp.c)
            # These kernels require standard VMX opcodes missing on Xenon.
            if (( altivec )); then args+=(-include rxdk-scalar-dsp.h); fi
            ;;
        */libavutil/error.c)
            args+=(-include rxdk-error-config.h)
            ;;
        */libavutil/slicethread.c)
            if (( altivec )); then args+=(-include rxdk-slice-atomics.h); fi
            ;;
        */libavcodec/mpeg12dec.c|*/libavcodec/mpegvideo_dec.c|*/libavcodec/mpegvideo_motion.c|*/libavcodec/simple_idct.c)
            args+=(-O3)
            ;;
        */libswresample/rematrix.c)
            # Optimized RXDK codegen emits an unsupported 64-bit record form.
            args+=(-O0)
            ;;
    esac
    case "$arg" in
        /*) arg=$(wslpath -w "$arg") ;;
        -I/*) arg="-I$(wslpath -w "${arg#-I}")" ;;
    esac
    args+=("$arg")
done
includes=$(wslpath -w "$sdk/bin/clang/include")
# Include stdlib before FFmpeg's config can define its no-getenv fallback.
"$sdk/bin/clang/bin/clang.exe" --target=powerpc64-unknown-xbox360 \
    -fshort-wchar -ffunction-sections -fdata-sections -D__Picolibc__ -D_GNU_SOURCE \
    -isystem "$includes/config" -isystem "$includes/picolibc" \
    -include picolibc.h -include stdlib.h "${args[@]}" || {
    status=$?
    if [[ -n $depfile ]]; then rm -f -- "$depfile"; fi
    exit "$status"
}
if [[ -n $depfile && -f $depfile ]]; then
    # Windows Clang writes drive-letter paths, which GNU make reads as rules.
    sed -i -E 's@\\([^[:space:]])@/\1@g; s@([A-Z]):/@/mnt/\L\1/@g' "$depfile"
fi
EOF
cat > rxdk-ld <<'EOF'
#!/usr/bin/env bash
source ./rxdk-env
# Configure only links target probes; it never runs them on the host.
args=()
for arg in "$@"; do
    case "$arg" in
        -lm) ;;  # RXDK math symbols are in libc.
        *) args+=("$arg") ;;
    esac
done
extra=()
if (( altivec )); then
    extra=(--gc-sections "$(wslpath -w "$sdk/lib/xbox/xapilib.a")" rxdk-helpers.a)
fi
exec "$sdk/bin/clang/bin/ld.lld.exe" -e main "${args[@]}" \
    --start-group "$(wslpath -w "$sdk/lib/xbox/libc.a")" \
    "${extra[@]}" kernel_import.a --end-group
EOF
cat > rxdk-ar <<'EOF'
#!/usr/bin/env bash
source ./rxdk-env
exec "$sdk/bin/clang/bin/llvm-ar.exe" "$@"
EOF
chmod +x rxdk-cc rxdk-ld rxdk-ar

"$sdk/bin/clang/bin/clang.exe" --target=powerpc64-unknown-xbox360 \
    -c "$(wslpath -w "$sdk/lib/xbox/kernel_import.s")" -o kernel_import.o
./rxdk-ar rcs kernel_import.a kernel_import.o
if (( altivec )); then
    ./rxdk-ar x "$(wslpath -w "$sdk/lib/xbox/libcMT.a")" crtfpr.o crtgpr.o crtvmx.o chkstk.o u64tod.o
    ./rxdk-ar rcs rxdk-helpers.a crtfpr.o crtgpr.o crtvmx.o chkstk.o u64tod.o
fi

exec "$src/configure" \
    --arch=ppc64 --target-os=none --enable-cross-compile --cc=./rxdk-cc \
    --cxx=./rxdk-cc --ld=./rxdk-ld --ar=./rxdk-ar --ranlib=true --nm=nm --host-cc=gcc \
    --disable-everything --disable-autodetect --disable-programs --disable-doc \
    --disable-network --disable-asm --disable-inline-asm "${simd[@]}" \
    --disable-pthreads --disable-avdevice --enable-avfilter \
    --enable-filter=bwdif \
    --enable-swresample --disable-swscale \
    --enable-decoder=mpeg2video,ac3,mp2,pcm_dvd,dca,dvdsub \
    --enable-parser=mpegvideo,ac3,mpegaudio,dca,dvdsub \
    --enable-demuxer=mpegps,mpegvideo "$@"
