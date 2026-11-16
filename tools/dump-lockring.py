#!/usr/bin/env python3
# Follow the turnstile chain to see whether the waiters form a cycle.
#
# struct turnstile (osfmk/kern/turnstile.h) ends with
#     thread_t ts_thread;   /* thread the turnstile is attached to */
# and our dumps put that field at turnstile+0x350. Each thread carries the
# turnstile it is waiting on at thread+0x28. Following
#     thread -> turnstile -> ts_thread -> ... 
# either terminates (someone is runnable / no turnstile) or closes a loop, and a
# loop is a deadlock: every member waits on a lock held by the next member.
#
#   gdb -p $(pidof host-install) -batch -ex "set print thread-events off" -x tools/dump-lockring.py

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

TH = {0x01: "WAIT", 0x02: "SUSP", 0x04: "RUN", 0x08: "UNINT", 0x10: "TERM",
      0x20: "TERM2", 0x40: "WAITREP", 0x80: "IDLE", 0x100: "WAKING"}
TS_THREAD = 0x350     # measured; the last kernel pointer in the turnstile


def bits(v):
    return "+".join(n for b, n in TH.items() if v & b) or "-"


pcomm = find_pcomm()
k, ppa, pva = find_proc(pcomm)
ro_pa = walk(p64(ppa + 0x18))
task = p64(ro_pa + 8)
tpa = walk(task)
head = p64(tpa + 0x50)
threads = []
cur = head
for _ in range(16):
    if cur is None or cur >> 56 not in (0xff, 0xfe) or cur in threads:
        break
    threads.append(cur)
    p = walk(cur)
    if p is None:
        break
    cur = p64(p)
    if cur == head:
        break
print("launchd threads: " + ", ".join("0x%x" % t for t in threads))

seen = set()
cur = threads[0] if threads else None
for step in range(12):
    if cur is None or cur in seen:
        print("  -> cycle closed at 0x%x after %d hops" % (cur or 0, step))
        break
    seen.add(cur)
    tp = walk(cur)
    if tp is None:
        print("  thread 0x%x unmapped" % cur)
        break
    st = p64(tp + 0x1c8) & 0xffffffff
    ts = p64(tp + 0x28)
    print("  [%d] thread 0x%x state=0x%x(%s) waiting-on turnstile 0x%x"
          % (step, cur, st, bits(st), ts))
    if ts == 0 or ts >> 56 not in (0xff, 0xfe):
        print("      no turnstile: this thread is not waiting on a lock")
        break
    tsp = walk(ts)
    if tsp is None:
        print("      turnstile unmapped")
        break
    nxt = p64(tsp + TS_THREAD)
    print("      turnstile 0x%x ts_thread(+0x%x) -> 0x%x" % (ts, TS_THREAD, nxt))
    if nxt >> 56 not in (0xff, 0xfe):
        print("      ts_thread is not a thread; chain ends here")
        break
    cur = nxt
