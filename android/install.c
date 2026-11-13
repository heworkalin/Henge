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
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <android/log.h>

#include "dispatch.h"
#include "install.h"
#include "machine.h"
#include "vmapple.h"

/* Bytes of the auxiliary storage image: 32 MB of data after a 16 KB header block. */
#define AUX_BYTES 33570816ull

/* The root disk's default size. macOS wants at least ~32 GB; 64 GiB leaves room to work and costs
 * nothing until written, because the file is sparse. */
#define DISK_BYTES_DEFAULT (64ull << 30)

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
static volatile double g_install_progress;
static int g_running;

/* Defined in henge_vm.c: one machine at a time, so starting one means stopping the other. */
void hv_android_vm_teardown(void);
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

static void on_install_progress(int step, double progress, void *user) {
    (void)user;
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

    if (!rom || !disk || !aux || !ipsw) {
        say(&cursor, end, "  files          : MISSING — ROM, disk, auxiliary storage and IPSW are all needed\n");
        return -1;
    }
    if (cache_dir && *cache_dir) setenv("XDG_CACHE_HOME", cache_dir, 1);

    if (hv_disk_open(&g_disk, disk, 0, 0) != 0) {
        say(&cursor, end, "  root disk      : FAILED to open %s (%s)\n", disk, strerror(errno));
        return -1;
    }
    if (hv_disk_open(&g_aux, aux, 16384, 0) != 0) {
        say(&cursor, end, "  auxiliary      : FAILED to open %s (%s)\n", aux, strerror(errno));
        return -1;
    }

    // One machine per process, and the graphics device says so: reims-vgpu refuses to create a
    // second display with the same serial number, which is what an install started while a boot
    // was still up used to hit (a warning, then a display that had been destroyed underneath it).
    // Tearing the previous machine down first is the only ordering that is correct.
    if (hv_android_install_running()) return -1;
    hv_android_vm_teardown();

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

    g_install_step = 0;
    g_install_progress = 0;
    if (hv_install_start(g_board.vusb, ipsw, workdir, ecid, on_install_progress, NULL) != 0) {
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
    return 0;
}

/* The last progress idevicerestore reported: step (0-…, or HV_INSTALL_DONE/FAILED) and 0..1. */
int hv_android_install_status(double *progress) {
    if (progress) *progress = g_install_progress;
    return g_install_step;
}

int hv_android_install_running(void) { return g_running; }
