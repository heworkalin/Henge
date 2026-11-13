/*
 * What this device's Vulkan actually offers.
 *
 * The W^X probe answers "can the engine JIT here". This answers the other
 * half of Stage 0: "can reims-vgpu's Vulkan platform run here" — before
 * spending time cross-compiling a 466k-line Rust library to find out.
 *
 * The technique is lifted from MobileGlues (FCL-Team), which does exactly this
 * at startup: `config/gpu_utils.cpp` dlopen()s libvulkan.so, creates an instance
 * declaring VK_API_VERSION_1_3, enumerates the physical devices and reads
 * VkPhysicalDeviceProperties, then decides whether to take its Vulkan path at
 * all. Its own bar is `deviceProperties.apiVersion >= VK_API_VERSION_1_2`, and
 * it refuses that path outright on Adreno 730/740 — `checkIfANGLESupported()`
 * returns false for them. That refusal is the single most useful piece of
 * evidence in this project for how much to trust a vendor Vulkan driver.
 *
 * What is reported, and why:
 *
 *   apiVersion / driverVersion   the loader and driver levels present
 *   device name                  which GPU family, and whether it is one the
 *                                ecosystem already distrusts
 *   the 1.1/1.2/1.3 feature bits metal2vulkan-class consumers need, read from
 *                                a Features2 pNext chain rather than guessed at
 *                                from the version number, because a driver can
 *                                advertise 1.3 and still report false
 *   memory heaps                 a 4-8 GB guest needs a large device-local heap
 *   driver source                which library produced all of the above
 *
 * The driver can be named: pass a plugin directory and library name and that
 * library is opened instead of the system loader. Nothing about the device is
 * used to decide this — a fixed development environment means saying which
 * driver to use rather than guessing from a renderer string. When no plugin is
 * named, or the named one fails to load, the system loader is used and the
 * report says which of those happened.
 *
 * Everything is advisory: nothing here fails the probe. A device missing a
 * feature is a finding, and the run should say so rather than refuse to start.
 */

#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Everything goes through dlopen/dlsym, not a link-time dependency on
 * libvulkan.so. Two reasons, both real on devices at our minSdk of 26:
 *
 *   1. vkEnumerateInstanceVersion arrived with Vulkan 1.1. An Android 8.0 device
 *      can carry a 1.0 loader that does not export it, and a NEEDED entry would
 *      then make libhenge.so fail to load at all — the JIT, which needs nothing
 *      from Vulkan, would go down with the graphics probe.
 *   2. The engine has to start on a device with no Vulkan at all; the probe then
 *      reports that instead of the process dying at dlopen.
 *
 * This is the same choice MobileGlues makes in config/gpu_utils.cpp: open_lib()
 * with {"libvulkan.so"}, then dlsym each entry point and check it.
 */

#ifdef HENGE_HAVE_ADRENOTOOLS
/*
 * adrenotools, declared here rather than by including its header: this is a C
 * translation unit and the library is C++. Only one entry point is needed.
 *
 * What it does, and why a plain dlopen() is not enough — measured on the test
 * device:
 *
 *   dlopen("libvulkan_freedreno.so") ->
 *     library "libcutils.so" not found: needed by .../libvulkan_freedreno.so
 *     in namespace clns-7
 *
 * A GPU driver NEEDs vendor libraries (libcutils, libhardware, libsync,
 * libnativewindow) that no app's classloader namespace contains. adrenotools
 * creates a linker namespace whose parent is the caller's, links it to the
 * system libraries, installs a hook that catches the loader's own
 * android_dlopen_ext, and returns the *system* Vulkan loader running in that
 * namespace. The loader is then the one that resolves the driver, immediately
 * before enumerating physical devices.
 *
 * Arguments that matter here:
 *   featureFlags     ADRENOTOOLS_DRIVER_CUSTOM = 1 << 0
 *   tmpLibDir        writable dir for the renamed libvulkan.so copy (API < 29);
 *                    on API >= 29 adrenotools uses memfd and ignores it
 *   hookLibDir       where libmain_hook.so etc. live = this app's nativeLibraryDir
 *   customDriverDir  where the driver lives    = the same directory
 */
#define ADRENOTOOLS_DRIVER_CUSTOM 1
extern void *adrenotools_open_libvulkan(int dlopenMode, int featureFlags,
                                        const char *tmpLibDir, const char *hookLibDir,
                                        const char *customDriverDir, const char *customDriverName,
                                        const char *fileRedirectDir, void **userMappingHandle);
#endif

