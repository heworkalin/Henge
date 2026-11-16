# 用户态真的跑起来了，但没有正常干完 —— 2026-11-17

本文只记录**实测**，按「证据 → 结论 → 已排除 → 仍缺什么」排列。
凡是推断都标【推断】；凡是旧结论被推翻，都写明出处。

---

## 0. 一句话

把引擎对 guest 的所有介入（DT 长度、kernel rebase、OEM HVC、USB 强制重枚举、
vusb MSI）全部改成默认关闭之后，**内核的用户态第一次真正跑起来了**：
kext 启动、launchd 完成 bootstrap、`restored_external` 执行了 16077 次
syscall 和 8392 次缺页处理。

但它**没有正常干完**：最后停在 dyld 映射库的阶段，host 侧等 5 分钟后超时，
`restore finished` 仍然失败。**缺的到底是什么，目前还没有定论**，但排除项已经收窄到很小的范围。

---

## 1. 今天之前的问题：引擎一直在替 guest 做事

`host/enter.S` 的 SIMD 寄存器破坏是真的 bug（已修，提交 `f6dfb3e`）。
但在查它的过程中，往引擎里加了一堆**没有验证过必要性**的介入，
每一项都改变了 guest 看到的世界：

| 介入 | 当时的目的 | 现在的开关 |
|---|---|---|
| boot_args 的 DT 长度改成 16 MB | 「内核走不过 device tree」 | `HV_DT_LEN_PATCH=1` |
| 引擎自己 rebase 内核 `__DATA` | 「不补就跑不起来」 | `HV_KFIX=1` |
| 应答 vmapple OEM 超调用 | 「内核需要 UID/REVISION」 | `HENGE_OEM=1` |
| userland 起来后 8 秒强制 USB 重枚举 | 「让 host 重新看到设备」 | `HENGE_USB_REENUM=1` |
| 给 vusb 加 MSI capability | 「驱动需要中断」 | `HV_VUSB_MSI=1` |
| 每次追加 `-v` | — | `HV_BOOT_ARGS_VERBOSE=1` |

其中 USB 重枚举那条是**有害**的：它在 8 秒后用一个可能已被 installer
释放的指针调用 `hv_vusb_force_rescan()`，运行日志里紧跟它的就是 `Killed`
（`runO1.log`，全文只有 77 KB，其它运行是 MB 级）。已改成 opt-in。

**这解释了一个长期困扰**：早期运行里 `__PRELINK_TEXT` 0 块、没有一个 kext 启动 ——
那是被这些介入**掩盖**的现象，不是内核本身的行为。

---

## 2. 默认全关之后实际发生了什么

复现命令：

```bash
ZLIB=1 API=28 ./tools/build-install-android.sh
adb -s emulator-5554 push build/andcli/host-install /data/local/tmp/henge/
adb -s emulator-5554 shell 'cd /data/local/tmp/henge && rm -rf workD2 && \
  HV_LOG=1 HV_EXTRA_BOOT_ARGS="serial=3" setsid nohup ./host-install \
  AVPBooter.vmapple2.bin /data/local/tmp/ipsw/24G90.ipsw workD2 2048 2 \
  > /data/local/tmp/henge/runD2.log 2>&1 < /dev/null &'
```

### 2.1 内核和 kext 真的启动了【实测】

serial 里出现的驱动名，全部是它们自己 IOLog 打出来的：

```
IOUSBHostInterface(0x10000020b): matching deferred by IOUSBHostHIDDevice
AppleUSB20XHCIPort@05200000: enumerateDeviceComplete_block_invoke:
    enumerated 0x05ac/0265/0220 (Magic Trackpad 2 / 2) at 1.5 Mbps
IOUSBHostDevice::setConfigurationGated: AppleUSBHostCompositeDevice
    selected configuration 1
HengeVM USB Keyboard@05100000: ... selected configuration 1
```

