# restore 阶段性结论（2026-11-16 第二轮）

设备：emulator-5554 / Android 15 / 4KB 页；CLI `build/andcli/host-install`；
IPSW `24G90`；`HV_EXTRA_BOOT_ARGS="serial=3 debug=0xA"`。

**一句话状态：host 侧的恢复握手已经打通到「等 restore-mode 设备」这一步；卡住的原因是
restore 阶段 guest 的 USB 端没有建立（bdif 不再 online、virtio-usb 的 kext 停在
`prepareDefaults`），而同期用户态有 800 次 el0 translation fault（restored 崩溃循环）。**

---

## 1. 本轮确实修好并验证过的

### 1.1 XHCI（输入设备路径，不是 restore 阻塞点）

四处修复，同一条命令前后对比：

| 指标 | 修前 | 修后 |
|---|---|---|
| `forcePowerGated ... after 1001ms` | 反复 | 0 |
| `did not generate an event` | 多次 | 0 |
| `failed to create or open pipe zero` | 多处 | 0 |
| `device descriptor fragment is invalid` | 有 | 0 |
| `enumeration failed` | 有 | 0 |
| `wMaxPacketSize 0x0000` | 有 | 0 |
| `enumerated 0x05ac` | 0 | 2（键盘 + 触控板） |

修的是：LWS 的任何链路状态变化都要产生 PLC（原来只有 U0/resume）；status stage 无条件产生
Transfer Event（IOC 只门控中断）；以及我自己一度加错又撤销的「按 ERDP 同步 event index」。

**但 restore 走 virtio-usb（bdif），不走 xhci**，所以这四处不是 restore 的阻塞点，是附带收益。

### 1.2 host 侧恢复握手（第一段，已通过）

```
18:40:22  HengeVM: sending [setenv boot-args rd=md0 nand-enable-reformat=1 -progress -restore]
18:40:23  Waiting for device to enter restore mode...        ← 第一段已经过了
18:45:23  Device failed to enter restore mode.  (5 分钟)
```

三件事让它过去的：

1. **usbmuxd 的 PID 白名单没有 restore mode**。上游表只有 `0x1290..0x12af`（正常模式）、
   `0x8600`（T2）、`0x1901..0x1905`（Apple Silicon restore）；设备报的是 **`0x1281`**，
   比 `0x1290` 还小 → `usb_device_add()` 直接 `return -1`，`idevice_get_device_list()`
   永远空，`restore_check_mode()` 永远失败。补 `0x1280..0x1283` 后日志出现：
   ```
   [18:40:12.743][4] Found new device with v/p 05ac:1281 at 1-2
   [18:40:14.855][4] Found new device with v/p 05ac:1281 at 1-3
   ```
2. **iBEC 执行 `bootx` 时我们的模拟没有断开 USB**。`bdif` 只会写 `REG_UNK2=1`
   （online），从不写 0，所以 device 一直挂着，host 的
   `cond_wait_timeout(30000)` 空等到 `Failed to place device in restore mode`。
   现在在 **guest 首次执行内核代码**（pc 进入 `0xfffffe00...`）时做一次
   `hv_vusb_force_rescan()`，主机侧看到 `device left` → 第一段通过。
3. **`force_rescan` 不能复用 `set_online(0)`**：`set_online` 会 `drop_all` 掉请求队列，
   把 restore kernel 正在握手的 virtio 队列扔掉（表现为 kext 打完 `prepareDefaults`
   就再没下文）。`hv_vusb_force_rescan()` 只 `session++`，不动 guest 状态。

`ignition sequence complete` 之后 8 秒再做一次 rescan，用于第二段（等 restored 上线）。

### 1.3 工具：宿主 USB 链路测试（秒级，替代 3 分钟一轮）

`tests/test_usb_link.c`，11 项检查，`0 of 11 differ`：

```
cc -o build/test/test-usb-link tests/test_usb_link.c host/libusb_shim.c \
   -I. -Ithird_party -Ithird_party/libusb-1.0 -Idevices -Ihost -Icore -lpthread
```

它用脚本化的 guest 替代 virtio（只 mock 6 个符号：`hv_usb_reset`/`hv_usb_control`/
`hv_vusb_online`/`hv_vusb_send`/`hv_vusb_send_in`/`hv_vusb_recv`），验证：
arrived / left / 重新 arrive 的顺序，reset 无应答时仍能枚举，离线后设备列表为空，
以及**枚举占住事件路径的时长**（这正是设备上「48 线程全在 futex、日志停住」的原因）。

顺带修了 shim 三处：枚举不再要求 vendor reset 应答（restore kernel 的终端根本不会答）、
reset 超时 2000ms→150ms、失败枚举 250ms 退避（`HV_RESET_ENUM=1` 可恢复旧行为做对照）。

### 1.4 一个操作层面的坑（要记住）

`host/libusb_shim.c`、`host/usb_host.c`、`host/install.c` 是编进
**`build/android-arm64-v8a/lib/libhengerestore.a`** 的（`android/build-restore.sh`），
`tools/build-install-android.sh` 只链接它。**改这三个文件后必须重跑 `android/build-restore.sh`**，
否则二进制里还是旧逻辑。本轮为此白跑了两轮（验证方法：
`strings build/andcli/host-install | grep "<新字符串>"`）。

---

## 2. 现在卡在哪（证据链，按顺序）

```
host: setenv 发出 → bootx → 等 disconnect            ✓ 通过（force_rescan）
guest: restore kernel 起来（arm_init → kext → launchd）✓
guest: ignition sequence complete                     ✓
host: "Waiting for device to enter restore mode"      ✗ 5 分钟超时
```

不是 host 不认设备，而是**设备侧根本没建立 USB 通道**：

| 观察 | 数据 |
|---|---|
| `bdif usb: channel online` | 3 次，**全部在 iBEC 阶段**；restore kernel 阶段 0 次 |
| `AppleVirtIOUSBDeviceController` | 只出现 1 次：`prepareDefaults: model VirtualMac version 2.1`，之后无日志 |
| virtio 配置写（`HV_VIRTIO_CFG_LOG`） | 只有 device config 读（`config read +2..+14`），**没有一次 queue size/enable 写** |
| host 的 `GET_DESCRIPTOR` | `enumeration failed`；请求进不了 guest（`deliver_requests` 见不到，因为 `q[REQQ].enabled == 0`） |
| `hv_vusb_force_rescan` 后 | host 看到 `device left`，但再也没有 `arrived` |

即：**iBEC 交棒给内核时，USB 通道没有交接成功** —— bdif 不再 online（符合 iBEC 已退出），
而内核的 virtio-usb 设备端没有完成初始化（不写队列），于是谁都不响应 host。

---

## 3. 已排除的方向（都做了实验，别再重走）

| 假设 | 结论 | 依据 |
|---|---|---|
| 共享库映射失败 = `shared_region` 问题 | **无关** | `shared_region_map_and_slide_setup()` 每个失败分支都会 `SHARED_REGION_TRACE_ERROR`（默认 ERROR），日志一条都没有 → dyld 从未调用该旧 API |
| ramdisk 挂载失败 | 无关 | `md0s1 mount-complete`、`rootfs mount flags 0x480d001` |
| kext 没加载 | 无关 | 219 kext、`PCI configuration end`、键盘/触控板枚举 |
| 设备模型（virtio-usb / virtio / bdif / pcie）被我们改坏 | **排除** | `devices/virtio.c`、`devices/pcie.c` 与 `origin/main` **零差异**；`virtio_usb.c` 只多本轮的 `force_rescan` 与日志；`bdif.c` 只多日志 |
| JIT 代码生成（`translate.c`）回归 | **排除** | 把 `core/translate.c` 换成 `origin/main` 版本后现象完全不变（`enumeration failed` + kext 停在 `prepareDefaults`） |
| JIT 优化（superblock / K2）导致语义偏差 | **排除** | `work/knobs.txt` 写 `HV_SUPERBLOCKS=0` / `HV_K2=0` 跑，结果相同 |
| `rom_on` 未释放导致 kext 不启动 | **不成立** | 在 handoff 时强制 `rom_on = 0` 跑，现象不变（已保留该改动，因为它语义正确） |
| xhci 的 4 个 bug 阻塞 restore | 无关 | restore 走 virtio-usb/bdif，不走 xhci |

---

## 4. 未定性的一项（目前最像根因）

**801 次 el0 data abort**，且集中在「每段映射的首页」：

```
58×  esr 92000007  far 0x100908000  pc 0x100af2ebc
18×  esr 92000007  far 0x100908000  pc 0x100b6c2b8
18×  esr 92000007  far 0x100908000  pc 0x100b4436c
11×  esr 92000007  far 0x104b7c000  pc 0x100b6c2b8
 6×  esr 92000007  far 0x10b840000  ...
```

