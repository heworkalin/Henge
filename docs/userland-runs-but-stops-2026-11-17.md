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

---

## 7. 本轮新增的调试接口

### 7.1 串口同步断点 `HENGE_STOP_ON` / `HENGE_STOP_EVERY`

guest 和引擎之间**唯一的共同同步点就是串口**。所以"在某个已知时刻抓状态"最可靠的办法
不是轮询、也不是定时，而是：**console 打出某行时把整个进程停住**。

```bash
HV_LOG=1 \
HENGE_STOP_EVERY=1 \
HENGE_STOP_ON="restored_external) <Notice>: service state: spawning|\
restored_external [5]) <Notice>: service state: running|\
Successfully spawned restored_external|objc[5]" \
HV_EXTRA_BOOT_ARGS="serial=3" setsid nohup ./host-install ...
```

实现（`devices/vmapple.c` 的 `vmapple_serial_out()`）：

- 行结束时按 `|` 拆开 `HENGE_STOP_ON`，任一子串命中则 `raise(SIGSTOP)`；
- `HENGE_STOP_EVERY=1` 让每次命中都停（否则只停第一次），从而一条命令走完一条事件阶梯；
- 日志会打 `HENGE_STOP_ON matched "<命中的模式>", stopping pid N for gdb`；
- gdb 用 `-batch -x <script>` 读完自动 detach；**detach 不会替进程发 SIGCONT**，
  要继续必须显式 `adb shell 'kill -CONT $(pidof host-install)'`（这一步踩过：停在 T 但日志没有下一次命中）。

**为什么不用 `Successfully spawned` 当唯一点**：那只是 launchd 的记账（fork + 交给 xpcproxy），
停在那里无法判断进程是否真的执行过。停在它**之前**的 `service state: running` 更有信息量。

### 7.2 `HV_PCHK_CPU` / `HV_PCHK_MAX`

`HV_PCHK` 原来只对 cpu 0 生效，而用户态不一定跑在 cpu0。现在：

| 值 | 含义 |
|---|---|
| 不设 | cpu 0（保持旧日志可比） |
| `all` / `-1` | 全部 CPU |
| `N` | 指定核 |
| `HV_PCHK_MAX=N` | 命中打印上限（原固定 400） |

用户态命中时改为打印 `x0..x29 + lr + sp`；原来那套 dump 把 `x21` 当 proc、`x26` 当 slide 基址，
对用户态没有意义。已验证两个核都会命中（`pchk c1` / `pchk c0`）。

### 7.3 `hv_el0_flt_*`：EL0 故障的完整寄存器快照

环形 8 条，记录 EL0 同步异常（排除 SVC）那一刻的 `pc/far/esr/sp` 与全部 `x0..x30`。
坏指针的基址寄存器只在那一刻存在——事后再看，进程已退出、页表已销毁。

### 7.4 修掉的 bug：`hv_svc_ret[]` 恒为 0

`hv_svc_ret[]` 的更新原本和 pc 环形一起挂在 `log >= 2` 分支里，所以普通日志级别下
**所有系统调用返回值读出来都是 0**，此前据 `ret=0x0` 得到的判断不可信。
现在用户态部分拆成 `hv_el0_note()`，每块无条件执行。

### 7.5 关键的偏移纠正

| 字段 | 正确偏移 | 之前用的（错） |
|---|---|---|
| `task->threads` | **`task+0x50`** | `task+0x600` |
| `thread->state` | **`thread+0x1c8`** | `thread+0x340` |
| `p_comm` | `proc+0x5cc`，`char[17]` | — |

`+0x600`/`+0x340` 读出来的是别的东西，据此得到的"线程 state 全 0"等结论全部作废。
`p_comm` 只有 17 字节，`"restored_external"`（17 字符）实际存为 `"restored_externa"`，
用全名查找会误报"进程不存在"。

---

## 8. 停在哪里（阶梯断点的实测结果）

同步点阶梯（`HENGE_STOP_EVERY=1`），每停一次用 `dump-threads.py` / 自写 `th2.py` 扫线程表：

