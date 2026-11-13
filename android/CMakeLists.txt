#
# Henge engine — Android arm64 build.
#
# Produces libhenge.so: the portable engine (core/ + devices/ + the portable
# host/ files) plus a Mach/CoreFoundation compatibility layer, as one shared
# library the Kotlin app loads.
#
# Build:
#   ./android/build-c.sh
# or by hand:
#   cmake -B build/android \
#         -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33 \
#         -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/android -j
#
# Status (verified against NDK 29.0.14206865, aarch64 host):
#   core/*.c       9/9 compile       ✔
#   devices/*.c   20/20 compile      ✔  (pvg_lite.c built against a stub, see below)
#   host/         4 files            see the list under "host" below
#

cmake_minimum_required(VERSION 3.22)
# CXX is enabled for libadrenotools: it is a C++ library, and the final link has to
# go through the C++ driver so libc++abi's symbols (operator new, exception
# typeinfo) are resolved. Declaring the language here rather than relying on the
# subdirectory's own project() keeps CMAKE_CXX_CREATE_SHARED_LIBRARY defined when
# henge itself is generated.
project(henge_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT ANDROID)
    message(FATAL_ERROR
        "Android-only project. Configure with "
        "-DCMAKE_TOOLCHAIN_FILE=<ndk>/build/cmake/android.toolchain.cmake")
endif()

