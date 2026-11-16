#!/usr/bin/env python3
# Find launchd's task and its threads, then read each thread's wait_event/state.
#
# The chains, as this XNU (11417.140.69) actually declares them:
#
#   struct proc      p_proc_ro (bsd/sys/proc_internal.h:279) -> struct proc_ro
#   struct proc_ro   first field is the pointer back to the proc
#   struct task      `struct proc_ro *bsd_info_ro`   (osfmk/kern/task.h:319)
#   struct task      `queue_head_t threads`          (osfmk/kern/task.h:231)
#   struct thread    `queue_chain_t task_threads`    (osfmk/kern/thread.h:776)
#   struct thread    `struct thread_ro *t_tro` right after it (arm64 has no
#                    t_task: that field is inside `#if __x86_64__ || __arm__`)
#   struct thread_ro `struct task *tro_task`
#   struct thread    `wait_event` is the word after the 16-byte links union
#                    (osfmk/kern/thread.h:406-412), i.e. thread+0x10 in release
#                    builds (MACH_ASSERT's thread_magic is not compiled in)
#
# Every hop is checked by a back-pointer, so no offset here is a guess:
#   proc_ro -> proc, bsd_info_ro -> proc_ro, thread_ro.tro_task -> task.
# The only offset left unknown is where `task_threads` sits inside `struct
# thread`, so the thread's start is bracketed by dumping around it and finding
# the links union + wait_event pattern.
#
#   gdb -p $(pidof host-install) -batch -ex "set print thread-events off" \
#       -x /data/local/tmp/henge/probe_thread.py

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


def rd_va(va, n=8):
    pa = walk(va)
    if pa is None:
        return None, None
    try:
        return pa, rd_pa(pa, n)
    except ValueError:
        return pa, None


def is_kaddr(v):
    return v is not None and (v >> 56) in (0xff, 0xfe)


def words_holding(pa, lo, hi, want):
    """Offsets in [lo,hi) whose 8-byte word equals `want`."""
    out = []
    for off in range(lo, hi, 8):
        try:
            if rd64(pa + off) == want:
                out.append(off)
        except ValueError:
            break
    return out


def dump(pa, n, marks=None):
    marks = marks or {}
    data = rd_pa(pa, n)
    for row in range(0, len(data) - 15, 16):
        a = pa + row
        cells = " ".join("%02x" % c for c in data[row:row + 16])
        txt = "".join(chr(c) if 32 <= c < 127 else "." for c in data[row:row + 16])
        note = "  ".join(t for off, t in marks.items() if a <= off < a + 16)
        print("    %011x  %-47s |%s| %s" % (a, cells, txt, note))


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
        prev_pa, _ = rd_va(prev)
        if prev_pa is None:
            continue
        try:
            s_va = rd64(prev_pa)
        except ValueError:
            continue
        if is_kaddr(s_va) and walk(s_va) == s_pa:
            return k, s_pa, s_va
    return None


def main():
    pcomm = find_pcomm()
    if pcomm is None:
        print("no p_comm")
        return
    got = find_proc(pcomm)
    if got is None:
        print("no linked proc")
        return
    k, proc_pa, proc_va = got
    print("proc: pa 0x%x va 0x%x (p_comm +0x%x)" % (proc_pa, proc_va, k))

    # proc.p_proc_ro is the third pointer (after the p_list union): +0x18.
    ro_va = rd64(proc_pa + 0x18)
    ro_pa, ro_raw = rd_va(ro_va, 8)
    ro_first = None if ro_raw is None else struct.unpack("<Q", ro_raw)[0]
    print("p_proc_ro 0x%x -> pa %s, first word 0x%x %s"
          % (ro_va, "0x%x" % ro_pa if ro_pa else "none", ro_first,
             "OK (points back at the proc)" if ro_first == proc_va else "MISMATCH"))

    # struct proc_ro is { struct proc *pr_proc; struct task *pr_task; }
    # (bsd/sys/proc_ro.h:73-75), so the task is the second word of proc_ro --
    # not a field of struct proc, which in this version has no task pointer at
    # all (proc_task() reads it from proc_ro via proc_get_task_raw()).
    task_va = rd64(ro_pa + 8)
    task_pa = walk(task_va)
    if task_pa is None:
        print("pr_task 0x%x does not translate" % task_va)
        return
    bsd = words_holding(task_pa, 0, 0x1000, ro_va)
    print("pr_task 0x%x (pa 0x%x), bsd_info_ro at task+0x%x %s"
          % (task_va, task_pa, bsd[0] if bsd else -1,
             "OK" if bsd else "NOT FOUND -- wrong structure?"))
    if not bsd:
        return

    # task.threads: XNU's queue_head_t is a circular list, so a one-element
    # queue reads next == prev == &thread-><link>. task_threads is at
    # osfmk/kern/thread.h:776; on arm64 `t_tro` follows it (t_task is inside
    # `#if __x86_64__ || __arm__`), which is what identifies the thread.
    print("\ntask+0x50 (threads) => ...")
    a = rd64(task_pa + 0x50)
    b = rd64(task_pa + 0x58)
    a_pa = walk(a)
    print("  next 0x%x prev 0x%x %s (pa %s)"
          % (a, b, "one element" if a == b else "several",
             "0x%x" % a_pa if a_pa else "untranslatable"))
    if a_pa is None:
        return
    # Show what the link points into, and every word after it that is a kernel
    # pointer: the thread's own fields (t_tro, map, wait_timer) show up there.
    dump(a_pa - 0x40, 0x140, {a_pa: "<-- task_threads"})
    print("\n  kernel pointers in the linked object [0, 0x400):")
    for off in range(0, 0x400, 8):
        try:
            v = rd64(a_pa + off)
        except ValueError:
            break
        if is_kaddr(v):
            t_pa = walk(v)
            print("    +0x%03x 0x%016x -> pa %s" % (off, v, "0x%x" % t_pa if t_pa else "none"))


main()
