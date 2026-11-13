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

#define NATIVE_TAG "HENGENATIVE"
#define LINE_MAX_BYTES 2048

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
                if (used > 0) __android_log_write(ANDROID_LOG_INFO, NATIVE_TAG, line);
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
