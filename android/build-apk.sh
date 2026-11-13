#!/usr/bin/env bash
#
# Build the APK: native library first, then the app.
#
#   ./android/build-apk.sh                    # debug APK, targetSdk 36
#   TARGET_SDK=28 ./android/build-apk.sh      # the legacy-domain variant
#   ./android/build-apk.sh --clean            # from scratch
#
# The native library is built by build-c.sh, not by Gradle: $ANDROID_HOME's CMake
# is an x86-64 binary and cannot run on this aarch64 host, which is why the app
# module consumes build/android-arm64-v8a/lib through jniLibs.srcDirs instead of
# using externalNativeBuild. See gradle.properties for the longer note.
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"

clean=0
for arg in "$@"; do
    case "$arg" in
        --clean) clean=1 ;;
        *) echo "unknown argument: $arg" >&2; exit 2 ;;
    esac
done

if [[ $clean -eq 1 ]]; then
    echo "== clean =="
    rm -rf "$root/build/android-arm64-v8a" "$here/app/build" "$here/build"
fi

echo "== 1/2 native library =="
"$here/build-c.sh"

echo
echo "== 2/2 apk =="
cd "$here"
# Always start the app build clean.
#
# libhenge.so lives outside the module (jniLibs.srcDirs points at
# build/android-arm64-v8a/lib), and AGP's incremental packaging proved unreliable
# about a native library that changes under it: one run produced a 634 KB APK
# holding the same six entries as the 318 KB one, with the .so present once. It
# did not reproduce on demand, so rather than chase it, the app build directory is
# dropped every time. ~30 s, and the artefact is always the size it should be.
rm -rf "$here/app/build"

gradle_args=(:app:assembleDebug)
if [[ -n "${TARGET_SDK:-}" ]]; then
    gradle_args+=("-PtargetSdk=$TARGET_SDK")
    echo "   targetSdk=$TARGET_SDK"
fi
./gradlew "${gradle_args[@]}"

apk="$here/app/build/outputs/apk/debug/app-debug.apk"
echo
echo "=== artifact ==="
ls -la "$apk"
echo "--- contents ---"
unzip -l "$apk" | grep -E "lib/|classes|AndroidManifest"
echo "--- signature ---"
"$ANDROID_HOME/build-tools/36.1.0/apksigner" verify --print-certs "$apk" 2>/dev/null | head -2
echo
echo "Install with:"
echo "  adb install -r $apk"
echo "Then read the probe:"
echo "  adb logcat -s HENGEPROBE"
