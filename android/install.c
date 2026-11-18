/*
 * Installing macOS from an IPSW, on Android.
 *
 * This mirrors `ios/Henge/Native/henge-vm.c`'s install entry, which mirrors what
 * Virtualization.framework's VZMacOSInstaller does: the machine boots its ROM in DFU mode, shows
 * up on the host's USB port, and a standard restore — personalized against Apple's signing server
 * for this machine's ECID — writes the disk over that link.
 *
 * Nothing of Apple's is decrypted or patched here. `host/install.c` drives libirecovery, usbmuxd
 * and idevicerestore, unmodified, against `host/libusb_shim.c`, which makes the guest's virtio USB
 * port look like a USB device to them.
 *
 * The two images have to exist before the restore starts, and their shapes are Apple's:
 *
 *   - auxiliary storage: a 24-byte header and then zeros, 32 MB + 16 KB in all (VZMacAuxiliaryStorage)
 *   - the root disk: a sparse file, sized by the user
 *
 * The restore writes both. A disk that is too small fails late and expensively, so the default is
 * generous and sparse: it costs address space, not storage, until macOS writes to it.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android/log.h>
#include <mach/mach_time.h>

#include "dispatch.h"
#include "exception.h"
#include "mmu.h"
#include "sysreg.h"
#include "disk.h"
#include "install.h"
#include "machine.h"
#include "vmapple.h"
#include "net_slirp.h"   /* net_min.c implements this API; net_slirp.c needs libslirp */

/* Bytes of the auxiliary storage image: 32 MB of data after a 16 KB header block. */
#define AUX_BYTES 33570816ull

/* The root disk's default size. macOS wants at least ~32 GB; 64 GiB leaves room to work and costs
 * nothing until written, because the file is sparse. */
#define DISK_BYTES_DEFAULT (64ull << 30)

static void *monitor_thread(void *arg);
static void *kicker_thread(void *arg);

static HvMachine g_machine;
static HvVmapple g_board;
/*
 * One HvCpu per possible CPU, not one in total: `hv_vmapple_create` takes a pointer and the board
 * indexes it as `cpu[i]` for every CPU in the config, so a single object is out of bounds the
 * moment more than one CPU is asked for — which used to corrupt whatever followed it in .bss and
 * then fault in the middle of resetting a CPU's caches.
 */
static HvCpu g_cpus[HV_MAX_CPUS];
static HvDisk g_disk, g_aux;
static volatile int g_install_step = -1;
/*
 * Which run a progress report belongs to.
 *
 * A restore that failed leaves its own thread in idevicerestore for a while, and that thread keeps
 * reporting: the last thing the previous run says is "failed", and a new attempt that reads it a
 * second after starting decides it has failed too — the second attempt that went to 0% and stopped
 * on its own. Only the current run may write the step, and a new run starts from "nothing yet"
 * rather than inheriting the previous verdict.
 */
static volatile int g_install_gen;
static volatile double g_install_progress;
static int g_running;

/* Defined in henge_vm.c: one machine at a time, so starting one means stopping the other. */
void hv_android_vm_teardown(void);
void hv_android_install_stop(void);
int hv_android_install_running(void);

static void say(char **cursor, char *end, const char *fmt, ...) {
    if (*cursor >= end) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*cursor, (size_t)(end - *cursor), fmt, ap);
    va_end(ap);
    if (n > 0) *cursor += n;
    if (*cursor > end) *cursor = end;
}

/* VZMacAuxiliaryStorage's new file: a 24-byte header, the rest zero. */
int hv_android_make_aux(const char *path) {
    static const unsigned char header[24] = {0xa5, 0xd5, 0xb3, 0xfe, 0x97, 0xad, 0x1d, 0x27,
                                             1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, header, sizeof header) == (ssize_t)sizeof header
             && ftruncate(fd, (off_t)AUX_BYTES) == 0;
    close(fd);
    return ok ? 0 : -1;
}

/* A sparse root disk of `bytes`. Refuses to shrink an existing one. */
int hv_android_make_disk(const char *path, uint64_t bytes) {
    if (bytes == 0) bytes = DISK_BYTES_DEFAULT;
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
    struct stat st;
    int ok = 0;
    if (fstat(fd, &st) == 0) {
        if ((uint64_t)st.st_size >= bytes) {
            ok = 1;                       /* already big enough: leave its contents alone */
        } else {
            ok = ftruncate(fd, (off_t)bytes) == 0;
        }
    }
    close(fd);
    return ok ? 0 : -1;
}

static void stop_cpus(void);

static void on_install_progress(int step, double progress, void *user) {
    if ((int)(intptr_t)user != __atomic_load_n(&g_install_gen, __ATOMIC_RELAXED)) return;
    /* The restore is over: its guest (a panic, in a failed one) is spinning in a wfe loop that will
       never end. Stopping the CPUs here is what lets the next attempt build a machine at all.
       Unless a console breakpoint is armed: HENGE_STOP_ON means someone is going to attach gdb and
       read this machine, and the state that matters is exactly the one the timeout would tear
       down. Then the guest keeps running (idle or stuck, which is what is being measured) and only
       an explicit kill ends the process. */
    if (step >= HV_INSTALL_DONE) {
        const char *stop_on = getenv("HENGE_STOP_ON");
        if (stop_on && *stop_on) {
            static int said;
            if (!said) {
                said = 1;
                fprintf(stderr, "hengevm: install ended (step %d, r %s) but HENGE_STOP_ON is set: "
                                "keeping the machine alive for inspection; kill -9 %d to exit\n",
                        step, step == HV_INSTALL_DONE ? "done" : "failed", (int) getpid());
                fflush(stderr);
            }
        } else {
            stop_cpus();
        }
    }
    int previous = g_install_step;
    g_install_step = step;
    g_install_progress = progress;
    /*
     * The progress is logged, not just stored.
     *
     * A restore spends most of its time inside libtatsu and idevicerestore, which say nothing on a
     * log this process can see; without these lines a stuck install and a working one look exactly
     * the same from the outside, and every report becomes a guess.
     */
    if (step != previous) {
        __android_log_print(ANDROID_LOG_INFO, "HENGE",
                            "安装进度: step %d (%.0f%%)", step, progress * 100.0);
    }
}

/* HV_BLOCK_DUMP points at a file the engine writes every block to (see dispatch.c). */
static int startup_dump_blocks(const char *workdir) {
    if (!workdir || !*workdir) return -1;
    /* HENGE_NO_DUMP: on a host, where a run is one command and can be repeated, the per-block
       dump and the watched address are gigabytes of I/O for a question that was already asked. */
    if (getenv("HENGE_NO_DUMP")) return 0;
    static char path[512];
    snprintf(path, sizeof path, "%s/blocks.bin", workdir);
    setenv("HV_BLOCK_DUMP", path, 1);
    setenv("HV_BLOCK_DUMP_EVERY", "1", 1);
    /* HV_WATCH_VA / HV_WATCH_PA have no defaults: a watch is armed only when the caller names an
       address. A default here is worse than nothing -- a page watch fires for every store in the
       page, so a hardcoded one buries the store worth seeing under hundreds of unrelated ones and
       reads as if the engine had hit the address that was actually asked for.
       The physical form exists because the firmware builds the kernel image with the MMU off, so a
       write there is a write to that PA and never matches a kernel VA. */
    /* One run, for the question of whether the firmware decoded the bytes that were sent. */
    if (getenv("HENGE_DUMP_RAM")) setenv("HV_DUMP_RAM", "1", 1);
    return 0;
}

static void *cpu_thread(void *arg) {
    hv_run((HvCpu *)arg);
    return NULL;
}

/*
 * Starts the installer. Returns 0 once it is running.
 *
 * The machine is built with `installer = 1`, which is what puts the ROM into DFU mode and gives
 * the guest a USB device port for the restore to appear on. `hv_install_start` then runs on its
 * own threads and the CPUs are started here: the restore OS is guest code, so it only makes
 * progress while they run.
 */
