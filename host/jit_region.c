#include "jit_region.h"

#include <TargetConditionals.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#if defined(__ANDROID__)
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

/* The split shape on a host that can make it itself: plain RW memory, an
   alias, and the original turned R-X. Works for macOS binaries without the
   hardened runtime; on iOS the executable half has to come from a debugger.

   Android takes the same shape by a different route, because vm_remap does not
   exist: one memfd, mapped twice — MAP_SHARED R-X for execution, MAP_SHARED R-W
   for writing. Both mappings are the same page-cache pages, so a store through
   the writable half is what the executable half fetches, with no remap and no
   W+X mapping at any moment. That last part is the reason to prefer it: nothing
   here ever asks for a page that is writable and executable at once, which is
   exactly what Android's W^X rules are written to refuse. It is also how ART's
   JIT code cache and Chromium's V8 do it.

   Whether a given app is allowed to map a memfd executable depends on its
   SELinux domain and targetSdkVersion, which cannot be decided from the engine
   side. hv_jit_region_open() therefore probes the shapes in order and records
   which one worked, so a run can report it rather than guess. */
#if defined(__ANDROID__)

#ifndef __NR_memfd_create
#define __NR_memfd_create 279 /* arm64 */
#endif

/* One memfd, mapped R-X and R-W: the split shape without vm_remap. */
static int open_split_memfd(HvJitRegion *region, size_t bytes) {
    int fd = (int)syscall(__NR_memfd_create, "henge-jit", 0 /* MFD_CLOEXEC */);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)bytes) != 0) {
        close(fd);
        return -1;
    }
    void *rx = mmap(NULL, bytes, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    void *rw = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd); /* the mappings hold the pages */
    if (rx == MAP_FAILED || rw == MAP_FAILED) {
        if (rx != MAP_FAILED) munmap(rx, bytes);
        if (rw != MAP_FAILED) munmap(rw, bytes);
        return -1;
    }
    if (hv_jit_region_attach(region, rx, rw, bytes) != 0) {
        munmap(rx, bytes);
        munmap(rw, bytes);
        return -1;
    }
    region->owned = 1;
    return 0;
}

/* A single mapping that is writable and executable at once. Only reached where
   the split shape was refused; see hv_jit_region_open(). */
static int open_wx(HvJitRegion *region, size_t bytes) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return -1;
    region->base = p;
    region->rw_diff = 0;
    region->capacity = bytes / sizeof(uint32_t);
    region->owned = 1;
    return 0;
}

#else /* Apple */

static int open_split(HvJitRegion *region, size_t bytes) {
    void *rx = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (rx == MAP_FAILED) return -1;
    vm_address_t rw = 0;
    vm_prot_t cur, max;
    if (vm_remap(mach_task_self(), &rw, bytes, 0, VM_FLAGS_ANYWHERE, mach_task_self(), (vm_address_t)rx, FALSE, &cur, &max,
                 VM_INHERIT_NONE) != KERN_SUCCESS ||
        mprotect(rx, bytes, PROT_READ | PROT_EXEC) != 0) {
        munmap(rx, bytes);
        return -1;
    }
    if (hv_jit_region_attach(region, rx, (void *)rw, bytes) != 0) return -1;
    region->owned = 1;
    return 0;
}

#endif /* __ANDROID__ */

int hv_jit_region_open(HvJitRegion *region, size_t bytes) {
    memset(region, 0, sizeof *region);
    if (getenv("HV_JIT_SPLIT")) {
#if defined(__ANDROID__)
        return open_split_memfd(region, bytes);
#else
        return open_split(region, bytes);
#endif
    }
#if defined(__ANDROID__)
    /* Order is a policy question, not a preference, and only a real app can
       settle it: a W+X mapping is the cheapest shape but is what the W^X rules
       exist to refuse, while the memfd split is policy-clean by construction.
       Try the clean one first so a locked-down device still works, and keep the
       W+X attempt for hosts that allow it. hv_jit_region_attach() in either case
       leaves rw_diff as the alias offset the writer uses. */
    if (open_split_memfd(region, bytes) == 0) return 0;
    if (open_wx(region, bytes) == 0) return 0;
    return -1;
#elif TARGET_OS_OSX
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (p == MAP_FAILED) return -1;
    region->base = p;
    region->capacity = bytes / sizeof(uint32_t);
    region->owned = 1;
    return 0;
#else
    return open_split(region, bytes); /* works only where nothing enforces code signing (tests on a jailbroken device) */
#endif
}

