# 功能 ↔ 底层对接计划

界面已经成形（三 tab + 推入页），但**大多功能还没有接到 guest 上**。本文列出每一项：
现在有什么、还差什么、动哪些文件、有什么风险、大概什么量级。

排序原则：先让「装好的 macOS 能用」（输入、音频、保活），再让「长期可用」（快照、网络、共享），
最后才是「像产品」（多实例、外接屏、剪贴板）。实测安装跑通之前，前一组之外的都不要动。

---

## 0. 现状基线

| 能力 | 状态 |
|---|---|
| JIT / MMU / 设备模型 | 通（Stage 0 真机全绿） |
| 图形（reims-vgpu → metal2vulkan → Vulkan） | 通（真机建出机器、管线、IOSurface、display） |
| 引导循环（CPU 线程 + kicker + 磁盘 + ROM） | 通（`hv_android_vm_boot`） |
| 安装栈（idevicerestore 全链路 + curl→Java） | 编译链接通过，**未实测** |
| 输入 / 音频 / 网络 / 快照 / 共享 / 多实例 | 未接 |

---

## 1. 输入注入（触摸、键盘、鼠标）

**现成的东西**：`devices/virtio_input.c` 已经实现 virtio-input（键盘 + 相对坐标鼠标），
`hv_virtio_input_send(dev, type, code, value)` 可从任意线程投递，`hv_virtio_input_poll` 在 CPU tick 里交付。
`hv_vmapple_create` 已经建好 `keyboard` 与 `mouse` 两个设备。

**要做的**：
1. `android/henge_vm.c`：暴露 `hv_android_input(kind, type, code, value)` 与
   `hv_android_input_axis(x, y, absolute)`，内部转发到 `g_board.keyboard` / `g_board.mouse`。
2. `android/probe_jni.c`：JNI `nativeTouch(action, x, y, pointerId)`、`nativeKey(keyCode, down)`、
   `nativeScroll(dx, dy)`；坐标归一化在 Java 侧做（SurfaceView 的 px → 0..32767 绝对坐标）。
3. `DisplayActivity`：`SurfaceView` 上挂 `OnTouchListener`；点击=左键，双指拖动=滚轮，
   长按=右键，外部键盘 `onKeyDown/onKeyUp` 直接映射。
4. 设置项：「触摸模式：指针 / 直接点按」「反转滚动」——做完再放，不放空开关。

**风险**：macOS 把"绝对坐标 + 无左键"的设备识别成触控板，手势会走 macOS 自己的（双指滚动、三指拖动）；
需要真机试出哪种更顺手。左侧边缘手势要留出来给系统返回。

**量级**：1–2 天（设备已存在，主要是坐标与手势映射）。

---

## 2. 音频

**现成的东西**：`devices/virtio_snd.c` 已有，`hv_vmapple_create` 里 `hv_virtio_snd_create`，
可用 `HV_NO_SOUND` 关掉。

**要做的**：AAudio 输出线程（`AAudioStream`，低延迟模式，44.1/48 kHz 由设备定），
把 guest 的 PCM 环形缓冲喂给 AAudio；时钟以 AAudio 的 `presentationTime` 为准反向调
virtio-snd 的播放位置。JNI：`nativeAudioStart/Stop`。

**风险**：时钟漂移会导致爆音或断续，要按 AAudio 的时间戳做重采样/丢弃，不是简单 memcpy。

**量级**：2–3 天，含调延迟。

---

## 3. 保活（前台服务）

**现状**：guest 线程绑在 `DisplayActivity`；退到后台还在跑，但系统随时可以回收。

**要做的**：把 VM 生命周期从 Activity 挪到 `ForegroundService`（`android:foregroundServiceType`
用 `specialUse` 需在 manifest 声明理由），Activity 只做显示绑定；显示页销毁不再停 VM，
改为由服务持有并在通知里提供"停止"。

**风险**：Android 14+ 对前台服务类型查得严；`specialUse` 要在上架说明里写清。
不做这个，长时间跑（macOS 装好之后是长期开着的）一定被系统杀掉。

**量级**：1 天。

---

## 4. 驱动加载路径统一（已知的"小修改"）

