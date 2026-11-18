#!/usr/bin/env python3
# Last userland page faults, decoded, with the map/entry/PTE state of each far.
#
#   HENGE_FAULTS=8            how many ring slots to print (default 8)
#   gdb -p <pid> -batch -x tools/henge-gdb/faults.py
#
# The ring is global and cross-process, so every slot is resolved against its
# own ttbr0 when the engine recorded one (hv_el0_flt_ttbr0[]) -- using "the last
# ttbr0" for all of them mismatches about half of them.

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

N = arg("HENGE_FAULTS", 8)
DFSC = {0x04: "trans-L0", 0x05: "trans-L1", 0x06: "trans-L2", 0x07: "trans-L3",
        0x09: "access-L1", 0x0b: "access-L2", 0x0d: "perm-L1", 0x0e: "perm-L2",
        0x0f: "perm-L3", 0x10: "sync-ext", 0x21: "align"}

n = i64("hv_el0_flt_n") or 0
print("hv_el0_flt_n = %d" % n)
if not n:
    raise SystemExit

maps_cache = None


def cached_maps():
    global maps_cache
    if maps_cache is None:
        maps_cache = maps()
    return maps_cache


print("%-6s %-11s %-11s %-10s %-11s %s" % ("slot", "pc", "far", "kind", "ttbr0", "entry / PTE"))
for i in range(max(0, n - N), n):
    s = i % 8
    pc = i64("hv_el0_flt_pc[%d]" % s)
    far = i64("hv_el0_flt_far[%d]" % s)
    esr = i64("hv_el0_flt_esr[%d]" % s)
    ttb = i64("hv_el0_flt_ttbr0[%d]" % s) or None
    if ttb is not None and i64("hv_el0_flt_ttbr0[0]") is None:
        ttb = None
    dfsc = esr & 0x3f
    kind = DFSC.get(dfsc, "0x%x" % dfsc)
    note = ""
    if far and (far >> 56) not in (0xff, 0xfe):
        pg = far & ~0x3fff
        m = find_map_for(pg, cached_maps())
        if m is None:
            note = "no vm_map covers this page"
        else:
            ents = map_entries(m["pa"])
            hit = entry_for(pg, m["pa"], ents)
            ttb0 = pmap_ttb0(m["pmap"]) if ttb is None else ttb
            pte = walk_ttbr(pg, ttb0) if ttb0 else None
            if hit:
                ent, st, en = hit
                note = ("map@0x%x entry[0x%x,0x%x) pages=%d pte=%s"
                        % (m["pa"], st, en, (en - st) // 0x4000,
                           ("0x%x" % pte) if pte else "NONE"))
            else:
                note = "map@0x%x NO ENTRY pte=%s" % (m["pa"], ("0x%x" % pte) if pte else "NONE")
    print("%-6d 0x%-9x 0x%-9x %-10s %-11s %s"
          % (i, pc, far, kind, ("0x%x" % ttb) if ttb else "-", note))