struct VkApi {
    PFN_vkEnumerateInstanceVersion enumerateInstanceVersion; /* may be NULL */
    PFN_vkCreateInstance createInstance;
    PFN_vkDestroyInstance destroyInstance;
    PFN_vkEnumeratePhysicalDevices enumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceProperties getPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceFeatures2 getPhysicalDeviceFeatures2;
    PFN_vkEnumerateDeviceExtensionProperties enumerateDeviceExtensionProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties getPhysicalDeviceMemoryProperties;
};

static struct VkApi vk;
static void *vk_lib;
static PFN_vkGetInstanceProcAddr vk_gipa;

/* Which library the handle came from, and how that choice went. Printed, not
 * inferred: the whole point of a fixed environment is being able to say which
 * driver produced these numbers. */
static const char *vk_source = "none";

/* Set when dlopen() succeeded but the library does not answer to the names a
 * Vulkan client has to have. That distinction matters: a driver can be present,
 * load cleanly, and still be unreachable, and "no usable loader" would hide it. */
static int vk_opened_but_empty;

/* Set when the driver was named but adrenotools declined to open it. The plain
 * dlopen fallback is then skipped on purpose: it is known to fail on the vendor
 * NEEDED libraries, and running it would only replace a specific message with a
 * generic one. */
static int vk_ns_failed;
static char vk_source_detail[1024];

/*
 * Resolve a Vulkan entry point the way the spec intends: ask the driver's own
 * vkGetInstanceProcAddr first, fall back to a direct dlsym.
 *
 * Order matters. A Mesa ICD such as Turnip exported for loader replacement does
 * expose vkGetInstanceProcAddr, but the loader-independent entry points are not
 * all exported as plain symbols, so a pure dlsym() pass leaves holes. The
 * instance/device level functions in particular must be asked for with a real
 * instance, which is why bind_instance() runs again after vkCreateInstance.
 */
