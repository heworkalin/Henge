#!/usr/bin/env python3
# Per-entry residency: which vm_map_entries have no resident pages at all.
#
#   (no arguments)                 every discovered vm_map
#   HENGE_MAP_PA=0x83fb8220        one map
#   HENGE_VA=0x100fc0000           report the entry covering this page explicitly
#   HENGE_MAX_PAGES=64             skip entries larger than this (default 64)
#   HENGE_LIMIT=20                 how many zero/partial entries to list
#   gdb -p <pid> -batch -x tools/henge-gdb/entries.py
#
# This is the script that produced docs/fault-distribution-2026-11-17.md:
# a page can be registered in vm_map and still have PTE == 0, and the same
# page faults again, because nothing installs the mapping.

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

MAP_PA = arg("HENGE_MAP_PA", None)
FOCUS = arg_list("HENGE_VA")
MAXP = arg("HENGE_MAX_PAGES", 64)
LIMIT = arg("HENGE_LIMIT", 20)
PG = 0x4000

if MAP_PA is not None:
    maps_list = [dict(pa=MAP_PA, min=p64(MAP_PA + 0x20), max=p64(MAP_PA + 0x28),
                      nentries=u32(MAP_PA + 0x30), pmap=p64(MAP_PA + 0x40),
                      page_shift=u16(MAP_PA + 0x34), pmap_off=0x40)]
else:
    maps_list = maps()

for m in maps_list:
    ttb0 = pmap_ttb0(m["pmap"]) if kaddr(m["pmap"]) else None
    ents = map_entries(m["pa"])
    print("\n=== map @pa=0x%x  [0x%x,0x%x)  nentries=%d  pmap=0x%x  ttb0=%s ==="
          % (m["pa"], m["min"], m["max"], m["nentries"], m["pmap"],
             ("0x%x" % ttb0) if ttb0 else "-"))
    if not ents:
        print("   could not walk entries (stale map address?)")
        continue
    if ttb0 is None:
        print("   no ttb0: cannot test residency")
        continue

    small = [x for x in ents if 0 < (x[2] - x[1]) <= MAXP * PG]
    print("   entries walked: %d (%d are <= %d pages and resident-checked)"
          % (len(ents), len(small), MAXP))

    full, part, none = [], [], []
    for ent, s, e in small:
        pages = (e - s + PG - 1) // PG
        res = 0
        for k in range(pages):
            if walk_ttbr(s + k * PG, ttb0) is not None:
                res += 1
        if res == pages:
            full.append((ent, s, e, pages, res))
        elif res:
            part.append((ent, s, e, pages, res))
        else:
            none.append((ent, s, e, pages, res))

    print("   fully resident    : %d" % len(full))
    print("   partially resident: %d" % len(part))
    print("   NOT resident at all: %d" % len(none))
    if none:
        print("\n   -- entries with ZERO resident pages (first %d) --" % LIMIT)
        for ent, s, e, p, r in none[:LIMIT]:
            print("      entry@0x%-10x [0x%-11x,0x%-11x) %4d pages, 0 resident" % (ent, s, e, p))
    if part:
        print("\n   -- partially resident (first %d) --" % LIMIT)
        for ent, s, e, p, r in part[:LIMIT]:
            print("      entry@0x%-10x [0x%-11x,0x%-11x) %4d pages, %d resident"
                  % (ent, s, e, p, r))

    for va in FOCUS:
        pg = va & ~(PG - 1)
        hit = entry_for(pg, m["pa"], ents)
        pte = walk_ttbr(pg, ttb0)
        if hit:
            ent, s, e = hit
            print("\n   >>> 0x%x falls in entry@0x%x [0x%x,0x%x) pages=%d pte=%s"
                  % (va, ent, s, e, (e - s) // PG, ("0x%x" % pte) if pte else "NONE"))
        elif not (m["min"] <= pg < m["max"]):
            print("\n   >>> 0x%x is not in this map's range" % va)
        else:
            print("\n   >>> 0x%x is in this map's range but in NO entry (pte=%s)"
                  % (va, ("0x%x" % pte) if pte else "NONE"))