int hv_jit_region_attach(HvJitRegion *region, void *rx, void *rw, size_t bytes) {
    memset(region, 0, sizeof *region);
    if (!rx || !rw || bytes < 4096) return -1;
    region->base = rx;
    region->rw_diff = (intptr_t)((uint8_t *)rw - (uint8_t *)rx);
    region->capacity = bytes / sizeof(uint32_t);
    return 0;
}

void hv_jit_region_close(HvJitRegion *region) {
    if (region->owned && region->base) {
        size_t bytes = region->capacity * sizeof(uint32_t);
        if (region->rw_diff) munmap((uint8_t *)region->base + region->rw_diff, bytes);
        munmap(region->base, bytes);
    }
    memset(region, 0, sizeof *region);
}

void hv_jit_write(HvJitRegion *region, uint32_t *start, const void *src, size_t bytes) {
    /* One instruction may be rewritten while other CPUs run it (chaining a branch).
       memcpy may copy a word in pieces, and a CPU fetching in between takes half of
       the old branch and half of the new one: a jump to nowhere. */
    if (bytes == 4) {
        uint32_t word;
        memcpy(&word, src, 4);
#if TARGET_OS_OSX
        if (!region->rw_diff) pthread_jit_write_protect_np(0);
#endif
        __atomic_store_n((uint32_t *)((uint8_t *)start + region->rw_diff), word, __ATOMIC_RELEASE);
        return;
    }
    if (region->rw_diff) {
        memcpy((uint8_t *)start + region->rw_diff, src, bytes);
        return;
    }
#if TARGET_OS_OSX
    pthread_jit_write_protect_np(0); /* MAP_JIT: writable on this thread only, until hv_jit_finish */
#endif
    memcpy(start, src, bytes);
}

void hv_jit_finish(HvJitRegion *region, uint32_t *start, size_t words) {
    (void)region;
#if TARGET_OS_OSX
    if (!region->rw_diff) pthread_jit_write_protect_np(1);
#endif
    if (start && words) sys_icache_invalidate(start, words * sizeof(uint32_t));
}

/* As sys_icache_invalidate does it (libsystem_platform: DSB, IC IVAU per 64-byte line with a DSB after every 20 for
   the CPUs that need one, DSB, ISB), over both ranges. */
static void ic_lines(uint32_t *start, size_t words, int *n) {
    if (!start || !words) return;
    uintptr_t a = (uintptr_t)start & ~(uintptr_t)63, end = (uintptr_t)(start + words);
    for (; a < end; a += 64) {
        __asm__ volatile("ic ivau, %0" ::"r"(a) : "memory");
        if (++*n % 20 == 0) __asm__ volatile("dsb ish" ::: "memory");
    }
}

void hv_jit_finish2(HvJitRegion *region, uint32_t *a, size_t na, uint32_t *b, size_t nb) {
    (void)region;
#if TARGET_OS_OSX
    if (!region->rw_diff) pthread_jit_write_protect_np(1);
#endif
    int n = 0;
    __asm__ volatile("dsb ish" ::: "memory");
    ic_lines(a, na, &n);
    ic_lines(b, nb, &n);
    __asm__ volatile("dsb ish\n\tisb" ::: "memory");
}

void hv_jit_finish_branch(HvJitRegion *region, uint32_t *word) {
    (void)region;
#if TARGET_OS_OSX
    if (!region->rw_diff) pthread_jit_write_protect_np(1);
#endif
    /* The store first (so a refetch finds the new word), then the line, with no wait for every CPU's cache. */
    __asm__ volatile("dsb ish\n\tic ivau, %0" ::"r"((uintptr_t)word & ~(uintptr_t)63) : "memory");
}

/* Any thread: an even number of words (blocks stay 8-byte aligned), taken atomically. */
uint32_t *hv_jit_reserve(HvJitRegion *region, size_t words) {
    words = (words + 1) & ~(size_t)1;
    size_t at = __atomic_fetch_add(&region->used, words, __ATOMIC_RELAXED);
    size_t cap = region->limit && region->limit < region->capacity ? region->limit : region->capacity;
    if (at + words > cap) return NULL; /* full: the flush resets used */
    return region->base + at;
}

void hv_jit_reset(HvJitRegion *region) {
    region->used = 0;
}
