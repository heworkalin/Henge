#!/usr/bin/env bash
#
# Android cross-compilation environment for this tree.
#
#   source android/env.sh                    # arm64-v8a (default), API 33
#   API=26 source android/env.sh             # another API level
#   ABI=x86_64 source android/env.sh         # another ABI
#
# Exports what both halves of the build need:
#
#   C   — the NDK clang wrappers for `$ABI`, via CC/CXX/AR and the CMake toolchain
#   Rust — the aarch64-linux-android (or matching) target's linker and archiver,
#          so `cargo build --target $RUST_TARGET` needs no .cargo/config.toml
#
# Verified on an aarch64 Linux host with NDK 29.0.14206865: the NDK ships a
# native `linux-arm64` prebuilt, so clang runs without any x86_64 emulation.
#
# Note for Rust: this is a *cross* build even on an arm64 device, because the
# target is Android (bionic, API level) not Linux (glibc). Do not expect to run
# the produced .so through `cargo run`.
#

_hv_ndk="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"
if [[ -z "$_hv_ndk" || ! -d "$_hv_ndk" ]]; then
    echo "android/env.sh: ANDROID_NDK_HOME is not set" >&2
    return 1 2>/dev/null || exit 1
fi

_hv_abi="${ABI:-arm64-v8a}"
_hv_api="${API:-33}"

# The NDK's own host tag, detected rather than assumed: an x86_64 workstation
# gets linux-x86_64, an arm64 one linux-arm64.
_hv_host_tag=""
for _t in linux-arm64 linux-x86_64 darwin-arm64 darwin-x86_64; do
    if [[ -d "$_hv_ndk/toolchains/llvm/prebuilt/$_t" ]]; then _hv_host_tag="$_t"; break; fi
done
if [[ -z "$_hv_host_tag" ]]; then
    echo "android/env.sh: no usable prebuilt toolchain under $_hv_ndk/toolchains/llvm/prebuilt" >&2
    return 1 2>/dev/null || exit 1
fi

_hv_bin="$_hv_ndk/toolchains/llvm/prebuilt/$_hv_host_tag/bin"

# ABI -> clang target triple and Rust target.
case "$_hv_abi" in
    arm64-v8a)   _hv_trip=aarch64-linux-android;    _hv_rust=aarch64-linux-android ;;
    armeabi-v7a) _hv_trip=armv7a-linux-androideabi; _hv_rust=armv7-linux-androideabi ;;
    x86_64)      _hv_trip=x86_64-linux-android;     _hv_rust=x86_64-linux-android ;;
    x86)         _hv_trip=i686-linux-android;       _hv_rust=i686-linux-android ;;
    riscv64)     _hv_trip=riscv64-linux-android;    _hv_rust=riscv64-linux-android ;;
    *) echo "android/env.sh: unknown ABI '$_hv_abi'" >&2; return 1 2>/dev/null || exit 1 ;;
esac

export HV_ABI="$_hv_abi"
export HV_API="$_hv_api"
export HV_RUST_TARGET="$_hv_rust"
export HV_NDK="$_hv_ndk"
export HV_NDK_BIN="$_hv_bin"

# ---- C / CMake ------------------------------------------------------------
export CC="$_hv_bin/${_hv_trip}${_hv_api}-clang"
export CXX="$_hv_bin/${_hv_trip}${_hv_api}-clang++"
export AR="$_hv_bin/llvm-ar"
export RANLIB="$_hv_bin/llvm-ranlib"
export STRIP="$_hv_bin/llvm-strip"
export NM="$_hv_bin/llvm-nm"

# ---- Rust -----------------------------------------------------------------
# Cargo reads CARGO_TARGET_<TARGET>_LINKER / _AR (target upper-cased, dashes to
# underscores) and cc-rs reads CC_<target> / AR_<target> (lower-case, dashes to
# underscores). Both spellings are set so crates that build C (mbedtls, jemalloc,
# SDL3 for reims-vgpu) find the cross compiler too.
_hv_T=$(echo "$_hv_rust" | tr 'a-z-' 'A-Z_')
_hv_t=$(echo "$_hv_rust" | tr '-' '_')

export CARGO_TARGET_${_hv_T}_LINKER="$CC"
export CARGO_TARGET_${_hv_T}_AR="$AR"

export CC_${_hv_t}="$CC"
export CXX_${_hv_t}="$CXX"
export AR_${_hv_t}="$AR"
export CFLAGS_${_hv_t}="--sysroot=$_hv_ndk/toolchains/llvm/prebuilt/$_hv_host_tag/sysroot -O2 -fPIC"
export CXXFLAGS_${_hv_t}="$CFLAGS_${_hv_t}"

# reims-vgpu's Vulkan platform builds SDL3 from source; its CMake looks here.
export SDL3_ANDROID=1

echo "android/env.sh: ABI=$HV_ABI API=$HV_API target=$HV_RUST_TARGET"
echo "  NDK    $_hv_ndk ($_hv_host_tag)"
echo "  CC     $CC"

unset _hv_ndk _hv_abi _hv_api _hv_host_tag _hv_bin _hv_trip _hv_rust _hv_T _hv_t