- `ESR 0x92000007` = EC 0x24（lower EL data abort）+ DFSC 0x07（translation fault, **level 3**）
- 同一时刻**代码页是可执行/可读的**（`HV_EL0_DUMP` 拿到的 12 页是 arm64e 系统库代码，
  含 `autibsp`、结构遍历），说明 TEXT 段映射了、其它段没有
- 这解释了 restored 的**崩溃-重启循环**，也就解释了 bdif 为什么在 restore 阶段不 online
  （restored 还没活到初始化 USB）

**为什么还没定性**：`HV_PTW_LOG` 只追踪「walk 用到的页表页」且上限 512 页，存在假阴性，
不能用它证明「guest 从未映射」。为此已在 `core/exception.c` 加了
`hengevm: el0 fault far ... walk: fsc ... pa ... L3 ...`（前 10 次），
判读方式：

- `fsc == 0` 且 `pa` 有效 → guest **已经映射**该页，fault 来自我们 fast path 的
  stale TLB 条目 → 我们的 bug
- `fsc == 0x07` → guest 确实没建立 L3 项 → dyld 的映射没生效，继续查 dyld/ramdisk

---

## 5. 下一步（按顺序）

1. 跑一轮看 `el0 fault far ... walk:` 的 `fsc`（判据见上）。这是当前唯一未定的关键项，
   它决定往「我们的 TLB/页表」还是「guest 的 dyld 映射」两个方向走。
2. 若 `fsc == 0`（我们侧）：查 `hv_tlb_refill` / fast path 的页表缓存失效条件，
   特别是 `TTBR0` 切换与 `pmap` 修改页表后 guest 是否发了 TLBI。
3. 若 `fsc == 0x07`（guest 侧）：确认 ramdisk 里 `dyld_shared_cache*` 是否真的存在
   （`/tmp/ramdisk.img` 明文里 `dyld_shared` 命中 0 次；需排除 APFS 压缩导致 grep 不到），
   并用 `HV_PCHK` 打点 `_shared_region_map_and_slide` 观察 dyld 是否调用它、参数是什么。
4. USB 通道那一段（bdif 不 online、virtio-usb kext 停在 `prepareDefaults`）很可能是第 1 项
   的下游症状：先修页映射，再看它是否自行恢复；不要同时改设备模型。
5. Android 移植层还缺一块没触发但一定会挡的：`HV_HALT_RESET` → 重新引导
   （iOS 版有 `HENGE_VM_OUTCOME_RESTART`；`android/install.c` 收到 halt 直接停）。

---

# 第三轮补充（20:00）：el0 fault 被排除，卡点收窄到 vusb 的 ACKNOWLEDGE

## 6. el0 translation fault 不是 bug（已排除）

给 fault 的 `far` 加了一层 walk 输出后，判据很清楚：

```
far 102548000 walk: fsc 0x7 pa 0 L3 9cc58000          ← 首次访问：L3 未映射
far 102548000 walk: fsc 0x0 pa 9d698000 L3 9cc58000   ← 同一地址第二次：已映射，pa 有效
far 10254c008 walk: fsc 0x7 ... → fsc 0x0 pa 9d658008
far 102550000 walk: fsc 0x7 ... → fsc 0x0 pa 9dbc8000
```

同一个地址第一次 translation fault、第二次 walk 成功 —— guest 的 fault handler 确实在
page-in，我们的 MMU 也正确读到了新页表项。1655 次 fault 是 dyld 逐页加载 dylib 的正常过程
（`far` 递增）。**「共享库映射失败」这条线到此关闭。**

因此第二轮文档 §4 里「el0 fault 最像根因」的判断作废。

## 7. 卡点收窄：vusb 的 virtio 协商停在 ACKNOWLEDGE

给 virtio 的 common config 加 trace 后（`HV_VIRTIO_CFG_LOG=1`），两个设备的差别一目了然：

| 设备 | 序列 |
|---|---|
| virtio-input `1af4:1052` | `status 0x1 → 0x3 → 读 features → 0xb → 设队列 → 0xf(DRIVER_OK) → reset`（协商完整） |
| **vusb `106b:1a01`** | `status 0x0 → 0x1(ACKNOWLEDGE) → 读 status → 停`（**从未读 device features**） |

（注意：`1af4:1052` 是 virtio-input，config 里那个字符串是 `HengeVM`；把 trace 对准 vusb
需要匹配 `106b:1a01`。第一版 trace 就错在这里，看的是键盘的复位循环。）

## 8. 反汇编定位（符号表可用）

`AppleVirtIOUSBDeviceController` 不在 `AppleVirtIO`，而在
**`com.apple.driver.AppleVirtualPlatform`**（kernelcache fileoff `0x3325b0`，
`__TEXT_EXEC` vmaddr `0xfffffe000904ddd0`）；transport 在 `com.apple.driver.AppleVirtIO`
（fileoff `0x32d7d0`）。两个 kext 都有完整符号表，关键符号：

```
AppleVirtIOUSBDeviceController::start                 0xfffffe000905b904
AppleVirtIOUSBDeviceController::goOnBus               0xfffffe000905c16c
AppleVirtIOUSBDeviceController::prepareConfiguration  0xfffffe000905cf8c
AppleVirtIOUSBDeviceController::handleUSBReset        0xfffffe000905d77c
AppleVirtIOUSBDeviceController::startNonIsocEndpoint  0xfffffe000905d57c
AppleVirtIOPCITransport::start                        0xfffffe00090232b8
AppleVirtIOPCITransport::configureQueueInterrupt      0xfffffe00090242fc
AppleVirtIOPCITransport::assignInterruptAction        0xfffffe00090240f4
AppleVirtIOPCITransport::findNextMSIInterruptIndex    0xfffffe000902450c
AppleVirtIOPCITransport::usesMultipleInterruptVectors 0xfffffe00090245ec
```

`usesMultipleInterruptVectors()` 的判据（反汇编）：

```asm
ldrh w8, [x0, #0x132] ; cmp #1        → 必须是 1
ldrh w8, [x0, #0x130] ; cmp 0x1af4    → 只对标准 virtio vendor 用多向量
```

vusb 的 vendor 是 **`106b`（Apple）**，所以它走**单向量 INTx**；键盘（`1af4`）走多向量且
成功了。所以中断类型不是直接原因，但 `configureQueueInterrupt` / `assignInterruptAction`
在 ACKNOWLEDGE 之后被调用，是 next candidate：我们的 `hv_pci_init` cap 链只有 virtio 的
4 个 cap（common/ISR/device/notify），**没有 MSI(0x05) / MSI-X(0x11)**。

`AppleVirtIOUSBDeviceController::start` 本身只做对象分配与虚调用（PAC 签名 `blraa`），
virtio 初始化在父类（transport）里 —— 也就是 ACKNOWLEDGE 由 transport 写，写完后它调用的
下一步失败，于是 `status=0` 重来。

## 9. A/B 结论（宿主与设备逐行一致，排除平台）

宿主（glibc）跑的 vusb 序列与设备（bionic）**逐行相同**：`status 0 → 0x1 → 停`。
所以不是 Android 环境（bionic / 线程 / 4KB 页 / proot）的问题。

- `core/translate.c` 换回 `origin/main` → 现象不变（排除 JIT 代码生成回归）
- `core/exception.c` 换回上游 → **链接失败**（上游缺我们新增的 `hv_exc_ring`、
  `hv_hvc_count`、`hv_hvc_last_pc` 等符号），这条 A/B 需要先补适配层
- `devices/virtio.c`、`devices/pcie.c` 与上游**零差异**；`devices/virtio_usb.c` 只多本轮改动
  → 设备模型不是回归源

## 10. 下一步（按假设强度排序）

1. **给 virtio PCI 加 MSI/MSI-X capability**（真实 virtio 设备都有；transport 里有
   `findNextMSIInterruptIndex` / `configureQueueInterrupt`），看 vusb 是否越过 ACKNOWLEDGE。
   这是反汇编直接支撑的假设，改动小。
2. 若无效：确认 `AppleVirtIOQueue` + `AppleVirtIOInterruptEventSource` 在 INTx 路径下能否
   建立（`assignInterruptAction` 需要 provider 提供中断资源；xhci 能工作说明 PCI 中断本身可用）。
3. 若仍无效：做「上游 core + 最小适配」的整份对照（需要给上游 core 补我们新增的符号/字段，
   工作量较大，应事先约定范围）。

## 11. 环境变化（用户删除 work/ 之后）

- `work/` 被删除 → **Android CLI 构建需要重建 `work/andcli` 的 curl/openssl 静态库**
  （`tools/build-install-android.sh` 依赖它们）；宿主构建不需要
- `work/AVPBooter.vmapple2.bin` 已从设备 `/data/local/tmp/henge/` 取回
- `work/ca-bundle.pem` 由系统证书重建（`/etc/ssl/certs/ca-certificates.crt`）

## 12. ACKNOWLEDGE 之后失败点的追查（两个假设已被实验否掉）

