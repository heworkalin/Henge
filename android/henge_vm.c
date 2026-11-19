/*
 * The machine, built from inside the app.
 *
 * `host/vm.c` does this for the macOS tool and the iOS app, but it also carries snapshots, the
 * audio backend, CoreFoundation and dispatch: none of which an Android bring-up needs yet, and
 * all of which would have to be ported before anything could be tried. So this file does the
 * smallest thing that answers the question that matters right now:
 *
 *   does a real machine come up, with reims-vgpu as its GPU, on this device?
 *
 * It builds the board with no disks and no ROM, so no guest code ever runs — that is deliberate.
 * Everything up to that point (JIT region, guest RAM in a memfd, the GIC, the PCIe bus, the
 * virtio devices, and above all `rvg_device_create` with our memory map and interrupt callback)
 * is exercised, and a failure is reported with the line that failed rather than as a crash.
 *
 * What it does not do, and does not pretend to: run the guest. That needs the ROM, the installed
 * disk and the auxiliary storage, i.e. an IPSW install (`host/install.c`), which is the next
 * piece of work and not this one.
 */

/* glibc hides siginfo_t / SA_SIGINFO behind _GNU_SOURCE; the host build compiles this file too,
   since it holds the functions the shared command-line tool calls. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dispatch.h"
#include "machine.h"
#include "vmapple.h"
#include "xhci.h"

#ifdef HENGE_HAVE_REIMS
#include "reims_pvg.h"
#endif

/* Declared here as well, because the smoke entry tears down a previous attempt before it builds
 * the next one. */
void hv_android_vm_teardown(void);

/* One machine at a time: the engine holds this much state, and a probe that leaves two behind
 * would exhaust the device long before it reported anything. */
static HvMachine g_machine;
static HvVmapple g_board;
/*
 * One HvCpu per possible CPU, not one in total: `hv_vmapple_create` takes a pointer and the board
 * indexes it as `cpu[i]` for every CPU in the config, so a single object is out of bounds the
 * moment more than one CPU is asked for — which used to corrupt whatever followed it in .bss and
 * then fault in the middle of resetting a CPU's caches.
 */
static HvCpu g_cpus[HV_MAX_CPUS];
static int g_up;

static void say(char **cursor, char *end, const char *fmt, ...) {
    if (*cursor >= end) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*cursor, (size_t)(end - *cursor), fmt, ap);
    va_end(ap);
    if (n > 0) *cursor += n;
    if (*cursor > end) *cursor = end;
}

/* Bytes of guest RAM and JIT memory for the bring-up. Both are large on purpose: a machine that
 * only comes up with a small RAM would tell us nothing about the real configuration, and the
 * memfd is sparse until touched. */
#define SMOKE_RAM_BYTES ((uint64_t)2048 << 20)
#define SMOKE_JIT_BYTES ((size_t)768 << 20)

/*
 * Builds the board and reports what came up. Appends to `report`; returns 0 when the board is up.
 *
 * The caller must call `hv_android_vm_teardown()` before trying again.
 */
