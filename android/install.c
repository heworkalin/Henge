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
#include "mmu.h"
#include "sysreg.h"
#include "disk.h"
#include "install.h"
#include "machine.h"
#include "vmapple.h"

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
       never end. Stopping the CPUs here is what lets the next attempt build a machine at all. */
    if (step >= HV_INSTALL_DONE) stop_cpus();
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
    /* The kalloc_type view record of com.apple.driver.AppleUSBAudio, whose packed fields nothing
       rebased: whoever writes it is the thing to find, so watch that page. */
    setenv("HV_WATCH_VA", "fffffe001c4a7850", 1);
    /* The same record by physical address: the firmware builds the kernel image with the MMU off,
       so a write there is a write to this PA and never matches the kernel VA above. This one is the
       copy in the region iBEC assembles the kernel in before copying it into place: the value that
       ends up in the image at 0x74507860 is written here first. */
    setenv("HV_WATCH_PA", "837ef860", 1);
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
static void *kicker_thread(void *arg) {
    (void) arg;
    uint64_t n = 0;
    while (g_running) {
        usleep(1000);
        hv_machine_kick_all(&g_machine);
        if (getenv("HENGE_HEARTBEAT") && (++n % 1000) == 0)
            fprintf(stderr, "hengevm: kicked %llu times, ncpus %d\n",
                    (unsigned long long) n, g_machine.ncpus);
    }
    return NULL;
}

static void *monitor_thread(void *arg) {
    (void) arg;
    uint64_t t0 = mach_absolute_time();
    uint64_t prev_pc = 0, changes = 0, samples = 0, same = 0;
    while (g_running && !g_machine.stop) {
        struct timespec ts = { .tv_sec = 2, .tv_nsec = 0 };
        nanosleep(&ts, NULL);
        HvCpu *c = &g_cpus[0];
        uint64_t pc = c->pc;
        samples++;
        if (pc != prev_pc) changes++; else same++;
        prev_pc = pc;
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

int hv_android_install_running(void) { return g_running; }