set(HENGE_ROOT   ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(HENGE_COMPAT ${CMAKE_CURRENT_SOURCE_DIR}/compat)
set(MBEDTLS      ${HENGE_ROOT}/third_party/mbedtls)

# ---------------------------------------------------------------------------
# Machine layer compatibility
#
# The engine was written against Apple's Mach layer. compat/ reimplements the
# parts it reaches for, on bionic/Linux. See compat/include/*/ for the reasoning
# behind each one; the important invariant is that mach_absolute_time() keeps
# returning Apple's 24 MHz tick, because core/sysreg.c hands that counter to the
# guest as CNTPCT and core/dispatch.c uses it for every timeout.
# ---------------------------------------------------------------------------

set(COMPAT_SRC
    ${HENGE_COMPAT}/compat_android.c         # mach time, vm_*, thread_info, ulock
    ${HENGE_COMPAT}/commoncrypto_android.c   # CC_SHA256 / CCCrypt over mbedtls
    ${HENGE_COMPAT}/corefoundation_android.c # CFRetain / CFRelease as a token count
    ${HENGE_COMPAT}/pvg_metal_stub.c         # the Metal replay Android does not have
    ${CMAKE_CURRENT_SOURCE_DIR}/probe_jni.c  # JNI entry point for the probe APK
    ${CMAKE_CURRENT_SOURCE_DIR}/probe_vk.c   # what this device's Vulkan offers
    ${CMAKE_CURRENT_SOURCE_DIR}/probe_ns.c   # why the driver namespace load fails
    ${CMAKE_CURRENT_SOURCE_DIR}/henge_vm.c   # builds a machine from inside the app (bring-up)
    ${CMAKE_CURRENT_SOURCE_DIR}/install.c    # installing macOS from an IPSW
    ${CMAKE_CURRENT_SOURCE_DIR}/stderr_to_logcat.c  # the restore stack's own log
)

# mbedtls, for CommonCrypto. AES and SHA-256 only, with the two support files
# they call into. Vendored at third_party/mbedtls (3.6.2).
set(MBEDTLS_SRC
    ${MBEDTLS}/library/aes.c
    ${MBEDTLS}/library/aesce.c   # AArch64 crypto extensions, which aes.c dispatches to
    ${MBEDTLS}/library/sha256.c
    ${MBEDTLS}/library/platform_util.c
    ${MBEDTLS}/library/constant_time.c
)

# ---------------------------------------------------------------------------
# The engine
# ---------------------------------------------------------------------------

file(GLOB CORE_SRC CONFIGURE_DEPENDS ${HENGE_ROOT}/core/*.c)

file(GLOB DEVICE_SRC CONFIGURE_DEPENDS ${HENGE_ROOT}/devices/*.c)
# pvg_lite.c is the PVG device model. It is built for now because devices/vmapple.c
# calls hv_pvg_lite_create() unconditionally, so leaving it out is a link error.
# reims-vgpu (plan.md §0.1) replaces this device at Stage 2; until that wiring
# exists, the engine has to be complete. Its only Apple dependency is CFRetain /
# CFRelease on the display-layer handle, which compat/ supplies.

# host/: what is portable today. The rest is listed in the comment block below.
set(HOST_SRC
    ${HENGE_ROOT}/host/disk.c        # block device I/O (posix_fadvise for F_NOCACHE)
    ${HENGE_ROOT}/host/hvsparse.c    # sparse image pack/unpack (SEEK_DATA with a fallback)
    ${HENGE_ROOT}/host/jit_region.c  # executable memory: memfd split mapping
    ${HENGE_ROOT}/host/usb_host.c    # virtio USB host mapping (no real USB needed)
)

# The translator's entry and exit stubs: hv_enter, hv_exit, hv_helper_entry,
# hv_fp_enter/hv_fp_leave. Plain AArch64 assembly with no Darwin macros, so it
# assembles unchanged — but it is required, and leaving it out is invisible while
# gc-sections is on (see the note under "Keep every section" below) and a hard
# link error the moment it is off.
set(ASM_SRC ${HENGE_ROOT}/host/enter.S)

enable_language(ASM)

# Not built yet, and what each needs:
#
#   snapshot.c    <compression.h>  -> Apple libcompression; use zlib
#                 and Grand Central Dispatch (dispatch_apply + blocks): the NDK
#                 ships no BlocksRuntime, so this is a rewrite over pthreads,
#                 not a shim. Its mincore() skip is also unsound on Linux: a
#                 page that is not resident cannot be told apart from one zram
#                 swapped out, so use_mincore must be forced to 0.
#   share.c       <copyfile.h>     -> Apple copyfile(3); plain read/write loop
#   dns_proxy.c   <dns_sd.h>       -> Bonjour mDNS; Avahi or a stub
#   net_slirp.c   <libslirp.h>     -> libslirp is not vendored; add it
#   install.c     idevicerestore   -> vendored, needs its include path wired
#   libusb_shim.c third_party/libusb-1.0 -> vendored, needs its include path
#
#   vm.c          iOS host: GCD, CAMetalLayer, CoreFoundation   -> rewrite
#   main.c        Apple-silicon HVF host, mach thread state      -> irrelevant
#   prof_ios.c    os_signpost                                   -> drop or atrace
#   audio_mac.c   AudioUnit                                     -> audio_android.c
#   hvf_ref.c     Hypervisor.framework                          -> irrelevant

# ---------------------------------------------------------------------------
# Includes. compat/include comes first so <mach/*>, <os/lock.h>,
# <libkern/OSCacheControl.h>, <CommonCrypto/*> and <pthread/qos.h> resolve to
# the shims rather than failing outright.
# ---------------------------------------------------------------------------

include_directories(
    ${HENGE_COMPAT}/include
    ${MBEDTLS}/include
    ${HENGE_ROOT}/core
    ${HENGE_ROOT}/host
    ${HENGE_ROOT}/devices
)

# ---------------------------------------------------------------------------
# Target
# ---------------------------------------------------------------------------
#
# Keep every section.
#
# The NDK toolchain compiles with -ffunction-sections -fdata-sections and links
# with -Wl,--gc-sections, and this target is built with -fvisibility=hidden, so
# every engine symbol is local and nothing references it from outside. gc-sections
# therefore has no roots and discards the engine wholesale: the first build here
# produced a 7.7 KB .text whose only surviving hv_* symbols were the two
# hv_jit_region_* the JNI probe calls. The probe still passed and the APK looked
# right, while carrying an empty engine.
#
# The permanent fix is to export the VM's C API, which would then root everything
# reachable from it; that surface does not exist yet (plan.md Stage 4). Until it
# does, drop gc-sections. It is stripped out of the variables rather than added
# back as --no-gc-sections, because the last occurrence on the command line wins
# and the toolchain's copy lands after the target's options.
foreach(_hv_var
        CMAKE_SHARED_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS_DEBUG
        CMAKE_SHARED_LINKER_FLAGS_RELEASE CMAKE_SHARED_LINKER_FLAGS_RELWITHDEBINFO
        CMAKE_SHARED_LINKER_FLAGS_MINSIZEREL
        CMAKE_MODULE_LINKER_FLAGS CMAKE_MODULE_LINKER_FLAGS_DEBUG
        CMAKE_MODULE_LINKER_FLAGS_RELEASE CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO
        CMAKE_MODULE_LINKER_FLAGS_MINSIZEREL
        CMAKE_EXE_LINKER_FLAGS CMAKE_EXE_LINKER_FLAGS_DEBUG
        CMAKE_EXE_LINKER_FLAGS_RELEASE CMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO
        CMAKE_EXE_LINKER_FLAGS_MINSIZEREL)
    string(REPLACE "-Wl,--gc-sections" "" ${_hv_var} "${${_hv_var}}")
endforeach()


# lzfse, Apple's open-source decoder: LZFSE (the boot ROM's block) and LZVN (the pieces inside it).
set(LZFSE_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lzfse/lzfse_decode.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lzfse/lzfse_decode_base.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lzfse/lzfse_fse.c
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lzfse/lzvn_decode_base.c)

add_library(henge SHARED
    ${CORE_SRC}
    ${DEVICE_SRC}
    ${HOST_SRC}
    ${ASM_SRC}
    ${COMPAT_SRC}
    ${MBEDTLS_SRC}
    # The boot ROM has to be taken out of an Apple image before a machine can start, so its
    # unpacking lives with the engine rather than in Java: it needs lzfse and a hash check.
    ${CMAKE_CURRENT_SOURCE_DIR}/bootrom.c
    ${LZFSE_SRC}
)
target_include_directories(henge PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lzfse)

set_target_properties(henge PROPERTIES
    OUTPUT_NAME henge
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${ANDROID_ABI}
    # Hidden by default; probe_jni.c opts back in with JNIEXPORT, which carries
    # default visibility. So the probe is reachable via System.loadLibrary while
    # the engine's internals do not leak into the dynamic symbol table.
    C_VISIBILITY_PRESET hidden
)

target_compile_options(henge PRIVATE
    -O2
    -g
    -fPIC
    -fvisibility=hidden
    -mbranch-protection=standard
    -Wall
    -Wextra
    -Wno-unused-parameter
    # mbedtls 3.6 keeps the low-level AES/SHA API behind this, and its headers
    # warn about it otherwise.
    -Wno-deprecated-declarations
    # Darwin's <pthread.h> carries pthread_cond_timedwait_relative_np, which
    # core/dispatch.c calls; bionic does not declare it.
    #
    # C only: host/enter.S goes through the same target, and force-including a C
    # header into the assembler feeds it bionic's <android/api-level.h>, whose
    # macros the assembler rejects.
    $<$<COMPILE_LANGUAGE:C>:-include>
    $<$<COMPILE_LANGUAGE:C>:hv_darwin_compat.h>
)

target_link_libraries(henge
    android   # ANativeWindow for the display surface
    log
    dl
    m
)

# ---------------------------------------------------------------------------
# libadrenotools: loading a Vulkan driver that the app's own linker namespace
# cannot resolve.
#
# Measured on the test device: a plain dlopen() of libvulkan_freedreno.so fails
# with
#     library "libcutils.so" not found ... in namespace clns-7
# because the vendor libraries a GPU driver NEEDs (libcutils, libhardware,
# libsync, libnativewindow) are not in an app's classloader namespace. Creating
# a linker namespace and linking it to the system one is the only way around
# that, and libadrenotools is the maintained implementation of it (BSD-2-Clause,
# by Billy Laws) rather than something hand-rolled here.
#
# It builds four extra shared libraries that must be packaged as files next to
# the driver — main_hook.so is what the loader's dlopen lands on. That is why the
# app sets extractNativeLibs="true"; libadrenotools documents the same
# requirement.
set(ADRENOTOOLS ${HENGE_ROOT}/android/third_party/libadrenotools)
if(EXISTS ${ADRENOTOOLS}/CMakeLists.txt)
    set(CMAKE_CXX_STANDARD 17)
    add_subdirectory(${ADRENOTOOLS} ${CMAKE_BINARY_DIR}/third_party/adrenotools)

    # The hook libraries have to end up in the same directory the APK picks up as
    # jniLibs, because that directory is what gets passed as hookLibDir.
    foreach(_hook hook_impl main_hook file_redirect_hook gsl_alloc_hook)
        set_target_properties(${_hook} PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib/${ANDROID_ABI})
    endforeach()

    target_include_directories(henge PRIVATE ${ADRENOTOOLS}/include)
    # adrenotools is C++ (std::string, exceptions), so the C library needs a C++
    # runtime. Static, not shared: libc++_shared.so would have to be loaded before
    # any of this works, and this library is loaded by System.loadLibrary in a
    # process where nothing else has asked for it.
    target_link_libraries(henge adrenotools c++_static)
    # Link with the C++ driver rather than the C one: clang++ is what adds
    # libc++abi (operator new, __cxa_allocate_exception, exception typeinfo) after
    # libc++_static. With the C driver these stay undefined even though
    # libc++_static is named on the command line.
    set_target_properties(henge PROPERTIES LINKER_LANGUAGE CXX)
    target_compile_definitions(henge PRIVATE HENGE_HAVE_ADRENOTOOLS=1)
    message(STATUS "  adrenotools: yes (driver loading via linker namespace)")
else()
    message(STATUS "  adrenotools: no (third_party/libadrenotools missing) — the "
                   "driver will be dlopen()ed directly, which fails for a driver "
                   "that NEEDs vendor libraries")
endif()

# ---------------------------------------------------------------------------
# reims-vgpu: the paravirtual GPU, reimplemented as a host library.
#
# Built out of tree by cargo (android/third_party/reims-vgpu), because it is a
# 466k-line Rust workspace whose own build is the only thing that knows how to
# produce it:
#
#   cd android/third_party/reims-vgpu
#   cargo build --release --target aarch64-linux-android \
#         -p reims-vgpu-c --no-default-features --features vulkan
#
# The static library is what gets linked here: it makes libhenge.so self
# contained, and keeps the Rust library's own dependencies out of the APK.
set(REIMS_DIR ${HENGE_ROOT}/android/third_party/reims-vgpu)
set(REIMS_LIB ${REIMS_DIR}/target/aarch64-linux-android/release/libreims_vgpu_c.a)
set(REIMS_INC ${REIMS_DIR}/crates/reims-vgpu-c/include)
if(EXISTS ${REIMS_LIB})
    target_sources(henge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/reims_pvg.c)
    # android/ itself, so devices/*.c can include reims_pvg.h next to the file it names.
    target_include_directories(henge PRIVATE ${REIMS_INC} ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(henge ${REIMS_LIB} dl m)
    target_compile_definitions(henge PRIVATE HENGE_HAVE_REIMS=1)
    message(STATUS "  reims-vgpu: yes (libreims_vgpu_c.a)")
else()
    message(STATUS "  reims-vgpu: no (build it first, see the comment above) — "
                   "reims_pvg.c is left out and the GPU has no device model")
endif()

# ---------------------------------------------------------------------------
# The installer's libraries, built out of tree by android/build-restore.sh:
# libirecovery, usbmuxd, idevicerestore and what they need (libplist,
# libimobiledevice, libtatsu, libzip). They drive the guest's virtio USB port
# through host/libusb_shim.c, the same way they do on iOS.
#
# libcurl is NOT among them: Android has none, so android/curl_jni.c implements
# the subset the restore tools use by calling the app's own HTTPS
# (dev.henge.probe.Http), exactly as iOS implements it over NSURLSession.
set(RESTORE_LIB ${HENGE_ROOT}/build/android-${ANDROID_ABI}/lib/libhengerestore.a)
if(EXISTS ${RESTORE_LIB})
    target_sources(henge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/curl_jni.c)
    target_include_directories(henge PRIVATE
        ${HENGE_ROOT}/third_party/curl_include
        ${HENGE_ROOT}/third_party/libirecovery/include
        ${HENGE_ROOT}/third_party/idevicerestore/src
        ${HENGE_ROOT}/third_party/libzip/lib
        ${HENGE_ROOT}/host ${HENGE_ROOT}/devices ${HENGE_ROOT}/core)
    set(RESTORE_ZIP ${HENGE_ROOT}/build/android-${ANDROID_ABI}/restore/deps/libzip/lib/libzip.a)
    target_link_libraries(henge ${RESTORE_LIB} ${RESTORE_ZIP} z)
    target_compile_definitions(henge PRIVATE HENGE_HAVE_RESTORE=1)
    message(STATUS "  restore tools: yes (libhengerestore.a) — installing from an IPSW is available")
else()
    message(STATUS "  restore tools: no (run android/build-restore.sh) — installing from an IPSW is not available")
endif()

message(STATUS "henge → ${ANDROID_ABI}, android-${ANDROID_PLATFORM_LEVEL}")
list(LENGTH CORE_SRC n_core)
list(LENGTH DEVICE_SRC n_dev)
list(LENGTH HOST_SRC n_host)
message(STATUS "  sources: ${n_core} core, ${n_dev} devices, ${n_host} host, ${CMAKE_C_COMPILER}")
