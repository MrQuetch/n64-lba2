#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Samuele Voltan
#
# Wii U build wrapper — invokes the devkitpro/devkitppc Docker image with
# the devkitPPC + WUT toolchain, then packages the resulting .rpx into a
# .wuhb that Aroma can load directly from sd:/wiiu/apps/.
#
# Usage:
#   ./build-wiiu.sh                 # configure + build + package
#   ./build-wiiu.sh configure       # configure only
#   ./build-wiiu.sh clean           # rm -rf build-wiiu/
#   ./build-wiiu.sh shell           # interactive shell inside the container
#
# Notes:
#   - Container is started --rm; no persistent state, so DEVKITPRO env vars
#     come from the image's /etc/profile.d.
#   - On Git Bash for Windows, MSYS_NO_PATHCONV=1 prevents path mangling of
#     the volume mount.
#
# Environment overrides:
#   DOCKER       — docker binary (default: docker on PATH).
#   IMAGE        — Docker image tag (default: devkitpro/devkitppc:latest).
#   BUILDDIR     — out-of-source build directory (default: build-wiiu).

set -e

DOCKER="${DOCKER:-docker}"
IMAGE="${IMAGE:-devkitpro/devkitppc:latest}"
SRCDIR="$(pwd)"
BUILDDIR="${BUILDDIR:-build-wiiu}"
ACTION="${1:-build}"

case "$ACTION" in
    clean)
        rm -rf "$BUILDDIR"
        echo "Cleaned $BUILDDIR/"
        exit 0
        ;;
    shell)
        export MSYS_NO_PATHCONV=1
        exec "$DOCKER" run --rm -it -v "$SRCDIR:/src" "$IMAGE" bash
        ;;
    configure|build)
        ;;
    *)
        echo "Unknown action: $ACTION (expected: configure | build | clean | shell)"
        exit 1
        ;;
esac

export MSYS_NO_PATHCONV=1

# Assemble the .wuhb romfs content on the host (CommonGOG lives outside the
# /src mount). Ships the essential game data (~28 MB of .hqr/.ile/.obl — no
# VIDEO/VOX/music) plus a saves/ dir, so the engine can boot with no SD card
# (Cemu): the runtime falls back from /vol/external01/ to /vol/content/.
# cp -u keeps this incremental — first run copies, later runs are no-ops.
CONTENTDIR="$SRCDIR/wuhb-content"
GAMEDATA="$SRCDIR/../../CommonGOG"
if [ -d "$GAMEDATA" ]; then
    mkdir -p "$CONTENTDIR/data" "$CONTENTDIR/saves"
    # Top-level files only: CommonGOG now also holds music/ video/ vox/
    # subdirs (streamed from SD at runtime, deliberately not embedded), and a
    # bare `cp -u dir/*` exits 1 on them, killing the build under set -e.
    find "$GAMEDATA" -maxdepth 1 -type f -exec cp -u {} "$CONTENTDIR/data/" \;
    touch "$CONTENTDIR/saves/.keep"
else
    echo "WARNING: $GAMEDATA not found; building .wuhb without embedded data"
fi

CONFIGURE_CMD='cd /src && cmake -B '"$BUILDDIR"' \
    -DCMAKE_TOOLCHAIN_FILE=/src/cmake/wiiu-devkitpro.cmake \
    -DCMAKE_BUILD_TYPE=Release \
    -G "Unix Makefiles"'

BUILD_CMD='cd /src/'"$BUILDDIR"' && make -j8'

# wuhbtool wraps the .rpx with metadata (name shown in Wii U Menu / HBL).
# --content embeds wuhb-content/ as the romfs mounted at /vol/content.
PACKAGE_CMD='cd /src/'"$BUILDDIR"' && \
    if [ -f SOURCES/lba2.rpx ]; then \
        CONTENT_FLAG=""; \
        [ -d /src/wuhb-content/data ] && CONTENT_FLAG="--content=/src/wuhb-content"; \
        wuhbtool SOURCES/lba2.rpx lba2.wuhb \
            --name="LBA2 Wii U" \
            --short-name="LBA2" \
            --author="Samuele Voltan" \
            $CONTENT_FLAG && \
        echo "OK -> /src/'"$BUILDDIR"'/lba2.wuhb"; \
    else \
        echo "lba2.rpx not produced; skipping wuhbtool"; \
        ls -la SOURCES/ || true; \
    fi'

if [ "$ACTION" = "configure" ]; then
    SCRIPT="$CONFIGURE_CMD"
else
    SCRIPT="$CONFIGURE_CMD && $BUILD_CMD && $PACKAGE_CMD"
fi

exec "$DOCKER" run --rm \
    -v "$SRCDIR:/src" \
    "$IMAGE" \
    bash -c "$SCRIPT"