**问题**：我们现在有两条 Vulkan 路线，互不知情。
- App 侧：`libadrenotools` 把内置 Turnip（`libvulkan_freedreno.so`）注入到 `libvulkan.so` 的命名空间里，
  Stage 0 的探测走的是这条。
- 引擎侧：`reims-vgpu` 的 `metal2vulkan` 自己 `dlopen("libvulkan.so")`，拿到的是**系统 loader**，
  看到的永远是高通原厂驱动，看不到 Turnip。

**要做的**：让 reims 的 Vulkan 实例创建接受外部传入的 `vkGetInstanceProcAddr`（它已经有
`metal_registry_id` / `feature_level` 之类的注入点，加一个 loader 钩子即可），或者至少让它在
我们指定的路径 `dlopen`。两种都可以，前者更干净。

**为什么现在不改**：原厂驱动实测可用（Adreno 740 + Turnip 都通了）；等真实 macOS 起来之后，
按实际渲染错误决定用哪个驱动，再决定改哪边。

**量级**：半天（reims 侧改动很小），但需要 `reims-vgpu` 重新交叉编译 + 真机验证。

---

## 5. 快照（挂起 / 恢复）

**现成的东西**：`reims-vgpu` 有整套 `pause/unpause/will_suspend/finish_suspend/will_resume/did_resume`
接口，正是为快照准备的；`host/snapshot.c` 里有 iOS 的实现。

**要做的**：把 `host/snapshot.c` 重写成 Android 可用版本——它依赖 GCD + blocks + zlib，
bionic 没有 BlocksRuntime。改成 pthread + condvar，压缩用 zlib（Android 自带）。
磁盘用 `hvsparse` 的增量（已有）。

**风险**：快照格式与 guest 版本绑；恢复失败要能明确报错而不是静默起一台坏机器。

**量级**：3–4 天。

---

## 6. 网络

**现状**：`devices/virtio_net.c` 已有，`host/net_slirp.c` 未移植（libslirp 没 vendored）。

**要做的**：vendored libslirp（约 1 MB 源），或改用一个用户态 NAT（Android 无 TUN 权限，
`VpnService` 需要用户授权且与 VM 的 IP 栈要重写）。前者是唯一现实路径。

**风险**：DNS、DHCP、以及 macOS 的 mDNS。`host/dns_proxy.c`（iOS）依赖 `dns_sd.h`，Android 要重写。

**量级**：1 周。

---

## 7. 共享文件夹 / 剪贴板

**现状**：`host/share.c`（iOS）用 `copyfile.h`；Android 没有。

**要做的**：共享目录走 SAF（`DocumentsContract`）或直接绑定应用自己的外部目录；
剪贴板需要一个 guest 内 helper（virtio-serial 通道），不是纯宿主能做。

**量级**：共享 3 天；剪贴板 1 周（要写 macOS 侧）。

---

## 8. 多实例

**现状**：`android/henge_vm.c` 用全局 `g_board`，只能一台。

**要做的**：把板子 + 机器 + 线程收进一个 `HvInstance` 句柄，JNI 全部改成带句柄；
UI 侧 `Prefs` 从单份配置改成按实例 id 分组。

**量级**：2–3 天，但会碰到所有 JNI 签名，宜早不宜晚（越晚越贵）。

---

## 9. 显示相关的小尾巴

- 光标：`reims-vgpu` 有 cursor 回调（`RvgCursorGlyphFn` / `ShowFn` / `MoveFn`），
  接上才能在 macOS 里看到自己的指针；不接就是"没鼠标"。
- 分辨率：现在 guest 自己定，宿主不报尺寸；接上之后可以按 SurfaceView 实际像素设置缩放策略。
- 外接显示：`display_port_count` 已可配，第二个 surface 就能出一块外接屏。

---

## 10. 顺序建议

```
实测安装（现在）→ 输入（1–2 天）→ 音频（2–3 天）→ 前台服务保活（1 天）
   → 驱动统一（半天，按实测决定）→ 光标/分辨率（1 天）
   → 快照（3–4 天）→ 网络（1 周）→ 共享/剪贴板 → 多实例
```

每一步都按「先真机验证，再写文档」办；没验证的能力不进设置界面。
