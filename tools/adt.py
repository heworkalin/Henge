#!/usr/bin/env python3
"""Print an Apple device tree (ADT) blob, and the component layout it carries.

The layout is the one UniXNU's scripts/qemu-entry/make-dt.py writes (UniXNU-qemu
generates its machine's tree with it):

    node := u32 prop_count, u32 child_count, prop_count * prop, child_count * node
    prop := char key[32]; u32 len; u8 value[len]; pad4

with the node's own name in a property called "name", and no magic: the blob starts
at the root node. /chosen/memory-map holds one 16-byte {u64 addr, u64 size} per
component, filled in by the bootloader, which is what says where each image goes.

A dump taken with a length shorter than the tree stops mid-node; the parser keeps
what it read instead of failing, so a truncated file is still worth looking at.

  tools/adt.py work/dt.bin                 # whole tree; memory-map shown as a table
  tools/adt.py work/dt.bin /chosen         # one subtree
"""
import struct
import sys


def parse(b, at):
    """(node, end offset). Never raises: a truncated blob yields what parsed."""
    node = ['', [], []]
    try:
        nprops, nkids = struct.unpack_from('<II', b, at)
    except struct.error:
        return node, at
    at += 8
    for _ in range(nprops):
        if at + 36 > len(b):
            return node, at
        key = b[at:at + 32].split(b'\0')[0].decode('latin1')
        (ln,) = struct.unpack_from('<I', b, at + 32)
        at += 36
        if at + ln > len(b):
            return node, at
        node[1].append((key, b[at:at + ln]))
        at += ln + (-ln % 4)
    node[0] = dict(node[1]).get('name', b'').rstrip(b'\0').decode('latin1')
    for _ in range(nkids):
        if at + 8 > len(b):
            return node, at
        kid, at = parse(b, at)
        node[2].append(kid)
    return node, at


def walk(node, path, want, depth, out):
    name, props, kids = node
    here = (path + '/' + name) if name else (path or '')
    memmap = next((v for k, v in props if k == 'memory-map'), None)
    for key, val in props:
        if key == 'name':
            continue
        if key == 'memory-map':
            continue
        if want and want != here:
            continue
        if len(val) == 16:
            a, s = struct.unpack('<QQ', val)
            out.append(f"{'  ' * depth}{here}/{key}: addr {a:#x} size {s:#x}")
        elif len(val) <= 24:
            out.append(f"{'  ' * depth}{here}/{key}: {val.hex()}")
    for kid in kids:
        if kid[0] == 'memory-map':
            if want and want != here:
                continue
            out.append(f"{'  ' * depth}{here}/memory-map:")
            for key, val in kid[1]:
                if key == 'name' or len(val) != 16:
                    continue
                a, s = struct.unpack('<QQ', val)
                out.append(f"{'  ' * (depth + 1)}{key:18s} addr {a:#018x} size {s:#x}")
    for kid in kids:
        if kid[0] != 'memory-map':
            walk(kid, here, want, depth + 1, out)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    blob = open(sys.argv[1], 'rb').read()
    want = sys.argv[2].rstrip('/') if len(sys.argv) > 2 else ''
    root, used = parse(blob, 0)
    if used < len(blob):
        print(f"# parsed {used} of {len(blob)} bytes (truncated dump)", file=sys.stderr)
    else:
        print(f"# whole tree: {used} of {len(blob)} bytes", file=sys.stderr)
    out = []
    walk(root, '', want, 0, out)
    print('\n'.join(out) if out else '# nothing at that path')


if __name__ == '__main__':
    main()
