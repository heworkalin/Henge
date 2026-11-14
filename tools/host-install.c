/*
 * The install, run on the host instead of in the app.
 *
 * This is the same call the app makes — `hv_android_install_start` from android/install.c, which
 * boots the ROM in DFU mode, lets idevicerestore write the disk over the guest's virtio USB port,
 * and reports the steps — with the paths given on the command line instead of from an app.
 *
 * The point is the loop: an install on the phone costs a rebuild, an install, a tap and fifty
 * seconds of someone holding it. Here it is one command.
 *
 *   tools/host-install work/AVPBooter.vmapple2.bin android/ipsw/24G90.ipsw work
 *
 * `work` gets disk.img (sparse), aux.img and the restore's scratch files. Everything is created
 * if it is missing and left alone if it is there, so a second run continues where the last stopped.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>

int hv_android_install_start(const char *rom, const char *disk, const char *aux, const char *ipsw,
                             const char *workdir, int ram_mb, int ncpus, uint64_t ecid,
                             const char *cache_dir, char *report, size_t cap);
int hv_android_install_status(double *progress);

/* compat_android.c synthesises the commpage the translator reads while emitting counter
   instructions. In the app this happens from the JNI entry; here it has to happen before any guest
   code is translated, or the first counter read takes the process down at 0xfffffc088. */
int hv_android_map_commpage(void);

/* Defined in henge_vm.c in the app, which builds machines for the bring-up probe: nothing here
   uses it, and the install path only calls it to drop a previous machine. */
void hv_android_vm_teardown(void) {}

static int make_disk(const char *path, unsigned long long bytes) {
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return -1;
    struct stat st;
    int ok = 0;
    if (fstat(fd, &st) == 0) ok = (unsigned long long) st.st_size >= bytes ? 1 : ftruncate(fd, (off_t) bytes) == 0;
    close(fd);
    return ok ? 0 : -1;
}

static int make_aux(const char *path) {
    /* VZMacAuxiliaryStorage's new file: a 24-byte header, then zeros. */
    static const unsigned char header[24] = {0xa5, 0xd5, 0xb3, 0xfe, 0x97, 0xad, 0x1d, 0x27,
                                             1, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0};
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > 0) return 0;   /* keep what a previous run wrote */
    int fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    int ok = write(fd, header, sizeof header) == (ssize_t) sizeof header && ftruncate(fd, 33570816) == 0;
    close(fd);
    return ok ? 0 : -1;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <AVPBooter.bin> <image.ipsw> <workdir> [ram_mb] [cpus] [ecid]\n",
                argv[0]);
        return 2;
    }
    const char *rom = argv[1], *ipsw = argv[2], *work = argv[3];
    int ram_mb = argc > 4 ? atoi(argv[4]) : 4096;
    int cpus = argc > 5 ? atoi(argv[5]) : 2;
    unsigned long long ecid = argc > 6 ? strtoull(argv[6], NULL, 0) : 0x1234567890abcdefull;

    if (hv_android_map_commpage() != 0)
        fprintf(stderr, "no commpage: counter reads will use the helper path\n");

    mkdir(work, 0700);
    /* One installer per work directory: two share the sparse disk, the aux image and every
       dump, so a second one silently corrupts the first one's evidence. */
    {
        char lock[640];
        snprintf(lock, sizeof lock, "%s/.lock", work);
        int lfd = open(lock, O_RDWR | O_CREAT, 0600);
        if (lfd < 0 || flock(lfd, LOCK_EX | LOCK_NB) != 0) {
            fprintf(stderr, "another installer is using %s; stop it first "
                            "(pgrep -af host-install)\n", work);
            return 1;
        }
    }
    char disk[512], aux[512];
    snprintf(disk, sizeof disk, "%s/disk.img", work);
    snprintf(aux, sizeof aux, "%s/aux.img", work);
    if (make_disk(disk, 64ull << 30) != 0) {
        fprintf(stderr, "cannot create %s: %s\n", disk, strerror(errno));
        return 1;
    }
    if (make_aux(aux) != 0) {
        fprintf(stderr, "cannot create %s: %s\n", aux, strerror(errno));
        return 1;
    }

    static char report[8192];
    report[0] = 0;
    if (hv_android_install_start(rom, disk, aux, ipsw, work, ram_mb, cpus, ecid, NULL, report,
                                 sizeof report) != 0) {
        fprintf(stderr, "install did not start:\n%s\n", report);
        return 1;
    }
    printf("%s", report);
    fflush(stdout);

    /* The steps come from idevicerestore; the loop stays until it says it is over. */
    int last = -1;
    for (;;) {
        double progress = 0;
        int step = hv_android_install_status(&progress);
        if (step != last) {
            printf("step %d (%.0f%%)\n", step, progress * 100.0);
            fflush(stdout);
            last = step;
        }
        if (step >= 100) {
            printf("%s\n", step == 100 ? "restore finished" : "restore failed");
            fflush(stdout);
            break;
        }
        sleep(1);
    }
    return 0;
}
