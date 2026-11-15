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
