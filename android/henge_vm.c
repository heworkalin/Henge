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

#include <fcntl.h>
#include <pthread.h>
#include <sys/file.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/native_window.h>
#endif

#include "dispatch.h"
#include "machine.h"
#include "vmapple.h"
#include "xhci.h"

#ifdef HENGE_HAVE_REIMS
#include "reims_pvg.h"
#endif

/* Defined below hap_disks_open() but used by the boot entry above it. */
static int lock_root_disk(const char *path, char **cursor, char *end);
static void unlock_root_disk(void);

/* Declared here as well, because the smoke entry tears down a previous attempt before it builds
 * the next one. */
void hv_android_vm_teardown(void);

/* Where the display's frames can go now: "window", "offscreen" or "none" (reims_pvg.h). */
const char *hv_android_display_name(void);

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
/* The activity's window, as the engine holds it: `ANativeWindow_fromSurface`'s reference, kept
   until the display is detached. The render thread inside reims holds the pointer without a
   reference of its own, so this is what keeps it alive. */
static void *g_window;

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

    /* The engine's own statistics: blocks/s, irq/s, wfi/s with the idle share, and the pc of the
       CPU that printed. They are free when off and were off everywhere -- nothing set this -- so a
       guest sitting in WFI and a guest working through an interrupt storm produced the same log.
       On by default now, one line every ~65536 blocks (seconds apart); HV_STATS=0 turns it off. */
    const char *stats = getenv("HV_STATS");
    g_machine.stats_interval = (stats && atoi(stats) == 0) ? 0 : 1;

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
        say(&cursor, end, "  display        : %s\n", hv_android_display_name());
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

/* Which handler owns SIGSEGV right now, and when that changes. A driver that installs its own
   after ours leaves a crash with no marker at all, which is indistinguishable from a handler that
   was never installed unless the installation can be watched. */
static void on_host_fault(int sig, siginfo_t *info, void *ctx);
void hv_install_host_fault_handlers(void);

static void watch_fault_handler(void) {
    struct sigaction cur;
    if (sigaction(SIGSEGV, NULL, &cur) != 0) return;
    void (*now)(int, siginfo_t *, void *) = cur.sa_sigaction;
    if ((void *) now == (void *) on_host_fault) return;
    fprintf(stderr, "watch: SIGSEGV handler was %p (ours %p); reclaiming\n",
            (void *) (uintptr_t) now, (void *) (uintptr_t) on_host_fault);
    fflush(stderr);
    hv_install_host_fault_handlers();
}

