# 明天的工作：用 CLI 跑 GPU 的离屏渲染（2026-11-19 记）

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

## 第三步：用"非安装"模式跑

```
./host-install <booter> <已安装的 disk.img> <workdir> 2048 2
```

（不带 IPSW 参数 → `vc.installer = 0` → 走 reims 分支）

期望看到：
- `reims: guest mode WxH, IOSurface format 0x...`
- `hv_reims_pvg_have_display() == 0`（无窗口，正常）
- `hv_reims_pvg_have_mapper() == 1`（IOSurface mapper 起来）
- guest 的 GPU 驱动能完成初始化

## 已知的坑

| 坑 | 记在哪 |
|---|---|
| `reims-vgpu: memory.map.mapping_length_unaligned` → `death::die` → abort | 安装模式曾因它崩溃；已用 `pvg_lite` 绕开，但**正常启动时它仍会出现**，需要修（对齐映射长度） |
| `metal2vulkan` 的降级项（NativeNanEncoding / NativeMinMax / 收敛性） | 只影响渲染精度，不影响启动 |
| 链接 1 GB 静态库 | 增量构建没事，干净重建会很慢 |

## 今天的落点（第二天接手先看这里）

- **APK 已经能真正安装**：`disk blocks` 0 → 6.7 M（约 3.3 GB），进度 22%，
  `ramrod_probe_media_internal` / GPT / 合成 APFS 容器都在跑
- 根因是**编译选项不一致**，见 `docs/jni-cli-build-parity-2026-11-19.md`
- 最新提交 `3035700`
- 22% 之后磁盘增长变慢，需确认它是卡住还是在做慢操作（`henge-serial.log` 看最后一条）
