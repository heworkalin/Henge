# GPU 渲染链路打通记录（2026-11-21）

目标：已安装的 macOS 从 `workB/disk.img` 正常启动，**画面真正渲染出来**（CLI 离屏抓帧 / APK 出画面）。
本文记录这一天排除掉的卡点、改了什么、证据是什么、以及**当前卡在哪**。

结论先行：

- 呈现链路已经**通到 Surface**：`host presents` 从 0 → 109（app），swapchain 建立正常。
- guest **确实把画面交给 GPU**（`guest presents` 每秒约 1.6 次），但**内容全黑**。
- 2026-11-21 03:00 查到根因候选：guest 建的渲染管线里**依赖 framebuffer fetch 的那批被 m2v 拒绝**，
  对应的 `exec` 全部 fault（一轮 boot 里 **655 次** `render pipeline lookup failed`）。
  修法（把 `VK_EXT_rasterization_order_attachment_access` 写进 device options）**已写、未验证**。

---

## 1. 这一天修掉的四个真问题

### 1.1 IOSurface 查不到（`use_iosurface_mapper`）

**症状**：`exec faulted: resource 9 not found: no IOSurface for mapping 1` ×88，
而 `map surface 1..5` 全部成功、无 unmap。

**根因**：`RvgDeviceConfig.use_iosurface_mapper` 从没设过（默认 false）。device 因此
**不走我们建的 `RvgIOSurfaceDevice`** 去找 mapping id，永远查不到。

**改**（`android/reims_pvg.c`）：`config.use_iosurface_mapper = true;`

**证据**：run31 起 `no IOSurface for mapping` 计数 **88 → 0**。

### 1.2 `tasks.unsupported.writable_cursor`（`process_isolation`）

**症状**：50 秒左右 `reims-vgpu: fatal: tasks.unsupported.writable_cursor`，进程 Aborted。

**根因**：local task 不支持 writable cursor（`LocalTask::writable_cursor_from_virtual_offset`
注释即"A local task has no writable cursor; asking ends the process"），只有 isolated(remote)
task 的 service 实现它。

**改**（`android/reims_pvg.c`）：`config.process_isolation = true;`，并且把 task 长度 clamp
挪到 isolation 分支之前（`device/tasks.rs`），remote task 也受限，否则 256 GiB 的 task root
在 39 位用户地址空间里和 JIT 区打架。

**证据**：该 fatal 不再出现；启动推进到真正建渲染管线。

### 1.3 Android 窗口：attach 永远失败（display 创建时必须带 window link）

**症状**（app）：
```
reims: rvg_display_attach_android_window failed: unsupported struct size
reims: window refused, display stays offscreen
```

**根因**：`rvg_display_attach_android_window` 对**创建时没带 window 的 display** 返回
`RVG_ERR_UNSUPPORTED`（消息文本 "unsupported struct size" 极易误读）。我们为了避免 CLI 的
use-after-free，创建时一律 `window = NULL`，于是窗口链接根本不存在。

**改**：
- `android/reims_pvg.c`：`HV_REIMS_WINDOW=1` 时用 `RvgWindowConfig`（无 flags、无 callbacks、
  默认 1920×1080）创建 display；
- `android/henge_vm.c`：app 启动机器前 `setenv("HV_REIMS_WINDOW","1",0)`；CLI 不设，保持 offscreen。

**证据**：`reims: display attached to the host's window` + `window 1: 2640x1264 attached`。

### 1.4 swapchain 两个独立问题（实例扩展 + 图像格式）

**(a) 实例没启用 `VK_KHR_android_surface`**

**症状**：`no swapchain for the window (no surface): it stays empty`。

**根因**：reims 的 Android 分支复用 Linux 的 `WINDOW_SURFACE_EXTENSIONS`
（wayland/xcb/xlib）。Android loader 一个都不列 → 实例只加 `VK_KHR_surface` →
`vkGetInstanceProcAddr(instance,"vkCreateAndroidSurfaceKHR")` 为 NULL → `create_surface` 静默 None。

