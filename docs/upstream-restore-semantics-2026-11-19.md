# 恢复模式：上游的苹果语义，与我们相对上游的细微差异

基线：`origin/main` = **`e98920c`**（上游 Leviidev/Henge）。
我们的分支 `dev` 相对它有 **217 个提交**。

本文只做两件事：

1. **把上游在"安装/恢复模式"里的执行链和它对应的苹果语义写清楚**（上游自己怎么做的）；
2. **逐项列出我们在这条链路上改了上游的什么**，并给分级。

判定口径沿用 `docs/semantic-deviations.md` 的 E/X/D/U：

| 等级 | 含义 |
|---|---|
| **E（等效）** | guest 看到的行为不变（日志、平台适配、纯宿主超时） |
| **X（扩展）** | 上游没有这条路径，我们补上；上游原本在这里会降级/失败 |
| **D（刻意偏离）** | 行为与上游不同，且有证据表明上游在此处不对 |
| **U（未验证）** | 有偏离，但没有证据判定等效 |

本文**不重复** `core/*` 的差异审计（见 `docs/semantic-deviations.md`），只覆盖**恢复链路**。

---

## 1. 苹果侧语义：VZMacOSInstaller 是什么流程

上游源码里的这句话就是准确的苹果语义（`host/install.c` 文件头）：

> *"Installing macOS on a fresh virtual Mac, the way Virtualization.framework's
> `VZMacOSInstaller` does: the machine boots its ROM in DFU mode and shows up as a USB device,
> and a standard restore (personalized with Apple's signing server for this machine's ECID, then
> the restore OS writes the disk) runs over that link. … Nothing of Apple's is decrypted or
> patched here: encrypted images go to the guest as they are, and the restore OS asks for its
> keys itself (the host just fetches the URL)."*

拆成事实：

| 环节 | 苹果的定义 | 谁做 |
|---|---|---|
| 机器身份 | `ECID`（虚拟机的唯一芯片 ID） | 宿主传给 restore 栈 |
| 固件 | `AVPBooter.vmapple2.bin`，启动后先跑 **DFU 模式** | guest |
| 呈现给宿主 | 机器在宿主 USB 上**像一个 USB 设备**（DFU → Recovery → Restore 三种模式） | 设备模型 + USB 通路 |
| 个性化 | 用 Apple 的 **TSS**（签名服务器）按 ECID 签出本机可用的镜像 | idevicerestore |
| 数据 | 加密镜像**原样**发给 guest；密钥由 restore OS 自己通过 `RequestKeys` 要 | guest 内的 restored |
| 落盘 | restore OS（`restored` / `restored_external`）写 root disk 与 auxiliary storage | guest |
| 宿主角色 | 只搬 USB 数据 + 代取 URL，**不解密、不打补丁** | `host/libusb_shim.c` |

两张镜像的形状也是苹果的（上游 `henge-vm.c:253` / 我们的 `android/install.c:96`，两边**逐字节一致**）：

```
auxiliary storage: 24 字节 header  a5 d5 b3 fe 97 ad 1d 27 01 00... 01 00...
                   之后全零，总大小 32 MB + 16 KB = 33 570 816
root disk:         sparse 文件，大小由用户定（默认 64 GiB，写入前不占空间）
```

---

## 2. 上游的实现：一条链，五个层次

上游（iOS）**没有** Android 的 CLI/APK 之分，安装入口是 Swift → `henge-vm.c` → `host/install.c`。
完整执行顺序如下（行号对 `origin/main`）：

### 2.1 入口：`henge_vm_install()`（`ios/Henge/Native/henge-vm.c:222`）

```
1. 取 JIT 区域（iOS 必需：可执行页只能由 debugger 给）
2. HvVmConfig{ .installer = 1, .no_sound = 1, .jit_rx/.jit_rw/.jit_bytes }
3. hv_vm_create(&c)        ← 机器在此创建，ROM 进 DFU
4. hv_install_start(hv_vm_board()->vusb, ipsw, workdir, ecid, on_install_progress, NULL)
5. 进度的 step 直接映射 idevicerestore 的 RESTORE_STEP_*，100=DONE、101=FAILED
```

