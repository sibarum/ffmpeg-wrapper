# What every OS module's native/build.sh shares to build the decoding FFmpeg from source: the pinned dav1d, the
# patch that lets dav1d decode into the application's memory, and how FFmpeg is configured. Sourced, never run;
# the OS modules' scripts are what run.
#
# Why from source, not a prebuilt FFmpeg: the shim decodes straight into memory the caller lends (ffwrap.h,
# "decoding into the caller's memory"), through get_buffer2. FFmpeg's own decoders all take their memory that way;
# dav1d's AV1 wrapper does not, so with a prebuilt FFmpeg every AV1 frame is copied once.
# patches/libdav1d-get-buffer.patch makes it ask get_buffer2 too, and a patched FFmpeg is one built here.
#
# What it holds: every decoder, demuxer and parser FFmpeg has of its own, which is every format a picture viewer
# plays (H.264, HEVC, VP8, VP9, MPEG-1/2/4, ProRes, DNxHD, AAC, Opus, Vorbis, MP3, FLAC, ...), dav1d for AV1,
# zlib, and the file protocol. No encoder, muxer, filter, device or network protocol: the shim uses none.
# Against the full prebuilt build this replaced (BtbN's), what is gone is the external libraries whose formats
# FFmpeg decodes itself (libvpx, libopus, libvorbis, openh264, libwebp, OpenJPEG, libaom) and a few niche ones it
# does not (AVS2/AVS3, LCEVC, game-music and tracker-module files).
#
# LGPL, shared: FFmpeg stays a set of separate libraries a user can swap, nothing GPL is enabled, and the source is
# the pinned release (common.sh's FFMPEG_SRC) with the patch above.

# fetch_verified and the FFmpeg source pin.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

# dav1d, pinned. The tarball's sha256 is the one VideoLAN publishes beside it (dav1d-1.5.4.tar.xz.sha256).
DAV1D=1.5.4
DAV1D_SHA=686616b7c69eb88d44459391ab25cac13b6647a3b288835c5784e71c1514a5c5
DAV1D_URL="https://downloads.videolan.org/pub/videolan/dav1d/$DAV1D/dav1d-$DAV1D.tar.xz"

# The patches applied to FFmpeg's source, in order.
DEC_PATCHES="$COMMON/patches/libdav1d-get-buffer.patch"

# build_dav1d <source tarball> <work dir> <prefix>: dav1d as a static library, installed under <prefix> with its
# pkg-config file, so FFmpeg links it into avcodec. Extra meson arguments follow.
build_dav1d() {
    local tarball=$1 work=$2 prefix=$3
    shift 3
    rm -rf "$work/dav1d"
    mkdir -p "$work/dav1d"
    tar -xJf "$tarball" -C "$work/dav1d" --strip-components=1
    meson setup "$work/dav1d/build" "$work/dav1d" --prefix="$prefix" --libdir=lib \
        --buildtype=release --default-library=static \
        -Denable_tools=false -Denable_tests=false -Denable_examples=false "$@"
    ninja -C "$work/dav1d/build"
    ninja -C "$work/dav1d/build" install
}

# unpack_ffmpeg <source tarball> <dir>: FFmpeg's source, patched.
unpack_ffmpeg() {
    local tarball=$1 dir=$2
    rm -rf "$dir"
    mkdir -p "$dir"
    tar -xJf "$tarball" -C "$dir" --strip-components=1
    for p in $DEC_PATCHES; do
        patch -p1 -d "$dir" < "$p"
    done
}

# configure_decoding <source dir> <prefix> <extra configure arguments...>: FFmpeg's own decoders, dav1d and zlib.
# The OS module adds its toolchain's flags.
configure_decoding() {
    local src=$1 prefix=$2
    shift 2
    (cd "$src" && ./configure --prefix="$prefix" \
        --enable-shared --disable-static \
        --disable-programs --disable-doc --disable-debug --disable-network --disable-autodetect \
        --disable-avdevice --disable-avfilter \
        --disable-encoders --disable-muxers --disable-filters --disable-devices \
        --disable-protocols --enable-protocol=file \
        --enable-zlib --enable-libdav1d \
        "$@")
}

# stage_dec_notes <ffmpeg source dir> <dav1d source dir> <dest>: FFmpeg's and dav1d's licences, and where both
# came from.
stage_dec_notes() {
    local src=$1 dav1d=$2 dest=$3
    cp "$src/COPYING.LGPLv2.1" "$dest/FFMPEG-LICENSE.txt"
    cp "$dav1d/COPYING" "$dest/DAV1D-LICENSE.txt"
    printf 'FFmpeg %s, LGPL shared build of its decoders, configured by ffmpeg-wrapper/native/decode.sh,\nwith ffmpeg-wrapper/native/patches applied.\nSource: %s\ndav1d %s (BSD-2-Clause), linked into avcodec.\nSource: %s\n' \
        "$FFMPEG_SRC" "$FFMPEG_SRC_URL" "$DAV1D" "$DAV1D_URL" > "$dest/FFMPEG-SOURCE.txt"
    cp $DEC_PATCHES "$dest/"
    echo "staged: $dest ($(wc -l < "$dest/libs.txt") libraries, $(du -sh "$dest" | cut -f1))"
}
