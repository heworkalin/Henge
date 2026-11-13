# Android build environment

Cross-compilation setup for Henge on Android arm64. Two toolchains, both
verified working on this host:

| | toolchain | status |
|---|---|---|
| C engine (`core/`, `devices/`, `host/`) | NDK 29 `linux-arm64` clang | ✅ `libhenge.so` builds |
| Rust (`reims-vgpu`, Stage 2) | `aarch64-linux-android` via rustc 1.98.1 | ✅ links |

See [`../plan.md`](../plan.md) for why the graphics path goes through
`reims-vgpu` rather than a hand-written Metal→Vulkan translator.

---

## Quick start

```sh
./android/build-apk.sh                  # libhenge.so → app-debug.apk, one command

./android/build-c.sh                    # just the native library

source android/env.sh                   # CC / CXX / AR + cargo target linker
cargo build --target aarch64-linux-android --release
```

Requirements detected on this machine:

```
ANDROID_NDK_HOME=/home/he/Android/Sdk/ndk/29.0.14206865   (r29)
host tag          linux-arm64                            (native, no x86_64 emulation)
JDK               21.0.12
Gradle            8.13 (wrapper; cached locally)
AGP               8.13.2 (cached locally)
aapt2             build-tools/36.1.0 — an aarch64 binary
rustc             1.98.1                                 (reims-vgpu pins 1.98.1)
rust target       aarch64-linux-android                   (installed)
cmake 3.28 / ninja 1.11 / make 4.3                       (system, aarch64)
```

Target defaults: ABI `arm64-v8a`, API `33`. Override per invocation:

```sh
ABI=armeabi-v7a API=24 ./android/build-c.sh
API=26 source android/env.sh
TARGET_SDK=28 ./android/build-apk.sh    # the low-targetSdk variant
```

API 33 is the floor chosen for `Vulkan 1.3` availability and `minSdk` sanity;
`plan.md` §4 has the 16 KB-page discussion, which wants API 35 devices.

---

## Layout

```
android/
├── CMakeLists.txt            the build; documents what is and is not built
├── build-c.sh                C side, one command
├── env.sh                    both toolchains' env vars (source it)
├── app/src/main/jniLibs/     the embedded Vulkan driver (fetched, not committed)
├── driver-plugin/            optional: FCL-contract plugin APK, off by default
├── fetch-turnip.sh           fetch + pin the driver, SHA-256 in PROVENANCE.txt
├── probe_vk.c                the Vulkan capability + driver-source section
├── compat/                   Apple Mach → bionic shims
│   ├── include/
│   │   ├── mach/mach.h           types, mach_task_self, thread_info
│   │   ├── mach/mach_time.h      mach_absolute_time @ 24 MHz
│   │   ├── mach/thread_info.h    thread_basic_info_data_t
│   │   ├── mach/vm_map.h         vm_allocate → anonymous mmap
│   │   ├── mach/mach_error.h
│   │   ├── os/lock.h             os_unfair_lock → pthread_mutex_t
│   │   ├── libkern/OSCacheControl.h  sys_icache_invalidate → __clear_cache
│   │   ├── CommonCrypto/CommonDigest.h    CC_SHA256
│   │   ├── CommonCrypto/CommonCryptor.h   CCCrypt (AES ECB/CBC)
│   │   ├── pthread/qos.h         QOS_CLASS_* → nice value
│   │   └── hv_darwin_compat.h    force-included: pthread_cond_timedwait_relative_np
│   ├── compat_android.c
│   └── commoncrypto_android.c
```

---

## Building the APK

```sh
./android/build-apk.sh                 # → android/app/build/outputs/apk/debug/app-debug.apk
TARGET_SDK=28 ./android/build-apk.sh   # the legacy-domain variant, for the W^X measurement
```

Output is ~185 KB: `classes.dex`, `classes2.dex`, and `lib/arm64-v8a/libhenge.so`
(291 KB of `.text` after AGP strips it), signed with the standard debug key.
`extractNativeLibs="true"`, so the libraries are compressed inside the APK and
written out to `nativeLibraryDir` at install time — see "Which Vulkan driver".

### Which Vulkan driver

**One APK.** The Vulkan driver (Turnip) is embedded in the app's own `jniLibs`
and opened by absolute path at runtime. Users do not install a second app.

