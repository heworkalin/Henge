#!/usr/bin/env python3
# Find vm_map / pmap structures by signature and dump their header fields.
#
#   (no arguments)                  scan all guest RAM, list every candidate
#   HENGE_MAP_PA=0x83fb8220         inspect one known map instead
#   HENGE_PMAP=0xfffffe...          inspect one known pmap instead
#   HENGE_VA=0x100fc0000            mark the map whose range covers this VA
#   HENGE_DETAIL=1                  also dump the map header words
#   gdb -p <pid> -batch -x tools/henge-gdb/vmmap.py
#
# Why discovery rather than hard-coded addresses: the map/pmap addresses move
# between runs (docs/locating-vm-structures-2026-11-17.md).

exec(open('/data/local/tmp/henge/tsbase.py').read().split('TH = {')[0])

MAP_PA = arg("HENGE_MAP_PA", None)
PMAP = arg("HENGE_PMAP", None)
DETAIL = arg("HENGE_DETAIL", 0)
VAS = arg_list("HENGE_VA")


def kheap(v):
    return (v >> 40) in (0xfffffe, 0xfffffd)


def show_pmap(name, pmap):
    print("\n%s = 0x%x" % (name, pmap))
    p = walk(pmap)
    if p is None:
        print("   pmap VA is unmapped (stale pointer?)")
        return
    ttb = p64(p + 8)
    note = "  (page-table base)" if (ttb & 0x3fff) == 0 and 0 < ttb < 0x100000000 else ""
    print("   pmap pa=0x%x  ttb0(pmap+0x08)=0x%x%s" % (p, ttb, note))
    for off in range(0, 0x40, 8):
        v = p64(p + off)
        note = ""
        if (v & 0x3fff) == 0 and 0x1000000 < v < 0x1000000000:
            note = "  <= page-table base?"
        print("      pmap+0x%02x = 0x%016x%s" % (off, v, note))


if PMAP is not None:
    show_pmap("pmap", PMAP)
    raise SystemExit

if MAP_PA is not None:
    cands = [dict(pa=MAP_PA, min=p64(MAP_PA + 0x20), max=p64(MAP_PA + 0x28),
                  nentries=u32(MAP_PA + 0x30), page_shift=u16(MAP_PA + 0x34),
                  pmap=p64(MAP_PA + 0x40), pmap_off=0x40)]
else:
    cands = maps()

print("vm_map candidates: %d" % len(cands))
for m in cands:
    mark = ""
    for va in VAS:
        if m["min"] <= (va & ~0x3fff) < m["max"]:
            mark = "  <== covers 0x%x" % va
    print("\n  @pa=0x%-10x min=0x%-12x max=0x%-12x nentries=%-6d page_shift=%d"
          % (m["pa"], m["min"], m["max"], m["nentries"], m["page_shift"]))
    print("     pmap@+0x%02x = 0x%-18x %s"
          % (m["pmap_off"], m["pmap"], "kernel heap" if kheap(m["pmap"]) else "(not a kptr!)"))
    ttb0 = pmap_ttb0(m["pmap"]) if kaddr(m["pmap"]) else None
    print("     ttb0        = %s" % (("0x%x" % ttb0) if ttb0 else "-"))
    if DETAIL:
        for off in range(0, 0x80, 8):
            print("        vm_map+0x%02x = 0x%016x" % (off, p64(m["pa"] + off)))
    if VAS:
        ents = map_entries(m["pa"])
        if ents:
            print("     entries walked: %d (first [0x%x,0x%x), last [0x%x,0x%x))"
                  % (len(ents), ents[0][1], ents[0][2], ents[-1][1], ents[-1][2]))
        else:
            print("     entries walked: 0")
    if m["pmap"] and kaddr(m["pmap"]):
        show_pmap("pmap of map@0x%x" % m["pa"], m["pmap"])
