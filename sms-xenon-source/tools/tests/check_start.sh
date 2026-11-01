#!/usr/bin/env bash
# Starts a built game with an invalid disc image and checks that it gets as far
# as the disc check, and that Super Mario Eclipse's code mods are linked in
# exactly when they should be (platform/mods/modhooks.cpp finds their
# constructors). Used by .github/workflows/native-build.yml.
#   tools/tests/check_start.sh <executable> <eclipse: 0 or 1>
set -euo pipefail
exe=$1
eclipse=$2
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
head -c 32 /dev/zero > "$dir/invalid.iso"
limit=$(command -v timeout || command -v gtimeout || true)
status=0
SMS_CODE_MODS=0 SMS_HEADLESS=1 SMS_SAVE_DIR="$dir/saves" SMS_DISC_IMAGE="$dir/invalid.iso" \
  ${limit:+$limit 60} "$exe" "$dir/invalid.iso" > "$dir/start.log" 2>&1 || status=$?
cat "$dir/start.log"
echo "exit status $status"
if [[ "$eclipse" == 1 ]]; then
  grep -q 'code mods linked in but switched off' "$dir/start.log" || { echo "::error::$exe: Eclipse's code mods are not linked in"; exit 1; }
else
  ! grep -q 'code mods linked in' "$dir/start.log" || { echo "::error::$exe: code mods linked into a build without Eclipse"; exit 1; }
fi
grep -q 'not a usable GameCube disc image' "$dir/start.log" || { echo "::error::$exe did not reach the disc check"; exit 1; }
