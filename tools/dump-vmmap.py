#!/usr/bin/env python3
# Find the vm_map_entry that covers the address the guest died trying to execute,
# then walk that entry to its vm_object and pager -- i.e. name the thing the last
# page fault was for.
#
# Layout (osfmk/vm/vm_map_store_internal.h):
#   struct vm_map_links { prev; next; start; end; }      // +0x00,+0x08,+0x10,+0x18
#   struct vm_map_header { struct vm_map_links links; nentries; page_shift; ... }
#   struct vm_map { struct vm_map_header hdr; ... }
# so a map's head entry is at map+0x08 and entries chain through entry+0x08.
#
#   gdb -p $(pidof host-install) -batch -ex "set print thread-events off" -x tools/dump-vmmap.py

import struct

import gdb

# The address to look up: the last userland fault, taken from the ring rather than
# hard-coded, because the stall point moves between runs (0x100ea0924 once,
# 0x100420288 another time) -- itself a sign the failure is timing-dependent.
_n = int(gdb.parse_and_eval("hv_uexc_n"))
_s = (_n - 1) & 63
TARGET = int(gdb.parse_and_eval("hv_uexc_pc[%d]" % _s))
_far = int(gdb.parse_and_eval("hv_uexc_far[%d]" % _s))
if _far and _far >> 56 not in (0xff, 0xfe):
    TARGET = _far
print("last userland fault: pc 0x%x far 0x%x, looking up 0x%x"
      % (int(gdb.parse_and_eval("hv_uexc_pc[%d]" % _s)), _far, TARGET))

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])


def looks_like_map(pa):
    prev, nxt = p64(pa + 0x00), p64(pa + 0x08)
    if not (prev >> 56 in (0xff, 0xfe) and nxt >> 56 in (0xff, 0xfe)):
        return False
    st, en = p64(pa + 0x10), p64(pa + 0x18)
    return st <= en


pcomm = find_pcomm()
k, ppa, pva = find_proc(pcomm)
ro_pa = walk(p64(ppa + 0x18))
task = p64(ro_pa + 8)
tpa = walk(task)
print("task 0x%x pa 0x%x" % (task, tpa))

maps = []
for off in range(0, 0x100, 8):
    v = p64(tpa + off)
    if v >> 56 not in (0xff, 0xfe):
        continue
    p = walk(v)
    if p is None or not looks_like_map(p):
        continue
    maps.append((off, v, p))
    print("  task+0x%x -> 0x%x looks like a vm_map (nentries %d page_shift %d)"
          % (off, v, p64(p + 0x20), struct.unpack("<H", rd_pa(p + 0x24, 2))[0]))

for off, mv, mp in maps:
    head = p64(mp + 0x08)
    print("\nmap at task+0x%x: head entry 0x%x" % (off, head))
    cur = head
    seen = set()
    n = 0
    while cur and cur not in seen and cur != mv and n < 2000:
        seen.add(cur)
        ep = walk(cur)
        if ep is None:
            print("  entry 0x%x unmapped" % cur)
            break
        s, e = p64(ep + 0x10), p64(ep + 0x18)
        if s <= TARGET < e:
            print("  HIT entry 0x%x pa 0x%x [0x%x,0x%x)  (covers 0x%x)"
                  % (cur, ep, s, e, TARGET))
            for o in range(0, 0xc0, 8):
                v = p64(ep + o)
                tag = ""
                if v >> 56 in (0xff, 0xfe):
                    p2 = walk(v)
                    tag = "  -> pa %s" % ("0x%x" % p2 if p2 else "none")
                print("     entry+0x%03x = 0x%016x%s" % (o, v, tag))
            break
        cur = p64(ep + 0x08)
        n += 1
    else:
        print("  no entry covers 0x%x (walked %d)" % (TARGET, n))


main = None