```
android/app/src/main/jniLibs/arm64-v8a/libvulkan_freedreno.so   ← not committed
        fetched by android/fetch-turnip.sh, SHA-256 recorded in PROVENANCE.txt
```

Why the file is not called `libvulkan.so`: that name is what the platform loader
resolves, and shipping one inside an APK would hijack Vulkan for the whole
process by accident. The app opens the library explicitly instead, so the choice
appears in the report rather than hiding in a search order.

`extractNativeLibs="true"` exists for this. With extraction off, the driver could
only be reached through a `base.apk!/lib/arm64-v8a/...` path, which depends on
the zip entry being uncompressed and aligned; extracted, it is an ordinary file.
It also makes an embedded driver and a driver from a plugin APK behave
identically, which is what the selection code assumes.

Selection order, and the report states which one produced the numbers:

| | driver | source |
|---|---|---|
| 1 | embedded Turnip | `nativeLibraryDir/libvulkan_freedreno.so` |
| 2 | installed plugin APKs | any package declaring the FCL contract |
| 3 | system loader | `dlopen("libvulkan.so")` |

A named driver that fails to open is reported and then the system loader is used,
rather than the probe coming back empty:

```
--- Vulkan ---
  driver source : system (plugin failed)
                  plugin NOT loaded (/nonexistent/libvulkan_freedreno.so: ...) — fell back to system
```

**Why bundle a driver at all**, when the device already has one: Vendor Vulkan
drivers are the variable in this project. Vulkan mandates almost nothing — a
driver can report `apiVersion 1.3` and still answer false for
`timelineSemaphore`, `bufferDeviceAddress` and `descriptorIndexing`. Bundling
Turnip makes the feature set a property of *this build* instead of a property of
the phone, which is what "a fixed development environment" means. The vendor
driver stays selectable as an option. Reasoning in `plan.md` §0.6.

`android/driver-plugin/` still exists and still implements the
FCL-Team/FCLDriverPlugin contract, but it is excluded from the default build: it
is a way to check that contract and to hand a driver to FoldCraftLauncher, not
something a user of this app ever installs.

### 用 libadrenotools 加载驱动（实测驱动装不进来的原因）

一开始的做法是「按绝对路径 `dlopen` 内置驱动」。设备实测直接否掉：

```
driver source : system (plugin failed)
plugin NOT loaded (dlopen failed: library "libcutils.so" not found:
  needed by /data/app/.../lib/arm64/libvulkan_freedreno.so in namespace clns-7)
```

GPU 驱动 `NEEDED` 的是厂商库（`libcutils` / `libhardware` / `libsync` /
`libnativewindow`），**这些不在 app 的 classloader namespace 里**。这不是权限问题，
是 linker namespace 隔离，换任何路径都绕不开。

解法只有一条：自己建一个命名空间并挂到系统库上。这件事 FCL 和 `libadrenotools`
做法一致（`private_create_namespace` / `android_create_namespace` +
`link_namespaces` + 拦截 `android_dlopen_ext` 的 hook）。这里用后者
（BSD-2-Clause，Billy Laws），因为它是被维护的、且生态里正在配合这些驱动使用。

```
android/third_party/libadrenotools/          BSD-2-Clause
    lib/linkernsbypass/                      BSD-2-Clause（git 子模块）
```

构建会额外产出四个库，它们必须是**文件**形式（`extractNativeLibs="true"`，
libadrenotools 文档里也明确要求这一点）：

| 库 | 作用 |
|---|---|
| `libmain_hook.so` | 替换 `android_dlopen_ext`，即真正拦住 loader 的那一层 |
| `libhook_impl.so` | hook 实现与参数传递 |
| `libfile_redirect_hook.so` | 驱动读文件重定向（可选特性） |
| `libgsl_alloc_hook.so` | GPU 内存映射导入（可选特性） |

调用点只有一处：

```c
vk_lib = adrenotools_open_libvulkan(0, ADRENOTOOLS_DRIVER_CUSTOM,
             tmpDir /* 可写的 loader 副本目录，API>=29 用 memfd 忽略 */,
             nativeLibraryDir /* hook 库所在 */,
             nativeLibraryDir /* 驱动所在 */, "libvulkan_freedreno.so",
             NULL, NULL);
```

