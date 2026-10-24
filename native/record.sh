# What every record OS module's native/build.sh shares: the pinned FFmpeg and SVT-AV1 sources, and how FFmpeg is
# configured to hold the AV1 encoders and nothing else. Sourced, never run; the OS modules' scripts are what run.
#
# The decoding build (common.sh) is BtbN's prebuilt FFmpeg: every decoder, and 130 MB on Windows. An application
# that only records needs none of it, so recording has a build of its own, from source: FFmpeg configured with
# --disable-everything, then the four AV1 encoders, the Matroska and WebM muxers and the file protocol put back.
# SVT-AV1 is built from its own pinned source and linked into avcodec statically; the GPU encoders are the
# vendors' drivers, opened at run time, so they cost a few headers.
#
# Its libraries carry a -rec suffix (--build-suffix), so an application that plays and records loads both builds
# side by side, each shim bound to its own avcodec.
#
# LGPL, shared, as the decoding build is: FFmpeg stays a set of separate libraries a user can swap, nothing GPL is
# enabled, and the source is the pinned release below.

# fetch_verified, from the decoding build's script.
. "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

# The sources, pinned. FFmpeg's release tarball is the one its signature (ffmpeg-devel.asc) was checked against
# when this was pinned; the decoding build's BtbN autobuild is of the same release.
REC_FFMPEG=8.1.3
REC_FFMPEG_SHA=7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3
REC_FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-$REC_FFMPEG.tar.xz"
REC_SVT=4.2.0
REC_SVT_SHA=c7b13c4a84bd3751aa35fcc72be13e6875467e7c2216879251a486e5b1e4e740
REC_SVT_URL="https://gitlab.com/AOMediaCodec/SVT-AV1/-/archive/v$REC_SVT/SVT-AV1-v$REC_SVT.tar.gz"

# The libraries' suffix, and the libraries the shim links, each after the ones it needs.
REC_SUFFIX=-rec
REC_LIBS="avutil swscale avcodec avformat"

# This folder: the shim's source (record/).
REC_COMMON="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# build_svt <source tarball> <work dir> <prefix>: SVT-AV1's encoder as a static library, installed under <prefix>
# with its pkg-config file. Extra cmake arguments follow.
build_svt() {
    local tarball=$1 work=$2 prefix=$3
    shift 3
    rm -rf "$work/svt"
    mkdir -p "$work/svt"
    tar -xzf "$tarball" -C "$work/svt" --strip-components=1
    cmake -S "$work/svt" -B "$work/svt/build" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" -DBUILD_SHARED_LIBS=OFF \
        -DBUILD_APPS=OFF -DBUILD_TESTING=OFF -DSVT_AV1_LTO=OFF "$@"
    cmake --build "$work/svt/build"
    cmake --install "$work/svt/build"
}

# configure_ffmpeg <source dir> <prefix> <extra configure arguments...>: FFmpeg with the AV1 encoders and nothing
# else. The OS module adds the GPU encoders' switches its platform has, and its toolchain's flags.
configure_ffmpeg() {
    local src=$1 prefix=$2
    shift 2
    (cd "$src" && ./configure --prefix="$prefix" \
        --enable-shared --disable-static --build-suffix="$REC_SUFFIX" \
        --disable-programs --disable-doc --disable-debug --disable-network --disable-autodetect \
        --disable-everything --disable-avdevice --disable-avfilter --disable-swresample \
        --enable-libsvtav1 --enable-encoder=libsvtav1 \
        --enable-muxer=matroska,webm --enable-protocol=file \
        "$@")
}

# stage_rec_notes <ffmpeg source dir> <dest>: FFmpeg's and SVT-AV1's licences, and where both came from.
stage_rec_notes() {
    local src=$1 svt=$2 dest=$3
    cp "$src/COPYING.LGPLv2.1" "$dest/FFMPEG-LICENSE.txt"
    cp "$svt/LICENSE.md" "$dest/SVT-AV1-LICENSE.txt"
    cp "$svt/PATENTS.md" "$dest/SVT-AV1-PATENTS.txt"
    printf 'FFmpeg %s, LGPL shared build of the AV1 encoders only, configured by ffmpeg-wrapper/native/record.sh.\nSource: %s\nSVT-AV1 %s (BSD-3-Clause-Clear), linked into avcodec.\nSource: %s\n' \
        "$REC_FFMPEG" "$REC_FFMPEG_URL" "$REC_SVT" "$REC_SVT_URL" > "$dest/FFMPEG-SOURCE.txt"
    echo "staged: $dest ($(wc -l < "$dest/libs.txt") libraries, $(du -sh "$dest" | cut -f1))"
}
