#!/bin/sh
# Deterministic headless port run with a .dtm: portrun.sh WORKDIR MOVIE.dtm FIELDS [SECONDS]
# FIELDS: comma list for SMS_SHOTS. Fresh memory card each run.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
W=$1; M=$2; F=$3; T=${4:-150}
rm -rf "$W"; mkdir -p "$W/save" "$W/shots"
cd "$W"
SMS_SAVE_DIR="$W/save" SMS_SETTINGS=/dev/null SMS_TEXTURE_PACKS=0 SMS_HEADLESS=1 SMS_AUDIO=0 \
SMS_VI_DETERMINISTIC=1 SMS_TRACE_SYNC=${SYNC:-+0} SMS_MOVIE="$M" SMS_SHOTS="$F" SMS_SHOT_DIR="$W/shots" \
timeout -s KILL "$T" "$HERE/../../build/linux-32/sms" \
    "$HOME/src/xbox-dev/sms-xenon/data/GMSE01.iso" > "$W/run.log" 2>&1 || true
cd "$W/shots" && for f in *.ppm; do magick "$f" "${f%.ppm}.png" && rm "$f"; done