返回的是**系统 Vulkan loader** 的 handle（不是驱动），所以后续 `dlsym` 拿到的是
loader 的入口点，驱动由 loader 在枚举物理设备前一刻解析。

实测结果（PJE110 / SM8550 / Adreno 740 / Android 15）：

```
--- Vulkan ---
  driver source : plugin (adrenotools namespace)
  --- device 0 ---
  name             : Turnip Adreno (TM) 740
  apiVersion       : 1.3.296         (system driver: 1.3.128)
  extensions       : 145             (system driver: 114)
```

三个系统驱动缺的扩展，Turnip 都有：`VK_EXT_external_memory_dma_buf`、
`VK_EXT_host_image_copy`、`VK_EXT_image_drm_format_modifier`。第一条正是
reims-vgpu 的 `os/linux/` 导入 guest 内存用的，等于消掉了一个移植工作项。

**调用坑（必须记）**：`customDriverDir` 要以 `/` 结尾。其内部校验是

```c
stat((std::string(customDriverDir) + customDriverName).c_str(), &buf)
```

没有斜杠时会 stat 成 `.../liblibvulkan_freedreno.so`，然后**静默**
`return nullptr`，全程不打印任何日志。我们就是这样白跑了一轮。

代价一条，如实记：

> `libhenge.so` 从 1.78 MB 增到 4.26 MB，因为 adrenotools 是 C++，且
> `-Wl,--gc-sections` 被刻意关掉（见陷阱 1），完整 libc++ 被静态链入。
> 换来的好处是不需要额外打包 `libc++_shared.so`、不依赖加载顺序。

### What the reference project taught, and where this deviates

`/home/he/AlinOs-for-android/app_proot` is the reference for what an aarch64 host
needs. Taken from it:

| setting | why |
|---|---|
| `android.aapt2FromMavenOverride` → `build-tools/36.1.0/aapt2` | AGP fetches an **x86-64** aapt2 from Maven, which cannot run here. 36.1.0 and 37.0.0 are the aarch64 builds (30.0.3 / 34.x / 35.0.0 are x86-64 — verified with `file`). |
| Gradle 8.13 + JDK 21 | the versions cached on this machine |
| Aliyun mirrors first in `settings.gradle` | this host reaches Google/Maven Central slowly |
| `org.gradle.workers.max=4` | fewer parallel workers, less I/O contention |

Deliberately **not** taken:

| reference setting | why not |
|---|---|
| `plugins { id 'com.android.application' version '8.13.1' }` | the plugins-DSL form resolves a *plugin marker* artifact that is not in the local cache, so `--offline` cannot start. `buildscript { classpath 'com.android.tools.build:gradle:8.13.2' }` resolves the real artifact, which is cached. |
| `cmake.dir` → local aarch64 CMake | `$ANDROID_HOME/cmake/3.22.1/bin/cmake` is an **x86-64** binary and cannot run. Rather than hunt for aarch64 CMake, the native library is built entirely outside Gradle by `build-c.sh`, and `app/build.gradle` points `jniLibs.srcDirs` at its output. One toolchain, one command, no CMake inside Gradle to keep in step. |
| `useLegacyPackaging true` | that exists in the reference because proot/tar/loader are renamed ELF files executed from `nativeLibraryDir`. `libhenge.so` is a real library loaded with `System.loadLibrary`, so it stays compressed inside the APK and `extractNativeLibs="false"`. |

### The probe

`app` is not a shell yet — it is the instrument that answers the one question that
cannot be answered anywhere else: **which shape of executable memory does a
normally-installed app actually get?** (plan.md §4, `jit_region.c`.)

`probe_jni.c` measures three things independently, each by writing a real function
and calling it — `mmap` returning non-NULL is not the question:

1. `memfd_create` → two `MAP_SHARED` mappings, R-X to run and R-W to write
2. one anonymous W+X mapping
3. whatever `hv_jit_region_open()` itself returns (`rw_diff` says which shape)

It also prints the app's SELinux domain and device API level, so the result can be
attributed rather than just recorded.

The report is made easy to quote, because the numbers only matter if they can be
pasted somewhere. Four ways out:

| how | what |
|---|---|
| screen | monospace, selectable, scrollable |
| **复制** button | whole report to the clipboard |
| **分享** button | `ACTION_SEND` to any text target |
| file | `getExternalFilesDir()/probe.txt`, no permission needed |
| logcat | one line per report line: `adb logcat -s HENGEPROBE` |

