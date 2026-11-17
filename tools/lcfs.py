#!/usr/bin/env python3
"""Build a function-start index for every image in an Apple kernelcache.

    tools/lcfs.py /tmp/kc72.bin --json work/kernel_functions.json
    tools/lcfs.py /tmp/kc72.bin --symbol 0xfffffe001c890000

Why this exists: `tools/ksym.py` resolves addresses for kexts only. The kernel's
own code (where msleep / wakeup / assert_wait live) carries no nlist symbols at
all, so before any breakpoint can be set there we need at least function
boundaries. LC_FUNCTION_STARTS is exactly that table: a ULEB128 stream of deltas
from the image's __TEXT vmaddr.

Three traps, each of which costs a round if missed:

  * MH_FILESET: the top-level Mach-O is not an image. Each image is an
    LC_FILESET_ENTRY (cmd 0x80000035) whose `entry_id` is the string offset
    measured **from the command start**, not from the data that follows it.
  * `_PrelinkExecutableLoadAddr` from __PRELINK_INFO is a signed plist integer in
    practice; mask it with & 0xffffffffffffffff or you get negative file offsets.
  * LC_FUNCTION_STARTS's dataoff/datasize are offsets into the **whole
    kernelcache**, not into the image, exactly like symoff/stroff.
  * deltas are relative to the image's __TEXT vmaddr and span **all** executable
    segments (__TEXT, __TEXT_EXEC, __KLD), so a delta can easily be larger than
    __TEXT's size. Do not clamp to __TEXT.

What the output is NOT: function lengths. Mach-O only records starts, so `end_va`
is the next start (or segment end for the last one) and is labelled with a
confidence value. Never print these as symbol names.
"""
import argparse
import json
import plistlib
import struct
import sys

KC_BASE = 0xfffffe0007004000          # kernelcache link base
PRELINK_INFO_FILEOFF = 0x33b4000
PRELINK_INFO_SIZE = 0x31c000

LC_SEGMENT_64 = 0x19
LC_FUNCTION_STARTS = 0x26
LC_FILESET_ENTRY = 0x80000035


def u64(x):
    return x & 0xffffffffffffffff


def parse_uleb(buf, off):
    """Return (value, new_off). Raises ValueError on a truncated sequence."""
    val = 0
    shift = 0
    start = off
    while True:
        if off >= len(buf):
            raise ValueError("truncated ULEB128 at %d" % start)
        b = buf[off]
        off += 1
        val |= (b & 0x7F) << shift
        if not (b & 0x80):
            break
        shift += 7
        if shift > 63:
            raise ValueError("ULEB128 too long at %d" % start)
    return val, off


def macho_segments(d, fo):
    """Return (segments, lcfs) for the Mach-O header at file offset fo."""
    if fo < 0 or fo + 0x20 > len(d):
        return None, None
    if struct.unpack('<I', d[fo:fo + 4])[0] != 0xfeedfacf:
        return None, None
    ncmds = struct.unpack('<I', d[fo + 16:fo + 20])[0]
    off = fo + 32
    segs = []
    lcfs = None
    for _ in range(ncmds):
        if off + 8 > len(d):
            break
        c, sz = struct.unpack('<2I', d[off:off + 8])
        if sz < 8:
            break
        if c == LC_SEGMENT_64:
            name = d[off + 8:off + 24].rstrip(b'\0').decode('utf-8', 'replace')
            vmaddr, vmsize, foff, fsize = struct.unpack('<4Q', d[off + 24:off + 56])
            prot = struct.unpack('<i', d[off + 60:off + 64])[0]
            segs.append({'name': name, 'vmaddr': vmaddr, 'vmsize': vmsize,
                         'fileoff': foff, 'filesize': fsize, 'prot': prot})
        elif c == LC_FUNCTION_STARTS:
            lcfs = struct.unpack('<2I', d[off + 8:off + 16])
        off += sz
    return segs, lcfs


def fileset_entries(d):
    """MH_FILESET top level -> [(id, vmaddr, fileoff)]. Falls back to a single
    implicit entry when the header is a plain Mach-O."""
    magic = struct.unpack('<I', d[0:4])[0]
    if magic != 0xfeedfacf:
        return []
    ncmds = struct.unpack('<I', d[16:20])[0]
    off = 32
    out = []
    for _ in range(ncmds):
        c, sz = struct.unpack('<2I', d[off:off + 8])
        if c == LC_FILESET_ENTRY:
            vmaddr, fileoff = struct.unpack('<2Q', d[off + 8:off + 24])
            entry_id = struct.unpack('<I', d[off + 24:off + 28])[0]
            so = off + entry_id
            e = d.find(b'\0', so)
            out.append((d[so:e].decode('utf-8', 'replace'), vmaddr, fileoff))
        off += sz
    return out


def prelink_entries(d):
    seg = d[PRELINK_INFO_FILEOFF:PRELINK_INFO_FILEOFF + PRELINK_INFO_SIZE]
    i = seg.find(b'<plist')
    j = seg.find(b'</plist>')
    if i < 0 or j < 0:
        return []
    arr = plistlib.loads(seg[i:j + 8]).get('_PrelinkInfoDictionary', [])
    out = []
    for ent in arr:
        la = u64(ent.get('_PrelinkExecutableLoadAddr', 0) or 0)
        # 0x7fff_ffff_ffff_ffff is the "no executable image" sentinel, not a VA
        if la and la != 0x7fffffffffffffff:
            out.append((ent.get('CFBundleIdentifier', '?'), la, la - KC_BASE))
    return out