**改**（`reims-vgpu-vulkan/src/os/linux.rs`）：
```rust
#[cfg(target_os = "android")]
pub(crate) const WINDOW_SURFACE_EXTENSIONS: [&CStr; 1] = [ash::khr::android_surface::NAME];
```

**(b) swapchain 图像格式写死 `B8G8R8A8_UNORM`**

**症状**：`no swapchain for the window (the window's surface offers no B8G8R8A8_UNORM in
SRGB_NONLINEAR)`。

**根因**：Android 的 `ANativeWindow` 在 Turnip 下报 `R8G8B8A8_UNORM`。

**改**（`swapchain.rs`）：候选表按序尝试
`B8G8R8A8_UNORM / R8G8B8A8_UNORM / B8G8R8A8_SRGB / R8G8B8A8_SRGB`，选中的
`VkFormat` 与给 runtime 的 `PixelFormat` 一起存进 `Swapchain`，贯穿 `image_info` /
`image_descriptor`；全部不匹配时日志列出 surface 实际提供的格式 raw 值。

**证据**：app 日志 `new frames=109 | guest presents=109 | host presents=109`（此前 host 恒为 0）。

---

## 2. 驱动加载：Turnip 必须走 libadrenotools 的 linker namespace

厂商标定 Adreno 驱动（1.3.128，114 扩展）**同时缺** framebuffer fetch 的两种形式：

| 探针 | 结果 |
|---|---|
| 厂商驱动 `VK_EXT_rasterization_order_attachment_access` | 无 |
| 厂商驱动 `VK_EXT_fragment_shader_interlock` | 无 |
| Turnip 1.3.296（145 扩展） | 有 `VK_EXT_rasterization_order_attachment_access`（+ ARM 变体） |
| Turnip 的 feature 查询 | `rasterizationOrderColorAttachmentAccess=1`、depth=1、stencil=1、pixelInterlock=0 |

**不能直接 dlopen 驱动**：它的 `NEEDED` 是 `libcutils` / `libhardware` / `libsync` /
`libnativewindow`，只在系统 namespace 里可见。实测报错：
`library "libcutils.so" not found: needed by ... in namespace clns-7`。

**改**：
- 新增 `android/vk_loader.c|h`：`henge_vk_open_adrenotools(dir)` 走 libadrenotools，
  返回**系统 loader** 的 handle；`dir` 为空时用 `dladdr` 取自己所在目录（app 的
  `nativeLibraryDir`，Turnip 与 hook 库都在那儿）。
- `reims-vgpu-vulkan/src/os/linux.rs`：`HV_VK_DRIVER=<dir>` 或 Android 默认目录时，
  用 `ash::Entry::from_static_fn(vkGetInstanceProcAddr)` 建 entry —— **全程没有 dlopen 驱动**。
- CLI 也链 `libadrenotools.a` + `linkernsbypass` + 静态 libc++（`tools/build-install-android.sh`）。

**坑（已记录过，又踩了一次）**：`customDriverDir` **必须以 `/` 结尾**，否则 libadrenotools
内部 `stat(".../liblibvulkan_freedreno.so")` 失败并**静默返回 NULL**，看起来像"namespace 方案不可用"。

**证据**：`reims[reims_vgpu_vulkan::os::linux]: Vulkan loader: libadrenotools namespace, driver
directory /data/local/tmp/henge/driver`；进程 maps 里出现
`/data/local/tmp/henge/driver/libvulkan_freedreno.so`。

---

## 3. 当前卡点：framebuffer fetch 的管线被拒 → exec 全 fault

一轮 boot（run45）里：

```
reims[...gpu::states]: pipeline refused: unsupported: Unsupported { feature: FramebufferFetch,
    needs: AnyOf([[Feature(RasterizationOrderColorAttachmentAccess)], [FragmentShaderPixelInterlock]]) }
reims[...resources::manager::lookup]: render pipeline lookup failed: Metal refused a render pipeline: ...
reims[...gpu_task::exec]: task 2: exec faulted: render pipeline lookup failed
```

计数：**655 次 exec fault，全部是 `render pipeline lookup failed`**。

