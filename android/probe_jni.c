/*
 * The executable-memory probe, as a JNI entry point.
 *
 * Why this exists: hv_jit_region_open() has two shapes — a memfd mapped twice
 * (R-X to run, R-W to write, never W+X) and a single W+X anonymous mapping. Which
 * one an app is allowed to make depends on its SELinux domain and its
 * targetSdkVersion, and that cannot be settled from inside the engine.
 *
 * Nor can it be settled in the proot/glibc container this port is developed in:
 * that environment already runs arbitrary code, so whatever exec restriction
 * exists was bypassed before the engine started, and a probe there answers
 * "allowed" regardless. The answer has to come from a real APK, installed
 * normally, running in its own domain. That is this.
 *
 * Three things are measured, each independently:
 *   1. the memfd split primitive
 *   2. the anonymous W+X primitive
 *   3. what hv_jit_region_open() actually returns, i.e. the engine's own decision
 *
 * Each one writes a real function and calls it, because "mmap returned non-NULL"
 * is not the question — the question is whether the CPU fetched and ran the bytes.
 */

#include <jni.h>
#include <android/native_window_jni.h>

#include "bootrom.h"

/* Declared in stderr_to_logcat.c: the engine's whole stream goes to this file, and a rate-limited
   sample of it plus every milestone goes to logcat. */
int hv_android_capture_output(void);
void hv_log_set_file(const char *path);

#include <android/api-level.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "jit_region.h"

#ifndef __NR_memfd_create
#define __NR_memfd_create 279 /* arm64 */
#endif

/* arm64: movz w0, #0x1234 ; ret — returns a value no leftover memory would
 * happen to contain.
 *   0x52824680  movz w0, #0x1234
 *   0xd65f03c0  ret
 */
static const uint32_t kAnswer[2] = {0x52824680u, 0xd65f03c0u};

#define EXPECTED 0x1234

static void append(char *out, size_t cap, const char *fmt, ...) {
    size_t len = strlen(out);
    if (len + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(out + len, cap - len, fmt, ap);
    va_end(ap);
}

static void read_domain(char *out, size_t cap) {
    FILE *f = fopen("/proc/self/attr/current", "r");
    if (!f) {
        snprintf(out, cap, "(unreadable: %s)", strerror(errno));
        return;
    }
    size_t n = fread(out, 1, cap - 1, f);
    fclose(f);
    out[n] = 0;
    while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) out[--n] = 0;
    if (!n) snprintf(out, cap, "(empty)");
}

/* Write kAnswer at `exec` after placing the bytes through `write_to` and returns
 * what calling it produced. -1 means the call could not be made at all. */
static int run_here(uint32_t *write_to, void *exec) {
    memcpy(write_to, kAnswer, sizeof kAnswer);
    __builtin___clear_cache((char *)exec, (char *)exec + sizeof kAnswer);
    int (*fn)(void) = (int (*)(void))exec;
    return fn();
}

/* Shape 1: one memfd, mapped R-X and R-W. No mapping is ever W+X. */
static int probe_memfd_split(int bytes, char *out, size_t cap) {
    int fd = (int)syscall(__NR_memfd_create, "henge-probe", 0 /* MFD_CLOEXEC */);
    if (fd < 0) {
        append(out, cap, "  memfd_create                 REFUSED  (%s)\n", strerror(errno));
        return 0;
    }
    if (ftruncate(fd, bytes) != 0) {
        append(out, cap, "  ftruncate                    REFUSED  (%s)\n", strerror(errno));
        close(fd);
        return 0;
    }
    void *rx = mmap(NULL, bytes, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    if (rx == MAP_FAILED) {
        append(out, cap, "  mmap R-X of memfd            REFUSED  (%s)\n", strerror(errno));
        close(fd);
        return 0;
    }
    void *rw = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (rw == MAP_FAILED) {
        append(out, cap, "  mmap R-W of memfd            REFUSED  (%s)\n", strerror(errno));
        munmap(rx, bytes);
        close(fd);
        return 0;
    }
    close(fd);

    int value = run_here((uint32_t *)rw, rx);
    append(out, cap, "  memfd split (R-X + R-W)      %s  call=0x%x %s\n",
           value == EXPECTED ? "WORKS  " : "BROKEN ", (unsigned)value,
           value == EXPECTED ? "" : "(exec refused or wrong bytes)");
    munmap(rx, bytes);
    munmap(rw, bytes);
    return value == EXPECTED;
}

/* Shape 2: a single anonymous mapping that is writable and executable at once. */
static int probe_wx(int bytes, char *out, size_t cap) {
    void *p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        append(out, cap, "  anon W+X                     REFUSED  (%s)\n", strerror(errno));
        return 0;
    }
    int value = run_here((uint32_t *)p, p);
    append(out, cap, "  anon W+X                     %s  call=0x%x %s\n",
           value == EXPECTED ? "WORKS  " : "BROKEN ", (unsigned)value,
           value == EXPECTED ? "" : "(exec refused or wrong bytes)");
    munmap(p, bytes);
    return value == EXPECTED;
}

