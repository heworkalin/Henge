#include "harness.h"

// See tests/test_bitops.c for why this exists.

// ---- bitfields and shifts, the reader's bread and butter ----
PROGRAM bits
    ubfx    x0, x1, #3, #11
    sbfx    x2, x3, #17, #23
    bfxil   x4, x5, #9, #19
    ubfiz   x6, x7, #11, #13
    sbfiz   x8, x9, #5, #27
    bfi     w10, w11, #7, #19
    extr    x12, x13, x14, #29
    ror     x15, x16, #13
    lsl     x17, x18, x19
    lsr     x20, x21, x22
    asr     w23, w24, w25
    rorv    x26, x27, x28
    clz     x0, x1
    cls     x2, x3
    rbit    x4, x5
    rev     x6, x7
    rev32   x8, x9
    rev16   w10, w11
    sxtb    x12, w13
    sxth    x14, w15
    sxtw    x16, w17
    uxtb    w18, w19
    uxth    w20, w21
    sbfm    x22, x23, #4, #27
    ubfm    x24, x25, #7, #31
    bfm     x26, x27, #9, #23
    csel    x0, x1, x2, lo
    csinc   x3, x4, x5, hi
    csinv   x6, x7, x8, eq
    csneg   x9, x10, x11, ne
    adcx    x12, x13, x14
    adcz    x15, x16, x17
END bits

// ---- NEON: widening, narrowing, shuffles, pairwise: the match copier ----
PROGRAM neon
    ushll   v0.8h, v1.8b, #3
    ushll2  v2.8h, v3.16b, #2
    sshll   v4.8h, v5.8b, #1
    sshll2  v6.8h, v7.16b, #4
    uxtl    v8.8h, v9.8b
    sxtl    v10.8h, v11.8b
    xtn     v12.8b, v13.8h
    sqxtn   v14.8b, v15.8h
    uqxtn   v16.8b, v17.8h
    addv    b18, v19.16b
    addv    h20, v21.8h
    addp    v22.16b, v23.16b, v24.16b
    zip1    v25.16b, v26.16b, v27.16b
    zip2    v28.16b, v29.16b, v30.16b
    uzp1    v31.16b, v0.16b, v1.16b
    trn1    v2.16b, v3.16b, v4.16b
    trn2    v5.16b, v6.16b, v7.16b
    ext     v8.16b, v9.16b, v10.16b, #5
    tbl     v11.16b, {v12.16b}, v13.16b
    tbl     v14.16b, {v15.16b, v16.16b}, v17.16b
    tbx     v18.16b, {v19.16b}, v20.16b
    shl     v21.8h, v22.8h, #7
    sshr    v23.8h, v24.8h, #3
    ushr    v25.8h, v26.8h, #2
    sli     v27.8h, v28.8h, #5
    sri     v29.8h, v30.8h, #4
    usra    v0.8h, v1.8h, #6
    srsra   v2.8h, v3.8h, #2
    umull   v4.4s, v5.4h, v6.4h
    umull2  v7.4s, v8.8h, v9.8h
    smull   v10.4s, v11.4h, v12.4h
    umlal   v13.4s, v14.4h, v15.4h
    mla     v16.8h, v17.8h, v18.8h
    mls     v19.8h, v20.8h, v21.8h
    pmul    v22.16b, v23.16b, v24.16b
    sqdmulh v25.8h, v26.8h, v27.8h
    uaddlp  v28.4s, v29.8h
    sadalp  v30.4s, v31.8h
    addhn   v0.8b, v1.8h, v2.8h
    raddhn  v3.8b, v4.8h, v5.8h
END neon

// ---- compares, selects and saturating arithmetic: length and distance decoding ----
PROGRAM cmp
    cmeq    v0.16b, v1.16b, v2.16b
    cmhi    v3.16b, v4.16b, v5.16b
    cmhs    v6.16b, v7.16b, v8.16b
    cmgt    v9.8h, v10.8h, v11.8h
    cmge    v12.4s, v13.4s, v14.4s
    cmeq    v15.2d, v16.2d, v17.2d
    cmtst   v18.16b, v19.16b, v20.16b
    bsl     v21.16b, v22.16b, v23.16b
    bit     v24.16b, v25.16b, v26.16b
    bif     v27.16b, v28.16b, v29.16b
    sqadd   v30.16b, v31.16b, v0.16b
    uqadd   v1.16b, v2.16b, v3.16b
    sqsub   v4.8h, v5.8h, v6.8h
    uqsub   v7.8h, v8.8h, v9.8h
    sqabs   v10.16b, v11.16b
    sqneg   v12.8h, v13.8h
    abs     v14.8h, v15.8h
    neg     v16.8h, v17.8h
    cnt     v18.16b, v19.16b
    not     v20.16b, v21.16b
    and     v22.16b, v23.16b, v24.16b
    bic     v25.16b, v26.16b, v27.16b
    orn     v28.16b, v29.16b, v30.16b
    eor     v31.16b, v0.16b, v1.16b
    dup     v2.16b, w3
    dup     v4.8h, w5
    ins     v6.b[7], w7
    ins     v8.h[3], w9
    movi    v10.16b, #0x5a
    mvni    v11.8h, #0x33, lsl #8
    movi    v12.4s, #0x1234, lsl #16
    fcmeq   v13.4s, v14.4s, v15.4s
    fabs    v16.4s, v17.4s
    fneg    v18.2d, v19.2d
    fadd    v20.4s, v21.4s, v22.4s
    fmul    v23.4s, v24.4s, v25.4s
    fmin    v26.4s, v27.4s, v28.4s
    fmax    v29.4s, v30.4s, v31.4s
END cmp

// ---- the loop shapes a decoder runs: accumulate a bit position, branch on bits ----
PROGRAM reader
    mov     x9, x0
    mov     x10, #0
    mov     x11, #64
    mov     x12, #0
1:
    ldr     x13, [x9]
    and     x14, x13, #0x1f
    ubfx    x15, x13, #5, #11
    lsl     x15, x15, x14
    ubfx    x16, x13, #16, #13
    extr    x17, x16, x15, #32
    add     x10, x10, x17
    ror     x18, x10, x14
    eor     x12, x12, x18
    add     x9, x9, #8
    subs    x11, x11, #1
    b.ne    1b
    str     x12, [x0, #64]
    str     x10, [x0, #72]
END reader
