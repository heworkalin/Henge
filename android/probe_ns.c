/*
 * Why libadrenotools could not open the driver.
 *
 * adrenotools_open_libvulkan() returned NULL on the test device and printed
 * nothing at all, which means its very first guard failed:
 *
 *     if (!linkernsbypass_load_status()) return nullptr;
 *
 * linkernsbypass sets that flag in a constructor that performs five steps, and
 * every step bails out with a bare `return`. So the answer is not in the log; it
 * has to be measured. This file repeats those five steps with the outcome of each
 * one recorded, which turns "it does not work" into a specific step.
 *
 * The steps, in order:
 *
 *   1. mprotect the page holding libdl's dlopen to R+W+X, because some devices map
 *      executable code --X.
 *   2. Walk AArch64 instructions from dlopen looking for a BL (signature 0x25)
 *      whose target is bionic's internal __loader_dlopen. Public dlopen is a thin
 *      wrapper around it.
 *   3. Call that internal __loader_dlopen("ld-android.so") passing public dlopen as
 *      the caller address. The caller address is what makes bionic pick the
 *      unrestricted namespace instead of the app's classloader namespace.
 *   4. dlsym __loader_android_link_namespaces{,_all_libs} from ld-android.so.
 *   5. Load libdl_android.so the same way and dlsym __loader_android_create_namespace
 *      and __loader_android_get_exported_namespace.
 *
 * Only if all five succeed does linkernsbypass consider itself loaded. This is
 * inherently version-specific: it depends on the exact instruction sequence in the
 * bionic that shipped with the device, and on those private symbols still being
 * exported from ld-android.so / libdl_android.so. Android 15 is a plausible place
 * for that to have changed, which is what this is meant to establish.
 *
 * Nothing here changes state that libadrenotools relies on: it is a separate
 * probe, and the protections it sets are the ones linkernsbypass sets anyway.
 */

#include <android/api-level.h>
#include <errno.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static void cat(char **cursor, char *end, const char *fmt, ...) {
    if (*cursor >= end) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*cursor, (size_t)(end - *cursor), fmt, ap);
    va_end(ap);
    if (n > 0) *cursor += n;
    if (*cursor > end) *cursor = end;
}

/* Is this address inside an executable mapping? Checked before branching to a
 * computed target: if the BL walk found something that is not code, jumping there
 * would take the app down and we would learn nothing. */
static int page_is_executable(void *addr) {
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512];
    int ok = 0;
    unsigned long want = (unsigned long)(uintptr_t)addr;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perms[8] = {0};
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3) continue;
        if (want >= lo && want < hi) {
            ok = perms[2] == 'x';
            break;
        }
    }
    fclose(f);
    return ok;
}

static void *page_of(void *p) {
    return (void *)((uintptr_t)p & ~(uintptr_t)(getpagesize() - 1));
}

/* One AArch64 instruction, split the way the linker's BL immediate is encoded. */
typedef union {
    uint32_t raw;
    struct {
        int32_t offset : 26; /* byte offset, already shifted */
        uint8_t sig : 6;     /* 0x25 = BL */
    };
} BranchLinked;

/* How far to look for the BL before giving up. bionic's dlopen is a handful of
 * instructions; 64 words covers it with room to spare and keeps the scan inside
 * one page even on a 4 KiB device. */
#define SCAN_WORDS 64

/* dlsym on a handle, reporting NULL rather than the caller having to. */
static const char *sym_state(void *handle, const char *name) {
    if (!handle) return "no handle";
    return dlsym(handle, name) ? "yes" : "NULL";
}