内核日志 dump 里另有 `VirtIOStorageDevice`、`AppleUSBHub`、`AccessoryPowerReady`。

也就是说 **IOKit 匹配链、USB 协议栈、VirtIO 存储驱动都真实工作**，
`__PRELINK_TEXT 0 块` 的现象不复现。

### 2.2 用户态进程链走通了【实测】

```
launchd            exiting bootstrap mode / exiting ondemand mode
kernelmanagerd[3]  Successfully spawned, service state: running
restored_external[5]  xpcproxy spawned → service state: running → created
objc[5]: Class _TtC16FetchRestoreKeys16CryptoKitWrapper is implemented in
         both .../DiskImages2 ... and /usr/local/bin/restored_external
```

进程结构自校验（`proc_ro->pr_proc` 回指 proc）：

```
restored_externa  proc 0xfffffe2f9dfc8050  task 0xfffffe2f9dfc87e8   OK
kernelmanagerd    proc 0xfffffe1e3e0671d0  task 0xfffffe1e3e067968   OK
```

注意：`p_comm` 是 `char[17]`，`"restored_external"` 有 17 个字符，
实际存的是 `"restored_externa"`。**用全名查找会误报"进程不存在"。**

### 2.3 它执行了多少【实测】

```
hv_svc_n      = 16077   次系统调用
hv_el0_flt_n  =  8392   次 EL0 同步故障
```

最后 20 次 syscall 全部是 `x16=-4/-6`（`mach_vm_allocate/deallocate` 一类），
`x0=0x203`（flags），`x1` 指向 0x10xxxxxxx 或栈，返回值是有效的用户态地址
（例如 `ret=0x16baa7ef8`）——**dyld 正在映射 dylib**。

EL0 故障的分类（ESR 的 EC/ISS）：

```
ec=0x20 (instruction abort from EL0) iss=0x7 (translation fault level 3)
ec=0x24 (data abort from EL0)        iss=0x47 (translation fault level 3, WnR)
```

**全部是正常按需缺页**，没有一条 permission fault（0x0d/0x0e/0x0f）、
对齐故障（0x21）或外部 abort（0x10）。

### 2.4 最终停在哪里【实测】

```
guest 侧：两个 CPU 都在 el1 pc=0xfffffe001ca35e68 (wfi)，daif=0x3c0
          用户态不再执行（hv_el0_flt_n 停在 8392 不再增长）
host 侧： 02:04:58  Waiting for device to enter restore mode...
          02:09:58  Device failed to enter restore mode.
                    Make sure that usbmuxd is running.
```

`restored_external` 的 proc/task 结构还在，但它的线程 `state` 读出来是 0 ——
**不是挂在某个等待队列上，而是没有可运行线程**。
【推断】它要么已经阻塞在一个永不返回的调用上，要么已经终止、结构尚未回收。

---

## 3. 已排除（都是实测，别再重走）

| 假设 | 实测反证 |
|---|---|
| `shared_region: ... vm_shared_region_start_address() failed` 是致命伤 | 该报错按进程计数：launchd 8 次、xpcproxy 23、kernelmanagerd 6、restored_externa 6。**launchd 带着它完成了 bootstrap** |
| `<Critical>: No task-access server configured!` 是致命伤 | 该行在 07:49:19.478，`restored_external created` 在 07:49:22.817 —— 之后系统继续推进 |
| `restored_external` 因异常崩溃 | 8392 次 EL0 故障全部是 translation fault（正常缺页），无 permission/undef |
| 缺网络后端 | 接入 `host/net_min.c`（ARP/DHCP/ICMP）后 `net tx=0 rx=0`，guest 一个帧都没发 |
| vusb 的 MSI capability 阻塞了驱动 | 去掉 MSI 后 vusb 配置序列完全一样：`status=0 → read+0x14 ×2 → status=0x1 → read+0x14 → isr=0` |
| `restored` 和 `restored_external` 是两个进程 | 两者的 `p_comm` 前缀匹配到同一个 proc/thread（`0xfffffe1f3c52c380`）。内核里只有 `restored_external` |