### 12.1 Apple vendor-specific PCI capability —— 否掉

反汇编 `AppleVirtIOTransport::getFirstAppleVendorDataCapability`（`0xfffffe0009038eb4`）：
它遍历 PCI 的 vendor-specific capability，找 **vendor == 0x106b** 的那个，找不到返回 NULL。
我们的 virtio cap 链里没有这种 cap（`add_cap` 写的是 virtio 布局，bytes 2..3 是 len/type）。

实验：只对 vusb 在链尾加一个标准 vendor cap（`09 00 6b 10 …`），宿主重跑 ——
**vusb 仍然停在 `status=0x1`**，行为完全不变。该假设否掉（改动保留，可用
`HV_NO_APPLE_CAP=1` 关闭）。

### 12.2 PCI slot / INTx 线号 —— 否掉

`hv_pci_set_intx` 用 `line = slot`，`update_line` 再接到 `intx_base + line`；DT 里
`pcie/interrupt-base = 0x40`（64），`VMAPPLE_IRQ_PCIE = 0x20`（32）→ base=64，slot 7 → 线 71，
在 GIC 范围内。而创建顺序让 vusb 落在 **slot 7**，xhci 落在 slot 3（工作）。

实验：把 vusb 提前创建（slot 1，`HV_VUSB_LATE=1` 可恢复原顺序），宿主重跑 ——
**仍是 `status=0x1`**。slot 假设否掉。

### 12.3 目前唯一还站着的定位

`AppleVirtIOPCITransport::setInterruptsEnabled(bool)`（`0xfffffe0009024e90`）：

```asm
strb w1, [x0, #0xe0]        ; 记录期望状态
ldr  x0, [x0, #0xd8]        ; 依赖 start 里分配的对象
... 调用其 +0x150 的方法 ...
cbz  w0, <失败路径>          ; 返回 0 就整体失败
```

`start` 里 `mov w0, #2; bl …; str x0, [x19, #0xd8]` 说明 `[+0xd8]` 是某一类 OSObject
（DMA 命令 / 队列一类）。virtio-input 走同一段代码却成功，说明该路径本身可用；差别只剩
「这个对象对 106b:1a01 的这个 transport 实例做了什么」。

再往下需要 guest 侧的证据（IOLog 在该失败路径上是静默的），可行手段：
`HV_PCHK` 在 `setInterruptsEnabled` 入口与 `cbz` 后的失败块打点，看是哪一次调用把 w0 变成 0。

## 13. 环境：work/andcli 已用脚本重建（后台）

`tools/build-andcli-deps.sh`：按会话里验证过的参数重编

- OpenSSL 3.0.15：`./Configure android-arm64 -D__ANDROID_API__=26 no-shared no-tests --prefix=work/andcli/ossl`
- curl 8.23.0：`--host=aarch64-linux-android26 … --with-openssl=work/andcli/ossl`（`LIBS=-ldl`）

产出 `work/andcli/{ossl,curl}/lib/*.a`，`tools/build-install-android.sh` 依赖它们。

## 14. 根因链闭合：PCI 缺 MSI capability → 中断源建不起来 → ACKNOWLEDGE 后中止

### 14.1 符号解析（把 `bl` 的 imm26 算准，objdump 对裸二进制显示的目标不可靠）

`AppleVirtIOPCITransport::start` 里的两个分配：

```
mov w0,#2 ; bl 0xfffffe0008475f64   →  OSArray::withCapacity(unsigned)   → [x19+0xd8]
            bl 0xfffffe000855290c   →  IOWorkLoop::workLoop()             → [x19+0xc8]
```

所以 `[+0xd8]` 是一个**容量 2 的 OSArray**（两个队列的中断源），不是 DMA 对象。

### 14.2 两个 gate

`AppleVirtIOPCITransport::setInterruptsEnabled(bool)`（`0x9024e90`）：
对那个 OSArray 调虚方法，返回 0 就整体失败（`cbz w0, <失败块>`），中途还有
`OSMetaClassBase::safeMetaCast`（类型检查）。

`AppleVirtIOPCITransport::assignInterruptAction(...)`（`0x90240f4`）：

```asm
bl   [x0+0xa88]        ; findNextMSIInterruptIndex()  —— 这就是 gate
tbz  w0, #0, <失败>     ; 失败直接返回，中断源**不加入** OSArray
bl   ...               ; IOInterruptEventSource 创建
[x19+0xd8](OSArray) +0x1e8   ; 把中断源 add 进数组
```

### 14.3 为什么 `findNextMSIInterruptIndex` 会失败

IOPCIFamily 的符号说明了它依赖什么：

```
IOPCIBridge::resolveMSIInterrupts(IOService*, IOPCIDevice*, uint, uint)
IOPCIMessagedInterruptController::getDeviceMSILimit(IOPCIDevice*, uint)   ← 读 MSI cap 的 MMC
IOPCIMessagedInterruptController::initDevice / enableDeviceMSI
```

**我们的 virtio PCI capability 链里没有 MSI capability（0x05）** → `getDeviceMSILimit` 得 0 →
`resolveMSIInterrupts` 分配不到向量 → `findNextMSIInterruptIndex` 返回 false →
`assignInterruptAction` 不把中断源放进 OSArray → `setInterruptsEnabled` 失败 →
`performDeviceInitialization` 在写完 `ACKNOWLEDGE` 之后中止 → `status=0` 重来。

于是 guest 永远不 enable 请求队列，host 的 `GET_DESCRIPTOR` 无人响应，restore-mode 设备
不出现 —— 这就是 `"Waiting for device to enter restore mode"` 五秒到五分钟超时的源头。

`virtio-input`（`1af4:1052`）不受影响，因为 `usesMultipleInterruptVectors()` 对标准 virtio
vendor 返回 true，走另一条分支，不吃这个 gate。

### 14.4 修复方向

给 PCI 设备加 MSI capability（`0x05`，Message Control 的 MMC ≥ 1 → 至少 2 个向量），
并处理 guest 对 Message Control / Address / Data 的写入；中断注入改为「按 guest 配置的
MSI 地址写数据」，落点是 `VMAPPLE_GIC_MSI`（`0x1fff0000`）那个 gic-msi 帧 ——
`hv_mmio_lookup()` + 该区间的 write 回调即可，不需要绕过 MMIO。

只对 `106b:1a01` 打开（`virtio-input` 现在的路径是通的，不要动它）。

## 15. 更正与再收窄（MSI 基础设施其实是通的）

### 15.1 两处错误判断，先纠正

1. **「gic-msi 帧被访问 0 次」是 grep 错前缀**。MMIO 直方图里明确有
   `gic-msi +0x0008 r 1 w 0` —— guest **确实**读了 MSI 帧的 type 寄存器，
   说明 `AppleVirtualPlatformPCIEMSIController` **被创建了**。
2. 因此「MSI controller 没起来」不成立；相应的 §14 结论要从「根因」降级为
   「必要但不充分」。

### 15.2 init 的实际条件（反汇编 `AppleVirtualPlatformPCIEMSIController::init`，`0x906a4c8`）

```
cbz  x1, <fail>                     ; provider 必须非 NULL
blraa [provider+0x308], "msi-frame-index"   ; 取 DT 属性
cbz  x0, <fail>                     ; 属性必须存在（我们的 DT 有：msi-frame-index = 2）
...
ldr  x23, [x19+0xc0] ; str x0,[x19+0xc0]    ; 保存/替换 MSI 内存映射
...
blraa [provider+0x710], index, 0    ; 初始化
```

创建后读 `gic-msi +0x8`：我们返回 `1<<31 | 96<<16 | 64` = `0x80600040`
→ base INTID 96、**上限 64 个向量**，`addDeviceInterruptProperties` 的
`cmp w8,w2 / b.ls` 不会挡住 2 个队列。

### 15.3 所以剩下的唯一闸门

`AppleVirtIOPCITransport::findNextMSIInterruptIndex`（`0x902450c`）里
`blraa [provider+0x738]`（provider = IOPCIDevice，x0=索引，x2=输出），
**返回非 0 即失败**。这个 `+0x738` 是对象虚表内的槽位，静态无法映射到符号名，
需要运行期证据：

- 在 `devices/pcie.c` 的 `ecam_read` 里对 vusb 的 `0x80..0x9f`（MSI cap 区）打印，
  确认 `IOPCIDevice` 到底有没有读我们新加的 MSI capability；不依赖 `HV_PCI_LOG` 的 slot 参数
- 若从未读 → cap 链没被遍历到（检查 `0x71` 的 next 指针与 `wmask`）
- 若读了但值不对 → 修 cap 内容，再跑

### 15.4 现状（宿主与设备一致）

```
vusb-cfg: status = 0x0 → 0x1(ACKNOWLEDGE) → 读 status → 停
gic-msi: +0x8 读过一次，门铃 +0x40 从未写（没有 MSI 中断发生）
```

