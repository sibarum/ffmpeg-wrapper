#!/usr/bin/env bash
# Build the decoding FFmpeg for Linux from source (../../native/decode.sh), the ffwrap shim against it, and stage
# them into native/dist/<platform>, which this module's jar bundles.
#
#   ./native/build-in-container.sh     (the release build: this script, on AlmaLinux 8, in podman or docker)
#   ./native/build.sh                  (this machine as it is, which must be as old: see MAX_GLIBC)
#
# Needs cc, make, meson, ninja, nasm (x86_64 only), pkg-config, patch, curl, xz and zlib's headers. FFmpeg and dav1d
# are built outside the source tree: their libraries are symlink chains, which a VM's shared folder cannot hold, and
# only the real files are staged. dav1d is linked into libavcodec statically; zlib stays the system's libz.so.1,
# which every distribution has.
#
# A Linux library runs only where glibc is at least as new as where it was built, so the release build is made on
# an old base, and this fails if anything it stages needs a glibc newer than MAX_GLIBC.
#
# NOT YET RUN: written beside the Windows build, which is; the first run on Linux may need its flags corrected.
set -euo pipefail
cd "$(dirname "$0")"
. ../../native/decode.sh

# The newest glibc a staged library may need: AlmaLinux 8's (build-in-container.sh), which is what BtbN's builds
# asked too, so the libraries run on any distribution from 2019 on (Debian 10, Ubuntu 20.04, RHEL 8 and later).
MAX_GLIBC=2.28

case "$(uname -m)" in
    x86_64) PLATFORM=linux-x86_64 ;;
    aarch64) PLATFORM=linux-aarch64 ;;
    *) echo "no decoding FFmpeg build for Linux on $(uname -m)" >&2; exit 1 ;;
esac

fetch_verified "$FFMPEG_SRC_URL" "ffmpeg/ffmpeg-$FFMPEG_SRC.tar.xz" "$FFMPEG_SRC_SHA"
fetch_verified "$DAV1D_URL" "ffmpeg/dav1d-$DAV1D.tar.xz" "$DAV1D_SHA"
fetch_miniaudio

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PREFIX="$WORK/prefix"

# Position-independent, as it goes into a shared libavcodec (meson's default for a static library, said anyway).
build_dav1d "ffmpeg/dav1d-$DAV1D.tar.xz" "$WORK" "$PREFIX" -Db_staticpic=true

unpack_ffmpeg "ffmpeg/ffmpeg-$FFMPEG_SRC.tar.xz" "$WORK/ffmpeg"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
configure_decoding "$WORK/ffmpeg" "$PREFIX" --pkg-config-flags=--static --enable-pic \
    --extra-ldflags="-static-libgcc"
make -C "$WORK/ffmpeg" -j"$(nproc)"
make -C "$WORK/ffmpeg" install

DEST="dist/$PLATFORM"
rm -rf "$DEST"
mkdir -p "$DEST" build
: > "$DEST/libs.txt"

"${CC:-cc}" -O2 -shared -fPIC -Wall -fvisibility=hidden -I "$COMMON/include" -I "$COMMON/miniaudio" -I "$PREFIX/include" \
    "$COMMON/ffwrap/ffwrap.c" "$COMMON/ffwrap/audio.c" "$COMMON/ffwrap/level.c" "$COMMON/ffwrap/staging.c" "$COMMON/ffwrap/miniaudio.c" -o build/libffwrap.so \
    -L "$PREFIX/lib" -lavformat -lavcodec -lswresample -lswscale -lavutil -lpthread -lm -ldl \
    -static-libgcc -Wl,-rpath,'$ORIGIN'
for lib in $LIBS; do
    # The soname file (libavcodec.so.62), copied as the real file it points at.
    file=$(cd "$PREFIX/lib" && ls "lib$lib".so.[0-9]* | grep -E '\.so\.[0-9]+$')
    cp -L "$PREFIX/lib/$file" "$DEST/"
    echo "$file" >> "$DEST/libs.txt"
done
cp build/libffwrap.so "$DEST/"
echo libffwrap.so >> "$DEST/libs.txt"
strip --strip-unneeded "$DEST"/*.so*

# Nothing but the C library's own and each other, and nothing from a newer glibc than MAX_GLIBC: either would fail
# to load on a user's machine.
for so in "$DEST"/*.so*; do
    for need in $(readelf -d "$so" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'); do
        case "$need" in
            libav*.so.[0-9]*|libsw*.so.[0-9]*|libc.so.6|libm.so.6|libpthread.so.0|libdl.so.2|librt.so.1|libz.so.1| \
            ld-linux-x86-64.so.2|ld-linux-aarch64.so.1) ;;
            *) echo "$(basename "$so") needs $need, which not every Linux has" >&2; exit 1 ;;
        esac
    done
    newest=$(objdump -T "$so" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)
    if [ -n "$newest" ] && [ "$(printf '%s\n%s\n' "$newest" "$MAX_GLIBC" | sort -V | tail -1)" != "$MAX_GLIBC" ]; then
        echo "$(basename "$so") needs glibc $newest, newer than $MAX_GLIBC: build it on an older base (build-in-container.sh)" >&2
        exit 1
    fi
done

stage_dec_notes "$WORK/ffmpeg" "$WORK/dav1d" "$DEST"