static void *kicker_thread(void *arg) {
    (void)arg;
    /* Every ten seconds, which interrupt lines have been used. A guest that stopped is waiting for
       something; the lines that were never pended say what never arrived, which the engine's
       aggregate irq counter cannot. */
    static int seconds;
    while (!g_stop) {
        usleep(1000);
        hv_machine_kick_all(&g_machine);
        /*
         * Input is delivered here, not only from the CPU thread's device tick: that tick also
         * presents the GPU's frames, so a present that blocks for a moment held every key and every
         * finger report behind it (measured: the pointer arrived in bursts, seconds late, while the
         * guest was drawing). This thread runs every millisecond whatever the guest is doing; the
         * device takes its lock, so the two callers cannot interleave a delivery.
         */
        if (g_up && g_board.usb) hv_xhci_poll(g_board.usb);
        if (++seconds % 100 == 0) watch_fault_handler();
        if (seconds % 10000 == 0) hv_gicv3_irq_report(g_board.gic, "boot");
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
    /* First thing, with write() rather than stdio: this handler may run on a thread that was
       holding the stdio lock, and a handler that deadlocks on its first fprintf leaves exactly the
       trace of a handler that never ran. This marker costs one write and settles which it is. */
    {
        static const char mark[] = "HENGE-FAULT\n";
        ssize_t ignored = write(STDERR_FILENO, mark, sizeof mark - 1);
        (void)ignored;
    }
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
    /* Where the pc lives, in terms that survive the process: the executable's load bias turns it
       into a file offset that addr2line can resolve, and /proc/self/maps names the region for both
       addresses. Without these two lines a crash report is a number. */
    {
        extern unsigned long getauxval(unsigned long);
#ifdef AT_PHDR
        unsigned long ph = getauxval(AT_PHDR);
        if (ph > 0x40) {
            unsigned long bias = ph - 0x40;
            fprintf(stderr, "hengevm:   exec bias %#lx  pc off %#lx  lr off %#lx\n",
                    bias, (unsigned long) pc - bias, (unsigned long) lr - bias);
        }
#endif
        FILE *mf = fopen("/proc/self/maps", "r");
        if (mf) {
            char line[512];
            while (fgets(line, sizeof line, mf)) {
                unsigned long lo = 0, hi = 0;
                if (sscanf(line, "%lx-%lx", &lo, &hi) != 2) continue;
                if (pc >= lo && pc < hi) fprintf(stderr, "hengevm:   pc  in %s", line);
                if ((uint64_t)(uintptr_t) info->si_addr >= lo &&
                    (uint64_t)(uintptr_t) info->si_addr < hi)
                    fprintf(stderr, "hengevm:   fa  in %s", line);
            }
            fclose(mf);
        }
    }
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

void hv_install_host_fault_handlers(void) {
    /*
     * Once per process, and never again.
     *
     * The handler restores whatever handler it replaced before returning, because the fault is not
     * recoverable and the next one must terminate the process. Installing it a second time made the
     * "previous" handler our own, so restoring it restored the handler that had just run: the
     * faulting instruction was retried, faulted again, and the thread span there forever. A boot
     * that had died in host code therefore looked alive (2766 faults in one log, guest never
     * advancing, GPU never getting a frame).
     */
    /* Re-installable on purpose. reims-vgpu is Rust, and Rust's standard library installs its own
       SIGSEGV handler (for stack-overflow detection) for every thread it starts, which lands after
       ours and replaces it: the crash then reaches Rust's handler, which decides the fault is not a
       stack overflow, restores the default and re-raises -- so only "Segmentation fault" is ever
       seen. `prior` is captured once, so repeated calls do not chain handlers. */
    static int saved_prior;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sa.sa_sigaction = on_host_fault;
    sigaction(SIGSEGV, &sa, saved_prior ? NULL : &prior_segv);
    sigaction(SIGBUS, &sa, saved_prior ? NULL : &prior_bus);
    sigaction(SIGILL, &sa, saved_prior ? NULL : &prior_ill);
    saved_prior = 1;
}

int hv_android_vm_boot(const char *rom, const char *disk, const char *aux, int ram_mb, int ncpus,
                       const char *cache_dir, char *report, size_t cap) {
    char *cursor = report + strlen(report);
    char *end = report + cap;
    say(&cursor, end, "\n--- boot ---\n");

    /* One machine per process. A second boot would build a second board over the same disk image
       and orphan the first one's threads. */
    if (g_up || g_running) {
        say(&cursor, end, "  already        : a machine is running; stop it first\n");
        return -1;
    }

    if (!rom || !disk || !aux) {
        say(&cursor, end, "  files          : MISSING — ROM, root disk and auxiliary storage are all "
                          "needed\n");
        return -1;
    }
    if (cache_dir && *cache_dir) setenv("XDG_CACHE_HOME", cache_dir, 1);
    /* The activity hands the display its SurfaceView's Surface later, and the library only keeps a
       window link where the display was created with one (android/reims_pvg.c), so the app asks
       for it here. The command-line tool never sets this and stays off-screen. */
    /* The CLI's working set, pinned here until the guest's display has a settings surface: the
       guest's raster order groups hold no fragment interlock, which Mesa implements for no device
       (Turnip has 145 extensions and not that one), so the translator's unordered lowering is
       what lets those pipelines exist at all (46 were refused without it, and a refused pipeline
       is a black layer). See reims-vgpu's os/linux.rs and tools/patches/m2v-rog-unordered.patch. */
    setenv("REIMS_ROG_UNORDERED", "1", 0);
    setenv("HV_REIMS_WINDOW", "1", 0);

    if (lock_root_disk(disk, &cursor, end) != 0) return -1;
    if (hap_disks_open(&cursor, end, disk, aux) != 0) {
        unlock_root_disk();
        return -1;
    }

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
    /* After the machine, not before: creating it brings up reims-vgpu and the Vulkan driver, and
       a driver that installs its own SIGSEGV handler (Adreno's does) overwrote ours when this ran
       first -- a crash then printed "Segmentation fault" from the shell and no marker from a
       handler that was never called. Measured: the marker appeared 0 times before this move. */
    hv_install_host_fault_handlers();

    if (hv_vmapple_load_rom(&g_board, rom) != 0) {
        say(&cursor, end, "  ROM            : FAILED to read %s\n", rom);
        return -1;
    }
    say(&cursor, end, "  ROM            : %s\n", rom);
    say(&cursor, end, "  root disk      : %s\n", disk);
    say(&cursor, end, "  auxiliary      : %s\n", aux);
    say(&cursor, end, "  guest RAM      : %llu MB (%s)\n", (unsigned long long)(ram >> 20),
        g_board.ram_fd >= 0 ? "memfd" : "anonymous — the GPU gets no views");
    /* Where frames can go, decided by whether a window has been attached: a command-line
     * process has none, so it is off-screen and the answer to "is the guest drawing" comes
     * from a grab, not from a screen. */
    say(&cursor, end, "  display kind  : %s\n", hv_android_display_name());

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
    /* `g_up` is what every other entry point treats as "there is a board to talk to":
       hv_android_attach_window, hv_android_is_up, hv_android_display_ready, teardown. It used to
       be set only by the smoke entry, so a real boot left attach_window returning immediately and
       the app concluded the boot had failed while the guest was already running. */
    g_up = 1;
    /* The window the activity handed over before there was a machine to give it to. */
    if (g_window) hv_reims_pvg_attach_window(g_board.pvg, g_window);
    say(&cursor, end, "  running        : %d cpu thread(s) + 1 ms kicker\n", g_cpu_thread_count);
    return 0;
}

/*
 * One machine per disk image.
 *
 * Two processes can be alive at once -- the package reinstall leaves the old process running while
 * the new one starts -- and both would then boot a guest over the same root disk, each writing
 * wherever the other's filesystem happens to point. Nothing about that is detectable from inside
 * either guest, and the damage is the installed macOS. A non-blocking lock on the disk file is
 * what makes the second one fail immediately and visibly instead.
 */
static int g_disk_lock_fd = -1;

static int lock_root_disk(const char *path, char **cursor, char *end) {
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        say(cursor, end, "  root disk      : cannot open %s\n", path);
        return -1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        say(cursor, end, "  root disk      : in use by another instance (%s)\n", path);
        close(fd);
        return -1;
    }
    g_disk_lock_fd = fd;
    return 0;
}

static void unlock_root_disk(void) {
    if (g_disk_lock_fd >= 0) {
        close(g_disk_lock_fd);
        g_disk_lock_fd = -1;
    }
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

/* The same by Linux input key code, which is what Android's `KeyEvent.getScanCode()` reports for
   a hardware keyboard (and what a host's own key table holds). */
void hv_android_key_linux(uint16_t code, int down) {
    if (g_up && g_board.usb) hv_xhci_key_linux(g_board.usb, code, down);
}

/*
 * One finger on the guest's Magic Trackpad 2: the app's touch screen as the pad's surface.
 *
 * `x` and `y` are the pad's own sensor units (HV_TP_SENSOR_X by HV_TP_SENSOR_Y), `ts_ms` the touch
 * sample's clock, `id` a stable 1..15 for one finger, and `phase` 0 down, 1 move, 2 up. Movement
 * per sample is the finger's own: macOS turns it into cursor movement with its own acceleration,
 * so the app must not add any.
 */
void hv_android_touch(int id, int phase, int x, int y, uint32_t ts_ms) {
    if (g_up && g_board.usb) hv_xhci_touch(g_board.usb, id, phase, x, y, ts_ms);
}

/* Ends a batch of moves: every finger that moved since the last frame goes in one report. */
void hv_android_touch_frame(uint32_t ts_ms) {
    if (g_up && g_board.usb) hv_xhci_touch_frame(g_board.usb, ts_ms);
}

/* The absolute tablet: a touch lands exactly where it is (0..32767 across and down). */
void hv_android_tablet(int x16, int y16, int buttons, int wheel) {
    if (g_up && g_board.usb) hv_xhci_tablet(g_board.usb, x16, y16, buttons, wheel);
}

/* A real mouse: relative movement, five buttons (1 = left, 2 = right, 3 = middle), and the wheel. */
void hv_android_mouse_move(int dx, int dy) {
    if (g_up && g_board.usb) hv_xhci_mouse_move(g_board.usb, dx, dy);
}

void hv_android_mouse_button(int button, int down) {
    if (g_up && g_board.usb) hv_xhci_mouse_button(g_board.usb, button, down);
}

void hv_android_mouse_wheel(int notches) {
    if (g_up && g_board.usb) hv_xhci_mouse_wheel(g_board.usb, notches);
}

/* The pad's click: down then up with no movement is a click where the pointer is; held, it is a
   drag (`touchesMoved` while the button is down). */
void hv_android_touch_button(int down) {
    if (g_up && g_board.usb) hv_xhci_touch_button(g_board.usb, down);
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
    static int trace = -1;
    if (trace < 0) trace = getenv("HV_BOOTFB_LOG") != NULL;
    uint64_t limit = size < (1ull << 30) ? size : 1ull << 30;
    for (uint64_t off = 0; off + 0x1000 <= limit; off += 0x4000) {
        const uint64_t *q = (const uint64_t *)(ram + off);
        const uint16_t *h = (const uint16_t *)(ram + off);
        if (h[0] < 2 || h[0] > 3 || h[1] < 1 || h[1] > 3) continue;
        if (q[1] >> 40 != 0xfffffe || q[2] < VMAPPLE_MEM || q[2] >= VMAPPLE_MEM + size) continue;
        if (q[3] == 0 || q[3] > size) continue;
        uint64_t base = q[5], row = q[7], w = q[8], hh = q[9], depth = q[10];
        if (trace) {
            fprintf(stderr, "bootfb: candidate at %#llx rev %u ver %u virt %#llx phys %#llx "
                            "base %#llx row %llu %llux%llu depth %llu\n",
                    (unsigned long long) off, h[0], h[1], (unsigned long long) q[1],
                    (unsigned long long) q[2], (unsigned long long) base,
                    (unsigned long long) row, (unsigned long long) w,
                    (unsigned long long) hh, (unsigned long long) depth);
        }
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
/**
 * Each CPU's guest pc, el and halted flag: {pc, el, halted} per CPU.
 *
 * Returns how many CPUs were written. This is what tells a running guest from a stopped one: a pc
 * that changes is a guest making progress, and a fixed pc over seconds says exactly where it
 * stopped, which no amount of device-level logging can.
 */
int hv_android_cpu_snapshot(uint64_t *out, int max_cpus) {
    if (!out || max_cpus <= 0) return 0;
    int n = g_board.ncpus < max_cpus ? g_board.ncpus : max_cpus;
    for (int i = 0; i < n; i++) {
        out[i * 3 + 0] = g_cpus[i].pc;
        out[i * 3 + 1] = g_cpus[i].el;
        out[i * 3 + 2] = g_cpus[i].halted ? 1 : 0;
    }
    return n;
}

int hv_android_is_up(void) { return g_up && g_running; }

/**
 * True once the guest has programmed a display mode and presented at least once: the point where
 * the app should stop showing the firmware's boot framebuffer and let the window show the GPU.
 */
/** Render state for the app: {has mode, guest presents, host presents}. */
void hv_android_render_stats(int out[3]) {
    out[0] = out[1] = out[2] = 0;
#ifdef HENGE_HAVE_REIMS
    if (!g_up || !g_board.pvg) return;
    extern void hv_reims_pvg_stats(HvReimsPvg *p, int out[3]);
    hv_reims_pvg_stats(g_board.pvg, out);
#endif
}

int hv_android_display_ready(void) {
#ifdef HENGE_HAVE_REIMS
    if (!g_up || !g_board.pvg) return 0;
    extern int hv_reims_pvg_ready(HvReimsPvg *p);
    return hv_reims_pvg_ready(g_board.pvg);
#else
    return 0;
#endif
}

/*
 * "window", "offscreen" or "none", for the boot report and the per-run verdict.
 *
 * Off-screen is not a failure: the guest can still set a mode and present, and
 * hv_android_grab_frame reads the frame back. It is only "cannot show a picture on a screen",
 * which is always true of a command-line process (no surface, no display permission).
 */
const char *hv_android_display_name(void) {
#ifdef HENGE_HAVE_REIMS
    if (!g_board.pvg) return "none (no GPU display)";
    switch (hv_reims_pvg_display_kind(g_board.pvg)) {
    case HV_REIMS_DISPLAY_WINDOW: return "window (frames go to the host's screen)";
    case HV_REIMS_DISPLAY_OFFSCREEN:
        return "offscreen (no window: frames can be encoded and read back)";
    default: return "none (the display was refused)";
    }
#else
    return "none (reims_pvg.c was left out)";
#endif
}

void hv_android_attach_window(void *window) {
#ifdef HENGE_HAVE_REIMS
    if (!window) return;
    /* The activity's window, kept for as long as the machine lives: `ANativeWindow_fromSurface`
       handed us a reference, and the render thread inside reims holds the raw pointer without
       one, so this is the reference that keeps it alive until [`hv_android_detach_window`]. */
    if (g_window && g_window != window) ANativeWindow_release(g_window);
    g_window = window;
    if (!g_up || !g_board.pvg) {
        /* Said out loud because it is silent and fatal to the picture: the window arrives when the
           activity is created, which is before 启动 is pressed, so there is no machine to hand it to
           yet. The machine takes it when it comes up (`hv_android_vm_boot`), and the activity
           attaches again once it is up; if this line is the last thing about the window, neither
           happened. */
        fprintf(stderr, "reims: window kept for the next machine (none is up yet)\n");
        return;
    }
    hv_reims_pvg_attach_window(g_board.pvg, window);
#else
    (void)window;
#endif
}

/*
 * The activity's surface went away (the screen was put away, or the activity was destroyed).
 *
 * Without this the engine keeps presenting into a window the compositor no longer consumes, and a
 * FIFO present blocks under the presenting queue's lock: the guest's own submissions then wait on
 * it, and the machine stops without saying so. Detaching first is also what makes the join in
 * [`hv_android_vm_stop`] able to return.
 */
void hv_android_detach_window(void) {
#ifdef HENGE_HAVE_REIMS
    if (g_up && g_board.pvg) hv_reims_pvg_detach_window(g_board.pvg);
    if (g_window) {
        ANativeWindow_release((ANativeWindow *)g_window);
        g_window = NULL;
    }
#endif
}

/*
 * One report for a guest that stopped: every CPU's architectural state (why it is not taking an
 * interrupt: PSTATE.DAIF, ELR, ESR/FAR of the last exception), every interrupt line's live GIC
 * state, and, on request, the guest's threads and what each waits on.
 *
 * This is deliberately one function and one report: "the log stopped" is answered by the CPU
 * state, "the event never came" by the GIC state, and "who is waiting" by the thread scan.
 * HV_STALL_THREADS=1 adds the scan (it walks guest RAM, so it is opt-in: a running guest can move
 * the structures under it).
 */
void hv_android_stall_report(void) {
    extern void hv_threads_dump(void);
    fprintf(stderr, "hengevm: stall report: %d cpu(s)\n", g_board.ncpus);
    for (int i = 0; i < g_board.ncpus; i++) {
        HvCpu *c = &g_cpus[i];
        fprintf(stderr,
                "hengevm: stall cpu %d pc %016llx el%u daif %03llx halted %d elr %016llx"
                " esr %016llx far %016llx sp0 %016llx tpidr %016llx\n",
                i, (unsigned long long)c->pc, c->el, (unsigned long long)c->daif,
                (int)c->halted,
                (unsigned long long)c->elr_el1, (unsigned long long)c->esr_el1,
                (unsigned long long)c->far_el1, (unsigned long long)c->sp_el0,
                (unsigned long long)c->tpidr_el1);
    }
    hv_gicv3_irq_report(g_board.gic, "stall");
    hv_gicv3_state_report(g_board.gic, "stall");
    /* Only once the guest has left the reset vector: at 0x100000 there is no address space yet and
       the scan answers "0 processes". At most twice per process, because the scan walks guest RAM
       and says the same thing about a guest that has not moved. */
    static int dumps;
    const char *threads = getenv("HV_STALL_THREADS");
    int up = 0;
    for (int i = 0; i < g_board.ncpus; i++)
        if (g_cpus[i].pc != 0x100000ull) up = 1;
    if (threads && *threads && strcmp(threads, "0") != 0 && up && dumps < 2) {
        dumps++;
        hv_threads_dump();
    }
}

void hv_android_vm_stop(void) {
    if (!g_running) return;
    /* The display first: a render thread blocked in a present holds the presenting queue's lock,
       and the guest's submissions (and so the joins below) wait on it. */
#ifdef HENGE_HAVE_REIMS
    if (g_up && g_board.pvg) hv_reims_pvg_detach_window(g_board.pvg);
#endif
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
    unlock_root_disk();
}
