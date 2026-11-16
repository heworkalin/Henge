#!/usr/bin/env python3
# Classify each blocked launchd thread: mutex wait or waitq/event wait.
#
# thread->wait_event is the waitq a thread is parked on; it is 0 when the
# thread is waiting on a lock's turnstile instead. Together with state and the
# turnstile owner (ts_proprietor) that separates "waiting for an event someone
# will post" from "waiting for a lock nobody holds any more", which is the
# difference between a slow path and a lost wakeup.
#
#   gdb -p <pid> -batch -x tools/dump-wait.py

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

TH = {0x01: "WAIT", 0x02: "SUSP", 0x04: "RUN", 0x08: "UNINT", 0x10: "TERM",
      0x20: "TERM2", 0x40: "WAITREP", 0x80: "IDLE", 0x100: "WAKING"}


def bits(v):
    return "+".join(n for b, n in TH.items() if v & b) or "-"


def kaddr(v):
    return v >> 56 in (0xff, 0xfe)


pcomm = find_pcomm()
k, ppa, pva = find_proc(pcomm)
ro_pa = walk(p64(ppa + 0x18))
task = p64(ro_pa + 8)
tpa = walk(task)
head = p64(tpa + 0x50)
threads = []
cur = head
for _ in range(32):
    if cur is None or not kaddr(cur) or cur in threads:
        break
    threads.append(cur)
    p = walk(cur)
    if p is None:
        break
    cur = p64(p)
    if cur == head:
        break
idle = [int(gdb.parse_and_eval("g_cpus[%d].tpidr_el1" % i)) for i in (0, 1)]
known = {t: "launchd%d" % i for i, t in enumerate(threads)}
for i, t in enumerate(idle):
    known[t] = "idle%d" % i
for i, t in enumerate(threads + idle):
    tp = walk(t)
    if tp is None:
        continue
    st = p64(tp + 0x1c8) & 0xffffffff
    ts = p64(tp + 0x28)
    we = p64(tp + 0x10)
    print("%s 0x%x state=0x%x(%s) wait_event=0x%x turnstile=0x%x"
          % (known[t], t, st, bits(st), we, ts))

for i in range(2):
    pc = int(gdb.parse_and_eval("g_cpus[%d].pc" % i))
    tp = int(gdb.parse_and_eval("g_cpus[%d].tpidr_el1" % i))
    print("cpu%d pc=0x%x tpidr_el1=0x%x" % (i, pc, tp))

tss = []
for t in threads:
    tp = walk(t)
    if tp is None:
        continue
    ts = p64(tp + 0x28)
    if kaddr(ts) and ts not in tss:
        tss.append(ts)
for ts in tss:
    tsp = walk(ts)
    print("\n===== turnstile 0x%x =====" % ts)
    for off in range(0x320, 0x380, 8):
        v = p64(tsp + off)
        note = ""
        if kaddr(v):
            note = "  <- kptr " + known.get(v, "(unknown)")
        print("  +0x%03x = 0x%x%s" % (off, v, note))
