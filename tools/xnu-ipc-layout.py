#!/usr/bin/env python3
"""Offsets for the IPC walk (xnu-11417, arm64 release), computed by ctypes.

Only the fields the engine actually reads are modelled; each is copied from
osfmk/ipc/*.h with its source line noted. Anything whose size depends on a union
we do not model (struct ipc_port) is left to the runtime self-check instead of
being guessed here.
"""
import ctypes as C

def show(name, fields):
    cls = type(name, (C.Structure,), {"_fields_": fields})
    print(f"{name}: sizeof = {C.sizeof(cls):#x}")
    for f, _ in fields:
        print(f"  {f:<18} +{getattr(cls, f).offset:#04x}")
    print()
    return cls

# lck_ticket_t is a 64-bit ticket (osfmk/kern/lck_ticket.h).
# os_ref_atomic_t / ipc_entry_num_t are 32-bit.
# SMR_POINTER(ipc_entry_table_t) is a pointer (8), task_t is a pointer (8).
show("ipc_space", [
    ("is_lock", C.c_uint64),
    ("is_bits", C.c_uint32),
    ("is_table_hashed", C.c_uint32),
    ("is_table_free", C.c_uint32),
    ("_pad", C.c_uint32),
    ("is_table", C.c_uint64),
    ("is_task", C.c_uint64),
])

# struct ipc_entry: ie_object (8) + union{ struct { ie_bits(4), ie_next(4),
# ie_dist(4), ie_index(4) }, struct smr_node } -> 24 bytes total.
show("ipc_entry", [
    ("ie_object", C.c_uint64),
    ("ie_bits", C.c_uint32),
    ("ie_next", C.c_uint32),
    ("ie_dist", C.c_uint32),
    ("ie_index", C.c_uint32),
])