int hv_android_install_start(const char *rom, const char *disk, const char *aux, const char *ipsw,
                             const char *workdir, int ram_mb, int ncpus, uint64_t ecid,
                             const char *cache_dir, char *report, size_t cap) {
    char *cursor = report + strlen(report);
    char *end = report + cap;
    say(&cursor, end, "\n--- install ---\n");
    /* Which build this log is from: a run that started before an install finished has produced
       more than one confusing log while this was being worked on. */
    fprintf(stderr, "HENGEBUILD %s %s\n", __DATE__, __TIME__);

    if (!rom || !disk || !aux || !ipsw) {
        say(&cursor, end, "  files          : MISSING — ROM, disk, auxiliary storage and IPSW are all needed\n");
        return -1;
    }
    if (cache_dir && *cache_dir) setenv("XDG_CACHE_HOME", cache_dir, 1);


    // One machine per process, and the graphics device says so: reims-vgpu refuses to create a
    // second display with the same serial number, which is what an install started while a boot
    // was still up used to hit (a warning, then a display that had been destroyed underneath it).
    // Tearing the previous machine down first is the only ordering that is correct.
    if (g_running) {
        if (g_install_step >= HV_INSTALL_DONE) {
            /* Finished or failed: its CPUs are still looping in guest code, so say stop rather
               than refuse. This is what "cannot install again after it failed" was. */
            say(&cursor, end, "  previous run   : ended (step %d), stopping it\n", g_install_step);
            hv_android_install_stop();
        } else {
            say(&cursor, end, "  previous run   : still running (step %d)\n", g_install_step);
            return -1;
        }
    }
    /* Before the disks are opened, not after: the teardown closes them, and a machine built on
       descriptors that have just been closed answers every disk request with EBADF. */
    hv_android_vm_teardown();
    hv_disk_close(&g_disk);
    hv_disk_close(&g_aux);

    if (hv_disk_open(&g_disk, disk, 0, 0) != 0) {
        say(&cursor, end, "  root disk      : FAILED to open %s (%s)\n", disk, strerror(errno));
        return -1;
    }
    if (getenv("HENGE_DISK_LOG"))
        fprintf(stderr, "hengevm: disks root fd %d size %#llx, aux fd %d size %#llx\n",
                g_disk.fd, (unsigned long long) g_disk.size, g_aux.fd, (unsigned long long) g_aux.size);
    if (hv_disk_open(&g_aux, aux, 16384, 0) != 0) {
        say(&cursor, end, "  auxiliary      : FAILED to open %s (%s)\n", aux, strerror(errno));
        return -1;
    }

    memset(&g_machine, 0, sizeof g_machine);
    memset(&g_board, 0, sizeof g_board);
    memset(g_cpus, 0, sizeof g_cpus);

    if (hv_machine_init(&g_machine, (size_t)768 << 20) != 0) {
        say(&cursor, end, "  JIT region     : FAILED\n");
        return -1;
    }
    /*
     * Engine diagnostics on, for an install only.
     *
     * The restore runs firmware this port has never executed: the first code after the ROM hands
     * over is iBSS, and when it stops the only account of why is what the engine saw — an unknown
     * system register, an undefined instruction, an access to an unmapped page. A normal boot stays
     * quiet; an install is exactly when that silence is worth nothing.
     */
    /* Level 2: every exception the guest takes, plus the block trace ring — enough to explain a
       fault, without the per-MMIO and per-sysreg printing of level 3, which slows the guest enough
       that the restore's own device discovery times out. */
    /* Level 2 prints every exception the guest takes, with its ESR, FAR, PC and EL, which is what
       a restore that panics in firmware needs: the moment before the panic matters more than the
       panic, and the guest only reports the last one. */
    g_machine.log = getenv("HV_LOG") ? atoi(getenv("HV_LOG")) : 2;
    /* The engine counts where its own time goes (translate, device-lock waits, wfe waits) but only
       prints it when a sampling interval is set, and nothing ever set one. */
    g_machine.stats_interval = getenv("HV_STATS") ? strtoull(getenv("HV_STATS"), NULL, 10) : 0;
    /* HV_TRACE keeps the last 256 block entries (VA, x0, x4) so a fault can be read backwards;
       the per-block print stays at log 4, or tracing would be the noise it is meant to explain. */
    g_machine.trace = getenv("HV_TRACE") != NULL;
    /* HV_PTW_LOG: record writes into pages the walk is using as translation tables, and
       HV_SLOW_MEM: send every load and store through C so those writes are all visible. */
    g_machine.ptw_log = getenv("HV_PTW_LOG") != NULL;
    g_machine.log_pc = getenv("HV_LOGPC") ? strtoull(getenv("HV_LOGPC"), NULL, 16) : 0;
    /* HV_PTW_SNAP: sample the tables every N blocks and keep the differences, which is the
       write history of every entry without slowing the guest down to catch the stores. */
    g_machine.ptw_snap_every = getenv("HV_PTW_SNAP") ? (uint32_t) atoi(getenv("HV_PTW_SNAP")) : 0;
    if (g_machine.ptw_log)
        fprintf(stderr, "hengevm: ptw log on, sampling every %u blocks\n", g_machine.ptw_snap_every);
    /* slow_mem is switched on when the kernel starts, not here: the firmware's own boot does not
       survive every access going through C, and its part of the run is not what is being read. */
    /*
     * Every translated block, written to the work directory: the guest words it came from and the
     * host words it became. A register that goes wrong in translated code cannot be explained from
     * the guest side — the guest's own instructions do not touch it — so the emitted code has to be
     * read, and this is how it becomes readable.
     */
    /*
     * Knobs from a file, so a codegen difference can be tested without rebuilding anything:
     *
     *   work/knobs.txt:   HV_K2=1
     *                     HV_SUPERBLOCKS=0
     *
     * Every knob the engine has is an environment variable (core/machine.c), so this only has to
     * read the file before the machine is built. The dump files are tagged with a hash of these
     * lines, so two runs with different knobs do not overwrite each other's evidence.
     */
    {
        char kpath[512];
        snprintf(kpath, sizeof kpath, "%s/knobs.txt", workdir ? workdir : ".");
        FILE *kf = fopen(kpath, "r");
        unsigned long tag = 2166136261ul;
        if (kf) {
            char line[256];
            while (fgets(line, sizeof line, kf)) {
                char *eq = strchr(line, '=');
                if (!eq) continue;
                *eq = 0;
                char *name = line;
                char *val = eq + 1;
                for (char *c = val; *c; c++) if (*c == '\n' || *c == '\r') *c = 0;
                if (!*name) continue;
                setenv(name, val, 1);
                for (const char *c = name; *c; c++) tag = (tag ^ (unsigned char) *c) * 16777619ul;
                for (const char *c = val; *c; c++) tag = (tag ^ (unsigned char) *c) * 16777619ul;
                say(&cursor, end, "  knob           : %s=%s\n", name, val);
            }
            fclose(kf);
            char t[32];
            snprintf(t, sizeof t, "%08lx", tag & 0xfffffffful);
            setenv("HV_DUMP_TAG", t, 1);
            say(&cursor, end, "  knob tag       : %s\n", t);
        }
    }
    if (startup_dump_blocks(workdir) != 0)
        say(&cursor, end, "  block dump     : unavailable\n");
    /*
     * EXPERIMENT (not a fix): chaining lets one block jump straight into the next, which is where
     * the restore firmware's bit reader ends up reading a host address out of a guest register.
     * With chaining off every block is entered through the dispatcher, which restores all guest
     * registers from their slots — if this makes iBSS run, the corruption is in what a chained
     * entry assumes about registers that live in slots.
     */
    /* Chaining was turned off here to test whether an entry that skips the dispatcher is what
       corrupts a register: it is not — the same fault happens either way. Back on. */

    HvCfgParams cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.nr_cpus = (uint32_t)(ncpus < 1 ? 1 : ncpus > HV_MAX_CPUS ? HV_MAX_CPUS : ncpus);
    cfg.ecid = ecid;
    cfg.seed = 1;
    cfg.run_installer1 = cfg.run_installer2 = 1;
    static const uint8_t mac[6] = {0x02, 0x48, 0x56, 0x4d, 0x00, 0x01};
    uint64_t ram = (uint64_t)(ram_mb > 0 ? ram_mb : 4096) << 20;

    if (hv_vmapple_create(&g_board, &g_machine, g_cpus, ram, &g_disk, &g_aux, &cfg, mac) != 0) {
        say(&cursor, end, "  machine        : FAILED\n");
        return -1;
    }
    if (hv_vmapple_load_rom(&g_board, rom) != 0) {
        say(&cursor, end, "  ROM            : FAILED to read %s\n", rom);
        return -1;
    }
    hv_vmapple_reset_cpu(&g_board);

    /* The guest's network has no backend unless one is attached: without it the
       interface never finishes DHCP and its first wait never returns. */
    if (g_board.net && hv_net_attach(&g_machine, g_board.net) != 0)
        fprintf(stderr, "hengevm: no network backend attached\n");
    else if (g_board.net)
        fprintf(stderr, "hengevm: network backend attached (net_min)\n");

    /* Nothing has been reported by this run yet: keeping the previous verdict here is what made
       the attempt after a failure look like it had already failed. */
    g_install_step = -1;
    g_install_progress = 0;
    int gen = __atomic_add_fetch(&g_install_gen, 1, __ATOMIC_RELAXED);
    if (hv_install_start(g_board.vusb, ipsw, workdir, ecid, on_install_progress,
                         (void *)(intptr_t)gen) != 0) {
        say(&cursor, end, "  installer      : could not start\n");
        return -1;
    }
    say(&cursor, end, "  installer      : started\n");
    say(&cursor, end, "  IPSW           : %s\n", ipsw);
    say(&cursor, end, "  work directory : %s\n", workdir);

    /* The restore OS is guest code: it only runs while the CPUs do. */
    for (int i = 0; i < g_board.ncpus; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, cpu_thread, &g_board.cpu[i]) == 0) pthread_detach(t);
    }
    g_running = 1;
    /* HENGE_KICKER=0 turns it off for a comparison run; on by default because without it the
       guest gets a timer only when a block happens to end. */
    if (!getenv("HENGE_KICKER") || atoi(getenv("HENGE_KICKER")) != 0) {
        pthread_t kt;
        if (pthread_create(&kt, NULL, kicker_thread, NULL) == 0) pthread_detach(kt);
    }
    if (getenv("HENGE_HEARTBEAT")) {
        pthread_t mt;
        if (pthread_create(&mt, NULL, monitor_thread, NULL) == 0) pthread_detach(mt);
    }
    return 0;
}

/* A clock on the wall, not on the guest: with blocks chained, neither icount nor the block count
   moves once a loop is hot, so a run that looks idle and a run that is stuck look the same. This
   samples the CPU from another thread and says where the guest is, and whether that spot changes. */
/* The engine only services timers, devices and interrupts when the dispatcher runs, and chained
   blocks do not return to it. host/vm.c has this thread; the app and the host installer build
   their own CPU threads instead, so without it the guest gets a tick only when a block happens to
   end: one interrupt in ninety seconds, and every timeout in the restore path expires. */
/* Which processes the kernel actually created. p_comm is a 16-byte name at a known offset in
   struct proc, and kalloc'd procs live in the kernel heap; scanning RAM for the names is cruder
   than walking allproc from a symbol, but it needs no symbol table and the answer it gives -- was
   restored ever forked -- is the one that separates "restored never started" from "restored ran
   and its USB side did not". Set HV_SCAN_PROC=1 to run it. */
/* Process names, and the kexts whose absence would explain a kernel that runs userland and
   touches no device: every device driver on this platform is a kext (ApplePCIe for the host
   bridge the virtio devices sit on, IOUSBHostFamily for the port restored talks to). Counting
   their names in RAM says whether the boot got far enough to load them. */
