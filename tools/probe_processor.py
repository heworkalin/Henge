#!/usr/bin/env python3
# Calibrate the `state` offset: take the processor a thread says it runs on
# (thread+0x18, `__runq`), find the thread that processor is currently running
# (the idle thread of a CPU parked in idle), and read its state. An idle thread
# must carry TH_IDLE (0x80); if +0xf0 shows 0x80/0x84 there, +0xf0 is state.
#
#   gdb -p $(pidof host-install) -batch -ex "set print thread-events off" \
#       -x /data/local/tmp/henge/probe_processor.py

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


def looks_like_thread(va):
    """thread+0x08 is t_tro, and the thread_ro's first word is the thread."""
    pa = walk(va)
    if pa is None:
        return None
    try:
        tro = rd64(pa + 8)
    except ValueError:
        return None
    if not is_kaddr(tro):
        return None
    tro_pa = walk(tro)
    if tro_pa is None:
        return None
    try:
        if rd64(tro_pa) != va:
            return None
    except ValueError:
        return None
    return pa


def show_thread(tag, va):
    pa = looks_like_thread(va)
    if pa is None:
        print("  %s 0x%x: not a thread by the t_tro test" % (tag, va))
        return
    row = {}
    for off in (0x0f0, 0x0a0, 0x0b0, 0x0b8):
        try:
            row[off] = struct.unpack("<I", rd_pa(pa + off, 4))[0]
        except ValueError:
            row[off] = -1
    bits = "+".join(n for b, n in TH.items() if row[0x0f0] & b) or "-"
    # The state must agree with the queue links: a thread carrying TH_WAIT has
    # to be on a wait queue (its link words do not point at itself), and one
    # without TH_WAIT must not be.
    nxt, prev = rd64(pa), rd64(pa + 8)
    we = rd64(pa + 0x10)
    waitq = rd64(pa + 0x20)
    # Independent check, no offset guessing: TH_WAIT means "queued on a wait
    # queue", and thread.waitq is the queue it is on. So `state & TH_WAIT`
    # must be equivalent to waitq != 0. Any candidate offset in the struct that
    # tracks that across all threads is the real state.
    has_wait = (row[0x0f0] & 0x01) != 0
    agree = "AGREES" if has_wait == (waitq != 0) else "*** DISAGREES ***"
    print("  %s 0x%x:+0xf0=0x%x(%-14s) waitq=0x%-18x wait_event=0x%-6x links{0x%x,0x%x} %s"
          % (tag, va, row[0x0f0], bits, waitq, we, nxt, prev, agree))


def threads_of_task(label, task_va):
    task_pa = walk(task_va)
    head = rd64(task_pa + 0x50)
    print("%s: task 0x%x threads head 0x%x" % (label, task_va, head))
    out = []
    seen = []
    cur = head
    for _ in range(16):
        if not is_kaddr(cur) or cur in seen:
            break
        seen.append(cur)
        out.append(cur)
        show_thread("  thread", cur)
        pa = walk(cur)
        if pa is None:
            break
        cur = rd64(pa)
        if cur == head:
            break
    return out


def main():
    pcomm = find_pcomm()
    got = find_proc(pcomm)
    if not got:
        print("no proc")
        return
    k, proc_pa, proc_va = got

    # allproc is a LIST whose first field is p_list, so following p_list.next
    # from one proc walks every process. p_pid is at proc+0x60 (measured) and
    # p_comm at proc+0x5cc.
    procs = []
    cur = proc_va
    seen = set()
    while is_kaddr(cur) and cur not in seen and len(procs) < 64:
        seen.add(cur)
        pa = walk(cur)
        if pa is None:
            break
        try:
            comm = rd_pa(pa + k, 17).split(b"\0")[0].decode("latin-1")
            pid, stat = struct.unpack("<ii", rd_pa(pa + 0x60, 8))
            task_va = rd64(walk(rd64(pa + 0x18)) + 8)
        except ValueError:
            break
        procs.append((pid, comm, stat, task_va, cur))
        try:
            cur = rd64(pa)
        except ValueError:
            break

    print("allproc: " + ", ".join("%s(pid %d, p_stat %d)" % (c, p, s)
                                  for p, c, s, _, _ in procs))
    for pid, comm, stat, task_va, _ in procs:
        threads_of_task("%s pid %d" % (comm, pid), task_va)


main()
