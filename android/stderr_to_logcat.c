/*
 * Sends the process's stdout and stderr to logcat.
 *
 * The restore stack — libimobiledevice, libtatsu, idevicerestore — reports what it is doing with
 * fprintf(stderr) and idevice_set_debug_level. Android discards both file descriptors, so a restore
 * that fails in two seconds leaves no trace: the difference between "Apple refused the signature"
 * and "the guest never appeared on its USB port" is exactly what those lines say, and neither is
 * visible from the app's own log.
 *
 * A pipe plus a reader thread is the smallest thing that fixes that without patching vendored code:
 * every write to fd 1 or fd 2 becomes one logcat line, in order, tagged so it can be filtered out.
 * Lines are split at newlines because logcat treats one call as one entry.
 */
#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define NATIVE_TAG "HENGENATIVE"
#define LINE_MAX_BYTES 2048

/*
 * A boot under emulation prints far more than logcat will carry: tens of thousands of lines a
 * second while a guest is translating, and Android's log daemon starts dropping by process quota
 * long before that, which hides the lines that matter. So the engine's whole stream goes to a
 * file, and logcat gets a rate-limited sample plus every line that looks like a milestone.
 *
 * The file is written where the app can pull it: HENGE_LOG_FILE, set from Java at startup.
 */
static char g_path[512];
static FILE *g_file;
/* The guest's own console (PL011) gets its own file: it is the only place the kernel and the
   restore system speak in sentences, and mixed into the engine's line-by-line diagnostics it is
   impossible to read. Written unbuffered-per-batch from the same reader, so order is preserved. */
static const char SERIAL_PREFIX[] = "hengevm: serial: ";
static char g_serial_path[512];
static FILE *g_serial;
static int g_rate = 600;          /* lines a second to logcat unless raised (HENGE_LOG_RATE) */
static unsigned g_window_second;
static int g_window_count;
static unsigned long g_dropped;

/* mkdir -p, ignoring what already exists. */
static void make_dirs(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        (void)mkdir(tmp, 0770);
        *p = '/';
    }
    (void)mkdir(tmp, 0770);
}

/**
 * Starts this run's log files: <path's directory>/log/<YYYY-MM-DD>/<HHMM>.log, and
 * <HHMM>-serial.log beside it for the guest's own console.
 *
 * One file per run under a date directory, instead of one file that grows without bound: a single
 * henge.log reached 182 MB, and a log nobody can open is a log nobody reads. The name is the start
 * time, because a run does not get to clean up after itself -- it is killed, crashes, or is
 * force-stopped, and none of those run code that could rename anything.
 *
 * `path` may be a file (the app passes .../vm/henge.log) or a directory; either way the layout
 * above is what is written.
 */
void hv_log_set_file(const char *path) {
    if (!path) return;
    char dir[512];
    snprintf(dir, sizeof dir, "%s", path);
    /* A trailing name with a dot in it is a file: keep only its directory. */
    char *slash = strrchr(dir, '/');
    if (slash && strchr(slash, '.')) *slash = 0;

    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    char day[32], clock[16];
    strftime(day, sizeof day, "%Y-%m-%d", &lt);
    strftime(clock, sizeof clock, "%H%M", &lt);

    char base[640];
    snprintf(base, sizeof base, "%s/log/%s", dir, day);
    make_dirs(base);

    /* A second run in the same minute gets its own file rather than appending to the first. */
    for (int n = 1; n < 100; n++) {
        if (n == 1) snprintf(g_path, sizeof g_path, "%s/%s.log", base, clock);
        else snprintf(g_path, sizeof g_path, "%s/%s-%d.log", base, clock, n);
        FILE *f = fopen(g_path, "a+");
        if (!f) continue;
        if (fseek(f, 0, SEEK_END) == 0 && ftell(f) == 0) { g_file = f; break; }
        fclose(f);
    }
    if (!g_file) g_file = fopen(g_path, "a");
    snprintf(g_serial_path, sizeof g_serial_path, "%s/%s-serial.log", base, clock);
    if (!g_serial) g_serial = fopen(g_serial_path, "a");
    if (g_file) {
        fprintf(g_file, "# henge %s %s\n", day, clock);
        fflush(g_file);
    }
}

