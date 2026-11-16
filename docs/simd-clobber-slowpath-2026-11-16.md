# SIMD registers lost on the TLB-refill slow path (2026-11-16)

The guest's v0-v31 are the host's registers: SIMD instructions are passed
through untranslated (`core/translate.c`), so any C code the JIT calls must
either save them or be prevented from using them. AAPCS lets a callee use
v0-v7 and v16-v31 freely, and the low halves of v8-v15.

`hv_helper_entry` (the full helper call) does save them, via
`SAVE_FLAGS_FP`/`LOAD_FLAGS_FP` in `host/enter.S`. Three other paths call C
without that pair:

| Path | Called |
| --- | --- |
| `hv_helper_thunk`, TLB refill branch | `hv_tlb_refill` |
| `hv_ijc_thunk` | `hv_ijc_resolve` |
| `hv_xjump_thunk`, miss branch | `hv_ijc_resolve` |

A guest memory access that takes the refill slow path therefore came back with
v0-v7 clobbered. Nothing noticed until the compiler changed: gcc's
`hv_tlb_refill`, `tlb_fill` and the walk helpers happen to use no SIMD
registers, while clang's do (directly or through what it inlines).

## Symptom

`tests/test_bitops.c`'s fuzz round compares `out_buf[0x400..0x700]`, which the
program fills from `movi v0.16b, #0x5a` / `movi v1.16b, #0xa5`:

```text
fuzz 0: result +192 native 5a5a5a5a5a5a5a5a jit 0000000000000000
```

`run_one("bitops")` reported ok the whole time because it compares bytes
0..768 while the program stores that pair at 0x4c0 — the test was not looking
at the bytes that differed.

## How it was found

The failure reproduced on the host with the engine built by clang instead of
gcc, so the device was not needed:

```sh
NDK=/home/he/Android/Sdk/ndk/29.0.14206865
BIN=$NDK/toolchains/llvm/prebuilt/linux-arm64/bin
NDK=$NDK OUT=build/clang-test \
  CC="$BIN/clang --target=aarch64-linux-gnu --sysroot=/ --gcc-toolchain=/usr -Wno-implicit-function-declaration" \
  ./android/test-bitops.sh
```

What ruled out the translator: the JIT code from both builds was byte-identical
(`HV_BLOCK_DUMP=/tmp/x.dump HV_BLOCK_DUMP_EVERY=1`), `sizeof`/`offsetof(HvCpu)`
matched, and `host/enter.S`'s own text matched. Only the C the thunks call
differed. A hardware breakpoint on the translated `stp q0,q1` (the JIT region
is r-x, so a software breakpoint cannot be inserted) showed it never executed:
every store took the refill branch, and v0 was zero after it came back.

## Fix

`host/enter.S`: `SAVE_FLAGS_FP` before and `LOAD_FLAGS_FP` after the C call in
all three paths. Verified on both compilers and on the device:

| Build | fuzz | memops |
| --- | --- | --- |
| gcc host | 0 disagreements | ok |
| clang host | 0 disagreements | ok |
| clang bionic (device) | 0 disagreements | ok |

Before the fix the device reported 5 disagreements at +192 and `memops FAIL`.

## What this does not explain

The launchd stall (§ `docs/restore-userland-stall-2026-11-16.md`) is still
open. A clobbered SIMD register corrupts whatever the interrupted kernel code
was holding — LZFSE FSE state, checksums, copied structs — which can produce
arbitrary later damage including a lock that is never released, so this is a
credible cause and no longer a mystery, but it is not yet demonstrated to be
the one.
