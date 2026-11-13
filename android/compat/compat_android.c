/*
 * Android/Linux compatibility layer for the Henge engine.
 *
 * Everything the engine took from Apple's Mach layer, implemented over Android
 * bionic / Linux:
 *
 *   mach_absolute_time()   CLOCK_MONOTONIC_RAW scaled to 24 MHz
 *   mach_task_self()       the process, identity
 *   mach_thread_self()     a stable per-thread number
 *   vm_allocate()          anonymous mmap (no footprint ledger to escape)
 *   memory-entry calls     successes that change nothing
 *
 * No part of this file has an iOS counterpart to keep in step: the semantics it
 * has to preserve are stated in the headers under include/.
 */
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_info.h>
#include <mach/vm_map.h>

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ---- time ------------------------------------------------------------- */

/*
 * ns -> 24 MHz ticks: whole seconds first, so the remainder term stays small.
 * 1e9 ns is exactly 24e6 ticks, and ns * 24 / 1000 is exact for the remainder
 * (nanoseconds divide by 1000 cleanly into 24ths).
 */
static uint64_t ticks_from_ns(uint64_t ns) {
    return ns / 1000000000ull * HV_TICK_HZ + (ns % 1000000000ull) * 24ull / 1000ull;
}

/*
 * 24 MHz ticks from CLOCK_MONOTONIC_RAW: the raw clock is not slewed by NTP,
 * so the guest's CNTPCT never steps backwards or stretches. CLOCK_MONOTONIC
 * would be visibly corrected, which the guest's timekeeping would see.
 */
uint64_t mach_absolute_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return ticks_from_ns((uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec);
}

/* ---- task / thread identity ------------------------------------------- */

mach_port_t mach_task_self(void) { return (mach_port_t)getpid(); }

kern_return_t mach_port_deallocate(mach_port_t task, mach_port_t name) {
    (void)task;
    (void)name;
    return KERN_SUCCESS; /* no port table to shrink */
}

char *mach_error_string(kern_return_t kr) {
    switch (kr) {
        case KERN_SUCCESS: return (char *)"KERN_SUCCESS";
        case KERN_INVALID_ADDRESS: return (char *)"KERN_INVALID_ADDRESS";
        case KERN_PROTECTION_FAILURE: return (char *)"KERN_PROTECTION_FAILURE";
        case KERN_NO_SPACE: return (char *)"KERN_NO_SPACE";
        case KERN_INVALID_ARGUMENT: return (char *)"KERN_INVALID_ARGUMENT";
        case KERN_FAILURE: return (char *)"KERN_FAILURE";
        default: return (char *)"KERN_?";
    }
}

/* ---- guest RAM -------------------------------------------------------- */

/*
 * Devices/vmapple.c's alloc_no_footprint() wants zeroed, writable, lazily
 * backed pages of an exact size, outside any accounting. That is precisely
 * MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE. MAP_NORESERVE matters: the app
 * asks for 4-8 GB on a device that will not overcommit that eagerly, and the
 * pages are faulted in as the guest touches them.
 */
kern_return_t vm_allocate(mach_port_t task, vm_address_t *address, vm_size_t size, int flags) {
    (void)task;
    (void)flags;
    if (!address || !size) return KERN_INVALID_ARGUMENT;
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        *address = 0;
        return KERN_NO_SPACE;
    }
    *address = (vm_address_t)p;
    return KERN_SUCCESS;
}

kern_return_t vm_deallocate(mach_port_t task, vm_address_t address, vm_size_t size) {
    (void)task;
    if (!address || !size) return KERN_INVALID_ARGUMENT;
    return munmap((void *)address, size) == 0 ? KERN_SUCCESS : KERN_INVALID_ADDRESS;
}

kern_return_t mach_make_memory_entry_64(mach_port_t task, memory_object_size_t *size,
                                        memory_object_offset_t offset, vm_prot_t permission,
                                        mach_port_t *object_handle, mach_port_t parent_entry) {
    (void)task;
    (void)size;
    (void)offset;
    (void)permission;
    (void)parent_entry;
    /* No entry exists; the caller only needs to learn that the range is usable,
     * which vm_allocate() already established. */
    if (object_handle) *object_handle = MACH_PORT_NULL;
    return KERN_SUCCESS;
}

kern_return_t vm_map(mach_port_t task, vm_address_t *address, vm_size_t size, vm_offset_t mask, int flags,
                     mach_port_t object, vm_offset_t offset, int copy, vm_prot_t cur_protection,
                     vm_prot_t max_protection, vm_inherit_t inheritance) {
    (void)task;
    (void)address;
    (void)size;
    (void)mask;
    (void)flags;
    (void)object;
    (void)offset;
    (void)copy;
    (void)cur_protection;
    (void)max_protection;
    (void)inheritance;
    return KERN_SUCCESS; /* the range is already mapped where the caller asked */
}

