#!/usr/bin/env python3
# Enumerate every thread of launchd's task, by scanning guest memory for the
# thread_ro's back-pointer, and report what each thread is waiting on.
#
# Why this shape. `struct thread_ro` (osfmk/kern/thread.h) is
#
#     struct thread_ro { struct thread *tro_owner; ... struct task *tro_task; };
#
# and the thread holds `struct thread_ro *t_tro`. So for every word equal to the
# task's address there is at most one thread_ro, whose address is (word position
# - offsetof(thread_ro, tro_task)) -- an offset this does not assume: it is the
# one for which `tro_owner` names a thread that holds a pointer back to that same
# thread_ro. Both directions must hold, so a coincidental word cannot pass.
#
# The thread's wait_event is the word after its 16-byte links union
# (osfmk/kern/thread.h:406-412); a release kernel has no MACH_ASSERT
# thread_magic in front of it.
#
#   gdb -p $(pidof host-install) -batch -ex "set print thread-events off" \
#       -x /data/local/tmp/henge/probe_all.py

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


def words_holding(pa, lo, hi, want):
    for off in range(lo, hi, 8):
        try:
            if rd64(pa + off) == want:
                return off
        except ValueError:
            return None
    return None


def all_occurrences(pat, chunk=4 << 20):
    """(guest pa, value position) for every alignment-8 occurrence of 8 bytes `pat`."""
    out = []
    for host, base, size in SEGS:
        off = 0
        while off < size:
            n = min(chunk, size - off)
            try:
                buf = bytes(INF.read_memory(host + off, n))
            except gdb.MemoryError:
                off += n
                continue
            i = buf.find(pat)
            while i >= 0:
                if i % 8 == 0:
                    out.append(base + off + i)
                i = buf.find(pat, i + 1)
            off += n
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


def dump(pa, n):
    data = rd_pa(pa, n)
    for row in range(0, len(data) - 15, 16):
        a = pa + row
        cells = " ".join("%02x" % c for c in data[row:row + 16])
        txt = "".join(chr(c) if 32 <= c < 127 else "." for c in data[row:row + 16])
        print("    %011x  %-47s |%s|" % (a, cells, txt))


def main():
    pcomm = find_pcomm()
    got = find_proc(pcomm) if pcomm else None
    if not got:
        print("no launchd proc")
        return
    k, proc_pa, proc_va = got
    ro_pa = walk(rd64(proc_pa + 0x18))
    task_va = rd64(ro_pa + 8)
    task_pa = walk(task_va)
    print("proc 0x%x pa 0x%x (p_comm +0x%x)" % (proc_va, proc_pa, k))
    print("task 0x%x pa 0x%x" % (task_va, task_pa))
    print("\nproc head (p_pid is an int32 == 1 near the start, followed by p_stat):")
    dump(proc_pa, 0x80)

    # Every word equal to the task address: one of them is a thread_ro's
    # tro_task, and one is task.bsd_info_ro.
    occ = all_occurrences(struct.pack("<Q", task_va))
    print("\n%d words in guest RAM equal the task address" % len(occ))

    threads = {}
    for p in occ:
        for x in range(8, 0x400, 8):
            ro = p - x                      # candidate thread_ro start
            try:
                owner = rd64(ro)
            except ValueError:
                continue
            if not is_kaddr(owner):
                continue
            owner_pa = walk(owner)
            if owner_pa is None:
                continue
            # thread -> t_tro must point back at this thread_ro
            if words_holding(owner_pa, 0, 0x800, ro) is None:
                continue
            threads[owner] = (owner_pa, ro, p)
            break

    print("\nwhat each of those words points from (0x40 before it):")
    for p in occ:
        print("  word at pa 0x%x:" % p)
        try:
            dump(p - 0x20, 0x30)
        except ValueError:
            print("    unreadable")

    print("\nthreads of this task (thread_ro.tro_owner -> thread), %d found:"
          % len(threads))
    for t_va, (t_pa, ro_va, tro_task_pa) in sorted(threads.items()):
        try:
            we = rd64(t_pa + 0x10)
            nxt, prev = rd64(t_pa), rd64(t_pa + 8)
            runq = rd64(t_pa + 0x18)
            waitq = rd64(t_pa + 0x20)
        except ValueError:
            continue
        print("  thread 0x%x pa 0x%x  (thread_ro 0x%x)" % (t_va, t_pa, ro_va))
        print("    wait_event 0x%-16x links next 0x%x prev 0x%x"
              % (we, nxt, prev))
        print("    __runq 0x%-16x waitq 0x%x" % (runq, waitq))


main()