#define VK_BIND(field, name)                                                        \
    do {                                                                            \
        vk.field = vk_gipa ? (PFN_##name)vk_gipa(VK_NULL_HANDLE, #name) : NULL;      \
        if (!vk.field) vk.field = (PFN_##name)dlsym(vk_lib, #name);                  \
    } while (0)

static int vk_api_open_from(const char *dir, const char *libname, const char *tmp_dir) {
    if (vk_lib) return 1;

    vk_source = "system";
#ifdef HENGE_HAVE_ADRENOTOOLS
    if (dir && *dir && libname && *libname && tmp_dir && *tmp_dir) {
        /* customDriverDir must end with a slash, undocumented but load-bearing:
         * driver.cpp validates the driver with
         *     stat((std::string(customDriverDir) + customDriverName).c_str(), &buf)
         * and returns nullptr silently when that fails. Without the slash it
         * stats ".../liblibvulkan_freedreno.so", which never exists — the symptom
         * is a bare NULL with no log line anywhere. Measured on device, not read
         * off the docs.
         *
         * Through the namespace: the only path that can resolve the driver's
         * vendor NEEDED libraries. Returns the system loader's handle, so the
         * name lookups below are the loader's, not the driver's. */
        char dir_with_slash[1024];
        snprintf(dir_with_slash, sizeof dir_with_slash, "%s%s", dir,
                 (dir[strlen(dir) - 1] == '/') ? "" : "/");
        vk_lib = adrenotools_open_libvulkan(0, ADRENOTOOLS_DRIVER_CUSTOM,
                                            tmp_dir, dir /* hookLibDir */,
                                            dir_with_slash /* customDriverDir */, libname,
                                            NULL, NULL);
        if (vk_lib) {
            vk_source = "plugin (adrenotools namespace)";
            snprintf(vk_source_detail, sizeof vk_source_detail, "%s/%s", dir, libname);
        } else {
            snprintf(vk_source_detail, sizeof vk_source_detail,
                     "adrenotools could not open %s/%s", dir, libname);
            vk_source = "system (plugin failed)";
            vk_ns_failed = 1;
        }
    } else
#endif
    if (dir && *dir && libname && *libname) {
        char path[PATH_MAX];
        snprintf(path, sizeof path, "%s/%s", dir, libname);
        vk_lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (vk_lib) {
            vk_source = "plugin";
            snprintf(vk_source_detail, sizeof vk_source_detail, "%.800s", path);
        } else {
            /* Record it, then fall through to the system loader rather than
             * reporting nothing. A plugin that fails to load is a finding. */
            snprintf(vk_source_detail, sizeof vk_source_detail,
                     "plugin NOT loaded (%s) — fell back to system", dlerror());
            vk_source = "system (plugin failed)";
        }
    }

    if (!vk_lib) {
        if (vk_ns_failed) {
            snprintf(vk_source_detail, sizeof vk_source_detail,
                     "%s/%s — see the adrenotools note in probe_vk.c", dir, libname);
            return 0;
        }
        vk_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
        if (!vk_lib) vk_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!vk_lib) {
            snprintf(vk_source_detail, sizeof vk_source_detail, "dlopen: %s", dlerror());
            vk_source = "none";
            return 0;
        }
        if (vk_source[0] == 's' && strcmp(vk_source, "system") == 0 && !vk_source_detail[0])
            snprintf(vk_source_detail, sizeof vk_source_detail, "libvulkan.so");
    }

    vk_gipa = (PFN_vkGetInstanceProcAddr)dlsym(vk_lib, "vkGetInstanceProcAddr");

    VK_BIND(createInstance, vkCreateInstance);
    VK_BIND(destroyInstance, vkDestroyInstance);
    VK_BIND(enumeratePhysicalDevices, vkEnumeratePhysicalDevices);
    VK_BIND(getPhysicalDeviceProperties, vkGetPhysicalDeviceProperties);
    VK_BIND(getPhysicalDeviceFeatures2, vkGetPhysicalDeviceFeatures2);
    VK_BIND(enumerateDeviceExtensionProperties, vkEnumerateDeviceExtensionProperties);
    VK_BIND(getPhysicalDeviceMemoryProperties, vkGetPhysicalDeviceMemoryProperties);
    VK_BIND(enumerateInstanceVersion, vkEnumerateInstanceVersion);

    vk_opened_but_empty = !vk.createInstance || !vk.enumeratePhysicalDevices;

    /* enumerateInstanceVersion is the only one allowed to be absent (Vulkan 1.0).
     * getPhysicalDeviceFeatures2 is core in 1.1 and may legitimately be missing
     * on a 1.0 driver; the caller degrades to reporting nothing rather than
     * treating that as a failed open. */
    return vk.createInstance && vk.destroyInstance && vk.enumeratePhysicalDevices &&
           vk.getPhysicalDeviceProperties && vk.enumerateDeviceExtensionProperties &&
           vk.getPhysicalDeviceMemoryProperties;
}

#undef VK_BIND

/* Instance-level commands: for an ICD these are only obtainable through the
 * instance, so re-resolve now that one exists. */
static void vk_api_bind_instance(VkInstance instance) {
    if (!vk_gipa) return;
#define VK_REBIND(field, name)                                                      \
    do {                                                                            \
        PFN_##name p = (PFN_##name)vk_gipa(instance, #name);                         \
        if (p) vk.field = p;                                                        \
    } while (0)
    VK_REBIND(enumeratePhysicalDevices, vkEnumeratePhysicalDevices);
    VK_REBIND(getPhysicalDeviceProperties, vkGetPhysicalDeviceProperties);
    VK_REBIND(getPhysicalDeviceFeatures2, vkGetPhysicalDeviceFeatures2);
    VK_REBIND(enumerateDeviceExtensionProperties, vkEnumerateDeviceExtensionProperties);
    VK_REBIND(getPhysicalDeviceMemoryProperties, vkGetPhysicalDeviceMemoryProperties);
#undef VK_REBIND
}

/* Room for the device name, feature lines and the full extension list. */
#define CAP_BYTES (24 * 1024)

static void cat(char **cursor, char *end, const char *fmt, ...) {
    if (*cursor >= end) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*cursor, (size_t)(end - *cursor), fmt, ap);
    va_end(ap);
    if (n > 0) *cursor += n;
    if (*cursor > end) *cursor = end;
}

/* yes/no for a VkBool32, right-aligned so a column of them is readable. */
static const char *yesno(VkBool32 v) { return v ? "yes" : "NO"; }

/* Does the device list this extension? Linear scan: the list is small (a few
 * hundred at most) and this runs once. */
static int has_ext(const VkExtensionProperties *props, uint32_t count, const char *name) {
    for (uint32_t i = 0; i < count; i++)
        if (strcmp(props[i].extensionName, name) == 0) return 1;
    return 0;
}