/* Shape 3: whatever the engine itself will get, fallback order included. */
static void probe_engine(int bytes, char *out, size_t cap) {
    unsetenv("HV_JIT_SPLIT");
    HvJitRegion r;
    if (hv_jit_region_open(&r, (size_t)bytes) != 0) {
        append(out, cap, "  hv_jit_region_open()         FAILED — the engine cannot JIT here\n");
        return;
    }
    const char *shape = r.rw_diff ? "memfd split" : "W+X single";
    append(out, cap, "  hv_jit_region_open()         %s (rw_diff=%lld)\n", shape, (long long)r.rw_diff);
    hv_jit_region_close(&r);
}

/* Built by probe_vk.c and appended to this report. The two strings name the
 * Vulkan driver to open — a directory and a file name, both from a driver-plugin
 * APK — or are null for the system loader. See android/probe_vk.c. */
/* Built by probe_ns.c: which linkernsbypass step fails on this device. */
extern void hv_probe_namespace(char *report, size_t cap);

/* Built by henge_vm.c: brings a machine up with reims-vgpu as its GPU, and tears it down. */
extern int hv_android_vm_smoke(char *report, size_t cap, const char *cache_dir);
extern void hv_android_vm_teardown(void);
/* Built by install.c: creating the two images, and the installer itself. */
extern int hv_android_make_aux(const char *path);
extern int hv_android_make_disk(const char *path, uint64_t bytes);
extern void hv_android_install_stop(void);

extern int hv_android_install_start(const char *rom, const char *disk, const char *aux,
                                    const char *ipsw, const char *workdir, int ram_mb, int ncpus,
                                    uint64_t ecid, const char *cache_dir, char *report, size_t cap);
extern int hv_android_install_status(double *progress);
extern int hv_install_result(void);

/* And the real thing: a board with the guest's files, its CPUs running. */
extern int hv_android_vm_boot(const char *rom, const char *disk, const char *aux, int ram_mb,
                              int ncpus, const char *cache_dir, char *report, size_t cap);
extern void hv_android_vm_stop(void);

extern void hv_probe_vulkan_capabilities(char *report, size_t cap,
                                         const char *driver_dir, const char *driver_lib,
                                         const char *tmp_dir);

/* Boots a guest. Returns a short report either way: a missing file is the normal answer here,
 * and it is a sentence, not a crash. */
JNIEXPORT jstring JNICALL
Java_dev_henge_probe_Native_nativeBoot(JNIEnv *env, jclass clazz, jstring rom, jstring disk,
                                       jstring aux, jint ram_mb, jint ncpus, jstring cache_dir) {
    (void)clazz;
    enum { BOOT_BYTES = 8 * 1024 };
    char *out = calloc(1, BOOT_BYTES);
    if (!out) return NULL;

    const char *r = rom ? (*env)->GetStringUTFChars(env, rom, NULL) : NULL;
    const char *d = disk ? (*env)->GetStringUTFChars(env, disk, NULL) : NULL;
    const char *a = aux ? (*env)->GetStringUTFChars(env, aux, NULL) : NULL;
    const char *c = cache_dir ? (*env)->GetStringUTFChars(env, cache_dir, NULL) : NULL;

    hv_android_vm_boot(r, d, a, ram_mb, ncpus, c, out, BOOT_BYTES);

    if (r) (*env)->ReleaseStringUTFChars(env, rom, r);
    if (d) (*env)->ReleaseStringUTFChars(env, disk, d);
    if (a) (*env)->ReleaseStringUTFChars(env, aux, a);
    if (c) (*env)->ReleaseStringUTFChars(env, cache_dir, c);

    jstring result = (*env)->NewStringUTF(env, out);
    free(out);
    return result;
}