宿主（glibc）与设备（bionic，Android CLI 已重建）**逐行一致**。

## 16. 「就绪」的判定条件（反汇编确认，这是继续修的依据）

### 16.1 `IOService::getInterruptType(index, out)`（`0xfffffe00084fe198`，内核主二进制）

```asm
ldr  x8, [x0, #0x80]        ; interruptSources 数组
cbz  x8, <error 0xe00002df>          ; ①
tbnz w20, #0x1f, <error>             ; ② index < 0
ldr  w8, [x21, #0x78]
cmp  w8, w20 ; b.le <error>          ; ③ index >= count
ldr  x0, [x8, #0x80] 经 ubfiz(index*16)  ; ④ sources[index] == NULL
cbz  x0, <error>
... controller->getInterruptType(+0x890)
```

四条全满足才算就绪，否则返回 `0xe00002df`。

### 16.2 MSI controller 恒成功

```asm
AppleVirtualPlatformPCIEMSIController::getInterruptType (0x906a758):
    cbz x3, <err 0xe00002c2>
    w0 = 0 ; *out = 0x10000    ; byte[+2]=1 → findNextMSIInterruptIndex 的 bit0 判据通过
```

### 16.3 specifier 的安装方

```asm
AppleVirtualPlatformPCIEInterruptResolver::setDeviceInterrupts(service, spec, count)  (0x90664e4)
    cbz x1 <err>; cbz x2 <err>; cmp w3,#1 ; b.ne <err>    ; count 必须正好 1
AppleVirtualPlatformPCIEInterruptResolver::resolvePCIInterrupt(...)                  (0x9066384)
    ... blraa [x21+0x160] ; cmp w0,#4 ; b.ne <err>        ; 要求返回 4 才继续
AppleVirtualPlatformPCIE::deviceInterruptController()                                (0x9069dd4)
_gIOInterruptSpecifiersKey  / _gIOInterruptControllersKey  (IOPCIFamily 的键)
```

### 16.4 当前结论

vusb 的 `IOPCIDevice` 上**没有 interrupt source**（数组空 / count 0 / `sources[0]==NULL`），
因此 `IOService::getInterruptType(0)` 返回 `0xe00002df`，
`findNextMSIInterruptIndex` 的 `cbnz w0` 直接失败（**不是**循环），
`assignInterruptAction` 不建中断源 → `OSArray` 空 → `setInterruptsEnabled` false →
`ACKNOWLEDGE` 之后中止。这也解释了为什么加 MSI capability 单独无用。

要修的是「让 resolver 给 vusb 装上 interrupt specifier」，而不是继续加 PCI capability。

## 17. specifier 安装链的完整反汇编（结论补全）

### 17.1 `IOPCIBridge::resolveMSIInterrupts(bridge, service, dev, slot, pin)`（`0x99a3b04`，IOPCIFamily）

```asm
ldrb  w8,[global+0x641] ; tbnz w8,#4, 0x11ba8     ; ① 全局 MSI 使能位
ldr   x1,[global+0x668] ; call [dev+0x2c8](x1)    ; ② 设备是否已带 MSI 属性
cbnz  x0, 0x11c5c                                 ;    有 → 用现成
ldr   x8,[bridge+0x98]  ; cbz x8, 0x11c04         ; ③ bridge 的中断控制器
ldr   x9,[x8+0x20]      ; cbnz x9, 0x11c04        ;    已初始化 → 放弃
ldr   x1,[x8+0x6c0]                               ; ④ 平台函数名
call  [bridge+0x6e8](x1, slot, 0, 0, 0)           ;    callPlatformFunction
```

### 17.2 `AppleVirtualPlatformPCIEInterruptResolver::callPlatformFunction`（`0x9066264`）

按 **OSSymbol 名** 分派：

```asm
ldr x1,[global+0xfe0] ; call [name+0x208]   ; resolvePCIInterruptFunctionName
cbz w0, <下一个> ; 匹配 → 0x185b4 (resolvePCIInterrupt)
ldr x1,[global+0xfe8] ; call [name+0x208]   ; setDeviceInterruptsFunctionName
匹配 → 0x18598 (setDeviceInterrupts)
```

### 17.3 五个可能的断点

| # | 断点 | 归属 |
|---|---|---|
| ① | 全局 MSI 使能位（`[global+0x641]` bit4） | guest 内部状态 |
| ② | 设备已带 MSI 属性（`[dev+0x2c8]` 非 0） | guest 内部状态 |
| ③ | `[bridge+0x98]` 为空 / `[+0x20]` 非空 | guest 内部状态 |
| ④ | platform function 名不匹配 | **可被我们的设备模型影响** |
| ⑤ | `resolvePCIInterrupt` 的 `cmp w0,#4` 失败（`0x9066444`） | **可被我们的设备模型影响** |

成功率最高的下一步是**运行期确认 ④/⑤ 有没有发生**：在 `devices/pcie.c` 里对 vusb 的
`0x3c/0x3d`（interrupt line / pin）和 `0x80..0x9f`（MSI cap）加定点日志。resolver 要算
INTID 必须读 interrupt pin；如果 guest 连 `0x3d` 都没读过，说明它压根没进 resolve 流程（①②③ 之一），
而不是我们的 PCI 设备哪里不对。

## 18. INTx specifier 链与 ACKNOWLEDGE 之后（2026-11-16 下午，实测 + 反汇编）

### 18.1 MSI capability 之前根本没装到 vusb 上（真 bug，已修，提交 2f2b5c9）

`hv_virtio_init()` 建的每个 virtio 设备都是 `1af4:xxxx`，`virtio_usb.c` 之后才把 id 改成
`106b:1a01`；而 MSI cap 的构造写在 `hv_virtio_init()` 里，条件是 `config[0]==0x6b`，**那一刻
vendor 还是 `0x1af4`**，所以永远不命中。实测证据（修前）：`pci vusb: read 080 (1) = 4`
（cap id `0x04` = Slot ID，不是 MSI 的 `0x05`）。修后：`read 080 (4) = 0x00020005`
（MSI，MMC=1，next=0），guest 随后 `read 082 (2) = 2` 解析了 Message Control。

**但 status 仍然停在 `0x1`** —— 因为 `usesMultipleInterruptVectors()` 只在 PCI vendor
为 `0x1af4` 时为真，vusb 走**单向量 INTx** 路径，MSI cap 对它无用。这个修复本身有价值
（能力描述合法了），但不是解锁点。

### 18.2 INTx specifier 确实被安装了（实测 + 反汇编）

`IOPCIBridge::resolveLegacyInterrupts(bridge, service, dev)`（`0x99a3c48`）：

```asm
w1 = 0x3d ; bl <configRead8>              ; 读 interrupt pin
sub w8,w0,#5 ; cmn w8,#-4 ; b.lo <err>    ; pin 必须 1..4
w21 = pin - 1
ubfx x4,[dev+0xa8],#11,#5                 ; slot
call [bridge+0x6e8]("ResolvePCIInterrupt", 0, service, pin-1, &out)
cbz  w0, <0x11d3c>                        ; 成功
  否则：读 0x3c(interrupt line) & 0xf 当 specifier
<0x11e88>:
call [bridge+0x6e8]("SetDeviceInterrupts", 0, dev, &spec, 1)   ; count == 1
```

两条路径**都会**走到 `SetDeviceInterrupts(count=1)`。实测 `pci vusb: read 03d (1) = 1`
（只读一次，就在 `status = 0` 之前）证明这条函数**确实为 vusb 跑过**。
→ **specifier 不是瓶颈**，之前「vusb 没有 interrupt source」的推断需要修正：
`IOService::getInterruptType` 检查的 `this+0x80` 是 `registerInterrupt` 之后才填的数组，
不是 `IOInterruptSpecifiers` 属性。

### 18.3 ACKNOWLEDGE 之后的分支（`AppleVirtIOTransport::performDeviceInitialization`，`0x9038be0`）

```asm
call [this+0x900]()            ; 读 status
cmp  w0,#2 ; b.hs <0x30d10>    ; 已 >= 2 就跳过
call [this+0x908](this, 1)     ; updateStatus(1) = ACKNOWLEDGE
tbz  w0,#0, <0x30bf4>          ; 返回值 bit0 == 0 → 中止
call [this+0x908](this, 2)     ; DRIVER
<0x30bf4/0x9038d94>:           ; 中止路径
    call [this+0x908](this, 0x80)   ; updateStatus(FAILED)
    return 0xe00002e9
```

`updateStatus`（`0x9025680`）是：

```asm
status = byte[cfg+0x14] | newbits      ; OR，不是赋值
byte[cfg+0x14] = status
x0 = [this+0xb0]                       ; provider
call [provider+0x970](this, 0x12)
call [provider+0x958](this, 0x12, status|new)
→ 返回值就是 updateStatus 的返回值（即上面那个 bit0 判据）
```

