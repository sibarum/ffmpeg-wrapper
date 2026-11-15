#!/usr/bin/env bash
# Build the decoding FFmpeg for Windows from source (../../native/decode.sh), the ffwrap shim against it, and stage
# them into native/dist/<platform>, which this module's jar bundles.
#
#   ./native/build.sh          (from Git Bash or an MSYS2 shell; it runs itself again in MSYS2's UCRT64)
#
# Needs MSYS2 at C:\msys64 with the UCRT64 toolchain FFmpeg and dav1d are built with:
#   pacman -S make diffutils patch mingw-w64-ucrt-x86_64-{gcc,meson,ninja,pkgconf,nasm,zlib}
# and the Visual Studio Build Tools (C++), which compile the shim, found through vswhere. FFmpeg and dav1d are
# built with MinGW-w64's gcc and linked so the DLLs need only Windows' own: libgcc, winpthread and zlib go in
# statically.
set -euo pipefail

if [ "${MSYSTEM:-}" != UCRT64 ]; then
    MSYS=/c/msys64/usr/bin
    [ -x "$MSYS/bash" ] || { echo "MSYS2 not found at C:\\msys64: install it, then the packages above" >&2; exit 1; }
    exec "$MSYS/env" MSYSTEM=UCRT64 CHERE_INVOKING=1 "$MSYS/bash" -lc '"$0" "$@"' "$(cygpath -u "$0" 2>/dev/null || echo "$0")" "$@"
fi

cd "$(dirname "$0")"
. ../../native/decode.sh

PLATFORM=windows-x86_64

fetch_verified "$FFMPEG_SRC_URL" "ffmpeg/ffmpeg-$FFMPEG_SRC.tar.xz" "$FFMPEG_SRC_SHA"
fetch_verified "$DAV1D_URL" "ffmpeg/dav1d-$DAV1D.tar.xz" "$DAV1D_SHA"
fetch_miniaudio

WORK="$PWD/build"
PREFIX="$WORK/prefix"
rm -rf "$PREFIX"
mkdir -p "$WORK"

build_dav1d "ffmpeg/dav1d-$DAV1D.tar.xz" "$WORK" "$PREFIX"

unpack_ffmpeg "ffmpeg/ffmpeg-$FFMPEG_SRC.tar.xz" "$WORK/ffmpeg"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
configure_decoding "$WORK/ffmpeg" "$PREFIX" \
    --target-os=mingw32 --pkg-config-flags=--static \
    --extra-ldflags="-static-libgcc -static"
make -C "$WORK/ffmpeg" -j"$(nproc)"
make -C "$WORK/ffmpeg" install

DEST="dist/$PLATFORM"
rm -rf "$DEST"
mkdir -p "$DEST"
: > "$DEST/libs.txt"

# The shim, with MSVC, against the import libraries FFmpeg's install leaves beside its DLLs.
VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
[ -x "$VSWHERE" ] || { echo "vswhere not found: install the Visual Studio Build Tools (C++)" >&2; exit 1; }
VS=$("$VSWHERE" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
    -property installationPath | tr -d '\r')
[ -n "$VS" ] || { echo "no Visual Studio with the C++ tools found" >&2; exit 1; }
BAT="$WORK/cl.bat"
w() { cygpath -w "$1"; }
cat > "$BAT" <<BATEOF
@set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
@call "$VS\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /LD /MD /W3 /I "$(w "$COMMON/include")" /I "$(w "$COMMON/miniaudio")" /I "$(w "$PREFIX/include")" "$(w "$COMMON/ffwrap/ffwrap.c")" "$(w "$COMMON/ffwrap/audio.c")" "$(w "$COMMON/ffwrap/level.c")" "$(w "$COMMON/ffwrap/staging.c")" "$(w "$COMMON/ffwrap/miniaudio.c")" /Fe"$(w "$WORK/ffwrap.dll")" /Fo$(w "$WORK")\\ /link /LIBPATH:"$(w "$PREFIX/bin")" avformat.lib avcodec.lib swresample.lib swscale.lib avutil.lib || exit /b 1
BATEOF
MSYS2_ARG_CONV_EXCL='*' cmd.exe /c "$(w "$BAT")"

for lib in $LIBS; do
    file=$(cd "$PREFIX/bin" && ls "$lib"-[0-9]*.dll)
    cp "$PREFIX/bin/$file" "$DEST/"
    echo "$file" >> "$DEST/libs.txt"
done
strip --strip-unneeded "$DEST"/*.dll
cp "$WORK/ffwrap.dll" "$DEST/"
echo ffwrap.dll >> "$DEST/libs.txt"

# Nothing but Windows' own DLLs, the MSVC runtime the shim was always linked to, and each other: a MinGW runtime
# left dynamic would be missing on a user's machine.
for dll in "$DEST"/*.dll; do
    for need in $(objdump -p "$dll" | sed -n 's/^\s*DLL Name: //p'); do
        case "$need" in
            av*-[0-9]*.dll|sw*-[0-9]*.dll|KERNEL32.dll|USER32.dll|ADVAPI32.dll|ole32.dll|OLEAUT32.dll|SHELL32.dll| \
            bcrypt.dll|api-ms-win-*.dll|ucrtbase.dll|msvcrt.dll|VCRUNTIME140.dll) ;;
            *) echo "$(basename "$dll") needs $need, which is not Windows' own" >&2; exit 1 ;;
        esac
    done
done

stage_dec_notes "$WORK/ffmpeg" "$WORK/dav1d" "$DEST"