/* Where the engine's log goes. Called from Java as soon as the app starts: the file is the record
   an install is diagnosed from, logcat only carries what fits. */
JNIEXPORT void JNICALL
Java_dev_henge_probe_Native_nativeSetLogFile(JNIEnv *env, jclass clazz, jstring path) {
    (void)clazz;
    if (!path) return;
    const char *p = (*env)->GetStringUTFChars(env, path, NULL);
    if (p) { hv_log_set_file(p); (*env)->ReleaseStringUTFChars(env, path, p); }
}

/**
 * Copies the firmware's boot framebuffer into `pixels` (ARGB_8888, one int per pixel) and writes
 * {width, height, stride} into `meta`. False when the guest has not published one yet.
 *
 * The guest's buffer holds BGRA bytes; on a little-endian host that is already 0xAARRGGBB as an
 * int, only with a zero alpha, which is forced opaque here.
 */
JNIEXPORT jboolean JNICALL
Java_dev_henge_probe_Native_nativeBootFramebuffer(JNIEnv *env, jclass clazz,
                                                  jintArray pixels, jintArray meta) {
    (void)clazz;
    extern int hv_android_boot_framebuffer(const uint8_t **pixels, uint32_t *width,
                                           uint32_t *height, uint32_t *stride);
    const uint8_t *px = NULL;
    uint32_t w = 0, h = 0, st = 0;
    if (hv_android_boot_framebuffer(&px, &w, &h, &st) != 0 || !px || !w || !h) return JNI_FALSE;
    if ((*env)->GetArrayLength(env, meta) < 3) return JNI_FALSE;
    jsize need = (jsize)(w * h);
    if ((*env)->GetArrayLength(env, pixels) < need) return JNI_FALSE;
    jint bits[3] = {(jint) w, (jint) h, (jint) st};
    (*env)->SetIntArrayRegion(env, meta, 0, 3, bits);
    /* One row at a time: the guest's stride need not be width * 4. */
    jint *row = malloc(sizeof(jint) * w);
    if (!row) return JNI_FALSE;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = px + (size_t) y * st;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t v = (uint32_t) src[x * 4] | (uint32_t) src[x * 4 + 1] << 8 |
                         (uint32_t) src[x * 4 + 2] << 16;
            row[x] = (jint) (v | 0xff000000u);
        }
        (*env)->SetIntArrayRegion(env, pixels, (jsize) (y * w), (jsize) w, row);
    }
    free(row);
    return JNI_TRUE;
}

/**
 * A line from the app's own log, into the engine's stream so it lands in the same file.
 *
 * logcat cannot be relied on: its main buffer is 256 KiB and the system's own services fill it in
 * seconds, so every HENGE line written a few seconds ago is already gone. The app's log mirrored
 * only there meant the Java half of a run left no trace at all -- which is exactly the half that
 * says whether a surface was handed over and what the screen was told.
 */
JNIEXPORT void JNICALL
Java_dev_henge_probe_Native_nativeLog(JNIEnv *env, jclass clazz, jstring line) {
    (void)clazz;
    if (!line) return;
    const char *s = (*env)->GetStringUTFChars(env, line, NULL);
    if (s) {
        fprintf(stderr, "app: %s\n", s);
        fflush(stderr);
        (*env)->ReleaseStringUTFChars(env, line, s);
    }
}