static void scan_proc_names(void) {
    static const char *const names[] = {"launchd", "restored", "restored_external",
                                        "mobile_restore", "watchdogd", "launchctl",
                                        "kernel_task", "usbcfwflasher", "diskimagesiod",
                                        "ApplePCIe", "IOUSBHost", "AppleUSBHost",
                                        "com.apple.iokit.IOUSBHostFamily",
                                        "AppleVirtIONetPCI", "VirtIOBlockDevice"};
    /* "restored" is a prefix of "restored_external", and the scan matches the byte after the
       name against \0, so both can be counted separately: a hit for "restored" means the name
       really ends there. */
    const int n = (int)(sizeof names / sizeof names[0]);
    int hits[16] = {0};
    for (uint64_t pa = 0x70000000ull; pa < 0x90000000ull; pa += 0x1000) {
        uint8_t pg[4096];
        if (hv_phys_read(&g_machine, pa, pg, sizeof pg) != 0) continue;
        for (int i = 0; i + 18 <= 4096; i++) {
            for (int k = 0; k < n; k++) {
                if (hits[k] >= 3) continue;
                size_t l = strlen(names[k]);
                if (memcmp(pg + i, names[k], l) != 0) continue;
                if (pg[i + l] != 0 && pg[i + l] >= 32 && pg[i + l] < 127) continue;
                hits[k]++;
                fprintf(stderr, "hengevm: proc name \"%s\" at pa %llx\n", names[k],
                        (unsigned long long) (pa + (uint64_t) i));
            }
        }
    }
    fprintf(stderr, "hengevm: proc scan:");
    for (int k = 0; k < n; k++) fprintf(stderr, " %s=%d", names[k], hits[k]);
    fprintf(stderr, "\n");
}

/* The firmware hands the machine to the restore kernel with bootx; on real hardware that also
   resets the USB controller, so the device leaves the bus and idevicerestore's
   recovery_enter_restore_mode() waits for exactly that ("Waiting for device to disconnect").
   The guest never tells us: bdif only ever reports the channel going online, never offline, so
   without this the host sits in cond_wait_timeout(30000) and reports "Failed to place device in
   restore mode" even though the device did re-enumerate. Bouncing the link once, when the guest
   first executes kernel code, reproduces the disconnect and also makes usbmuxd enumerate again
   after restored is up (the early enumeration happens before userland listens). */
static void usb_handoff_to_kernel(void) {
    static int done;
    if (done || !g_board.vusb) return;
    /* 0xfffffe00... is the kernel's half; the firmware runs at 0x700xxxxx. */
    if (!g_cpus[0].pc || g_cpus[0].pc < 0xfffffe0000000000ull) return;
    done = 1;
    fprintf(stderr, "hengevm: restore kernel entered; bouncing the USB link (host expects the "
                    "device to disconnect)\n");
    hv_vusb_force_rescan(g_board.vusb);
}

static void *kicker_thread(void *arg) {
    (void) arg;
    uint64_t n = 0;
    int scanned = 0;
    fprintf(stderr, "hengevm: kicker up (scan=%s)\n", getenv("HV_SCAN_PROC") ? "yes" : "no");
    while (g_running) {
        usleep(1000);
        usb_handoff_to_kernel();
        hv_net_poll();                  /* frames the backend owes the guest */
        hv_machine_kick_all(&g_machine);
        n++;
        /* Once, a few seconds in: early enough that restored would have been forked by then when
           the ramdisk mounted, late enough that the boot has got past the first init. */
        if (getenv("HV_SCAN_PROC") && !scanned && n >= 4000) {
            scanned = 1;
            fprintf(stderr, "hengevm: scanning for proc names at n=%llu\n", (unsigned long long) n);
            scan_proc_names();
            fprintf(stderr, "hengevm: proc scan done\n");
        }
        if (getenv("HENGE_HEARTBEAT") && (n % 1000) == 0)
            fprintf(stderr, "hengevm: kicked %llu times, ncpus %d\n",
                    (unsigned long long) n, g_machine.ncpus);
    }
    return NULL;
}

static void dump_last_el0(void);

static void *monitor_thread(void *arg) {
    (void) arg;
    uint64_t t0 = mach_absolute_time();
    uint64_t prev_pc = 0, changes = 0, samples = 0, same = 0;
    /* Host CPU time is the only honest measure of "is the guest actually running".  A chained
       superblock loops entirely inside translated code and never calls back into C, so cpu->pc,
       the block counters and the EL0 trace all look frozen while the host is in fact burning a
       core.  utime does not lie. */
    uint64_t prev_utime = 0;
    while (g_running && !g_machine.stop) {
        struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        HvCpu *c = &g_cpus[0];
        uint64_t pc = c->pc;
        samples++;
        if (pc != prev_pc) changes++; else same++;
        prev_pc = pc;
        {
            uint64_t u = 0;
            FILE *sf = fopen("/proc/self/stat", "r");
            if (sf) {
                char buf[1024];
                if (fgets(buf, sizeof buf, sf)) {
                    char *p2 = strrchr(buf, ')');
                    if (p2) {
                        unsigned long ut = 0, st = 0;
                        if (sscanf(p2 + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
                                   &ut, &st) == 2) u = ut + st;
                    }
                }
                fclose(sf);
            }
            uint64_t d = u - prev_utime;
            prev_utime = u;
            fprintf(stderr, "hengevm: cpu-ticks +%llu (pc %#llx)\n",
                    (unsigned long long) d, (unsigned long long) pc);
        }
        /* Dump the last userspace page as soon as userland stops moving.  Waiting for the restore
           timeout to fire costs minutes and tells us nothing new; twenty seconds without a new
           EL0 block is already conclusive, and the dump is the whole point of the run. */
        {
            extern uint64_t hv_last_el0_va;
            static uint64_t prev_el0;
            static int el0_same, dumped;
            if (hv_last_el0_va && hv_last_el0_va == prev_el0) {
                if (++el0_same == 10 && !dumped && !getenv("HV_NO_EL0_DUMP")) {
                    dumped = 1;
                    fprintf(stderr, "hengevm: userland has not moved for 20s (last %#llx)\n",
                            (unsigned long long) hv_last_el0_va);
                    dump_last_el0();
                }
            } else {
                el0_same = 0;
                prev_el0 = hv_last_el0_va;
            }
        }
        /* What is at that address, read through the guest's own tables: an address alone does not
           say whether it is the idle loop, a spin waiting for a device, or a panic handler. */
        uint32_t w[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; k++) {
            HvWalk walk;
            hv_mmu_walk(c, pc + (uint64_t) k * 4, HV_ACC_READ, 1, &walk);
            if (walk.fsc == 0) hv_phys_read(&g_machine, walk.pa, &w[k], 4);
        }
        fprintf(stderr, "hengevm: alive %.0fs pc %llx el%u daif %llx exc %llu irq %llu "
                        "moved %llu/%llu vctl %llx cval %llx now-off %llx vp %llx\n",
                (double) (mach_absolute_time() - t0) / 24000000.0, (unsigned long long) pc,
                c->el, (unsigned long long) c->daif,
                (unsigned long long) g_machine.stats.exceptions,
                (unsigned long long) g_machine.stats.interrupts,
                (unsigned long long) changes, (unsigned long long) samples,
                (unsigned long long) c->cntv_ctl, (unsigned long long) c->cntv_cval,
                (unsigned long long) (hv_counter() - c->cntvoff),
                (unsigned long long) g_machine.stats.interrupts);
        /* The GIC's own view of the virtual timer line: a timer that fires is not an interrupt
           delivered, and only these say which of the gates is shut. */
        if (g_board.gic) {
            HvGicCpu *g = &g_board.gic->cpu[0];
            /* If the kernel panicked, its message is in guest RAM; the console is not saying
           anything, so the memory is the only place to read it from. */
        if (getenv("HENGE_DUMP_RAM_AT") && getenv("HENGE_DUMP_RAM_AT")[0] &&
            samples == (uint64_t) atoi(getenv("HENGE_DUMP_RAM_AT")) / 2) {
            const char *wd = getenv("HV_BLOCK_DUMP");
            if (!wd || !*wd) wd = "/home/he/Henge/work";
            fprintf(stderr, "hengevm: dump checkpoint at %.0fs into %s\n",
                    (double) (mach_absolute_time() - t0) / 24000000.0, wd);
            {
                char dp[512];
                snprintf(dp, sizeof dp, "%s", wd);
                char *sl = strrchr(dp, '/');
                if (sl) snprintf(sl + 1, sizeof dp - (size_t)(sl + 1 - dp), "ram_end.bin");
                FILE *f = fopen(dp, "wb");
                if (f) {
                    size_t total = 0;
                    for (int r = 0; r < g_machine.nram; r++) {
                        HvRam *ram = &g_machine.ram[r];
                        size_t n = ram->size;
                        if (n > (192u << 20)) n = 192u << 20;   /* the interesting half */
                        uint8_t *buf = malloc(1u << 20);
                        for (size_t off = 0; off < n && buf; off += 1u << 20) {
                            size_t chunk = n - off < (1u << 20) ? n - off : (1u << 20);
                            if (hv_phys_read(&g_machine, ram->pa + off, buf, chunk) != 0) break;
                            fwrite(buf, 1, chunk, f);
                            total += chunk;
                        }
                        free(buf);
                        if (total > (320u << 20)) break;
                    }
                    fclose(f);
                    fprintf(stderr, "hengevm: dumped %zu bytes of RAM at %.0fs\n", total,
                            (double) (mach_absolute_time() - t0) / 24000000.0);
                }
            }
        }
        fprintf(stderr, "hengevm: gic ppi27 en %u grp %u prio %02x pmr %02x igrpen1 %llu "
                            "ctlr %x line %d\n", g->enabled[27], g->group[27], g->priority[27],
                    (unsigned) (g->pmr & 0xff), (unsigned long long) g->igrpen1,
                    g_board.gic->ctlr, (int) g_machine.percpu[0].irq_line);
        }
    }
    return NULL;
}

static void stop_cpus(void) {
    if (!g_running) return;
    for (int i = 0; i < HV_MAX_CPUS; i++)
        __atomic_store_n(&g_cpus[i].exit_req, 1, __ATOMIC_RELAXED);
    g_machine.stop = 1;
}