static void report_device(char **cursor, char *end, VkPhysicalDevice dev, uint32_t index) {
    VkPhysicalDeviceProperties props;
    vk.getPhysicalDeviceProperties(dev, &props);

    cat(cursor, end, "\n  --- device %u ---\n", index);
    cat(cursor, end, "  name             : %s\n", props.deviceName);
    cat(cursor, end, "  type             : %s\n",
        props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete"
        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU          ? "cpu"
                                                                   : "other");
    cat(cursor, end, "  apiVersion       : %u.%u.%u\n", VK_VERSION_MAJOR(props.apiVersion),
        VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
    cat(cursor, end, "  driverVersion    : 0x%08x\n", props.driverVersion);
    cat(cursor, end, "  vendorID/deviceID: 0x%04x / 0x%04x\n", props.vendorID, props.deviceID);

    /* Features2 with the 1.1/1.2/1.3 structs chained: read the bits, do not infer
     * them from apiVersion. A driver may report 1.3 while leaving a feature off. */
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                            .pNext = &f11};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                            .pNext = &f12};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13};
    vk.getPhysicalDeviceFeatures2(dev, &f2);

    cat(cursor, end, "\n  features metal2vulkan-class consumers want:\n");
    cat(cursor, end, "    storageBuffer16BitAccess   %s   (1.1)\n", yesno(f11.storageBuffer16BitAccess));
    cat(cursor, end, "    shaderFloat16 / Int8       %s / %s   (1.2)\n", yesno(f12.shaderFloat16), yesno(f12.shaderInt8));
    cat(cursor, end, "    bufferDeviceAddress        %s   (1.2)\n", yesno(f12.bufferDeviceAddress));
    cat(cursor, end, "    timelineSemaphore          %s   (1.2)\n", yesno(f12.timelineSemaphore));
    cat(cursor, end, "    descriptorIndexing         %s   (1.2)\n", yesno(f12.descriptorIndexing));
    cat(cursor, end, "    runtimeDescriptorArray     %s   (1.2)\n", yesno(f12.runtimeDescriptorArray));
    cat(cursor, end, "    hostQueryReset             %s   (1.2)\n", yesno(f12.hostQueryReset));
    cat(cursor, end, "    scalarBlockLayout          %s   (1.2)\n", yesno(f12.scalarBlockLayout));
    cat(cursor, end, "    dynamicRendering           %s   (1.3)\n", yesno(f13.dynamicRendering));
    cat(cursor, end, "    synchronization2           %s   (1.3)\n", yesno(f13.synchronization2));
    cat(cursor, end, "    maintenance4               %s   (1.3)\n", yesno(f13.maintenance4));

    /* Extensions that matter for a Metal-shaped API on Vulkan, and for getting
     * guest memory (host imports) into the GPU. */
    uint32_t ext_count = 0;
    VkExtensionProperties *exts = NULL;
    if (vk.enumerateDeviceExtensionProperties(dev, NULL, &ext_count, NULL) == VK_SUCCESS && ext_count) {
        exts = (VkExtensionProperties *)malloc(sizeof(VkExtensionProperties) * ext_count);
        if (exts && vk.enumerateDeviceExtensionProperties(dev, NULL, &ext_count, exts) != VK_SUCCESS) {
            free(exts);
            exts = NULL;
        }
    }

    if (exts) {
        static const struct {
            const char *name;
            const char *why;
        } wanted[] = {
            {"VK_KHR_swapchain", "present"},
            {"VK_KHR_timeline_semaphore", "Metal event/shared event"},
            {"VK_EXT_descriptor_indexing", "argument buffers"},
            {"VK_KHR_buffer_device_address", "Metal gpuAddress"},
            {"VK_KHR_dynamic_rendering", "Metal render passes"},
            {"VK_KHR_synchronization2", "Metal barriers"},
            {"VK_KHR_16bit_storage", "half in buffers"},
            {"VK_KHR_8bit_storage", "char in buffers"},
            {"VK_EXT_robustness2", "null descriptors"},
            {"VK_EXT_host_image_copy", "cheap uploads"},
            {"VK_EXT_external_memory_dma_buf", "guest memory import"},
            {"VK_EXT_image_drm_format_modifier", "tiled import"},
            {"VK_ANDROID_external_memory_android_hardware_buffer", "AHardwareBuffer"},
            {"VK_KHR_image_format_list", "format views"},
        };
        cat(cursor, end, "\n  extensions (%u total):\n", ext_count);
        for (size_t i = 0; i < sizeof wanted / sizeof wanted[0]; i++) {
            cat(cursor, end, "    %-52s %s   (%s)\n", wanted[i].name,
                has_ext(exts, ext_count, wanted[i].name) ? "yes" : "NO", wanted[i].why);
        }
        /* The full list, so nothing has to be guessed at later. */
        cat(cursor, end, "\n  all extensions:\n");
        for (uint32_t i = 0; i < ext_count; i++) cat(cursor, end, "    %s\n", exts[i].extensionName);
        free(exts);
    } else {
        cat(cursor, end, "\n  extensions: (none reported)\n");
    }

    VkPhysicalDeviceMemoryProperties mem;
    vk.getPhysicalDeviceMemoryProperties(dev, &mem);
    cat(cursor, end, "\n  memory heaps:\n");
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++) {
        cat(cursor, end, "    heap %u: %llu MiB %s\n", i,
            (unsigned long long)(mem.memoryHeaps[i].size >> 20),
            (mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? "(device local)" : "");
    }
}

