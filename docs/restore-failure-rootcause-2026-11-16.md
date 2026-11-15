# restore 失败根因结论（2026-11-16）

设备：emulator-5554（PJE110 / SM8550 / Android 15 / 4KB 页），CLI 跑
`build/andcli/host-install`，IPSW `24G90`，`HV_EXTRA_BOOT_ARGS="serial=3 debug=0xA"`。

一句话：**`restore failed` 不是内核没起来、也不是共享库加载失败，而是 host 侧
`recovery_enter_restore_mode()` 等「设备从 USB 断开」等了 30 秒超时。设备其实已经重启并以
restore mode（`05ac:1281`）重新出现，但唤醒 `cond_wait` 的那个 irecv 事件没送到。**

---

## 1. 「shared_region 空 → 共享库加载失败」这个假设不成立

`shared_region: 0x1 [1(launchd)] check_np(...) vm_shared_region_start_address() failed`
只说明 `sr_first_mapping == -1`，即「shared region 结构已建立，但没有任何文件被映射进去」。

判据来自源码，不是推测：

| 位置 | 打印内容 | 日志中出现 |
|---|---|---|
| `osfmk/vm/vm_shared_region.c:1035` | `sr_first_mapping == -1 → KERN_INVALID_ADDRESS` | ✅ 2 次（launchd） |
| `bsd/vm/vm_unix.c:1141` | `map(): no shared region` | ❌ |
| `bsd/vm/vm_unix.c:1160` | `map(...): root_dir mismatch` | ❌ |
| `bsd/vm/vm_unix.c:1260` | `map(...'%s'): not a file` | ❌ |
| `bsd/vm/vm_unix.c:1291` | `map(...): missing CS blob` | ❌ |
| `bsd/vm/vm_unix.c:1302` | `map(...): missing cdhash` | ❌ |
| `bsd/vm/vm_unix.c:1327` | `map(...): not in trust cache` | ❌ |
| `bsd/vm/vm_unix.c:1345` | `map(...): vnode_getattr failed` | ❌ |
| `bsd/vm/vm_unix.c:1590` | `map(): vm_shared_region_map_file() failed kr=` | ❌ |

`shared_region_map_and_slide_setup()` 的**每一个**失败分支都会走
`SHARED_REGION_TRACE_ERROR`，默认级别就是 ERROR，必然打印在串口上。一条都没有 ⇒
**dyld 从未调用 `shared_region_map_and_slide_np`**。

macOS 15 的 dyld4 不再依赖那个旧 API（它自己 mmap 共享缓存），所以 `check_np` 失败是
预期路径、无害。把它当根因会一直往 dyld/APFS 里挖，而那条路上没有任何异常打印。

反过来，有正面证据说明用户态活得很好：

- `load_init_program: attempting to load /sbin/launchd`
- `libignition: 1: hello: hello from launchd.1` / `ignition sequence complete`
- host 侧 USB 上出现 `05ac:1281`（restore mode）⇒ `restored` 已在跑并提供了 USB descriptor
- 188 MB ramdisk、DeviceTree、21 MB kernelcache 全部传完
- `[Subsystem: nvram] [Event: sync] completed with ret=00000000` ⇒ `setenv boot-args` 已落盘

---

## 2. 真正的失败点（host 日志，硬证据）

```
17:51:49.466  <Info> Sending RestoreKernelCache (21193404 bytes)
17:51:50.058  <Info> HengeVM: sending [setenv boot-args rd=md0 nand-enable-reformat=1 -progress -restore]
17:52:20.060  <Error> Failed to place device in restore mode
17:52:20.060  <Error> Unable to place device into restore mode
step 101 (100%)
restore failed
install: restore finished: -2
```

代码路径（`third_party/idevicerestore/src/recovery.c`）：

1. `recovery_send_kernelcache()`（:610）发 `setenv boot-args ...`（:635）
2. 紧接着 `irecv_send_command_breq(client, "bootx", 1)`（:643）
3. 回到 `recovery_enter_restore_mode()`：`logger("Waiting for device to disconnect...")`（:244）
4. `cond_wait_timeout(&client->device_event_cond, ..., 30000)`（:245）
5. 若 `client->mode == MODE_RECOVERY` 仍成立 → `Failed to place device in restore mode`（:248）

**host 等的是「设备从 USB 上断开」，不是等数据、不是等 kernel。**

---

## 3. USB 层其实发生了——设备成功进入 restore mode

`HV_LIBUSB_LOG=1`（`host/libusb_shim.c` 的 g_log）：

