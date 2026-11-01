#!/usr/bin/env bash
# tools/check-version.sh TAG: does the SDK say the version TAG names?
#   vX.Y.Z         a release: SMS_MOD_API_MAJOR X, _MINOR Y, _PRERELEASE 0
#   vX.Y.0-beta.N  a pre-release of X.Y.0: _PRERELEASE N
# and the lua mod (lua/mod.ini), CMake's project version and CHANGELOG.md
# agree. CI runs it before releasing a tag; run it before pushing one.
set -euo pipefail
cd "$(dirname "$0")/.."

tag=${1:?usage: tools/check-version.sh vX.Y.Z or vX.Y.0-beta.N}
if [[ $tag =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]]; then
  pre=0
elif [[ $tag =~ ^v([0-9]+)\.([0-9]+)\.(0)-beta\.([1-9][0-9]*)$ ]]; then
  pre=${BASH_REMATCH[4]}
else
  echo "check-version: $tag is neither vX.Y.Z nor vX.Y.0-beta.N" >&2
  exit 1
fi
major=${BASH_REMATCH[1]} minor=${BASH_REMATCH[2]} patch=${BASH_REMATCH[3]}
version=${tag#v}

failed=0
want() { # what, found, expected
  if [[ "$2" != "$3" ]]; then
    echo "check-version: $1 is '$2', $tag needs '$3'" >&2
    failed=1
  fi
}
define() { sed -n "s/^#define $1 \([0-9]*\).*/\1/p" include/sms_mod.h; }
want SMS_MOD_API_MAJOR "$(define SMS_MOD_API_MAJOR)" "$major"
want SMS_MOD_API_MINOR "$(define SMS_MOD_API_MINOR)" "$minor"
want SMS_MOD_API_PRERELEASE "$(define SMS_MOD_API_PRERELEASE)" "$pre"
want "lua/mod.ini version" "$(sed -n 's/^version = //p' lua/mod.ini)" "$version"
want "CMake's project version" "$(sed -n 's/^project(sms_mod_sdk VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)" "$major.$minor.$patch"
heading="## $version"
((pre)) && heading="### $version"
grep -qxF "$heading" CHANGELOG.md || { echo "check-version: CHANGELOG.md has no '$heading' section" >&2; failed=1; }

((failed)) && exit 1
if ((pre)); then
  echo "check-version: $tag is API $major.$minor beta $pre"
else
  echo "check-version: $tag is API $major.$minor"
fi
