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
# reims-vgpu: the paravirtual GPU as a host library. Built out of tree by cargo; the CLI links it
# so the GPU path can be exercised without the app (which is the only other place it exists today).
REIMS_DIR=${REIMS_DIR:-$PWD/android/third_party/reims-vgpu}
REIMS_LIB=$REIMS_DIR/target/aarch64-linux-android/release/libreims_vgpu_c.a
REIMS_INC=$REIMS_DIR/crates/reims-vgpu-c/include

for f in "$RESTORE" "$CURL/lib/libcurl.a" "$OSSL/lib/libssl.a" "$OSSL/lib/libcrypto.a"; do
    [ -f "$f" ] || { echo "build-install-android.sh: missing $f" >&2; exit 1; }
done

INC="-Icore -Ihost -Idevices -Iandroid -I$REIMS_INC -Iandroid/compat/include -Ithird_party/mbedtls/include -I$CURL/include -I$OSSL/include"
DEFS="-D_GNU_SOURCE -D__ANDROID__ -include android/compat/include/hv_darwin_compat.h"
# Same switch the app's CMake uses: devices/vmapple.c then hands a normal boot to reims-vgpu and
# keeps pvg_lite for installer boots.
REIMS_ARG=""
if [ -f "$REIMS_LIB" ]; then
    DEFS="$DEFS -DHENGE_HAVE_REIMS=1"
    REIMS_ARG=$REIMS_LIB
    echo "reims-vgpu: linking $REIMS_LIB"
else
    echo "reims-vgpu: not built (no GPU device model in this CLI)"
fi
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

# mmu.c: no SIMD. hv_tlb_refill runs straight out of translated code with the guest's SIMD
# registers live (guest v0-v31 ARE the host's, since SIMD instructions are passed through), and
# it is reached from the refill thunk, not through put_helper, so nothing else can save them. With
# SIMD enabled here a TLB miss wiped the guest's vector state: iBEC's LZFSE decoder keeps its FSE
# state in q0-q2 across a load or store that misses, lost it, and decoded the kernelcache into
# wrong bytes (a kalloc_type view's kt_flags read 0x2 instead of 0x71 -> panic in validate_flags).
# The Makefile has carried this flag for mmu.c for the same reason; the Android builds did not.
build_mmu() {
    obj="$OUT/obj/core_mmu.o"
    if stale "$obj" core/mmu.c; then $CC $CFLAGS -mgeneral-regs-only -c core/mmu.c -o "$obj"; fi
}
build_core() {
    for src in "$@"; do
        [ "$src" = "core/mmu.c" ] && continue
        obj="$OUT/obj/$(echo "${src%.c}" | tr '/' '_').o"
        if stale "$obj" "$src"; then $CC $CFLAGS -c "$src" -o "$obj"; fi
    done
}
build_mmu
build_core core/*.c
build devices/*.c
build host/disk.c host/hvsparse.c host/jit_region.c host/usb_host.c host/net_min.c
build android/compat/compat_android.c android/compat/commoncrypto_android.c \
      android/compat/corefoundation_android.c android/compat/pvg_metal_stub.c
build third_party/mbedtls/library/aes.c third_party/mbedtls/library/sha256.c \
      third_party/mbedtls/library/platform_util.c third_party/mbedtls/library/constant_time.c
build android/install.c
# The app's normal-boot entry (HV_BOOT_ONLY path in host-install.c): same engine, no restore OS.
build android/henge_vm.c
# HV_VK_DRIVER: libadrenotools' linker namespace, so the engine can open Turnip (see the file).
build android/vk_loader.c
[ -n "$REIMS_ARG" ] && build android/reims_pvg.c
if stale "$OUT/obj/host_enter.o" host/enter.S; then
    $CC -D_GNU_SOURCE -D__ANDROID__ -Ihost -Icore --target=aarch64-linux-android$API \
        -c host/enter.S -o "$OUT/obj/host_enter.o"
fi
# bionic has had pthread_gettid_np since API 31, and this build targets 28 or newer, so the
# shim the glibc host build needs has no place here.
rm -f "$OUT/obj/tidshim.o" "$OUT/obj/tidshim.c"

objs=$(ls "$OUT"/obj/*.o | grep -v '/tools_host_install\.o$')
# libadrenotools is C++: its static library needs libc++ and libc++abi, which this link takes
# statically (the CLI ships as one file). Only the HV_VK_DRIVER path calls into it.
ADR=${ADRENOTOOLS_LIB:-build/android-arm64-v8a/third_party/adrenotools}
ADR_LIBS=""
if [ -f "$ADR/libadrenotools.a" ]; then
    ADR_LIBS="$ADR/libadrenotools.a $ADR/lib/linkernsbypass/liblinkernsbypass.a"
    echo "adrenotools: linking $ADR/libadrenotools.a"
else
    echo "adrenotools: not built (HV_VK_DRIVER will report no driver)"
fi
# libadrenotools is C++ and needs its runtime: clang++ is what adds libc++/libc++abi to the link
# (clang does not for a C source, and the archive's `__gxx_personality_v0` stays undefined).
LINK=${CXX:-${CC}++}
LINK_FLAGS=$(echo "$CFLAGS" | sed 's/-std=gnu11//')
# main() as C: a clang++ compile would mangle the engine's C symbols it calls.
$CC $CFLAGS -c tools/host-install.c -o "$OUT/obj/tools_host_install.o"
objs="$objs $OUT/obj/tools_host_install.o"
$LINK $LINK_FLAGS $objs "$RESTORE" \
    "$CURL/lib/libcurl.a" "$OSSL/lib/libssl.a" "$OSSL/lib/libcrypto.a" \
    ${LIBZIP:+$LIBZIP} $REIMS_ARG $ADR_LIBS -o "$OUT/host-install" \
    -static-libstdc++ -lm -ldl -llog -landroid ${ZLIB:+-lz} 2>&1 | tail -25
ls -lh "$OUT/host-install"