/* Ends the current run, if any: the CPUs return, the machine can be built again. */
void hv_android_install_stop(void) {
    if (!g_running) return;
    stop_cpus();
    /* The CPU threads return on their own; they are detached and only read this machine, so a
       short wait is enough to know they are out of the way before the next memset below. */
    for (int i = 0; i < 100 && g_running; i++) usleep(2000);
    g_running = 0;   /* the next start memsets the machine, which clears its stop flag */
}

/* The last progress idevicerestore reported: step (0-…, or HV_INSTALL_DONE/FAILED) and 0..1. */
int hv_android_install_status(double *progress) {
    if (progress) *progress = g_install_progress;
    return g_install_step;
}

/* Scan the guest's RAM for process names, on demand. Exported because the host installer's main
   loop is the one place that is guaranteed to run after the restore has failed: the kicker
   thread it used to live in does not get to print there (its stderr line never appeared in the
   run that had it), and a probe that never runs is worse than none. */
extern uint64_t hv_seg_kext_blocks, hv_seg_kernel_blocks, hv_seg_other_blocks;

/* The kernel's own log, out of msgbuf in RAM. The serial console was never initialised, so every
   IOLog/printf the kernel made is only in this buffer -- and the answer to "did it parse the
   kext plist" is written there by name ("KernelConfigTables syntax error", "has no kext info
   dictionaries", "can't find prelinked kext executable size", ...). */
static void scan_kernel_log(void) {
    static const char *const keys[] = {
        "KernelConfigTables", "kext info dictionaries", "prelinked kext",
        "KextLog", "OSKext", "IOKit", "koxt", "KernelBootstrap", "unsupported",
        /* Personalities only exist as heap objects once they entered the IOCatalogue; the
           same words also live in __PRELINK_INFO at 0x76ecc000 (PA kimage_pa+0x33b4000),
           which the scan skips, so a hit below 0x90000000 is a real copy. */
        "IOProviderClass", "IONameMatch", "personalities sent", "sent to the IOCatalogue",
        "matching started", "IOService matching",
        /* OSUnserialize copies every plist key into the heap, so these prove the prelink
           plist was actually parsed (not merely present in __PRELINK_INFO). */
        "CFBundleIdentifier", "IOKitPersonalities", "IOProbeScore", "pcie,vmapple1",
        /* The DT root's compatible strings. IORegistryEntry::init(old,plane) copies the root's
           property table into gRootNub, so these must appear as heap copies; and
           AppleVirtualPlatformARMPE's IONameMatch is exactly one of them. */
        "AppleVirtualPlatformARM", "VirtualMac2,1", "VMA2MACOSAP", "device-tree",
        /* Set by IOService only when a personality actually matched a nub. If none of these
           exist on the heap, matching never produced a hit (candidates: traversal found no
           nub, or the compare failed), and no kext could ever be probed. */
        "IOMatchedPersonality", "IOMatchedServiceCount", "IOMatchedAtBoot",
        "IOProbeScore", "IOResourceMatch", "omitting its personalities"
    };
    /* Start above the kernel image (loaded at 0x73b18000, ~19 MB plus __PRELINK_INFO at
       0x76ecc000): everything the image itself contains would otherwise match here. */
    for (uint64_t pa = 0x80000000ull; pa < 0x90000000ull; pa += 0x1000) {
        static uint8_t pg[4096];
        if (hv_phys_read(&g_machine, pa, pg, sizeof pg) != 0) continue;
        for (int i = 0; i + 8 < (int) sizeof pg; i++) {
            for (unsigned k = 0; k < sizeof keys / sizeof keys[0]; k++) {
                size_t l = strlen(keys[k]);
                if (memcmp(pg + i, keys[k], l) != 0) continue;
                if (i > 0 && pg[i - 1] >= 32 && pg[i - 1] < 127) continue;  /* inside a word */
                /* printable run around it, up to 160 chars */
                char buf[200];
                int n = 0;
                int start = i > 60 ? i - 60 : 0;
                for (int j = start; j < (int) sizeof pg && n < (int) sizeof buf - 1; j++) {
                    uint8_t c = pg[j];
                    if (c == 0) break;
                    buf[n++] = (c >= 32 && c < 127) ? (char) c : ' ';
                }
                buf[n] = 0;
                fprintf(stderr, "hengevm: klog pa %llx: %s\n", (unsigned long long) (pa + (uint64_t) start), buf);
                break;
            }
        }
    }
}

void hv_trace_dump(void);

int hv_trace_last_below(uint64_t limit, uint64_t *out, int n);

/* Dump the page(s) around the last userspace block the guest executed.  After the guest gives up,
   the trace's final EL0 address is where dyld was when it last handed control to the kernel --
   reading those bytes back through the guest's own page tables identifies the library call or
   syscall without needing the shared cache file or a debugger. */
static void dump_last_el0(void) {
    extern uint64_t hv_last_el0_va;
    uint64_t last = hv_last_el0_va;
    if (!last) { fprintf(stderr, "hengevm: no userspace block recorded\n"); return; }
    {
        /* Exceptions taken from userland, on their own ring.  The general one is filled by timer
           interrupts from the idle loop within milliseconds, so a userland fault there would be
           long overwritten by the time the guest stops. */
        extern uint64_t hv_uexc_n, hv_uexc_elr[], hv_uexc_far[], hv_uexc_pc[];
        extern uint32_t hv_uexc_esr[], hv_uexc_type[];
        uint64_t n = hv_uexc_n < 64 ? hv_uexc_n : 64, b = hv_uexc_n - n;
        FILE *uf = fopen("/tmp/uexc_tail.txt", "w");
        if (uf) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t k = (b + i) & 63u;
                fprintf(uf, "%llu type=%u esr=%#x pc=%#llx elr=%#llx far=%#llx\n",
                        (unsigned long long) (b + i), hv_uexc_type[k], (unsigned) hv_uexc_esr[k],
                        (unsigned long long) hv_uexc_pc[k],
                        (unsigned long long) hv_uexc_elr[k],
                        (unsigned long long) hv_uexc_far[k]);
            }
            fclose(uf);
            fprintf(stderr, "hengevm: userland exceptions total %llu -> /tmp/uexc_tail.txt\n",
                    (unsigned long long) hv_uexc_n);
        }
    }
    {
        /* The last exceptions the guest took.  If userland stopped without any exception right
           before it, the process was blocked or killed; if there is one, its ESR/FAR names the
           fault, and that is the failure point. */
        extern HvExcRec hv_exc_ring[];
        extern uint32_t hv_exc_at;
        FILE *ef = fopen("/tmp/exc_tail.txt", "w");
        if (ef) {
            uint32_t n = hv_exc_at < 128 ? hv_exc_at : 128;
            for (uint32_t i = 0; i < n; i++) {
                const HvExcRec *r = &hv_exc_ring[(hv_exc_at - n + i) & (HV_EXC_RING - 1)];
                fprintf(ef, "%u el%u t%u esr=%#x elr=%#llx far=%#llx sp=%#llx\n", i, r->el,
                        r->type, (unsigned) r->esr, (unsigned long long) r->elr,
                        (unsigned long long) r->far, (unsigned long long) r->sp);
            }
            fclose(ef);
            fprintf(stderr, "hengevm: exceptions total %u, last 128 -> /tmp/exc_tail.txt\n",
                    hv_exc_at);
        }
    }
    {
        extern int hv_pc_hist_export(int, uint64_t *, int);
        static uint64_t kt[512];
        int n = hv_pc_hist_export(0, kt, 512);
        FILE *kf = fopen("/tmp/kernel_tail.txt", "w");
        if (kf) {
            for (int i = 0; i < n; i++) fprintf(kf, "%#llx\n", (unsigned long long) kt[i]);
            fclose(kf);
            fprintf(stderr, "hengevm: kernel tail %d blocks -> /tmp/kernel_tail.txt\n", n);
        }
    }
    {
        extern uint64_t hv_svc_n, hv_svc_pc[], hv_svc_x16[], hv_svc_x0[], hv_svc_x1[], hv_svc_ret[];
        extern char hv_svc_path[][96];
        extern int hv_svc_pending[];
        extern uint32_t hv_svc_imm[], hv_svc_el[];
        uint64_t n = hv_svc_n < 4096 ? hv_svc_n : 4096, base = hv_svc_n - n;
        FILE *sf = fopen("/tmp/svc_trace.txt", "w");
        if (sf) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t slot = (base + i) & 4095u;
                if (hv_svc_path[slot][0])
                    fputs(hv_svc_path[slot], sf), fputc('\n', sf);
                fprintf(sf, "%llu el%u svc#%#x pc=%#llx x16=%llu(%#llx) args=%#llx,%#llx ret=%#llx\n",
                        (unsigned long long) (base + i), hv_svc_el[slot], hv_svc_imm[slot],
                        (unsigned long long) hv_svc_pc[slot],
                        (unsigned long long) hv_svc_x16[slot],
                        (unsigned long long) hv_svc_x16[slot],
                        (unsigned long long) hv_svc_x0[slot],
                        (unsigned long long) hv_svc_x1[slot],
                        (unsigned long long) hv_svc_ret[slot]);
            }
            fclose(sf);
            fprintf(stderr, "hengevm: syscalls total %llu, pending cpu0=%d cpu1=%d -> /tmp/svc_trace.txt\n",
                    (unsigned long long) hv_svc_n, hv_svc_pending[0], hv_svc_pending[1]);
        }
    }
    {
        extern uint64_t hv_el0_trace_n, hv_el0_trace_first_va;
        extern uint64_t hv_el0_trace_va[], hv_el0_trace_x16[];
        fprintf(stderr, "hengevm: EL0 blocks total %llu, first va %#llx\n",
                (unsigned long long) hv_el0_trace_n,
                (unsigned long long) hv_el0_trace_first_va);
        uint64_t n = hv_el0_trace_n < 16384 ? hv_el0_trace_n : 16384;
        uint64_t base = hv_el0_trace_n - n;
        FILE *tf = fopen("/tmp/el0_trace.txt", "w");
        if (tf) {
            for (uint64_t i = 0; i < n; i++) {
                uint64_t slot = (base + i) & 16383u;
                fprintf(tf, "%llu %#llx %#llx\n",
                        (unsigned long long) (base + i),
                        (unsigned long long) hv_el0_trace_va[slot],
                        (unsigned long long) hv_el0_trace_x16[slot]);
            }
            fclose(tf);
            fprintf(stderr, "hengevm: wrote %llu EL0 blocks -> /tmp/el0_trace.txt\n",
                    (unsigned long long) n);
        }
    }
    /* Which processes exist right now.  "userland stopped" has two very different meanings: the
       process blocked (it is still in the list, waiting on something) or it exited or was killed
       (it is gone, and nothing else is left to run).  The page dump cannot tell them apart, and
       this runs before the fast paths below return. */
    scan_proc_names();
    extern uint64_t hv_last_el0_ttbr0, hv_last_el0_page_va, hv_last_el0_page_pa;
    extern uint64_t hv_last_el0_x0, hv_last_el0_x22, hv_last_el0_x23, hv_last_el0_lr, hv_last_el0_sp;
    fprintf(stderr, "hengevm: last EL0 block %#llx (ttbr0 %#llx, page %#llx -> pa %#llx)\n",
            (unsigned long long) last, (unsigned long long) hv_last_el0_ttbr0,
            (unsigned long long) hv_last_el0_page_va, (unsigned long long) hv_last_el0_page_pa);
    fprintf(stderr, "hengevm:   x0=%#llx x22=%#llx x23=%#llx lr=%#llx sp=%#llx\n",
            (unsigned long long) hv_last_el0_x0, (unsigned long long) hv_last_el0_x22,
            (unsigned long long) hv_last_el0_x23, (unsigned long long) hv_last_el0_lr,
            (unsigned long long) hv_last_el0_sp);
    {
        extern uint64_t hv_last_el0_x16, hv_last_el0_x1, hv_last_el0_x2, hv_last_el0_x3,
            hv_last_el0_x8;
        fprintf(stderr, "hengevm:   SYSCALL x16=%llu (%#llx) x0=%#llx x1=%#llx x2=%#llx x3=%#llx x8=%#llx\n",
                (unsigned long long) hv_last_el0_x16, (unsigned long long) hv_last_el0_x16,
                (unsigned long long) hv_last_el0_x0, (unsigned long long) hv_last_el0_x1,
                (unsigned long long) hv_last_el0_x2, (unsigned long long) hv_last_el0_x3,
                (unsigned long long) hv_last_el0_x8);
    }
    /* Preferred path: the physical page was resolved while the mapping still existed.  Walking
       the tables now fails as soon as the owning process exits, which is exactly what happens
       when launchd's startup fails. */
    if (hv_last_el0_page_pa) {
        static uint8_t pbuf[0x4000];
        if (hv_phys_read(&g_machine, hv_last_el0_page_pa, pbuf, sizeof pbuf) == 0) {
            char ppath[128];
            snprintf(ppath, sizeof ppath, "/tmp/el0_page_%llx.bin",
                     (unsigned long long) hv_last_el0_page_va);
            FILE *pf = fopen(ppath, "wb");
            if (pf) {
                fwrite(pbuf, 1, sizeof pbuf, pf);
                fclose(pf);
                fprintf(stderr, "hengevm: dumped 16KB (from pa) at %#llx -> %s\n",
                        (unsigned long long) hv_last_el0_page_va, ppath);
            }
        } else {
            fprintf(stderr, "hengevm: physical read at pa %#llx failed\n",
                    (unsigned long long) hv_last_el0_page_pa);
        }
        return;
    }
    uint64_t base = last & ~0x3fffull;
    /* Walk using the address space that was live when that block ran: the CPU is on the idle
       thread now, whose TTBR0 has no launchd mappings at all. */
    HvCpu *probe = calloc(1, sizeof *probe);
    if (!probe) return;
    memcpy(probe, &g_cpus[0], sizeof *probe);
    probe->ttbr0_el1 = hv_last_el0_ttbr0;
    for (int pg = -1; pg <= 1; pg++) {
        uint64_t va = base + (uint64_t)(pg * 0x4000);
        static uint8_t buf[0x4000];
        int bad = 0;
        for (unsigned off = 0; off < sizeof buf; off += 0x1000) {
            HvWalk w;
            hv_mmu_walk(probe, va + off, HV_ACC_READ, 0, &w);
            if (w.fsc || hv_phys_read(&g_machine, w.pa, buf + off, 0x1000) != 0) { bad = 1; break; }
        }
        if (bad) { fprintf(stderr, "hengevm: page %#llx not mapped\n", (unsigned long long) va); continue; }
        char path[128];
        snprintf(path, sizeof path, "/tmp/el0_%llx.bin", (unsigned long long) va);
        FILE *f = fopen(path, "wb");
        if (!f) continue;
        fwrite(buf, 1, sizeof buf, f);
        fclose(f);
        fprintf(stderr, "hengevm: dumped 16KB at %#llx -> %s\n", (unsigned long long) va, path);
    }
    free(probe);
}

