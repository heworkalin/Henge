#!/bin/sh
# Materialize the pinned metal2vulkan translator (the compile layer, `crates/metal2vulkan`) with
# our patches, for the Cargo `[patch]` entry in android/third_party/reims-vgpu/Cargo.toml.
#
# Why a copy and not the crate from the git dependency: Cargo fingerprints a git dependency by its
# revision and does not notice edits inside the checkout, and a `[patch]` entry needs a path.
# The copy is a build artefact (`build/` is ignored); the patches it takes are in tools/patches/,
# which is what the repository records. Upstream: github.com/steelbrain/metal2vulkan, branch
# `development`, rev 46e7cc96a284150bdf19b4a6a7890b0a8bc0ef71.
set -e
REV=46e7cc96a284150bdf19b4a6a7890b0a8bc0ef71
SHORT=46e7cc9
CARGO_HOME=${CARGO_HOME:-$HOME/.cargo}
SRC=$(ls -d "$CARGO_HOME"/git/checkouts/metal2vulkan-*/$SHORT 2>/dev/null | head -1)
DST=build/m2v-compile-layer
[ -n "$SRC" ] || { echo "no metal2vulkan checkout at $CARGO_HOME/git/checkouts: run a build once" >&2; exit 1; }
if [ ! -f "$DST/crates/metal2vulkan/src/lower/fragment/rog.rs" ]; then
    mkdir -p "$DST/crates"
    cp "$SRC/Cargo.toml" "$SRC/README.md" "$DST/"
    for c in metal2vulkan metal2vulkan-air metal2vulkan-devtools; do cp -a "$SRC/crates/$c" "$DST/crates/"; done
    # Only the crates copied are members: the others (validation, conformance, runtime) are large
    # and the runtime is vendored separately (android/third_party/metal2vulkan-runtime).
    python3 - "$DST/Cargo.toml" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
members = '''members = [
    "crates/metal2vulkan",
    "crates/metal2vulkan-air",
    "crates/metal2vulkan-devtools",
]'''
s = re.sub(r'members = \[[^\]]*\]', members, s, count=1)
open(p, 'w').write(s)
PY
fi
for p in tools/patches/m2v-*.patch; do
    patch --forward --batch -p1 -d "$DST" < "$p" >/dev/null || true
done
echo "$DST ready (translator $REV + $(ls tools/patches/m2v-*.patch | wc -l) patch(es))"
