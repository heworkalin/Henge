#!/bin/sh
# Builds the engine for the host (aarch64 glibc) and runs tests/test_regs.c.
#
# The Makefile's tests use Darwin assembler syntax and Apple frameworks, so they cannot build
# here; this compiles the same engine sources with the Android compatibility headers, which are
# plain POSIX apart from the memfd JIT region that glibc has too. A device is not involved, so a
# regression round takes seconds instead of a reinstall.
#
#   ./android/test-regs.sh            build and run
#   CC=aarch64-linux-gnu-gcc ./android/test-regs.sh
set -eu
cd "$(dirname "$0")/.."
: "${CC:=aarch64-linux-gnu-gcc}"
OUT=${OUT:-build/host-test}
mkdir -p "$OUT"
INC="-Icore -Ihost -Idevices -Iandroid/compat/include"
DEFS="-D_GNU_SOURCE -D__ANDROID__ -include android/compat/include/hv_darwin_compat.h"
CFLAGS="-std=gnu11 -O1 -g -w $INC $DEFS"

# shim: bionic has pthread_gettid_np, glibc does not, and nothing here uses its value
cat > "$OUT/tid.c" <<'EOF'
#include <pthread.h>
int pthread_gettid_np(pthread_t t) { (void)t; return 0; }
EOF

for f in core/*.c host/jit_region.c android/compat/compat_android.c; do
    o="$OUT/$(basename "$f" .c).o"
    [ -f "$o" ] && [ "$o" -nt "$f" ] || $CC $CFLAGS -c "$f" -o "$o"
done
[ -f "$OUT/enter.o" ] || $CC -c host/enter.S -o "$OUT/enter.o"
[ -f "$OUT/tid.o" ] || $CC -c "$OUT/tid.c" -o "$OUT/tid.o"

$CC $CFLAGS tests/test_regs.c "$OUT"/*.o -o "$OUT/test_regs" -lpthread -lm -latomic
exec "$OUT/test_regs"