/* Debug-only: what every thread of every process is waiting on.
 *
 * Port of tools/dump-wait.py that needs no gdb: the engine reads its own guest RAM, so this
 * works where attaching a debugger does not (a PIE under proot, where gdb resolves globals to
 * their unrelocated addresses and then stalls loading 75 MB of symbols).
 *
 * The question it answers is the one the idle stats raise: blocks 0/s with wfi at 95% means no
 * runnable thread, and a thread's wait_event (0 = it is waiting on a turnstile instead) says
 * whether something is going to post an event or whether the wakeup was lost.
 *
 * Offsets are XNU's for xnu-11417.140.69, same as tools/henge-gdb/tsbase.py:
 *   proc+0x18 -> proc_ro, proc_ro+8 -> task, task+0x50 -> threads list head,
 *   thread+0x00 -> next, thread+0x10 -> wait_event, +0x28 -> turnstile, +0x1c8 -> state.
 * HV_THREADS=1 enables it; it is called from hv_android_scan_procs() and from SIGUSR1. */
static uint64_t th_walk(uint64_t va) {
    HvWalk w;
    if (!va) return 0;
    hv_mmu_walk(&g_cpus[0], va, HV_ACC_READ, 1, &w);
    return w.fsc ? 0 : w.pa;
}

static uint64_t th_k64(uint64_t va) {
    uint64_t pa = th_walk(va), v = 0;
    if (!pa || hv_phys_read(&g_machine, pa, &v, 8) != 0) return 0;
    return v;
}

static uint64_t th_pa64(uint64_t pa, int *ok) {
    uint64_t v = 0;
    if (hv_phys_read(&g_machine, pa, &v, 8) != 0) { if (ok) *ok = 0; return 0; }
    if (ok) *ok = 1;
    return v;
}

static int th_kaddr(uint64_t v) { return (v >> 56) == 0xff || (v >> 56) == 0xfe; }

/* proc PA from a p_comm PA: walk backwards until proc+off holds list nodes whose neighbours
   point back at it, which is what distinguishes the real struct proc from a copy of the name. */
static uint64_t th_find_proc(uint64_t pc, uint64_t *node_va) {
    /* p_comm sits well inside struct proc; a match a few bytes from a list node is random data
       that happens to look like one (and made every short string a "process"). */
    for (uint64_t k = 0x100 + ((pc % 8) ? pc % 8 : 0); k < 0x1000; k += 8) {
        uint64_t s = pc - k;
        int ok1 = 0, ok2 = 0;
        uint64_t nxt = th_pa64(s, &ok1), prev = th_pa64(s + 8, &ok2);
        if (!ok1 || !ok2 || !th_kaddr(nxt) || !th_kaddr(prev)) continue;
        uint64_t pp = th_walk(prev);
        if (!pp) continue;
        uint64_t sv = 0;
        if (hv_phys_read(&g_machine, pp, &sv, 8) != 0) continue;
        if (th_kaddr(sv) && th_walk(sv) == s) { *node_va = sv; return s; }
    }
    return 0;
}

/* The turnstile a blocked thread sits on. ts_proprietor is the owner it waits for: if that is a
   live thread we have a chain (who waits for whom), and if it is not, the wakeup was lost. The
   exact offset depends on the build, so print every kernel pointer in the tail of the structure
   and let the caller compare it against the thread addresses above. */
static uint64_t ts_seen[32];
static int ts_n;
/* Thread address -> process name, so a turnstile's owner can be named. */
typedef struct { uint64_t th; char comm[20]; } HvThMap;
static HvThMap g_tm[256];
static int g_tm_n;

static const char *th_comm_of(uint64_t t) {
    for (int i = 0; i < g_tm_n; i++) if (g_tm[i].th == t) return g_tm[i].comm;
    for (int c = 0; c < g_board.ncpus; c++) if (g_cpus[c].tpidr_el1 == t) return "idle";
    return NULL;
}

static int th_printable(uint8_t c) { return c >= 32 && c < 127; }

/* A p_comm is a short printable name at a fixed offset inside struct proc, so scanning RAM finds
   processes without the symbol table. Candidates are restricted to the character set real process
   names use, and struct proc is confirmed through the list node around the name; a string constant
   or a copy of a name does not have that. */
typedef struct { uint64_t proc_pa; char comm[20]; } HvProcEnt;
static HvProcEnt g_pe[64];
static int g_pe_n;