/* ---- thread identity and per-thread CPU time -------------------------- */

/*
 * A Mach port does not exist here, so mach_thread_self() hands out a slot and
 * thread_info() maps it back to the thread it named. The slot is stable for the
 * life of the thread: core/machine.c captures the handle once and asks about it
 * later from another thread, when probing a stalled vCPU.
 *
 * Slots are reused when a thread ends, so a handle held past its thread's exit
 * names whatever thread took the slot next. Apple's ports have the same flaw
 * (a dead port name is recycled), and no caller here outlives its thread.
 */
#define HV_THREAD_SLOTS 128
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_slot_thread[HV_THREAD_SLOTS]; /* NULL means free */
static __thread mach_port_t t_slot;              /* this thread's slot, once taken */

mach_port_t mach_thread_self(void) {
    if (t_slot) return t_slot;
    pthread_mutex_lock(&g_slot_mu);
    for (int i = 1; i < HV_THREAD_SLOTS; i++) {
        if (!g_slot_thread[i]) {
            g_slot_thread[i] = pthread_self();
            t_slot = (mach_port_t)i;
            break;
        }
    }
    pthread_mutex_unlock(&g_slot_mu);
    return t_slot; /* 0 when the table is full: thread_info() then refuses */
}

static pthread_t slot_thread(mach_port_t port) {
    if (port == 0 || port >= HV_THREAD_SLOTS) return (pthread_t)0;
    pthread_mutex_lock(&g_slot_mu);
    pthread_t p = g_slot_thread[port];
    pthread_mutex_unlock(&g_slot_mu);
    return p;
}

/* Seconds and microseconds of CPU time this thread has burned, split by mode. */
static void read_cpu_time(pthread_t p, time_value_t *user, time_value_t *sys) {
    user->seconds = user->microseconds = 0;
    sys->seconds = sys->microseconds = 0;
    clockid_t cid;
    if (clock_getcpuclockid(p, &cid) != 0) return;
    struct timespec ts;
    if (clock_gettime(cid, &ts) != 0) return;
    /* CLOCK_THREAD_CPUTIME_ID is user+system combined; Linux does not expose the
     * split per thread without taskstats, so report it all as user time. The
     * only consumer adds the two together. */
    user->seconds = (int32_t)ts.tv_sec;
    user->microseconds = (int32_t)(ts.tv_nsec / 1000);
}

/* 'R' in /proc/self/task/<tid>/stat means the thread is on a CPU right now. */
static int thread_is_running(pthread_t p) {
    pid_t tid = 0;
#if defined(__ANDROID__)
    tid = pthread_gettid_np(p);
#else
    tid = (pid_t)(uintptr_t)p; /* glibc: pthread_t is the TID */
#endif
    if (tid <= 0) return 0;
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d/stat", (int)tid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[512];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    /* The comm field is parenthesised and may itself hold spaces or parens, so
     * the state is the first non-space character after the last ')'. */
    char *close_paren = strrchr(buf, ')');
    if (!close_paren) return 0;
    for (char *q = close_paren + 1; *q; q++)
        if (*q != ' ') return *q == 'R';
    return 0;
}

kern_return_t thread_info(mach_port_t thread, int flavor, thread_info_t info, mach_msg_type_number_t *count) {
    if (flavor != THREAD_BASIC_INFO) return KERN_INVALID_ARGUMENT;
    if (!info || !count) return KERN_INVALID_ARGUMENT;
    if (*count < THREAD_BASIC_INFO_COUNT) return KERN_INVALID_ARGUMENT;

    pthread_t p = slot_thread(thread);
    if (!p) return KERN_INVALID_ARGUMENT;

    thread_basic_info_data_t *ti = (thread_basic_info_data_t *)info;
    memset(ti, 0, sizeof *ti);
    read_cpu_time(p, &ti->user_time, &ti->system_time);
    ti->run_state = thread_is_running(p) ? TH_STATE_RUNNING : TH_STATE_WAITING;
    ti->sleep_time = -1;
    *count = THREAD_BASIC_INFO_COUNT;
    return KERN_SUCCESS;
}

/* ---- ulock: Darwin's compare-and-wait ---------------------------------- */