也就是说：guest 不是"没派任务给 GPU"，而是**派了但 draw 用的管线建不出来**，所以画不出内容，
`guest presents` 只是把空 surface 送去 present（这也解释了 1.6 次/秒的黑帧）。

**已写、未验证的修法**（`reims-vgpu-vulkan/src/os/wsi.rs::add_raster_order_extension`）：
在 `device_options()` 里探测物理设备扩展表，若有
`VK_EXT_rasterization_order_attachment_access`（或 `VK_ARM_...`）就加进
`options.device_extensions`。m2v 的 `enable_where_featured` 才会链上
`VkPhysicalDeviceRasterizationOrderAttachmentAccessFeaturesEXT` 并把
`rasterizationOrderColorAttachmentAccess` 置真，`through_inputs()` 才成立。

> 关键点：**设备支持 ≠ 运行时启用**。Turnip 明明报 `=1`，但 reims 没在 device options 里命名该扩展，
> m2v 的 profile 就一直认为它没有，于是拒绝管线。

---

## 4. 新增的可观测性（这次加的都留着）

| 开关 / 日志 | 作用 |
|---|---|
| `reims: status \| window= mode= new frames= guest presents= host presents=` | 每 15 秒，**无条件**（不看 `HV_REIMS_LOG`），app 侧唯一能读的症状线 |
| `reims: display kind = window/offscreen/NONE` | 启动时一次，**无条件** |
| `reims: bus \| gfx writes=… reads=… iosfc writes=… new frames=…`（每秒 + 增量） | 每秒的 GPU 设备访问速率；判"guest 到底有没有在跟 GPU 说话" |
| `reims: efi reg +0x12xx` | EFI display 寄存器写（首个 120 条）；实测**一条都没有** → 固件进度条从不进 GPU 通路 |
| `REIMS_DISPLAY_SAMPLE=1` | 在 `present_frame` 里**直接采样 guest 交上来的那块 IOSurface 字节**，打印 `sampled / non-black / max channel` |
| `HV_GRAB_GPU=1` + `HV_GRAB_EVERY=<秒>` | 每 N 秒写 `gpu-NNN.bmp`（GPU 显示帧）与 `fb-NNN.bmp`（固件帧缓冲），同一时刻对照 |
| `HV_SCAN_PROC=<秒>`（`HV_BOOT_ONLY` 下也生效） | 周期扫 guest 进程名（正常启动没有串口，这是唯一入口） |

`REIMS_DISPLAY_SAMPLE` 的判读：

- `non-black=0` → guest 送来的是空 surface（guest 侧还没画，或画到别的 surface）；
- `non-black>0` → 内容在，是我们把它搬进 Vulkan / 窗口这一段丢了（拷贝或格式兼容）。

---

## 5. 两个被纠正的错误（方法论）

### 5.1 "进度条冻结 = 卡死"是错的，而且我用错了检测带

实测（run45，每 60 秒一张 `fb-NNN.bmp`，640×1136）：

```
fb-001 fill_x=275
fb-002 fill_x=296
fb-003 fill_x=306
fb-004 fill_x=315
fb-005 fill_x=319   ← 条在 y=131..142，x 跨度 86..553，fill 319 ≈ 50%
fb-006 … fb-015     fill_x=319，相邻帧逐像素 diff = 0
```

第一版脚本只在 80%~95% 高度找条，结论是"没有亮条"，**是错的**：条在距顶 11% 处。
而 `docs/gpu-offscreen-2026-11-19.md:150` 已写明：**进度条在 40~50% 之后冻结是 GPU 接管的正常现象**。

教训：整幅图找"亮行"，不要预设位置；以及"冻结"要结合既有结论解释。

### 5.2 boot-args 注入点一直是错的

`hengevm: boot-args " serial=3" (dt length 0)`：`0x837f0000` 那一页的 deviceTreeLength 是 0，
说明它**不是**内核真正拿到的 boot_args（真结构在 0x78ed0000 附近，会移动）。

