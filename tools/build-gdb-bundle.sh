#!/usr/bin/env bash
#
# Build a self-contained GDB bundle out of the Termux install on the device:
# gdb, its whole dependency tree, guile's Scheme sources and compiled cache,
# and a tiny entry binary that re-roots all of it at run time.
#
# Why a bundle. The launcher used to be /data/local/tmp/henge/gdb.sh: a shell
# script with the Termux prefix baked in. A package update moved the libraries,
# and a command line with a quote or a `$` was re-interpreted by the shell
# before gdb saw it. The bundle takes its own directory as the prefix
# (tools/gdbwrap.c reads /proc/self/exe), so it can be tarred, pushed anywhere
# and run.
#
#   ./tools/build-gdb-bundle.sh              # build into build/gdb-bundle
#   ./tools/build-gdb-bundle.sh --push       # build, push, and smoke-test
#   ./tools/build-gdb-bundle.sh --push-only
#
# The device path is where Termux lives; override with HENGE_TERMUX_PREFIX.
# The build host needs the NDK (android/env.sh) and llvm-readelf from it.

set -euo pipefail

cd "$(dirname "$0")/.."
REPO=$PWD

DEVICE=${DEVICE:-emulator-5554}
PREFIX=${HENGE_TERMUX_PREFIX:-/data/local/tmp/termux/files/usr}
OUT=$REPO/build/gdb-bundle
TARBALL=$REPO/build/gdb-bundle.tar.gz
PUSH_DIR=${HENGE_BUNDLE_DIR:-/data/local/tmp/henge/gdb-bundle}

# What the BFS does not follow into: these come from /system/lib64 on the
# device and must not be bundled with Android's own copies shadowing them.
# Note that this is by soname, not by prefix: gdb needs libz.so.1, which is
# Termux's, while Android's is libz.so -- different names, different libraries.
SYSTEM_LIBS=' libc.so libm.so libdl.so liblog.so libandroid.so libstdc++.so libc++.so '

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

adb_dev() { adb -s "$DEVICE" "$@"; }

# llvm-readelf from the NDK: the host has no aarch64 ELF parser otherwise.
find_readelf() {
    local ndk
    ndk=$(sed -n 's/^[[:space:]]*export NDK=//p' android/env.sh 2>/dev/null | head -1 | tr -d '"')
    [ -n "$ndk" ] || ndk=${ANDROID_NDK_HOME:-}
    [ -n "$ndk" ] || die "NDK not found: set ANDROID_NDK_HOME or keep android/env.sh's export NDK="
    local r
    r=$(ls "$ndk"/toolchains/llvm/prebuilt/*/bin/llvm-readelf 2>/dev/null | head -1)
    [ -n "$r" ] || die "llvm-readelf not found under $ndk"
    printf '%s' "$r"
}

# The sonames a shared object needs, one per line.
needed_of() {
    "$READELF" -d "$1" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\].*/\1/p'
}

BUILD=1 PUSH=0
for arg in "$@"; do
    case $arg in
        --push) PUSH=1 ;;
        --push-only) BUILD=0 PUSH=1 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) die "unknown argument: $arg" ;;
    esac
done

if [ "$BUILD" = 1 ]; then

READELF=$(find_readelf)
say "readelf: $READELF"
adb_dev get-state >/dev/null || die "device $DEVICE not available"
adb_dev shell "test -x $PREFIX/bin/gdb" || die "no gdb at $PREFIX/bin/gdb on $DEVICE"

# Start from scratch: a stale lib/ from an older gdb would silently satisfy a
# soname and then fail on a missing symbol, which is a confusing failure.
rm -rf "$OUT"
mkdir -p "$OUT/bin" "$OUT/lib"

say "pulling gdb"
adb_dev pull "$PREFIX/bin/gdb" "$OUT/bin/gdb.real" >/dev/null

# Breadth-first over the dependency graph. Only libraries that gdb actually
# reaches are copied, so the bundle stays ~90 MB instead of the Termux tree's
# 666 MB of lib/. A soname that is already in lib/ is not pulled again, which
# also terminates the cycles (libguile <-> libgc and friends).
say "resolving the dependency tree"
# In memory, not through a file: the loop appends while iterating, and a
# redirected file would stop at the first EOF and quietly pull one library.
deps=()
while IFS= read -r s; do [ -n "$s" ] && deps+=("$s"); done < <(needed_of "$OUT/bin/gdb.real")
declare -A seen=()
pulled=0
i=0
while [ "$i" -lt "${#deps[@]}" ]; do
    soname=${deps[$i]}
    i=$((i + 1))
    [ -n "${seen[$soname]:-}" ] && continue
    seen[$soname]=1
    if printf '%s' "$SYSTEM_LIBS" | grep -qF " $soname "; then
        continue
    fi
    [ -e "$OUT/lib/$soname" ] && continue
    # Termux's lib/*.so are symlinks (libexpat.so.1 -> libexpat.so.1.13.1) and
    # adb pull refuses to follow one, so resolve first and keep the soname as
    # the local name -- that is what the loader asks for.
    real=$(adb_dev shell "readlink -f $PREFIX/lib/$soname" | tr -d '\r')
    if [ -z "$real" ] || ! adb_dev shell "test -e $real"; then
        printf 'error: %s is needed but not in %s/lib\n' "$soname" "$PREFIX" >&2
        printf '       (if it lives in /system/lib64, add it to SYSTEM_LIBS)\n' >&2
        exit 1
    fi
    adb_dev pull "$real" "$OUT/lib/$soname" >/dev/null
    pulled=$((pulled + 1))
    while IFS= read -r s; do [ -n "$s" ] && deps+=("$s"); done < <(needed_of "$OUT/lib/$soname")
