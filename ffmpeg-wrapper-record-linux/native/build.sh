#!/usr/bin/env bash
# Build the encoding-only FFmpeg for Linux from source (../../native/record.sh), the ffrecord shim over it, and
# stage them into native/dist/<platform>, which this module's jar bundles.
#
#   ./native/build.sh
#
# Needs a C compiler (cc), make, cmake, ninja, nasm, pkg-config, curl and xz, and the GPU encoders' headers:
# nv-codec-headers (pkg-config ffnvcodec) for NVENC and AMF's headers (AMF/core/Factory.h) for AMD. Quick Sync is
# not built on Linux: it goes through VAAPI there, which the recorder does not wire, so Intel and AMD GPUs without
# AMF record with SVT-AV1. FFmpeg and SVT-AV1 are built outside the source tree: their libraries are symlink chains,
# which a VM's shared folder cannot hold, and only the real files are staged.
#
# NOT YET RUN: written beside the Windows build, which is; the first run on Linux may need its flags corrected.
set -euo pipefail
cd "$(dirname "$0")"
. ../../native/record.sh

case "$(uname -m)" in
    x86_64) PLATFORM=linux-x86_64 ;;
    aarch64) PLATFORM=linux-aarch64 ;;
    *) echo "no encoding FFmpeg build for Linux on $(uname -m)" >&2; exit 1 ;;
esac

fetch_verified "$REC_FFMPEG_URL" "ffmpeg/ffmpeg-$REC_FFMPEG.tar.xz" "$REC_FFMPEG_SHA"
fetch_verified "$REC_SVT_URL" "ffmpeg/SVT-AV1-v$REC_SVT.tar.gz" "$REC_SVT_SHA"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
PREFIX="$WORK/prefix"

build_svt "ffmpeg/SVT-AV1-v$REC_SVT.tar.gz" "$WORK" "$PREFIX" -DCMAKE_POSITION_INDEPENDENT_CODE=ON

mkdir -p "$WORK/ffmpeg"
tar -xJf "ffmpeg/ffmpeg-$REC_FFMPEG.tar.xz" -C "$WORK/ffmpeg" --strip-components=1

# NVENC and AMF load the driver at run time and need only headers.
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$PREFIX/lib64/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
GPU=()
if pkg-config --exists ffnvcodec; then
    GPU+=(--enable-ffnvcodec --enable-nvenc --enable-encoder=av1_nvenc)
else
    echo "nv-codec-headers not found: building without NVENC" >&2
fi
if [ -f /usr/include/AMF/core/Factory.h ] || [ -f /usr/local/include/AMF/core/Factory.h ]; then
    GPU+=(--enable-amf --enable-encoder=av1_amf)
else
    echo "AMF headers not found: building without AMF" >&2
fi
configure_ffmpeg "$WORK/ffmpeg" "$PREFIX" --pkg-config-flags=--static --enable-pic \
    --extra-ldflags="-static-libgcc" "${GPU[@]}"
make -C "$WORK/ffmpeg" -j"$(nproc)"
make -C "$WORK/ffmpeg" install

DEST="dist/$PLATFORM"
rm -rf "$DEST"
mkdir -p "$DEST" build
: > "$DEST/libs.txt"

"${CC:-cc}" -O2 -shared -fPIC -Wall -fvisibility=hidden -I "$REC_COMMON/record" -I "$PREFIX/include" \
    "$REC_COMMON/record/record.c" -o build/libffrecord.so \
    -L "$PREFIX/lib" -lavformat$REC_SUFFIX -lavcodec$REC_SUFFIX -lswscale$REC_SUFFIX -lavutil$REC_SUFFIX \
    -lpthread -lm -ldl -Wl,-rpath,'$ORIGIN'
for lib in $REC_LIBS; do
    # The soname file (libavcodec-rec.so.62), copied as the real file it points at.
    file=$(cd "$PREFIX/lib" && ls "lib$lib$REC_SUFFIX".so.[0-9]* | grep -E '\.so\.[0-9]+$')
    cp -L "$PREFIX/lib/$file" "$DEST/"
    echo "$file" >> "$DEST/libs.txt"
done
cp build/libffrecord.so "$DEST/"
echo libffrecord.so >> "$DEST/libs.txt"
strip --strip-unneeded "$DEST"/*.so*
stage_rec_notes "$WORK/ffmpeg" "$WORK/svt" "$DEST"
