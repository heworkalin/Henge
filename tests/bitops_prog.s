// Instructions a bit reader is built from, with immediates only.
//
// No inputs: the program seeds its own registers with movi/movz and then applies one operation
// after another, storing every result to the buffer in x0. Running the same bytes natively and
// through the JIT and comparing the buffers therefore tests the operations and nothing else.
//
// GNU syntax on purpose: this is assembled by the host test script, not by the Darwin Makefile.

    .text
    .globl  bitops_prog
    .type   bitops_prog, %function
bitops_prog:
    // Callee-saved state, kept: this program clobbers x19..x24 and every SIMD register it uses,
    // and a caller that finds its own v8..v15 changed crashes somewhere else entirely.
    stp     x19, x20, [sp, #-208]!
    stp     x21, x22, [sp, #16]
    stp     x23, x24, [sp, #32]
    stp     x25, x26, [sp, #48]
    stp     x27, x28, [sp, #64]
    stp     q8, q9, [sp, #80]
    stp     q10, q11, [sp, #112]
    stp     q12, q13, [sp, #144]
    stp     q14, q15, [sp, #176]
    // Operands come from the buffer, so the same program can be run over and over with different
    // values: the identical bytes are loaded by the native run and by the JIT run.
    add     x25, x0, #0x400
    ldp     x1, x2, [x0]
    ldp     x3, x4, [x0, #16]
    ldp     x5, x6, [x0, #32]
    ldp     x7, x8, [x0, #48]
    ldp     x9, x10, [x0, #64]
    ldp     x11, x12, [x0, #80]
    ldp     x13, x14, [x0, #96]
    ldp     x15, x16, [x0, #112]
    ldp     x17, x18, [x0, #128]
    ldp     x19, x20, [x0, #144]
    ldp     x21, x22, [x0, #160]
    ldp     x23, x24, [x0, #176]
    ldp     q0, q1, [x0, #192]
    ldp     q2, q3, [x0, #224]
    ldp     q4, q5, [x0, #256]
    ldp     q6, q7, [x0, #288]
    // bitfields
    ubfx    x6, x1, #3, #11
    sbfx    x7, x1, #17, #23
    bfxil   x8, x2, #9, #19
    ubfiz   x9, x2, #11, #13
    sbfiz   x10, x3, #5, #27
    extr    x11, x1, x2, #29
    extr    x12, x2, x1, #7
    ror     x13, x1, #13
    clz     x14, x1
    cls     x15, x2
    rbit    x16, x1
    rev     x17, x1
    rev32   x18, x1
    sxtb    x19, w1
    sxth    x20, w2
    uxtb    w21, w2
    // bitfield move forms
    sbfm    x22, x1, #4, #27
    ubfm    x23, x2, #7, #31
    bfm     x24, x1, #9, #23
    // NEON, immediate seeds
    movi    v0.16b, #0x5a
    movi    v1.16b, #0xa5
    movi    v2.8h, #0x12, lsl #8
    movi    v3.4s, #0xff, lsl #16
    eor     v4.16b, v0.16b, v1.16b
    and     v5.16b, v0.16b, v2.16b
    orr     v6.16b, v1.16b, v3.16b
    bic     v7.16b, v0.16b, v1.16b
    not     v8.16b, v0.16b
    cnt     v9.16b, v1.16b
    ushll   v10.8h, v0.8b, #3
    ushll2  v11.8h, v0.16b, #2
    sshll   v12.8h, v1.8b, #1
    sshll2  v13.8h, v1.16b, #4
    xtn     v14.8b, v2.8h
    sqxtn   v15.8b, v2.8h
    uqxtn   v16.8b, v2.8h
    addv    b17, v0.16b
    addv    h18, v2.8h
    addp    v19.16b, v0.16b, v1.16b
    zip1    v20.16b, v0.16b, v1.16b
    zip2    v21.16b, v0.16b, v1.16b
    uzp1    v22.16b, v0.16b, v1.16b
    trn1    v23.16b, v0.16b, v1.16b
    ext     v24.16b, v0.16b, v1.16b, #5
    tbl     v25.16b, {v0.16b}, v1.16b
    tbx     v26.16b, {v0.16b}, v1.16b
    shl     v27.8h, v2.8h, #7
    sshr    v28.8h, v2.8h, #3
    ushr    v29.8h, v2.8h, #2
    sli     v30.8h, v2.8h, #5
    sri     v31.8h, v2.8h, #4
    // store everything
    stp     x1, x2, [x25]
    stp     x3, x4, [x25, #16]
    stp     x5, x6, [x25, #32]
    stp     x7, x8, [x25, #48]
    stp     x9, x10, [x25, #64]
    stp     x11, x12, [x25, #80]
    stp     x13, x14, [x25, #96]
    stp     x15, x16, [x25, #112]
    stp     x17, x18, [x25, #128]
    stp     x19, x20, [x25, #144]
    stp     x21, x22, [x25, #160]
    stp     x23, x24, [x25, #176]
    stp     q0, q1, [x25, #192]
    stp     q2, q3, [x25, #224]
    stp     q4, q5, [x25, #256]
    stp     q6, q7, [x25, #288]
    stp     q8, q9, [x25, #320]
    stp     q10, q11, [x25, #352]
    stp     q12, q13, [x25, #384]
    stp     q14, q15, [x25, #416]
    stp     q16, q17, [x25, #448]
    stp     q18, q19, [x25, #480]
    stp     q20, q21, [x25, #512]
    stp     q22, q23, [x25, #544]
    stp     q24, q25, [x25, #576]
    stp     q26, q27, [x25, #608]
    stp     q28, q29, [x25, #640]
    stp     q30, q31, [x25, #672]
    ldp     x19, x20, [sp]
    ldp     x21, x22, [sp, #16]
    ldp     x23, x24, [sp, #32]
    ldp     x25, x26, [sp, #48]
    ldp     x27, x28, [sp, #64]
    ldp     q8, q9, [sp, #80]
    ldp     q10, q11, [sp, #112]
    ldp     q12, q13, [sp, #144]
    ldp     q14, q15, [sp, #176]
    add     sp, sp, #208
    ret
    .size   bitops_prog, .-bitops_prog

// ---- the match copier: unaligned, and across a page boundary ----
//
// A decoder copies a run of bytes with ldp/stp of sixteen bytes at an address that is almost never
// aligned, and a run that starts near the end of a page crosses into the next one. That case takes
// the engine's slow path, and a slow path that reads or writes the wrong eight bytes corrupts
// exactly the way the firmware's output is corrupted. The buffer is two pages; the copies here
// straddle the boundary between them.
    .globl  memops_prog
    .type   memops_prog, %function
memops_prog:
    stp     x19, x20, [sp, #-32]!
    stp     x21, x22, [sp, #16]
    movz    x1, #0x0123
    movk    x1, #0x4567, lsl #16
    movk    x1, #0x89ab, lsl #32
    movk    x1, #0xcdef, lsl #48
    movz    x2, #0xfedc
    movk    x2, #0xba98, lsl #16
    movk    x2, #0x7654, lsl #32
    movk    x2, #0x3210, lsl #48
    // A pattern across the page boundary, written sixteen bytes at a time, unaligned.
    add     x3, x0, #4088
    mov     x4, #0
1:
    add     x11, x3, x4
    stp     x1, x2, [x11]
    add     x4, x4, #16
    cmp     x4, #64
    b.ne    1b
    // The same bytes copied from the first page into the second, unaligned both ways.
    add     x5, x0, #4080
    add     x6, x0, #2048
    mov     x4, #0
2:
    add     x11, x5, x4
    add     x12, x6, x4
    ldp     x7, x8, [x11]
    stp     x7, x8, [x12]
    add     x4, x4, #16
    cmp     x4, #96
    b.ne    2b
    // And through SIMD, from every alignment.
    add     x5, x0, #4096
    sub     x5, x5, #1
    mov     x4, #0
3:
    add     x11, x5, x4
    add     x12, x6, x4
    ldr     q0, [x11]
    str     q0, [x12]
    add     x4, x4, #16
    cmp     x4, #128
    b.ne    3b
    // Post-index forms, which write their base back.
    add     x7, x0, #3072
    add     x8, x0, #1024
    mov     x4, #0
    mov     x9, x7
    mov     x10, x8
4:
    ldr     q1, [x9], #16
    str     q1, [x10], #16
    add     x4, x4, #1
    cmp     x4, #32
    b.ne    4b
    /* Offsets, not the pointers themselves: the two runs use different buffers, so a raw pointer
       would differ for a reason that has nothing to do with the instruction. */
    sub     x9, x9, x0
    sub     x10, x10, x0
    add     x11, x0, #768
    stp     x9, x10, [x11]
    ldp     x19, x20, [sp]
    ldp     x21, x22, [sp, #16]
    add     sp, sp, #32
    ret
    .size   memops_prog, .-memops_prog

// Multiply, divide, carry and conditional compare: the arithmetic an allocator uses to turn a
// size and an index into an address. Cheap to get subtly wrong, and a wrong address is exactly
// what a missing page table entry would look like from the outside.
//
// Operands come from [x0] like the program above, results go to [x0+0x400]; a division by zero
// and INT_MIN / -1 are both in the operand mix on purpose, since those are the cases an emulator
// is most likely to answer differently from hardware.
    .text
    .globl  mathops_prog
    .type   mathops_prog, %function
mathops_prog:
    stp     x19, x20, [sp, #-32]!
    stp     x21, x22, [sp, #16]
    add     x25, x0, #0x400
    ldp     x1, x2, [x0]
    ldp     x3, x4, [x0, #16]
    ldp     x5, x6, [x0, #32]
    ldp     x7, x8, [x0, #48]

    mul     x9, x1, x2
    str     x9, [x25], #8
    madd    x9, x1, x2, x3
    str     x9, [x25], #8
    msub    x9, x1, x2, x3
    str     x9, [x25], #8
    mneg    x9, x1, x2
    str     x9, [x25], #8
    smulh   x9, x1, x2
    str     x9, [x25], #8
    umulh   x9, x1, x2
    str     x9, [x25], #8
    smull   x9, w1, w2
    str     x9, [x25], #8
    umull   x9, w1, w2
    str     x9, [x25], #8
    smaddl  x9, w1, w2, x3
    str     x9, [x25], #8
    umaddl  x9, w1, w2, x3
    str     x9, [x25], #8
    smsubl  x9, w1, w2, x3
    str     x9, [x25], #8
    umsubl  x9, w1, w2, x3
    str     x9, [x25], #8
    sdiv    x9, x1, x2
    str     x9, [x25], #8
    udiv    x9, x1, x2
    str     x9, [x25], #8
    sdiv    w9, w1, w2
    uxtw    x9, w9
    str     x9, [x25], #8
    udiv    w9, w1, w2
    uxtw    x9, w9
    str     x9, [x25], #8
    mul     w9, w1, w2
    uxtw    x9, w9
    str     x9, [x25], #8

    adds    x9, x1, x2
    adc     x10, x3, x4
    str     x10, [x25], #8
    subs    x9, x1, x2
    sbc     x10, x3, x4
    str     x10, [x25], #8
    adds    x9, x1, x2
    adcs    x10, x3, x4
    str     x10, [x25], #8
    cmp     x1, x2
    ngc     x9, x3
    str     x9, [x25], #8

    lslv    x9, x1, x2
    str     x9, [x25], #8
    lsrv    x9, x1, x2
    str     x9, [x25], #8
    asrv    x9, x1, x2
    str     x9, [x25], #8
    rorv    x9, x1, x2
    str     x9, [x25], #8

    cmp     x1, x2
    ccmp    x3, x4, #0x5, ne
    cset    x9, eq
    str     x9, [x25], #8
    sxtw    x9, w2
    str     x9, [x25], #8
    uxtw    x9, w2
    str     x9, [x25], #8
    sxtb    x9, w4
    str     x9, [x25], #8

    ldp     x21, x22, [sp, #16]
    ldp     x19, x20, [sp], #32
    ret
    .size   mathops_prog, .-mathops_prog

// Exclusive and atomic read-modify-write: what a free list is built from.
//
// Each group starts from a known scratch word, does one thing to it, and records both the
// instruction's own result (the old value, or the store-exclusive status) and the word
// afterwards — so a wrong answer shows up either as a wrong result or as a wrong memory.
// The failure cases matter as much as the success ones: a store-exclusive that succeeds when
// the value changed is how one element gets handed to two callers.
    .arch   armv8.4-a
    .text
    .globl  atomics_prog
    .type   atomics_prog, %function
atomics_prog:
    stp     x19, x20, [sp, #-32]!
    stp     x21, x22, [sp, #16]
    add     x25, x0, #0x400
    add     x26, x0, #0x800

    // LDXR/STXR with nothing in between: succeeds, status 0.
    mov     x9, #0x11
    str     x9, [x26]
    ldxr    x1, [x26]
    add     x1, x1, #1
    stxr    w2, x1, [x26]
    str     x2, [x25], #8
    ldr     x3, [x26]
    str     x3, [x25], #8

    // A plain store between them: the monitor is lost, status 1.
    ldxr    x1, [x26]
    mov     x4, #0x22
    str     x4, [x26]
    stxr    w2, x1, [x26]
    str     x2, [x25], #8
    ldr     x3, [x26]
    str     x3, [x25], #8

    // STXR with no LDXR before it: status 1.
    mov     x1, #0x33
    stxr    w2, x1, [x26]
    str     x2, [x25], #8
    ldr     x3, [x26]
    str     x3, [x25], #8

    // CLREX clears the monitor: status 1.
    ldxr    x1, [x26]
    clrex
    mov     x1, #0x44
    stxr    w2, x1, [x26]
    str     x2, [x25], #8
    ldr     x3, [x26]
    str     x3, [x25], #8

    // Exclusive pair, nothing in between: succeeds.
    mov     x9, #0x100
    str     x9, [x26]
    str     x9, [x26, #8]
    ldxp    x1, x3, [x26]
    add     x1, x1, #1
    add     x3, x3, #2
    stxp    w2, x1, x3, [x26]
    str     x2, [x25], #8
    ldp     x4, x5, [x26]
    str     x4, [x25], #8
    str     x5, [x25], #8

    // Exclusive pair with one word changed in between: fails, memory unchanged.
    ldxp    x1, x3, [x26]
    mov     x4, #0x777
    str     x4, [x26, #8]
    stxp    w2, x1, x3, [x26]
    str     x2, [x25], #8
    ldp     x4, x5, [x26]
    str     x4, [x25], #8
    str     x5, [x25], #8

    // LDADDAL: old value out, sum in.
    mov     x9, #0x10
    str     x9, [x26]
    mov     x1, #5
    ldaddal x1, x2, [x26]
    str     x2, [x25], #8
    ldr     x3, [x26]
    str     x3, [x25], #8

    // LDSMAXAL / LDUMINAL: signed and unsigned are not the same instruction.
    mov     x9, #0
    sub     x9, x9, #1
    str     x9, [x26]
    mov     x1, #1
    ldsmaxal x1, x2, [x26]          // signed: 1 > -1, so memory becomes 1
    str     x2, [x25], #8
    ldr     x3, [x26]
    str     x3, [x25], #8
    mov     x9, #0
    sub     x9, x9, #1
    str     x9, [x26]
    lduminal x1, x2, [x26]          // unsigned: 1 < 0xffff... , so memory stays
    str     x2, [x25], #8
    ldr     x3, [x26]
    str     x3, [x25], #8

    // CASAL: matching compare swaps, mismatching one does not.
    mov     x9, #0x99
    str     x9, [x26]
    mov     x1, #0x99
    mov     x2, #0xaa
    casal   x1, x2, [x26]
    str     x1, [x25], #8
    ldr     x3, [x26]
    str     x3, [x25], #8
    mov     x1, #0x11
    mov     x2, #0xbb
    casal   x1, x2, [x26]
    str     x1, [x25], #8
    ldr     x3, [x26]
    str     x3, [x25], #8

    // SWPAL, and the byte forms of the same ideas.
    mov     x1, #0x5a
    swpal   x1, x2, [x26]
    str     x2, [x25], #8
    ldr     x3, [x26]
    str     x3, [x25], #8
    mov     w1, #0x7
    ldaddalb w1, w2, [x26]
    str     x2, [x25], #8
    ldrb    w3, [x26]
    str     x3, [x25], #8

    // CASPAL: compare and swap a pair, matching and not.
    mov     x9, #0x111
    str     x9, [x26]
    mov     x9, #0x222
    str     x9, [x26, #8]
    ldp     x0, x1, [x26]
    mov     x2, #0x333
    mov     x3, #0x444
    caspal  x0, x1, x2, x3, [x26]
    ldp     x6, x7, [x26]
    str     x6, [x25], #8
    str     x7, [x25], #8

    ldp     x21, x22, [sp, #16]
    ldp     x19, x20, [sp], #32
    ret
    .size   atomics_prog, .-atomics_prog

// Scheduler list operations: the multi-step store sequences thread_block/thread_go
// perform on a thread structure, and nothing else.
//
//   enqueue : 2 loads, 3 stores (circle_enqueue_tail)
//   dequeue : 2 loads, 3 stores (circle_dequeue)
//   clear_wait : one read-modify-write of `state`
//
// The same bytes run natively and through the JIT, so a store the translator drops
// or places at the wrong offset shows up as a difference in the buffer.
//
// buffer (x0):
//   +0x000 head
//   +0x020 t0.next   +0x028 t0.prev   +0x080 t0.state
//   +0x040 t1.next   +0x048 t1.prev   +0x084 t1.state
//   +0x060 t2.next   +0x068 t2.prev   +0x088 t2.state

    .text
    .globl  sched_prog
    .type   sched_prog, %function
sched_prog:
    stp     x19, x20, [sp, #-32]!
    stp     x21, x22, [sp, #16]
    mov     x19, x0

    // circle_queue_init: head = NULL
    str     xzr, [x19]

    // Three nodes, links self-looped (= on no queue), state = TH_WAIT|TH_UNINT.
    mov     w9, #9
    add     x1, x19, #0x20
    str     x1, [x1]
    str     x1, [x1, #8]
    str     w9, [x19, #0x80]
    add     x1, x19, #0x40
    str     x1, [x1]
    str     x1, [x1, #8]
    str     w9, [x19, #0x84]
    add     x1, x19, #0x60
    str     x1, [x1]
    str     x1, [x1, #8]
    str     w9, [x19, #0x88]

    // enqueue t0 into the empty queue
    add     x1, x19, #0x20
    str     x1, [x19]
    str     x1, [x1]
    str     x1, [x1, #8]

    // enqueue t1: tail = head->prev; elt.next = tail.next; elt.prev = tail;
    //             tail.next = elt; elt.next->prev = elt
    add     x2, x19, #0x40
    ldr     x3, [x19]
    ldr     x4, [x3, #8]
    ldr     x5, [x4]
    str     x5, [x2]
    str     x4, [x2, #8]
    str     x2, [x4]
    str     x2, [x5, #8]

    // enqueue t2
    add     x2, x19, #0x60
    ldr     x3, [x19]
    ldr     x4, [x3, #8]
    ldr     x5, [x4]
    str     x5, [x2]
    str     x4, [x2, #8]
    str     x2, [x4]
    str     x2, [x5, #8]

    // dequeue t1 (the middle one): prev.next = elt.next; elt.next->prev = elt.prev;
    //                              elt.next = elt.prev = elt
    add     x6, x19, #0x40
    ldr     x7, [x6]
    ldr     x8, [x6, #8]
    str     x7, [x8]
    str     x8, [x7, #8]
    str     x6, [x6]
    str     x6, [x6, #8]

    // clear_wait on t0 and t2: state = (state & ~TH_WAIT) | TH_RUN
    ldr     w3, [x19, #0x80]
    and     w3, w3, #0xfffffffe
    orr     w3, w3, #0x4
    str     w3, [x19, #0x80]
    ldr     w3, [x19, #0x88]
    and     w3, w3, #0xfffffffe
    orr     w3, w3, #0x4
    str     w3, [x19, #0x88]

    ldp     x21, x22, [sp, #16]
    ldp     x19, x20, [sp], #32
    ret
    .size   sched_prog, .-sched_prog

// Multi-core scheduler handoff, two halves that run on two vCPUs at the same time.
//
// This is the shape of the deadlock seen in the guest: one CPU stands in for the
// thread being woken and one for whoever calls wakeup/clear_wait. The waiting side
// publishes TH_WAIT and spins on TH_RUN; the waking side clears TH_WAIT and sets
// TH_RUN. If the store that publishes the wake never reaches the other core, or the
// spin never observes it, the waiter never finishes -- which is exactly the guest's
// symptom (TH_WAIT still set, no TH_RUN, nothing runnable).
//
// Per-side instructions are the ones XNU uses: plain ldr/str around dmb, plus
// ldar/stlr for the state word.
//
// buffer (x0):
//   +0x00 state        +0x04 ready        +0x08 waker_done    +0x0c waiter_done

    .text
    .globl  sched_mc_waiter
    .type   sched_mc_waiter, %function
sched_mc_waiter:
    mov     w1, #9                  // TH_WAIT|TH_UNINT
    str     w1, [x0]                // thread_block: publish the wait
    dmb     sy
    mov     w1, #1
    str     w1, [x0, #4]            // ready = 1
    dmb     sy
1:
    ldar    w1, [x0]                // acquire the state word
    tbz     w1, #2, 1b              // spin until TH_RUN
    mov     w1, #1
    str     w1, [x0, #12]           // waiter_done = 1
    ret
    .size   sched_mc_waiter, .-sched_mc_waiter

    .text
    .globl  sched_mc_waker
    .type   sched_mc_waker, %function
sched_mc_waker:
    add     x2, x0, #4
2:
    ldar    w1, [x2]                // acquire ready
    cbz     w1, 2b                  // spin until the waiter published
    ldar    w1, [x0]
    and     w1, w1, #0xfffffffe     // state &= ~TH_WAIT
    and     w1, w1, #0xfffffff7     // state &= ~TH_UNINT   (clear_wait does both)
    orr     w1, w1, #0x4            // state |=  TH_RUN
    stlr    w1, [x0]                // release the state (the wake)
    mov     w1, #1
    str     w1, [x0, #8]            // waker_done = 1
    ret
    .size   sched_mc_waker, .-sched_mc_waker
