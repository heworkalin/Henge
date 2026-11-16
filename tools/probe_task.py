#!/usr/bin/env python3
# From a located `struct proc`, find its `task` and that task's threads, using
# only pointers that point back at themselves' owners:
#
#   proc                task                     thread
#   +----------+        +----------------+       +------------------+
#   | p_list   |        | ...            |       | task_links       |<--.
#   | ...      |  --->  | bsd_info ----.-|---->  | task ------------|---'
#   | (p_comm) |        | threads  -----|------>| state            |
#   +----------+        +----------------+       | wait_event       |
#                                                +------------------+
#
# Every step is checked by the back-pointer, so an offset is never assumed:
#   task->bsd_info == proc, thread->task == task.
#
# Run under the bundle's gdb:
#   gdb -p $(pidof host-install) -batch -ex "set print thread-events off" \
#       -x /data/local/tmp/henge/probe_task.py

import struct

import gdb

INF = gdb.selected_inferior()
OA_MASK = 0x0000ffffffffffff
CMD, NAME = 17, 33

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


def rd_va64(va):
    pa = walk(va)
    if pa is None:
        return None, None
    try:
        return pa, rd64(pa)
    except ValueError:
        return pa, None


def is_kaddr(v):
    return v is not None and (v >> 56) in (0xff, 0xfe)


def dump(pa, n, mark=None):
    data = rd_pa(pa, n)
    for row in range(0, len(data) - 15, 16):
        a = pa + row
        cells = " ".join("%02x" % c for c in data[row:row + 16])
        txt = "".join(chr(c) if 32 <= c < 127 else "." for c in data[row:row + 16])
        star = " <--" if mark is not None and a <= mark < a + 16 else ""
        print("    %011x  %-47s |%s|%s" % (a, cells, txt, star))


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
    """The proc whose p_list is a real chain: linked, not a self-loop."""
    for k in range(pcomm % 8 or 8, 0x1000, 8):
        s_pa = pcomm - k
        try:
            nxt, prev = rd64(s_pa), rd64(s_pa + 8)
        except ValueError:
            break
        if not (is_kaddr(prev) and is_kaddr(nxt)):
            continue
        if nxt == s_pa + 0 and prev == s_pa + 8:
            continue                      # an empty list's self-loop
        prev_pa, _ = rd_va64(prev)
        if prev_pa is None:
            continue
        try:
            s_va = rd64(prev_pa)          # *(le_prev) == this proc
        except ValueError:
            continue
        if not is_kaddr(s_va):
            continue
        pa = walk(s_va)
        if pa != s_pa:
            continue
        return k, s_pa, s_va, nxt, prev
    return None


def main():
    pcomm = find_pcomm()
    print("p_comm pa 0x%x" % pcomm)
    found = find_proc(pcomm)
    if found is None:
        print("no linked proc found")
        return
    k, proc_pa, proc_va, nxt, prev = found
    print("proc pa 0x%x va 0x%x (p_comm offset 0x%x)" % (proc_pa, proc_va, k))
    print("p_list next 0x%x prev 0x%x (prev is allproc's head field)"
          % (nxt, prev))

    # p_pid: XNU's proc has pid_t p_pid well before p_comm, and launchd's is 1.
    pids = []
    for off in range(0, k, 4):
        try:
            if rd32(proc_pa + off) == 1:
                pids.append(off)
        except ValueError:
            break
    print("int32==1 inside the proc before p_comm: %s"
          % ["0x%x" % p for p in pids])

    # The task: the only kernel pointer inside the proc whose target holds the
    # proc's own address (task->bsd_info).
    tasks = []
    for off in range(0, 0x800, 8):
        try:
            v = rd64(proc_pa + off)
        except ValueError:
            break
        if not is_kaddr(v):
            continue
        t_pa = walk(v)
        if t_pa is None:
            continue
        back = None
        for toff in range(0, 0x1000, 8):
            try:
                w = rd64(t_pa + toff)
            except ValueError:
                break
            if w == proc_va:
                back = toff
                break
        if back is not None:
            tasks.append((off, v, t_pa, back))

    print("\nproc fields pointing at a struct that points back at the proc:")
    for off, v, t_pa, back in tasks:
        print("  proc+0x%x -> 0x%x (pa 0x%x), which holds the proc at +0x%x"
              % (off, v, t_pa, back))

    if not tasks:
        print("  none: dumping the first 0x300 of the proc for inspection")
        dump(proc_pa, 0x300, pcomm)
        return

    off, task_va, task_pa, bsd_off = tasks[0]
    print("\ntask: va 0x%x pa 0x%x  (bsd_info at task+0x%x)" % (task_va, task_pa, bsd_off))
    dump(task_pa, 0x300)


main()