`.installer = 1` 是**整个恢复模式的开关**：它让 ROM 进 DFU，并把 USB device port 交给宿主侧。

### 2.2 `hv_install_start()`（`host/install.c:72`，**只有 94 行的文件**）

```
1. hv_libusb_shim_attach(usb)      ← 把标准 libusb API 接到 virtio-usb
2. 起 usbmuxd 线程：
     usbmuxd -f -p -P NONE -S 127.0.0.1:27100+pid%800 -C <workdir>
   （-P NONE：不用 PnP；-S：私有 loopback socket；USBMUXD_SOCKET_ADDRESS 指过去）
3. usleep(300ms)
4. 起 idevicerestore 线程：
     idevicerestore_client_new → FLAG_ERASE（HV_INSTALL_LOG 时 +FLAG_DEBUG）
     set_ipsw / set_cache_path / set_ecid / set_progress_callback → start()
```

**这就是上游恢复模式的全部宿主编排**：一个 usbmuxd、一个 idevicerestore、一个 USB shim。
其余所有事情（DFU、iBoot、内核、ramdisk、restored）都发生在 guest 里。

### 2.3 USB 通路：libusb_shim ↔ virtio-usb ↔ guest

```
idevicerestore / libirecovery
        │  标准 libusb API
        ▼
host/libusb_shim.c          ← 上游只做"把调用翻译成 virtio 请求"
        ▼
devices/virtio_usb.c        ← AppleVirtIOUSBDeviceController 的对端（PCI 106b:1a01）
        ▼
guest: AppleVirtIOUSBDeviceController / IOUSBHostFamily
```

### 2.4 guest 内的引导链（恢复模式）

```
AVPBooter (DFU)
  → iBoot Stage1 → Stage2 (iBEC)
  → kernelcache + ramdisk + device tree
  → XNU (RELEASE_ARM64_VMAPPLE)
  → launchd bootstrap
  → restored / restored_external  ← 恢复 OS 在这里写盘
```

### 2.5 idevicerestore 的三个等待点（理解卡点必须知道）

| 等待 | 触发条件 | 上游如何满足 |
|---|---|---|
| "Waiting for device to disconnect" | `recovery_enter_restore_mode`：真实硬件在 `bootx` 时复位 USB | 由设备模型自然产生 |
| "Waiting for device to enter restore mode" | 重新枚举且出现 `restored` 服务 | 由重新枚举自然产生 |
| 进度回调 | idevicerestore 的 `RESTORE_STEP_*` | 回调到 UI |

---

## 3. 苹果侧的设备语义（我们模拟的是哪些苹果设备）

VMAPPLE 是 Apple Virtualization.framework 的 guest 平台。上游的设备模型对应关系：

| 设备（我们的文件） | 苹果侧的名字/语义 | 参照 |
|---|---|---|
| `devices/vmapple.c` | AVPBooter 平台、配置区、序列口、sealed/ctrr | XNU `RELEASE_ARM64_VMAPPLE`；QEMU `hw/vmapple/*` |
| `devices/bdif.c` | Apple 的 boot/DFU 通道（USB 请求 + 块请求共用门铃） | QEMU `hw/vmapple/vmapple.c`（**名称未在苹果公开文档中确认，标注不确定**） |
| `devices/virtio_usb.c` | `AppleVirtIOUSBDeviceController`（PCI `106b:1a01`） | 实测 PCI ID + XNU kext |
| `devices/cfg.c` | `VMAppleCfg` 配置结构（版本/CPU/ECID/RAM/MAC/serial…） | QEMU `hw/vmapple/vmapple.c` 的 cfg 结构 |
| `devices/vmapple.c` 的 `avp-rtc` | `AppleVirtualPlatformRTC`：`0x30240040` 读 `0xf001` 表示"用 PL031" | iBootStage2 的探测逻辑（上游注释） |
| `devices/pcie.c` | ECAM 配置空间 | PCIe 规范 |
| `devices/pvg*.c` | ParavirtualizedGraphics（PVG） | QEMU `hw/display/apple-gfx.m` |
| `devices/virtio_blk.c` | root/aux 两块 virtio-blk | QEMU vmapple |
| `devices/xhci.c` | USB3 控制器（**restore 不走它**，走 virtio-usb） | 规范 |
| `devices/gicv3.c` / `pl011.c` / `pl031.c` / `pl061.c` | ARM 标准外设 | 规范 |

