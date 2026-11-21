# 第一次画出 guest 的画面（2026-11-21，CLI 离屏）

**结论先行**：guest 自己的合成器（macOS 的 WindowServer）已经通过我们整条 GPU 链路把画面画出来了。
抓到的第 3 帧是 **macOS Setup Assistant 的欢迎屏**（阿拉伯语 "مرحبًا"，即 "你好"），1920×1080，
**2 052 038 / 2 073 600 像素非黑**，`PageFault` 与 `AccessRevoked` 均为 0。

**范围限制（必须写清）**：这是 **CLI（`tools/host-install.c`）的离屏抓帧路径**。
`reims: status` 里 `window=0`、`host presents` 只有个位数 —— 说明**窗口/APK 的显示路径还没有验证**。
「CLI 能画出图像」≠「App 里能显示」，后者是下一件事。

---

## 1. 成功渲染的确切环境

### 设备

| 项 | 值 |
|---|---|
| 型号 / 板 | PJE110（OnePlus）/ `kalama`（SM8550） |
| GPU | Adreno 740 |
| 系统 | Android 15，`OnePlus/PJE110/OP5CF9L1:15/TP1A.220905.001/U.1d94395_275952_27eb03:user/release-keys` |
| 内核 | `5.15.167-android13-8-o-01144-gdc8278c1c5f9` |
| 宿主页 | 4 KiB（guest 内部仍是 16 KiB 页） |
| adb | `adb -s emulator-5554`（真机，`adb connect 127.0.0.1:5555` 的约定名） |
| shell 身份 | uid=2000（`shell`），无 root |

### 驱动

| 项 | 值 |
|---|---|
| 驱动 | **Mesa Turnip**（`/data/local/tmp/henge/driver/libvulkan_freedreno.so`，9 411 112 B，md5 `7fbf3afbfa159cfceff02fb1c0fc53eb`） |
| 加载方式 | **libadrenotools 命名空间**（`henge_vk_open_adrenotools`，目录必须以 `/` 结尾），不是 `dlopen` 系统驱动 |
| 能力（实测） | `Turnip Adreno (TM) 740: 145 extensions｜external_memory_host=no｜dma_buf=YES｜ahb=YES` |
| 厂商驱动对照 | `Adreno (TM) 740, 1.3.128: 114 扩展｜external_memory_host=no｜dma_buf=no｜ahb=YES` |

### 代码版本

- 主仓库 `dev`：`2b295a5`（metal2vulkan 46e7cc96 的 vendor + 我们的改动 + `tools/m2v-*`）
- 子模块 `android/third_party/reims-vgpu`（分支 `rebuild`）：`7cbd8f3`
- translator：`steelbrain/metal2vulkan` @ `46e7cc96a284150bdf19b4a6a7890b0a8bc0ef71`（branch `development`），
  经 `tools/m2v-compile-layer.sh` 从 cargo checkout 复制到 `build/m2v-compile-layer` 并打
  `tools/patches/m2v-rog-unordered.patch`

### 运行命令（可复现）

```bash
# 设备侧，/data/local/tmp/henge
HV_RAM_NO_DMABUF=1 \
REIMS_ROG_UNORDERED=1 \
HV_GRAB_GPU=1 HV_GRAB_EVERY=45 \
HV_VK_DRIVER=/data/local/tmp/henge/driver \
HV_BOOT_ONLY=1 HV_LOG=1 \
  ./host-install AVPBooter.vmapple2.bin workB/disk.img workB 2048 2 > run.log 2>&1
```

产物：`workB/gpu-NNN.bmp`（GPU 帧，1920×1080×32）与 `workB/fb-NNN.bmp`（EFI 固件帧，640×1136，只有进度条）。

### 实测判据

```
reims: status | window=0 mode=1920x1080 | new frames=3002 | guest presents=3002 | host presents=5
display present #N: port 0 format 0x52476841 1920x1080 stride 15360 base 0x...: sampled 20615 pixels, 1125 non-black, max channel >0
gpu-003.bmp: 1920x1080 bpp=32 non-black=2052038 bbox=(0,0)-(1919,1079)   ← macOS 欢迎屏
fb-003.bmp:  640x1136  bpp=32 non-black=5604   bbox=(86,131)-(553,142)   ← 固件进度条
PageFault 0, AccessRevoked 0, pipeline refused 0
```