/** Render state for the app: {has mode, guest presents, host presents}. */
JNIEXPORT void JNICALL
Java_dev_henge_probe_Native_nativeRenderStats(JNIEnv *env, jclass clazz, jintArray out) {
    (void)clazz;
    extern void hv_android_render_stats(int out[3]);
    jint v[3] = {0, 0, 0};
    hv_android_render_stats(v);
    if ((*env)->GetArrayLength(env, out) >= 3) (*env)->SetIntArrayRegion(env, out, 0, 3, v);
}

/** Whether the GPU has taken the screen over, so the boot framebuffer can be dropped. */
JNIEXPORT jboolean JNICALL
Java_dev_henge_probe_Native_nativeDisplayReady(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    extern int hv_android_display_ready(void);
    return hv_android_display_ready() ? JNI_TRUE : JNI_FALSE;
}

/** Whether a machine is running in this process, so the UI can reopen it instead of booting. */
JNIEXPORT jboolean JNICALL
Java_dev_henge_probe_Native_nativeIsUp(JNIEnv *env, jclass clazz) {
    (void)env; (void)clazz;
    extern int hv_android_is_up(void);
    return hv_android_is_up() ? JNI_TRUE : JNI_FALSE;
}

/*
 * The display surface. ANativeWindow_fromSurface() gives the window the engine's GPU device draws
 * into; it takes its own reference, so the caller may drop the Surface afterwards.
 */
JNIEXPORT void JNICALL
Java_dev_henge_probe_Native_nativeAttachWindow(JNIEnv *env, jclass clazz, jobject surface) {
    (void)clazz;
    extern void hv_android_attach_window(void *window);
    if (!surface) return;
    ANativeWindow *window = ANativeWindow_fromSurface(env, surface);
    if (!window) {
        fprintf(stderr, "henge: ANativeWindow_fromSurface returned NULL\n");
        return;
    }
    hv_android_attach_window(window);
    ANativeWindow_release(window);   /* the engine keeps its own reference */
}

JNIEXPORT void JNICALL
Java_dev_henge_probe_Native_nativeStop(JNIEnv *env, jclass clazz) {
    (void)env, (void)clazz;
    hv_android_vm_stop();
}

/* Creates the two images the installer writes, if they are not there yet. */
JNIEXPORT jstring JNICALL
Java_dev_henge_probe_Native_nativeInstallPrepare(JNIEnv *env, jclass clazz, jstring dir, jstring disk,
                                                 jstring aux, jlong disk_bytes) {
    (void)clazz;
    const char *d = dir ? (*env)->GetStringUTFChars(env, dir, NULL) : NULL;
    const char *k = disk ? (*env)->GetStringUTFChars(env, disk, NULL) : NULL;
    const char *a = aux ? (*env)->GetStringUTFChars(env, aux, NULL) : NULL;
    char out[512];
    if (!d || !k || !a) {
        snprintf(out, sizeof out, "参数缺失");
    } else {
        int disk_r = hv_android_make_disk(k, (uint64_t)disk_bytes);
        int aux_r = hv_android_make_aux(a);
        snprintf(out, sizeof out, "磁盘: %s\n辅助存储: %s", disk_r == 0 ? "就绪" : "失败",
                 aux_r == 0 ? "就绪" : "失败");
        if (disk_r != 0 || aux_r != 0) {
            snprintf(out + strlen(out), sizeof(out) - strlen(out), "\n目录: %s", d);
        }
    }
    if (d) (*env)->ReleaseStringUTFChars(env, dir, d);
    if (k) (*env)->ReleaseStringUTFChars(env, disk, k);
    if (a) (*env)->ReleaseStringUTFChars(env, aux, a);
    return (*env)->NewStringUTF(env, out);
}

