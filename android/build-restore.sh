#!/usr/bin/env bash
#
# Builds the installer's libraries for Android arm64 into
# build/android-arm64-v8a/libhengerestore.a:
#
#   libplist, libimobiledevice-glue, libusbmuxd, libimobiledevice, libtatsu,
#   libirecovery, usbmuxd, idevicerestore   (all third_party, vendored sources)
#   libzip                                  (cmake, needs zlib from the NDK)
#   mbedtls' TLS half                       (libimobiledevice compiles against it;
#                                            a restore never opens a TLS connection)
#
# This mirrors tools/build_ios_restore.sh, which is the same sources for iOS. The
# differences are all Android's, and each one is a line in here rather than a
# change to a vendored file where it can be helped:
#
#   - HAVE_MBEDTLS instead of HAVE_GNUTLS: what the iOS build passes too.
#   - libplist's `node.h` lives in libcnary/include, which its Makefile adds.
#   - libzip's zipconf.h comes from its cmake build, so libzip is built by cmake
#     with the NDK toolchain file instead of by this script.
#   - The USB link is host/libusb_shim.c, as on iOS: the guest's virtio USB port
#     is a real USB device as far as these libraries are concerned.
#
# Not built here: libcurl. The restore stack wants it for Apple's signing
# requests and for downloads, and Android has no libcurl. Rather than build curl
# and a TLS library, the curl subset it uses is implemented over the app's own
# HTTPS (android/curl_jni.c over Http.java), exactly as iOS implements it over
# NSURLSession (host/curl_shim.m).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="$ROOT/third_party"
ABI="${ABI:-arm64-v8a}"
API="${API:-33}"
BUILD="${BUILD:-$ROOT/build/android-$ABI/restore}"

# shellcheck source=/dev/null
set +u   # env.sh appends to variables it does not define itself
source "$ROOT/android/env.sh" >/dev/null
set -u

OUT="$BUILD/obj"
DEPS="$BUILD/deps"
mkdir -p "$OUT" "$DEPS"

COMMON=(-O2 -w -fPIC -DHAVE_CONFIG_H
        -I"$TP/restore_config" -I"$TP" -I"$TP/libusb-1.0" -I"$TP/curl_include"
        -I"$TP/libplist/include" -I"$TP/libplist/libcnary/include"
        -I"$TP/libimobiledevice-glue/include" -I"$TP/libusbmuxd/include"
        -I"$TP/libimobiledevice/include" -I"$TP/libimobiledevice"
        -I"$TP/libtatsu/include" -I"$TP/libirecovery/include"
        -I"$TP/libzip/lib" -I"$DEPS/libzip" -I"$TP/mbedtls/include")

OBJS=()
fail=0

compile() { # name extra-flags... -- files...
    local name=$1; shift
    local flags=()
    while [ "$1" != "--" ]; do flags+=("$1"); shift; done
    shift
    mkdir -p "$OUT/$name"
    for f in "$@"; do
        local o="$OUT/$name/$(basename "${f%.*}").o"
        local d; d="$(dirname "$f")"
        if ! "$CC" "${COMMON[@]}" -I"$d" "${flags[@]}" -c "$f" -o "$o"; then
            echo "FAILED: $f" >&2
            fail=$((fail + 1))
        else
            OBJS+=("$o")
        fi
    done
}

echo "== libzip (cmake) =="
if [ ! -f "$DEPS/libzip/lib/libzip.a" ]; then
    cmake -S "$TP/libzip" -B "$DEPS/libzip" \
        -DCMAKE_TOOLCHAIN_FILE="${ANDROID_NDK_HOME}/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI="$ABI" -DANDROID_PLATFORM="android-$API" \
        -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF \
        -DENABLE_OPENSSL=OFF -DENABLE_WINDOWS_CRYPTO=OFF \
        -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF \
        -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF \
        -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF -DLIBZIP_DO_INSTALL=OFF >/dev/null
    cmake --build "$DEPS/libzip" -j"$(nproc)" >/dev/null
fi

echo "== libplist =="
compile plist -DPACKAGE_VERSION='"2.6.0-henge"' \
    -- "$TP"/libplist/src/*.c "$TP"/libplist/libcnary/*.c

echo "== libimobiledevice-glue =="
# HAVE_NET_IF_DL_H comes from the shared config.h (written for Apple's BSD headers);
# Android has no <net/if_dl.h>, and its AF_PACKET path is the one that compiles.
compile glue -DPACKAGE_VERSION='"1.0.0-henge"' -UHAVE_NET_IF_DL_H -DHAVE_NETPACKET_PACKET_H \
    -- "$TP"/libimobiledevice-glue/src/*.c

echo "== libusbmuxd =="
compile usbmuxd -DPACKAGE_VERSION='"2.0.0-henge"' -- "$TP"/libusbmuxd/src/*.c

echo "== libimobiledevice (HAVE_MBEDTLS) =="
compile imd -DHAVE_MBEDTLS -DPACKAGE_VERSION='"1.4.0-henge"' \
    -DPACKAGE_STRING='"libimobiledevice"' \
    -I"$TP/libimobiledevice/src" -I"$TP/libimobiledevice/common" \
    -- "$TP"/libimobiledevice/src/*.c "$TP"/libimobiledevice/common/*.c

echo "== libtatsu =="
compile tatsu -DPACKAGE_VERSION='"1.0.0-henge"' -- "$TP"/libtatsu/src/*.c

echo "== libirecovery =="
compile irecovery -DPACKAGE_VERSION='"1.0.0-henge"' -- "$TP"/libirecovery/src/*.c

echo "== usbmuxd =="
compile usbmuxd_daemon -DPACKAGE_NAME='"usbmuxd"' -DPACKAGE_VERSION='"1.1.1-henge"' \
    -Dmain=usbmuxd_main -Dlog_level=usbmuxd_log_level \
    -DHAVE_PPOLL=1 \
    -- "$TP"/usbmuxd/src/*.c

echo "== idevicerestore =="
compile idevicerestore -DIDEVICERESTORE_NOMAIN -DPACKAGE_NAME='"idevicerestore"' \
    -DPACKAGE_VERSION='"1.0.0-henge"' -I"$TP/idevicerestore/src" \
    -- "$TP"/idevicerestore/src/*.c

# mbedtls: libimobiledevice is compiled against it (HAVE_MBEDTLS), so its calls into mbedtls must
# resolve even though a restore never opens a TLS connection — the handshake code is reachable but
# never reached. The whole library is compiled, as the iOS build does, rather than stubbing
# individual functions: a stub that is actually called is a crash in the middle of an install.
echo "== mbedtls (for libimobiledevice's TLS half) =="
compile mbedtls -I"$TP/mbedtls/library" -I"$TP/mbedtls/include" -- "$TP"/mbedtls/library/*.c

echo "== the host's side: USB and install driver =="
compile host -I"$ROOT/devices" -I"$ROOT/host" -I"$ROOT/core" -I"$ROOT/android/compat/include" \
    -- "$ROOT"/host/usb_host.c "$ROOT"/host/libusb_shim.c "$ROOT"/host/install.c

if [ "$fail" -ne 0 ]; then
    echo "FAILED: $fail translation unit(s)" >&2
    exit 1
fi

echo "== archive =="
mkdir -p "$ROOT/build/android-$ABI/lib"
rm -f "$ROOT/build/android-$ABI/lib/libhengerestore.a"
"$AR" rcs "$ROOT/build/android-$ABI/lib/libhengerestore.a" "${OBJS[@]}"
ls -lh "$ROOT/build/android-$ABI/lib/libhengerestore.a"
