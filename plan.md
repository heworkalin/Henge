# Henge → Android 移植计划

## 概述

Henge 是一个运行 macOS 的虚拟机模拟器（arm64 系统），其核心引擎（CPU、MMU、设备模型、JIT 翻译器等）为纯 C/C++ 代码，完全可移植。目标是**无需 root、完全在非特权用户态运行**的虚拟 macOS 环境。

> **两条硬约束（决定所有技术选型）：**
> 1. **非特权**：Android App 沙箱，无 root、无内核模块、无 MMIO、无内核驱动加载。
> 2. **用户态**：一切实现必须通过 Android 系统提供的公开 API（Vulkan / EGL / GLES / AAudio / JNI）完成。

---

## 0️⃣ 现状调研与参考项目定位

### 0.0 关键事实一：Henge 的 GPU 路径是 PVG，不是 Metal API

```
Henge（iOS 现状）
  guest macOS  ──►  Apple PVG 半虚拟显卡协议（设备级命令流，MMIO + FIFO）
                        │
                  devices/pvg_lite.c        解析 PVG 命令流
                        │
                  devices/pvg_metal.m       重放到 Metal API
                        │
                  Apple Metal.framework ──► AGX GPU
```

- **PVG（ParavirtualizedGraphics）** 是 Apple 给虚拟机用的私有设备协议，不是 Metal API。Henge 自己就是那个"虚拟显卡设备"。
- 所以 Android 上缺的不是"Metal 驱动"，而是 **「PVG 命令流 → Vulkan」** 这条链路。

### 0.1 关键事实二：PVG → Vulkan **已经有完整开源实现**

> **这是本计划最重要的发现。** 之前判断「PVG → Vulkan 没人做过，必须自己写」是**错误的**。

# ⭐ [`steelbrain/reims-vgpu`](https://github.com/steelbrain/reims-vgpu)

> *"reims-vgpu replaces that framework. It takes the guest's GPU command stream and runs it on Metal on a Mac, or on Vulkan on Linux through metal2vulkan."*

**完整重写 `ParavirtualizedGraphics.framework` 宿主侧的库**，不是薄壳。

| 项 | 值 |
|---|---|
| 规模 | **807 个 Rust 文件，约 466,000 行** |
| 许可 | `GPL-2.0-or-later OR LGPL-3.0-or-later` → 与 Henge 的 GPL-2.0 **兼容** |
| 状态 | Beta，README：「largely stabilized」 |
| 已验证 | arm64 macOS 13/14/15/26（M3 Max）；**x86-64 macOS 26 on Linux KVM（RTX 5080）** |
| 接口 | **纯 C API** `crates/reims-vgpu-c/include/reims_vgpu.h`（QEMU 的 `apple-gfx` 就是这样接的）→ Henge 可直接当 host |
| 未验证 | **aarch64 Linux / Android**（全新目标） |

#### 架构（与 Henge 的需求 1:1 对应）

```
reims-vgpu-core     ← 全部 PVG 行为，泛型于 P: Platform，不链任何 Apple 框架
  ├ reims-vgpu-metal    Platform over Metal / IOSurface / Mach VM
  └ reims-vgpu-vulkan   Platform over metal2vulkan-runtime   ← 关键路径
        os/macos/  和  os/linux/（host VM over mmap、IOSurface 替身）
reims-vgpu-window   ← AppKit / SDL3（SDL3 有 Android 后端）
reims-vgpu-video-*  ← VideoToolbox / VAAPI / NVDEC 视频解码
reims-vgpu-c        ← C API（cdylib + staticlib + rlib）
```

#### 三个直接改变计划的细节

1. **PVG 客户机 ABI 是公开文档化的。**
   `docs/testing.md` §6「Guest ABI reference」就是协议规格：MMIO 寄存器表（0x1000–0x1234）、
   binary version 协商（51 个合法版本及各自 feature flag）、FIFO wire 格式、task 页表。
   Henge 的 `pvg_lite.c` 一直在自己摸的东西，这里有成文规格。

2. **确认 16 KiB 页是 PVG 协议固有的。**
   ```
   Pages are 16 KiB (APV_PAGE_SIZE, 0x4000, guest/records.rs)
   ```
   不是 Henge 的实现选择，是协议本身。风险性质因此改变：不是「也许能改」，而是「必须处理」。

3. **Vulkan 路径是「快速形 + 精确形双轨」，成熟度很高。**
   `docs/accuracy-ledger.md` 逐条列出 Vulkan 相对 Metal 的每处精度/顺序妥协、代价、以及
   `REIMS_VGPU_EXACT=<field>` 精确开关（heap 压缩纹理、half 链式舍入、pipeline 编译延迟导致
   的 draw 丢弃等）。

### 0.2 因此：技术路线作废与重写

| 原判断 | 修正 |
|---|---|
| PVG → Vulkan 要自己写（最大工作量） | ❌ **不用写**，reims-vgpu 已完成 |
| AIR shader 要自己转（`AirNir`） | ❌ **不需要**，metal2vulkan 自己吃 AIR；reims-vgpu 自己的 shader 是仓库内预编译 metallib |
| Metal 语义映射自己摸（`AdrenoMetal`） | ⬇️ 降级为参考（与 reims-vgpu 共用同一 `metal2vulkan`） |
| Stage 2 需 4–6 周从零写 | ✅ 变为「把成熟 Rust 库移植到 Android」 |

**真正剩下的工作：**

1. 交叉编译到 `aarch64-linux-android`（reims-vgpu 只在 x86_64-linux / aarch64-macos 验证过）
2. 替换 OS 服务：无 `/dev/udmabuf`（在 reims-vgpu 里只是 NVIDIA 的加速项，有回退）、
   无 QEMU memory-backend 文件（Henge 的 guest RAM 是匿名的，要改 `memfd` 或文件背衬）、
   IOSurface 替身适配
