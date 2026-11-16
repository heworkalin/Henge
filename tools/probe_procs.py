#!/usr/bin/env python3
# Walk allproc, list every process with its p_stat and its task, then use
# kernel_task's threads as a calibration object for `struct thread` field
# offsets.
#
# Why: `state` cannot be located by reading osfmk/kern/thread.h, because the
# running kernel's layout does not match that header (measured: t_tro sits at
# thread+0x08, while the header puts it 396 lines into the struct). So the
# offset has to be *calibrated*: the idle thread of kernel_task must carry
# TH_IDLE (0x80) in its state, and a thread blocked in a wait queue must carry
# TH_WAIT (0x01). Only an offset whose value tracks those two facts is state.
#
#   gdb -p $(pidof host-install) -batch -ex "set print thread-events off" \
#       -x /data/local/tmp/henge/probe_procs.py

import struct

import gdb

INF = gdb.selected_inferior()
OA_MASK = 0x0000ffffffffffff
CMD = 17
PSTAT = {1: "SIDL", 2: "SRUN", 3: "SSLEEP", 4: "SSTOP", 5: "SZOMB"}
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


def proc_info(proc_va, k):
    p_pa = walk(proc_va)
    if p_pa is None:
        return None
    comm = rd_pa(p_pa + k, 17).split(b"\0")[0].decode("latin-1")
    # Measured: p_pid at proc+0x60, p_stat right after it; p_comm at proc+0x5cc.
    pid, stat = struct.unpack("<ii", rd_pa(p_pa + k - 0x56c, 8))
    ro = rd64(p_pa + 0x18)
    ro_pa = walk(ro)
    task_va = rd64(ro_pa + 8) if ro_pa else None
    return {"va": proc_va, "pa": p_pa, "comm": comm, "pid": pid,
            "stat": stat, "task": task_va, "ro": ro}


def main():
    pcomm = find_pcomm()
    got = find_proc(pcomm)
    if not got:
        print("no launchd proc")
        return
    k, proc_pa, proc_va = got
    print("p_comm offset in proc: 0x%x" % k)

    # allproc is a LIST: p_list is the first field, so a proc's own VA is its
    # p_list, and prev points at the head field that holds it.
    procs = []
    cur = proc_va
    seen = set()
    while cur and cur not in seen and len(procs) < 200:
        seen.add(cur)
        info = proc_info(cur, k)
        if info is None:
            break
        procs.append(info)
        try:
            cur = rd64(info["pa"])
        except ValueError:
            break

    print("\n%d processes in allproc:" % len(procs))
    for p in procs:
        print("  pid %-5d %-24s p_stat=%-6s task 0x%x"
              % (p["pid"], p["comm"], PSTAT.get(p["stat"], "?%d" % p["stat"]),
                 p["task"] or 0))

    # kernel_task is pid 0. Its task's threads give the calibration object.
    kt = next((p for p in procs if p["pid"] == 0), None)
    if kt is None:
        print("\nno pid 0 in allproc")
        return
    t_pa = walk(kt["task"])
    print("\nkernel_task 0x%x pa 0x%x" % (kt["task"], t_pa))
    # The threads queue we already know how to read: a queue_head_t whose link
    # target is a thread (its thread_ro points back at it). Enumerate the ring.
    first = rd64(t_pa + 0x50)
    print("  threads head 0x%x (prev 0x%x)" % (first, rd64(t_pa + 0x58)))
    cur = first
    for _ in range(8):
        if not is_kaddr(cur) or cur == 0:
            break
        th_pa = walk(cur)
        if th_pa is None:
            print("  link 0x%x untranslatable" % cur)
            break
        print("  thread 0x%x:" % cur)
        for off in range(0, 0x200, 4):
            v = struct.unpack("<I", rd_pa(th_pa + off, 4))[0]
            if 0 < v < 0x200 and (v & ~0x1ff) == 0 and off in (
                    0xf0, 0xa0, 0xb0, 0xb8, 0xcc, 0x13c, 0x1c8):
                bits = "+".join(n for b, n in TH.items() if v & b)
                if bits:
                    print("    +0x%03x = 0x%x (%s)" % (off, v, bits))
        # queue_chain_t is two pointers; follow its "next" by reading the
        # thread's own link slot at the offset the ring told us.
        nxt = rd64(th_pa)
        if nxt == cur or nxt == first:
            break
        cur = nxt


main()
