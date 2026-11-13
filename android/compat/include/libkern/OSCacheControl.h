/*
 * Android/Linux compatibility: <libkern/OSCacheControl.h>
 *
 * host/jit_region.c flushes the translated blocks with sys_icache_invalidate().
 * On AArch64 that is the DC CVAU / IC IVAU sequence, which the compiler
 * emits for __builtin___clear_cache(); bionic also exports
 * __builtin___clear_cache and cacheflush() for the same purpose.
 *
 * AArch64 has no coherent I-cache for self-modifying code, so this call is
 * load-bearing, not a formality: skipping it makes the JIT execute stale
 * instructions.
 */
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline void sys_icache_invalidate(void *start, size_t len) {
    __builtin___clear_cache((char *)start, (char *)start + len);
}

static inline void sys_dcache_flush(void *start, size_t len) {
    __builtin___clear_cache((char *)start, (char *)start + len);
}

#ifdef __cplusplus
}
#endif
