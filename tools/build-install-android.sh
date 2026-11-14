#!/bin/sh
# Builds the installer as an Android command-line binary, to be pushed to /data/local/tmp.
#
# android/install.c already contains the whole installer; the app reaches it through JNI and
# tools/host-install.c reaches it through main(). This build is the second one, but against
# bionic instead of glibc: same libc, same pthreads, same page size as the phone, so a timing
# bug reproduces here and not only in the APK -- and it runs without the proot layer that sits
# under the host build, which changes how threads and signals behave.
#
#   tools/build-install-android.sh
#   adb push build/andcli/host-install /data/local/tmp/henge/
#
# Needs the Android curl and OpenSSL built by hand (see work/andcli/README if present):
#   work/andcli/curl/lib/libcurl.a     (static, --with-openssl, no other deps)
#   work/andcli/ossl/lib/libssl.a
#   work/andcli/ossl/lib/libcrypto.a
set -eu
cd "$(dirname "$0")/.."

NDK=${ANDROID_NDK_HOME:-/home/he/Android/Sdk/ndk/29.0.14206865}
: "${API:=26}"
BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
: "${CC:=$BIN/aarch64-linux-android$API-clang}"

OUT=${OUT:-build/andcli}
AND=${AND:-build/android-arm64-v8a/lib}
RESTORE=${RESTORE:-$AND/libhengerestore.a}
CURL=$PWD/work/andcli/curl
OSSL=$PWD/work/andcli/ossl
LIBZIP=${LIBZIP:-$AND/../restore/deps/libzip/lib/libzip.a}

for f in "$RESTORE" "$CURL/lib/libcurl.a" "$OSSL/lib/libssl.a" "$OSSL/lib/libcrypto.a"; do
    [ -f "$f" ] || { echo "build-install-android.sh: missing $f" >&2; exit 1; }
done

INC="-Icore -Ihost -Idevices -Iandroid/compat/include -Ithird_party/mbedtls/include -I$CURL/include -I$OSSL/include"
DEFS="-D_GNU_SOURCE -D__ANDROID__ -include android/compat/include/hv_darwin_compat.h"
CFLAGS="-std=gnu11 -O2 -g -w --target=aarch64-linux-android$API $INC $DEFS"
mkdir -p "$OUT/obj"

# An object is stale when its source *or* any header is newer: the engine's layouts are shared
# through headers, and a half-rebuilt tree is a crash at boot rather than a compile error.
stale() {
    [ -f "$1" ] || return 0
    [ "$1" -nt "$2" ] || return 0
    [ -n "$(find core host devices android/compat/include \
                 -name '*.h' -newer "$1" -print -quit 2>/dev/null)" ]
}

build() {
    for src in "$@"; do
        obj="$OUT/obj/$(echo "${src%.c}" | tr '/' '_').o"
        if stale "$obj" "$src"; then $CC $CFLAGS -c "$src" -o "$obj"; fi
    done
}

build core/*.c
build devices/*.c
build host/disk.c host/hvsparse.c host/jit_region.c host/usb_host.c
build android/compat/compat_android.c android/compat/commoncrypto_android.c \
      android/compat/corefoundation_android.c android/compat/pvg_metal_stub.c
build third_party/mbedtls/library/aes.c third_party/mbedtls/library/sha256.c \
      third_party/mbedtls/library/platform_util.c third_party/mbedtls/library/constant_time.c
build android/install.c
if stale "$OUT/obj/host_enter.o" host/enter.S; then
    $CC -D_GNU_SOURCE -D__ANDROID__ -Ihost -Icore --target=aarch64-linux-android$API \
        -c host/enter.S -o "$OUT/obj/host_enter.o"
fi
# bionic has had pthread_gettid_np since API 31, and this build targets 28 or newer, so the
# shim the glibc host build needs has no place here.
rm -f "$OUT/obj/tidshim.o" "$OUT/obj/tidshim.c"

objs=$(ls "$OUT"/obj/*.o)
$CC $CFLAGS tools/host-install.c $objs "$RESTORE" \
    "$CURL/lib/libcurl.a" "$OSSL/lib/libssl.a" "$OSSL/lib/libcrypto.a" \
    ${LIBZIP:+$LIBZIP} -o "$OUT/host-install" \
    -lm -ldl -llog ${ZLIB:+-lz} 2>&1 | tail -25
ls -lh "$OUT/host-install"
