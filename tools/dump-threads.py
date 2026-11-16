#!/usr/bin/env python3
# Every thread of the launchd task, with what each one is waiting on.
#
#   gdb -p <pid> -batch -x tools/dump-threads.py
exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

TH = {0x01: "WAIT", 0x02: "SUSP", 0x04: "RUN", 0x08: "UNINT", 0x10: "TERM",
      0x20: "TERM2", 0x40: "WAITREP", 0x80: "IDLE", 0x100: "WAKING"}


def bits(v):
    return "+".join(n for b, n in TH.items() if v & b) or "-"


pcomm = find_pcomm()
k, ppa, pva = find_proc(pcomm)
ro_pa = walk(p64(ppa + 0x18))
task = p64(ro_pa + 8)
tpa = walk(task)
print("task 0x%x (pa 0x%x) threads head 0x%x" % (task, tpa, p64(tpa + 0x50)))
head = p64(tpa + 0x50)
ths = []
cur = head
for _ in range(64):
    if cur is None or cur >> 56 not in (0xff, 0xfe) or cur in ths:
        break
    ths.append(cur)
    p = walk(cur)
    if p is None:
        break
    cur = p64(p)
    if cur == head:
        break
print("%d threads" % len(ths))
for i, t in enumerate(ths):
    p = walk(t)
    st = p64(p + 0x1c8) & 0xffffffff
    ts = p64(p + 0x28)
    print("  launchd%d 0x%x state=0x%x(%s) turnstile=0x%x" % (i, t, st, bits(st), ts))