done

say "pulled $pulled libraries"

# guile: gdb is built with it and initialises the interpreter before main().
# Without the Scheme sources it aborts (scm_load_startup_files), and without
# the compiled cache it spends a minute compiling them at every start.
say "pulling guile (Scheme sources and compiled cache)"
mkdir -p "$OUT/share" "$OUT/lib"
# Through a tar stream: both trees hold symlinks and thousands of small .go
# files, which adb pull handles badly (it skips symlinks) and slowly (one stat
# per file).
adb_dev exec-out "tar -czf - -C $PREFIX share/guile lib/guile" > "$OUT/guile.tgz" \
    || die "cannot tar guile on the device"
tar -xzf "$OUT/guile.tgz" -C "$OUT" || die "cannot unpack guile"
rm -f "$OUT/guile.tgz"

# gdb's own data directory (guile's gdb modules, the Python support library,
# syscall tables) is looked up at its compile-time prefix unless told
# otherwise; the entry binary passes --data-directory=<bundle>/share/gdb.
say "pulling gdb's data directory and Python's standard library"
adb_dev exec-out "tar -czf - -C $PREFIX share/gdb lib/python3.14" > "$OUT/data.tgz" \
    || die "cannot tar gdb's data directory on the device"
tar -xzf "$OUT/data.tgz" -C "$OUT" || die "cannot unpack gdb's data directory"
rm -f "$OUT/data.tgz"

# Anything guile or gdb dlopen()s is invisible to the soname walk above; the
# .go/.so modules came with lib/guile, and these are the two that are opened by
# name from lib/ directly.
for extra in libthread_db.so libpython3.14.so; do
    if adb_dev shell "test -e $PREFIX/lib/$extra"; then
        [ -e "$OUT/lib/$extra" ] && continue
        real=$(adb_dev shell "readlink -f $PREFIX/lib/$extra" | tr -d '\r')
        adb_dev pull "$real" "$OUT/lib/$extra" >/dev/null
    fi
done

say "building the entry binary"
# The NDK's own clang, by absolute path: android/env.sh exports API 33's, and
# the entry binary has no reason to require anything newer than 28.
if [ -z "${CC:-}" ] || ! command -v "$CC" >/dev/null; then
    CC=$(ls "$(dirname "$READELF")"/aarch64-linux-android28-clang 2>/dev/null | head -1)
    [ -n "$CC" ] || CC=$(ls "$(dirname "$READELF")"/aarch64-linux-android*-clang 2>/dev/null | sort -V | head -1)
fi
[ -n "$CC" ] || die "no cross compiler in the NDK"
say "cc: $CC"
# Link against the system libc only: the entry binary must work even when the
# bundle's own libraries are the broken part.
"$CC" -O2 -Wall -Wextra -o "$OUT/bin/gdb" "$REPO/tools/gdbwrap.c"
"$READELF" -d "$OUT/bin/gdb" | sed -n 's/.*Shared library: \[\(.*\)\].*/    needs \1/p'
# A bundled lib must not shadow the entry binary's own libc.
[ -e "$OUT/lib/libc.so" ] && rm -f "$OUT/lib/libc.so"

say "bundle: $(du -sh "$OUT" | cut -f1) at $OUT"
tar -czf "$TARBALL" -C "$REPO/build" gdb-bundle
say "tarball: $(du -h "$TARBALL" | cut -f1) at $TARBALL"

fi  # BUILD

if [ "$PUSH" = 0 ]; then
    cat <<EOF

Next:
  ./tools/build-gdb-bundle.sh --push-only     # push and smoke-test
  adb -s $DEVICE shell '$PUSH_DIR/bin/gdb --version'

Use it as plain gdb. No shell sets its environment, so arguments arrive byte
for byte (a gdb command with quotes, $, ; or a glob used to be re-read by the
old gdb.sh). It also follows its own directory, so the bundle can be moved or
renamed without rebuilding:

  adb -s $DEVICE shell '$PUSH_DIR/bin/gdb -p \$(pidof host-install) -batch \
      -ex "p/x g_cpus[0].pc" -ex "p/x g_cpus[0].el"'
EOF
    exit 0
fi

say "pushing to $DEVICE:$PUSH_DIR"
adb_dev push "$TARBALL" "/data/local/tmp/gdb-bundle.tar.gz" >/dev/null
adb_dev shell "rm -rf $PUSH_DIR && mkdir -p $(dirname "$PUSH_DIR") && \
  tar -xzf /data/local/tmp/gdb-bundle.tar.gz -C /tmp && \
  mv /tmp/gdb-bundle $PUSH_DIR && rm -f /data/local/tmp/gdb-bundle.tar.gz && \
  chmod -R u+rwX $PUSH_DIR && chmod 755 $PUSH_DIR/bin/gdb $PUSH_DIR/bin/gdb.real"

say "smoke test"
adb_dev shell "$PUSH_DIR/bin/gdb --version" | head -1 || die "the bundle does not run"

say "done: $PUSH_DIR/bin/gdb"