/*
 * core/machine.c parks a vCPU that waits for another's acknowledgement with
 * Darwin's __ulock_wait/__ulock_wake, on a 32-bit word, and only wakes when that
 * word still holds the value it saw. That is precisely Linux's futex:
 *
 *   UL_COMPARE_AND_WAIT  == FUTEX_WAIT (sleep while *addr == value, then EAGAIN)
 *   UL_ULF_WAKE_ALL      == FUTEX_WAKE with INT_MAX waiters
 *
 * The file declares both itself, so defining them here needs no change to it.
 * Darwin's UL_ULF_NO_ERRNO asks for a positive errno-style result instead of
 * -1/errno; the one caller ignores the return value, but the convention is kept
 * so a future caller is not misled.
 *
 * The timeout is microseconds on Darwin and a relative timespec on Linux, with a
 * NULL timespec meaning "wait forever". A NULL pointer to the futex word is a
 * programming error in both.
 */
#include <linux/futex.h>
#include <sys/syscall.h>
#include <limits.h>

static int hv_futex(uint32_t *addr, int op, uint32_t value, const struct timespec *timeout) {
    return (int)syscall(SYS_futex, addr, op, value, timeout, NULL, 0);
}

int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us) {
    (void)operation; /* only compare-and-wait is used */
    if (!addr) return 22 /* EINVAL */;
    struct timespec ts, *tsp = NULL;
    if (timeout_us) {
        ts.tv_sec = (time_t)(timeout_us / 1000000u);
        ts.tv_nsec = (long)(timeout_us % 1000000u) * 1000;
        tsp = &ts;
    }
    int rc = hv_futex((uint32_t *)addr, FUTEX_WAIT, (uint32_t)value, tsp);
    if (rc == 0) return 0;                    /* woken */
    return errno;                             /* EAGAIN: value moved; ETIMEDOUT: deadline */
}

int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value) {
    (void)operation;
    (void)wake_value; /* compare-and-wait wakes the whole word, not one value */
    if (!addr) return 22 /* EINVAL */;
    int rc = hv_futex((uint32_t *)addr, FUTEX_WAKE, (uint32_t)INT_MAX, NULL);
    return rc < 0 ? errno : 0;
}

/* ---- ledger-tagged memory entries -------------------------------------- */

/*
 * devices/vmapple.c declares this itself and calls it after making a memory
 * entry, to move the pages off the app's footprint ledger. There is no ledger
 * here and no entry to tag (mach_make_memory_entry_64 above returns success with
 * MACH_PORT_NULL), so there is nothing to do but agree.
 */
kern_return_t mach_memory_entry_ownership(mach_port_t entry, task_t owner, int ledger_tag, int ledger_flags) {
    (void)entry;
    (void)owner;
    (void)ledger_tag;
    (void)ledger_flags;
    return KERN_SUCCESS;
}


/*
 * The page the JIT reads to decide how to inline a counter read.
 *
 * On XNU every process has the commpage mapped at 0xfffffc000, and the engine reads two fields of
 * it while translating: the timebase type, which says whether mach_absolute_time is CNTVCT plus an
 * offset or a syscall, and the offset itself, which the generated code adds at runtime. On iOS that
 * page is simply there, so the engine never had to map it.
 *
 * Linux has no such mapping, and the generated code loads from that address *on the host* — which
 * is why a guest reading its counter took the process down with a SIGSEGV at 0xfffffc088.
 *
 * So the same page is synthesised where the engine expects it. The values are the ones this port
 * already lives by: mach_absolute_time here is CLOCK_MONOTONIC_RAW, and on arm64 that is the arch
 * counter CNTVCT_EL0 itself, so the offset between them is zero and the type is "read CNTVCT".
 *
 * Failure is not fatal: the caller logs it, and the engine's other paths still work — the counters
 * would simply be the ones its helper produces instead of the inlined form.
 */
int hv_android_map_commpage(void) {
    static int done;
    if (done) return 0;

    const uintptr_t base = 0xfffffc000ull;
    size_t page = 4096;
    void *want = (void *) base;
    void *got = mmap(want, page, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (got == MAP_FAILED) {
        fprintf(stderr, "henge: no commpage at %p (%s); counter reads stay on the helper path\n",
                want, strerror(errno));
        return -1;
    }
    if (got != want) {
        munmap(got, page);
        fprintf(stderr, "henge: commpage landed at %p, not %p\n", got, want);
        return -1;
    }

    /* Fields the engine reads: the offset (u64) at 0x88, the type (byte) at 0x90. */
    volatile uint64_t *offset = (volatile uint64_t *) (base + 0x88);
    volatile uint8_t *type = (volatile uint8_t *) (base + 0x90);
    *offset = 0;   /* mach_absolute_time and CNTVCT_EL0 are the same counter here */
    *type = 1;     /* "read CNTVCT_EL0, add the offset" */
    __builtin___clear_cache((char *) want, (char *) want + page);
    done = 1;
    fprintf(stderr, "henge: commpage at %p, timebase type 1, offset 0\n", want);
    return 0;
}
