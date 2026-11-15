# What every OS module's native/build.sh shares: the pinned FFmpeg source, the libraries the shim links,
# miniaudio, and the fetch-and-verify they all do. Sourced, never run; the OS modules' scripts are what run.
#
# FFmpeg is built from the pinned release source: the decoding build by decode.sh, patched so AV1 decodes into the
# caller's memory too, and the recording build by record.sh. The shim (native/ffwrap, this folder) is compiled
# against the decoding build's headers and linked against its libraries. The sound output, miniaudio, is one
# header fetched the same way into native/miniaudio/ and compiled into the shim.
#
# LGPL, shared: only FFmpeg's own decoders and dav1d, which is BSD-licensed, and FFmpeg stays a separate library a
# user can swap. Nothing GPL is bundled.

# FFmpeg's source, pinned, for both builds. The release tarball is the one its signature (ffmpeg-devel.asc) was
# checked against when this was pinned.
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
