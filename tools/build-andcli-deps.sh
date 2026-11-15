#!/bin/sh
# Rebuilds the two Android static libraries that tools/build-install-android.sh links:
#
#   work/andcli/ossl/lib/libssl.a, libcrypto.a   (OpenSSL 3.0.15, bionic, no-shared)
#   work/andcli/curl/lib/libcurl.a               (curl 8.23.0, static, --with-openssl)
#
# Both are built with the NDK's aarch64-linux-android26 toolchain, because the host's glibc
# archives do not link against bionic (__errno, __FD_SET_chk are undefined). The parameters are
# the ones that produced the working pair before; this script exists so the recipe is not lost
# when work/ is cleaned.
#
#   tools/build-andcli-deps.sh          # both, skipping whichever is already built
#
set -eu
cd "$(dirname "$0")/.."
ROOT=$PWD
NDK=${ANDROID_NDK_HOME:-/home/he/Android/Sdk/ndk/29.0.14206865}
export ANDROID_NDK_ROOT=$NDK
export PATH=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin:$PATH

CURL_VER=8.23.0
mkdir -p work/andcli
cd work/andcli

echo "== openssl =="
if [ -f ossl/lib/libssl.a ] && [ -f ossl/lib/libcrypto.a ]; then
    echo "already built"
else
    [ -d ossl-src ] || cp -a /home/he/build-openssh/src/openssl-3.0.15 ossl-src
    (
        cd ossl-src
        make distclean >/dev/null 2>&1 || true
        rm -f Makefile configdata.pm
        ./Configure android-arm64 -D__ANDROID_API__=26 no-shared no-tests \
            --prefix="$ROOT/work/andcli/ossl" --openssldir=/system/etc/security/cacerts
        make -j8 build_sw
        make install_sw
    )
fi
ls -l ossl/lib/libssl.a ossl/lib/libcrypto.a

echo "== curl =="
if [ -f curl/lib/libcurl.a ]; then
    echo "already built"
else
    if [ ! -d "curl-$CURL_VER" ]; then
        curl -sfLO "https://curl.se/download/curl-$CURL_VER.tar.xz" ||
            curl -sfLO "https://github.com/curl/curl/releases/download/curl-8_23_0/curl-$CURL_VER.tar.xz"
        tar xf "curl-$CURL_VER.tar.xz"
    fi
    (
        cd "curl-$CURL_VER"
        # OpenSSL's configure probe needs the extra libs spelled out for the cross build.
        LIBS="-ldl" ./configure --host=aarch64-linux-android26 \
            --prefix="$ROOT/work/andcli/curl" \
            --disable-shared --enable-static --disable-ldap --disable-ldaps --disable-manual \
            --disable-ares --disable-debug \
            --without-libidn2 --without-brotli --without-libpsl --without-librtmp \
            --without-libssh2 --without-nghttp2 --without-zstd --without-zlib \
            --with-openssl="$ROOT/work/andcli/ossl" \
            --with-ca-bundle=/system/etc/security/cacerts ac_cv_func_getpwuid=yes
        make -j8
        make install
    )
fi
ls -l curl/lib/libcurl.a

echo "== done: point tools/build-install-android.sh at these =="
echo "  $ROOT/work/andcli/curl/lib/libcurl.a"
echo "  $ROOT/work/andcli/ossl/lib/libssl.a"