> 苹果语义里"恢复模式"只要求：ROM 能 DFU、USB 能被宿主看见、restore OS 能写盘。
> 这些在苹果那边由 Virtualization.framework 的虚拟硬件提供，**没有公开的设备规范文档**；
> 唯一可对拍的实现是 QEMU 的 `hw/vmapple/*`（macOS 12 guest），版本较旧。

---

## 4. 我们相对上游的差异（恢复链路）

### 4.1 第三方恢复栈（`third_party/`）——**恢复能否开始的关键**

| # | 文件 | 上游 | 我们 | 等级 |
|---|---|---|---|---|
| T-1 | `usbmuxd/src/usb.h`、`usb.c` | `usb_device_add()` 只认 T2（0x8600）、Apple Silicon restore（0x1901–0x1905）、常规范围 | 增加 **恢复模式范围 `0x1280..0x1283`**（实测 guest 报 `0x1281`）。没有它 usbmuxd 直接 `return -1`，设备列表为空，`restore_check_mode()` 超时 | **X** |
| T-2 | `idevicerestore/src/recovery.c` | `setenv boot-args <restore_boot_args>`（256 字节缓冲） | 追加 `HV_RESTORE_BOOT_ARGS`（缓冲改 512），并打印实际发送的命令 | **X** |
| T-3 | `idevicerestore/src/restore.c` | 把 DeviceTree 原样发给 iBoot | 在 `DeviceTree`/`RestoreDeviceTree` 前**注入空的 `/chosen/memory-map` 节点**（`adt_add_memory_map`） | **X / U** |
| T-4 | `libimobiledevice-glue/src/thread.c` | `cond_wait_timeout` 原样 | `HENGE_TIMEOUT_SCALE` 放大所有等待 | **E**（纯宿主） |
| T-5 | `restore_config/config.h` | 依赖 `<net/if_dl.h>` | Android 无它：改用 `AF_PACKET`（`HAVE_NETPACKET_PACKET_H`） | **E**（平台） |

**T-3 必须单独说明**：这是**改 guest 的设备树**，不是宿主适配。文件注释的理由是
"iBoot 自己造的 memory-map 会用没有任何保留的地址，导致 kernelcache 在启动时被覆盖"。
这个判断**来自我们自己的观测**，没有苹果/QEMU 源码佐证，所以标 **U**，不要当成定论。

### 4.2 设备模型（`devices/`）——**恢复握手的关键**