### 18.4 观测与反汇编的矛盾（下一步要解的）

- 日志有 `status = 0x1`（updateStatus(1) 生效）
- 日志**没有** `status = 0x81` → 没走中止路径
- 日志**没有** `status = 0x2` → 也没走 DRIVER

所以 `performDeviceInitialization` 两次 `updateStatus` 之间就停了，**或者** vusb 的
`status = 0x1` 写入根本不是这条路径发出的（`status = 0` 与 `status = 0x1` 的 guest pc
相差 `0x118`，是两处不同代码，说明 reset 与 update 分开写，符合预期）。

**下一步（静态，可控）**：反汇编 provider `[this+0xb0]` 的槽 `0x958` / `0x970`（参数 `0x12`），
看它在什么条件下返回 bit0=0。若 `[this+0xb0]` 是 `AppleVirtualPlatformPCIE`，
`0x970`/`0x958` 就是平台侧对 "device status 变化" 的处理（很可能是
`getVendorDataCapabilities` 或 virtio 配置空间的转发，失败即返回错误）。

## 19. vtable 解析成功：provider 的三个槽全部落实（2026-11-16）

### 19.1 方法（重要，之前我在猜 PAC，是错的）

内核 kext 的指针是 **`DYLD_CHAINED_PTR_64_KERNEL_CACHE`**：

```
bits[63]    isAuth
bits[62:52] next
bits[51:50] key
bits[49]    addrDiv
bits[48:33] diversity
bits[32:31] cacheLevel
bits[29:0]  target  →  目标 VA = 0xfffffe0007004000 + target
```

校验：`IOPCIDevice` 的 `__ZTV` 前几个槽解出来是 `IOPCIDevice::~IOPCIDevice`(D1/D0)、
`OSObject::release/retain/getRetainCount` —— 与 Itanium C++ ABI 完全一致，说明公式正确。
虚函数槽 `n` 位于 `__ZTV + 0x10 + 8n`。

脚本：`/tmp/vt.py`（`dumprange <fileoff> <n>`）。实现要点：从 MH_FILESET 的每个
`LC_FILESET_ENTRY` 取符号表，统一按 VA 排序后查名字。

### 19.2 结果（`IOPCIDevice` 的 vtable）

| 槽 | 偏移 | 符号 |
|---|---|---|
| 231 | `0x738` | `IOService::getInterruptType(int, int*)` |
| 299 | `0x958` | `IOPCIDevice::ioWrite8(UInt16, UInt8, IOMemoryMap*)` |
| 302 | `0x970` | `IOPCIDevice::ioRead8(UInt16, IOMemoryMap*)` |

### 19.3 由此得到的确定事实

`AppleVirtIOTransport::updateStatus(this, newbits)` 的真实实现：

```asm
status = ioRead8(0x12) | newbits      ; 经 IOPCIDevice 的 BAR 访问
ioWrite8(0x12, status)
→ 返回值即 ioWrite8/ioRead8 的结果（performDeviceInitialization 检查其 bit0）
```

也就是说 **Apple 的 virtio transport 用 BAR 偏移 `0x12` 存设备状态**，而标准 virtio PCI
common config 里 `0x12` 是 `num_queues`、`0x14` 才是 `device_status`。
我们的 `bar_read/bar_write` 按标准布局实现，所以：

- guest 通过 `ioRead8(0x12)` 读到的不是它的状态字
- `ioWrite8(0x12, ...)` 写进去的也不是状态
- 但日志里又能看到 `vusb-cfg: status = 0x1` —— 说明还有另一条路径（直接 MMIO
  `+0x14`）在写状态，两条路径的语义不一致

**下一步（确定且具体）**：反汇编 `IOPCIDevice::ioRead8`/`ioWrite8`（`0x99bd830` /
`0x99bd9a0`），确认它们对 **memory BAR** 是走 `IOMemoryMap` 的虚拟地址还是走 I/O 端口；
再对照我们的 `bar_read/bar_write` 的偏移布局，决定是「让 0x12 承载状态」还是
「修正 transport 认为的状态偏移」。同时重新检查 `findNextMSIInterruptIndex`（`0x902450c`）
对 `getInterruptType` 返回值的处理——之前记为「非 0 立刻失败」，需要按新解析的 vtable 复核。

## 20. 实测：`m->cpu->pc` 不可用于符号定位，slide 每次不同

### 20.1 vtable 解析工具（已可用）

`/tmp/vt.py`：按 `DYLD_CHAINED_PTR_64_KERNEL_CACHE` 解 kernel collection 的指针，
`目标 = 0xfffffe0007004000 + (raw & 0x3FFFFFFF)`，虚函数槽 `n` 在 `__ZTV + 0x10 + 8n`。
校验通过（`IOPCIDevice` 前几槽 = 析构 + `OSObject::release/retain/...`）。

### 20.2 已确认（确定）

- `IOPCIDevice` 槽 231 = `IOService::getInterruptType(int,int*)`
- 槽 299 = `IOPCIDevice::ioWrite8(UInt16,UInt8,IOMemoryMap*)`
- 槽 302 = `IOPCIDevice::ioRead8(UInt16,IOMemoryMap*)`
- `ioRead8(off)` 实现为 `*(uint8_t*)(map->getVirtualAddress() + off)`，即直接读 BAR 的 MMIO 映射
- `AppleVirtIOTransport::updateStatus` 正常路径是 `byte[this+0x100 + 0x14] |= newbits`（直接内存写，
  `this+0x100` 就是 BAR 映射的 VA），并**不**在正常路径上走 `ioRead8/ioWrite8(0x12)`
  —— 后者是另一条分支（`0x90256cc`），实测从未触发（日志里没有 `read +0x12`）

### 20.3 两个工具陷阱（实测）

1. **`m->cpu->pc` 不是发起 MMIO 的指令地址**，而是 JIT 块的起始 pc。
   证据：把日志里的 status 写 pc（`0x1db396ac` / `0x1e4d42fc`）按 16 KB 对齐反推 slide，
   候选全部落在 `com.apple.kernel` 的 `__TEXT_EXEC` 内，而写 virtio status 的代码在
   AppleVirtIO kext 里。→ **不要用这个 pc 去查符号。**
2. **slide 不是固定的**：两次运行 pc 相差约 `0x969c850`，与 `core/dispatch.c:2542` 的注释
   （"slide 0x14b14000，因为 random seed 关掉了所以固定"）矛盾。
   → `HV_PCHK` 的地址必须按当次 slide 计算，否则 0 命中（本轮已实测 0 命中）。

### 20.4 下一步（明确）

放弃从 pc 反推，改用**栈**：

- `HV_STACKSCAN=1` 在卡住时 dump 内核栈，能直接看出 `AppleVirtIOUSBDeviceController`
  停在哪一层调用（是 `start` 里等事件，还是 `performDeviceInitialization` 里）
- 同时确认 `common_write` 是否需要接受 `0x12` 偏移（Apple 的 legacy 布局语义），
  但当前证据只显示 guest 访问 `0x14`，**暂不改语义**

## 21. 断点打在 MMIO 上：拿到真实调用链（2026-11-16 晚）

### 21.1 方法（这次是对的工具）

`cpu->pc` 是 **JIT 块起始**，用它查符号一定错。但 **MMIO 回调运行在 guest 停在该访问上的那一刻**，
所以 `x0..x7`、`sp`、`lr`、`el` 都是 guest 的真实值。在 `devices/virtio.c` 里加了
`vusb_state_dump()`：打印寄存器 + 用 `hv_mmu_walk` 走 guest 页表扫栈，挑出指向内核代码
（`0xfffffe..` 且半字对齐）的字。

### 21.2 由此确定的 slide 与调用链

`0x1db396ac − 0x90256ac = 0x14b14000`，且 4 个独立地址全部解析成合理符号：

| guest 地址 | link | 符号 |
|---|---|---|
| `0x1db396ac` | `0x90256ac` | `AppleVirtIOPCITransport::updateStatus(uint8)+0x2c` |
| `0x1db37934` | `0x9023934` | `AppleVirtIOPCITransport::start(IOService*)+0x67c` ← 调用者 |
| `0x1db397c4` | `0x90257c4` | `AppleVirtIOPCITransport::reset()+0x1c` |
| `0x1d0218d4` | `0x850d8d4` | `IOService::startCandidate(IOService*)+0x214` |
| `0x1d020ed4` | `0x850ced4` | `IOService::probeCandidates(OSOrderedSet*)+0x1e6c` |

### 21.3 `AppleVirtIOPCITransport::start` 在 ACK 之后做什么（确定）

```asm
reset()                                    ; status = 0
updateStatus(1)                            ; ACKNOWLEDGE    ← 日志里的 status = 0x1
bl 0x8475f64 (w0=2)                        ; 一个短延时
updateStatus(1)                            ; 再确认
call [this+0x378](this, provider, plane)   ; IORegistryEntry::attachToParent   (槽 111)
call [this+0x5e0](this, 0)                 ; IOService::registerService        (槽 188)
```

