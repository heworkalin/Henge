#!/bin/sh
# Builds the whole installer for the host: the engine, the devices, the Android compatibility layer
# and the restore stack, into build/host/host-install.
#
# The app's installer is android/install.c; this links the same file, so the code under test is the
# code the phone runs, with the paths coming from the command line instead of from an app.
#
#   tools/build-install-host.sh          (needs build/host/lib/libhengerestore.a, see
#                                         tools/build-restore-host.sh)
set -eu
cd "$(dirname "$0")/.."

: "${CC:=gcc}"
OUT=${OUT:-build/host}
RESTORE=$OUT/lib/libhengerestore.a
[ -f "$RESTORE" ] || { echo "build-install-host.sh: run tools/build-restore-host.sh first" >&2; exit 1; }

INC="-Icore -Ihost -Idevices -Iandroid/compat/include -Itools/host_include -Ithird_party/mbedtls/include"
DEFS="-D_GNU_SOURCE -D__ANDROID__ -include android/compat/include/hv_darwin_compat.h"
CFLAGS="-std=gnu11 -O2 -g -w $INC $DEFS"
mkdir -p "$OUT/obj"

# An object is stale when its source is newer *or* any header is: the engine's structs are
# shared through headers, and a layout change that half the objects missed is a crash at boot,
# not a compile error. That cost an hour once; the find below is cheap enough.
stale() {
    [ -f "$1" ] || return 0
    [ "$1" -nt "$2" ] || return 0   # the object is not newer than its source: rebuild
    [ -n "$(find core host devices android/compat/include tools/host_include \
                 -name '*.h' -newer "$1" -print -quit 2>/dev/null)" ]
}

build() { # out-name source...
    name=$1; shift
    for src in "$@"; do
        # The name keeps the path: devices/aes.c and third_party/mbedtls/library/aes.c would
        # otherwise share one object, and the link picks whichever was written last.
        obj="$OUT/obj/$(echo "${src%.c}" | tr '/' '_').o"
        if stale "$obj" "$src"; then $CC $CFLAGS -c "$src" -o "$obj"; fi
    done
}

# glibc has no pthread_gettid_np; the one caller only logs it.
if [ ! -f "$OUT/obj/tidshim.o" ]; then
    printf 'int pthread_gettid_np(void *t) { (void)t; return 0; }\n' > "$OUT/obj/tidshim.c"
    $CC $CFLAGS -c "$OUT/obj/tidshim.c" -o "$OUT/obj/tidshim.o"
fi

build core core/*.c
build dev devices/*.c
build host host/disk.c host/hvsparse.c host/jit_region.c host/usb_host.c
build compat android/compat/compat_android.c android/compat/commoncrypto_android.c \
              android/compat/corefoundation_android.c android/compat/pvg_metal_stub.c
build mbed third_party/mbedtls/library/aes.c third_party/mbedtls/library/sha256.c \
            third_party/mbedtls/library/platform_util.c third_party/mbedtls/library/constant_time.c
build app android/install.c

if stale "$OUT/obj/host_enter.o" host/enter.S; then $CC -D_GNU_SOURCE -D__ANDROID__ -Ihost -Icore -c host/enter.S -o "$OUT/obj/host_enter.o"; fi

objs=$(ls "$OUT"/obj/*.o)

# pvg_lite.c is the device model the board always creates; it is in devices/*.c above.
$CC $CFLAGS tools/host-install.c $objs "$RESTORE" -o "$OUT/host-install" \
    -lpthread -lm -latomic -lz -lcurl "$OUT/restore/deps/libzip/lib/libzip.a" 2>&1 | tail -20
ls -lh "$OUT/host-install"