```sh
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb logcat -s HENGEPROBE
adb pull /sdcard/Android/data/dev.henge.probe/files/probe.txt
```

The report ends with `--- end of report ---` so "did it finish?" is never a guess.

#### Measured result #1 — API 35 device, targetSdk 36, 4 KiB pages

```
  page size        : 4096
  SELinux domain   : u:r:untrusted_app:s0:c85,c257,c512,c768
  device API level : 35
  requested bytes  : 1048576

  memfd split (R-X + R-W)      WORKS    call=0x1234
  anon W+X                     WORKS    call=0x1234

  hv_jit_region_open()         memfd split (rw_diff=-1048576)

VERDICT: split works — keep it. Policy-clean, no W+X at any point.
--- end of report ---
```

`call=0x1234` means the CPU actually fetched and ran the bytes — not merely that
`mmap` returned non-NULL. Both shapes are granted, and the engine picks the memfd
split as designed.

Four facts worth keeping from this run:

**1. The W^X risk is closed for this device class.** The tightening in Android 10
applies to *files* (executing writable files in app data dirs), not to anonymous
`execmem`, which every app with a JIT needs and which stays granted.

**2. The domain is `untrusted_app`, with no version suffix.** Compare the development
host, which runs in `untrusted_app_27` because the app that hosts it targets
SDK 27. The rule this makes visible: **versioned domains are for legacy targets
only; every targetSdk ≥ 29 lands in the unsuffixed `untrusted_app`.** So lowering
`targetSdk` does change the domain — and it would have changed nothing useful
here, because both shapes are already allowed in the modern one. `TARGET_SDK=28`
stays as a build knob, not as a workaround.

**3. `device API level : 35` is the *OS*, not the target — Android 15.** `targetSdk
36` is what the APK declares. Edge-to-edge enforcement starts at API 35 for
targetSdk 35+, which is why trap 7 below hit on this device.

**4. Page size is 4096.** This is a 4 KiB device, so it does not benefit from
Android 15's 16 KiB support — and PVG's `APV_PAGE_SIZE` is 0x4000 (16 KiB), so the
software page-mapping layer discussed in plan.md §4 is mandatory for it. **No 16 KiB
device has been tested yet**, and that is the remaining unknown in this area.

The memfd split stays the default for a different reason than "it is allowed": it
never has a W+X mapping at any instant, which is what hardened ROMs and
tamper-detection care about. The W+X path remains as a fallback for a ROM that
denies `execmem`.

#### Measured result #2 — the same run's Vulkan section

**The capability section is diagnostics, not a gate.** Vulkan has almost no
mandatory features — a driver may report `apiVersion 1.3` and still answer false
for `timelineSemaphore`, `bufferDeviceAddress` and `descriptorIndexing`, all of
which are optional in 1.2. So "this device has it" does not imply "the user's
does", and per-device testing is sampling, not design.

The architecture that follows from that is in `plan.md` §0.6: a universal subset,
a documented degradation ladder, and — on Adreno — shipping our own Vulkan driver
so the feature set stops varying per OEM. This report's job is to say which rung
of the ladder a machine can reach, and to attribute a bug to a missing bit after
the fact.

It is gathered with MobileGlues' technique (`config/gpu_utils.cpp`: dlopen
`libvulkan.so`, create an instance, enumerate, read properties) rather than a
link-time dependency.

On the Adreno 740 test device: **Vulkan 1.3.128**, 114 device extensions, and
every feature metal2vulkan-class code needs reported `yes` — `bufferDeviceAddress`,
`timelineSemaphore`, `descriptorIndexing`, `runtimeDescriptorArray`,
`dynamicRendering`, `synchronization2`, `shaderFloat16/Int8`,
`storageBuffer16BitAccess`, `hostQueryReset`, `scalarBlockLayout`, `maintenance4`.
`VK_KHR_sampler_ycbcr_conversion` and
`VK_ANDROID_external_memory_android_hardware_buffer` are present too.

Three extensions are absent, with different consequences:

| absent | consequence | workaround |
|---|---|---|
| `VK_EXT_external_memory_dma_buf` | reims-vgpu's `os/linux/` uses it to import guest memory | `AHardwareBuffer` (present) or plain buffers plus copies — **a concrete porting item** |
| `VK_EXT_host_image_copy` | upload convenience only | staging buffers |
| `VK_EXT_image_drm_format_modifier` | tiled imports; Android uses AHB anyway | none needed |

The tension worth knowing: this is the same Adreno 740 that FCL's
`checkIfANGLESupported()` refuses to run Vulkan on. That refusal is about **ANGLE**
(GLES-on-Vulkan), not about Vulkan itself — starling runs wgpu→Vulkan on Adreno
phones, and UniXNU's metal2vulkan frontend was developed on A740. What has *not*
been tried is metal2vulkan against Qualcomm's **stock** driver; UniXNU paired it
with their own kernel driver. That is the remaining Stage 0 unknown.

Full numbers in `plan.md` §0.5.

---

## Traps found while getting this to build

Five of these produced a **build that succeeded while being wrong**, which is why
they are written down.

### 1. gc-sections silently emptied the engine

The NDK toolchain compiles with `-ffunction-sections -fdata-sections` and links
with `-Wl,--gc-sections`; this target is built with `-fvisibility=hidden`, so every
engine symbol was local and unreferenced. The linker had no roots and discarded the
whole engine: a **7.7 KB `.text`** whose only surviving `hv_*` symbols were the two
`hv_jit_region_*` the probe calls. The probe passed, the APK installed, and the
library did nothing.

Fixed by stripping `-Wl,--gc-sections` out of the linker-flag variables at
configure time — not by adding `--no-gc-sections`, because the *last* occurrence on
the command line wins and the toolchain's copy lands after the target's options.
`.text` is now 291 KB and 271 `hv_*` symbols survive.

The permanent fix is to export the VM's C API, which roots everything reachable
from it (plan.md Stage 4). Until then, keep the sections.

### 2. `host/enter.S` was missing entirely — masked by trap 1

`hv_enter`, `hv_exit`, `hv_fp_enter`, `hv_fp_leave` are defined in the AArch64
assembly file, which the iOS Makefile builds and this build did not. gc-sections
had been dropping every caller, so nothing complained. It is now in the build.

### 3. `host/enter.S` uses Mach-O symbol names

The file writes `.globl _hv_enter` — Mach-O's leading underscore. Assembler output
showed `_hv_enter` while C wanted `hv_enter`, so every symbol was undefined. Because
`.S` goes through the C preprocessor, a guarded name map at the top of the file fixes
it without touching a line of the assembly or affecting the iOS build.

### 4. `-include hv_darwin_compat.h` broke the assembler

The compat header was force-included into *every* language, so `enter.S` got
bionic's `<android/api-level.h>` through it and the assembler rejected the macros.
Now scoped with `$<COMPILE_LANGUAGE:C>`.

### 5. `__ulock_wait` / `__ulock_wake` are Darwin-only

`core/machine.c` parks a waiting vCPU on a 32-bit word with Darwin's ulock API.
That is Linux's futex, so `compat_android.c` implements both over
`SYS_futex` with `FUTEX_WAIT` / `FUTEX_WAKE`, converting microseconds to a relative
`timespec`. No change to `machine.c` was needed — it declares both itself.

### 6. `OS_UNFAIR_LOCK_INIT` is a compound literal, not a brace initialiser

`devices/pvg_lite.c` does `p->layer_lock = OS_UNFAIR_LOCK_INIT;` — an *assignment*,
which only compiles because Apple defines the macro as `((os_unfair_lock){0})`.
The shim now defines `((os_unfair_lock)PTHREAD_MUTEX_INITIALIZER)` so the same
assignment is valid in a declaration and at file scope.

### 7. Edge-to-edge hid the top of the report

First run on device showed the report starting at the first probe line: the four
header lines — page size, SELinux domain, device API level, requested bytes — were
missing, and the app title "Henge Probe" sat above them.

Not a formatting bug. `targetSdk 36` means Android 15+ **enforces edge-to-edge**:
the window is laid out behind the status bar whether the app asked or not, and
`windowOptOutEdgeToEdgeEnforcement` is ignored from API 36. With
`Theme.DeviceDefault` the action bar took the top strip and the first content lines
went under the status bar.

Fixed twice over, because both halves were wrong:

- `Theme.DeviceDefault.NoActionBar` — a one-word title is not worth a strip of
  screen on a diagnostic
- system-bar + cutout insets applied as root padding, which is a no-op on devices
  that still fit the decor and is the only correct answer on ones that do not

Worth remembering for the real app: **every** Activity in this project targets 35+
and must inset itself. `fitsSystemWindows` is not a substitute; it is ignored in
edge-to-edge windows in many cases.

The diagnostic that separates "the report is missing lines" from "the UI hid them"
is logcat: the Java side logs the report one line per `Log.i`, before any layout
exists.

### 8. `pvg_lite.c` needs the Metal replay to link

`devices/pvg_lite.c` is the PVG device model; `devices/pvg_metal.m` is the Metal
replay it calls into. Android has no Metal, and reims-vgpu *replaces* that half
rather than porting it — but `vmapple.c` calls `hv_pvg_lite_create()`
unconditionally, so the device model has to be in the library.

`compat/pvg_metal_stub.c` provides the ten `hv_pvg_metal_*` entry points: the GPU's
MMIO and FIFO are still emulated so the guest's driver talks to a real device, the
commands are parsed and discarded, and the first use prints one line saying so
instead of leaving a blank screen with no explanation. A frame request answers
"none" rather than handing back garbage that would look like a rendering bug.

---

## The one invariant worth stating twice

**`mach_absolute_time()` must keep returning 24 MHz ticks.**

The engine bakes that rate into arithmetic all over:

| where | literal |
|---|---|
| `core/sysreg.c` | `CNTFRQ 24000000ull`, `counter()` returns this to the guest as CNTPCT |
| `core/dispatch.c` | `24 * 50` = 50 µs; `24000` = 1 ms; `* 24000000ull` for seconds |
| `core/machine.c` | `dt < 240 / 2400 / 24000` scheduling buckets |
| `devices/pvg_lite.c` | `24000000ull / refresh` |

So the shim derives ticks from `CLOCK_MONOTONIC_RAW` (`compat_android.c`).
`CLOCK_MONOTONIC` would be NTP-slewed, and the guest would see its counter
stretch and step — `MONOTONIC_RAW` never is.

---

## What builds, and what does not

### C engine — `core/` 9/9 + `devices/` 20/20 + 4 host files ✔

`pvg_lite.c` is built, against `compat/pvg_metal_stub.c` rather than against
Metal: `devices/vmapple.c` calls `hv_pvg_lite_create()` unconditionally, so the
device model has to be in the library or nothing links. The stub parses the FIFO
and discards the commands (trap 8). `reims-vgpu` replaces this layer
(plan.md §0.1); until then it stays in the tree as the protocol reference.

Host files built today:

