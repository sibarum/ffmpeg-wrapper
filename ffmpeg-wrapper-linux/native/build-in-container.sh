#!/usr/bin/env bash
# The release build of the Linux natives: build.sh, run on AlmaLinux 8 in a container, so the libraries need no
# glibc newer than 2.28 and run on any distribution from 2019 on. The repository is mounted into the container and
# native/dist/<platform> is staged into it as build.sh would, owned by whoever ran this.
#
#   ./native/build-in-container.sh             (podman, or docker when there is no podman; ENGINE= picks one)
#
# Builds for the machine's own architecture (x86_64 or aarch64). The image is pinned by its tag; its packages are
# what AlmaLinux 8 ships (gcc 8, meson, nasm 2.15), which the pinned FFmpeg and dav1d build with.
#
# NOT YET RUN: written beside the Windows build, which is; the first run may need its packages corrected.
set -euo pipefail
cd "$(dirname "$0")"

IMAGE=docker.io/library/almalinux:8
ENGINE=${ENGINE:-$(command -v podman || command -v docker || true)}
[ -n "$ENGINE" ] || { echo "neither podman nor docker found: install one, or run build.sh on a machine as old" >&2; exit 1; }

REPO=$(cd ../.. && pwd)

# :Z relabels the mount for SELinux hosts (Fedora, RHEL), and is ignored elsewhere.
"$ENGINE" run --rm -v "$REPO:/src:Z" -w /src/ffmpeg-wrapper-linux -e HOST_IDS="$(id -u):$(id -g)" "$IMAGE" bash -euo pipefail -c '
    dnf -y -q install dnf-plugins-core
    dnf config-manager --set-enabled powertools
    dnf -y -q install gcc make meson ninja-build pkgconf-pkg-config patch curl xz zlib-devel diffutils binutils
    if [ "$(uname -m)" = x86_64 ]; then dnf -y -q install nasm; fi
    status=0
    ./native/build.sh || status=$?
    chown -R "$HOST_IDS" native/dist native/build native/ffmpeg ../native/miniaudio 2>/dev/null || true
    exit $status
'