static void th_scan_all(void) {
    g_pe_n = 0;
    uint64_t img_lo = g_machine.kimage_pa ? g_machine.kimage_pa : 0x73b18000ull;
    for (uint64_t pa = 0x70000000ull; pa < 0x90000000ull && g_pe_n < 64; pa += 0x1000) {
        if (pa + 0x1000 > img_lo && pa < img_lo + 0x4000000ull) continue;
        uint8_t pg[4096];
        if (hv_phys_read(&g_machine, pa, pg, sizeof pg) != 0) continue;
        for (int i = 0; i + 17 <= 4096 && g_pe_n < 64; ) {
            uint8_t c = pg[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
                  c == '.')) { i++; continue; }
            int l = 0;
            while (l < 16) {
                uint8_t d = pg[i + l];
                if (!((d >= 'a' && d <= 'z') || (d >= '0' && d <= '9') || d == '_' || d == '-' ||
                      d == '.')) break;
                l++;
            }
            if (l < 2 || pg[i + l] != 0) { i++; continue; }
            uint64_t node = 0;
            uint64_t pr = th_find_proc(pa + (uint64_t) i, &node);
            if (pr) {
                if (getenv("HV_PCOMM_OFF") && l >= 8 && g_pe_n < 12)
                    fprintf(stderr, "hengevm: pcomm off 0x%llx (name %.*s)\n",
                            (unsigned long long) ((pa + (uint64_t) i) - pr), l, pg + i);
                int dup = 0;
                for (int k = 0; k < g_pe_n; k++) if (g_pe[k].proc_pa == pr) dup = 1;
                if (!dup) {
                    g_pe[g_pe_n].proc_pa = pr;
                    memcpy(g_pe[g_pe_n].comm, pg + i, l);
                    g_pe[g_pe_n].comm[l] = 0;
                    g_pe_n++;
                }
            }
            i += l;
        }
    }
}

/* (task VA, threads head VA) of a process, or 0. */
static uint64_t th_proc_task(uint64_t proc_pa, uint64_t *head) {
    uint64_t ro_pa = th_walk(th_pa64(proc_pa + 0x18, NULL));
    if (!ro_pa) return 0;
    uint64_t task = th_pa64(ro_pa + 8, NULL);
    uint64_t tpa = th_walk(task);
    if (!tpa) return 0;
    if (head) *head = th_pa64(tpa + 0x50, NULL);
    return task;
}

static void th_build_map(void) {
    g_tm_n = 0;
    for (int k = 0; k < g_pe_n; k++) {
        uint64_t head = 0;
        if (!th_proc_task(g_pe[k].proc_pa, &head) || !head) continue;
        uint64_t cur = head;
        for (int n = 0; n < 32; n++) {
            if (!cur || !th_kaddr(cur)) break;
            if (g_tm_n < 256) {
                g_tm[g_tm_n].th = cur;
                snprintf(g_tm[g_tm_n].comm, sizeof g_tm[g_tm_n].comm, "%s", g_pe[k].comm);
                g_tm_n++;
            }
            uint64_t tp = th_walk(cur);
            if (!tp) break;
            uint64_t next = th_pa64(tp, NULL);
            if (next == head) break;
            cur = next;
        }
    }
}

/* Who references this turnstile. A turnstile belongs to a lock object (lck_mtx's waiter
   pointer, a waitq, ...); finding the reference names the lock, and the fields around it say
   whether the lock still believes it has an owner -- the difference between "the lock object
   was never unlocked" and "it was unlocked and the wakeup was dropped". */
static void th_find_refs(uint64_t target) {
    enum { CHUNK = 1 << 20 };
    static uint8_t buf[CHUNK];
    int found = 0;
    for (int r = 0; r < g_machine.nram && found < 6; r++) {
        uint64_t pa = g_machine.ram[r].pa, sz = g_machine.ram[r].size;
        for (uint64_t off = 0; off + CHUNK <= sz && found < 6; off += CHUNK) {
            if (hv_phys_read(&g_machine, pa + off, buf, CHUNK) != 0) continue;
            for (uint32_t i = 0; i + 8 <= CHUNK; i += 8) {
                uint64_t v;
                memcpy(&v, buf + i, 8);
                if (v != target) continue;
                fprintf(stderr, "hengevm:       ref pa %llx ctx:",
                        (unsigned long long) (pa + off + i));
                for (int k = -4; k <= 6; k++) {
                    uint32_t j = (uint32_t) ((int32_t) i + k * 8);
                    if ((int32_t) j < 0 || j + 8 > CHUNK) continue;
                    uint64_t w;
                    memcpy(&w, buf + j, 8);
                    fprintf(stderr, " %s%llx", k == 0 ? "[" : "", (unsigned long long) w);
                    if (k == 0) fprintf(stderr, "]");
                }
                fprintf(stderr, "\n");
                if (++found >= 6) break;
            }
        }
    }
    if (!found) fprintf(stderr, "hengevm:       no reference to turnstile %llx in RAM\n",
                        (unsigned long long) target);
}

static void th_mtx_for_ts(uint64_t ts, uint64_t tpa);

static void th_dump_turnstile(uint64_t ts, int depth) {
    if (!ts) return;
    uint64_t tpa = th_walk(ts);
    if (!tpa) { fprintf(stderr, "hengevm:       turnstile %llx (not walkable)\n",
                        (unsigned long long) ts); return; }
    fprintf(stderr, "hengevm:       turnstile %llx pa %llx:", (unsigned long long) ts,
            (unsigned long long) tpa);
    const char *owner = NULL;
    for (uint64_t off = 0x300; off < 0x3c0; off += 8) {
        uint64_t v = th_pa64(tpa + off, NULL);
        if (!th_kaddr(v)) continue;
        fprintf(stderr, " +0x%llx=%llx", (unsigned long long) off, (unsigned long long) v);
        const char *c = th_comm_of(v);
        if (c) { fprintf(stderr, "(%s)", c); if (!owner) owner = c; }
    }
    fprintf(stderr, "\n");
    /* Layout from XNU: waitq(40) free_turnstiles(16) inheritor_queue(8) inheritor_links(8)
       htable_link(8) then ts_proprietor, i.e. around +0x50 on arm64 release builds. Print the
       head unconditionally: a zero here is the whole answer (a lock with no owner). */
    fprintf(stderr, "hengevm:       ts head:");
    for (uint64_t off = 0x28; off < 0x90; off += 8) {
        uint64_t v = th_pa64(tpa + off, NULL);
        fprintf(stderr, " +0x%llx=%llx%s", (unsigned long long) off, (unsigned long long) v,
                th_kaddr(v) ? "*" : "");
        if (th_kaddr(v)) {
            const char *c = th_comm_of(v);
            if (c) fprintf(stderr, "(%s)", c);
        }
    }
    fprintf(stderr, "\n");
    /* Arm on any turnstile, not just an unowned one: a normal unlock that does wake its waiter
       shows the pattern the broken one is missing, and the pc/x30 of each write names the code. */
    if (getenv("HV_TS_WATCH") && !g_machine.watch_pa) {
        g_machine.watch_pa = tpa + 0x50;
        fprintf(stderr, "hengevm: watch armed on turnstile %llx +0x50 (pa %llx)\n",
                (unsigned long long) ts, (unsigned long long) (tpa + 0x50));
    }
    {
        /* ts_proprietor: 0 means the lock has no owner, so whoever is waiting on it cannot be
           woken by the owner -- a lost wakeup. Non-zero: follow the owner one level, because an
           owner that is itself blocked is the next link of the chain. */
        uint64_t prop = th_pa64(tpa + 0x50, NULL);
        if (!prop || !th_kaddr(prop)) {
            fprintf(stderr, "hengevm:       proprietor NONE (unowned lock)\n");
            if (getenv("HV_TS_REF")) th_find_refs(ts);
            if (getenv("HV_TS_MTX")) th_mtx_for_ts(ts, tpa);
            /* Arm the write watch on this turnstile's proprietor: the next unlock that clears it
               prints the writing pc and its caller, which is what says whether the wakeup path ran
               from the same place. */

        } else {
            uint64_t pp = th_walk(prop);
            if (!pp) {
                fprintf(stderr, "hengevm:       proprietor %llx not walkable (freed thread?)\n",
                        (unsigned long long) prop);
            } else {
                uint64_t pst = th_pa64(pp + 0x1c8, NULL) & 0xffffffffull;
                uint64_t pts = th_pa64(pp + 0x28, NULL);
                fprintf(stderr, "hengevm:       proprietor %llx state 0x%llx waits_on %llx%s\n",
                        (unsigned long long) prop, (unsigned long long) pst,
                        (unsigned long long) pts, th_comm_of(prop) ? th_comm_of(prop) : "");
                if (depth > 0 && pts && th_kaddr(pts) && pts != ts)
                    th_dump_turnstile(pts, depth - 1);
            }
        }
    }
    if (getenv("HV_TS_FULL")) {
        for (uint64_t off = 0x300; off < 0x400; off += 8) {
            uint64_t v = th_pa64(tpa + off, NULL);
            fprintf(stderr, "hengevm:         ts+0x%llx = %llx%s\n", (unsigned long long) off,
                    (unsigned long long) v, th_kaddr(v) ? "  (kptr)" : "");
        }
        for (uint64_t off = 0; off < 0x40; off += 8) {
            uint64_t v = th_pa64(tpa + off, NULL);
            fprintf(stderr, "hengevm:         ts+0x%02llx = %llx%s\n", (unsigned long long) off,
                    (unsigned long long) v, th_kaddr(v) ? "  (kptr)" : "");
        }
    }
    /* One level up: if the owner is itself blocked on a turnstile, that is the next link. */
    if (depth <= 0) return;
    for (uint64_t off = 0x300; off < 0x3c0; off += 8) {
        uint64_t v = th_pa64(tpa + off, NULL);
        if (!th_kaddr(v) || !th_comm_of(v)) continue;
        uint64_t vp = th_walk(v);
        if (!vp) continue;
        uint64_t st = th_pa64(vp + 0x1c8, NULL) & 0xffffffffull;
        uint64_t ts2 = th_pa64(vp + 0x28, NULL);
        if (!ts2) continue;
        fprintf(stderr, "hengevm:       owner %llx (%s) state 0x%llx waits on:\n",
                (unsigned long long) v, th_comm_of(v), (unsigned long long) st);
        th_dump_turnstile(ts2, depth - 1);
    }
}

/* Where a blocked thread is, without knowing where kernel_stack sits inside struct thread: a
   blocked thread still has its call chain on its 16 KiB stack, and a stack pointer is page
   aligned, so any page-aligned kernel pointer inside struct thread is a candidate stack top.
   Kernel text return addresses found on it name the functions it is waiting in.
   Algorithm from tools/dump-stacks.py. */
