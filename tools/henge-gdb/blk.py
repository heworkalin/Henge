#!/usr/bin/env python3
# Block-device counters: is the guest waiting on disk I/O we never completed?
#
#   gdb -p <pid> -batch -x tools/henge-gdb/blk.py
#
# Offsets are HvVmapple's bdif slots (root_blk at +168, aux_blk at +160):
#   requests +9016, submitted +9040, completed +9044, inflight +9052
# A growing "submitted - completed" with the guest idle means the I/O never
# came back; equal counters mean the guest is not waiting on us.

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

board = int(gdb.parse_and_eval("&g_board"))
print("HvVmapple @0x%x" % board)
for name, off in (("root_blk", 168), ("aux_blk", 160)):
    b = p64(board + off)
    if b == 0:
        print("%-9s NULL" % name)
        continue
    requests = p64(b + 9016)
    submitted = u32(b + 9040)
    completed = u32(b + 9044)
    inflight = struct.unpack("<i", rd_pa(b + 9052, 4))[0]
    print("%-9s 0x%x  requests=%-6d submitted=%-6d completed=%-6d inflight=%d%s"
          % (name, b, requests, submitted, completed, inflight,
             "   <== outstanding" if submitted != completed else ""))
