#!/bin/sh
# tools/clone-deps.sh
#
# Clone (and check out) every third-party source this repository depends on, so a fresh machine can
# build Henge without guessing where each tree came from.
#
# Why this script exists: the dependency set is recorded in three different ways, and none of them
# is a submodule (there is no .gitmodules anywhere in this repository):
#
#   1. android/third_party/reims-vgpu      a gitlink (mode 160000) with no .gitmodules entry
#   2. android/third_party/libadrenotools  a gitlink (mode 160000), same
#   3. android/third_party/metal2vulkan-runtime  vendored: committed as ordinary files
#   4. android/third_party/lzfse           vendored: committed as ordinary files
#   5. third_party/*                       vendored: committed as ordinary files, each with UPSTREAM_COMMIT
#
# So "git clone" alone gets 3, 4 and 5 (they are in the tree), while 1 and 2 come out EMPTY — their
# directories exist but hold nothing. That is the trap this script fixes.
#
# Default mode only handles 1 and 2. `--verify` additionally checks every UPSTREAM_COMMIT in
# third_party/ against its upstream repository, which is what tells you whether the recorded
# provenance is still true.
#
# reims-vgpu has a second problem: the commit Henge pins is not on any upstream branch. The 12
# commits that bring Android up (window, swapchain, page size, driver bring-up) live on a local
# branch 'rebuild' that has never been pushed to any remote. Until a remote has them, this script
# checks out the upstream base instead and says so. Point REIMS_REPO at a remote that does have
# them, without editing this file:
#
#   REIMS_REPO=https://github.com/<owner>/reims-vgpu tools/clone-deps.sh
#
# Usage:
#   tools/clone-deps.sh                    # fill the two gitlink trees from their remotes
#   tools/clone-deps.sh --verify           # also re-check third_party/*/UPSTREAM_COMMIT upstream
#   tools/clone-deps.sh --list             # print the table and exit
#
# Environment:
#   REIMS_REPO    remote for reims-vgpu            (default: upstream steelbrain)
#   ADRENO_REPO   remote for libadrenotools        (default: upstream bylaws)
#   VERIFY_LIMIT  check only the first N trees in --verify (smoke test)
#
# Read-only: no submodule is registered, nothing is staged, no history is rewritten.

set -eu