| # | 文件 | 上游 | 我们 | 等级 |
|---|---|---|---|---|
| V-1 | `virtio.c` `common_write(0x14)` | 只记 `status` | guest 写 ACKNOWLEDGE（status bit0）时**主动拉起 INTx**（`hv_pci_set_intx`），模拟"设备已连接"；`HV_VUSB_INTX=0` 关。理由：`AppleVirtIOUSBDeviceController` 在 start 里等这个事件，否则协商停在 0x1、队列永不启用 | **X** |
| V-2 | `virtio_usb.c` | 直接 `hv_virtio_add_msi()`？——**上游没有 MSI**（INTx only） | 默认**也不加** MSI（`HV_VUSB_MSI=1` 才加）。注释：Apple 传输层对 vendor 0x106b 选单向量 INTx，广告 MSI 会让驱动等一个不会来的中断 | **E**（与上游一致） |
| V-3 | `virtio_usb.c` | 无 | 新增 `hv_vusb_force_rescan()`：`rom_on=0; session++; broadcast`，**不动 virtio 队列**，让宿主重新枚举 | **X** |
| V-4 | `bdif.c` | 少量打印 | 采样日志（`log>=3` 每 32 条）、通道上下线、请求不在 RAM、读失败 errno、失败诊断（缓冲是否在 guest RAM/镜像大小） | **E** |
| V-5 | `vmapple.c` | 无串口转发 | 串口逐行转发 `hengevm: serial:`、`HENGE_STOP_ON`、`HENGE_STOP_EVERY`、panic 关键词扩展（`panic(cpu `/`Panic`/`assert`/`Fatal error`）、`HV_AVP_LOG` 或 `log>=2` 打开 ctrr/sealed 日志 | **E**（`HENGE_USB_REENUM` 为 **X**，默认关） |
| V-6 | `pcie.c`、`xhci.c`、`pl011.c`、`timer.c` | — | 诊断/日志 | **E** |

### 4.3 宿主层（`host/`）——**USB 枚举与中断行为**

| # | 文件 | 上游 | 我们 | 等级 |
|---|---|---|---|---|
| H-1 | `libusb_shim.c` | 枚举前 `hv_usb_reset(g_usb, 2000)`，之后各 descriptor 超时 2000 ms | **默认不发 reset**（`HV_RESET_ENUM=1` 恢复上游行为）；reset 超时 150 ms、descriptor 500 ms；失败枚举按 250 ms 退避 | **D / U** |
| H-2 | `libusb_shim.c` | 无总线状态日志 | 打印 guest 总线 on/off 与 session | **E** |
| H-3 | `usb_host.c` | control OUT 超长时直接截断 | 默认只**警告**（行为不变），`HV_USB_CTRL_CHUNK=1` 才按 guest 接收缓冲（16 KiB）分块 | **E**（默认） |
| H-4 | `disk.c` | `F_NOCACHE`（macOS 第二描述符） | Android 无该标志：`posix_fadvise(POSIX_FADV_DONTNEED)` 把写过的范围推出去（`HV_WRITE_CACHE=0` 路径） | **E**（平台） |
| H-5 | `hvsparse.c` | `SEEK_DATA/SEEK_HOLE` | bionic 不声明，手写 `3/4` 并运行期探测 | **E**（平台） |
| H-6 | `jit_region.c` | iOS：debugger 给的可执行区 + `vm_remap` | Android 无 `vm_remap`：**一个 memfd 映射两次**（R-X 执行 / R-W 写同一页） | **E**（平台） |
| H-7 | `virtio_balloon.c` | `F_PUNCHHOLE` / `MADV_FREE_REUSABLE` | `fallocate(PUNCH_HOLE|KEEP_SIZE)` / `MADV_DONTNEED` | **E**（平台） |

### 4.4 安装编排（新增，`android/install.c`）

`android/install.c`（1546 行）是**新增**的 Android 编排，但**它复用上游 `host/install.c`**
（`#include "install.h"` → `hv_install_start()`）。所以恢复栈本身仍是上游那一套，
我们加的是"把机器跑起来 + 观察"：

| 内容 | 说明 | 等级 |
|---|---|---|
| VM 创建与 CPU/kicker 线程 | 对应上游 `host/vm.c` 的宿主侧 | E |
| `hv_android_make_aux()` / `make_disk()` | header/大小与上游 `henge_make_aux()` **逐字节一致** | E |
| `usb_handoff_to_kernel()` | guest 首次执行内核代码（`pc >= 0xfffffe0000000000`）时调 `hv_vusb_force_rescan()`，**主动制造一次"设备断开"** | **X** |
| 日志（`stderr_to_logcat`、限速、全量文件） | 纯输出 | E |
| `hv_threads_dump()`、`scan_proc_names()`、`hv_svc_dump_tail()` | 引擎内读 guest RAM，纯只读 | E |
| 时间基测量 | 启动时打印 `timebase measured`，纯输出 | E |