对比上游（`origin/main:host/vm.c`）：**上游从不写 CommandLine**，只是扫描 RAM 找 boot_args
来读 framebuffer 参数。按上游的扫描法实现后，在 kernel 第一条 block 时刻**仍找不到**（那时
iBEC 还没写好）——所以 boot-args 注入要"延后重试"才有意义，本轮**不追**。
现状：`core/dispatch.c` 会打印
`hengevm: no live boot_args found in the first GB of RAM (HV_EXTRA_BOOT_ARGS not applied)`。

---

## 6. task 长度 0x4000000000 的误读（旁证）

reims 日志（protocol 209）：
```
define_task2 raw: words 00000001 00000000 00000004 0001e879 -> id 0 length 0x400000000 root 125049
define_task2 node @0x7a1e4000: words 0001e872 00000002 00000000 00000000 00000000 declared length 0x400000000
```
`0x400000000` = `0x4 << 32`，`0x4000000000` = `0x40 << 32`：**低 32 位恒为 0，高 32 位是 4 / 0x40**。
这形状正是"把两个相邻 u32 拼成了 u64"。reims 按 245 版的 `u64 length@4` 解 209 版载荷，
真实字段可能就在 +8（u32），量级 0x40 页 ×16 KiB = **1 MiB**，符合消费级。

现状：仍按 32 GiB clamp（`device/tasks.rs`），保证 task 存在；**不是结论**。

---

## 7. 下一步（按顺序）

1. **验证 raster order 扩展的修法**（编译中）：
   ```
   HV_VK_DRIVER=/data/local/tmp/henge/driver HV_BOOT_ONLY=1 HV_LOG=1 \
   REIMS_DISPLAY_SAMPLE=1 HV_GRAB_GPU=1 HV_GRAB_EVERY=60 \
     ./host-install AVPBooter.vmapple2.bin workB/disk.img workB 2048 2 > runNN.log 2>&1 &
   ```
   成功判据：`pipeline refused` 归零、`exec faulted` 归零、`display present #N ... non-black>0`。
2. 若管线仍被拒：抓 `reims_vgpu_vulkan::wsi` 的 "no rasterization order attachment access"
   一行，确认是探测失败（`every_lists`）还是 m2v 的 `enable_where_featured` 拒绝（feature 不全）。
3. 若 `non-black=0` 但管线正常：查 guest 是否把内容画到**另一块 surface**（多 surface/plane），
   用 `REIMS_DISPLAY_SAMPLE` 的 surface id 与 `map surface N` 对照。
4. APK 侧：`hv_reims_pvg_ready()` 目前是 `have_mode && guest>0`，与上游
   `gpu_frames > 0` 等价；等 GPU 内容非黑后，屏幕应当自愈。若要更稳，可改为
   "GPU 首帧非黑才切"，但那需要一次读回，暂不做。

---

## 8. 复现命令速查

```bash
# 引擎（Rust）
cd android/third_party/reims-vgpu
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang
cargo build --release --target aarch64-linux-android -p reims-vgpu-c --no-default-features --features vulkan

# CLI（含 libadrenotools；libadrenotools 是 C++，final link 必须 clang++）
ZLIB=1 API=33 ./tools/build-install-android.sh

# 设备
adb -s emulator-5554 push build/andcli/host-install /data/local/tmp/henge/
# 驱动目录（只需一次）
adb -s emulator-5554 shell 'mkdir -p /data/local/tmp/henge/driver'
adb -s emulator-5554 push android/app/src/main/jniLibs/arm64-v8a/libvulkan_freedreno.so /data/local/tmp/henge/driver/
adb -s emulator-5554 push build/android-arm64-v8a/lib/arm64-v8a/libmain_hook.so \
    build/android-arm64-v8a/lib/arm64-v8a/libhook_impl.so \
    build/android-arm64-v8a/lib/arm64-v8a/libfile_redirect_hook.so /data/local/tmp/henge/driver/

# APK
./android/build-c.sh && ./android/build-apk.sh && adb -s emulator-5554 install -r \
  android/app/build/outputs/apk/debug/app-debug.apk
```