| 停点 | `hv_el0_trace_n` | `restored_external` 线程 | 说明 |
|---|---|---|---|
| `exiting bootstrap mode` | 95 382 | 尚未创建 | 只有 `launchd0`（WAIT）+ 两个 idle（RUN），CPU 都在 idle 线程上 |
| `service state: spawning` | — | — | 已停（未扫） |
| `service state: running` | — | **1 个，state=0x4 (RUN)** | 已在运行队列，可被调度 |
| `Successfully spawned restored_external` | 135 649 | **1 个，state=0x4 (RUN)** | 用户态块增长 4 万，确实在跑 |
| 之后（无 objc[5]） | — | **5 个，全部 WAIT** | 见下 |

阶梯之后的终态（`restored_external` 的 task，threads=5）：

```
[0] state=0x1  (WAIT)                    turnstile=0xfffffe46aa09ac88
[1] state=0x49 (WAIT+UNINT+WAITREP)      turnstile=0xfffffe46aa09ac88
[2] state=0x9  (WAIT+UNINT)              turnstile=0xfffffe46aa09ac88
[3] state=0x41 (WAIT+WAITREP)            turnstile=0xfffffe46aa09ac88
[4] state=0x9  (WAIT+UNINT)              turnstile=0xfffffe46aa09ac88
```

同时：

- **串口没有出现 `objc[5] ... DiskImages2`** —— 说明 `DiskImages2.framework` 没有被加载完成；
- `hv_el0_flt_n` 在 45 秒里只增长 2，用户态基本停止；
- 最后的 syscall 序列**全部有返回值**（`-50 / -47 / -19 / -18 / 75 / 478`），
  没有阻塞在某个 syscall 里 —— 所以线程是卡在**内核内部**等一个对象，而不是卡在调用上；
- 两个 `idle` 线程 `state=0x4 (RUN)`，两个 CPU 都在跑它们。

---

## 9. 可能的结论（按可信度排序）

### 9.1 最可信：`restored_external` 的 5 个线程在等同一个 I/O / page

依据：

1. 5 个线程的 `turnstile` 字段**是同一个值** `0xfffffe46aa09ac88`；
2. 其中 3 个是 `UNINT`（不可中断睡眠）—— 这是内核 I/O 与 page 等待的典型形态，
   不是用户态互斥锁（那通常是可中断的 `WAIT`）；
3. 最后一批 syscall 没有一个是阻塞调用，全部返回 —— 所以等待发生在内核内部；
4. `objc[5]`（`DiskImages2` 加载）没有出现，而 `DiskImages2` 正是**磁盘镜像**相关框架。

`turnstile 0xfffffe46aa09ac88` 的内容：

```
+0x08 = 0xfffffe2aea38d030   → 线程 [3]（WAIT+WAITREP）
+0x50 = 0xfffffe2aea0c9828   → 另一个内核对象
```

**推论**：请求已经提交给块设备，但 completion 没有回来，于是线程集体停在不可中断等待上。
这与 `HV_BLK_STATS` 观测到的「只有 1 req/s、`busy` 很低」一致 —— 有少量提交，没有推进。

### 9.2 次可信：`DiskImages2` 的 I/O 通过某个我们没实现的设备/路径

`restored_external` 用 `DiskImages2.framework` 打开磁盘镜像。如果它走的是
`AppleVirtIOStorageDevice` 之外的另一条路径（例如需要 `diskimagesiod` 这个 daemon 配合），
而那个 daemon 没有起来，等待就会落到"内核内部等一个永远不会到来的事件"。

### 9.3 已排除（本轮实测）

| 假设 | 反证 |
|---|---|
| `restored_external` 没有执行 | 执行了：`hv_el0_trace_n` 从 95k 涨到 135k+，线程 `state=0x4 (RUN)` |
| 卡在某个 syscall 上 | 最后 15 个 syscall 全部有返回值 |
| 用户态崩溃 | 8392 次 EL0 故障全是 translation/access-flag fault，无权限/未定义指令 |
| `shared_region ... failed` 是致命伤 | launchd 带着它完成 bootstrap |
| `No task-access server` 是致命伤 | 它之后系统继续 spawn |
| 缺网络后端 | `hv_net_tx/rx = 0 / 0` |
| vusb 的 MSI capability | 去掉后 vusb 配置序列完全一致 |

### 9.4 下一步（一次运行即可判定）

带 `HV_BLK_STATS=5` 跑，在 5 个线程进入 `UNINT` 之后看块设备：

- 若 `req/s` 归零但队列里有未完成请求 → **completion 路径的问题**，在 `devices/virtio_blk.c`；
- 若完全没有请求 → 等待的不是块设备，回到 `9.2`，查 `diskimagesiod` 那条路径。