int hv_android_vm_smoke(char *report, size_t cap, const char *cache_dir) {
    char *cursor = report + strlen(report);
    char *end = report + cap;

    if (g_up) hv_android_vm_teardown();

    say(&cursor, end, "\n--- machine bring-up ---\n");

    /* The GPU stack caches translated shaders on disk, and metal2vulkan looks its directory up
     * the way a desktop program would: $XDG_CACHE_HOME, or $HOME/.cache. An Android app has
     * neither, and the runtime refuses to make a device without one — which reims-vgpu then
     * treats as fatal, aborting the process with no explanation beyond "no home directory".
     * The app's cache directory is the right answer, so it is set here before anything asks. */
    if (cache_dir && *cache_dir) {
        setenv("XDG_CACHE_HOME", cache_dir, 1);
        say(&cursor, end, "  shader cache   : %s\n", cache_dir);
    } else {
        say(&cursor, end, "  shader cache   : NO CACHE DIR — the GPU stack will refuse to start\n");
    }

    memset(&g_machine, 0, sizeof g_machine);
    memset(&g_board, 0, sizeof g_board);
    memset(g_cpus, 0, sizeof g_cpus);

    if (hv_machine_init(&g_machine, SMOKE_JIT_BYTES) != 0) {
        say(&cursor, end, "  JIT region     : FAILED (%zu MB)\n",
            SMOKE_JIT_BYTES >> 20);
        return -1;
    }
    say(&cursor, end, "  JIT region     : %zu MB\n", SMOKE_JIT_BYTES >> 20);

    HvCfgParams cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.nr_cpus = 1;
    cfg.ecid = 0x1234567890abcdefull;
    cfg.seed = 1;
    static const uint8_t mac[6] = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01};

    /* No root disk, no auxiliary storage: the board builds its devices without them, and this
     * run never reaches the point where a guest would read one. */
    if (hv_vmapple_create(&g_board, &g_machine, g_cpus, SMOKE_RAM_BYTES, NULL, NULL, &cfg, mac) !=
        0) {
        say(&cursor, end, "  machine        : FAILED (%llu MB of guest RAM)\n",
            (unsigned long long)(SMOKE_RAM_BYTES >> 20));
        hv_machine_free(&g_machine);
        return -1;
    }
    say(&cursor, end, "  machine        : up, %llu MB guest RAM, %d cpu\n",
        (unsigned long long)(SMOKE_RAM_BYTES >> 20), g_board.ncpus);

    /* The one line this whole exercise is about: guest RAM is file-backed, so reims-vgpu can
     * build its views of guest pages. */
    say(&cursor, end, "  guest RAM file : %s\n",
        g_board.ram_fd >= 0 ? "memfd (the GPU can reach guest memory)"
                            : "NONE (the GPU cannot reach guest memory)");

#ifdef HENGE_HAVE_REIMS
    if (!g_board.pvg) {
        say(&cursor, end, "  GPU (reims)    : FAILED — rvg_device_create refused the device\n");
        say(&cursor, end, "  reading        : the refusal is on stderr (reims: ... failed: ...)\n");
    } else {
        say(&cursor, end, "  GPU (reims)    : device up\n");
        say(&cursor, end, "  display        : %s\n",
            hv_reims_pvg_have_display(g_board.pvg) ? "yes" : "NO (frames have nowhere to go)");
        say(&cursor, end, "  IOSurface map  : %s\n",
            hv_reims_pvg_have_mapper(g_board.pvg) ? "yes" : "NO");
        /* One tick: proves the device survives being driven, without a guest behind it. */
        hv_reims_pvg_poll(g_board.pvg);
        say(&cursor, end, "  first tick     : survived\n");
    }
#else
    say(&cursor, end, "  GPU            : not built in (reims_pvg.c was left out)\n");
#endif

    g_up = 1;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Booting a guest: the run loop, without host/vm.c.
 *
 * `host/vm.c` is the iOS/macOS host for this: 757 lines of snapshots, audio, input and a
 * CoreFoundation display, built on GCD. Porting its dispatch shims would mean bringing in blocks
 * and a BlocksRuntime that bionic does not have, for a run loop that is four things:
 *
 *   - one thread per CPU, each in `hv_run()`;
 *   - one kicker thread calling `hv_machine_kick_all()` every millisecond, which is what makes the
 *     machine's devices tick (the GPU's `poll` runs from the board's tick, so frames flow from
 *     there);
 *   - the disks, the ROM, and the CPU reset the board expects;
 *   - a stop flag, and threads that end when it is set.
 *
 * That is what this is. Everything else the iOS host does — snapshots, audio, input, the EFI boot
 * framebuffer — is left for later, and named as missing rather than silently absent.
 */

/* Declared before use: the boot entry opens the disks with it, and the disks are file-scope
 * because the board keeps pointers to them for as long as it lives. */
static int hap_disks_open(char **cursor, char *end, const char *disk, const char *aux);

