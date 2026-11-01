#!/usr/bin/env bash
# Checks tools/gameptr's pass plugin on arm64 macOS: builds the plugin and
# test.cpp with it, and runs the test (docs/MACOS-ARM64.md).
#   tools/gameptr/test.sh [path/to/clang++]
# The compiler defaults to Homebrew LLVM's; llvm-config must be beside it.
set -euo pipefail
cd "$(dirname "$0")"
[[ "$(uname -s)-$(uname -m)" == Darwin-arm64 ]] || { echo "test.sh runs on arm64 macOS only" >&2; exit 1; }
cxx=${1:-$(brew --prefix llvm 2>/dev/null)/bin/clang++}
llvm_config="$(dirname "$cxx")/llvm-config"
[[ -x "$cxx" && -x "$llvm_config" ]] || { echo "need clang++ with llvm-config beside it (brew install llvm)" >&2; exit 1; }
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
# --cxxflags names the include folder unquoted (it may hold spaces): pass it whole.
include=$("$llvm_config" --includedir)
cxxflags=$("$llvm_config" --cxxflags)
cxxflags=${cxxflags//"-I$include"/}
# shellcheck disable=SC2086
"$cxx" -shared -fPIC -O2 $cxxflags "-I$include" -undefined dynamic_lookup -o "$out/plugin.dylib" GamePtrPass.cpp
"$cxx" -O2 -arch arm64 -ffp-contract=off -Wno-int-to-pointer-cast -fpass-plugin="$out/plugin.dylib" -o "$out/test" test.cpp
"$out/test"
