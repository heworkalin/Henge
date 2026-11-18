#!/usr/bin/env python3
# List processes, and optionally each thread's state / wait event / turnstile.
#
#   (no arguments)              every process with pid/ppid/stat/comm
#   HENGE_COMM=restored         only this process (p_comm, truncated to 16 chars)
#   HENGE_THREADS=1             also list that process's threads
#   HENGE_TURNSTILE=1           also dump each distinct turnstile (+0x320..0x380)
#   gdb -p <pid> -batch -x tools/henge-gdb/procs.py
#
# p_comm is char[17]; "restored_external" is truncated to "restored_externa",
# so pass the truncated name.

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

WANT = arg("HENGE_COMM", None, str)
THREADS = arg("HENGE_THREADS", 0)
TSDUMP = arg("HENGE_TURNSTILE", 0)
LIST_OFF = 0x8


def cstr(pa, n=32):
    raw = try_rd_pa(pa, n)
    return None if raw is None else raw.split(b"\0")[0].decode("ascii", "replace")


def proc_fields(pa, comm_off):
    return (p64(pa + 0x60) & 0xffffffff, p64(pa + 0x38) & 0xffffffff,
            p64(pa + 0x64) & 0xffffffff, cstr(pa + comm_off))


def show_proc(pva):
    pa = walk(pva)
    if pa is None:
        return
    pid, ppid, stat, comm = proc_fields(pa, comm_off)
    print("   proc 0x%-14x pid=%-5d ppid=%-5d stat=0x%-4x comm=%r"
          % (pva, pid, ppid, stat, comm))


pc = find_pcomm(WANT)
if pc is None:
    print("p_comm %r not found in guest RAM" % (WANT or "launchd"))
    raise SystemExit
k, ppa, pva = find_proc(pc)
comm_off = pc - ppa
print("%r: p_comm pa=0x%x proc=0x%x list-node offset=0x%x"
      % (cstr(pc), pc, pva, k))

show_proc(pva)
node = pva + (comm_off - k)
seen = {pva}
for _ in range(64):
    p = walk(node)
    if p is None:
        break
    nxt = p64(p)
    if not kaddr(nxt) or nxt in seen:
        break
    nv = nxt - (comm_off - k)
    if nv in seen:
        break
    seen.add(nv)
    show_proc(nv)
    node = nxt

pva2, task = proc_task(WANT)
if THREADS and task:
    _t, count, ths = task_threads(task)
    print("\ntask 0x%x: %d threads" % (task, count))
    for i, t in enumerate(ths):
        tp = walk(t)
        if tp is None:
            continue
        st = p64(tp + 0x1c8) & 0xffffffff
        we = p64(tp + 0x10)
        ts = p64(tp + 0x28)
        print("   thread 0x%-14x state=0x%-4x(%-12s) wait_event=0x%-14x turnstile=0x%x"
              % (t, st, th_bits(st), we, ts))
    if TSDUMP:
        tss = []
        for t in ths:
            tp = walk(t)
            if tp is None:
                continue
            ts = p64(tp + 0x28)
            if kaddr(ts) and ts not in tss:
                tss.append(ts)
        for ts in tss:
            tsp = walk(ts)
            print("\n   ===== turnstile 0x%x =====" % ts)
            for off in range(0x320, 0x380, 8):
                v = p64(tsp + off)
                note = ""
                if kaddr(v):
                    note = "  <- kptr"
                print("      +0x%03x = 0x%x%s" % (off, v, note))
