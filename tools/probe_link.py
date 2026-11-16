#!/usr/bin/env python3
# Decide what the task's threads queue actually links to.
#
# A queue_head_t in XNU is a circular list: a one-element queue reads
# next == prev == &element->link. What that element *is* is the open question,
# so this does not assume: it takes the link target L, and for every kernel
# pointer W in [L, L+0x400) it reads the object W names and prints its first
# word. `struct thread_ro { struct thread *tro_owner; ... }` means the thread_ro
# is the one whose first word equals L -- that both identifies the link as
# t_tro and gives the thread's start.
#
#   gdb -p $(pidof host-install) -batch -ex "set print thread-events off" \
#       -x /data/local/tmp/henge/probe_link.py

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
        print("no proc")
        return
    k, proc_pa, proc_va = got
    ro_pa = walk(rd64(proc_pa + 0x18))
    task_va = rd64(ro_pa + 8)
    task_pa = walk(task_va)
    print("proc 0x%x  task 0x%x (pa 0x%x)" % (proc_va, task_va, task_pa))

    # Print the whole task head so the fields before the queue can be read
    # against the source by eye, not by arithmetic.
    dump(task_pa, 0x80, {task_pa: "<-- task"})

    link = rd64(task_pa + 0x50)
    print("\ntask+0x50 = 0x%x (prev 0x%x)" % (link, rd64(task_pa + 0x58)))
    link_pa = walk(link)
    if link_pa is None:
        print("untranslatable")
        return
    dump(link_pa, 0x80, {link_pa: "<-- link target"})

    # `link` itself is the thread: its t_tro (found above) points at a
    # thread_ro whose tro_owner is `link`. So read the thread as the source
    # says it is laid out: links union, wait_event, __runq, waitq, turnstile,
    # inheritor, ... and pull out state along the way.
    TH = {"TH_WAIT": 0x01, "TH_SUSP": 0x02, "TH_RUN": 0x04,
          "TH_UNINT": 0x08, "TH_TERMINATE": 0x10, "TH_WAIT_REPORT": 0x40,
          "TH_IDLE": 0x80, "TH_WAKING": 0x100}
    we = rd64(link_pa + 0x10)
    print("\nthread 0x%x pa 0x%x" % (link, link_pa))
    print("  wait_event (thread+0x10) = 0x%x" % we)
    print("  wait_links next 0x%x prev 0x%x"
          % (rd64(link_pa), rd64(link_pa + 8)))
    print("  __runq 0x%x  waitq 0x%x  turnstile 0x%x  inheritor 0x%x"
          % (rd64(link_pa + 0x18), rd64(link_pa + 0x20),
             rd64(link_pa + 0x28), rd64(link_pa + 0x30)))
    print("\n  words in [0,0x600) whose 32-bit value is a plausible TH_* mask:")
    for off in range(0, 0x600, 4):
        try:
            v = struct.unpack("<I", rd_pa(link_pa + off, 4))[0]
        except ValueError:
            break
        if v == 0 or v & ~0x1ff:
            continue
        names = "+".join(n for n, b in TH.items() if v & b)
        print("    thread+0x%03x = 0x%-4x %s" % (off, v, names))

    print("\nobjects named by pointers inside [link, link+0x400), first word of each:")
    for off in range(0, 0x400, 8):
        try:
            v = rd64(link_pa + off)
        except ValueError:
            break
        if not is_kaddr(v):
            continue
        v_pa = walk(v)
        if v_pa is None:
            continue
        try:
            first = rd64(v_pa)
        except ValueError:
            continue
        tag = ""
        if first == link:
            tag = "  <-- tro_owner == link: this is the thread_ro"
        elif first == task_va:
            tag = "  <-- first word is the task"
        elif first == proc_va:
            tag = "  <-- first word is the proc"
        print("  +0x%03x 0x%016x -> pa 0x%x  [pa]=0x%x%s"
              % (off, v, v_pa, first, tag))


main()