**结论：`start()` 自身是成功的**，ACK 只是它的正常一步。`DRIVER(0x2)` 与队列由**下一个驱动**
`AppleVirtIOUSBDeviceController::start`（`0x905b904`）负责，而它在 `prepareDefaults` 之后就没动静了。

### 21.4 已排除项（本轮）

- 我们伪造的 INTx：`HV_VUSB_INTX=1`（完全不拉线）与拉线两种情况下，vusb 的全部活动
  （5 条记录：`status=0`、`read +0x14`×2、`status=0x1`、`read +0x14`）**逐条一致**，
  唯一差别是 `isr read` 从 1 次变 0 次 → **中断不是卡点**
- `performDeviceInitialization`：ACK 根本不是它发的（见上），之前那条推断链作废

### 21.5 下一步

反汇编 `AppleVirtIOUSBDeviceController::start`（`0x905b904`），找它**设置 `DRIVER`/写队列寄存器**
的位置和前置条件；`prepareDefaults` 之后它应当建立 RX 队列并等 host 的 SETUP，
现在既没有 queue 写入也没有 device config 读取，说明它在那之前就放弃了。

## 22. 卡点锁定：`goOnBus()` 从未运行（2026-11-16 晚）

### 22.1 `AppleVirtIOUSBDeviceController::start` 做了什么（手工解码 bl，objdump 对裸 binary 不可靠）

```
safeMetaCast                                 0x905b95c
IOCommandPool::withWorkLoop                  0x905b9c0
AppleVirtIOUSBResponseTransaction::transaction 0x905b9fc
AppleVirtIOUSBCompletionQueue::completionQueue 0x905ba3c
IOUSBDeviceController::readyForHost()        0x905badc   ← 告诉 USB 核心"可接受 host 请求"
（之后是 stop() 的 cold 路径，都是 bounded_ptr 断言，不是正常错误）
```

### 22.2 真正协商的是 `goOnBus()`，而它从未执行

`AppleVirtIOUSBDeviceController` 有两个：

| 地址 | 符号 | 内容 |
|---|---|---|
| `0x905c0b0` | `goOnBus(bool)` | 上线开关（vtable 槽 281 / 偏移 `0x8d8`）；比较 `[this+0x1d0]` online 标志 |
| `0x905c16c` | `goOnBus()` | **真正的 virtio 协商** |

`goOnBus()` 的开头与成功结尾：

```asm
call [transport+0x900]() ; getStatus ; cmp w0,#2 ; b.hs <skip>
call [transport+0x908](1)            ; updateStatus(1) = ACK      ; cbz w0 → 失败
call [transport+0x908](2)            ; updateStatus(2) = DRIVER   ; cbz w0 → 失败
...
w1 = 4
call [transport+0x908](4)            ; updateStatus(4)
strb 1,[this+0x1d0]                  ; online = 1
call [this+0x960]()
w19 = 0                              ; 成功
```

**实测反证**：日志里 `status` 只出现过 `0x0`（reset）和一次 `0x1`（ACK）。
若 `goOnBus()` 执行过，必然出现**第二次** `status=0x1`、`status=0x3`、`status=0x4/0x5`。
**一个都没有** → `goOnBus()` 未执行。

### 22.3 推论（下一步的依据）

`start()` 成功返回后，设备控制器停在「等待上线（`goOnBus(true)`）」的等待态。
`goOnBus(bool)` 是 vtable 槽 281，但在 `com.apple.iokit.IOUSBDeviceFamily` 的
`__TEXT_EXEC` 里搜不到 `ldr/add #0x8d8` 的调用点，说明触发者在别处（或经基类槽位映射）。

下一步两条：
1. 在 `IOUSBHostFamily` / 其他 kext 里找槽 281 的调用者，确定"上线"由哪个事件触发
2. 从 DT/IORegistry 侧检查 `port-virtual-1` 是否缺"设备已连接"相关的属性
   （串口显示 `IOPortTransportStateUSB dataRole=Device`、`USB2@` registerService 成功，
   port 层是通的，缺的是让 device controller 认为 host 已连上）

## 23. `readyForHost()` 的内容 + 槽 281 无调用点（2026-11-16 晚）

`IOUSBDeviceController::readyForHost()`（`0x9b92360`）做的事（手工解码 bl）：

```
call [this+0x2d8](<string>)                      ; 槽 91
USBStringIndex::newUSBStringIndex()              ; 0x9b945b0
call [this+0xa40](this)                          ; 槽 328
IOUSBDeviceControlRequest::getScratchBufferPtr(&this->0xe8) ; 0x9b88f9c
   → str x0,[this+0xe0]
IOUSBDeviceController::createUSBDevice(OSDictionary*)       ; 0x9b9ad7c
```

即：建立字符串索引、取 scratch buffer、**构造 USB 设备对象**。所以 `start()` 结束时
guest 侧是「设备对象已建好、等 host 来枚举」的状态，缺的只是 virtio 通道。

`goOnBus(bool)`（vtable 槽 281 / 偏移 `0x8d8`）的调用者：搜遍全部 kext 的 `__TEXT_EXEC`，
`ldr/add #0x8d8` 的命中只有无关的 `stopThreadCallGated` 等 → **它不通过 vtable 偏移调用**，
触发机制是别的（函数指针字段或基类内部状态机）。

### 下一步（工具已就绪）

slide 实测为 `0x14b14000`（`0x1db37934 − 0x9023934`），所以 `HV_PCHK` 可以重新启用：
在 `goOnBus(bool)`（run `0x1db700b0`）和 `goOnBus()`（run `0x1db7016c`）下探针，
确认前者是否被调用、后者是否被跳过。

## 24. 触发链闭合：`AppleVirtualUSBPort::finishStart` → `goOnBus(true)` → `goOnBus()`

### 24.1 尾调用（决定性的那一条）

`AppleVirtIOUSBDeviceController::goOnBus(bool)`（`0x905c0b0`）末尾：

```asm
0x905c130  mov x0,x19
0x905c134  cbz w20, <0x905c154>      ; enable == 0 → bl goOffBus; return 0
0x905c150  b   0xe39c                ; enable != 0 → 尾调用 goOnBus()（0x905c16c）
```

**即 `goOnBus(true)` 一旦被调用，协商必然执行。** 日志里协商零痕迹 ⇒ `goOnBus(true)` 没被调用。

### 24.2 唯一调用者

在 `AppleVirtualPlatform` 的 `__TEXT_EXEC` 里搜 `add/ldr #0x8d8`：

```
0x9060b48  add x9, x8, #0x8d8    }  AppleVirtualUSBPort::finishStart(IOService*)+0x1c/0x20
0x9060b4c  ldr x8, [x8, #0x8d8]  }
0x9060b58  blraa x8, x17         ; → goOnBus(enable)，x0 = *(全局+0x460) 的 deviceController，
                                 ;   w1 保留入口的 provider 指针 → enable = true
0x9060b5c  cbz w0, <0x9060c38>   ; 返回 0 被当作失败
```

### 24.3 完整因果链

```
AppleVirtualUSBPort::finishStart()
   └─> [deviceController + 0x8d8](true)          = goOnBus(true)
         └─> 尾调用 goOnBus()
               └─> updateStatus(1) → (2) → (4)；enable 2 队列；strb 1,[this+0x1d0]
```

而 `AppleVirtIOUSBDeviceController::start` 只做到 `readyForHost()`（内部 `createUSBDevice`），
它不会自己上线。**所以「内核在这个状态应该做什么」的答案就是：`finishStart` 必须被调用到
`goOnBus(true)` 那一句。**

### 24.4 还没确定的一环

`finishStart` 在 `AppleVirtualPlatform` 内部既没有 `bl` 调用者，`__DATA_CONST` 里也没有
指向它的函数指针 —— 说明它是**基类 `IOPort` 通过虚表调用的**（`finishStart` 是重写）。
下一步：取 `AppleVirtualUSBPort` 的 `__ZTV` 里 `finishStart` 的槽号，再去 `IOPortFamily` /
`AppleVirtualPlatformUSBPort` 的 `start` 链路里找该槽的调用点与前置条件。

## 25. 执行轨迹（HV_TRACE_PC）：时序倒置，这是根因方向

`HV_TRACE_PC` + `HV_SCAN_PROC=1` 跑一轮（2048 MB），trace 1 267 959 块（10 MB）。
按块号排序的关键函数：

| 块号 | 函数 |
|---|---|
| 155934 | `AppleVirtualUSBPort::start` |
| 157671 | `AppleVirtualUSBPort::finishStart`（只执行 1 个块就进基类） |
| 157672 | `IOPort::finishStart`（基类 super 调用） |
| 157677 | `IOPortFamily::finishStart` |
| 189778 | `AppleVirtIOPCITransport::start` |
| 193304 | `AppleVirtIOUSBDeviceController::start` |
| — | `goOnBus(bool)`、`goOnBus()`：**0 块** |
| — | `goOffBus()`：0 块 |

