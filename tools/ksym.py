#!/usr/bin/env python3
"""Resolve a guest kernel/kext address to a symbol, from a kernelcache.

    tools/ksym.py work/kc-24G90.bin 0xfffffe001db396ac [more...]

How the names are found, and the two traps that cost time here:

  * A kernelcache is MH_FILESET. Every kext is a fileset entry whose
    _PrelinkExecutableLoadAddr (from __PRELINK_INFO's plist) is a *link VA*;
    subtract the kernelcache base (0xfffffe0007004000) to get the file offset of
    that kext's Mach-O header, then read its LC_SYMTAB.
  * symoff/stroff in those LC_SYMTABs are offsets into the *whole kernelcache*,
    not relative to the kext -- read d[symoff], not d[kext_off+symoff].
  * nlist_64 is 16 bytes at symoff+k*16, and its `value` already includes the
    slide (it is a run VA). Do NOT subtract the slide: doing so silently maps
    everything onto the wrong (low) symbols.
  * names only resolve when `strx < strsize`; the entries with strx == 0 or out
    of range are codeless/placeholder rows and must be skipped, not read (reading
    them yields garbage such as 0x96340e00 and poisons the name table).

Kext code lives in __TEXT_EXEC, so this covers kexts (AppleVirtIO, IOKit, ...).
The kernel's own code carries no symbols in this table -- for those use
LC_FUNCTION_STARTS to at least get function boundaries.
"""
import bisect
import plistlib
import struct
import sys

KC_BASE = 0xfffffe0007004000
PRELINK_INFO_FILEOFF = 0x33b4000
PRELINK_INFO_SIZE = 0x31c000


def u64(x):
    return x & 0xffffffffffffffff


def build(path):
    d = open(path, 'rb').read()
    seg = d[PRELINK_INFO_FILEOFF:PRELINK_INFO_FILEOFF + PRELINK_INFO_SIZE]
    i = seg.find(b'<plist')
    j = seg.find(b'</plist>')
    arr = plistlib.loads(seg[i:j + 8])['_PrelinkInfoDictionary']
    syms = {}
    for ent in arr:
        la = u64(ent.get('_PrelinkExecutableLoadAddr', 0) or 0)
        if not la:
            continue
        fo = la - KC_BASE
        if fo < 0 or fo + 0x20 > len(d):
            continue
        if struct.unpack('<I', d[fo:fo + 4])[0] != 0xfeedfacf:
            continue
        ncmds = struct.unpack('<I', d[fo + 16:fo + 20])[0]
        off, symoff, nsyms, stroff, strsize = fo + 32, 0, 0, 0, 0
        for _ in range(ncmds):
            cmd, cmdsize = struct.unpack('<2I', d[off:off + 8])
            if cmd == 0x2:
                symoff, nsyms, stroff, strsize = struct.unpack('<4I', d[off + 8:off + 24])
            off += cmdsize
        if not nsyms:
            continue
        for k in range(nsyms):
            p = symoff + k * 16
            if p + 16 > len(d):
                break
            strx, _t, _s, _d, val = struct.unpack('<IBBHQ', d[p:p + 16])
            if not val or val == 0xffffffffffffffff:
                continue
            if strx == 0 or strx >= strsize:
                continue
            end = d.find(b'\0', stroff + strx, stroff + strsize)
            if end < 0:
                continue
            nm = d[stroff + strx:end].decode('utf-8', 'replace')
            if nm and val not in syms:
                syms[val] = nm
    return syms


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    syms = build(sys.argv[1])
    keys = sorted(syms)
    print("symbols: %d" % len(syms))
    for a in sys.argv[2:]:
        t = int(a, 16)
        idx = bisect.bisect_right(keys, t) - 1
        if idx < 0:
            print("0x%x: no symbol before it" % t)
            continue
        b = keys[idx]
        print("0x%x -> %s + 0x%x" % (t, syms[b], t - b))
    return 0


if __name__ == '__main__':
    sys.exit(main())
