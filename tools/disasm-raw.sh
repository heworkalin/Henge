#!/usr/bin/env bash
#
# Disassembles a window of a raw AArch64 blob, at the address it runs at.
#
#   tools/disasm-raw.sh fwcode.bin 0x700a0000 0x700a8300 0x700a8500
#
#   argv[1]  the blob (no ELF header, exactly what the guest has in memory)
#   argv[2]  the address the first byte of the blob runs at
#   argv[3]  first address to print
#   argv[4]  end address (exclusive)
#
# llvm-objcopy can give a raw blob the one thing llvm-objdump needs — a section
# header that says where it lives — so this needs nothing but the NDK that the
# build already uses.
set -euo pipefail

blob=${1:?blob}
base=${2:?base address, e.g. 0x700a0000}
from=${3:-$base}
to=${4:-}
if [[ -z "$to" ]]; then
    size=$(stat -c %s "$blob")
    to=$(printf '0x%x' $((base + size)))
fi

ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}
if [[ -z "$ndk" ]]; then
    echo "disasm-raw.sh: set ANDROID_NDK_HOME (source android/env.sh)" >&2
    exit 1
fi
host=$(basename "$(dirname "$(dirname "$(command -v "$ndk/toolchains/llvm/prebuilt/"*/bin/llvm-objdump 2>/dev/null | head -1)")")" 2>/dev/null || true)
bin=""
for t in linux-arm64 linux-x86_64 darwin-arm64 darwin-x86_64; do
    if [[ -x "$ndk/toolchains/llvm/prebuilt/$t/bin/llvm-objdump" ]]; then
        bin="$ndk/toolchains/llvm/prebuilt/$t/bin"
        break
    fi
done
if [[ -z "$bin" ]]; then
    echo "disasm-raw.sh: no llvm-objdump under $ndk/toolchains/llvm/prebuilt" >&2
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

"$bin/llvm-objcopy" -I binary -O elf64-littleaarch64 --binary-architecture=aarch64 \
    --rename-section .data=.text,alloc,load,readonly,code \
    --change-section-address=.text="$base" "$blob" "$tmp/blob.o"

# llvm-objcopy leaves a binary-input section at address 0 whatever it is told,
# so the range is given relative to the blob and the printed addresses are moved
# to where the blob runs — the only thing that matters when reading the code.
off_from=$(printf '%d' $((from - base)))
off_to=$(printf '%d' $((to - base)))
echo "# blob $blob loaded at $base"
"$bin/llvm-objdump" -d --start-address="$off_from" --stop-address="$off_to" "$tmp/blob.o" \
| python3 -c '
import re, sys
base = int(sys.argv[1], 16)
for line in sys.stdin:
    m = re.match(r"\s*([0-9a-f]+):(.*)$", line.rstrip("\n"))
    if m:
        print("  %x:%s" % (int(m.group(1), 16) + base, m.group(2)))
    else:
        print(line.rstrip("\n"))
' "$base"
