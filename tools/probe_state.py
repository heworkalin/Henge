#!/usr/bin/env python3
# Dump launchd's thread whole, annotated, so `state` can be located by reading
# the object against osfmk/kern/thread.h rather than by arithmetic over 109
# lines of #if'd field declarations.
#
# What makes this readable by eye:
#   * the links union is the first 16 bytes (runq_links / wait_links) and, for
#     a thread on a wait queue, both words are the queue head;
#   * wait_event is next (thread.h:406-412);
#   * `thread_id` (thread.h:878) is a small monotonically assigned number;
#   * `state` is a small int made of TH_* bits (0x01 TH_WAIT, 0x02 TH_SUSP,
#     0x04 TH_RUN, 0x08 TH_UNINT, 0x10 TH_TERMINATE, 0x80 TH_IDLE,
#     0x100 TH_WAKING) and is followed by `ast`;
#   * the thread's own `t_tro` points at a thread_ro whose tro_owner is this
#     thread -- printed below, as the anchor.
#
#   gdb -p $(pidof host-install) -batch -ex "set print thread-events off" \
#       -x /data/local/tmp/henge/probe_state.py

import struct

import gdb

INF = gdb.selected_inferior()
OA_MASK = 0x0000ffffffffffff
CMD = 17
TH = {0x01: "TH_WAIT", 0x02: "TH_SUSP", 0x04: "TH_RUN", 0x08: "TH_UNINT",
      0x10: "TH_TERM", 0x20: "TH_TERM2", 0x40: "TH_WAIT_REPORT",
      0x80: "TH_IDLE", 0x100: "TH_WAKING"}

SEGS = []
for _i in range(int(gdb.parse_and_eval("g_machine.nram"))):
    SEGS.append((int(gdb.parse_and_eval("g_machine.ram[%d].host" % _i)),
                 int(gdb.parse_and_eval("g_machine.ram[%d].pa" % _i)),
                 int(gdb.parse_and_eval("g_machine.ram[%d].size" % _i))))

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
    tsz = max(16, min(39, (tcr >> 16) & 63 if hi else tcr & 63))
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


def is_kaddr(v):
    return v is not None and (v >> 56) in (0xff, 0xfe)


def find_pcomm(name=b"launchd"):
    for host, base, size in SEGS:
        off = 0
        while off < size:
            n = min(4 << 20, size - off)
            try:
                buf = bytes(INF.read_memory(host + off, n))
            except gdb.MemoryError:
                off += n
                continue
            i = buf.find(name)
            while i >= 0:
                pa = base + off + i
                try:
                    if rd_pa(pa + CMD, len(name)) == name:
                        return pa
                except ValueError:
                    pass
                i = buf.find(name, i + 1)
            off += n
    return None


def find_proc(pcomm):
    for k in range(pcomm % 8 or 8, 0x1000, 8):
        s_pa = pcomm - k
        try:
            nxt, prev = rd64(s_pa), rd64(s_pa + 8)
        except ValueError:
            break
        if not (is_kaddr(prev) and is_kaddr(nxt)) or (nxt == s_pa and prev == s_pa + 8):
            continue
        prev_pa = walk(prev)
        if prev_pa is None:
            continue
        try:
            s_va = rd64(prev_pa)
        except ValueError:
            continue
        if is_kaddr(s_va) and walk(s_va) == s_pa:
            return k, s_pa, s_va
    return None


def analyse(name):
    pcomm = find_pcomm(name)
    got = find_proc(pcomm) if pcomm else None
    if not got:
        print("no proc")
        return
    k, proc_pa, proc_va = got
    ro_pa = walk(rd64(proc_pa + 0x18))
    task_va = rd64(ro_pa + 8)
    task_pa = walk(task_va)
    # proc.p_stat, right after p_pid (ints at +0x60, +0x64 here).
    print("proc 0x%x pa 0x%x  p_pid %d  p_stat %d"
          % (proc_va, proc_pa, struct.unpack("<i", rd_pa(proc_pa + 0x60, 4))[0],
             struct.unpack("<i", rd_pa(proc_pa + 0x64, 4))[0]))
    print("task 0x%x pa 0x%x" % (task_va, task_pa))

    # The thread we already proved: the queue target at task+0x50 whose
    # thread_ro points back at it.
    a = rd64(task_pa + 0x50)
    t_pa = walk(a)
    if t_pa is None:
        print("task+0x50 untranslatable")
        return
    # find its t_tro (a pointer whose target's first word is `a`)
    tro = None
    for off in range(0, 0x200, 8):
        w = rd64(t_pa + off)
        if not is_kaddr(w):
            continue
        w_pa = walk(w)
        if w_pa is None:
            continue
        if rd64(w_pa) == a:
            tro = (off, w, w_pa)
            break
    if tro:
        print("thread 0x%x pa 0x%x  t_tro at +0x%x = 0x%x (tro_owner == thread OK)"
              % (a, t_pa, tro[0], tro[1]))
    else:
        print("thread 0x%x pa 0x%x  (t_tro not found in first 0x200)" % (a, t_pa))

    data = rd_pa(t_pa, 0x800)
    hits = []
    for off in range(0, 0x800, 4):
        v = struct.unpack_from("<I", data, off)[0]
        if 0 < v < 0x200 and (v & ~0x1ff) == 0 and off not in (0x10, 0x14, 0x18, 0x1c):
            bits = "+".join(n for b, n in TH.items() if v & b)
            hits.append((off, v, bits))
    print("  TH-like words: " + ", ".join("+0x%x=0x%x(%s)" % h for h in hits))
    return
    for off in range(0, 0x800, 16):
        row = data[off:off + 16]
        w0 = struct.unpack_from("<Q", data, off)[0]
        w1 = struct.unpack_from("<Q", data, off + 8)[0]
        i0 = struct.unpack_from("<i", data, off)[0]
        i1 = struct.unpack_from("<i", data, off + 8)[0]
        txt = "".join(chr(c) if 32 <= c < 127 else "." for c in row)
        notes = []
        if is_kaddr(w0):
            notes.append("q0=kptr")
        if is_kaddr(w1):
            notes.append("q1=kptr")
        for v, tag in ((i0, "i0"), (i1, "i1")):
            if 0 < v < 0x200 and (v & ~0x1ff) == 0:
                bits = "+".join(n for b, n in TH.items() if v & b)
                notes.append("%s=0x%x%s" % (tag, v, ("(%s)" % bits) if bits else ""))
        print("  +0x%03x  %s  |%s|  %s"
              % (off, " ".join("%02x" % c for c in row), txt, " ".join(notes)))


for _n in (b"kernel_task", b"launchd"):
    print("=== %s ===" % _n.decode())
    try:
        analyse(_n)
    except Exception as e:
        print("  failed: %s" % e)
