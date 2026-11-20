/*
 * The Vulkan loader the engine uses, opened the way the app opens it.
 *
 * On Android the GPU driver NEEDs the vendor libraries (libcutils, libhardware, libsync,
 * libnativewindow), which no classloader namespace holds: a plain dlopen of
 * libvulkan_freedreno.so fails on them (measured; android/README.md). libadrenotools builds a
 * linker namespace whose parent resolves them and hooks android_dlopen_ext, so the loader
 * resolves the driver when it enumerates devices. The handle it returns is the system loader's,
 * so vkGetInstanceProcAddr on it is the loader's, and everything after it is ordinary Vulkan.
 *
 * Why the engine wants Turnip at all, measured on PJE110 (Adreno 740, Android 15):
 *
 *   system driver  Adreno 1.3.128, 114 device extensions: no
 *                  VK_EXT_rasterization_order_attachment_access and no
 *                  VK_EXT_fragment_shader_interlock, the two forms of Metal's framebuffer fetch,
 *                  so every pipeline whose fragment function fetches its colour attachment
 *                  (macOS's own in-place copy pass among them) was refused.
 *   Turnip         Adreno 1.3.296, 145 extensions: VK_EXT_rasterization_order_attachment_access
 *                  (and VK_ARM_...), VK_EXT_external_memory_dma_buf, VK_EXT_host_image_copy,
 *                  VK_EXT_image_drm_format_modifier.
 *
 * HV_VK_DRIVER names the directory holding libmain_hook.so, libhook_impl.so and the driver:
 *
 *   HV_VK_DRIVER=/data/local/tmp/henge/driver
 *
 * Nothing here is Android-only in principle, but only Android has the namespace to build, so the
 * engine asks for it under `target_os = "android"` alone.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "vk_loader.h"

#ifdef __ANDROID__
#include <dlfcn.h>
#include <stdint.h>
#include <unistd.h>

/* libadrenotools' entry point: the loader handle, or NULL with nothing printed (it returns
 * silently when the driver path does not stat -- see the trailing-slash note below). */
extern void *adrenotools_open_libvulkan(int dlopenMode, int featureFlags, const char *tmpLibDir,
                                        const char *hookLibDir, const char *customDriverDir,
                                        const char *customDriverName, const char *fileRedirectDir,
                                        void **userMappingHandle);

#define ADRENOTOOLS_DRIVER_CUSTOM 1
#define ADRENOTOOLS_DRIVER_NAME "libvulkan_freedreno.so"

static char vk_loader_error[512];

/**
 * The directory this library was loaded from: on Android that is the app's
 * `nativeLibraryDir`, where the driver and the hook libraries are unpacked beside libhenge.so
 * (`extractNativeLibs="true"`). The command-line tool has no such directory of its own and names
 * one in `HV_VK_DRIVER` instead.
 */
static int own_library_dir(char *out, size_t n) {
    Dl_info info;
    if (!dladdr((void *)(uintptr_t)&henge_vk_open_adrenotools, &info) || !info.dli_fname) return 0;
    const char *slash = strrchr(info.dli_fname, '/');
    if (!slash) return 0;
    size_t len = (size_t)(slash - info.dli_fname);
    if (len + 1 > n) return 0;
    memcpy(out, info.dli_fname, len);
    out[len] = 0;
    return 1;
}

/** Whether `dir` holds the driver this shim opens. */
static int has_driver(const char *dir) {
    char path[1024];
    if (snprintf(path, sizeof path, "%s/libvulkan_freedreno.so", dir) >= (int)sizeof path) return 0;
    return access(path, F_OK) == 0;
}

/** Opens the driver named by `driver_dir` through libadrenotools' namespace. */
void *henge_vk_open_adrenotools(const char *driver_dir) {
    char own[1024];
    if (!driver_dir || !*driver_dir) {
        if (!own_library_dir(own, sizeof own) || !has_driver(own)) {
            snprintf(vk_loader_error, sizeof vk_loader_error,
                     "no driver directory given, and none beside this library");
            return NULL;
        }
        driver_dir = own;
    }
    /* The directory must end with a slash: libadrenotools stats
     * `customDriverDir + customDriverName` and returns NULL silently without it (measured). */
    char dir[1024];
    size_t n = strlen(driver_dir);
    if (n + 2 > sizeof dir) {
        snprintf(vk_loader_error, sizeof vk_loader_error, "driver directory too long: %s", driver_dir);
        return NULL;
    }
    memcpy(dir, driver_dir, n);
    if (dir[n - 1] != '/') dir[n++] = '/';
    dir[n] = 0;
    /* The hook libraries and the driver live in the same directory here: the CLI has no
     * nativeLibraryDir of its own. */
    void *loader = adrenotools_open_libvulkan(0, ADRENOTOOLS_DRIVER_CUSTOM, dir, dir, dir,
                                              ADRENOTOOLS_DRIVER_NAME, NULL, NULL);
    if (!loader) {
        snprintf(vk_loader_error, sizeof vk_loader_error,
                 "adrenotools could not open %s%s", dir, ADRENOTOOLS_DRIVER_NAME);
    }
    return loader;
}

/** A symbol of an opened loader (or of any handle), or NULL. */
void *henge_vk_symbol(void *handle, const char *name) {
    return handle ? dlsym(handle, name) : NULL;
}

/** Why the last [`henge_vk_open_adrenotools`] failed. */
const char *henge_vk_loader_error(void) { return vk_loader_error; }
#else
void *henge_vk_open_adrenotools(const char *driver_dir) { (void)driver_dir; return NULL; }
void *henge_vk_symbol(void *handle, const char *name) { (void)handle; (void)name; return NULL; }
const char *henge_vk_loader_error(void) { return "not built for Android"; }
#endif