```
libusb shim: device 05ac:1227 arrived (address 1)   ← recovery (iBEC)
libusb shim: device left
libusb shim: device 05ac:1281 arrived (address 2)   ← restore mode（restored 在跑）
libusb shim: device left
libusb shim: device 05ac:1281 arrived (address 3)
```

`0x1281` 就是 restore mode 的 Apple 设备。也就是说 **设备重启、重新枚举都发生了**，
`enumerate failed` 计数为 0，`virtio-usb: request` 有 13161 次（含 `ep 0` 的
GET_DESCRIPTOR 控制传输）。

同一轮 guest 侧：

- `bdif usb: channel online (a new stage connected)` ×3
- `virtio-usb: guest went on the bus` ×3
- **`offline` / `guest went off the bus` = 0**
- **`hengevm: psci 0x84000009`（SYSTEM_RESET）= 0，`halted 0`**

即：guest 从未自己发 PSCI reset；USB 的「断开」是 vusb session 变化
（`hv_vusb_set_online()` 里 `if (on || d->online) d->session++`）造成的，而不是设备真的关机重启。

---

## 4. 为什么 `cond_wait` 没被唤醒

能唤醒它的只有 `idevicerestore.c:305` 起的 `irecv_event_cb()`，两个分支：

- `IRECV_DEVICE_ADD`：需 `event->device_info->ecid == client->ecid`
- `IRECV_DEVICE_REMOVE`：需 `event->device_info->ecid == client->ecid`

而 irecv 事件来自 `libirecovery.c` 的 hotplug 回调：

```c
static int _irecv_usb_hotplug_cb(...)            /* :3071 */
{
    if (!_irecv_is_recovery_device(device)) return 0;     /* :3073  ← 白名单过滤 */
    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        thread_new(&th, _irecv_handle_device_add, device); /* :3079  ← 异步线程 */
        thread_detach(th);
    } else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
        uint32_t location = (bus << 16) | address;         /* :3087 */
        struct irecv_usb_device_info *devinfo = _irecv_find_device_by_location(location);
        if (devinfo) _irecv_handle_device_remove(devinfo);  /* 找不到就什么也不做 */
    }
}
```

白名单 `_irecv_is_recovery_device()`（:2675）只接受
`0x1222 / 0x1227 / 0xf014 / RECOVERY_MODE_1..4 / KIS`，
**不含 `0x1281`**。所以：

- restore mode 设备的 ARRIVED **不会**产生 irecv 事件（这是 libirecovery 的设计，正常）
- 唯一能唤醒的是 **`0x1227` 的 DEVICE_LEFT**，而它靠 `(bus<<16)|address` 反查 devinfo

shim 侧：`libusb_get_bus_number()` 恒为 1，`libusb_get_device_address()` 每次枚举
`g_next_address++`。`host/libusb_shim.c` 与 `origin/main` **零差异**；
`refresh()`（:129）在同一轮里先 `fire(LEFT)`，再 `enumerate(session)` + `fire(ARRIVED)`。

**推断（未最终验证，按不确定处理）**：在这种「同一瞬间 LEFT→ARRIVED」的序列下，
上一轮 ARRIVED 启动的异步 `_irecv_handle_device_add` 线程可能还没来得及 open 设备并登记
devinfo，设备就被下一轮替换了；于是随后的 `DEVICE_LEFT` 按 location 查不到 devinfo，
`_irecv_handle_device_remove()` 不执行 ⇒ `client->mode` 停在 `MODE_RECOVERY`
⇒ `cond_wait_timeout(30000)` 超时 ⇒ 本文件的第 2 节。

`/tmp` 侧已排除的解释：`enumerate failed` 为 0、`libusb shim: enumeration failed` 为 0。

---

## 5. 「缺了什么 / 没移植什么」逐项交代

### 5.1 本轮已修的（不是移植缺失，是设备模型 bug；上游同样存在）

`devices/xhci.c`，4 处，全部验证有效：

| # | bug | 后果 | 修法 |
|---|---|---|---|
| 1 | `port_write()` 的 LWS 分支只在 `PLS==0/15` 产生 `PLC` | 驱动请求 U3(suspend) 等不到链路变化 → `forcePowerGated ... after 1001ms`，端口被反复拆掉 | 任何 LWS 链路状态变化都置 `PORT_PLC` + `port_event()` |
| 2 | 一次错误的「按 ERDP 同步 event index」改动（我引入又撤销） | index 越界 → 命令 completion 写到已消费槽位 | 撤销：producer index 必须独立前进 |
| 3 | status stage 用 `IOC` 门控 Transfer Event | 驱动等不到 TD 完成 → `device descriptor fragment is invalid` / `wMaxPacketSize 0x0000` | 无条件 `transfer_event()`（IOC 只门控中断） |
| 4 | 端口电源/挂起状态机不完整 | `setPowerStateGated ... 0xe00002e9` + `hardwareExceptionThreadCallGated: command failure` recovery 循环 | 随 #1 一并消失 |