/* Starts the installer; returns at once with a report, progress comes from nativeInstallStatus. */
JNIEXPORT jstring JNICALL
Java_dev_henge_probe_Native_nativeInstallStart(JNIEnv *env, jclass clazz, jstring rom, jstring disk,
                                               jstring aux, jstring ipsw, jstring workdir,
                                               jint ram_mb, jint ncpus, jlong ecid,
                                               jstring cache_dir) {
    (void)clazz;
    enum { REPORT = 4096 };
    char *out = calloc(1, REPORT);
    if (!out) return NULL;
    const char *r = rom ? (*env)->GetStringUTFChars(env, rom, NULL) : NULL;
    const char *d = disk ? (*env)->GetStringUTFChars(env, disk, NULL) : NULL;
    const char *a = aux ? (*env)->GetStringUTFChars(env, aux, NULL) : NULL;
    const char *i = ipsw ? (*env)->GetStringUTFChars(env, ipsw, NULL) : NULL;
    const char *w = workdir ? (*env)->GetStringUTFChars(env, workdir, NULL) : NULL;
    const char *c = cache_dir ? (*env)->GetStringUTFChars(env, cache_dir, NULL) : NULL;

    hv_android_install_start(r, d, a, i, w, ram_mb, ncpus, (uint64_t)ecid, c, out, REPORT);

    if (r) (*env)->ReleaseStringUTFChars(env, rom, r);
    if (d) (*env)->ReleaseStringUTFChars(env, disk, d);
    if (a) (*env)->ReleaseStringUTFChars(env, aux, a);
    if (i) (*env)->ReleaseStringUTFChars(env, ipsw, i);
    if (w) (*env)->ReleaseStringUTFChars(env, workdir, w);
    if (c) (*env)->ReleaseStringUTFChars(env, cache_dir, c);

    jstring result = (*env)->NewStringUTF(env, out);
    free(out);
    return result;
}

/* Ends the running install: its CPUs stop and the machine can be built again. */
JNIEXPORT void JNICALL
Java_dev_henge_probe_Native_nativeInstallStop(JNIEnv *env, jclass clazz) {
    (void)env, (void)clazz;
    hv_android_install_stop();
}

/* "step 3 42% running" — the installer's own steps, from idevicerestore. */
JNIEXPORT jstring JNICALL
Java_dev_henge_probe_Native_nativeInstallStatus(JNIEnv *env, jclass clazz) {
    (void)clazz;
    double progress = 0;
    int step = hv_android_install_status(&progress);
    /* The verdict comes from this run's own steps. hv_install_result() speaks for whichever restore
       thread is in the process, including one belonging to a run that has already been replaced. */
    const char *state = step >= 101 ? "失败" : step == 100 ? "完成" : "进行中";
    char out[128];
    snprintf(out, sizeof out, "%d %.0f%% %s", step < 0 ? 0 : step, progress * 100.0, state);
    return (*env)->NewStringUTF(env, out);
}

