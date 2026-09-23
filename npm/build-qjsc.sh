#!/bin/sh
# Builds qjsc-kmp for one npm platform package: npm/build-qjsc.sh <platform> <out-dir>
set -eu
platform=$1
out=$2
root=$(cd "$(dirname "$0")/.." && pwd)
build="$root/build/qjsc/$platform"
case $platform in
    darwin-arm64) extra="-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0" ;;
    darwin-x64) extra="-DCMAKE_OSX_ARCHITECTURES=x86_64 -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0" ;;
    # built against musl and fully static, so one binary runs on every distribution, glibc or musl
    linux-x64 | linux-arm64) extra="-DCMAKE_EXE_LINKER_FLAGS=-static" ;;
    *) echo "unknown platform: $platform" >&2; exit 2 ;;
esac
cmake -S "$root/native" -B "$build" -DCMAKE_BUILD_TYPE=Release -DQJS_TOOLS=ON $extra
cmake --build "$build" --target qjsc-kmp --parallel
mkdir -p "$out"
cp "$build/qjsc-kmp" "$out/qjsc-kmp"
strip "$out/qjsc-kmp"
# strip invalidates the linker's ad-hoc signature, and arm64 macOS kills unsigned binaries
case $platform in darwin-*) codesign --force --sign - "$out/qjsc-kmp" ;; esac