REIMS_REPO=${REIMS_REPO:-https://github.com/steelbrain/reims-vgpu}
ADRENO_REPO=${ADRENO_REPO:-https://github.com/bylaws/libadrenotools}
M2V_REPO=${M2V_REPO:-https://github.com/steelbrain/metal2vulkan}

# The reims-vgpu commit Henge's gitlink points at. It is NOT on any upstream branch: the 12 commits
# that bring the Android window, swapchain, page-size and driver bring-up up live on a local branch
# `rebuild` only, whose base is upstream 1fe0acb (origin/master). No remote has it yet.
REIMS_GITLINK=0830c4fdc184470fe3a5948aa62989a465ee393a
REIMS_LOCAL_BRANCH=rebuild
REIMS_BASE=1fe0acb68b287db36f1e6c7b9f45383940b8a76f

ADRENO_GITLINK=8fae8ce254dfc1344527e05301e43f37dea2df80

# The translator revision reims-vgpu pins in its Cargo.toml. Vendored separately as the runtime.
M2V_REV=46e7cc96a284150bdf19b4a6a7890b0a8bc0ef71

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

MODE=clone
VERIFY=0

while [ $# -gt 0 ]; do
    case $1 in
        --verify) VERIFY=1; shift ;;
        --list)   MODE=list; shift ;;
        -h|--help) sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done

# third_party/ trees:  name  repo  recorded revision
TP_TABLE='idevicerestore          https://github.com/libimobiledevice/idevicerestore
libimobiledevice        https://github.com/libimobiledevice/libimobiledevice
libplist                https://github.com/libimobiledevice/libplist
libusbmuxd              https://github.com/libimobiledevice/libusbmuxd
libirecovery            https://github.com/libimobiledevice/libirecovery
libimobiledevice-glue   https://github.com/libimobiledevice/libimobiledevice-glue
libtatsu                https://github.com/libimobiledevice/libtatsu
usbmuxd                 https://github.com/libimobiledevice/usbmuxd
mbedtls                 https://github.com/mbed-tls/mbedtls
libzip                  https://github.com/nih-at/libzip'

if [ "$MODE" = list ]; then
    cat <<EOF
Henge's third-party sources, and where each one comes from
==========================================================

gitlink trees (empty on a fresh clone; this script fills them)
  android/third_party/reims-vgpu
      $REIMS_REPO
      pinned  ${REIMS_GITLINK}  <- NOT upstream: local branch $REIMS_LOCAL_BRANCH
              base upstream $REIMS_BASE
      NOTE    no remote has this commit yet; set REIMS_REPO when one does
  android/third_party/libadrenotools
      $ADRENO_REPO
      pinned  $ADRENO_GITLINK  (upstream has it; plain clone works)

vendored trees (already in the clone; nothing to fetch)
  android/third_party/metal2vulkan-runtime   from $M2V_REPO
                                             rev $M2V_REV
  android/third_party/lzfse                  Apple's LZFSE
  third_party/curl_include                   curl headers only
  third_party/libusb-1.0                     libusb header only
  third_party/restore_config                 restore configuration material
  third_party/idevicerestore                \\\\
  third_party/libimobiledevice               |  recorded per-tree in UPSTREAM_COMMIT;
  third_party/libplist                       |  this repository carries local patches on
  third_party/libusbmuxd                     |  4 of them (see below)
  third_party/libirecovery                   |
  third_party/libimobiledevice-glue          |
  third_party/libtatsu                       |
  third_party/usbmuxd                       /
  third_party/mbedtls                        (tag mbedtls-3.6.2)
  third_party/libzip

downloaded, never committed
  Turnip libvulkan_freedreno.so    android/fetch-turnip.sh <url> [sha256]
                                   PROVENANCE.txt records what was fetched

local patches carried by this repository (not upstream, and required)
  third_party/idevicerestore/src/restore.c                 +109   device-tree memory map before iBoot
  third_party/idevicerestore/src/recovery.c                 +11   one installer at a time
  third_party/usbmuxd/src/usb.{c,h}                         +10   PID 0x1280..0x1283 (restore mode)
  third_party/libimobiledevice-glue/src/thread.c            +13   longer waits
EOF
    exit 0
fi

# A directory counts as "already a clone" only if HEAD resolves to a commit. A clone interrupted
# half-way holds a .git and nothing else; treating that as done would leave the tree empty forever.
is_usable_clone() {
    [ -e "$1/.git" ] && git -C "$1" rev-parse --verify -q HEAD >/dev/null 2>&1
}

say() { printf '%s\n' "$*"; }
have_git() { command -v git >/dev/null 2>&1; }

have_git || { echo "git not found" >&2; exit 1; }

# --- 1. libadrenotools: a plain clone at the pinned commit -------------------
say "==> android/third_party/libadrenotools"
ADRENO_DIR=android/third_party/libadrenotools
if is_usable_clone "$ADRENO_DIR"; then
    say "    already a clone at $(git -C "$ADRENO_DIR" rev-parse --short HEAD)"
elif [ -n "$(ls -A "$ADRENO_DIR" 2>/dev/null | grep -v '^\.git$' || true)" ]; then
    say "    non-empty and not a usable clone; leaving it alone"
else
    rm -rf "$ADRENO_DIR"
    mkdir -p "$ADRENO_DIR"
    git clone -q "$ADRENO_REPO" "$ADRENO_DIR"
    git -C "$ADRENO_DIR" -c advice.detachedHead=false checkout -q "$ADRENO_GITLINK"
    say "    cloned, checked out ${ADRENO_GITLINK}"
fi

# --- 2. reims-vgpu: the pinned commit exists only on a local branch ---------
say "==> android/third_party/reims-vgpu"
REIMS_DIR=android/third_party/reims-vgpu
if is_usable_clone "$REIMS_DIR"; then
    say "    already a clone at $(git -C "$REIMS_DIR" rev-parse --short HEAD)"
elif [ -n "$(ls -A "$REIMS_DIR" 2>/dev/null | grep -v '^\.git$' || true)" ]; then
    say "    non-empty and not a usable clone; leaving it alone"
else
    rm -rf "$REIMS_DIR"
    mkdir -p "$REIMS_DIR"
    say "    cloning $REIMS_REPO"
    if ! GIT_TERMINAL_PROMPT=0 git clone -q "$REIMS_REPO" "$REIMS_DIR" 2>/dev/null; then
        say ""
        say "    ERROR: could not clone $REIMS_REPO"
        say "    (wrong URL, a private remote without credentials, or no network)"
        rm -rf "$REIMS_DIR"
        exit 3
    fi
    if git -C "$REIMS_DIR" cat-file -e "$REIMS_GITLINK^{commit}" 2>/dev/null; then
        git -C "$REIMS_DIR" -c advice.detachedHead=false checkout -q "$REIMS_GITLINK"
        say "    checked out ${REIMS_GITLINK}"
    else
        git -C "$REIMS_DIR" -c advice.detachedHead=false checkout -q "$REIMS_BASE"
        say ""
        say "    The pinned commit is NOT in $REIMS_REPO"
        say "        wanted  $REIMS_GITLINK"
        say "        absent  (checked: not an object in that remote)"
        say ""
        say "    That is expected: no remote has it yet. The 12 commits that make Android"
        say "    graphics work (window attach, swapchain, page size, driver bring-up, the"
        say "    REIMS_DIAG probes) are on a local branch '$REIMS_LOCAL_BRANCH' that has never"
        say "    been pushed. Checked out the upstream base instead:"
        say "        $REIMS_BASE  (origin/master)"
        say ""
        say "    Not fatal: the C engine builds and the install path is unaffected. Only the"
        say "    GPU path stops at the first draw. When a remote does have the commits, point"
        say "    this script at it (no edit needed):"
        say "        REIMS_REPO=<that remote URL> tools/clone-deps.sh"
    fi
fi

# --- 3. what a fresh clone already has, and what it does not ---------------
say ""
say "Already in the tree (vendored, nothing to fetch):"
say "    android/third_party/metal2vulkan-runtime   rev $M2V_REV"
say "    android/third_party/lzfse"
say "    third_party/  (10 trees with UPSTREAM_COMMIT, plus curl_include, libusb-1.0, restore_config)"
say ""
say "Not in the tree by design:"
say "    Turnip libvulkan_freedreno.so  -> android/fetch-turnip.sh <url> [sha256]"

# --- 4. put the local patches back ---------------------------------------
# vendored trees carry this repository's patches already. If you REPLACE one of them with a
# pristine upstream clone, these edits are lost and the install breaks in ways that look
# unrelated. Say so once, loudly, instead of leaving it to be discovered later.
say ""
say "Reminder: 4 of the third_party/ trees carry local patches this repository needs"
say "    idevicerestore/src/restore.c, idevicerestore/src/recovery.c,"
say "    usbmuxd/src/usb.c+usb.h, libimobiledevice-glue/src/thread.c"
say "Replace any of them with an upstream checkout and the install stops working."

# --- 5. --verify: does every recorded UPSTREAM_COMMIT still exist? --------
if [ "$VERIFY" = 1 ]; then
    say ""
    say "==> verifying recorded provenance"
    printf '%s\n' "$TP_TABLE" | while read -r name url; do
        [ -n "$name" ] || continue
        rev=$(cat "third_party/$name/UPSTREAM_COMMIT" 2>/dev/null || true)
        if [ -z "$rev" ]; then
            say "    $name: no UPSTREAM_COMMIT"
            continue
        fi
        tmp=$(mktemp -d)
        # blobless, no checkout: we only need the commit object graph
        if git clone -q --filter=blob:none --no-checkout "$url" "$tmp/r" 2>/dev/null; then
            if git -C "$tmp/r" cat-file -e "$rev^{commit}" 2>/dev/null; then
                say "    $name: ${rev} ok  ($url)"
            else
                say "    $name: ${rev} NOT FOUND in $url"
            fi
        else
            say "    $name: could not clone $url"
        fi
        rm -rf "$tmp"
    done
fi

say ""
say "done"