/* The machine identity the guest's disk was installed with. A disk installed under another ECID
 * boots to a refused activation instead of a desktop, so this has to match whatever made the
 * image; a fixed value is right for an image this project installs itself. */
#define HAP_SMOKE_ECID 0x1234567890abcdefull

static int g_running;
static volatile int g_stop;
static pthread_t g_cpu_threads[HV_MAX_CPUS];
static int g_cpu_thread_count;
static pthread_t g_kicker;
static HvDisk g_disk, g_aux;
static int g_disks_open;

static void *cpu_thread(void *arg) {
    HvCpu *c = arg;
    hv_run(c);
    return NULL;
}

static void *kicker_thread(void *arg) {
    (void)arg;
    while (!g_stop) {
        usleep(1000);
        hv_machine_kick_all(&g_machine);
    }
    return NULL;
}

/*
 * Boots the machine described by `cfg`'s files. Returns 0 and leaves the guest running.
 *
 * `report` receives what happened: which file failed, or that the machine is up and for how many
 * CPUs. A refusal is a line of text, never a crash — a missing IPSW is the normal case here.
 */
/*
 * A host fault in translated code: say where, without changing the timeline. host/vm.c has had
 * this since the start (on_ill); this entry point had none, so a SIGSEGV here only produced
 * "Segmentation fault" from the shell and no address at all. Installing a handler costs nothing
 * until a signal arrives, which is why it does not hide the crash the way a log line in the hot
 * path does.
 */
static struct sigaction prior_segv, prior_bus, prior_ill;

static void on_host_fault(int sig, siginfo_t *info, void *ctx) {
    ucontext_t *uc = ctx;
    uint64_t pc = (uint64_t)uc->uc_mcontext.pc;
    uint64_t lr = (uint64_t)uc->uc_mcontext.regs[30];
    uint64_t sp = (uint64_t)uc->uc_mcontext.sp;
    uint64_t rx = (uint64_t)(uintptr_t)g_machine.jit.base;
    uint64_t size = (uint64_t)g_machine.jit.capacity * 4;
    uint64_t used = (uint64_t)g_machine.jit.used * 4;
    int in_rx = rx && pc >= rx && pc < rx + size;
    fprintf(stderr, "hengevm: host signal %d: pc %#llx lr %#llx sp %#llx addr %p (in jit %d, rx%+lld)\n",
            sig, (unsigned long long)pc, (unsigned long long)lr, (unsigned long long)sp,
            info->si_addr, in_rx, (long long)(pc - rx));
    fprintf(stderr, "hengevm:   jit rx %#llx..%#llx used %#llx of %#llx words\n",
            (unsigned long long)rx, (unsigned long long)(rx + size), (unsigned long long)used,
            (unsigned long long)((uint64_t)g_machine.jit.capacity * 4));
    for (int i = 0; i < g_board.ncpus; i++)
        fprintf(stderr, "hengevm:   cpu %d guest pc %#llx el%u halted %d\n", i,
                (unsigned long long)g_cpus[i].pc, g_cpus[i].el, g_cpus[i].halted);
    /* The words around the faulting pc, which is what says whether it landed in a translation
       or in the engine's own code. */
    if (in_rx) {
        uint64_t from = pc - 0x40 < rx ? rx : pc - 0x40;
        fprintf(stderr, "hengevm:   words at rx+%#llx:", (unsigned long long)(from - rx));
        for (uint64_t at = from; at < pc + 0x40 && at < rx + size; at += 4)
            fprintf(stderr, " %08x", *(const uint32_t *)(uintptr_t)at);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
    struct sigaction *prior = sig == SIGILL ? &prior_ill : sig == SIGBUS ? &prior_bus : &prior_segv;
    sigaction(sig, prior, NULL);
}

static void install_host_fault_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = on_host_fault;
    sigaction(SIGSEGV, &sa, &prior_segv);
    sigaction(SIGBUS, &sa, &prior_bus);
    sigaction(SIGILL, &sa, &prior_ill);
}