/* Lines that always survive the limiter: a restore that fails says so in one of these. */
static int important(const char *line) {
    /* Milestones only. "serial:" and "exception" used to be here, and they fire hundreds of times
       a second during a fault loop: every one of them bypassed the limiter, which made the logcat
       calls the slowest part of this thread and backed the pipe up into the engine's own fprintf
       (measured: the guest kept faulting while the console went silent for minutes). The file
       still gets every line; this list is only about what is worth logcat's quota. */
    static const char *const keys[] = {"panic", "error", "Error", "failed", "halt", "Sending",
                                       "install", "ASR", "End of iBoot"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++)
        if (strstr(line, keys[i])) return 1;
    return 0;
}

static int g_pipe[2] = {-1, -1};
static pthread_t g_reader;
static int g_started;

static void *reader(void *arg) {
    (void) arg;
    char line[LINE_MAX_BYTES];
    size_t used = 0;
    for (;;) {
        char buffer[512];
        ssize_t n = read(g_pipe[0], buffer, sizeof buffer);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        for (ssize_t i = 0; i < n; i++) {
            char c = buffer[i];
            if (c == '\n' || used == sizeof(line) - 1) {
                line[used] = '\0';
                if (used > 0) {
                    /* Everything to the file, in order and complete. */
                    if (g_file) {
                        fputs(line, g_file);
                        fputc('\n', g_file);
                        static int since_flush;
                        if (++since_flush >= 64) { since_flush = 0; fflush(g_file); }
                    }
                    /* The guest's console, kept apart and never rate-limited. */
                    if (g_serial && strncmp(line, SERIAL_PREFIX, sizeof SERIAL_PREFIX - 1) == 0) {
                        fputs(line, g_serial);
                        fputc('\n', g_serial);
                        static int since_serial_flush;
                        if (++since_serial_flush >= 16) { since_serial_flush = 0; fflush(g_serial); }
                    }
                    /* A sample to logcat, plus anything that reads like a milestone. */
                    unsigned sec = (unsigned) time(NULL);
                    if (sec != g_window_second) {
                        if (g_dropped) {
                            char note[128];
                            snprintf(note, sizeof note, "(log: %lu lines kept out of logcat in the last second)",
                                     g_dropped);
                            __android_log_write(ANDROID_LOG_INFO, NATIVE_TAG, note);
                            g_dropped = 0;
                        }
                        g_window_second = sec;
                        g_window_count = 0;
                    }
                    if (important(line) || g_window_count < g_rate) {
                        __android_log_write(ANDROID_LOG_INFO, NATIVE_TAG, line);
                        g_window_count++;
                    } else {
                        g_dropped++;
                    }
                }
                used = 0;
            } else if (c != '\r') {
                line[used++] = c;
            }
        }
    }
    return NULL;
}

/*
 * Redirects stdout and stderr into logcat. Safe to call more than once.
 *
 * Returns 0 on success. On failure nothing is redirected and the process behaves as before — a
 * missing log must not be a reason for an install to fail.
 */
int hv_android_capture_output(void) {
    if (g_started) return 0;
    if (!g_path[0]) {
        const char *p = getenv("HENGE_LOG_FILE");
        if (p && *p) hv_log_set_file(p);
    }
    {
        const char *r = getenv("HENGE_LOG_RATE");
        if (r) { int v = atoi(r); if (v > 0) g_rate = v; }
    }
    if (pipe(g_pipe) != 0) return -1;
#ifdef F_SETPIPE_SZ
    /* Default is 64 KB. The engine writes from the CPU threads, so a reader that stalls (logcat,
       a slow file) would otherwise block guest execution itself. Best effort: a refusal just
       leaves the default. */
    (void)!fcntl(g_pipe[1], F_SETPIPE_SZ, 1 << 20);
#endif
    if (dup2(g_pipe[1], STDOUT_FILENO) < 0 || dup2(g_pipe[1], STDERR_FILENO) < 0) {
        close(g_pipe[0]);
        close(g_pipe[1]);
        g_pipe[0] = g_pipe[1] = -1;
        return -1;
    }
    // Both descriptors now point at the pipe; the originals are duplicates of fd 2's target.
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (pthread_create(&g_reader, NULL, reader, NULL) != 0) return -1;
    pthread_detach(g_reader);
    g_started = 1;
    return 0;
}
