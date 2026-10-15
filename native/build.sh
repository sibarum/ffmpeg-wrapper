#!/usr/bin/env bash
# Build the ffwrap shim against a pinned FFmpeg and stage it, with the FFmpeg libraries it needs, into
# native/dist/bin, which the Maven build bundles into the jar.
#
# FFmpeg itself is not built here. A pinned LGPL shared build from BtbN/FFmpeg-Builds is fetched into
# native/ffmpeg/ (not committed) and checked against its sha256, and the shim is compiled against its
# headers and linked against its libraries: MSVC on Windows, the system C compiler on Linux.
#
#   ./native/build.sh
#
# LGPL, shared: only FFmpeg's own decoders and the permissively licensed ones BtbN links in (dav1d,
# libvpx, openh264, ...), and FFmpeg stays a separate library a user can swap. Nothing GPL is bundled.
set -euo pipefail
cd "$(dirname "$0")"

# The build, pinned. A month's last autobuild, which BtbN keeps; a bump changes all of these together.
RELEASE=autobuild-2026-09-30-13-08
BUILD=ffmpeg-n8.1.3-9-g29e619e767
URL="https://github.com/BtbN/FFmpeg-Builds/releases/download/$RELEASE"

# The jar keeps one directory per platform, named the way NativeLib.java asks for it at runtime.
case "$(uname -s)-$(uname -m)" in
    MINGW*-x86_64|MSYS*-x86_64|CYGWIN*-x86_64)
        PLATFORM=windows-x86_64; ASSET=$BUILD-win64-lgpl-shared-8.1.zip
        SHA=3e47bda1607740550141e37c0e49d1e5182b34699f15adfd137ee266d346811a ;;
    Linux-x86_64)
        PLATFORM=linux-x86_64; ASSET=$BUILD-linux64-lgpl-shared-8.1.tar.xz
        SHA=d4d7b6936f492c0b866b1cb4a29a1bbae09d6a064b4ec4eb90c95bed7969535a ;;
    Linux-aarch64)
        PLATFORM=linux-aarch64; ASSET=$BUILD-linuxarm64-lgpl-shared-8.1.tar.xz
        SHA=d44838f375c66cf31a8b1473b6f8bad76e5c7dac55870c4d96a75c907a52ba2d ;;
    *) echo "no pinned FFmpeg build for $(uname -s)-$(uname -m): BtbN builds Windows and Linux only," \
            "so macOS needs FFmpeg from elsewhere — add it here and to NativeLib.java" >&2; exit 1 ;;
esac

# --- FFmpeg ---------------------------------------------------------------------------------------
mkdir -p ffmpeg
if [ ! -f "ffmpeg/$ASSET" ]; then
    echo "fetching $ASSET"
    curl -fSL -o "ffmpeg/$ASSET.part" "$URL/$ASSET"
    mv "ffmpeg/$ASSET.part" "ffmpeg/$ASSET"
fi
echo "$SHA  ffmpeg/$ASSET" | sha256sum -c --quiet - || {
    echo "ffmpeg/$ASSET does not match its pinned sha256: delete it and run again" >&2
    exit 1
}
FF="ffmpeg/${ASSET%.zip}"
FF="${FF%.tar.xz}"
if [ ! -d "$FF" ]; then
    case "$ASSET" in
        *.zip) unzip -q "ffmpeg/$ASSET" -d ffmpeg ;;
        *) tar -xJf "ffmpeg/$ASSET" -C ffmpeg ;;
    esac
fi

# The libraries the shim needs, in the order they must be loaded: each after the ones it links. libs.txt
# says this order to NativeLib, so the loader never has to know a version number.
LIBS="avutil swresample swscale avcodec avformat"

DEST="dist/bin/$PLATFORM"
rm -rf "$DEST"
mkdir -p "$DEST" build
: > "$DEST/libs.txt"

# --- the shim -------------------------------------------------------------------------------------
case "$PLATFORM" in
    windows-*)
        VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
        [ -x "$VSWHERE" ] || { echo "vswhere not found: install the Visual Studio Build Tools (C++)" >&2; exit 1; }
        VS=$("$VSWHERE" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
            -property installationPath | tr -d '\r')
        [ -n "$VS" ] || { echo "no Visual Studio with the C++ tools found" >&2; exit 1; }
        BAT=build/cl.bat
        cat > "$BAT" <<EOF
@set "PATH=%PATH%;%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
@call "$VS\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul || exit /b 1
cl /nologo /O2 /LD /MD /W3 /I "$(cygpath -w include)" /I "$(cygpath -w "$FF/include")" "$(cygpath -w ffwrap/ffwrap.c)" /Fe"$(cygpath -w build/ffwrap.dll)" /Fo"$(cygpath -w build/ffwrap.obj)" /link /LIBPATH:"$(cygpath -w "$FF/lib")" avformat.lib avcodec.lib swscale.lib avutil.lib || exit /b 1
EOF
        cmd.exe //c "$(cygpath -w "$BAT")"
        for lib in $LIBS; do
            file=$(cd "$FF/bin" && ls "$lib"-[0-9]*.dll)
            cp "$FF/bin/$file" "$DEST/"
            echo "$file" >> "$DEST/libs.txt"
        done
        cp build/ffwrap.dll "$DEST/"
        echo ffwrap.dll >> "$DEST/libs.txt"
        ;;
    linux-*)
        CC=${CC:-cc}
        # $ORIGIN: the shim finds FFmpeg beside itself, wherever the jar was extracted to.
        "$CC" -O2 -shared -fPIC -Wall -I include -I "$FF/include" ffwrap/ffwrap.c -o build/libffwrap.so \
            -L "$FF/lib" -lavformat -lavcodec -lswscale -lavutil -Wl,-rpath,'$ORIGIN'
        for lib in $LIBS; do
            # The soname, libavutil.so.60: what the libraries name each other by.
            file=$(cd "$FF/lib" && ls "lib$lib".so.[0-9]* | grep -E '\.so\.[0-9]+$')
            cp -L "$FF/lib/$file" "$DEST/"
            echo "$file" >> "$DEST/libs.txt"
        done
        cp build/libffwrap.so "$DEST/"
        echo libffwrap.so >> "$DEST/libs.txt"
        ;;
esac

# LGPL: the licence goes with the libraries, and says where their source is.
cp "$FF/LICENSE.txt" "$DEST/FFMPEG-LICENSE.txt"
printf 'FFmpeg %s, LGPL shared build from https://github.com/BtbN/FFmpeg-Builds (%s).\nSource: https://github.com/FFmpeg/FFmpeg/tree/%s\n' \
    "${BUILD#ffmpeg-}" "$RELEASE" "${BUILD##*-g}" > "$DEST/FFMPEG-SOURCE.txt"

echo "staged: $DEST ($(wc -l < "$DEST/libs.txt") libraries, $(du -sh "$DEST" | cut -f1))"
