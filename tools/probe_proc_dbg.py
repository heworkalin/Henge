#!/usr/bin/env python3
# Diagnostic for probe_xnu.py: dump the words around launchd's p_comm and say,
# for each one, whether it is a kernel pointer and where it translates to. This
# is what decides whether the LIST invariant is failing because the offset range
# is wrong, because a pointer is not what it looks like, or because the walk is.
#
#   gdb -p $(pidof host-install) -batch -x probe_proc_dbg.py

import struct

import gdb

INF = gdb.selected_inferior()
OA_MASK = 0x0000ffffffffffff
CMD, NAME = 17, 33

SEGS = []
for i in range(int(gdb.parse_and_eval("g_machine.nram"))):
    SEGS.append((int(gdb.parse_and_eval("g_machine.ram[%d].host" % i)),
                 int(gdb.parse_and_eval("g_machine.ram[%d].pa" % i)),
                 int(gdb.parse_and_eval("g_machine.ram[%d].size" % i))))

_CPU = {}


def cpu(name):
    if name not in _CPU:
        _CPU[name] = int(gdb.parse_and_eval("g_cpus[0].%s" % name))
    return _CPU[name]


def rd_pa(pa, n):
    for host, base, size in SEGS:
        if base <= pa and pa + n <= base + size:
            return bytes(INF.read_memory(host + (pa - base), n))
    raise ValueError("pa 0x%x" % pa)


def rd64(pa):
    return struct.unpack("<Q", rd_pa(pa, 8))[0]


def granule_bits(tg, ttbr1):
    if ttbr1:
        return 14 if tg == 1 else 16 if tg == 3 else 12
    return 14 if tg == 2 else 16 if tg == 1 else 12


def walk(va):
    if not (cpu("sctlr_el1") & 1):
        return va
    tcr = cpu("tcr_el1")
    hi = (va >> 55) & 1
    tsz = (tcr >> 16) & 63 if hi else tcr & 63
    tsz = max(16, min(39, tsz))
    epd = (tcr >> 23) & 1 if hi else (tcr >> 7) & 1
    g = granule_bits((tcr >> 30) & 3 if hi else (tcr >> 14) & 3, hi)
    if epd:
        return None
    inputsize = 64 - tsz
    stride = g - 3
    levels = (inputsize - g + stride - 1) // stride
    first = 4 - levels
    level = first
    base = (cpu("ttbr1_el1") if hi else cpu("ttbr0_el1")) & OA_MASK & ~1
    while True:
        shift = g + stride * (3 - level)
        idx_bits = inputsize - shift if level == first else stride
        if idx_bits <= 0:
            return None
        idx = (va >> shift) & ((1 << idx_bits) - 1)
        try:
            desc = rd64(base + idx * 8)
        except (ValueError, gdb.MemoryError):
            return None
        if not (desc & 1):
            return None
        if level < 3 and (desc & 2):
            base = desc & OA_MASK & ~((1 << g) - 1)
            level += 1
            continue
        if level == 3 and not (desc & 2):
            return None
        if level < 3 and (level == 0 or (g != 12 and level == 1)):
            return None
        size = 1 << shift
        return (desc & OA_MASK & ~(size - 1)) | (va & (size - 1))


def find():
    for host, base, size in SEGS:
        off = 0
        while off < size:
            n = min(4 << 20, size - off)
            try:
                buf = bytes(INF.read_memory(host + off, n))
            except gdb.MemoryError:
                off += n
                continue
            i = buf.find(b"launchd")
            while i >= 0:
                pa = base + off + i
                try:
                    if rd_pa(pa + CMD, 7) == b"launchd":
                        return pa
                except ValueError:
                    pass
                i = buf.find(b"launchd", i + 1)
            off += n
    return None


pcomm = find()
print("p_comm pa: %s" % ("0x%x" % pcomm if pcomm else "not found"))
if pcomm is None:
    raise SystemExit

print("cpu0 tcr 0x%x ttbr1 0x%x" % (cpu("tcr_el1"), cpu("ttbr1_el1")))

print("\n-- kernel pointers in [p_comm-0x2000, p_comm+0x800) --")
for off in range(-0x2000, 0x800, 8):
    pa = pcomm + off
    try:
        v = rd64(pa)
    except ValueError:
        continue
    if (v >> 56) not in (0xff, 0xfe):
        continue
    w = walk(v)
    extra = "walk=none"
    if w is not None:
        try:
            inner = rd64(w)
            # A LIST entry pointing at another node: the pointed word is a VA too.
            extra = "-> pa 0x%x [pa]=0x%x%s" % (
                w, inner, "  <-- [pa] is a VA" if (inner >> 56) in (0xff, 0xfe) else "")
        except ValueError:
            extra = "-> pa 0x%x unreadable" % w
    print("  %+05x  %016x  %s" % (off, v, extra))