/*
 * Appends the capability section to a report the caller has already started.
 * `report` is the buffer start; the section begins after whatever is in it, so
 * the probe keeps one string and one copy button.
 *
 * Advisory throughout: a device that cannot be opened still produces text saying
 * why, because "Vulkan is absent" is a finding too.
 */
void hv_probe_vulkan_capabilities(char *report, size_t cap,
                                  const char *driver_dir, const char *driver_lib,
                                  const char *tmp_dir) {
    char *cursor = report + strlen(report);
    char *end = report + cap;

    int opened = vk_api_open_from(driver_dir, driver_lib, tmp_dir);

    cat(&cursor, end, "\n--- Vulkan ---\n");
    cat(&cursor, end, "  driver source : %s\n", vk_source);
    if (vk_source_detail[0]) cat(&cursor, end, "                  %s\n", vk_source_detail);

    if (!opened) {
        if (vk_lib && vk_opened_but_empty) {
            /* The library loaded but no entry point came out of it. Report what
             * was asked for and what answered, because this is the shape a
             * "sealed" driver has: dlopen succeeds, every name is NULL, and
             * without this line the failure looks like "Vulkan is missing". */
            cat(&cursor, end, "  dlopen        : OK, but NO Vulkan entry points\n");
            cat(&cursor, end,
                "  dlsym         : vkGetInstanceProcAddr=%s vkCreateInstance=%s\n"
                "                  vkEnumeratePhysicalDevices=%s vkGetPhysicalDeviceProperties=%s\n",
                vk_gipa ? "yes" : "NULL", vk.createInstance ? "yes" : "NULL",
                vk.enumeratePhysicalDevices ? "yes" : "NULL",
                vk.getPhysicalDeviceProperties ? "yes" : "NULL");
            cat(&cursor, end,
                "  reading       : a driver with no exported entry points cannot be used\n"
                "                  by dlsym, and the system loader cannot use it either\n"
                "                  (it asks for vk_icdGetInstanceProcAddr the same way)\n");
        } else {
            cat(&cursor, end, "  libvulkan.so  : NOT AVAILABLE (no usable loader)\n"
                              "  Stage 2 note  : the Vulkan path cannot run on this device\n");
        }
        return;
    }

    uint32_t loader_version = 0;
    if (vk.enumerateInstanceVersion && vk.enumerateInstanceVersion(&loader_version) == VK_SUCCESS) {
        cat(&cursor, end, "\n--- Vulkan loader ---\n  apiVersion : %u.%u.%u\n",
            VK_VERSION_MAJOR(loader_version), VK_VERSION_MINOR(loader_version),
            VK_VERSION_PATCH(loader_version));
    } else {
        cat(&cursor, end, "\n--- Vulkan loader ---\n  apiVersion : 1.0 (vkEnumerateInstanceVersion absent)\n");
    }

    /* Ask for 1.3: a loader that only knows 1.1 refuses, which is itself the
     * answer. The instance version requested does not restrict what the devices
     * may report. */
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "Henge probe",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app};
    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vk.createInstance(&ci, NULL, &instance);
    if (r == VK_SUCCESS) vk_api_bind_instance(instance);
    if (r != VK_SUCCESS) {
        cat(&cursor, end, "  vkCreateInstance(1.3) : FAILED (%d)\n", (int)r);
        return;
    }

    uint32_t count = 0;
    if (vk.enumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS || count == 0) {
        cat(&cursor, end, "  physical devices      : NONE\n");
        vk.destroyInstance(instance, NULL);
        return;
    }
    cat(&cursor, end, "  physical devices      : %u\n", count);

    VkPhysicalDevice *devs = (VkPhysicalDevice *)malloc(sizeof(VkPhysicalDevice) * count);
    if (!devs) {
        vk.destroyInstance(instance, NULL);
        return;
    }
    if (vk.enumeratePhysicalDevices(instance, &count, devs) == VK_SUCCESS)
        for (uint32_t i = 0; i < count; i++) report_device(&cursor, end, devs[i], i);

    free(devs);
    vk.destroyInstance(instance, NULL);
}
