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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#include <pthread.h>

int hv_android_install_start(const char *rom, const char *disk, const char *aux, const char *ipsw,
                             const char *workdir, int ram_mb, int ncpus, uint64_t ecid,
                             const char *cache_dir, char *report, size_t cap);
int hv_android_install_status(double *progress);
void hv_android_scan_procs(void);

/* HV_TRACE_PC's buffer only reaches a file when the restore gives up (scan_procs), and a stuck
   guest can sit in the page-fault loop for many minutes before that. SIGUSR1 dumps it on demand:
   the handler only raises a flag, the main loop (which sleeps 1 s at a time) does the IO, so no
   malloc/fopen runs in signal context. `kill -USR1 <pid>` while the guest is stuck, then read
   the file HV_TRACE_PC pointed at. */
void hv_trace_dump(void);
static volatile sig_atomic_t trace_dump_req;
static void on_sigusr1(int sig) { (void) sig; trace_dump_req = 1; }

/* compat_android.c synthesises the commpage the translator reads while emitting counter
   instructions. In the app this happens from the JNI entry; here it has to happen before any guest
   code is translated, or the first counter read takes the process down at 0xfffffc088. */
int hv_android_map_commpage(void);

/* Defined in henge_vm.c in the app, which builds machines for the bring-up probe: nothing here
   uses it, and the install path only calls it to drop a previous machine. */
/* android/henge_vm.c defines the real one; this keeps a CLI built without it linkable.
   (A weak definition loses to the strong one, so both builds work.) */
__attribute__((weak)) void hv_android_vm_teardown(void) {}

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


/*
 * HV_POINTER_MS=<ms>: a pointer that never stops moving, in random directions, for ever.
 *
 * The guest's compositor repaints only what its pointer dirties, so a still pointer leaves the
 * screen frozen and no repaint is ever seen (the operator's words: it needs the mouse moving, or
 * every frame caught happens to be a normal one). This thread sends a random move every `ms`
 * milliseconds from the moment the process starts -- the reports wait in the guest's own xhci
 * device until it configures the endpoint -- so the guest is repainting continuously by the time
 * anything is worth grabbing. A tablet report now and then puts the pointer somewhere new, which
 * keeps a relative-only pointer from being parked against an edge of the screen.
 */
