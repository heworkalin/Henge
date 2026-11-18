#!/usr/bin/env python3
# Walk guest VAs through the guest's own page tables and print every descriptor.
#
#   HENGE_VA=0x100fc0000              one VA (or a comma separated list)
#   HENGE_RANGE=0x100f8000:0x100fc0000[:0x4000]   a sweep (step defaults to 16 KiB)
#   HENGE_TTBR0=0x...                 walk a recorded ttbr0 instead of the CPU's
#   HENGE_ENTRY=1                     also print the covering vm_map_entry
#   gdb -p <pid> -batch -x tools/henge-gdb/walkva.py
#
# Use this instead of hand-computing indices: the guest is 16 KiB granule, so
# 4 KiB arithmetic gives wrong indices (docs/investigation-tree.md #5).

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

VAS = arg_list("HENGE_VA")
RNG = arg_range("HENGE_RANGE")
if RNG and not VAS:
    lo, hi, step = RNG
    VAS = list(range(lo, hi, step))
if not VAS:
    # Nothing given: the page the last userland fault was for.
    far = i64("hv_last_el0_va") or 0
    VAS = [far & ~0x3fff] if far else []
    if not VAS:
        print("set HENGE_VA=0x... or HENGE_RANGE=lo:hi[:step]")
        raise SystemExit

ttbr_arg = arg("HENGE_TTBR0", None)
ttbr0 = ttbr_arg if ttbr_arg is not None else cpu("ttbr0_el1")
print("ttbr0 = 0x%x  (16 KiB granule, TG0=%d)"
      % (ttbr0, (cpu("tcr_el1") >> 14) & 3))

show_entry = arg("HENGE_ENTRY", 0)
mcache = maps() if show_entry else []

for va in VAS:
    trail = []
    pa = walk_ttbr(va, ttbr0, trail=trail)
    print("\nVA 0x%x -> %s" % (va, ("PA 0x%x" % pa) if pa else "UNMAPPED"))
    for level, base, idx, d in trail:
        print("   L%d table=0x%-10x idx=%-5d desc=0x%016x%s"
              % (level, base, idx, d,
                 "" if d & 1 else "  (invalid -> stop)"))
    if pa is not None and trail and (trail[-1][3] & 3) == 3:
        d = trail[-1][3]
        print("   AF=%d AP=%d UXN=%d PXN=%d"
              % ((d >> 10) & 1, (d >> 6) & 3, (d >> 54) & 1, (d >> 53) & 1))
    if show_entry and pa is not None:
        pg = va & ~0x3fff
        m = find_map_for(pg, mcache)
        if m is None:
            print("   no vm_map covers this page")
        else:
            hit = entry_for(pg, m["pa"])
            if hit:
                ent, s, e = hit
                print("   map@0x%x entry@0x%x [0x%x,0x%x) pages=%d"
                      % (m["pa"], ent, s, e, (e - s) // 0x4000))
            else:
                print("   map@0x%x: NO entry covers this page" % m["pa"])
