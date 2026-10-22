#!/usr/bin/env bash
# Build the ffwrap shim for Linux against the pinned FFmpeg (../../native/common.sh) and stage it, with the FFmpeg
# libraries it needs, into native/dist/<platform>, which this module's jar bundles.
#
#   ./native/build.sh
#
# Needs a C compiler (cc), curl and xz. FFmpeg is unpacked outside the source tree: its libraries are symlink
# chains (libavcodec.so -> .so.62 -> .so.62.28.103), which a VM's shared folder cannot hold, and only the real
# files are staged.
set -euo pipefail
cd "$(dirname "$0")"
. ../../native/common.sh

case "$(uname -m)" in
    x86_64)
        PLATFORM=linux-x86_64; ASSET=$BUILD-linux64-lgpl-shared-8.1.tar.xz
        SHA=d4d7b6936f492c0b866b1cb4a29a1bbae09d6a064b4ec4eb90c95bed7969535a ;;
    aarch64)
        PLATFORM=linux-aarch64; ASSET=$BUILD-linuxarm64-lgpl-shared-8.1.tar.xz
        SHA=d44838f375c66cf31a8b1473b6f8bad76e5c7dac55870c4d96a75c907a52ba2d ;;
    *) echo "no pinned FFmpeg build for Linux on $(uname -m)" >&2; exit 1 ;;
esac

fetch_verified "$URL/$ASSET" "ffmpeg/$ASSET" "$SHA"
fetch_miniaudio

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
tar -xJf "ffmpeg/$ASSET" -C "$WORK"
FF="$WORK/${ASSET%.tar.xz}"

DEST="dist/$PLATFORM"
rm -rf "$DEST"
mkdir -p "$DEST" build
: > "$DEST/libs.txt"

"${CC:-cc}" -O2 -shared -fPIC -Wall -fvisibility=hidden -I "$COMMON/include" -I "$COMMON/miniaudio" -I "$FF/include" \
    "$COMMON/ffwrap/ffwrap.c" "$COMMON/ffwrap/audio.c" "$COMMON/ffwrap/level.c" "$COMMON/ffwrap/encode.c" "$COMMON/ffwrap/miniaudio.c" -o build/libffwrap.so \
    -L "$FF/lib" -lavformat -lavcodec -lswresample -lswscale -lavutil -lpthread -lm -ldl \
    -Wl,-rpath,'$ORIGIN'
for lib in $LIBS; do
    # The soname file (libavcodec.so.62), copied as the real file it points at.
    file=$(cd "$FF/lib" && ls "lib$lib".so.[0-9]* | grep -E '\.so\.[0-9]+$')
    cp -L "$FF/lib/$file" "$DEST/"
    echo "$file" >> "$DEST/libs.txt"
done
cp build/libffwrap.so "$DEST/"
echo libffwrap.so >> "$DEST/libs.txt"
stage_notes "$FF" "$DEST"