## 2. 让它从"全黑"变成"有画面"的那一处

vendored runtime 的 `copied.rs`：

```rust
// 上游
let drops_writes = read_only || !mapped_writable(start, start.saturating_add(length));
// 我们（Android bring-up）
let drops_writes = false;
```

原因：**Android 15 上对 guest RAM 页问"可写吗"会得到否定答案**（尽管它就是我们
`mmap(PROT_READ|PROT_WRITE)` 的 memfd，guest 每秒都在写），于是每个"拷贝"都被标成
"GPU 写会被丢弃"，render pass 的 store-out 触发 `PageFault`，而 Metal 语义里同队列第二个
自造 GPU 错误就**吊销队列**，之后所有提交 `AccessRevoked`，屏幕因此全黑。

拷贝本来的语义是对的：`copied.rs` 在每次完成时把 GPU 改过的字节 **merge_back 回调用者的页**。
这条路径就是"虚拟化层自己实现写入语义"（方案 A）：guest 无从分辨 GPU 是直写它的页，
还是写我们的映像再合并回去。

## 3. 这一轮一起解决的其它 Android 坑

| 现象 | 根因 | 处理 |
|---|---|---|
| `PlacementConflict::Held`（同地址二次 pin）| 46e7cc96 的 `DeviceProperties` 是每个 generation handle 自己的副本，`ptr::eq` 永远不等 → 同地址重复 `reserve_address_pool` | 改按 `registry_id()` 认物理设备 |
| `RasterOrderGroups` 被拒 46 次 | Turnip **没有** `VK_EXT_fragment_shader_interlock`（Mesa 全树未实现），组挂在"被使用的 buffer/texture"上就必须有 interlock | `REIMS_ROG_UNORDERED=1`：translator 里把该组降级为无序（记录为偏离，默认关） |
| `fstatfs(dma-buf fd)` 得 `EACCES` | Android 15 拒绝在 dma-buf 文件系统上 `statfs` | 改用 `/proc/self/fd/N` 的 `/dmabuf` 名字判断（runtime 与 reims 都改） |
| dma-buf 窗口是分散页 | VMAPPLE 的 IOSurface 页在 guest RAM 里是散的，一个窗口跨多个 run；Linux 靠 udmabuf 的 `UDMABUF_CREATE_LIST` 合成一个 dma-buf，**Android 没有 udmabuf** | 源返回"无 dma-buf 服务这些页" → runtime 走拷贝（写会合并回去） |
| Metal `Private` 位（bit 24）出现在 no-copy buffer 上 | 真 Metal 里非法；VMAPPLE guest 会这么传，而在本机这些页就是 guest 的 RAM | `no_copy_options` 剥掉该位且不丢写 |

## 4. 现在的默认与开关

- **guest RAM 默认 memfd**：dma-buf RAM 的代码在（`HV_RAM_DMABUF=/dev/dma_heap/system` 可开，写路径已验证能分配 2 GiB），
  但因为 surface 的页是散的，dma-buf 会让 IOSurface 的纹理创建失败；等"多 chunk / bounce"支持之后再切。
  `HV_RAM_NO_DMABUF=1` 是显式写法，用来锁死复现条件。
- `REIMS_ROG_UNORDERED=1`：无序降级（默认关）。
- `REIMS_WITHHOLD_ROAA=1`：隐藏 `VK_EXT/ARM_rasterization_order_attachment_access`（用于组挂在颜色输入的情形）。

## 5. 已知未完成

1. **窗口/APK 路径**：`host presents` 只有个位数，`window=0`；离屏抓帧通 ≠ 屏幕显示通。
2. DIAG 探针还在树里（`DIAG …` 的 `eprintln!`），需要清理。
3. guest 停在 Setup Assistant——需要交互（或换一条自动化路径），不是 GPU 的问题。
4. `dma-buf RAM` 的零拷贝路径：需要支持"一个窗口散落在多个 dma-buf 块"（或做 bounce + 我们自己的回拷）。
