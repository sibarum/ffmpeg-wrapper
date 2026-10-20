#!/usr/bin/env bash
# Build the ffwrap shim for Windows against the pinned FFmpeg (../../native/common.sh) and stage it, with the
# FFmpeg DLLs it needs, into native/dist/<platform>, which this module's jar bundles.
#
#   ./native/build.sh          (from Git Bash)
#
# Needs the Visual Studio Build Tools (C++): the shim is compiled with MSVC, found through vswhere.
set -euo pipefail
cd "$(dirname "$0")"
. ../../native/common.sh

PLATFORM=windows-x86_64
ASSET=$BUILD-win64-lgpl-shared-8.1.zip
SHA=3e47bda1607740550141e37c0e49d1e5182b34699f15adfd137ee266d346811a

fetch_verified "$URL/$ASSET" "ffmpeg/$ASSET" "$SHA"
fetch_miniaudio

FF="ffmpeg/${ASSET%.zip}"
if [ ! -d "$FF" ]; then
    unzip -q "ffmpeg/$ASSET" -d ffmpeg
fi

DEST="dist/$PLATFORM"
rm -rf "$DEST"
mkdir -p "$DEST" build
: > "$DEST/libs.txt"

VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
[ -x "$VSWHERE" ] || { echo "vswhere not found: install the Visual Studio Build Tools (C++)" >&2; exit 1; }
VS=$("$VSWHERE" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
    -property installationPath | tr -d '\r')
[ -n "$VS" ] || { echo "no Visual Studio with the C++ tools found" >&2; exit 1; }
BAT=build/cl.bat
cat > "$BAT" <<BATEOF
@set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
@call "$VS\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /LD /MD /W3 /I "$(cygpath -w "$COMMON/include")" /I "$(cygpath -w "$COMMON/miniaudio")" /I "$(cygpath -w "$FF/include")" "$(cygpath -w "$COMMON/ffwrap/ffwrap.c")" "$(cygpath -w "$COMMON/ffwrap/audio.c")" "$(cygpath -w "$COMMON/ffwrap/miniaudio.c")" /Fe"$(cygpath -w build/ffwrap.dll)" /Fo$(cygpath -w build)\ /link /LIBPATH:"$(cygpath -w "$FF/lib")" avformat.lib avcodec.lib swresample.lib swscale.lib avutil.lib || exit /b 1
BATEOF
cmd.exe //c "$(cygpath -w "$BAT")"
for lib in $LIBS; do
    file=$(cd "$FF/bin" && ls "$lib"-[0-9]*.dll)
    cp "$FF/bin/$file" "$DEST/"
    echo "$file" >> "$DEST/libs.txt"
done
cp build/ffwrap.dll "$DEST/"
echo ffwrap.dll >> "$DEST/libs.txt"
stage_notes "$FF" "$DEST"
