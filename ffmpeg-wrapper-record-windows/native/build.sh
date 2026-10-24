#!/usr/bin/env bash
# Build the encoding-only FFmpeg for Windows from source (../../native/record.sh), the ffrecord shim over it, and
# stage them into native/dist/<platform>, which this module's jar bundles.
#
#   ./native/build.sh          (from Git Bash or an MSYS2 shell; it runs itself again in MSYS2's UCRT64)
#
# Needs MSYS2 at C:\msys64 with the UCRT64 toolchain and what FFmpeg's encoders are built against:
#   pacman -S make diffutils mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,nasm,ffnvcodec-headers,amf-headers,libvpl}
# Everything is built with MinGW-w64's gcc and linked so the DLLs need only Windows' own: libgcc, libstdc++ (which
# libvpl's dispatcher is written in) and winpthread go in statically.
set -euo pipefail

if [ "${MSYSTEM:-}" != UCRT64 ]; then
    MSYS=/c/msys64/usr/bin
    [ -x "$MSYS/bash" ] || { echo "MSYS2 not found at C:\\msys64: install it, then the packages above" >&2; exit 1; }
    exec "$MSYS/env" MSYSTEM=UCRT64 CHERE_INVOKING=1 "$MSYS/bash" -lc '"$0" "$@"' "$(cygpath -u "$0" 2>/dev/null || echo "$0")" "$@"
fi

cd "$(dirname "$0")"
. ../../native/record.sh

PLATFORM=windows-x86_64

fetch_verified "$REC_FFMPEG_URL" "ffmpeg/ffmpeg-$REC_FFMPEG.tar.xz" "$REC_FFMPEG_SHA"
fetch_verified "$REC_SVT_URL" "ffmpeg/SVT-AV1-v$REC_SVT.tar.gz" "$REC_SVT_SHA"

WORK="$PWD/build"
PREFIX="$WORK/prefix"
rm -rf "$PREFIX"
mkdir -p "$WORK"

build_svt "ffmpeg/SVT-AV1-v$REC_SVT.tar.gz" "$WORK" "$PREFIX"

rm -rf "$WORK/ffmpeg"
mkdir -p "$WORK/ffmpeg"
tar -xJf "ffmpeg/ffmpeg-$REC_FFMPEG.tar.xz" -C "$WORK/ffmpeg" --strip-components=1

# The GPU's encoders: NVENC and AMF load the driver at run time and need only headers; Quick Sync goes through
# libvpl's dispatcher, which finds the driver's runtime the same way. Quick Sync takes frames through D3D11.
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
configure_ffmpeg "$WORK/ffmpeg" "$PREFIX" \
    --target-os=mingw32 --pkg-config-flags=--static \
    --enable-ffnvcodec --enable-nvenc --enable-encoder=av1_nvenc \
    --enable-amf --enable-encoder=av1_amf \
    --enable-libvpl --enable-encoder=av1_qsv --enable-d3d11va \
    --extra-ldflags="-static-libgcc -static-libstdc++ -static" \
    --extra-libs="-lstdc++"
make -C "$WORK/ffmpeg" -j"$(nproc)"
make -C "$WORK/ffmpeg" install

gcc -O2 -Wall -shared -o "$WORK/ffrecord.dll" "$REC_COMMON/record/record.c" \
    -I"$REC_COMMON/record" -I"$PREFIX/include" -L"$PREFIX/lib" \
    -lavformat$REC_SUFFIX -lavcodec$REC_SUFFIX -lswscale$REC_SUFFIX -lavutil$REC_SUFFIX \
    -static-libgcc

DEST="dist/$PLATFORM"
rm -rf "$DEST"
mkdir -p "$DEST"
: > "$DEST/libs.txt"
for lib in $REC_LIBS; do
    file=$(cd "$PREFIX/bin" && ls "$lib$REC_SUFFIX"-[0-9]*.dll)
    cp "$PREFIX/bin/$file" "$DEST/"
    echo "$file" >> "$DEST/libs.txt"
done
cp "$WORK/ffrecord.dll" "$DEST/"
echo ffrecord.dll >> "$DEST/libs.txt"
strip --strip-unneeded "$DEST"/*.dll

# Nothing but Windows' own DLLs and each other: a MinGW runtime left dynamic would be missing on a user's machine.
for dll in "$DEST"/*.dll; do
    for need in $(objdump -p "$dll" | sed -n 's/^\s*DLL Name: //p'); do
        case "$need" in
            *"$REC_SUFFIX"-[0-9]*.dll|KERNEL32.dll|USER32.dll|ADVAPI32.dll|ole32.dll|OLEAUT32.dll|SHELL32.dll| \
            bcrypt.dll|api-ms-win-*.dll|d3d11.dll|dxgi.dll|d3d9.dll|dxva2.dll|ucrtbase.dll|msvcrt.dll) ;;
            *) echo "$(basename "$dll") needs $need, which is not Windows' own" >&2; exit 1 ;;
        esac
    done
done

stage_rec_notes "$WORK/ffmpeg" "$WORK/svt" "$DEST"