JNIEXPORT jstring JNICALL
Java_dev_henge_probe_Native_nativeProbe(JNIEnv *env, jclass clazz, jint bytes,
                                              jstring driver_dir, jstring driver_lib,
                                              jstring tmp_dir) {
    (void)clazz;
    if (bytes < 4096) bytes = 1 << 20;

    const char *dir = driver_dir ? (*env)->GetStringUTFChars(env, driver_dir, NULL) : NULL;
    const char *lib = driver_lib ? (*env)->GetStringUTFChars(env, driver_lib, NULL) : NULL;
    const char *tmp = tmp_dir ? (*env)->GetStringUTFChars(env, tmp_dir, NULL) : NULL;

    /* Heap, not stack: the Vulkan section lists every device extension and runs to
     * a few kilobytes on a typical phone. */
    enum { REPORT_BYTES = 48 * 1024 };
    char *out = calloc(1, REPORT_BYTES);
    if (!out) return NULL;
    const size_t cap = REPORT_BYTES;

    char domain[256];
    read_domain(domain, sizeof domain);

    append(out, cap, "Henge executable-memory probe\n\n");
    append(out, cap, "  page size        : %ld\n", (long)sysconf(_SC_PAGESIZE));
    append(out, cap, "  SELinux domain   : %s\n", domain);
    append(out, cap, "  device API level : %d\n", android_get_device_api_level());
    append(out, cap, "  Vulkan driver    : %s\n",
           (dir && lib) ? "named plugin" : "system loader");
    append(out, cap, "  requested bytes  : %d\n\n", bytes);

    int split_ok = probe_memfd_split(bytes, out, cap);
    int wx_ok = probe_wx(bytes, out, cap);
    append(out, cap, "\n");
    probe_engine(bytes, out, cap);

    append(out, cap, "\n");
    if (split_ok)
        append(out, cap, "VERDICT: split works — keep it. Policy-clean, no W+X at any point.\n");
    else if (wx_ok)
        append(out, cap, "VERDICT: only W+X works. The split is refused on this device, so\n"
                                "         the engine falls back to a W+X mapping.\n");
    else
        append(out, cap, "VERDICT: neither works. The engine cannot run here.\n");

    /* Stage 0's other half: what this device's Vulkan actually offers, via the
     * driver that was named (or the system loader when none was). */
    hv_probe_vulkan_capabilities(out, cap, dir, lib, tmp);

    /* Only worth asking why when a driver was named and could not be opened. */
    if (dir && lib) hv_probe_namespace(out, cap);

    /* The last section: a real machine, with the real GPU device on it. This allocates 2 GB of
     * guest RAM and 768 MB of JIT memory, so it runs once and is torn down at once. */
    hv_android_vm_smoke(out, cap, tmp);
    hv_android_vm_teardown();

    if (dir) (*env)->ReleaseStringUTFChars(env, driver_dir, dir);
    if (lib) (*env)->ReleaseStringUTFChars(env, driver_lib, lib);
    if (tmp) (*env)->ReleaseStringUTFChars(env, tmp_dir, tmp);

    /* An explicit end marker, so "did it finish?" is not a guess. Everything the
     * probe has to say is above this line. */
    append(out, cap, "\n--- end of report ---\n");

    jstring result = (*env)->NewStringUTF(env, out);
    free(out);
    return result;
}


/*
 * Unpacks the virtual Mac's boot ROM from one compressed block of an Apple restore image.
 *
 * Java fetches the block — it already has an HTTP stack and a service that survives the screen —
 * and this does the part Java cannot: LZFSE, then the compressed fork's LZVN pieces, then the hash.
 * The file is written only after the hash matches, so a half-unpacked ROM can never be left behind
 * for the next run to trust.
 */
JNIEXPORT jstring JNICALL
Java_dev_henge_probe_Native_nativeBootRomUnpack(JNIEnv *env, jobject thiz,
                                                jbyteArray block, jstring out_path) {
    (void) thiz;
    if (!block || !out_path) return (*env)->NewStringUTF(env, "参数为空");

    jsize len = (*env)->GetArrayLength(env, block);
    if ((size_t) len < HV_BOOTROM_BLOCK_BYTES) {
        return (*env)->NewStringUTF(env, "块长度不足");
    }
    jbyte *bytes = (*env)->GetByteArrayElements(env, block, NULL);
    if (!bytes) return (*env)->NewStringUTF(env, "无法读取块数据");

    static uint8_t rom[HV_BOOTROM_BYTES];
    int rc = hv_bootrom_unpack((const uint8_t *) bytes, (size_t) len, rom, sizeof(rom));
    (*env)->ReleaseByteArrayElements(env, block, bytes, JNI_ABORT);

    if (rc != 0) {
        const char *why = rc == -1 ? "块解压失败" : rc == -2 ? "fork 结构不符"
                        : rc == -3 ? "长度不符" : "SHA-256 不符（Apple 的镜像可能变了）";
        return (*env)->NewStringUTF(env, why);
    }

    const char *path = (*env)->GetStringUTFChars(env, out_path, NULL);
    if (!path) return (*env)->NewStringUTF(env, "路径为空");
    FILE *f = fopen(path, "wb");
    if (!f) {
        (*env)->ReleaseStringUTFChars(env, out_path, path);
        return (*env)->NewStringUTF(env, "无法写入文件");
    }
    size_t wrote = fwrite(rom, 1, sizeof(rom), f);
    fclose(f);
    (*env)->ReleaseStringUTFChars(env, out_path, path);
    if (wrote != sizeof(rom)) return (*env)->NewStringUTF(env, "写入不完整");
    return NULL;
}

