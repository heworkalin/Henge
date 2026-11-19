# CLI 上的 GPU 与离屏显示（2026-11-19）

**结论**：不靠窗口、不靠 APK，CLI 已经能跑 GPU 通路并**把画面抓成图片**。
macOS 从安装好的 `disk.img` 正常启动，进度条在走（实测 38% → 46% / 130 s），
还没到 GPU 出帧的阶段（那要等 WindowServer 设置显示模式）。

---

## 1. CLI 接上 reims-vgpu

`tools/build-install-android.sh` 增加了可选的 reims 链接（库存在时自动启用）：

| 项 | 值 |
|---|---|
| 宏 | `-DHENGE_HAVE_REIMS=1` |
| 源文件 | `android/reims_pvg.c` |
| 头文件 | `-Iandroid -I<crates/reims-vgpu-c/include>` |
| 库 | `android/third_party/reims-vgpu/target/aarch64-linux-android/release/libreims_vgpu_c.a` |
| 系统库 | 额外需要 `-landroid`（reims 的 window crate 用 `ANativeWindow_*`） |
| **API level** | **必须 33**（`memfd_create` 是 API 30+；APK 那边 CMake 用 android-33） |

重编 reims 本身时（改了 Rust）要指定 NDK 链接器：

```bash
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$ANDROID_NDK_TOOLCHAIN/bin/aarch64-linux-android33-clang
cargo build --release --target aarch64-linux-android -p reims-vgpu-c --no-default-features --features vulkan
```

## 2. 两个崩溃，各自的根因

### 2.1 `memory.map.mapping_length_unaligned`

reims 的 `page_size()`（`reims-vgpu-vulkan/src/os/linux/vm.rs`）取的是 **`sysconf(_SC_PAGESIZE)`**：

| 平台 | 宿主页 | guest 页（VMAPPLE） |
|---|---|---|
| macOS（reims 原本） | 16 KiB | 16 KiB |
| **Android** | **4 KiB** | **16 KiB** |

这个值被 `iosurface/device.rs` 的 `page_size()` 拿去当 **guest** 映射的长度（descriptor table、
ring），而 `new_range_mapping` 要求它是 `APV_PAGE_SIZE = 16384` 的倍数 → 4096 不满足 → `die`。

**修法**（在 reims 侧，Android 时返回 guest 页大小）：

```rust
let host = u64::try_from(unsafe { libc::sysconf(libc::_SC_PAGESIZE) }).unwrap_or(4096);
if cfg!(target_os = "android") { 16384 } else { host }
```

宿主侧按 16 KiB 粒度分配在 4 KiB 页的宿主上仍然合法（大粒度是小粒度的倍数）。

**修好后的实测**：`gfx write +0x1008 = 0x1c → 0x34 → 0x44 …`（guest 的 GPU 门铃在递增），
不再 abort。

> 这一项**不是**"放宽检测"。检查本身是对的，错的是喂给它的页大小语义。

### 2.2 宿主 SIGSEGV（`Segmentation fault`，10 多秒）

我曾在 boot 循环里接上 `hv_android_scan_procs()`（安装模式有这条，我照搬）。
它从主线程遍历几十万个 guest 页，而 CPU 线程同时在跑 guest 代码 → 与它们的内存路径竞态。

**证据**：
- 带 `HV_SCAN_PROC` + SIGUSR1 → 10 秒内 SIGSEGV
- 不带 → 稳定跑几分钟
- **gdb attach → 不崩**（attach 停住所有线程，竞态窗口消失，Heisenbug）

**已移除**，并写明原因：安装模式能扫是因为它在 restore 结束后运行，**正常启动的 guest 没有那个静止时刻**。

## 3. boot framebuffer：真 boot_args 在别处

上游 `host/vm.c` 的 `hv_vm_boot_display()` fallback **扫描 RAM 前 1 GB** 找 boot_args 结构
（revision 2..3、version 1..3、virtBase 高 8 位 `0xfffffe`、physBase 落在 guest RAM 内），
再从里面取 framebuffer：`q[5]` base、`q[7]` row、`q[8]` width、`q[9]` height、`q[10]` depth。

**iBoot 把它放在 `~0x78ed0000`**——而引擎自己的 `HV_EXTRA_BOOT_ARGS` 补丁看的是 `0x837f0000`。
那次测量里 `0x837f0000` 一直是 `rev 0 ver 0 / dt length 0`，**而内核真正的参数在别处**，
这就解释了为什么正常启动时那个补丁"落空"。