#define KSSIZE 0x4000u
static void th_dump_stack(uint64_t tva) {
    uint64_t tp = th_walk(tva);
    if (!tp) return;
    for (uint64_t off = 0; off < 0x1800; off += 8) {
        uint64_t v = th_pa64(tp + off, NULL);
        if (!th_kaddr(v) || (v & 0xfff)) continue;
        for (int which = 0; which < 2; which++) {
            uint64_t base = which ? v : v - KSSIZE;
            uint64_t bp = th_walk(base);
            if (!bp) continue;
            static uint8_t buf[KSSIZE];
            if (hv_phys_read(&g_machine, bp, buf, KSSIZE) != 0) continue;
            int hits = 0;
            for (uint32_t j = 0; j < KSSIZE; j += 8) {
                uint64_t w;
                memcpy(&w, buf + j, 8);
                if (w < 0xfffffe0010000000ull || w >= 0xfffffe0040000000ull) continue;
                if (!hits)
                    fprintf(stderr, "hengevm:   stack th %llx base %llx text pcs:",
                            (unsigned long long) tva, (unsigned long long) base);
                if (hits++ < 14) fprintf(stderr, " %llx", (unsigned long long) w);
            }
            if (hits) { fprintf(stderr, "\n"); return; }
        }
    }
}

static void th_dump_one(int idx) {
    uint64_t head = 0;
    uint64_t task = th_proc_task(g_pe[idx].proc_pa, &head);
    fprintf(stderr, "hengevm: proc %-16s pa %llx task %llx head %llx\n", g_pe[idx].comm,
            (unsigned long long) g_pe[idx].proc_pa, (unsigned long long) task,
            (unsigned long long) head);
    if (!task || !head) return;
    uint64_t cur = head;
    for (int n = 0; n < 32; n++) {
        if (!cur || !th_kaddr(cur)) break;
        uint64_t tp = th_walk(cur);
        if (!tp) break;
        uint64_t next = th_pa64(tp, NULL);
        uint64_t we = th_pa64(tp + 0x10, NULL);
        uint64_t ts = th_pa64(tp + 0x28, NULL);
        uint64_t st = th_pa64(tp + 0x1c8, NULL) & 0xffffffffull;
        const char *tag = "";
        for (int c = 0; c < g_board.ncpus; c++)
            if (g_cpus[c].tpidr_el1 == cur) tag = " (running here)";
        uint64_t wq = th_pa64(tp + 0x20, NULL);
        if (getenv("HV_TS_LAYOUT")) {
            for (int k = 0; k < 3; k++) {
                static const char *nm[3] = {"0x20", "0x28", "0x30"};
                uint64_t p = th_pa64(tp + 0x20 + (uint64_t) k * 8, NULL);
                uint64_t pp = th_walk(p);
                uint32_t f = 0;
                if (pp) hv_phys_read(&g_machine, pp, &f, 4);
                fprintf(stderr, "hengevm:     thread+%s = %llx%s flags %x type %u tsid %x\n",
                        nm[k], (unsigned long long) p, pp ? "" : " (not walkable)", f, f & 7u,
                        (f >> 12) & 0xfffffu);
            }
        }
        fprintf(stderr, "hengevm:   th %llx state 0x%llx wait_event %llx waitq %llx%s\n",
                (unsigned long long) cur, (unsigned long long) st,
                (unsigned long long) we, (unsigned long long) wq, tag);
        /* The waitq it is parked on: any thread pointer inside names the others waiting there.
           Two processes on one queue means one post would move both, which is the difference
           between "waiting for work" and "waiting for a poster that is itself waiting". */
        if (wq && th_kaddr(wq)) {
            uint64_t wqp = th_walk(wq);
            if (wqp) {
                fprintf(stderr, "hengevm:     waitq %llx pa %llx:", (unsigned long long) wq,
                        (unsigned long long) wqp);
                for (uint64_t off = 0; off < 0x120; off += 8) {
                    uint64_t v = th_pa64(wqp + off, NULL);
                    if (!th_kaddr(v)) continue;
                    const char *c = th_comm_of(v);
                    fprintf(stderr, " +0x%llx=%llx", (unsigned long long) off,
                            (unsigned long long) v);
                    if (c) fprintf(stderr, "(%s)", c);
                }
                fprintf(stderr, "\n");
            }
        }
        /* The saved kernel PC: struct machine_thread holds arm_saved_state_t *kpcb, and inside
           that, ash(8) + ss_64.pc(0x100). Scanning thread for a kernel pointer whose +0x108 is
           kernel text finds it without knowing machine_thread's offset (it varies with config).
           This is the reliable answer to "which function is this thread blocked in" -- a stack
           scan shows stale data too. */
        if (getenv("HV_TH_PCB")) {
            for (uint64_t off = 0; off < 0x1800; off += 8) {
                uint64_t ptr = th_pa64(tp + off, NULL);
                if (!th_kaddr(ptr)) continue;
                uint64_t pp = th_walk(ptr);
                if (!pp) continue;
                uint64_t pc = th_pa64(pp + 0x108, NULL);
                if (pc < 0xfffffe001c890000ull || pc >= 0xfffffe001eec92f4ull) continue;
                /* kpcb lives on the thread's kernel stack, so sp must be a stack address; a
                   pointer whose "pc" merely falls in the text range is usually data. */
                uint64_t sp0 = th_pa64(pp + 0x100, NULL);
                if (sp0 < 0xfffffe2000000000ull || sp0 >= 0xfffffe4000000000ull) continue;
                fprintf(stderr, "hengevm:   kpcb thread+0x%llx: pc %llx lr %llx sp %llx"
                                " (pc+0x%llx in kernel text)\n",
                        (unsigned long long) off, (unsigned long long) pc,
                        (unsigned long long) th_pa64(pp + 0xf8, NULL),
                        (unsigned long long) th_pa64(pp + 0x100, NULL),
                        (unsigned long long) (pc - 0xfffffe001bb18000ull));
            }
        }
        if (getenv("HV_TH_STACK")) th_dump_stack(cur);
        th_dump_turnstile(ts, 2);
        if (next == head) break;
        cur = next;
    }
}

/* The turnstile's own compact id: WAITQ_FLAGS places it at bits 12.. (type:3 fifo:1 preposted:1
   unused:7 ts_compact_id:20, CTSID_BITS=20). lck_mtx stores the same (mangled) id in its 24-bit
   lck_mtx_tsid, so matching the two says whether this turnstile still belongs to a mutex. */
static void th_mtx_for_ts(uint64_t ts, uint64_t tpa) {
    uint32_t flags = 0, state = 0;
    if (hv_phys_read(&g_machine, tpa, &flags, 4) != 0) return;
    uint32_t tsid = (flags >> 12) & 0xfffffu;
    fprintf(stderr, "hengevm:       tsid %x (flags %x, type %u)\n", tsid, flags, flags & 7u);
    if (!tsid) { fprintf(stderr, "hengevm:       tsid 0: not a turnstile?\n"); return; }
    int hits = 0;
    for (uint64_t pa = 0x84000000ull; pa < 0x85000000ull && hits < 4; pa += 0x1000) {
        uint8_t pg[4096];
        if (hv_phys_read(&g_machine, pa, pg, sizeof pg) != 0) continue;
        for (int i = 0; i + 16 <= 4096 && hits < 4; i += 4) {
            uint32_t v0;
            memcpy(&v0, pg + i, 4);
            if ((v0 >> 24) != 0x22) continue;             /* LCK_TYPE_MUTEX */
            if ((v0 & 0xfffffu) != tsid) continue;        /* same compact id */
            memcpy(&state, pg + i + 8, 4);
            hits++;
            fprintf(stderr, "hengevm:       -> mtx pa %llx state %x owner %x needs_wakeup %d\n",
                    (unsigned long long) (pa + (uint64_t) i), state, state & 0x0fffffffu,
                    (int) ((state >> 30) & 1));
        }
    }
    if (!hits)
        fprintf(stderr, "hengevm:       no lck_mtx carries tsid %x (turnstile is not attached to"
                        " a mutex)\n", tsid);
}

/* lck_mtx_t (arm64 uses the non-arch definition, osfmk/kern/lock_mtx.h):
 *   +0x00  uint32_t lck_mtx_tsid:24, lck_mtx_type:8   (LCK_TYPE_MUTEX = 0x22)
 *   +0x04  uint32_t lck_mtx_grp
 *   +0x08  lck_mtx_state: data = owner:28 | ilocked:1 | spin_mode:1 | needs_wakeup:1 | profile:1
 * The turnstile is referenced by the 24-bit tsid, NOT by a pointer, which is why scanning for a
 * pointer to the turnstile found only the waiter itself.
 *
 * needs_wakeup set with owner == 0 is the state that must not persist: unlock has to clear the
 * owner, see needs_wakeup and call turnstile_wakeup. Finding such a lock names the exact object
 * whose waiter was left behind. */