3. 窗口：SDL3 的 Android 后端，或直接写 `ANativeWindow`
4. 接线：Henge 的 C 引擎 ↔ `reims_vgpu.h` 的 C API

**新的最大风险**：Android 的 Vulkan 驱动（Adreno/Mali）远比测试用的 NVIDIA/Intel/AMD 弱，
`metal2vulkan` 需要的特性可能缺失。**这是要最先验证的事。**

### 0.3 其余参考项目（按优先级）

| 优先级 | 项目 | 层级 | 许可 | 用途 |
|---|---|---|---|---|
| **P0** | [`steelbrain/reims-vgpu`](https://github.com/steelbrain/reims-vgpu) | 用户态 | GPL-2.0+/LGPL-3.0+ | **核心**：PVG 宿主侧 + Vulkan 平台，直接移植 |
| **P0** | [`steelbrain/metal2vulkan`](https://github.com/steelbrain/metal2vulkan) | 用户态 | — | Metal API → Vulkan 运行时（reims-vgpu-vulkan 的底座） |
| **P0** | [`steelbrain/qemu-reims-vgpu`](https://github.com/steelbrain/qemu-reims-vgpu) | 用户态 | GPL-2.0 | C API 的参考宿主（`hw/display/apple-gfx`） |
| **P1** | [`UniXNU/AdrenoMetal`](https://github.com/UniXNU/AdrenoMetal) = [`Droid-VM/NativeMetal`](https://github.com/Droid-VM/NativeMetal) | 用户态 | MIT | 同一条 metal2vulkan 路线的 ObjC 前端；Metal 语义覆盖清单 |
| **P1** | [`Droid-VM/macOS-DroidVM`](https://github.com/Droid-VM/macOS-DroidVM) `docs/` | 内核+用户态 | — | 在 Android 上跑 macOS 的实战踩坑日志 |
| **P2** | [`FCL-Team/MobileGlues`](https://github.com/FCL-Team/MobileGlues) | 用户态 | LGPL-2.1 | 用户态翻译层骨架范式、EGL 上下文管理 |
| **P2** | [`FCL-Team/FCLDriverPlugin`](https://github.com/FCL-Team/FCLDriverPlugin) | 用户态 | — | 免 root 加载外部 Vulkan ICD（驱动兼容性兜底） |
| **P3** | [`UniXNU/Adreno740`](https://github.com/UniXNU/Adreno740) | 内核态 | MIT | hostsim 测试替身、trace/计数器、GPU 提交链路语义 |
| **P3** | [`UniXNU/AirNir`](https://github.com/UniXNU/AirNir) | 编译期 | MIT | AIR → NIR（本计划**不需要**，metal2vulkan 已覆盖） |

**已排查但没有可用方案的**：

| 项目 | 为何不用 |
|---|---|
| Vectras VM | 纯 QEMU TCG 软模拟 + VNC/3dfx 软渲染，无 GPU 加速 |
| Inferno-Android | Apple Silicon 硬件模拟，作者自述「very slow」，无 GPU 加速，打补丁还需 Mac |
| HakiOS | Darwin 内核裸机刷机，不是 App，无 macOS 用户态 |
| DroidVM / macOS-DroidVM | 需 root + Gunyah/KVM + 自定义 macOS 内核 + kext 注入，架构不同 |
| QEMU `apple-gfx` | 薄壳，真干活的是 Apple 二进制框架，只能在 macOS 宿主编译 |
| UTM / UTM SE | iOS 侧，且 SE 无 JIT |

---

### 0.5 Stage 0 实测基线（已完成的探测）

`android/` 里的探针 APK 在真机上测完了两件事。**Stage 0 的两个未知，消掉了一个半。**

#### 可执行内存 —— ✅ 绿

```
page size        : 4096
SELinux domain   : u:r:untrusted_app:s0:c85,c257,c512,c768
device API level : 35
memfd split (R-X + R-W)   WORKS  call=0x1234
anon W+X                  WORKS  call=0x1234
hv_jit_region_open()      memfd split (rw_diff=-1048576)
```

#### Vulkan 能力 —— ✅ 绿（但带一个已知移植项）

测试设备：**Adreno 740**（`vendorID 0x5143`，SM8550 / 骁龙 8 Gen 2），
**Vulkan 1.3.128**，driverVersion `0x802a4047`，114 个设备扩展。

metal2vulkan 依赖的 11 个 feature **全部为 yes**：

`bufferDeviceAddress`、`timelineSemaphore`、`descriptorIndexing`、
`runtimeDescriptorArray`、`dynamicRendering`、`synchronization2`、
`shaderFloat16/Int8`、`storageBuffer16BitAccess`、`hostQueryReset`、
`scalarBlockLayout`、`maintenance4`

关键扩展全部存在：`VK_KHR_swapchain`、`VK_KHR_sampler_ycbcr_conversion`（YCbCr 双平面视频）、
`VK_ANDROID_external_memory_android_hardware_buffer`、`VK_KHR_push_descriptor`、
`VK_KHR_imageless_framebuffer`、`VK_EXT_extended_dynamic_state(2)`、
`VK_EXT_vertex_input_dynamic_state`、`VK_EXT_transform_feedback`、
`VK_EXT_custom_border_color`、`VK_EXT_4444_formats`、`VK_EXT_filter_cubic`。

**缺少的 3 项及影响：**

| 缺 | 影响 | 替代 |
|---|---|---|
| `VK_EXT_external_memory_dma_buf` | reims-vgpu 的 **`os/linux/` 用它做 guest 内存导入** | `AHardwareBuffer`（✅ 有）或普通 host-visible buffer + 拷贝。**这是确定的移植工作项** |
| `VK_EXT_host_image_copy` | 上传便利性 | staging buffer |
| `VK_EXT_image_drm_format_modifier` | 平铺导入 | Android 用 AHB，无关 |

#### 0.5.1 自带 Turnip 后的实测基线（决定性结果）

把 Turnip 打进 APK、经 `libadrenotools` 的 linker namespace 加载后，同一台设备
（PJE110 / SM8550 / Adreno 740 / Android 15）的 Vulkan 报告变成：

| | 系统驱动（高通） | 自带 Turnip |
|---|---|---|
| device name | `Adreno (TM) 740` | `Turnip Adreno (TM) 740` |
| apiVersion | 1.3.128 | **1.3.296** |
| driverVersion | `0x802a4047` | `0x06003004` |
| 设备扩展数 | 114 | **145** |
| heap 0 | 15133 MiB | 11350 MiB（device local） |

**关键差异：三个原本缺失的扩展，Turnip 全部提供。**

| 扩展 | 系统驱动 | Turnip | 意味着 |
|---|---|---|---|
| `VK_EXT_external_memory_dma_buf` | ❌ | **✅** | **reims-vgpu `os/linux/` 的 guest 内存导入路径可直接用**，不必改 AHardwareBuffer |
| `VK_EXT_host_image_copy` | ❌ | **✅** | 上传路径不必退化成 staging buffer |
| `VK_EXT_image_drm_format_modifier` | ❌ | **✅** | 平铺格式导入可用 |

即「自带驱动」不只是提升通用性，它**直接消掉了一个已记录的移植工作项**。
metal2vulkan 依赖的 11 个 feature 在 Turnip 下同样全部为 yes。

#### 0.5.1b 实测：metal2vulkan 在 Adreno 740 原厂驱动上跑通了

之前 §0.5 留的那个「唯一未验证的组合」——metal2vulkan + 高通**原厂**驱动——已经有答案了，
而且是设备实测（`dev.henge.probe`，APK 里带 `reims-vgpu-c` 静态库）：

```
--- machine bring-up ---
  machine        : up, 2048 MB guest RAM, 1 cpu
  guest RAM file : memfd (the GPU can reach guest memory)
  GPU (reims)    : device up
  display        : yes
  IOSurface map  : yes
  first tick     : survived
```

logcat 里的对应日志：

```
device starting on Metal device Adreno (TM) 740
newComputePipelineStateWithFunction:error:: ... made uniqueIdentifier 137438953476; kernel "BlitInRGB_2P_XR10_A8"
newComputePipelineStateWithFunction:error:: ... made 137438953477; kernel "BlitOutRGB_2P_XR10_A8"
iosurface::device: host device 0xb400007536d0c630 created
newFunctionWithName:: displayPresentVertex / displayPresentFragment / ...WithGammaTableAndColorMatrix
```

**结论**：metal2vulkan 的着色器翻译与管线创建在 Adreno 740 原厂驱动上成立。
（注意：reims-vgpu 自己按 `libvulkan.so` 名字打开 loader，所以走的是**原厂驱动**，
不是我们经 adrenotools 注入的 Turnip。两条路各自可用，选哪条是后续决策。）

已知的一条降级（日志里明确报了）：

```
views of scattered guest pages take a kernel mapping a run of pages, counted against
vm.max_map_count, as /dev/udmabuf would not open (No such file or directory):
load the udmabuf module, ...
```

即 `/dev/udmabuf` 不存在（Android 没有该节点），散页视图改走内核映射方案。
这不是失败，是既定的替代路径，但会消耗 `vm.max_map_count`，值得在长时间运行时关注。

**另外发现两个集成必做项**，都是库按桌面程序假设、Android 不成立的地方：

| 问题 | 症状 | 处理 |
|---|---|---|
| 没有 `$HOME` / `$XDG_CACHE_HOME` | metal2vulkan 拒绝创建设备，reims 视为致命 → **进程 abort**，且死因只写 stderr（Android 上等于丢失） | 创建前 `setenv("XDG_CACHE_HOME", <app cache dir>, 1)`。已做在 `android/henge_vm.c` |
| 致死原因只走 stderr | 只看到 abort，没有原因 | 给 `reims-vgpu-core` 的 `die()` 加一行 `log::error!`，配合 `rvg_log_set_handler` 转 logcat。已做 |

#### 0.5.2 加载路径：为什么必须用 linker namespace

首次尝试按绝对路径 `dlopen` 内置驱动，设备实测直接失败：

```
dlopen failed: library "libcutils.so" not found: needed by
  .../lib/arm64/libvulkan_freedreno.so in namespace clns-7
```

GPU 驱动 `NEEDED` 的厂商库（`libcutils` / `libhardware` / `libsync` /
`libnativewindow`）不在 app 的 classloader namespace 里。这是 **linker namespace
隔离，不是权限**，任何路径写法都绕不过。

正确做法（`libadrenotools`，BSD-2-Clause）：
建命名空间 → 挂到系统库 → 装 hook 拦截 `android_dlopen_ext` → 返回**系统 Vulkan
loader** 的 handle。驱动由 loader 在枚举物理设备前一刻解析。

实测中还发现一个 API 坑：`customDriverDir` **必须以 `/` 结尾**。否则其内部

```cpp
stat((std::string(customDriverDir) + customDriverName).c_str(), &buf)
```

会 stat 成 `.../liblibvulkan_freedreno.so`，然后静默 `return nullptr`，全程无日志。

另有一条待解释但**不影响推进**的现象：这些 Turnip 构建的 `.dynsym` 只导出
`HMI`（一个 248 字节的标记对象），Vulkan 入口点是 `LOCAL`。实测说明在这种加载
方式下它仍可用，所以此前「零导出＝不可用」的推断作废；但也不要据此认为零导出是
安全的，换加载方式仍需重新量。

#### 一个重要矛盾要正视

**Adreno 740 正是 FCL 的 `checkIfANGLESupported()` 明确排除 Vulkan 的那颗 GPU。**
但三者的消费方式不同：

| 项目 | 消费方式 | 在 A740 上的表现 |
|---|---|---|
| FCL / MobileGlues | ANGLE（GLES-on-Vulkan） | **主动放弃 Vulkan，退回 GLES** |
| FCL / starling | wgpu → Vulkan | 正常出图 |
| UniXNU / AdrenoMetal | **metal2vulkan → Turnip** | **就是为 A740 开发的**（SM8550 "sheng"） |

Henge 的路线与后两者同类（不是 ANGLE），而 metal2vulkan 前端恰好是在 A740 上做的。
**唯一未验证的组合**：metal2vulkan + 高通**原厂**驱动（UniXNU 配的是自写内核驱动 NativeAdreno）。
这是 Stage 0 剩下的最后一个真风险。

堆：`heap 0: 15133 MiB`（UMA，整机 RAM），`heap 1: 256 MiB`。
4–8 GB guest RAM 的约束来自 Android 单进程限制，不来自 Vulkan。

---

### 0.6 图形层的通用性设计原则（决定架构，不是优化）

> 这一节的原则优先于 §0.5 的实测数据。实测数据是**降级策略的输入**，不是准入条件。

#### 为什么不能按设备适配

Vulkan 里几乎没有 mandatory feature：`timelineSemaphore`、`bufferDeviceAddress`、
`descriptorIndexing` 在 1.2 里都是**可选** feature。厂商完全可以在 `apiVersion 1.3`
的同时把这些位全填 false。所以「这台设备有」推不出「用户那台有」，按设备测永远是抽样。

#### 三层，缺一不可（FCL 给的是三层，不是一层）

| 层 | 做法 | 证据 |
|---|---|---|
| **1. 通用子集** | 只依赖几乎必然存在的接口，其余全部可选 | starling `gfx.rs`：`let required_features = adapter.features() & features;` |
| **2. 降级阶梯** | 缺能力走慢路径，且**显式记录**而非静默 | reims-vgpu `docs/accuracy-ledger.md` + `REIMS_VGPU_EXACT=<field>` |
| **3. 自带驱动** | **把可变性从 OEM 手里拿走** | `FCLDriverPlugin`：Turnip/panvk 打包为 APK，linker hook 拦 `android_dlopen_ext`，**免 root** |

第三层才是"通用"的真正答案：前两层的天花板取决于 OEM 驱动质量，而它不可控；
自带 Turnip 后 feature 集合是 Mesa 的，**对所有 Adreno 相同**。

#### 分级

| Tier | 内容 | 策略 |
|---|---|---|
| **A** | Vulkan 1.1 core：swapchain、基本管线、基本格式 | 假设存在，不做探测 |
| **B** | `bufferDeviceAddress`、`timelineSemaphore`、`descriptorIndexing`、`dynamicRendering`、`synchronization2` | 运行时探测 + 降级；**缺失时报告，不静默** |
| **C** | 自带 Turnip | Adreno 上把 Tier B 变成常量 |

**关键判断**：metal2vulkan 对 Tier B 依赖很深 —— `gpuAddress`、shared event、
argument buffer 是 Metal 语义的骨架，不是可选优化。所以 Tier B 是必需的，
而保证它在所有设备成立的唯一办法就是 Tier C。

> **结论：Turnip 不是"万一不行"的兜底，而是 Adreno 上默认的 Vulkan 提供者。**
> 厂商驱动降级为"能跑就用"的可选加速项 —— 这正是「针对某设备启用它的全部可用」的位置。

#### 边界（必须写明，不能假装全平台等价）

**Turnip 只覆盖 Adreno。** Mali 量产机是 `mali_kbase`，上游 panvk 不支持，需要非上游移植
（FCL 自己的 README 也这么说）。所以「Android 通用」实际上按 GPU 家族分叉：

| GPU 家族 | 通用基线 | 天花板 |
|---|---|---|
| Adreno | **Turnip（自带）** | 统一、可控 |
| Mali / Immortalis | 厂商驱动 + 探测降级 | 不可控 |

Henge 需要 4–8 GB RAM + 43 GB 存储，现实设备就是旗舰档，该分叉可接受，但必须如实记录。

#### 能力探针的定位

`android/probe_vk.c` **不是准入检查**，只是：
1. 降级阶梯的输入 —— 这台机器能爬到第几级
2. 现场诊断 —— 出问题时能对上报的 feature 位直接归因

这与 starling 的用法一致：报告是给性能和 bug 归因用的，不是给能不能启动用的。

---

### 0.7 Stage 2 集成路线（按风险排序）

`reims-vgpu-c` 的 C API 是「虚拟机宿主」的形状：`mmio_read/write`、`display_*`、
`frame_target_*`、`dirty_slot`。对 Henge 而言它就是 `devices/pvg_lite.c` 的替代品，
而 `devices/vmapple.c` 只调用 `hv_pvg_lite_create/poll/destroy` **三个**函数
（MMIO 由 `pvg_lite` 自己注册在 `VMAPPLE_GFX` / `VMAPPLE_IOSFC`），所以适配面是有界的。

| # | 内容 | 状态 |
|---|---|---|
| 1 | **guest RAM 改 memfd 承载** | ✅ 已完成（`devices/vmapple.c`）。理由：`RvgMemoryRangeFile` 要求 guest 内存背后有**文件**，库靠 mmap 这些文件构建 guest 页视图；匿名内存拿不到视图，GPU 就够不到 guest 内存 |
| 2 | `rvg_display_attach_android_window` | ✅ 已完成。`Window::id()` 改 `pub`；`reims-vgpu-window` 重新导出 `set_window`/`clear_window`；`Display::attach_android_window()` 新增；`reims-vgpu-c` 暴露 C 入口并写进头文件（`#if defined(__ANDROID__)`）。已确认导出符号存在 |
| 3 | 写 `reims_pvg.c` 替换 `pvg_lite.c` | ✅ 代码完成、已链接进 `libhenge.so`、设备上加载正常；**但尚未真正创建过 device**（探针不建 VM），所以是「编译通过 + 可加载」，不是「跑通」。内容：`rvg_device_create`（memory_map + memory_files + raise_interrupt）+ `rvg_iosurface_device_create` + `rvg_display_create`（带 window 配置）+ 两个 MMIO 窗口转发 + `hv_reims_pvg_attach_window` |
| 4 | 首次点亮：guest 起 → GPU 驱动初始化 → `mode_change` 回调 | 🟡 **半程**：`android/henge_vm.c` 已能在 app 内建起真实机器（2 GB guest RAM / memfd / GIC / PCIe / virtio / reims GPU 全部就位，display 与 IOSurface mapper 均创建成功，metal2vulkan 完成着色器翻译）。**未做**：ROM 与已安装磁盘，所以 guest 还没跑起来 |
| 5 | 首帧上 SurfaceView | 🟡 界面已就绪：全屏 `SurfaceView` + surface 生命周期全记录 + 启动/停止按钮；缺的是 guest 真的跑起来 |
| 6 | 界面骨架（五屏） | ✅ 实例库 / 显示 / 诊断 / 日志 / 设置，见 `docs/android-ui-plan.md` §六之二 |

**启动路径已就位**（`android/henge_vm.c` 的 `hv_android_vm_boot`）：不依赖 `host/vm.c`
（它要 GCD，而 bionic 没有 BlocksRuntime），只做四件事 —— 每 CPU 一个线程跑 `hv_run()`、
一个 1 ms kicker 调 `hv_machine_kick_all()`、打开磁盘、加载 ROM 并复位 CPU。

**现在缺的是三个文件**，放在应用外部目录下的 `vm/`：

```
/sdcard/Android/data/dev.henge.probe/files/vm/AVPBooter.vmapple2.bin
/sdcard/Android/data/dev.henge.probe/files/vm/disk.img
/sdcard/Android/data/dev.henge.probe/files/vm/aux.img
```

没有它们，「启动」会在显示页上直接列出缺哪个 —— 这是正常路径，不是异常。
要自己产出这三个文件，就得移植 `host/install.c`：它依赖 `third_party/` 下约 14 MB 的
libirecovery + usbmuxd + idevicerestore + libimobiledevice 栈，外加 libcurl+TLS 连 Apple
签名服务器。那是独立的一大块工作，和「借现成镜像」是两条路。

已知的接口细节（写下来避免再翻）：

- `RvgDeviceConfig.memory_map` + `memory_files`：每个 range 一个文件 fd；`fd = -1` 表示该
  range 没有视图（巨型页 / hugetlbfs 必须给 -1，且这类 range 的 GPU 使用会失败）。
- `RvgDeviceConfig.dirty_source` 可为 NULL（无脏页跟踪）。
- `RvgDisplayConfig.window` 可给 `RvgWindowConfig`；给了就等于打开一个窗口。
- `rvg_device_pause / unpause / stop / will_suspend / finish_suspend / will_resume / did_resume`
  对应 VM 生命周期，Henge 的快照机制可以直接对接。

## 1️⃣ 现有结构分析（可移植性评分）

| 目录 | 语言 | 难度 | 备注 |
|------|----------|----------|------|
| `core/` | C | ✅ 直接移植 | CPU、MMU、翻译器、JIT 区域，纯 C |
| `devices/` | C | ✅ 直接移植 | virtio 设备、MMIO、PCIe、USB、音频；`pvg_lite.c` 可保留作对照/回退 |
| `host/` | C | ✅ 直接移植 | JIT 区域、磁盘、网络（slirp）、WebDAV、USB shim |
| `tl/` | ObjC / C | ⚠️ 可丢弃 | 仅 macOS 内嵌模拟测试用 |
| `third_party/` | C / 多种 | ✅ 直接移植 | libslirp、libplist、libzip、libcurl |
| `ios/` | Swift/SwiftUI | ❌ 重写 | App 层、界面、权限、启动流程 |
| `devices/pvg_metal.m` | ObjC | ❌ 被 reims-vgpu 取代 | 保留仅作回退或参考 |
| `docs/architecture.md` | 文档 | ✅ 保留 | — |

---

## 2️⃣ 移植任务分解（时长粗估，单人开发）

| 阶段 | 主要任务 | 预计时长 | 关键文件 |
|------|----------|----------|----------|
| Stage 0 | **风险 Spike**：reims-vgpu 能否在 Android arm64 + Adreno 上跑起来 | 1–2 周 | — |
| Stage 1 | 核心引擎移植（C → NDK），产出 `libhenge.so` | 1 周 | `core/*`、`devices/*`、`host/*` |
| Stage 2 | 图形：集成 reims-vgpu，替换 PVG 后端 | 2–4 周 | `android/reims_bridge.c`、OS 服务适配层 |
| Stage 3 | 音频：CoreAudio → AAudio / OpenSL ES | 1 周 | `host/audio_mac.c` → `host/audio_android.c` |
| Stage 4 | 平台适配（JNI + Android 原生服务） | 2 周 | `ios/` → `android/` |
| Stage 5 | 测试验证 | 1–2 周 | `tests/` |
| 总计 | — | **约 8–12 周（粗估）** | — |

> Stage 0 是新增的**前置门槛**。若 Vulkan 特性不达标，后续计划要重估。

---

## 3️⃣ 具体任务项

### Stage 0 – 风险 Spike（先做，决定后续）

1. **WebGL/Vulkan 特性核验**：在目标 Android 设备上枚举 Vulkan 版本、扩展、limits，
   对照 `metal2vulkan` 的 `docs/FAST-PATHS.md` 与 reims-vgpu 的 `accuracy-ledger` 需求
2. **交叉编译验证**：`cargo build --target aarch64-linux-android`（reims-vgpu-vulkan +
   metal2vulkan-runtime），记录所有编译错误与缺失依赖
3. **OS 服务替换评估**：`crates/reims-vgpu-vulkan/src/os/linux/` 逐项对照 Android 能力
   （`memfd` vs 文件背衬、IOSurface 替身、无名 fd 传递）
4. **16 KB 页含意**：`APV_PAGE_SIZE = 0x4000` 是协议常量 —— 确认 Android 15+ 的 16 KB 页
   设备能否直接受益，4 KB 设备需要怎样的软件映射
5. **最小闭环**：让 reims-vgpu 在一个最小 C 宿主里创建 device、拿到 display 帧

**Go/No-Go 判据**：若 Adreno 的 Vulkan 缺失 `metal2vulkan` 必需特性且无降级路径，
则退回「自写 PVG → Vulkan 子集」并重估工期（此时 reims-vgpu 仍是最重要的参考实现）。

### Stage 1 – 引擎层基础移植

> **环境已就绪**（见 `android/README.md`）。NDK 29 `linux-arm64` clang + rustc 1.98.1
> `aarch64-linux-android` 均实测可用。

**已完成（实测）**：

- `core/*.c` **28/28**、`devices/*.c` **28/28** 编译通过；`host/` 的 `disk.c`、
  `hvsparse.c`、`jit_region.c`、`usb_host.c` 也通过；`libhenge.so` 链接成功，
  NEEDED 只有 bionic（`libandroid/liblog/libdl/libm/libc`）
- 新增 `android/compat/`：Apple Mach 层 → bionic 的兼容层（`mach_absolute_time` 保持
  24 MHz tick 语义、`vm_allocate`→匿名 mmap、`os_unfair_lock`→pthread、
  `CommonCrypto`→vendored mbedtls、`TargetConditionals`、`pthread/qos.h` 等）
- **JIT 区域已实现**：`memfd_create` + 双映射（`R-X` 执行 / `R-W` 写入），
  全程无 W+X 映射；`rw_diff` 记录用的是哪种形状，便于真实 App 上报
- 两处源码修正：`virtio_balloon.c` 的 Darwin 原语加 `__ANDROID__` 分支；
  `hvsparse.c` 修了一个**静默数据丢失 bug**（`SEEK_DATA` 在 exFAT/FUSE 上返回
  `EINVAL` 时被当成"无数据"，会产出空归档）

**剩余工作**：

- **`host/` 剩余文件**：`snapshot.c`（需 zlib + GCD/blocks → pthread 重写，
  且 `mincore` 在 Linux 上不可用于跳过未触碰页）、`share.c`（`copyfile.h`）、
  `dns_proxy.c`（`dns_sd.h`）、`net_slirp.c`（libslirp 未 vendored）、
  `install.c` / `libusb_shim.c`（已 vendored，接 include 路径即可）
- **JDK 侧**：`vm.c` 是 iOS host（GCD + CAMetalLayer + CoreFoundation），需重写为
  Android host；`main.c`、`hvf_ref.c`、`prof_ios.c` 不适用
- **Darwin 依赖剥离**：`Foundation`
- **iOS 宏清理**：`HV_JIT_SPLIT`、increased memory limit、dynamic-codesigning
- **单元测试**：`tests/` 先在当前 arm64 glibc 环境跑通，再交叉编译到 NDK

### Stage 2 – 图形（集成 reims-vgpu）

> 技术路线：**用 reims-vgpu 取代 `pvg_lite.c` + `pvg_metal.m` 整条链路**。

#### 2.1 集成方式

- 新增 `android/reims_bridge.c`：实现 `reims_vgpu.h` 要求的 host 回调
  （create/destroy task、map/unmap/read memory、raise interrupt、trace range、display 回调、window 输入）
- 参考宿主：[`steelbrain/qemu-reims-vgpu`](https://github.com/steelbrain/qemu-reims-vgpu) 的 `hw/display/apple-gfx`
- 保留 `devices/pvg_lite.c`、`pvg_metal.m` 不删，作为对照与回退

#### 2.2 OS 服务适配（`reims-vgpu-vulkan/src/os/linux/` → Android）

| Linux 现状 | Android 改法 |
|---|---|
| host VM over `mmap` of guest RAM files | Henge 的 guest RAM 是匿名内存 → `memfd_create` 或 `ashmem`/`SharedMemory` |
| `/dev/udmabuf` | 无此设备；reims-vgpu 里它只是 NVIDIA 加速项，有回退，可直接跳过 |
| IOSurface 替身 | 保留（纯内存替身，无 Android 依赖） |
| SDL3 窗口 | SDL3 有 Android 后端；或直接 `ANativeWindow` + `VkSwapchainKHR` |

#### 2.3 构建与集成

- Rust 交叉编译：`aarch64-linux-android` target + NDK toolchain
- 产物：`libreims_vgpu.a`（staticlib）链进 `libhenge.so`，或 `.so` 动态加载
- shader：`crates/reims-vgpu-vulkan/metallibs` 是仓库内预编译的 metallib，**可直接复用**，
  不受「只有 Apple 编译器能产 metallib」限制

#### 2.4 分步验收

1. device 创建成功
2. display 帧输出到 `ANativeWindow`
3. macOS 桌面出画面（对应 reims-vgpu 的 boot 流程）
4. 纹理 / 管线 / blit 全链路
5. 拖动帧率对比基准

### Stage 3 – 音频（AAudio / OpenSL ES）

- `host/audio_mac.c`（AudioUnit）→ `host/audio_android.c`，**AAudio**（API 26+）为主、OpenSL ES 兜底
- `devices/virtio_snd.c` 逻辑不变，仅适配原生缓冲区与线程模型

### Stage 4 – 平台适配（JNI + 原生 App）

- `android/henge_vm.c`：VM 生命周期
- `android/henge_jni.c`：JNI 桥（Surface / 输入 / 音频 / 路径）
- `android/CMakeLists.txt`
- 启动流程：Boot ROM 下载 → libcurl；DFU 走 `host/usb_host.c` 的内部 virtio USB 路径
- 权限：Scoped Storage、通知、`foregroundServiceType`、`largeHeap`
- 输入：`virtio_input.c` → `KeyEvent` / `MotionEvent`
- 共享：`share.c`（WebDAV）→ 本地 HTTP + `FileProvider`

### Stage 5 – 测试验证

- 单元测试（NDK 交叉编译）
- 集成测试：启动 → CPU → 磁盘 → 网络 → 图形 → 音频 → USB → 输入
- 冒烟：真机跑 Sonoma 14.6.1 / Ventura 13.6
- 性能：启动耗时、FPS、音频延迟、内存峰值、**首次 shader 编译耗时**
- 诊断：参考 Adreno740 的 trace/计数器与 reims-vgpu 的 `REIMS_VGPU_STATS`

---

## 4️⃣ 关键风险与缓解措施

| 风险项 | 影响 | 缓解方法 |
|--------|------|----------|
| ~~Android Vulkan 特性不足~~ | ~~高~~ → **改为架构问题** | 不能按设备判断（Vulkan 里几乎没有 mandatory feature，见 §0.6）。按「通用子集 + 降级 + 自带驱动」三层设计解决；Adreno 上默认用 Turnip，把 Tier B 变成常量。实测 Adreno 740 全绿仅是记录，不是准入依据（§0.5） |
| ~~metal2vulkan + 高通原厂驱动~~ | ~~中~~ → **已绕开** | 不再依赖原厂驱动：Adreno 上默认走自带 Turnip（实测 Mesa 26 / Vulkan 1.3.296 / 145 扩展，经 linker namespace 加载成功）。原厂驱动降级为可选项 |
| ~~`VK_EXT_external_memory_dma_buf` 缺失~~ | ~~中~~ → **已消解** | 内置 Turnip 提供该扩展（设备实测），`os/linux/` 的 guest 内存导入路径可直接用 |
| ~~ARMv8.4 / FEAT_LSE2 下限~~ | ~~中~~ → **已消解** | 改为**默认特性编译 + 运行期判定**：`ldsetal`/`ldapr` 只在带 `#[target_feature(enable="lse,rcpc")]` 的单函数里，全局仍是 Android 基线，构建不需要任何 `-C target-feature`；运行期按 `ID_AA64MMFR2_EL1.AT`（+HWCAP）判定，支持走单条原子指令，不支持走 granule 锁，老设备不会 SIGILL。详见 [`docs/asm-feature-floor.md`](docs/asm-feature-floor.md) |
| **aarch64-linux-android 是全新 target** | **高** | Stage 0 先试交叉编译，摸清所有编译/依赖问题 |
| **16 KB PVG 页 vs 4 KB Android 页** | ~~高~~ → **中** | 协议常量 `APV_PAGE_SIZE=0x4000` 是**guest 页粒度**，不是宿主 mmap 粒度。已实测测试机为 4 KiB，并复查引擎：**没有把 `getpagesize()` 硬编码成 0x4000 的地方**（唯一的 `getpagesize()` 在 `snapshot.c`，用法正确）。guest RAM 是一整块连续映射，所以 1 个 16 KiB guest 页 = 4 个连续 4 KiB 宿主页，软件 TLB 本来就在做这层。剩下两处只需收尾：`virtio_balloon.c` 的 `HOST_PAGE 0x4000`（粒度粗 4 倍但语义正确）、`snapshot.c` 的 `mincore` 跳过逻辑（Linux 上不成立，见 Stage 1）。**未测过任何 16 KiB 设备** |
| ~~SELinux / `mmap(PROT_EXEC)` W^X~~ | ~~高~~ → **已消解** | **真机实测通过**（`dev.henge.probe` APK，API 35 设备 / targetSdk 36 / 域 `untrusted_app` / 4 KiB 页）：memfd split **和** 匿名 W+X 都返回 `call=0x1234`，引擎按设计选中 memfd split（`rw_diff=-1048576`）。原因：Android 10 的 W^X 收紧针对**文件**（app 数据目录可写文件的 `execve`），不是匿名 `execmem`；后者因为 ART/V8/Skia 都要 JIT，对 app 域一直放行。memfd split 仍是首选 —— 不是因为「能过」，而是**任何时刻不存在 W+X 映射**，对加固 ROM / 反篡改检测更稳 |
| ~~`targetSdkVersion` 与 SELinux 域~~ | ~~中~~ → **不需要** | 实测对比：targetSdk 36 → `u:r:untrusted_app`（无后缀）；维护 targetSdk 27 的宿主 → `untrusted_app_27`。即**带版本号的域只给 legacy target，targetSdk ≥ 29 一律进无后缀的 `untrusted_app`**。所以降 targetSdk 确实会换域，但现代域里两种映射本来就放行，换了也没用。不再作为依赖项 |
| guest RAM 匿名 vs 需要 mmap 背衬 | 中 | `memfd_create` 背衬，或改 reims-vgpu 的 host VM 层 |
| 首次 shader（pipeline）编译卡顿 | 中 | reims-vgpu 有 `pipeline_waits` 开关；需配 cache 与预编译种子 |
| Android OOM / 后台被杀 | 中 | `largeHeap` + Foreground Service + 关闭电池优化 |
| 系统 Vulkan 驱动有 bug | 中 | FCLDriverPlugin 插件契约加载 Turnip 兜底（免 root 可行） |
| 音频层重写 | 低 | 先 AAudio 简单实现 |
| 呈现撕裂 / 半帧 | 中 | fence 等待 + display hold |
| 内核态方案诱惑（Adreno740 / AdrenoPM4） | — | **明确排除**：非特权 App 无权限 |
| 存储 43 GB / 内存 4–8 GB | 中 | 明确告知门槛；运行时下载 IPSW 并解析 |

---

## 5️⃣ 预计产出

| 产出 | 描述 | 形式 | 状态 |
|------|------|------|------|
| `android/` 编译环境 | NDK + CMake + Rust target + Mach 兼容层 | CMake / Shell | ✅ **已建成并验证** |
| `libhenge.so` | 核心引擎（core + devices + host + asm + compat） | NDK 构建 | ✅ **可构建，291 KB text** |
| `android/` App 骨架 | Gradle 8.13 + AGP 8.13.2 + aapt2(aarch64) | Gradle | ✅ **出 APK** |
| `dev.henge.probe` APK | 可执行内存形状探针（阶段 0 的 Go/No-Go 依据） | APK | ✅ **可构建，待装真机** |
| `libreims_vgpu.a` | reims-vgpu 的 Android arm64 静态库 | Rust 交叉编译 | ⬜ 待做 |
| `android/reims_bridge.c` | reims-vgpu C API ↔ Henge 引擎的胶水 | C | ⬜ 待做 |
| `host/jit_region.c`（Android 版） | W^X 合规的可执行内存分配 | C | ⬜ 待做 |
| `host/audio_android.c` | AAudio / OpenSL ES 后端 | C | ⬜ 待做 |
| `android/henge_jni.c` | JNI 桥 | C | ⬜ 待做 |
| `android/` App 壳 | Kotlin 壳 + Foreground Service + UI | Kotlin | ⬜ 待做 |
| `tests/` | 单元测试 + 集成测试 | C | ⬜ 待做 |

---

## 6️⃣ 下一步（优先级）

### 立即：Stage 0 风险 Spike

1. **拿一台目标机枚举 Vulkan 能力**，对照 metal2vulkan 需求
2. **`cargo build --target aarch64-linux-android`** 试 reims-vgpu-vulkan，记录全部错误
3. **读 `crates/reims-vgpu-vulkan/src/os/linux/`**，逐项标出 Android 要改的地方
4. **读 `docs/testing.md` §6**（guest ABI）与 `docs/accuracy-ledger.md`，判断可接受的精度妥协

### 并行

5. 建 `android/` 骨架 + NDK 构建，把纯 C 部分编出 `libhenge.so`（Stage 1 与 Stage 0 互不阻塞）
6. `tests/` 在当前 arm64 glibc 环境跑通，形成回归基线

---

## 附：参考项目速查

| 项目 | 层级 | 许可 | 用途 | 关键文件 |
|------|------|------|------|---------|
| [`steelbrain/reims-vgpu`](https://github.com/steelbrain/reims-vgpu) | 用户态 | GPL-2.0+/LGPL-3.0+ | **核心：直接移植** | `crates/reims-vgpu-core/`、`crates/reims-vgpu-vulkan/`、`crates/reims-vgpu-c/include/reims_vgpu.h`、`docs/testing.md` §6、`docs/architecture.md`、`docs/accuracy-ledger.md` |
| [`steelbrain/metal2vulkan`](https://github.com/steelbrain/metal2vulkan) | 用户态 | — | Metal → Vulkan 运行时 | `docs/FAST-PATHS.md` |
| [`steelbrain/qemu-reims-vgpu`](https://github.com/steelbrain/qemu-reims-vgpu) | 用户态 | GPL-2.0 | C API 参考宿主 | `hw/display/apple-gfx` |
| [`UniXNU/AdrenoMetal`](https://github.com/UniXNU/AdrenoMetal) / [`Droid-VM/NativeMetal`](https://github.com/Droid-VM/NativeMetal) | 用户态 | MIT | 同路线 ObjC 前端、Metal 语义清单 | `NativeBridgeMetalFrontend.mm`、`bridge/NativeMetalObjects.rs` |
| [`Droid-VM/macOS-DroidVM`](https://github.com/Droid-VM/macOS-DroidVM) | 内核+用户态 | — | Android 上跑 macOS 的踩坑日志 | `docs/TODO-gpu.md`、`docs/k27-virtio-gpu-3d-plan.md` |
| [`FCL-Team/MobileGlues`](https://github.com/FCL-Team/MobileGlues) | 用户态 | LGPL-2.1 | 翻译层骨架范式 | `MobileGlues-cpp/egl/context.cpp`、`gl/*` |
| [`FCL-Team/FCLDriverPlugin`](https://github.com/FCL-Team/FCLDriverPlugin) | 用户态 | — | 免 root 加载外部 Vulkan ICD | `README.md` |
| [`UniXNU/Adreno740`](https://github.com/UniXNU/Adreno740) | 内核态 | MIT | hostsim 测试替身、trace/计数器 | `tests/hostsim/*`、`NativeGpuTrace.*` |
| [`UniXNU/AirNir`](https://github.com/UniXNU/AirNir) | 编译期 | MIT | AIR → NIR（本计划不需要） | — |
