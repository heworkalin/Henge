#!/bin/sh
# Decodes the real kernel payload twice — natively and as guest code through the JIT — and compares.
#
# This is the local stand-in for "boot the firmware and see what its decompressor produces": the
# decoder is Apple's own LZFSE source, the payload is the one captured off the wire, and a
# disagreement between the two runs is the emulation fault this whole exercise is looking for.
#
#   ./android/test-lzfse.sh payload.lz [reference.bin]
#
# The captured payload and the image it should expand to are both kept from the USB capture; see
# docs/android-install-debug.md.
set -eu
cd "$(dirname "$0")/.."

: "${NDK:=${ANDROID_NDK_HOME:-/home/he/Android/Sdk/ndk/29.0.14206865}}"
BIN=""
for t in linux-arm64 linux-x86_64; do
    if [ -x "$NDK/toolchains/llvm/prebuilt/$t/bin/clang" ]; then BIN="$NDK/toolchains/llvm/prebuilt/$t/bin"; break; fi
done
[ -n "$BIN" ] || { echo "test-lzfse.sh: no llvm in $NDK" >&2; exit 1; }
: "${CC:=aarch64-linux-gnu-gcc}"

OUT=${OUT:-build/host-test}
mkdir -p "$OUT"

INC="-Icore -Ihost -Idevices -Iandroid/compat/include -I$OUT"
DEFS="-D_GNU_SOURCE -D__ANDROID__ -include android/compat/include/hv_darwin_compat.h"
CFLAGS="-std=gnu11 -O1 -g -w $INC $DEFS"

# The decoder calls the C library; guest code cannot. Its memory routines are renamed to ones that
# live in this binary, so everything executed as guest code is inside the registered range.
LZ="-O2 -w -Iandroid/third_party/lzfse -fno-builtin \
    -Dmemcpy=hv_memcpy -Dmemset=hv_memset -Dmemmove=hv_memmove"
for f in android/third_party/lzfse/lzfse_decode.c android/third_party/lzfse/lzfse_decode_base.c \
         android/third_party/lzfse/lzfse_fse.c android/third_party/lzfse/lzvn_decode_base.c; do
    o="$OUT/$(basename "$f" .c).lz.o"
    [ -f "$o" ] && [ "$o" -nt "$f" ] || $CC $LZ -c "$f" -o "$o"
done

cat > "$OUT/lzmem.c" <<'EOF'
/* The decoder's memory routines, in this binary, so guest code never calls into the C library.
   Linked with --wrap so every reference — including the ones the compiler generates for a struct
   copy — lands here rather than in libc, which is not mapped as guest memory. */
#include <stdint.h>
#include <stddef.h>
static uint8_t arena[1u << 20];
static size_t arena_at;
void *__wrap_malloc(size_t n) {
    size_t at = (arena_at + 15) & ~(size_t) 15;
    if (at + n > sizeof arena) return 0;
    arena_at = at + n;
    return arena + at;
}
void __wrap_free(void *p) { (void) p; }
void *hv_memcpy(void *d, const void *s, size_t n);
void *hv_memset(void *d, int c, size_t n);
void *hv_memmove(void *d, const void *s, size_t n);
void *__wrap_memcpy(void *d, const void *s, size_t n) { return hv_memcpy(d, s, n); }
void *__wrap_memset(void *d, int c, size_t n) { return hv_memset(d, c, n); }
void *__wrap_memmove(void *d, const void *s, size_t n) { return hv_memmove(d, s, n); }
void *hv_memcpy(void *d, const void *s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
void *hv_memset(void *d, int c, size_t n) {
    uint8_t *dd = d;
    while (n--) *dd++ = (uint8_t) c;
    return d;
}
void *hv_memmove(void *d, const void *s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    if (dd < ss) while (n--) *dd++ = *ss++;
    else { dd += n; ss += n; while (n--) *--dd = *--ss; }
    return d;
}
EOF

cat > "$OUT/tid.c" <<'EOF'
#include <pthread.h>
int pthread_gettid_np(pthread_t t) { (void)t; return 0; }
EOF

for f in core/*.c host/jit_region.c android/compat/compat_android.c; do
    o="$OUT/$(basename "$f" .c).o"
    [ -f "$o" ] && [ "$o" -nt "$f" ] || $CC $CFLAGS -c "$f" -o "$o"
done
[ -f "$OUT/enter.o" ] || $CC -c host/enter.S -o "$OUT/enter.o"
[ -f "$OUT/tid.o" ] || $CC $CFLAGS -c "$OUT/tid.c" -o "$OUT/tid.o"
$CC $CFLAGS -c "$OUT/lzmem.c" -o "$OUT/lzmem.o"
# `_end` comes from the linker; the compat header defines it as nothing useful for a static object.
$CC $CFLAGS -no-pie tests/test_lzfse_jit.c "$OUT"/*.o -o "$OUT/test_lzfse_jit" -lpthread -lm -latomic \
    -Wl,--wrap=memcpy,--wrap=memset,--wrap=memmove,--wrap=malloc,--wrap=free
exec "$OUT/test_lzfse_jit" "$@"