void hv_probe_namespace(char *report, size_t cap) {
    char *cursor = report + strlen(report);
    char *end = report + cap;

    cat(&cursor, end, "\n--- linker namespace (libadrenotools prerequisite) ---\n");
    cat(&cursor, end, "  device API level : %d\n", android_get_device_api_level());
    cat(&cursor, end, "  public dlopen    : %p\n", (void *)(uintptr_t)&dlopen);

    /* Step 1: make the code page writable so the BTI attribute can be cleared. */
    void *dl_page = page_of((void *)(uintptr_t)&dlopen);
    int mp = mprotect(dl_page, (size_t)getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC);
    if (mp == 0)
        cat(&cursor, end, "  1 mprotect R+W+X : ok\n");
    else
        cat(&cursor, end, "  1 mprotect R+W+X : FAILED (%s)\n", strerror(errno));

    /* Step 2: find the BL from dlopen to __loader_dlopen. */
    BranchLinked *insn = (BranchLinked *)(uintptr_t)&dlopen;
    BranchLinked *found = NULL;
    for (int i = 0; i < SCAN_WORDS; i++) {
        /* Stop at the end of the mapped page: reading past it would fault, and a
         * DL stub that is not in this page is not the shape we are looking for. */
        if ((void *)(insn + i) >= (void *)((char *)dl_page + getpagesize())) break;
        if (insn[i].sig == 0x25) {
            found = &insn[i];
            break;
        }
    }

    if (!found) {
        cat(&cursor, end, "  2 BL to __loader_dlopen : NOT FOUND in %d words\n", SCAN_WORDS);
        cat(&cursor, end, "    reading      : dlopen is not the thin BL wrapper linkernsbypass expects.\n"
                          "                   __loader_dlopen cannot be located, so the unrestricted\n"
                          "                   namespace is unreachable and adrenotools cannot work.\n");
        return;
    }
    int32_t off = found->offset; /* already scaled by 4 in the encoding */
    cat(&cursor, end, "  2 BL to __loader_dlopen : found at +%d, target %+d bytes\n",
        (int)(found - insn), off);

    void *loader_dlopen_addr = (void *)(uintptr_t)(found + off);
    cat(&cursor, end, "    __loader_dlopen : %p\n", loader_dlopen_addr);

    if (!page_is_executable(loader_dlopen_addr)) {
        /* Not code: the BL points somewhere unexpected. Refuse to call it rather
         * than crash; the report already says what was computed. */
        cat(&cursor, end, "    WARNING      : target is not in an executable mapping — not calling it.\n"
                          "                   the BL walk found the wrong instruction, or dlopen's\n"
                          "                   wrapper shape differs from what linkernsbypass expects.\n");
        return;
    }

    /* Step 3: load ld-android.so through the internal entry point, with dlopen as
     * the caller address. That address is the whole trick. */
    typedef void *(*loader_dlopen_t)(const char *, int, const void *);
    loader_dlopen_t loader_dlopen = (loader_dlopen_t)loader_dlopen_addr;

    /* Clear the BTI attribute on the entry point, as linkernsbypass does, so it can
     * be branched to indirectly. */
    mprotect(page_of(loader_dlopen_addr), (size_t)getpagesize(), PROT_READ | PROT_WRITE | PROT_EXEC);

    dlerror();
    void *ld_android = loader_dlopen("ld-android.so", RTLD_LAZY, (const void *)(uintptr_t)&dlopen);
    cat(&cursor, end, "  3 ld-android.so  : %s%s\n", ld_android ? "loaded" : "FAILED",
        ld_android ? "" : (dlerror() ? dlerror() : ""));

    /* Step 4: the namespace functions. */
    cat(&cursor, end, "  4 __loader_android_link_namespaces          : %s\n",
        sym_state(ld_android, "__loader_android_link_namespaces"));
    cat(&cursor, end, "    __loader_android_link_namespaces_all_libs : %s\n",
        sym_state(ld_android, "__loader_android_link_namespaces_all_libs"));

    /* Step 5: namespace creation, from libdl_android.so. */
    dlerror();
    void *libdl_android = loader_dlopen("libdl_android.so", RTLD_LAZY, (const void *)(uintptr_t)&dlopen);
    cat(&cursor, end, "  5 libdl_android.so: %s%s\n", libdl_android ? "loaded" : "FAILED",
        libdl_android ? "" : (dlerror() ? dlerror() : ""));
    cat(&cursor, end, "    __loader_android_create_namespace         : %s\n",
        sym_state(libdl_android, "__loader_android_create_namespace"));
    cat(&cursor, end, "    __loader_android_get_exported_namespace   : %s\n",
        sym_state(libdl_android, "__loader_android_get_exported_namespace"));

    int all = ld_android && libdl_android &&
              dlsym(ld_android, "__loader_android_link_namespaces_all_libs") &&
              dlsym(ld_android, "__loader_android_link_namespaces") &&
              dlsym(libdl_android, "__loader_android_create_namespace") &&
              dlsym(libdl_android, "__loader_android_get_exported_namespace");
    cat(&cursor, end, "  verdict          : %s\n",
        all ? "linkernsbypass should load — the later failure is elsewhere"
            : "a step above failed, which is why adrenotools returned NULL silently");
}