修复后对照（同一命令、同一 IPSW）：

```
did not generate event      0     (原：多次)
failed to create pipe zero  0
enumeration failed          0
descriptor fragment invalid 0
wMaxPacketSize 0x0000       0
forcePowerGated 1001ms      0
enumerated 0x05ac           2     (HengeVM Keyboard + Magic Trackpad 2)
```

**但要注意：restore 走的是 virtio-usb（bdif）通道，不走 xhci。**
所以这 4 个修复让输入设备/端口稳定，却**不是** restore 的阻塞点。

### 5.2 Android 版确实缺的一块：guest 请求重启后的重新引导

- 上游 iOS：`ios/Henge/Native/henge-vm.c:47`
  `case HV_HALT_RESET: exit_outcome = HENGE_VM_OUTCOME_RESTART;`
  然后 Swift 层 `SessionView.swift:669` 重新 `boot`。
- Android：`android/install.c` **完全没有** halt-reason 处理；
  `tools/host-install.c` 的主循环（:114）只轮询 `hv_android_install_status()` 的 step，
  guest 一 halt 就没人再引导它。

这一轮它没有被触发（`halted 0`、无 `psci 0x84000009`），所以不是本次失败的直接原因；
但一旦 host 的 disconnect 事件修好、设备真的按协议重启，**这个坑立刻会挡在上面**。

### 5.3 host 侧没有缺东西（与上游零差异）

```
host/install.c       无差异
host/libusb_shim.c   无差异
host/vm.c            无差异
devices/bdif.c       仅 +日志/诊断（功能逻辑与上游一致）
devices/xhci.c       仅本轮的 4 个修复
```

`android/install.c` 是本仓库新增文件，也就是移植层本身。

### 5.4 已排除的错误方向（都有硬证据，别再重走）

| 曾经的假设 | 结论 | 证据 |
|---|---|---|
| kext 根本没 start | 假象 | 串口日志里有 `AppleARMLightEmUp::start`、`AppleUSBXCI...`、`PCI configuration end`；MH_FILESET 下 kext 文本不落在 0x1bb20000 区间，`hv_seg_note` 的「kext 0」是地址范围用错 |
| ramdisk 挂载失败 | 其实成功 | `handle_mount:829: md0s1 ... (unencrypted)`、`mount-complete volume ramdisk`、`rootfs mount flags 0x480d001` |
| `HV_PCHK` 零命中 → 代码没跑 | 假阴性 | 源码比较 `(uint32_t)cpu->pc`（运行时 VA），必须传 `link_va + slide` |
| `__PRELINK_TEXT/__PRELINK_INFO` 解压错 | 逐字节正确 | 3967 个差异 qword 全 = `ref + slide(0x14b14000)`；PRELINK_INFO 0 差异 |

---

## 6. 还没定论的一处（不要当成结论）

608~888 次 data abort：

```
esr 9600004f far fffffe92380xxxxx pc fffffe001c890b5c el1   ×230   (kernel, permission L3, 写)
esr 92000007 far 0x100794000/0x100f58000/... pc 0x100a3036c   ~370   (el0, translation L3, 4KB 对齐)
```

`HV_PTW_LOG=1 HV_PTW_SNAP=3000` 显示 `va 0x100794000` **从未被写过 PTE**。

但 `HV_PTW_LOG` 只追踪「walk 用到的页表页」且上限 512 页，**存在假阴性可能**，
所以不能据此断定「guest 从未映射」。这两个数字目前**既不能证明 restored 崩溃，
也不能排除它**——能确定的只是：USB 上 restored 已经能提供 descriptor，
所以它至少跑到过 virtio-usb 初始化。

---

## 7. 下一步（按优先级）

1. `HV_INSTALL_LOG=1`（`FLAG_DEBUG`）抓 libirecovery debug，看
   `_irecv_handle_device_add` 是否失败、`_irecv_find_device_by_location` 是否命中
   → 判定第 4 节的推断是否成立。
2. 若成立，修 `host/libusb_shim.c` 的 `refresh()`：LEFT 与 ARRIVED 之间插入真实的
   重新枚举延迟（几百 ms，模拟硬件），不要在同一轮里瞬间完成，给异步 add 线程留时间。
   （这是 shim 的实现细节，不改 host 的恢复逻辑。）
3. 补 `android/install.c`：`HV_HALT_RESET` → 重新初始化 machine（保留 `vusb` 指针，
   installer 线程持有它）→ 从 booter 重新引导。否则 host 通过后设备重启会直接停机。