static void *pointer_thread(void *arg) {
    extern void hv_android_mouse_move(int dx, int dy);
    extern void hv_android_tablet(int x16, int y16, int buttons, int wheel);
    long ms = (long)(intptr_t)arg;
    /* xorshift64: no libm, no table, different every run. */
    uint64_t s = 0x9e3779b97f4a7c15ull ^ (uint64_t)time(NULL);
    unsigned n = 0;
    for (;;) {
        usleep((useconds_t)ms * 1000);
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        /* Long strides, not jitter: the guest recomputes its layers when the pointer crosses
           them, and a short move recomputes almost nothing (the operator's note: it must move,
           and far enough to make it redraw). */
        int dx = (int)(s % 161) - 80;
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        int dy = (int)(s % 161) - 80;
        hv_android_mouse_move(dx, dy);
        /* Every so often, say where the pointer is in absolute terms too: it is a tablet the guest
           knows as a real pointing device, and it un-parks a cursor that drifted to an edge. */
        if (++n % 32 == 0) {
            int x = 32768 + (int)((s >> 8) % 26000) - 13000;
            int y = 32768 + (int)((s >> 24) % 20000) - 10000;
            hv_android_tablet(x, y, 0, 0);
        }
    }
    return NULL;
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

    /* HV_POINTER_MS: the pointer thread, before the guest ever has a display: it queues its
       reports, and the guest's own xhci device holds them until it configures the endpoint. */
    {
        const char *ms = getenv("HV_POINTER_MS");
        if (ms && *ms && atoi(ms) > 0) {
            pthread_t t;
            if (pthread_create(&t, NULL, pointer_thread, (void *)(intptr_t)atoi(ms)) != 0)
                fprintf(stderr, "henge: no pointer thread\n");
            else {
                pthread_detach(t);
                fprintf(stderr, "henge: pointer moving in random directions every %d ms\n",
                        atoi(ms));
            }
        }
    }
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
    snprintf(aux, sizeof aux, "%s/aux.img", work);

    /* HV_BOOT_ONLY=1: start what is already on a disk instead of restoring onto it.
     *
     *   host-install <AVPBooter.bin> <disk.img> <workdir> [ram_mb] [cpus]
     *
     * The second argument is the disk then, not an IPSW. This is the path a GPU or a running
     * system test needs: no idevicerestore, no restore OS, and nothing written to the image
     * beyond what the booted system itself does. Kept opt-in so the ordinary invocation keeps
     * meaning "install". */
    if (getenv("HV_BOOT_ONLY")) {
        /* reims-vgpu keeps its shader cache at $HOME/.cache/reims-vgpu-vulkan, and a shell login
           has HOME=/ -- the Vulkan runtime refuses to start when it cannot write there (measured:
           "the shader cache directory /.cache/reims-vgpu-vulkan cannot be used", then abort).
           The work directory is writable and is the right place for a run's scratch anyway. */
        const char *home = getenv("HOME");
        if (!home || !*home || strcmp(home, "/") == 0) setenv("HOME", work, 1);
        extern int hv_android_vm_boot(const char *rom, const char *disk, const char *aux,
                                      int ram_mb, int ncpus, const char *cache_dir,
                                      char *report, size_t cap);
        if (make_aux(aux) != 0) {
            fprintf(stderr, "cannot create %s: %s\n", aux, strerror(errno));
            return 1;
        }
        static char boot_report[8192];
        boot_report[0] = 0;
        if (hv_android_vm_boot(rom, ipsw, aux, ram_mb, cpus, NULL, boot_report,
                               sizeof boot_report) != 0) {
            fprintf(stderr, "boot did not start:\n%s\n", boot_report);
            return 1;
        }
        printf("%s", boot_report);
        fflush(stdout);
        /* The display decision, in one line at the start of the run: a command-line process has
           no window, so the display is off-screen and a grab is the only way to see anything. The
           point is that "no picture on a screen" and "the guest is not drawing" stop looking the
           same in the log. */
        {
            extern const char *hv_android_display_name(void);
            fprintf(stderr, "henge: display = %s\n", hv_android_display_name());
        }
        /* The guest runs on its own threads; this one waits for SIGUSR1 and dumps on demand --
           the same handlers the install loop below uses, because the guest's own log is in
           msgbuf (the normal boot, like upstream's, has no serial console; only the restore boot
           gets one from idevicerestore's boot-args). */
        {
            /* HV_SCAN_PROC=<seconds>: ask the guest's own memory which processes it has, now and
               then. The normal boot has no serial console (its boot-args are iBEC's, and the engine
               does not rewrite them), so this is the only way to tell "macOS reached its window
               server" from "the guest stopped early" -- and a black screen is exactly the case
               where the two look alike. */
            extern void hv_android_scan_procs(void);
            static int every = -1;
            static time_t last;
            if (every < 0) {
                const char *e = getenv("HV_SCAN_PROC");
                every = (e && *e) ? atoi(e) : 0;
                if (every <= 0) every = 1;
            }
            if (getenv("HV_SCAN_PROC") && time(NULL) - last >= every) {
                last = time(NULL);
                fprintf(stderr, "henge: scanning the guest's processes\n");
                hv_android_scan_procs();
            }
        }
        for (;;) {
            /* HV_FAKE_ESC=<seconds>: inject one Escape after the guest has had that long to come
               up. Reuses the engine's own injection (hv_xhci_key through hv_android_key), which is
               what the app's Hardware.swift ends up calling -- a booted macOS sits at a "press any
               key" / login prompt otherwise, and there is no other way to tell it to wake. */
            {
                const char *after = getenv("HV_FAKE_ESC");
                static int sent;
                static time_t started;
                if (after && *after && !sent) {
                    if (!started) started = time(NULL);
                    if (time(NULL) - started >= atoi(after)) {
                        extern void hv_android_key(uint8_t usage, int down);
                        hv_android_key(0x29, 1);
                        usleep(80000);
                        hv_android_key(0x29, 0);
                        sent = 1;
                        fprintf(stderr, "henge: injected Escape\n");
                    }
                }
            }
            /* One line per 30 s with every CPU's guest pc, el and halted flag: a pc that changes is
               a guest at work, a fixed one is exactly where it stopped. The engine has the same
               snapshot for the app (`hv_android_cpu_snapshot`); a command-line run needs it too
               because "the log stopped" and "the guest stopped" are different answers. */
            {
                static time_t last_cpu_line, last_stall_report;
                if (time(NULL) - last_cpu_line >= 30) {
                    extern int hv_android_cpu_snapshot(uint64_t *out, int max_cpus);
                    extern void hv_android_stall_report(void);
                    static uint64_t prev[64 * 3];
                    static int prev_n, same;
                    uint64_t snap[64 * 3];
                    last_cpu_line = time(NULL);
                    int n = hv_android_cpu_snapshot(snap, 64);
                    fprintf(stderr, "henge: cpus");
                    for (int i = 0; i < n; i++)
                        fprintf(stderr, " [%d] pc=%016llx el=%llu halted=%llu", i,
                                (unsigned long long)snap[i * 3 + 0],
                                (unsigned long long)snap[i * 3 + 1],
                                (unsigned long long)snap[i * 3 + 2]);
                    fprintf(stderr, "\n");
                    /* Every pc the same as the sample before: 30 s of no guest progress. Three of
                       those (90 s) and the report says why -- once, then at most every 5 min. The
                       question it answers is the one the old logs could not: which CPU state and
                       which interrupt line the guest is actually stopped on. */
                    same = (n == prev_n && memcmp(snap, prev, (size_t)n * 3 * sizeof snap[0]) == 0)
                               ? same + 1 : 0;
                    memcpy(prev, snap, sizeof prev);
                    prev_n = n;
                    /* HV_STALL_EVERY=<seconds>: the report on a clock instead of on a detected
                       stall. A guest whose two CPUs take turns between idle and kernel work never
                       gives three identical samples, yet may still be going nowhere; this is how
                       its threads get named. */
                    {
                        static int every;
                        static int every_read;
                        if (!every_read) {
                            const char *e = getenv("HV_STALL_EVERY");
                            every = e ? atoi(e) : 0;
                            every_read = 1;
                        }
                        if (every > 0 && time(NULL) - last_stall_report >= every) {
                            last_stall_report = time(NULL);
                            hv_android_stall_report();
                        }
                    }
                    if (same >= 2 && time(NULL) - last_stall_report >= 300) {
                        last_stall_report = time(NULL);
                        fprintf(stderr, "henge: no guest progress for %d s\n", (same + 1) * 30);
                        hv_android_stall_report();
                    }
                }
            }
            /* HV_GRAB_FRAME=<seconds>: once the guest has had that long, write its current frame
               to a BMP next to the work directory. This is the off-screen display path: no
               window, so a command-line bring-up can still see what the guest drew. */
            /* HV_GRAB_EVERY=<seconds>: keep writing frame-NNN.bmp, so the firmware's progress bar
               can be compared across time (a stalled boot and a slow one look the same once). */
            {
                const char *every = getenv("HV_GRAB_EVERY");
                static time_t last;
                static int seq;
                if (every && *every && time(NULL) - last >= atoi(every)) {
                    extern int hv_android_boot_framebuffer(const uint8_t **px, uint32_t *w,
                                                           uint32_t *h, uint32_t *stride);
                    const uint8_t *px = NULL;
                    uint32_t w = 0, h = 0, st = 0;
                    last = time(NULL);
                    if (hv_android_boot_framebuffer(&px, &w, &h, &st) == 0 && px) {
                        char bmp[672];
                        snprintf(bmp, sizeof bmp, "%s/fb-%03d.bmp", work, ++seq);
                        FILE *f = fopen(bmp, "wb");
                        if (f) {
                            uint8_t hd[54];
                            memset(hd, 0, sizeof hd);
                            hd[0] = 'B', hd[1] = 'M';
                            uint32_t fs = (uint32_t)(54 + (uint64_t)st * h), off2 = 54, hr = 40;
                            int32_t ww = (int32_t)w, hh2 = (int32_t)h;
                            uint16_t pl = 1, bpp = 32;
                            memcpy(hd + 2, &fs, 4); memcpy(hd + 10, &off2, 4);
                            memcpy(hd + 14, &hr, 4); memcpy(hd + 18, &ww, 4);
                            memcpy(hd + 22, &hh2, 4); memcpy(hd + 26, &pl, 2);
                            memcpy(hd + 28, &bpp, 2);
                            fwrite(hd, 1, 54, f);
                            for (int y = (int)h - 1; y >= 0; y--)
                                fwrite(px + (size_t)y * st, 1, (size_t)w * 4, f);
                            fclose(f);
                            fprintf(stderr, "henge: %s (progress bar sample)\n", bmp);
                        }
                    }
                    /* The same sample from the GPU's side: the frame reims-vgpu drew, not the
                       firmware's boot framebuffer. Both are wanted -- the progress bar lives in
                       the latter until the guest's own compositor runs, and a black GPU frame
                       beside a drawn progress bar says the grab path is the one to look at. */
                    {
                        extern int hv_android_grab_frame(const char *path);
                        const char *name = getenv("HV_GRAB_GPU");
                        if (name && *name) {
                            char pic[672];
                            snprintf(pic, sizeof pic, "%s/gpu-%03d.bmp", work, seq);
                            int got = hv_android_grab_frame(pic);
                            fprintf(stderr, "henge: %s (gpu frame %d)\n", pic, got);
                        }
                    }
                }
            }
            /* HV_AUTO_INPUT=<seconds>: the pointer moves and Return is pressed, so a command-line
               run reaches the screens a person would and has a cursor over them. The operator's
               fault needs a moving pointer to appear at all ("at least one mouse signal"), and a
               guest left on one dialog draws no cursor: this is the off-screen counterpart of
               touching the screen. */
            {
                const char *secs = getenv("HV_AUTO_INPUT");
                static time_t last_input;
                static int step;
                if (secs && *secs && time(NULL) - last_input >= atoi(secs)) {
                    extern void hv_android_tablet(int x16, int y16, int buttons, int wheel);
                    extern void hv_android_mouse_move(int dx, int dy);
                    extern void hv_android_key(uint8_t usage, int down);
                    last_input = time(NULL);
                    step++;
                    /* A tablet report puts the guest's pointer at a place; the relative move after
                       it is what a trackpad would add, and what makes the guest repaint around it. */
                    int x = 32768 + (step % 5) * 4000 - 8000;
                    int y = 32768 + (step % 3) * 5000 - 5000;
                    hv_android_tablet(x, y, 0, 0);
                    hv_android_mouse_move((step % 2) ? 40 : -40, (step % 3) ? 25 : -25);
                    /* Every fourth step: a click on Setup Assistant's Continue arrow (bottom right
                       of the 1920x1080 mode), which is what advances it -- Return alone does not,
                       the page wants the button (measured: eight Returns and the language page
                       stayed). The button is a real one: the tablet reports absolute position. */
                    if (step % 4 == 0) {
                        int bx = (int)(1560.0 * 65535 / 1920), by = (int)(793.0 * 65535 / 1080);
                        hv_android_tablet(bx, by, 0, 0);
                        usleep(60000);
                        hv_android_tablet(bx, by, 1, 0);
                        usleep(60000);
                        hv_android_tablet(bx, by, 0, 0);
                        fprintf(stderr, "henge: auto input click at %d,%d\n", bx, by);
                    }
                    fprintf(stderr, "henge: auto input step %d (tablet %d,%d)\n", step, x, y);
                }
            }
            {
                const char *after = getenv("HV_GRAB_FRAME");
                static int grabbed;
                static time_t gstarted;
                if (after && *after && !grabbed) {
                    if (!gstarted) gstarted = time(NULL);
                    if (time(NULL) - gstarted >= atoi(after)) {
                        extern int hv_android_grab_frame(const char *path);
                        char pic[640];
                        snprintf(pic, sizeof pic, "%s/frame.bmp", work);
                        grabbed = 1;
                        int got = hv_android_grab_frame(pic);
                        {
                            extern const char *hv_android_display_name(void);
                            fprintf(stderr, "henge: display = %s, grab = %s\n",
                                    hv_android_display_name(),
                                    got == 0 ? "content written (the guest draws)"
                                             : "no content (no mode yet, or no frame)");
                        }
                        if (got != 0) {
                            /* No GPU frame yet: fall back to the firmware's boot framebuffer, the
                               same two-step host/vm.c's hv_vm_boot_display does. */
                            extern int hv_android_boot_framebuffer(const uint8_t **px,
                                                                   uint32_t *w, uint32_t *h,
                                                                   uint32_t *stride);
                            const uint8_t *px = NULL;
                            uint32_t w = 0, h = 0, st = 0;
                            if (hv_android_boot_framebuffer(&px, &w, &h, &st) == 0 && px) {
                                char bmp[672];
                                snprintf(bmp, sizeof bmp, "%s/bootfb.bmp", work);
                                FILE *f = fopen(bmp, "wb");
                                if (f) {
                                    uint8_t hd[54];
                                    memset(hd, 0, sizeof hd);
                                    hd[0] = 'B', hd[1] = 'M';
                                    uint32_t fs = (uint32_t)(54 + (uint64_t)st * h), off2 = 54, hr = 40;
                                    int32_t ww = (int32_t)w, hh2 = (int32_t)h;
                                    uint16_t pl = 1, bpp = 32;
                                    memcpy(hd + 2, &fs, 4); memcpy(hd + 10, &off2, 4);
                                    memcpy(hd + 14, &hr, 4); memcpy(hd + 18, &ww, 4);
                                    memcpy(hd + 22, &hh2, 4); memcpy(hd + 26, &pl, 2);
                                    memcpy(hd + 28, &bpp, 2);
                                    fwrite(hd, 1, 54, f);
                                    /* The guest's framebuffer is 32-bit BGRX, already the BMP
                                       order; rows go bottom-up. */
                                    for (int y = (int)h - 1; y >= 0; y--)
                                        fwrite(px + (size_t)y * st, 1, (size_t)w * 4, f);
                                    fclose(f);
                                    got = 0;
                                    fprintf(stderr, "henge: boot framebuffer %ux%u (stride %u) -> %s\n",
                                            w, h, st, bmp);
                                }
                            }
                        }
                        fprintf(stderr, "henge: grabbing a frame into %s: %s\n", pic,
                                got == 0 ? "ok" : "no frame (no mode yet, and no boot framebuffer)");
                    }
                }
            }
            if (trace_dump_req) {
                trace_dump_req = 0;
                hv_trace_dump();
                { extern void hv_fault_stats_dump(void); hv_fault_stats_dump(); }
                { extern void hv_svc_dump_tail(void); hv_svc_dump_tail(); }
                if (getenv("HV_THREADS")) { extern void hv_threads_dump(void); hv_threads_dump(); }
                /* No hv_android_scan_procs() here: it walks hundreds of thousands of guest pages
                   from this thread while the CPU threads are running, and that races with their
                   own memory path -- measured as a host SIGSEGV a few seconds after the request,
                   which disappears entirely under a debugger because attaching stops the CPUs.
                   The install loop can call it because it runs after the restore has ended; a
                   booted guest does not have that quiescent moment. */
            }
            sleep(1);
        }
    }

    snprintf(disk, sizeof disk, "%s/disk.img", work);
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
    signal(SIGUSR1, on_sigusr1);
    int last = -1;
    for (;;) {
        if (trace_dump_req) {
            trace_dump_req = 0;
            hv_trace_dump();
            { extern void hv_fault_stats_dump(void); hv_fault_stats_dump(); }
            { extern void hv_svc_dump_tail(void); hv_svc_dump_tail(); }
            if (getenv("HV_THREADS")) { extern void hv_threads_dump(void); hv_threads_dump(); }
            if (getenv("HV_SCAN_PROC")) hv_android_scan_procs();
        }
        double progress = 0;
        int step = hv_android_install_status(&progress);
        if (step != last) {
            printf("step %d (%.0f%%)\n", step, progress * 100.0);
            fflush(stdout);
            last = step;
        }
        if (step >= 100) {
            /* Before the verdict is printed, ask the guest's own memory which processes it made:
               the restore either never forked restored, or forked it and it never opened the USB
               channel, and those two want different next steps. */
            hv_android_scan_procs();
            printf("%s\n", step == 100 ? "restore finished" : "restore failed");
            fflush(stdout);
            /* With a console breakpoint armed, the machine is the thing under inspection, and the
               state at the moment the restore gives up is exactly what is wanted: keep it alive
               (no teardown, no exit) until an explicit kill. install.c already skips stop_cpus()
               for this case; without this the CLI would exit and take the machine with it. */
            const char *stop_on = getenv("HENGE_STOP_ON");
            if (stop_on && *stop_on) {
                fprintf(stderr, "hengevm: HENGE_STOP_ON is set, staying alive for gdb "
                                "(pid %d); kill -9 %d to exit\n", (int) getpid(), (int) getpid());
                fflush(stderr);
                for (;;) sleep(60);
            }
            break;
        }
        sleep(1);
    }
    return 0;
}
