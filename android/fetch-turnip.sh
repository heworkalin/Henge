#!/usr/bin/env bash
#
# Fetch the Turnip Vulkan driver (libvulkan_freedreno.so) for the driver plugin.
#
#   ./android/fetch-turnip.sh <url> [sha256]
#   ./android/fetch-turnip.sh --from-file <path> [sha256]
#   ./android/fetch-turnip.sh --verify
#
# Why a script with an explicit URL instead of a pinned release: this project has
# not chosen a Turnip build yet, and quietly pulling "latest" from a third party
# is not something a build should do. So the URL is an argument, and the SHA-256
# is recorded in PROVENANCE.txt next to the library.
#
# Rules this script enforces:
#   - the download goes to a temp file, never straight into jniLibs
#   - a supplied sha256 must match, or nothing is installed
#   - without a sha256, the computed one is printed and the file is NOT installed
#     unless --accept-new is passed, so the first fetch is a deliberate act
#   - PROVENANCE.txt records url, sha256, size and time
#
# The driver is embedded in the app's own jniLibs, not shipped as a second APK:
# one install, and the app decides which Vulkan library to open. The FCL plugin
# contract is still what the selection code speaks (see android/driver-plugin),
# so an externally installed plugin is usable too — it is just not required.
#
# Turnip is a Mesa component, MIT-licensed. Whatever build is used, its license
# and source must travel with it — the app ships third-party code.
#
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
dest_dir="$here/app/src/main/jniLibs/arm64-v8a"
dest="$dest_dir/libvulkan_freedreno.so"
prov="$here/app/src/main/jniLibs/arm64-v8a/PROVENANCE.txt"

usage() {
    sed -n '2,25p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit "${1:-0}"
}

if [[ $# -eq 0 ]]; then usage 2; fi

# ---- --verify: check what is installed against the recorded provenance -------
if [[ "$1" == "--verify" ]]; then
    if [[ ! -f "$dest" ]]; then echo "not installed: $dest" >&2; exit 1; fi
    actual="$(sha256sum "$dest" | cut -d' ' -f1)"
    echo "file    : $dest"
    echo "sha256  : $actual"
    echo "size    : $(stat -c%s "$dest") bytes"
    if [[ -f "$prov" ]] && grep -q "$actual" "$prov"; then
        echo "provenance: matches $prov"
        exit 0
    fi
    echo "provenance: NO MATCH (or no record) — this binary is unaccounted for" >&2
    exit 1
fi

accept_new=0
args=()
for a in "$@"; do
    case "$a" in
        --accept-new) accept_new=1 ;;
        *) args+=("$a") ;;
    esac
done

src="${args[0]:-}"
expected="${args[1]:-}"
if [[ -z "$src" ]]; then usage 2; fi

tmp="$(mktemp)"
trap 'rm -f "$tmp"' EXIT

if [[ "$src" == "--from-file" ]]; then
    # A local copy: the driver came from a repository already on this machine.
    # The path is still recorded, because "which binary is this" has to survive
    # the fact that it did not come off the network.
    path="${args[1]:-}"
    expected="${args[2]:-}"
    if [[ -z "$path" || ! -f "$path" ]]; then
        echo "ERROR: --from-file needs an existing path" >&2
        exit 1
    fi
    src="from-file:$path"
    echo "== copy =="
    echo "path: $path"
    cp "$path" "$tmp"
else
    echo "== fetch =="
    echo "url: $src"
    if command -v curl >/dev/null; then
        curl -fL --retry 3 --connect-timeout 20 -o "$tmp" "$src"
    else
        wget -O "$tmp" "$src"
    fi
fi

size="$(stat -c%s "$tmp")"
actual="$(sha256sum "$tmp" | cut -d' ' -f1)"
echo "size  : $size bytes"
echo "sha256: $actual"

if [[ -n "$expected" ]]; then
    if [[ "${expected,,}" != "$actual" ]]; then
        echo "ERROR: sha256 mismatch" >&2
        echo "  expected $expected" >&2
        echo "  actual   $actual" >&2
        exit 1
    fi
    echo "sha256: matches the value given"
elif [[ $accept_new -eq 0 ]]; then
    echo
    echo "No sha256 was given, so the file was NOT installed."
    echo "Check that the value above is the one you expect, then re-run with it:"
    echo "  $0 '$src' $actual"
    echo "or accept it as-is (first fetch only):"
    echo "  $0 --accept-new '$src'"
    exit 1
fi

# A Turnip build is an ELF shared object for aarch64; refuse anything else rather
# than installing a tarball or an HTML error page as if it were a driver.
if ! head -c 20 "$tmp" | od -An -tx1 | tr -d ' \n' | grep -q '^7f454c46'; then
    echo "ERROR: not an ELF file — is the URL a direct link to the .so?" >&2
    echo "       (a .zip/.tar.gz needs unpacking first; this script does not do it)" >&2
    exit 1
fi

# A driver that does not export an entry point in .dynsym cannot be used at all:
# dlopen() succeeds, dlsym() returns NULL, and the failure surfaces later as
# "Vulkan not available" with no explanation. FCL-Team/FCLDriverPlugin ships
# exactly such a binary — one defined dynamic symbol (HMI, an object), with
# vk_icdGetInstanceProcAddr and vkGetInstanceProcAddr local. Verify the shape
# *before* installing, not after the app fails to start.
defined_dyn_funcs() {
    local f="$1" tool
    if command -v readelf >/dev/null; then
        tool=readelf
    elif [[ -x "${ANDROID_NDK_HOME:-}/toolchains/llvm/prebuilt/linux-arm64/bin/llvm-readelf" ]]; then
        tool="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-arm64/bin/llvm-readelf"
    else
        echo "        (no readelf available, skipping the export check)" >&2
        return 0
    fi
    "$tool" --dyn-syms -W "$f" | awk '$7 != "UND" && $4 == "FUNC" { print $8 }'
}

if ! defined_dyn_funcs "$tmp" | grep -qxE 'vk_icdGetInstanceProcAddr|vkGetInstanceProcAddr'; then
    echo "ERROR: this library exports no Vulkan entry point in .dynsym." >&2
    echo "       dlopen() would succeed and dlsym() would return NULL." >&2
    echo "       Defined dynamic FUNC symbols:" >&2
    defined_dyn_funcs "$tmp" | sed 's/^/         /' >&2
    echo "       Need a build that exports vk_icdGetInstanceProcAddr (an ICD) or" >&2
    echo "       vkGetInstanceProcAddr (a loader-compatible library)." >&2
    exit 1
fi

mkdir -p "$dest_dir"
mv "$tmp" "$dest"
chmod 644 "$dest"
trap - EXIT

{
    echo "# Turnip driver provenance — written by android/fetch-turnip.sh"
    echo "file   : libvulkan_freedreno.so"
    echo "source : $src"
    echo "sha256 : $actual"
    echo "size   : $size"
    echo "fetched: $(date '+%Y-%m-%d %H:%M:%S')"
    echo
    echo "# Turnip is a Mesa component (MIT). Its license text must accompany any"
    echo "# APK that ships this library; add it under driver-plugin/licenses/."
} > "$prov"

echo
echo "installed: $dest"
echo "provenance: $prov"
echo
echo "Next:"
echo "  ./android/build-apk.sh      # embeds the driver in the app APK"