static void th_scan_mtx(void) {
    int total = 0, stale = 0, orphan = 0;
    for (uint64_t pa = 0x84000000ull; pa < 0x85000000ull; pa += 0x1000) {
        uint8_t pg[4096];
        if (hv_phys_read(&g_machine, pa, pg, sizeof pg) != 0) continue;
        for (int i = 0; i + 16 <= 4096; i += 4) {
            uint32_t v0, state;
            memcpy(&v0, pg + i, 4);
            if ((v0 >> 24) != 0x22) continue;         /* lck_mtx_type == LCK_TYPE_MUTEX */
            memcpy(&state, pg + i + 8, 4);
            uint32_t owner = state & 0x0fffffffu;
            int ilocked = (int) ((state >> 28) & 1), spin = (int) ((state >> 29) & 1);
            int needs = (int) ((state >> 30) & 1);
            total++;
            /* tsid != 0 means this mutex still has a turnstile attached (somebody queued), while
               owner == 0 means nobody holds it. That combination can never resolve: the queued
               thread waits for an unlock that already happened. */
            if ((v0 & 0xffffffu) && owner == 0 && !spin) {
                orphan++;
                if (orphan <= 20)
                    fprintf(stderr, "hengevm: mtx pa %llx tsid %x state %x: OWNERLESS with a"
                                    " turnstile attached (needs_wakeup %d)\n",
                            (unsigned long long) (pa + (uint64_t) i), v0 & 0xffffffu, state, needs);
            }
            if (needs && owner == 0 && !spin) {
                stale++;
                if (stale <= 20)
                    fprintf(stderr, "hengevm: mtx pa %llx tsid %x state %x: needs_wakeup, no owner"
                                    " (ilocked %d)\n",
                            (unsigned long long) (pa + (uint64_t) i), v0 & 0xffffffu, state,
                            ilocked);
            }
        }
    }
    fprintf(stderr, "hengevm: mtx scan: %d mutexes, %d ownerless-with-turnstile, %d with"
                    " needs_wakeup && no owner\n", total, orphan, stale);
}

void hv_threads_dump(void) {
    th_scan_all();
    th_build_map();
    fprintf(stderr, "hengevm: threads: %d processes, %d threads mapped\n", g_pe_n, g_tm_n);
    for (int i = 0; i < g_pe_n; i++) th_dump_one(i);
    if (getenv("HV_TS_MTX")) th_scan_mtx();
    for (int c = 0; c < g_board.ncpus; c++)
        fprintf(stderr, "hengevm: cpu%d pc %llx tpidr_el1 %llx el%u\n", c,
                (unsigned long long) g_cpus[c].pc, (unsigned long long) g_cpus[c].tpidr_el1,
                g_cpus[c].el);
}

void hv_android_scan_procs(void) {
    scan_proc_names();
    scan_kernel_log();
    hv_trace_dump();
    { extern void hv_fault_stats_dump(void); hv_fault_stats_dump(); }
    dump_last_el0();
    /* Did the boot ever run a kext? __PRELINK_TEXT holds every device driver on this platform, so
       kext=0 with kernel>0 means the boot never got as far as driving a device -- which is what the
       mmio histogram (no pcie, no uart) already suggested. */
    fprintf(stderr, "hengevm: seg blocks: kext %llu kernel %llu other %llu\n",
            (unsigned long long) hv_seg_kext_blocks,
            (unsigned long long) hv_seg_kernel_blocks,
            (unsigned long long) hv_seg_other_blocks);
    /* Where the guest is at the moment the restore gives up, and how it got there. With this
       boot's slide the kext code lives at 0xfffffe001bb20000..0xfffffe001c210000 and the kernel's
       own code at 0xfffffe001c890000.., so the pc says at once whether the boot ever entered a
       kext at all: the mmio histogram showed no device touched, and "never ran a kext" and "ran
       one and it failed" want different next steps. */
    for (int i = 0; i < g_board.ncpus; i++) {
        HvCpu *c = &g_cpus[i];
        fprintf(stderr, "hengevm: cpu %d pc %llx el%u icount %llu halted %d stop %d\n", i,
                (unsigned long long) c->pc, c->el, (unsigned long long) c->icount, c->halted,
                g_machine.stop ? 1 : 0);
    }
    hv_pc_hist_dump(&g_cpus[0]);
    /* The kernel collection as the guest actually received it, against the same blob decrypted
       from the IPSW: __PRELINK_TEXT (kext code, file offset 0x8000, 1.7 MB) is where a residual
       decompression error would show up first, and it is exactly the region whose blocks never
       run. If these match, "the data is wrong" is ruled out and the problem is in what the kernel
       does with correct data. */
    if (getenv("HV_DUMP_KC") && g_machine.kimage_pa) {
        /* __PRELINK_INFO: the 3.1 MB XML plist listing all 238 kexts and their personalities.
           libsa/bootstrap.cpp turns it into OSKext objects, and that is the step before any kext
           code can run (which it never does). Same test as __PRELINK_TEXT: dump what the guest
           holds and compare it with the IPSW plaintext, so "the data is wrong" and "the parser
           never saw correct data" stay separate hypotheses. */
        {
            enum { IOFF = 0x33b4000, ILEN = 0x31c000 };
            FILE *inf = fopen("/data/local/tmp/henge/kc_prelink_info.bin", "wb");
            if (inf) {
                enum { CH = 1 << 20 };
                uint8_t *buf = malloc(CH);
                uint64_t done = 0;
                int bad = 0;
                while (buf && done < ILEN && !bad) {
                    uint64_t n = ILEN - done < CH ? ILEN - done : CH;
                    if (hv_phys_read(&g_machine, g_machine.kimage_pa + IOFF + done, buf, (uint32_t) n) != 0) { bad = 1; break; }
                    if (fwrite(buf, 1, n, inf) != n) { bad = 1; break; }
                    done += n;
                }
                free(buf);
                fclose(inf);
                fprintf(stderr, "hengevm: kc prelink_info %llu/%llu bytes from pa %llx%s\n",
                        (unsigned long long) done, (unsigned long long) ILEN,
                        (unsigned long long) (g_machine.kimage_pa + IOFF), bad ? " (read error)" : "");
            }
        }
        enum { KOFF = 0x8000, KLEN = 0x6f0000 };
        FILE *kf = fopen("/data/local/tmp/henge/kc_prelink_text.bin", "wb");
        if (kf) {
            enum { CH = 1 << 20 };
            uint8_t *buf = malloc(CH);
            uint64_t done = 0;
            int bad = 0;
            while (buf && done < KLEN && !bad) {
                uint64_t n = KLEN - done < CH ? KLEN - done : CH;
                if (hv_phys_read(&g_machine, g_machine.kimage_pa + KOFF + done, buf, (uint32_t) n) != 0) { bad = 1; break; }
                if (fwrite(buf, 1, n, kf) != n) { bad = 1; break; }
                done += n;
            }
            free(buf);
            fclose(kf);
            fprintf(stderr, "hengevm: kc prelink_text %llu/%llu bytes from pa %llx%s\n",
                    (unsigned long long) done, (unsigned long long) KLEN,
                    (unsigned long long) (g_machine.kimage_pa + KOFF), bad ? " (read error)" : "");
        }
    }
    /* The restore ramdisk itself: the device tree says RAMDisk @ 0x783ac000, size 0xb400000, and
       that is where restored lives. Its first page names the filesystem; the rest is the program
       the boot was running. Dumped from guest RAM, so it does not depend on the userland page
       tables still being installed at this point (the walk above finds nothing after stop). */
    {
        const char *wd = getenv("HV_BLOCK_DUMP");
        if (wd && *wd) {
            /* HV_DUMP_RAMDISK writes the whole thing (180 MB): the ramdisk is an APFS container,
               which cannot be mounted here, but the programs inside it are Mach-O images and can
               be found by their magic. Off by default -- it is 180 MB of I/O per run. */
            if (getenv("HV_DUMP_RAMDISK")) {
                FILE *rf = fopen("/data/local/tmp/henge/ramdisk.img", "wb");
                if (rf) {
                    enum { CH = 1 << 20 };
                    uint8_t *buf = malloc(CH);
                    uint64_t done = 0, total = 0xb400000ull;
                    int bad = 0;
                    while (buf && done < total && !bad) {
                        uint64_t n = total - done < CH ? total - done : CH;
                        if (hv_phys_read(&g_machine, 0x783ac000ull + done, buf, (uint32_t) n) != 0) { bad = 1; break; }
                        if (fwrite(buf, 1, n, rf) != n) { bad = 1; break; }
                        done += n;
                    }
                    free(buf);
                    fclose(rf);
                    fprintf(stderr, "hengevm: ramdisk image %llu/%llu bytes%s\n",
                            (unsigned long long) done, (unsigned long long) total, bad ? " (read error)" : "");
                }
            }
            uint8_t hdr[4096];
            if (hv_phys_read(&g_machine, 0x783ac000ull, hdr, sizeof hdr) == 0) {
                size_t n = strlen(wd);
                char path[600];
                snprintf(path, sizeof path, "%.*s/ramdisk_hdr.bin", (int)(n - 11 > 0 ? n - 11 : 0), wd);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(hdr, 1, sizeof hdr, f); fclose(f); }
                fprintf(stderr, "hengevm: ramdisk hdr %02x %02x %02x %02x at 783ac000 -> %s\n",
                        hdr[0], hdr[1], hdr[2], hdr[3], path);
            }
        }
    }
    /* What the guest's userland was actually executing. Without a console the only account of
       restored is its own code, and the faulting pcs the log already printed say where it was:
       pull those pages out of guest RAM and disassemble them offline. */
    {
        static const uint64_t va[] = {0x10284a9a0ull, 0x1027fcf90ull, 0x102818eb8ull,
                                      0x1028742b8ull, 0x1027fd3b8ull, 0x1027fbe44ull};
        const char *wd = getenv("HV_BLOCK_DUMP");
        if (wd && *wd) {
            for (unsigned k = 0; k < sizeof va / sizeof va[0]; k++) {
                HvWalk w;
                memset(&w, 0, sizeof w);
                hv_mmu_walk(&g_cpus[0], va[k], HV_ACC_READ, 0, &w);
                if (w.fsc) continue;
                uint8_t pg[4096];
                if (hv_phys_read(&g_machine, w.pa & ~0xfffull, pg, sizeof pg) != 0) continue;
                size_t n = strlen(wd);
                char path[600];
                snprintf(path, sizeof path, "%.*s/el0_%llx.bin", (int)(n - 11 > 0 ? n - 11 : 0), wd,
                         (unsigned long long) va[k]);
                FILE *f = fopen(path, "wb");
                if (f) { fwrite(pg, 1, sizeof pg, f); fclose(f); }
                fprintf(stderr, "hengevm: el0 pc %llx -> pa %llx page dumped\n",
                        (unsigned long long) va[k], (unsigned long long) w.pa);
            }
        }
    }
}

int hv_android_install_running(void) { return g_running; }