同一套扫描移植到 `android/henge_vm.c` 的 `hv_android_boot_framebuffer()`，实测：
**640×1136，stride 2560，黑屏 + 底部进度条**。

## 4. 离屏帧目标（真正"出图片"的那条路）

不是伪造 `ANativeWindow`，reims 本身就是给了这条 API：

```
rvg_frame_target_create(device, {w,h,format}, &target)     // "the CPU can read"
rvg_display_encode_frame(display, target, NULL, &frame)     // display 的当前帧 → target
rvg_frame_commit(frame); rvg_frame_wait(frame)
rvg_frame_target_read_pixels(target, dst, bytes_per_row, len)   // CPU 读回
```

`android/reims_pvg.c` 里实现为 `hv_reims_pvg_grab(p, path)`，写 32-bit BMP（BGRA，行倒序）。
**前提**：guest 设置过显示模式（`have_mode`），否则没有可编码的帧。

帧计数用 `rvg_display_present_counts(display, &guest, &host)`，是上游 `hv_vm_gpu_frames()` 的对应物。

## 5. 输入注入（上游的做法）

```c
// host/vm.c:662（上游）
void hv_vm_key_hid(uint8_t usage, int down) { hv_xhci_key(board.usb, usage, down); }
```

`devices/xhci.h`：`hv_xhci_key()` **任何线程**可调；ESC = `0x29`（HID usage page 7）。
Android 侧导出 `hv_android_key(usage, down)`（板子在本文件内 static），CLI 用
`HV_FAKE_ESC=<秒>` 在启动若干秒后注入一次。

实测：注入成功，但**对还在 boot framebuffer 阶段的 guest 没有反应**（它本来就还没到输入阶段）。

## 6. 两条显示路径的现状

| 路径 | 上游 iOS | CLI（现在） |
|---|---|---|
| **boot framebuffer**（固件进度条） | `hv_vm_boot_display` fallback | ✅ `HV_GRAB_EVERY=<秒>` → `workB/fb-NNN.bmp` |
| **GPU 帧**（macOS 起来后） | `hv_pvg_lite_frame` | ✅ `rvg_frame_target_*`（等 `have_mode`） |
| 窗口呈现 | CAMetalLayer | APK 的 SurfaceView（CLI 没有） |

**实测进度**：`fb-001`（38%）→ `fb-007`（46%），130 秒走 8%，画面差异 2268 字节——**在推进**。

## 7. 下一步

1. 等进度条走完 → guest 设置显示模式（`reims: guest mode WxH`）→ `frame.bmp` 出现真画面
2. 之后 GPU 帧可以直接用 `hv_reims_pvg_grab()` 抓，做画面比对
3. 若启动变慢或停住，用 `HV_GRAB_EVERY` 的图片序列判断是"慢"还是"停"

## 8. 未解：多核时偶发的宿主 Segmentation fault

**状态：未知。不作归因。**

- **现象**：4 核（以及 8 核）运行时，进程被 `Segmentation fault` 结束；2 核从未出现。
- **未能复现**：同一份二进制、同样参数再跑 4 核，稳定运行 2 分钟以上无崩溃。
- **没有留下现场**：`android/henge_vm.c` 已装 host fault handler（打印 pc、lr、sp、出错地址、
  JIT 范围、各 CPU 的 guest pc），但那几次崩溃没有产生任何 handler 输出——原因本身也没查清。
- **观察到的相关事实**：4 核时 `reims-vgpu.fifo` 与两个 CPU 线程各占约 93% CPU；
  但按操作者的说明，那是 GPU 模块正常的周期性轮询，不构成异常证据。
- **为什么不猜**：没有任何模块级证据（没有 pc、没有栈、没有日志差异）。把它归到 reims、归到
  引擎的锁、归到 JIT，都是猜测。
- **为什么不继续多核**：核心越多，能同时出错的地方越多，而当前连一次现场都抓不到；
  排查成本会指数上升。**正式运行用 2 核**，直到有足够详细的日志/现场可定位。

**要推进这一项，需要的是现场，不是更多核**：

1. 让 host fault handler 的输出可靠地留下来（先验证它在**能复现**的崩溃上确实会打印）；
2. 或者用 core dump（`ulimit -c`）而不是信号处理器，避免 handler 本身影响时序；
3. 或者记录每个 CPU 线程的退出方式（正常退出 / 信号 / 卡住），先判断"是谁先死"。

**在此之前**：2 核。进度条在 40~50% 之后冻结是 GPU 接管的正常现象（操作者已确认），
不能再用"进度条不动"判断卡死。