### 25.1 纠正一处误判

`AppleVirtualUSBPort::finishStart` 里的 `[x8+0x8d8]` **不是** `goOnBus`，而是
**`IOPort::finishStart`（基类调用）** —— trace 顺序直接证明（下一块就是 `IOPort::finishStart`）。
之前那节的「finishStart calls goOnBus(true)」作废。

### 25.2 真正的结论：port 比 device controller 更早上线

```
port::start ─> finishStart ─> IOPort::finishStart ...      (block ~155k-158k)
                                    ↓  约 3.4 万块之后
              transport::start ─> usbdc::start              (block 189k / 193k)
```

`goOnBus()` 从未执行，而它唯一的入口是 device controller 侧的上线路径。
**port 在 controller 存在之前就完成了启动** → 上线通知落在空处。

### 25.3 下一步

1. 找 `goOnBus(bool)` 的真正调用者：它是 vtable 槽（`__ZTV30AppleVirtIOUSBDeviceController`
   的 `+0x8d8`，即对象 `+0x8c8`），但 `AppleVirtualPlatform` 里 `add #0x8c8` 的三处都不是它
   （`goOnBus()` 自身、IdentityVirtioMessenger、HIDBridge）→ 调用者在别处
2. 更重要：确认上游是如何保证「controller 先 start、port 后 start」的顺序，
   我们这边是否因为 DT/PCI 创建顺序（vusb 被提前创建）导致倒置

## 26. 排除：vusb 的 PCI 创建顺序不是原因（2026-11-16 深夜）

我们的 `devices/vmapple.c` 把 `hv_virtio_usb_create()` 提到了 block 设备之前
（注释说明是为了让它的 INTx 走 `interrupt-base + slot` 的低 slot），
`origin/main` 则是最后创建。`HV_VUSB_LATE=1` 可切回上游顺序。

实测（2048 MB，`HV_VUSB_LATE=1`）：

```
status = 0 → read 0x14 ×2 → status = 0x1（ACK）→ read 0x14 → isr read = 0
队列寄存器 / DRIVER_OK 写入：0 次
```

与"提前创建"完全一致。**结论：PCI slot / 创建顺序无关**，
trace 里观察到的「port 先于 controller start」在上游顺序下同样成立（我们这次就是上游顺序），
所以那是正常流程，不是回归点。

至此「谁应该调 `goOnBus()`」仍是唯一未闭合项：它是
`__ZTV30AppleVirtIOUSBDeviceController` 的 vtable 槽（对象 `+0x8c8`），
调用者必须持有一个该类型的对象；`AppleVirtualPlatform` 里三处 `add #0x8c8` 都不是它。
下一步据此在其它 kext（IOPortFamily / IOUSBDeviceFamily / IOUSBHostFamily）里按
「对象是 AppleVirtIOUSBDeviceController」这一类型线索定位调用点。

## 27. trace 精确定位：`start` 到 `readyForHost` 就结束，之后全空

用 trace 按块号展开 `AppleVirtIOUSBDeviceController::start`（block 193304 起）：

```
193304  0x905b904  AppleVirtIOUSBDeviceController::start
193307  safeMetaCast
193313  AppleVirtIOInput::start              (邻居设备的 start，说明在遍历)
193324  0x9b904a4  IOUSBDeviceController::start   ← 子类把工作交给基类
        ...（基类内部）...
        readyForHost 的返回点 0x905bae0 出现
```

块区域内**完全为零**的符号（每个都实测 0 块）：

| 符号 | 块数 |
|---|---|
| `startEp0In`（`0x905d3d0`） | 0 |
| `startNonIsocEndpoint`（`0x905d57c`） | 0 |
| `prepareConfiguration`（`0x905cf8c`） | 0 |
| `handleUSBReset`（`0x905d77c`） | 0 |
| `goOnBus()`（`0x905c16c`） | 0 |
| `goOffBus()`（`0x905c624`） | 0 |

而 `goOnBus(bool)` 在全库范围内**没有任何 `bl`/`b` 调用者**，唯一的 vtable 槽调用点是
`IOUSBDeviceController::handleUSBSoftDisconnect` 的 block（传 `false` = 下线）。

### 27.1 因此当前状态是

内核的 vusb 驱动完成了「控制器对象构造 + `readyForHost`（内部 `createUSBDevice`）」，
然后**停在那里等待一个外部事件**，该事件才会驱动：
`startEp0In`（建 EP0 接收队列）→ `prepareConfiguration` → `handleUSBReset` → 协商推进到 DRIVER。

在真实 Virtualization.framework 中，这个事件是 **VMM（host 侧）** 给出的「设备已连接到端口」。
我们的设备模型从未发出它，所以整条链一个函数都没进。

### 27.2 下一步（唯一剩下的方向）

反汇编基类 `IOUSBDeviceController::start`（`0x9b904a4`），看它在 `readyForHost` 之后
究竟等什么（事件/信号量/端口状态回调），以及哪个 `IOPort` 状态会触发它。
那也直接告诉我们：设备模型要模拟什么才能让 guest 走下去。

## 28. 主路径定位：`io()` → `ioGated()` → `startEp0In()`，而 `io()` 从未被调用

### 28.1 `IOUSBDeviceController::start` 里没有状态机入口

手工解码基类 `start`（`0x9b904a4`，size `0xc7c`）的 bl：

```
prepareDefaults                          0x9b905d8
OSSerializer::forTarget                  0x9b905f8
IOCommandGate::commandGate               0x9b90850
IOMapper::copyMapperForDevice            0x9b908cc
IOCommandPool::withWorkLoop              0x9b90ba8
thread_call_allocate_with_options        0x9b90bd4   ← 回调 = 0x9ba1d6c（remote wake/内部）
IOTimerEventSource::timerEventSource     0x9b90d1c
registerPrioritySleepWakeInterest        0x9b910e0
```

那个 thread_call 的回调（`0x9ba1d6c`，位于 `setupDeviceSetSELDataCompletion` 与
`handleSetConfiguration` 之间的无符号函数）只会被 `tryRemoteWakeEx` / `forceControllerReinit`
调用 → **它是 remote wake，不是启动状态机**。

### 28.2 真正的主路径

| 调用者 | 被调 |
|---|---|
| `IOUSBDeviceController::io(...)`（两个重载，`0x9b936cc` / `0x9b93cb0`） | `[this+0xa60]` = vtable 槽 332 = **`AppleVirtIOUSBDeviceController::ioGated(_IOUSBDeviceIORequestTag*)`** |
| `ioGated`（`0x905d2c4`） | `startEp0In`（`0x905d388`）、`startNonIsocEndpoint`（`0x905d3b4`） |
| `handleRequest` | `startNonIsocEndpoint`（`0x905df48`） |

`ioGated` 在整个 kernelcache 里**没有任何 `bl` 调用者**，只有 vtable 槽引用
（`__DATA_CONST 0x7a40808`）→ 它只经由 `IOUSBDeviceController::io()` 的虚调用进入。

### 28.3 结论

guest 的 vusb 驱动在 `readyForHost` 之后**等待上层提交第一个 IO 请求**；
该请求一旦到达就会走 `io()` → `ioGated()` → `startEp0In()`，建立 EP0 接收队列，
之后 host 的 SETUP 才有落点。

实测：`io()` / `ioGated` / `startEp0In` **全部 0 块**，所以队列从未建立 ——
这与「status 停在 ACKNOWLEDGE」是同一件事的两个侧面。

### 28.4 下一步

定位 `IOUSBDeviceController::io()` 的调用方（USB device 侧的上层驱动，通常是
`AppleUSBDevice`/`IOUSBDevice` 系列），确定它在什么条件下提交第一个 EP0 请求 ——
这也是设备模型需要促成的事件。

## 29. 明确的模型缺口：vusb 的 device config 从未实现（config_len = 0）

`devices/virtio_usb.c` 的 `hv_virtio_usb_create()` 只设了 PCI id / class / MSI cap，
**从未设置 `v->config_len`**，因此 `HvVirtio.config[256]` 整体为 0、长度为 0。

对照其它 virtio 设备，全都实现了 device config：

| 设备 | config_len |
|---|---|
| `virtio_blk` | 96 |
| `virtio_input` | 136 |
| `virtio_net` | 12 |
| `virtio_snd` | 12 |
| `virtio_balloon` | 8 |
| **`virtio_usb`** | **未设置（0）** |

而 PCI capability 里 DEVICE 窗口声明长度是 `0x100` ——
guest 认为有 256 字节可读，实际读到 0。

### 29.1 与 trace 结论的关系