int hv_android_vm_boot(const char *rom, const char *disk, const char *aux, int ram_mb, int ncpus,
                       const char *cache_dir, char *report, size_t cap) {
    char *cursor = report + strlen(report);
    char *end = report + cap;
    say(&cursor, end, "\n--- boot ---\n");

    if (!rom || !disk || !aux) {
        say(&cursor, end, "  files          : MISSING — ROM, root disk and auxiliary storage are all "
                          "needed\n");
        return -1;
    }
    if (cache_dir && *cache_dir) setenv("XDG_CACHE_HOME", cache_dir, 1);

    if (hap_disks_open(&cursor, end, disk, aux) != 0) return -1;
    install_host_fault_handlers();

    memset(&g_machine, 0, sizeof g_machine);
    memset(&g_board, 0, sizeof g_board);
    memset(g_cpus, 0, sizeof g_cpus);

    if (hv_machine_init(&g_machine, SMOKE_JIT_BYTES) != 0) {
        say(&cursor, end, "  JIT region     : FAILED\n");
        return -1;
    }

    HvCfgParams cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.nr_cpus = (uint32_t)(ncpus < 1 ? 1 : ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : ncpus);
    cfg.ecid = HAP_SMOKE_ECID;
    cfg.seed = 1;
    static const uint8_t mac[6] = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01};
    uint64_t ram = (uint64_t)(ram_mb > 0 ? ram_mb : 2048) << 20;

    if (hv_vmapple_create(&g_board, &g_machine, g_cpus, ram, &g_disk, &g_aux, &cfg, mac) != 0) {
        say(&cursor, end, "  machine        : FAILED\n");
        return -1;
    }
    if (hv_vmapple_load_rom(&g_board, rom) != 0) {
        say(&cursor, end, "  ROM            : FAILED to read %s\n", rom);
        return -1;
    }
    say(&cursor, end, "  ROM            : %s\n", rom);
    say(&cursor, end, "  root disk      : %s\n", disk);
    say(&cursor, end, "  auxiliary      : %s\n", aux);
    say(&cursor, end, "  guest RAM      : %llu MB (%s)\n", (unsigned long long)(ram >> 20),
        g_board.ram_fd >= 0 ? "memfd" : "anonymous — the GPU gets no views");

    hv_vmapple_reset_cpu(&g_board);

    g_stop = 0;
    g_cpu_thread_count = 0;
    for (int i = 0; i < g_board.ncpus; i++) {
        if (pthread_create(&g_cpu_threads[i], NULL, cpu_thread, &g_board.cpu[i]) == 0) {
            g_cpu_thread_count++;
        } else {
            say(&cursor, end, "  cpu %d          : thread FAILED\n", i);
            break;
        }
    }
    if (pthread_create(&g_kicker, NULL, kicker_thread, NULL) != 0) {
        say(&cursor, end, "  kicker         : thread FAILED\n");
        return -1;
    }
    g_running = 1;
    say(&cursor, end, "  running        : %d cpu thread(s) + 1 ms kicker\n", g_cpu_thread_count);
    return 0;
}

/* Opens the two disk images. Split out so the caller can report which one failed. */
static int hap_disks_open(char **cursor, char *end, const char *disk, const char *aux) {
    if (hv_disk_open(&g_disk, disk, 0, 0) != 0) {
        say(cursor, end, "  root disk      : FAILED to open %s\n", disk);
        return -1;
    }
    g_disks_open = 1;
    if (hv_disk_open(&g_aux, aux, 16384, 0) != 0) {
        say(cursor, end, "  auxiliary      : FAILED to open %s\n", aux);
        return -1;
    }
    return 0;
}

/*
 * Injects one HID usage-page-7 key (0x29 = Escape), the same call the app makes through
 * hv_vm_key_hid. Here because the board is static to this file: a command-line bring-up needs a
 * way to wake a guest that is sitting at a prompt or a login window. Any thread, per xhci.h.
 */
