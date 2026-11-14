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
    movz    x1, #0x1234, lsl #16
    movk    x1, #0xabcd, lsl #48
    movz    x2, #0x00ff
    movk    x2, #0xff00, lsl #32
    movz    x3, #0xffff
    movz    x4, #1
    /* Every register the program writes is seeded here first: a read-modify-write form like bfxil
       would otherwise start from whatever the caller left behind, which is not a property of the
       instruction being tested. */
    movz    x5, #0x1111
    movz    x6, #0x2222, lsl #16
    movz    x7, #0x3333, lsl #16
    movz    x8, #0x4444, lsl #32
    movz    x9, #0x5555, lsl #32
    movz    x10, #0x6666, lsl #48
    movz    x11, #0x7777
    movz    x12, #0x8888, lsl #16
    movz    x13, #0x9999, lsl #32
    movz    x14, #0xaaaa
    movz    x15, #0xbbbb, lsl #16
    movz    x16, #0xcccc, lsl #32
    movz    x17, #0xdddd, lsl #48
    movz    x18, #0xeeee, lsl #16
    movz    x19, #0x0191
    movz    x20, #0x0292, lsl #16
    movz    x21, #0x0393, lsl #16
    movz    x22, #0x0494, lsl #32
    movz    x23, #0x0595, lsl #48
    movz    x24, #0x0696, lsl #16
    lsl     x5, x4, #37
    orr     x5, x5, #0x7ff
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
    stp     x1, x2, [x0]
    stp     x3, x4, [x0, #16]
    stp     x5, x6, [x0, #32]
    stp     x7, x8, [x0, #48]
    stp     x9, x10, [x0, #64]
    stp     x11, x12, [x0, #80]
    stp     x13, x14, [x0, #96]
    stp     x15, x16, [x0, #112]
    stp     x17, x18, [x0, #128]
    stp     x19, x20, [x0, #144]
    stp     x21, x22, [x0, #160]
    stp     x23, x24, [x0, #176]
    stp     q0, q1, [x0, #192]
    stp     q2, q3, [x0, #224]
    stp     q4, q5, [x0, #256]
    stp     q6, q7, [x0, #288]
    stp     q8, q9, [x0, #320]
    stp     q10, q11, [x0, #352]
    stp     q12, q13, [x0, #384]
    stp     q14, q15, [x0, #416]
    stp     q16, q17, [x0, #448]
    stp     q18, q19, [x0, #480]
    stp     q20, q21, [x0, #512]
    stp     q22, q23, [x0, #544]
    stp     q24, q25, [x0, #576]
    stp     q26, q27, [x0, #608]
    stp     q28, q29, [x0, #640]
    stp     q30, q31, [x0, #672]
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
