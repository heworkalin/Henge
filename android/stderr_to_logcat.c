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

void hv_log_set_file(const char *path) {
    if (!path) return;
    snprintf(g_path, sizeof g_path, "%s", path);
    if (!g_file) g_file = fopen(g_path, "a");
    /* Same directory, same stem: .../henge.log -> .../henge-serial.log */
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t stem = dot ? (size_t)(dot - path) : strlen(path);
    snprintf(g_serial_path, sizeof g_serial_path, "%.*s-serial.log", (int) stem, path);
    if (!g_serial) g_serial = fopen(g_serial_path, "a");
}

/* Lines that always survive the limiter: a restore that fails says so in one of these. */
static int important(const char *line) {
    static const char *const keys[] = {"panic", "error", "Error", "failed", "restore", "step",
                                       "serial:", "halt", "exception", "Sending", "install"};
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
