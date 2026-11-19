# 明天的工作：用 CLI 跑 GPU 的离屏渲染（2026-11-19 记）

## ⚠️ 磁盘镜像约束（操作者明确要求，动手前先读）

- **CLI 的安装流程只跑一次。** 成功之后 `workB/disk.img`（逻辑大小 64 GiB）**不得删除**，
  也**不得再启动第二次安装**——每跑一次安装都会创建/覆盖一个 64 GiB 的镜像。
- 这个镜像要**保留并复用于 GPU 启动渲染测试**（把它当成"已安装的 macOS"来引导）。
- 需要新实验环境时，先确认能否复用现有镜像；确实需要新镜像，**必须先问操作者**。
- 2026-11-19 傍晚：App（APK）的数据目录已由操作者清空，所以现在唯一的安装资产
  只能是 CLI 跑出来的那一份。**不要用 App 再装一遍**，那也是 64 GiB。

## 为什么要做

CLI（`tools/build-install-android.sh`）编的是 `pvg_metal_stub.c`，Metal replay 是空实现，
所以 **GPU 通路只能在 APK 里验证**——每轮都要人工点 UI，反馈慢。

但 `android/reims_pvg.c` 的接口表明这件事可以拆开：

```c
HvReimsPvg *hv_reims_pvg_create(...);          /* 不需要窗口 */
void hv_reims_pvg_attach_window(p, window);    /* 显示是可选的，后传 */
int hv_reims_pvg_have_display(p);              /* 0 = 没有显示面，设备照样跑 */
```

头文件原话：*a device whose display was refused still runs, it just has nowhere to show a frame*。

## 第一步（30 秒，决定后面全部）

**验证 `shell` 用户能不能用 Adreno 的 Vulkan。** 这是唯一未验证的前提。

写一个最小探测程序（不进仓库，`/tmp` 里）：

```
vkCreateInstance -> vkEnumeratePhysicalDevices -> 打印 deviceName / apiVersion
```

用 NDK 编译成 aarch64-linux-android 可执行文件，推到 `/data/local/tmp` 跑。

- **能枚举到 Adreno 740** → 继续第 2 步
- **枚举不到（或 loader 只返回空表）** → CLI 这条路不通，GPU 仍只能在 APK 里调；
  那就把精力放在 APK 的 GPU 调试上（`probe_vk.c` / `probe_ns.c` 已有现成探测）

## 第二步：CLI 接入 reims（若第 1 步通过）

在 `tools/build-install-android.sh` 里：

1. 加宏 `-DHENGE_HAVE_REIMS=1`
2. 编译 `android/reims_pvg.c`
3. 链接 `android/third_party/reims-vgpu/target/aarch64-linux-android/release/libreims_vgpu_c.a`
   （1 GB，链接一次要几分钟）
4. 加 `-lvulkan`

注意：`devices/vmapple.c` 里**安装模式仍走 `pvg_lite`**（`cfg->run_installer1/2` 分支），
所以接入 reims 不影响安装流程，只在**正常启动 macOS** 时生效。

## 第二步半：把帧读回来，存成图片

**现状**：`hv_reims_pvg_poll()` 在没有窗口时**只打印** `reims: frame WxH`，不做编码：

```c
if (p->frame_pending && p->have_mode) {
    p->frame_pending = 0;
    if (p->log) fprintf(stderr, "reims: frame %ux%u\n", p->mode_width, p->mode_height);
}
```

真正的呈现走的是 `attach_android_window()` → `rvg_display_attach_android_window()`，
所以 CLI（无 Surface）目前**没有任何像素输出**。

**做法（不改 reims）**：实现一个**假的 `ANativeWindow`**。
`ANativeWindow` 的本质是一张函数表 + 一个 buffer 队列，`rvg_display_attach_android_window()`
拿到它之后会走标准的 `dequeueBuffer` / `queueBuffer` 流程。我们自己实现这几个回调，
在 `queueBuffer` 里把 `ANativeWindowBuffer` 的数据拷出来，写成 PPM/PNG。

```
假 ANativeWindow（CLI）
   dequeueBuffer -> 返回我们自己的 buffer
   queueBuffer   -> 拿 ANativeWindowBuffer 的像素，存 /tmp/frame-%04d.png
   perform       -> NATIVE_WINDOW_SET_BUFFERS_FORMAT/SIZE 等，记录参数
```

好处：**reims 一行不改**，同一份代码在 APK（真 Surface）和 CLI（假 Surface）上都能渲染，
而且 CLI 侧能拿到确定的图片文件，用来判断渲染内容对不对（颜色/几何/文字），
而不是只看"有没有创建成功"。

## 第三步：用"非安装"模式跑

```
./host-install <booter> <已安装的 disk.img> <workdir> 2048 2
```

（不带 IPSW 参数 → `vc.installer = 0` → 走 reims 分支）

期望看到：
- `reims: guest mode WxH, IOSurface format 0x...`
- `hv_reims_pvg_have_mapper() == 1`（IOSurface mapper 起来）
- 假 window 被 attach：`reims: display attached to the host's window`
- **`/tmp/frame-0001.png` 之类真的出现，且内容不是全黑/全绿**

## 最终目标（分阶段）

1. **安装跑完**（当前：ASR 22%，磁盘 3.3 GB）——数据走 USB，不需要 guest 网络
2. **GPU 正常渲染**（CLI 离屏 → 出图片；APK → 出画面）
3. **再谈封装/UI**（只有前两步成立，UI 才有真实内容可显示）

## 已知的坑

| 坑 | 记在哪 |
|---|---|
| `reims-vgpu: memory.map.mapping_length_unaligned` → `death::die` → abort | 安装模式曾因它崩溃；已用 `pvg_lite` 绕开，但**正常启动时它仍会出现**，需要修（对齐映射长度） |
| `metal2vulkan` 的降级项（NativeNanEncoding / NativeMinMax / 收敛性） | 只影响渲染精度，不影响启动 |
| 链接 1 GB 静态库 | 增量构建没事，干净重建会很慢 |

## 当前落点（第二天接手先看这里）

- **APK 已经能真正安装**：`disk blocks` 0 → 6.7 M（约 3.3 GB），进度到过 22%，
  `ramrod_probe_media_internal` / GPT / 合成 APFS 容器都在跑。
  那次运行的数据目录**已被操作者清空**，不再是资产。
- 根因是**编译选项不一致**，见 `docs/jni-cli-build-parity-2026-11-19.md`（提交 `3035700`）。
- **接下来用 CLI 跑一次完整安装**（`/data/local/tmp/henge`，workB），
  跑完保留 `workB/disk.img`，之后 GPU 测试直接复用，不再重装。