void hv_android_key(uint8_t usage, int down) {
    if (g_up && g_board.usb) hv_xhci_key(g_board.usb, usage, down);
}

/*
 * Writes the guest's current frame to `path` (BMP), off-screen. The Android build's counterpart of
 * what the app gets from its Surface: reims encodes into a CPU-readable target, so a command-line
 * run can look at what macOS drew without any window at all.
 */
int hv_android_grab_frame(const char *path) {
#ifdef HENGE_HAVE_REIMS
    if (!g_up || !g_board.pvg) return -1;
    return hv_reims_pvg_grab(g_board.pvg, path);
#else
    (void)path;
    return -1;
#endif
}

/*
 * The boot framebuffer, found the way host/vm.c finds it: scan the first GB of guest RAM for the
 * boot_args structure (revision 2..3, version 1..3, a kernel virtual base in 0xfffffe..., a
 * physical base inside guest RAM) and take the framebuffer description out of it. iBoot puts that
 * structure near 0x78ed0000, which is not the 0x837f0000 page the engine's own boot-args patch
 * looks at -- measured: that page stays at rev 0 ver 0 while the kernel's arguments live here.
 *
 * This is the Android-side counterpart of hv_vm_boot_display's fallback, and it is what lets a
 * command-line run see the firmware's progress bar before the guest ever programs its display.
 */
int hv_android_boot_framebuffer(const uint8_t **pixels, uint32_t *width, uint32_t *height,
                                uint32_t *stride) {
    uint8_t *ram = g_board.ram;
    uint64_t size = g_board.ram_size;
    if (!ram || !size) return -1;
    uint64_t limit = size < (1ull << 30) ? size : 1ull << 30;
    for (uint64_t off = 0; off + 0x1000 <= limit; off += 0x4000) {
        const uint64_t *q = (const uint64_t *)(ram + off);
        const uint16_t *h = (const uint16_t *)(ram + off);
        if (h[0] < 2 || h[0] > 3 || h[1] < 1 || h[1] > 3) continue;
        if (q[1] >> 40 != 0xfffffe || q[2] < VMAPPLE_MEM || q[2] >= VMAPPLE_MEM + size) continue;
        if (q[3] == 0 || q[3] > size) continue;
        uint64_t base = q[5], row = q[7], w = q[8], hh = q[9], depth = q[10];
        if (w < 320 || w > 8192 || hh < 200 || hh > 8192) continue;
        if (row < w * 4 || (depth & 0xff) != 32) continue;
        if (base < VMAPPLE_MEM || base - VMAPPLE_MEM + row * hh > size) continue;
        *pixels = ram + (base - VMAPPLE_MEM);
        *width = (uint32_t)w;
        *height = (uint32_t)hh;
        *stride = (uint32_t)row;
        return 0;
    }
    return -1;
}

/*
 * Hands the host's ANativeWindow to the running machine's GPU device, and the same call again
 * whenever the surface comes back. This is what the app's display has been missing: the
 * SurfaceView exists, but nothing ever gave reims-vgpu a window, so frames had nowhere to go.
 */
int hv_android_is_up(void) { return g_up && g_running; }

void hv_android_attach_window(void *window) {
#ifdef HENGE_HAVE_REIMS
    if (!g_up || !g_board.pvg || !window) return;
    hv_reims_pvg_attach_window(g_board.pvg, window);
#else
    (void)window;
#endif
}

void hv_android_vm_stop(void) {
    if (!g_running) return;
    g_stop = 1;
    pthread_join(g_kicker, NULL);
    for (int i = 0; i < g_cpu_thread_count; i++) pthread_join(g_cpu_threads[i], NULL);
    g_running = 0;
    (void)g_disks_open;
}

void hv_android_vm_teardown(void) {
    if (!g_up) return;
#ifdef HENGE_HAVE_REIMS
    if (g_board.pvg) hv_reims_pvg_destroy(g_board.pvg);
#endif
    hv_vmapple_destroy(&g_board);
    hv_machine_free(&g_machine);
    g_up = 0;
}
