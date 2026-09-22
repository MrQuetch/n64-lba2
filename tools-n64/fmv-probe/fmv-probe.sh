#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Copyright (C) 2026 Samuele Voltan
#
# Builds fmvprobe.z64: one or more of the retail Smacker movies, re-encoded for
# the N64 at several codec/quality settings, in a ROM that measures whether the
# console can play them (see fmvprobe.c and README.md).
#
# Bring your own assets, as everywhere else in this port: the movies are read
# from a local retail install and never land in the repository.
#
# Run it from this directory on the host; with no libdragon toolchain in the
# environment ($N64_INST) it re-executes itself inside the preview image built
# from Dockerfile.preview (see there for why it is a separate image).
#
# Usage:
#   ./fmv-probe.sh                 # INTRO, the default clip set
#   ./fmv-probe.sh END SORT        # other movies (names from the ACF list)
#   ./fmv-probe.sh list            # what is in VIDEO.HQR, with durations
#   ./fmv-probe.sh clean
#
# Environment overrides:
#   DOCKER    - docker binary (default: docker on PATH)
#   IMAGE     - image tag (default: n64lba-fmv-preview:latest)
#   GAMEDATA  - retail data dir, for RESS.HQR (default: ../../../../CommonGOG)
#   VIDEOHQR  - VIDEO.HQR path (default: $GAMEDATA/video/VIDEO.HQR; the GOG
#               install keeps it in Common/VIDEO/, often left out of a BYOA copy)
#   CLIPS     - encodings to build, "codec:quality[:fps]" (default below).
#               MPEG-1 only accepts the standard frame rate codes, so a 15 fps
#               source must be forced to 24 or 25; H.264 takes it as it is.
#   VOICE     - Smacker audio track mixed under the music: 1=FR 2=DE 3=EN (default 3)
#   KEEP      - 1 to keep the intermediate .smk/.mkv/.wav in work/
set -e

GAMEDATA="${GAMEDATA:-../../../../CommonGOG}"
CLIPS="${CLIPS:-h264:55 h264:80 mpeg1:55:24}"
VOICE="${VOICE:-3}"

case "${1:-}" in
    clean)
        rm -rf build work filesystem fmvprobe.z64
        echo "[fmv-probe] cleaned"
        exit 0
        ;;
esac

