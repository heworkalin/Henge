#!/usr/bin/env bash
#
# Build libhenge.so for Android arm64-v8a.
#
#   ./android/build-c.sh              # Release, arm64-v8a, API 33
#   ABI=armeabi-v7a API=24 ./android/build-c.sh
#   BUILD_TYPE=Debug ./android/build-c.sh
#
# Requires ANDROID_NDK_HOME (or ANDROID_NDK_ROOT) to point at an NDK r26+.
# Verified with NDK 29.0.14206865 on an aarch64 Linux host: the NDK ships a
# native linux-arm64 toolchain, so no x86_64 emulation is involved.
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "$NDK" || ! -d "$NDK" ]]; then
    echo "error: ANDROID_NDK_HOME is not set or does not exist" >&2
    exit 1
fi
if [[ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]]; then
    echo "error: $NDK does not look like an NDK (no build/cmake/android.toolchain.cmake)" >&2
    exit 1
fi

ABI="${ABI:-arm64-v8a}"
API="${API:-33}"
BUILD_TYPE="${BUILD_TYPE:-Release}"
build_dir="$root/build/android-$ABI"

echo "NDK       $NDK"
echo "host tag  $(basename "$(ls -d "$NDK"/toolchains/llvm/prebuilt/* | head -1)")"
echo "target    $ABI / android-$API / $BUILD_TYPE"
echo "build dir $build_dir"

cmake -S "$here" -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM="android-$API" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE"

cmake --build "$build_dir" -j "$(nproc)"

echo
echo "=== artifact ==="
so="$build_dir/lib/$ABI/libhenge.so"
ls -la "$so"
file "$so"
echo "--- NEEDED ---"
readelf -d "$so" | grep NEEDED
