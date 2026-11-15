# What every OS module's native/build.sh shares: the pinned FFmpeg, the libraries the shim links, miniaudio, and
# the fetch-and-verify they all do. Sourced, never run; the OS modules' scripts are what run.
#
# On Linux, FFmpeg itself is not built: a pinned LGPL shared build from BtbN/FFmpeg-Builds is fetched and checked
# against its sha256, and the shim (native/ffwrap, this folder) is compiled against its headers and linked against
# its libraries. On Windows it is built from the pinned source below, patched so AV1 decodes into the caller's
# memory too (decode.sh). The sound output, miniaudio, is one header fetched the same way into native/miniaudio/
# and compiled into the shim.
#
# LGPL, shared: only FFmpeg's own decoders and permissively licensed ones (dav1d, and on Linux the others BtbN
# links in: libvpx, openh264, ...), and FFmpeg stays a separate library a user can swap. Nothing GPL is bundled.

# The prebuilt build, pinned, which Linux uses: a month's last autobuild, which BtbN keeps; a bump changes this
# and every OS module's asset sha256 together.
RELEASE=autobuild-2026-09-30-13-08
BUILD=ffmpeg-n8.1.3-9-g29e619e767
URL="https://github.com/BtbN/FFmpeg-Builds/releases/download/$RELEASE"

# FFmpeg's source, pinned, for the builds made from it: the decoding build on Windows (decode.sh) and every
# recording build (record.sh). The release tarball is the one its signature (ffmpeg-devel.asc) was checked against
# when this was pinned; BtbN's autobuild above is of the same release.
FFMPEG_SRC=8.1.3
FFMPEG_SRC_SHA=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
FFMPEG_SRC_URL="https://ffmpeg.org/releases/ffmpeg-$FFMPEG_SRC.tar.xz"

# The FFmpeg libraries the shim links, each after the ones it needs.
LIBS="avutil swresample swscale avcodec avformat"

# This folder: the shim's source (ffwrap/, include/) and miniaudio.
COMMON="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# fetch_verified <url> <file> <sha256>: download once, then always check.
fetch_verified() {
    local url=$1 file=$2 sha=$3
    mkdir -p "$(dirname "$file")"
    if [ ! -f "$file" ]; then
        echo "fetching $(basename "$file")"
        curl -fSL -o "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    echo "$sha  $file" | sha256sum -c --quiet - || {
        echo "$file does not match its pinned sha256: delete it and run again" >&2
        exit 1
    }
}

# fetch_miniaudio: the header into $COMMON/miniaudio, pinned the same way.
MINIAUDIO=0.11.25
MINIAUDIO_SHA=ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
fetch_miniaudio() {
    fetch_verified "https://raw.githubusercontent.com/mackron/miniaudio/$MINIAUDIO/miniaudio.h" \
        "$COMMON/miniaudio/miniaudio.h" "$MINIAUDIO_SHA"
}

# stage_notes <ffmpeg dir> <dest>: FFmpeg's licence, and where the build came from, beside the libraries.
stage_notes() {
    cp "$1/LICENSE.txt" "$2/FFMPEG-LICENSE.txt"
    printf 'FFmpeg %s, LGPL shared build from https://github.com/BtbN/FFmpeg-Builds (%s).\nSource: https://github.com/FFmpeg/FFmpeg/tree/%s\n' \
        "${BUILD#ffmpeg-}" "$RELEASE" "${BUILD##*-g}" > "$2/FFMPEG-SOURCE.txt"
    echo "staged: $2 ($(wc -l < "$2/libs.txt") libraries, $(du -sh "$2" | cut -f1))"
}