def build_index(d, image_id, hdr_fo, stats):
    segs, lcfs = macho_segments(d, hdr_fo)
    if segs is None:
        stats['bad_header'].append(image_id)
        return []
    text = next((s for s in segs if s['name'] == '__TEXT'), None)
    if text is None:
        stats['no_text'].append(image_id)
        return []
    base = text['vmaddr']
    exec_segs = [s for s in segs if s['prot'] & 4 and s['vmsize'] > 0]
    if not exec_segs:
        stats['no_exec'].append(image_id)
        return []
    if lcfs is None:
        stats['no_lcfs'].append(image_id)
        return []
    dataoff, datasize = lcfs
    if dataoff + datasize > len(d):
        stats['truncated'].append(image_id)
        return []
    blob = d[dataoff:dataoff + datasize]

    starts = []
    off = 0
    cur = 0
    try:
        while off < len(blob):
            delta, off = parse_uleb(blob, off)
            if delta == 0:          # terminator
                break
            cur += delta
            starts.append(base + cur)
    except ValueError as e:
        stats['uleb_errors'].append((image_id, str(e)))
        return []

    # executable range coverage
    hi = max(s['vmaddr'] + s['vmsize'] for s in exec_segs)
    lo = min(s['vmaddr'] for s in exec_segs)
    in_exec = 0
    out_exec = []
    for va in starts:
        if lo <= va < hi:
            in_exec += 1
        else:
            out_exec.append(va)
    mono = all(starts[i] < starts[i + 1] for i in range(len(starts) - 1))
    dups = len(starts) - len(set(starts))

    rows = []
    for i, va in enumerate(starts):
        if i + 1 < len(starts):
            end = starts[i + 1]
            conf = 'high'          # bounded by the next recorded start
        else:
            end = hi
            conf = 'low'           # only bounded by the segment end
        rows.append({
            'image': image_id,
            'start_va': va,
            'end_va': end,
            'size': end - va,
            'start_offset': va - base,
            'confidence': conf,
        })
    stats['images'].append({
        'image': image_id,
        'base': base,
        'functions': len(starts),
        'exec_lo': lo,
        'exec_hi': hi,
        'in_exec': in_exec,
        'out_exec': len(out_exec),
        'monotonic': mono,
        'duplicates': dups,
    })
    return rows


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('kernelcache')
    ap.add_argument('--json', help='write the index here')
    ap.add_argument('--csv', help='write a CSV here')
    ap.add_argument('--symbol', nargs='*', default=[],
                    help='resolve run VAs to image + offset')
    ap.add_argument('--only', help='restrict to images whose id contains this')
    ap.add_argument('--quiet', action='store_true')
    args = ap.parse_args()

    d = open(args.kernelcache, 'rb').read()
    stats = {'bad_header': [], 'no_text': [], 'no_exec': [], 'no_lcfs': [],
             'truncated': [], 'uleb_errors': [], 'images': []}
    rows = []

    images = fileset_entries(d)
    for name, _vm, fo in images:
        if args.only and args.only not in name:
            continue
        rows.extend(build_index(d, name, fo, stats))

    # kexts reachable only through __PRELINK_INFO (not always fileset entries)
    seen = {n for n, _, _ in images}
    for name, _la, fo in prelink_entries(d):
        if name in seen:
            continue
        if args.only and args.only not in name:
            continue
        rows.extend(build_index(d, name, fo, stats))

    total = sum(i['functions'] for i in stats['images'])
    print("images with index : %d" % len(stats['images']))
    print("functions total   : %d" % total)
    print("bad header        : %d" % len(stats['bad_header']))
    print("no __TEXT         : %d" % len(stats['no_text']))
    print("no exec segment   : %d" % len(stats['no_exec']))
    print("no LC_FUNCTION_STARTS: %d" % len(stats['no_lcfs']))
    print("truncated data    : %d" % len(stats['truncated']))
    print("ULEB128 errors    : %d" % len(stats['uleb_errors']))
    nonmono = [i for i in stats['images'] if not i['monotonic']]
    dupimg = [i for i in stats['images'] if i['duplicates']]
    outrange = [i for i in stats['images'] if i['out_exec']]
    print("non-monotonic     : %d" % len(nonmono))
    print("images with dups  : %d" % len(dupimg))
    print("images with starts outside exec segs: %d" % len(outrange))
    for i in outrange[:5]:
        print("    %-42s %d of %d outside [0x%x,0x%x)"
              % (i['image'], i['out_exec'], i['functions'], i['exec_lo'], i['exec_hi']))
    for i in stats['uleb_errors'][:5]:
        print("    ULEB error %s: %s" % i)

    if args.only is None and not args.quiet:
        print("\nlargest images:")
        for i in sorted(stats['images'], key=lambda x: -x['functions'])[:8]:
            print("  %-42s %6d functions  base=0x%x" % (i['image'], i['functions'], i['base']))

    if args.json:
        with open(args.json, 'w') as f:
            json.dump({'rows': rows, 'stats': stats}, f)
        print("\nwrote %s (%d rows)" % (args.json, len(rows)))
    if args.csv:
        with open(args.csv, 'w') as f:
            f.write("image,start_va,end_va,size,start_offset,confidence\n")
            for r in rows:
                f.write("%s,0x%x,0x%x,0x%x,0x%x,%s\n" % (
                    r['image'], r['start_va'], r['end_va'], r['size'],
                    r['start_offset'], r['confidence']))
        print("wrote %s (%d rows)" % (args.csv, len(rows)))

    for want in args.symbol:
        va = int(want, 0)
        hits = [r for r in rows if r['start_va'] <= va < r['end_va']]
        if not hits:
            print("%s -> not inside any indexed function" % want)
            continue
        r = hits[0]
        print("%s -> %s + 0x%x  (function 0x%x..0x%x, size 0x%x, %s)"
              % (want, r['image'], va - r['start_va'], r['start_va'],
                 r['end_va'], r['size'], r['confidence']))


if __name__ == '__main__':
    main()
