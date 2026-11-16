#!/usr/bin/env python3
# Locate launchd's thread without assuming any offset inside struct thread.
#
# The identity used is a two-way one from `struct thread_ro`
# (osfmk/kern/thread.h):
#
#     struct thread_ro {
#         struct thread *tro_owner;   /* back to the thread */
#         ...
#         struct task   *tro_task;    /* back to the task   */
#     };
#
# and the thread holds `struct thread_ro *t_tro`. So a word W inside the task's
# threads queue target is confirmed to be a `t_tro` when the object it points at
# has tro_owner pointing back into the same object, and holds the task's own
# address. The thread's start is then tro_owner, and nothing was assumed about
# where t_tro or task_threads sit.
#
# Run under the bundle's gdb:
#   gdb -p $(pidof host-install) -batch -ex "set print thread-events off" \
#       -x /data/local/tmp/henge/probe_thread2.py

import struct

import gdb

INF = gdb.selected_inferior()
OA_MASK = 0x0000ffffffffffff
CMD = 17

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


def rd32(pa):
    return struct.unpack("<I", rd_pa(pa, 4))[0]


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


def rd_va64(va):
    pa = walk(va)
    if pa is None:
        return None, None
    try:
        return pa, rd64(pa)
    except ValueError:
        return pa, None


def words_holding(pa, lo, hi, want):
    out = []
    for off in range(lo, hi, 8):
        try:
            if rd64(pa + off) == want:
                out.append(off)
        except ValueError:
            break
    return out


def find_pcomm():
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


def find_proc(pcomm):
    for k in range(pcomm % 8 or 8, 0x1000, 8):
        s_pa = pcomm - k
        try:
            nxt, prev = rd64(s_pa), rd64(s_pa + 8)
        except ValueError:
            break
        if not (is_kaddr(prev) and is_kaddr(nxt)):
            continue
        if nxt == s_pa and prev == s_pa + 8:
            continue
        prev_pa, _ = rd_va64(prev)
        if prev_pa is None:
            continue
        try:
            s_va = rd64(prev_pa)
        except ValueError:
            continue
        if is_kaddr(s_va) and walk(s_va) == s_pa:
            return k, s_pa, s_va
    return None


def dump(pa, n, marks=None):
    marks = marks or {}
    data = rd_pa(pa, n)
    for row in range(0, len(data) - 15, 16):
        a = pa + row
        cells = " ".join("%02x" % c for c in data[row:row + 16])
        txt = "".join(chr(c) if 32 <= c < 127 else "." for c in data[row:row + 16])
        note = "  ".join(t for off, t in marks.items() if a <= off < a + 16)
        print("    %011x  %-47s |%s| %s" % (a, cells, txt, note))


def main():
    pcomm = find_pcomm()
    got = find_proc(pcomm) if pcomm else None
    if not got:
        print("no launchd proc")
        return
    k, proc_pa, proc_va = got
    ro_va = rd64(proc_pa + 0x18)
    ro_pa, ro_first = rd_va64(ro_va)
    task_va = rd64(ro_pa + 8)
    task_pa = walk(task_va)
    print("proc va 0x%x / task va 0x%x pa 0x%x  (proc_ro->proc %s)"
          % (proc_va, task_va, task_pa,
             "OK" if ro_first == proc_va else "MISMATCH"))

    # Every kernel pointer in the task, and for each, every kernel pointer near
    # the object it names: one of those is t_tro, and the object it names is a
    # thread_ro whose tro_owner points back into that same object.
    hits = []
    for off in range(0, 0x800, 8):
        try:
            v = rd64(task_pa + off)
        except ValueError:
            break
        if not is_kaddr(v):
            continue
        v_pa = walk(v)
        if v_pa is None:
            continue
        for o2 in range(-0x600, 0x600, 8):
            try:
                w = rd64(v_pa + o2)
            except ValueError:
                continue
            if not is_kaddr(w):
                continue
            w_pa = walk(w)
            if w_pa is None:
                continue
            try:
                owner = rd64(w_pa)          # thread_ro.tro_owner
            except ValueError:
                continue
            if not is_kaddr(owner):
                continue
            owner_pa = walk(owner)
            if owner_pa is None:
                continue
            # tro_owner must be the thread that holds this t_tro, i.e. inside
            # the same object we found the pointer in.
            if not (0 <= v_pa - owner_pa < 0x1000):
                continue
            if not words_holding(w_pa, 0, 0x200, task_va):
                continue
            hits.append((off, v, v_pa, w, w_pa, owner, owner_pa))

    print("thread candidates (task pointer, t_tro, thread_ro, thread):")
    for off, v, v_pa, w, w_pa, owner, owner_pa in hits:
        print("  task+0x%x -> 0x%x (pa 0x%x); t_tro 0x%x (pa 0x%x);"
              " tro_owner 0x%x (pa 0x%x)"
              % (off, v, v_pa, w, w_pa, owner, owner_pa))
    if not hits:
        print("  none")
        return

    off, v, v_pa, w, w_pa, thread_va, thread_pa = hits[0]
    print("\nthread: va 0x%x pa 0x%x (%d candidates)"
          % (thread_va, thread_pa, len(hits)))
    print("  t_tro is at thread+0x%x" % (w_pa - thread_pa))

    # wait_event is the word after the 16-byte links union (thread.h:406-412),
    # and MACH_ASSERT's thread_magic is not in a release kernel.
    print("\n  first 0x30 of the thread (links union, then wait_event):")
    dump(thread_pa, 0x40, {thread_pa + 0x10: "<-- wait_event (union is 16 bytes)"})

    print("\n  32-bit words in [0,0x600) that look like a thread state:")
    for off2 in range(0, 0x600, 4):
        try:
            s = rd32(thread_pa + off2)
        except ValueError:
            break
        if s != 0 and s & ~0x3ff == 0:
            print("    thread+0x%03x = 0x%x" % (off2, s))


main()
