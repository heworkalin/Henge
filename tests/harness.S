// Running a test program natively with every register taken from a HvCpu.
//
// hv_native_run(HvCpu *cpu, void *entry) saves the host state and jumps to a
// program's native entry (its PROLOGUE), which loads all guest registers from
// the HvCpu, x28 last. The program's EPILOGUE needs a base register without
// one to spare, so it pushes x0/x1 onto the guest stack, loads the HvCpu
// pointer from a global, saves everything, and comes back to hv_native_return.

    .text
    .p2align 2

// sym@PAGE is Mach-O; ELF wants a plain adrp plus :lo12:. See tests/harness.h.
#if defined(__APPLE__)
#define ADRP_SYM(r, s)      adrp r, s@PAGE
#define ADD_SYM(r, s, t)    add r, s, t@PAGEOFF
#else
#define ADRP_SYM(r, s)      adrp r, s
#define ADD_SYM(r, s, t)    add r, s, :lo12:t
#endif
    .globl _hv_native_run
    .globl hv_native_run
    .globl _hv_native_return
    .globl hv_native_return
    .globl _hv_native_cpu
    .globl hv_native_cpu

_hv_native_run:
hv_native_run:
    stp x29, x30, [sp, #-160]!
    stp x19, x20, [sp, #16]
    stp x21, x22, [sp, #32]
    stp x23, x24, [sp, #48]
    stp x25, x26, [sp, #64]
    stp x27, x28, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    ADRP_SYM(x9, _hv_native_cpu)
    ADD_SYM(x9, x9, _hv_native_cpu)
    str x0, [x9]
    mov x10, sp
    str x10, [x9, #8]
    mov x28, x0
    br x1

_hv_native_return:
hv_native_return:
    ADRP_SYM(x9, _hv_native_cpu)
    ADD_SYM(x9, x9, _hv_native_cpu)
    ldr x10, [x9, #8]
    mov sp, x10
    ldp d14, d15, [sp, #144]
    ldp d12, d13, [sp, #128]
    ldp d10, d11, [sp, #112]
    ldp d8, d9, [sp, #96]
    ldp x27, x28, [sp, #80]
    ldp x25, x26, [sp, #64]
    ldp x23, x24, [sp, #48]
    ldp x21, x22, [sp, #32]
    ldp x19, x20, [sp, #16]
    ldp x29, x30, [sp], #160
    ret

    .data
    .p2align 3
_hv_native_cpu:
hv_native_cpu:
    .quad 0     // HvCpu *
    .quad 0     // host SP