| file | what was needed |
|---|---|
| `disk.c` | `F_NOCACHE` → `fdatasync` + `posix_fadvise(POSIX_FADV_DONTNEED)` on the write path (Linux has no per-descriptor "don't cache" flag; `O_DIRECT` needs aligned buffers the guest's DMA does not give). `F_BARRIERFSYNC` → `fdatasync`. |
| `hvsparse.c` | `SEEK_DATA`/`SEEK_HOLE` are Linux-only names: added `_GNU_SOURCE` for glibc, and the *values* 3/4 for bionic, which declares neither. **Plus a real bug fix** — see below. |
| `jit_region.c` | `vm_remap` → one `memfd` mapped twice (R-X and R-W). See below. |
| `usb_host.c` | nothing, it was already portable. |

#### `hvsparse.c`: a silent-data-loss bug, not just a portability shim

The hole walk broke out of its loop on any `lseek(SEEK_DATA) < 0`. On a
filesystem without hole reporting (exFAT, FUSE, some SD-card stacks) that means
`EINVAL`, not "no more data" — the packer would have produced an **empty
archive from a full file** and reported success. Now `ENXIO` means done and any
other error flips to a plain whole-file walk, where the zero-chunk skip still
finds the holes, just less cheaply.

#### `jit_region.c`: memfd instead of `vm_remap`

macOS gives two aliases of the same pages with `vm_remap`. Android builds the
same shape from one `memfd_create` mapped twice:

```
fd = memfd_create("henge-jit", MFD_CLOEXEC); ftruncate(fd, bytes)
rx = mmap(NULL, bytes, PROT_READ|PROT_EXEC,  MAP_SHARED, fd, 0)   ← executed
rw = mmap(NULL, bytes, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)   ← written
region->rw_diff = rw - rx
```

Why this and not a W+X mapping: **no mapping is ever writable and executable at
the same time**, which is exactly what Android's W^X rules are written to
refuse. It is how ART's JIT code cache and Chromium's V8 do it. A W+X
anonymous mapping is still tried as a fallback.

Which shape actually succeeds depends on the app's SELinux domain and
`targetSdkVersion`, which the engine cannot decide for itself — so
`hv_jit_region_open()` probes in order (memfd split first, W+X second) and
`region->rw_diff` says which one it got, so a run can report it instead of
guessing.

> **This cannot be settled in the proot/glibc container this was developed in.**
> That environment can already execute arbitrary code, i.e. whatever exec
> restriction exists was bypassed before the engine ran, so a probe there proves
> nothing about a plain app. It needs a test APK, installed normally, logging
> which shape it got. See "targetSdkVersion" below.

### `targetSdkVersion` and the SELinux domain — measured, and not needed

The domain an app runs in *is* derived from its `targetSdkVersion` through
`seapp_contexts`, and this project now has both ends of that on record:

| targetSdk | domain |
|---|---|
| 27 (the app hosting the development proot) | `u:r:untrusted_app_27:s0:c104,…` |
| 36 (this probe) | `u:r:untrusted_app:s0:c85,c257,c512,c768` |

So versioned domains are a legacy-target mechanism only: **every targetSdk ≥ 29
lands in the unsuffixed `untrusted_app`.** Lowering `targetSdk` to 28 would move
the app into a versioned domain — and it would buy nothing here, because the
modern domain already grants both shapes.

That retires the hypothesis this section used to carry. `TARGET_SDK=28` remains
available as a build knob (`./android/build-apk.sh`), not as a workaround.

There are still reasons not to lean on a low `targetSdkVersion` for anything else:

- it switches on a pile of legacy behaviours (storage, services, notifications)
  which help in some places and bite in others
- the targetSdk → domain → permissions mapping differs per OEM and cannot be
  pinned down without the device's own policy
- Android 14+ refuses to install apps below `targetSdkVersion 23`; the floor moves

So: measure it in a test APK, log the result, and prefer the shape that does not
need it.

### One source change was required

`devices/virtio_balloon.c` used two Darwin-only primitives to hand guest pages
back to the OS. Now `#if defined(__ANDROID__) || defined(__linux__)`-guarded:

| Darwin | Linux | semantic difference |
|---|---|---|
| `fcntl(fd, F_PUNCHHOLE, &ph)` | `fallocate(fd, FALLOC_FL_PUNCH_HOLE \| FALLOC_FL_KEEP_SIZE, off, len)` | none — same hole, same file length |
| `MADV_FREE_REUSABLE` | `MADV_DONTNEED` | Linux drops eagerly instead of keeping-then-compressing. The guest is promised zeroed pages on inflate, so eagerly is never wrong, only less lazy. |
| `MADV_FREE_REUSE` | *(nothing)* | after `MADV_DONTNEED` there is no reuse state to leave |

---

## Stage 2: the Rust half

`reims-vgpu` is what Stage 2 integrates (plan.md §0.1). Its own requirements are
stricter than what has been proven here:

- it upstream-targets **x86_64 Linux (KVM)** and **aarch64 macOS** only —
  `aarch64-linux-android` has never been built
- its Vulkan platform needs `metal2vulkan-runtime`, which needs a fairly capable
  Vulkan driver (the upstream reference is an RTX 5080)
- its Linux OS services use `mmap` of QEMU memory-backend files and
  `/dev/udmabuf`; Android has neither in that form

That is what `plan.md` Stage 0 exists to test. This environment is the
prerequisite for running that test, not the answer to it.

---

## Verified commands

```sh
# C
$ ./android/build-c.sh
...
[100%] Linking C shared library lib/arm64-v8a/libhenge.so
--- NEEDED ---
  libandroid.so  liblog.so  libdl.so  libm.so  libc.so     # bionic only

# Rust
$ source android/env.sh && cargo build --target aarch64-linux-android --release
$ file target/aarch64-linux-android/release/librt.so
ELF 64-bit LSB shared object, ARM aarch64
```
