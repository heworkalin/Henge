/*
 * Android/Linux compatibility: <mach/vm_map.h>
 *
 * devices/vmapple.c's alloc_no_footprint() is the only caller. On iOS it asks
 * Mach for anonymous pages, splits them into named memory entries and re-maps
 * each over the same range with MAP_MEM_LEDGER_TAGGED_ + LEDGER_NO_FOOTPRINT,
 * so the 4-8 GB of guest RAM never counts against the app's memory limit
 * (allowed once a debugger is attached, which is how iOS grants JIT at all).
 *
 * Android has no such ledger, so the whole dance collapses to one anonymous
 * mapping:
 *   vm_allocate()            -> mmap(..., MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE)
 *   mach_make_memory_entry_64 -> success, entry stays MACH_PORT_NULL
 *   mach_memory_entry_ownership -> success, nothing to tag
 *   vm_map()                 -> success; the range is already mapped
 *   vm_deallocate()          -> munmap()
 *   mach_port_deallocate()   -> no-op
 *
 * The caller's contract is preserved: it gets a zeroed writable range of the
 * requested size, or NULL when the mapping is refused.
 */
#pragma once

#include <mach/mach.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uintptr_t vm_address_t;
typedef uintptr_t vm_offset_t;
typedef size_t vm_size_t;
typedef int vm_prot_t;
typedef int vm_inherit_t;
typedef uint64_t memory_object_size_t;
typedef uint64_t memory_object_offset_t;
typedef mach_port_t mem_entry_name_port_t;

#define VM_PROT_NONE 0x00
#define VM_PROT_READ 0x01
#define VM_PROT_WRITE 0x02
#define VM_PROT_EXECUTE 0x04

#define VM_FLAGS_ANYWHERE 0x0001
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_NONE 2
#define VM_INHERIT_SHARE 0

kern_return_t vm_allocate(mach_port_t task, vm_address_t *address, vm_size_t size, int flags);
kern_return_t vm_deallocate(mach_port_t task, vm_address_t address, vm_size_t size);
kern_return_t mach_make_memory_entry_64(mach_port_t task, memory_object_size_t *size, memory_object_offset_t offset,
                                       vm_prot_t permission, mach_port_t *object_handle, mach_port_t parent_entry);
kern_return_t vm_map(mach_port_t task, vm_address_t *address, vm_size_t size, vm_offset_t mask, int flags,
                     mach_port_t object, vm_offset_t offset, int copy, vm_prot_t cur_protection,
                     vm_prot_t max_protection, vm_inherit_t inheritance);

#ifdef __cplusplus
}
#endif