trace 里 device config 一次都没被读（没有 `virtio 106b:1a01 config read` 日志），
而 `AppleVirtIOUSBDeviceController` 的 `start` 在 `readyForHost` 之后就停了。
两者一致地指向同一件事：**设备控制器没有从 device config 拿到它需要的设备描述，
因此没有进入 EP0 队列建立（`io()`→`ioGated`→`startEp0In` 全为 0 块）。**

这是**已确认的模型缺口**（不是已证实的唯一根因）：设备配置区是空的，
而真实 Virtualization.framework 的 vusb 会在这里提供 USB 设备描述与端口状态。

### 29.2 下一步

从 guest 侧反汇编推出 device config 的确切布局：在
`AppleVirtIOUSBDeviceController` 里找通过 `IOMemoryMap + offset (>= 0x2000)` 读取的地方，
列出它访问的偏移与宽度，据此填充 `virtio_usb.c` 的 `config[]` 与 `config_len`。

## 30. `createUSBDevice` 提前退出：设备描述从未构建

### 30.1 它做了什么、走到哪里

`IOUSBDeviceController::createUSBDevice(OSDictionary*)`（`0x9b9ad7c`，`0x2948` 字节）本应构建
完整 USB 描述（`USBStringIndex`、`USBConfiguration`、`IOUSBDeviceInterface`、
`USBInterfaceAssociation`、描述符拷贝等，见其 bl 列表）。

trace 实测它只执行 7 个块：

```
202551  0x9b9ad7c  createUSBDevice
202553  0x9b9ae28  createUSBDevice+0xac
202555  0x9b9d428  createUSBDevice+0x26ac   ← 函数尾部（cleanup 区）
202556  0x9b9d440  createUSBDevice+0x26c4
→ 返回 readyForHost
```

即：`+0xac` 处直接跳到尾部，**设备描述没有被构建**。

### 30.2 那条分支的判据是 boot-arg 位

```
0x9b9adc4  bl current_task
0x9b9add8  b.eq <kernel_task>
0x9b9ade4  bl copyClientEntitlement
0x9b9ae10  ldr w21,[controller+0x1c4]
0x9b9ae14  tbnz w21,#0, <0x20290 完整构建>
0x9b9ae24  b  <0x223f4 尾部/清理>
```

`[controller+0x1c4]` 在 `IOUSBDeviceController::start` 里由 `PE_parse_boot_argn` 写入，
参数名解码为 **`IOUSBDeviceController-debug`**（`0x9b90520`）与 **`usb_dev_nmi`**（`0x9b90554`）。

### 30.3 实测：注入 `usb_dev_nmi=1` 无效

```
HV_EXTRA_BOOT_ARGS="serial=3 debug=0xA usb_dev_nmi=1"
→ status = 0 → read ×2 → status = 0x1 → read → isr read = 0
→ 队列寄存器/DRIVER 写入：0
```

与不注入完全一致。说明该位只是**调试开关**（控制是否走带日志的分支），
不是"能否构建设备描述"的判据 —— 两条分支都是正常路径，区别只在日志量。

### 30.4 因此现在的判断

`createUSBDevice` 仍然提前返回（7 块），原因尚未确定。已知它需要的是
`readyForHost` 传入的 **OSDictionary**（设备属性）以及 controller 自身的属性，
而不是 virtio device config（后者是空的，但 guest 从未读取）。

下一步：沿 `createUSBDevice+0xac` 之后的实际控制流（它 `b 0x223f4` 到达尾部前
经过的少量块）确认是哪一项属性缺失导致提前返回；`0x223f4` 附近只有少数块，
可以逐条对照反汇编判断。

## 31. `createUSBDevice` 提前返回的真正判据：`[+0x1c4]` 的 bit30（不是 bit0）

### 31.1 两条门

```asm
0x9b9ae14  tbnz w21, #0,    <0x20290>    ; bit0  = debug 位
0x9b9b240  tbz  w21, #0x1e, <0x2062c>    ; bit30 = 功能位（设备描述能否构建）
```

第 30 节把 bit0 当成功能判据是错的：bit0 只决定走带日志的版本。
真正的门是 **bit30**，所以 `usb_dev_nmi=1`（只影响 bit0）实测无效是必然的。

### 31.2 `[controller+0x1c4]` 的三个写入点

| 地址 | 函数 | 写入内容 |
|---|---|---|
| `0x9b90518` / `0x9b90540` | `IOUSBDeviceController::start` | `|= PE_parse_boot_argn("IOUSBDeviceController-debug"/"usb_dev_nmi")` |
| `0x9b8e754` | `handleUserlandCommand+0x294` | `|= w20`（userland 传入的标志） |
| `0x9b9d398` | `createUSBDevice+0x261c` | 成功路径末尾设置 |

另：`createUSBDevice` 开头有 `copyClientEntitlement` 检查（`0x9b9ade4`，NULL 则失败返回）。

### 31.3 结论与下一步

vusb 的 USB 设备描述无法构建，因为 `[+0x1c4]` 的 bit30 未置；
该位要么由 **userland 经 `IOUSBDeviceInterfaceUserClient` → `handleUserlandCommand`** 提供，
要么由前一次成功的 `createUSBDevice` 设置。

下一步（两选一，都很具体）：
1. 反汇编 `handleUserlandCommand` 的调用链，确认它期望 userland 提供什么
   （对应 guest 里 `IOUSBDeviceInterfaceUserClient` 的 external method），
   再查 restore 环境里是否有该 userland 组件；
2. 反汇编 `0x9b9b240`（bit30 之后）到 `0x9b9d398`（成功标志）之间，确认
   `createUSBDevice` 需要哪些 controller 属性，判断是否只需我们补 device config
   / DT 属性即可满足。

## 32. 根因链闭合到属性：`getProperty("DeviceDescription")` 返回 NULL

### 32.1 `readyForHost` 的第一步

```asm
0x9b92394  adrp x1, .. ; add x1, x1, #0xdeb   → "DeviceDescription"
0x9b923a4  call [this+0x2d8](x1)               ; IORegistryEntry::getProperty(const char*)
0x9b923b8  x20 = 返回值
0x9b923fc  call [this+0xa40](this)
0x9b92418  createUSBDevice(x20)                 ; ← 参数就是该属性
```

`createUSBDevice(dict)` 用这个 dict 构建 USB 设备描述（字符串索引、配置、接口关联、描述符）。
**dict 为 NULL 时它无法构建**，于是 `[controller+0x1c4]` 的 bit30 永不置位，
后续每次进入都直接走尾部返回 —— 与 trace（7 块、`0xe00002c1`）完全一致。

### 32.2 实测：属性没有被任何人提供

- guest 串口日志：`DeviceDescription` 出现 **0 次**
- DT（原厂 IPSW 的 ADT）里**没有** `device-description` / `DeviceDescription` 属性：
  - `/device-tree/port-virtual-1`: `port-type`, `transports-supported`, `port-number`, `device_type="port-virtual"`
  - `/device-tree/product/usb-device`: `vendor-id=0x05ac`, `product-string="macOS Virtual Machine"`,
    `configuration-string`, `ncm-self-name-unit`, `ncm-interrupt-ep-disabled`, `ncm-control-ecid-mac`
- 且全 DT 中**没有任何节点引用 `usb-device` 节点的 phandle（0x46）**

### 32.3 因此完整因果链（自顶向下）

```
AppleVirtualUSBPort::start/finishStart           OK（trace 有块）
AppleVirtIOUSBDeviceController::start            OK
IOUSBDeviceController::start (prepareDefaults)   OK（串口有打印）
IOUSBDeviceController::readyForHost              OK
  getProperty("DeviceDescription") -> NULL       ← 断点
createUSBDevice(NULL) -> 0xe00002c1              ← 立即返回
[controller+0x1c4] bit30 永不置位               
EP0 队列永不建立 (io -> ioGated -> startEp0In 全 0 块)
virtio status 永远停在 ACKNOWLEDGE(0x1)
host 的 GET_DESCRIPTOR 无处投递 -> "Waiting for device to enter restore mode"
```

### 32.4 下一步

确认 `DeviceDescription` 在正常 VMAPPLE 上由谁生成。候选：

1. `AppleVirtualUSBPort`（从 DT 的 `product/usb-device` 与 `port-virtual-1` 拼装）——
   需要看它是否读 DT 子节点；它当前只有 `start`/`finishStart` 两个方法，
   说明这项工作在 `IOPort` 基类或 `AppleVirtualPlatform` 的其它驱动里；
2. 端口服务（`IOPortTransportStateUSB`）在设备 attach 时把描述写进 IORegistry；
3. guest 内的某个服务（userland）通过 `IOUSBDeviceInterfaceUserClient` 提交。

若属 1/2，那我们缺的是让 port 正确识别 vusb 为其设备来源（DT/PCI 关联）；
若属 3，则需要 restore 环境里对应的用户态组件。