if [ -z "$N64_INST" ]; then
    # --- host side: hand over to the preview container ---------------------
    DOCKER="${DOCKER:-docker}"
    IMAGE="${IMAGE:-n64lba-fmv-preview:latest}"
    export MSYS_NO_PATHCONV=1
    if ! "$DOCKER" image inspect "$IMAGE" >/dev/null 2>&1; then
        echo "[fmv-probe] building image $IMAGE (one-off, a few minutes)"
        "$DOCKER" build -t "$IMAGE" -f Dockerfile.preview .
    fi
    HERE="$(cd "$(pwd)" && pwd)"
    ROOT="$(cd "$HERE/../../../.." && pwd)"   # parent of the repo's parent
    REL="${HERE#$ROOT/}"
    EXTRA=""
    if [ -d "$GAMEDATA" ]; then
        GD="$(cd "$GAMEDATA" && pwd)"
        case "$GD" in
            "$ROOT"/*) GAMEDATA_IN="/project/${GD#$ROOT/}" ;;
            *) EXTRA="$EXTRA -v $GD:/gamedata"; GAMEDATA_IN="/gamedata" ;;
        esac
    else
        GAMEDATA_IN="$GAMEDATA"
    fi
    VIDEOHQR_IN="${VIDEOHQR:-$GAMEDATA_IN/video/VIDEO.HQR}"
    if [ -n "$VIDEOHQR" ] && [ -f "$VIDEOHQR" ]; then
        VD="$(cd "$(dirname "$VIDEOHQR")" && pwd)"
        case "$VD" in
            "$ROOT"/*) VIDEOHQR_IN="/project/${VD#$ROOT/}/$(basename "$VIDEOHQR")" ;;
            *) EXTRA="$EXTRA -v $VD:/videohqr"; VIDEOHQR_IN="/videohqr/$(basename "$VIDEOHQR")" ;;
        esac
    fi
    exec "$DOCKER" run --rm -v "$ROOT:/project" $EXTRA -w "/project/$REL" \
        -e "GAMEDATA=$GAMEDATA_IN" -e "VIDEOHQR=$VIDEOHQR_IN" \
        -e "CLIPS=$CLIPS" -e "VOICE=$VOICE" -e "KEEP=${KEEP:-}" \
        "$IMAGE" sh fmv-probe.sh "$@"
fi

# --- container side -------------------------------------------------------

VIDEOHQR="${VIDEOHQR:-$GAMEDATA/video/VIDEO.HQR}"
RESS="$GAMEDATA/RESS.HQR"
[ -f "$VIDEOHQR" ] || { echo "[fmv-probe] VIDEO.HQR not found at $VIDEOHQR"; exit 1; }
[ -f "$RESS" ] || { echo "[fmv-probe] RESS.HQR not found at $RESS"; exit 1; }

if [ "${1:-}" = "list" ]; then
    exec python3 video_hqr.py list "$VIDEOHQR" "$RESS"
fi

MOVIES="${*:-INTRO}"
mkdir -p work filesystem
rm -f filesystem/*

for movie in $MOVIES; do
    tag=$(echo "$movie" | tr 'A-Z' 'a-z' | cut -c1-6)
    smk="work/$tag.smk"
    [ -f "$smk" ] || python3 video_hqr.py extract "$VIDEOHQR" "$RESS" "$movie" "$smk"

    # videoconv64 takes one video input plus an optional separate audio file.
    # Split the Smacker accordingly: lossless video (so the encoder sees the
    # original pixels, not a second generation), and the audio the engine would
    # play — track 0 is the music, 1/2/3 are the FR/DE/EN voices.
    if [ ! -f "work/${tag}_v.mkv" ]; then
        echo "[fmv-probe] $movie: splitting video/audio"
        ffmpeg -v error -y -i "$smk" -an -c:v ffv1 -level 3 "work/${tag}_v.mkv"
        ffmpeg -v error -y -i "$smk" \
            -filter_complex "[0:a:0][0:a:$VOICE]amix=inputs=2:duration=longest:normalize=0" \
            -ac 1 -ar 32000 "work/${tag}_a.wav"
    fi

    for spec in $CLIPS; do
        codec=$(echo "$spec" | cut -d: -f1)
        quality=$(echo "$spec" | cut -d: -f2)
        fps=$(echo "$spec" | cut -d: -f3)
        fpsarg=""
        [ -n "$fps" ] && fpsarg="-r $fps"
        out="work/out_${tag}_${codec}${quality}"
        rm -rf "$out"; mkdir -p "$out"
        echo "[fmv-probe] $movie: encoding $codec q=$quality ${fps:+@${fps}fps}"
        videoconv64 -c "$codec" -w 320 -q "$quality" $fpsarg --no-progress \
            -o "$out" "work/${tag}_v.mkv" "work/${tag}_a.wav"
        # Stage under a name that says what it is; fmv_play finds the audio
        # track by matching basename.
        for f in "$out"/*; do
            ext="${f##*.}"
            cp "$f" "filesystem/${tag}_${codec}${quality}.$ext"
        done
    done
done

make

[ "$KEEP" = "1" ] || rm -rf work

echo
echo "[fmv-probe] clips in the ROM:"
ls -l filesystem | awk 'NR>1 {printf "  %-28s %8.2f MiB\n", $9, $5/1048576}'
echo "[fmv-probe] fmvprobe.z64 ready - run it and read the [fmvprobe] lines on ISViewer"