**`usb_handoff_to_kernel()` 是恢复模式里最"主动"的一个偏离**：它人为让 USB 链路断一次，
以满足 idevicerestore 的 "Waiting for device to disconnect"。上游靠设备模型自然产生这个事件；
我们因为 bdif 只报"上线"不报"下线"，只能在这里补。

---

## 5. 汇总：恢复链路上真正改变 guest/宿主交互的只有这几处

按"是否改变行为"排序（E 类不计）：

| # | 改动 | 为什么必须 | 风险 |
|---|---|---|---|
| 1 | usbmuxd 认恢复模式 PID `0x1280..0x1283` | 否则设备根本不可见 | 低（只放宽过滤） |
| 2 | `usb_handoff_to_kernel()` 主动 bounce USB | 否则 idevicerestore 等不到 disconnect | **中**：人为事件，时机依赖 `pc` 首次进内核 |
| 3 | `virtio.c` 在 ACKNOWLEDGE 时拉 INTx | 否则 USB 协商停在 0x1 | **中**：模拟了一个硬件中断，时序可能与真机不同 |
| 4 | `libusb_shim.c` 默认不发 vendor reset | reset 之后内核侧设备端不再投递接收缓冲，枚举永久失败 | **中**：与上游行为不同，属于 D |
| 5 | 设备树注入 `/chosen/memory-map` | 否则 iBoot 自造 map 可能覆盖 kernelcache | **高**：直接改 guest 设备树，属 U |
| 6 | `HV_RESTORE_BOOT_ARGS` 追加 boot-args | 调试需要（`serial=3`） | 低（默认不设） |
| 7 | `HENGE_TIMEOUT_SCALE` | 模拟器比真机慢 | 低（默认 1） |

---

## 6. 结论与下一步

1. **上游的恢复语义是清楚的**：`VZMacOSInstaller` 那一套——DFU ROM + USB + ECID 个性化 +
   restore OS 写盘；上游的实现只有 94 行的 `host/install.c` 加一个 USB shim，
   恢复逻辑全部在 guest 内。
2. **我们的移植没有重写恢复栈**：`android/install.c` 只是把同一套 `hv_install_start()` 跑起来，
   外加日志和观察工具；第三方恢复栈的补丁也是为了"让设备可见/让 boot-args 生效"。
3. **真正的偏离集中在 5 处**（上表 2–5 是行为偏离，1、6、7 是必要条件）：
   两处 USB 中断/枚举时序（`usb_handoff`、INTx）、一处不发 reset、一处改设备树。
   **这四处正是恢复模式"跑得动但状态不真实"的候选来源**，与 §10.2 的整机死锁问题需要
   分开对待：它们影响的是恢复**能否开始**，不解释"开始之后为什么全局停滞"。
4. 尚未验证、仍需核对的：
   - **T-3 的 `/chosen/memory-map` 注入**是否真的必要（对照：不注入时 iBoot 的实际行为）；
   - **H-1 不发 reset** 对真实恢复流程的影响（上游为什么要发）；
   - **V-1 的 INTx** 在真机上由谁产生（是 `AppleVirtIOUSBDeviceController` 自己等，
     还是设备真的应中断）；
   - **`bdif` 的苹果语义**（名称/寄存器布局）没有公开文档，只能对 QEMU。

> 这份文档只记录"与上游/苹果语义的差异"，**不推断死锁原因**。
> 死锁的现状与下一步见 `docs/lost-wakeup-2026-11-19.md` §7 与 `docs/investigation-tree.md` §9。