---

## 4. 仍缺什么 —— 尚未定论，但有两条线索

### 线索 A：`restored_external` 的日志不走串口

serial 里它只有 launchd 替它打的 spawn 记录和一条 objc 警告，
**它自己的 NSLog/printf 一条都没有**（macOS 用户态默认进 ASL，不进 console）。
所以"它停在哪"，串口答不了，只能靠引擎侧的记录（`hv_svc_*`、`hv_el0_*`）。

### 线索 B：最后的活动是 dyld 映射库

最后 20 次 syscall 全是 `mach_vm_*`。有两种可能【推断，未验证】：

1. 某个 `mach_vm_*` 返回了它不接受的值 → 走了错误分支 → 访问 `x20` 指向的坏指针；
2. 库映射完成之后，它进入某个等待（XPC 回复、端口消息），而那个消息永远不来。

一次先前运行的记录支持第 1 种形态：用户态最后一个块是

```
pc 0x104e3f5d4   ldr x8, [x20, #0x20]      ← x20 未知
上一条 0x104e3f5d0  cbz w0, +0xe8           ← 前一个 bl 返回非 0，没跳走
```

**但那次的 `0x104e3f5d4` 在下一次运行里没有命中**（dyld 把该 dylib 映射到了别处），
所以断点地址必须取自**当前运行**，不能跨运行复用。

---

## 5. 本轮新增/修好的工具

| 工具 | 说明 |
|---|---|
| `HV_PCHK_CPU=all\|N` | 执行断点原来只对 cpu0 生效，用户态不一定在 cpu0。现在 `all`/`-1` 全核、`N` 指定核、不设仍是 cpu0。已实测两核都命中（`pchk c1` / `pchk c0`） |
| `HV_PCHK_MAX=N` | 命中打印次数（原固定 400） |
| `HV_PCHK` 用户态打印 | 原 dump 把 `x21` 当 proc、`x26` 当 slide 基址，对用户态无意义；现在 `cpu->el == 0` 时改为打印全部 x0..x29 + lr + sp |
| `hv_el0_flt_*` | EL0 同步故障（非 SVC）瞬间的完整寄存器快照，环形 8 条。坏指针的基址寄存器只在这一刻存在 |
| `hv_el0_note()` | 每块无条件记录用户态最后位置、`hv_last_el0_*`、`hv_el0_trace_*` |
| **修的 bug** | `hv_svc_ret[]` 的更新原来在 `log>=2` 分支里，普通日志级别下**所有 syscall 返回值恒为 0**。此前据 `ret=0x0` 得出的结论不可信 |

新增的 gdb 读取脚本：`tools/tsbase.py`（guest VA→PA）、`procs2.py` 一族、
`/tmp/sv2.py`（按时间顺序读 syscall 环形）、`/tmp/ef.py`（读 EL0 故障环）。

**读 syscall 环时务必按时间序**：`slot = i % HV_SVC_CAP`，i 从 `hv_svc_n-20` 到 `hv_svc_n-1`。
按槽位顺序读 0..4095 拿到的是**最早**那段，不是最新。

---

## 6. 下一步

1. **等它停住后读等待状态**：`restored_external` 的线程在等哪个端口/服务（XPC 消息、`psynch_cvwait`、`semwait`）。这能直接回答"缺什么"。
2. 若确认是等 XPC：查那个服务为什么没被 launchd 启动（`com.apple.system.notification_center` 在另一次运行里出现过 `failed lookup ... error = 3: No such process`）。
3. 若要重复"最后一个块"的断点，**必须在同一次运行里**先取 `hv_last_el0_va`，再用 `HV_PCHK_CPU=all` 断它——不能复用上一次的地址。
