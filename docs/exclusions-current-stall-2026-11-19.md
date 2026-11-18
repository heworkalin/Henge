# 当前卡点的排除清单（2026-11-19）

**卡点**：`restored_external` 停在 `CHECKPOINT BEGIN: MAIN:[0x0406] set_progress_0`（无 END），
随后整机进入**累积死锁**：所有线程 `WAIT`、两个 CPU 进 `WFI`、无 panic、无异常退出。
（`docs/investigation-tree.md` §9、`docs/lost-wakeup-2026-11-19.md` §7）

本文只做一件事：**把"已经排除掉的东西"列清楚**，避免重复排查。
不包含尚未验证的推测。

判定口径：

| 标记 | 含义 |
|---|---|
| ✅ 排除 | 有实测证据，且证据不依赖运行期日志的完整性 |
| ⚠️ 前提已变 | 当时观察正确，底层修好/开关变化后不再复现 |
| 🔶 未定 | 有线索但没有定论，**不能当结论** |

---

## 1. 缺页与页表（L5 线，原以为的"主战场"）

| # | 曾经的假设 | 结论 | 证据 |
|---|---|---|---|
| 1 | PTE 从未建立 | ✅ 排除 | PTE 100% 建立并在推进；旧观测是采样方式问题（`docs/fault-pmap-2026-11-18.md`） |
| 2 | `pmap_enter_options` 从未被调用 | ✅ 排除 | 该函数确实执行过；`HV_PCHK` 零命中是 chaining 假阴性 |
| 3 | `vm_map` 里没有 entry | ✅ 排除 | entry 存在，大小正常，横跨匿名零页与子映射两类 |
| 4 | PTE 写到错误的物理页 | ✅ 排除 | `HV_SLOW_MEM=1` 下 `TLB-fresh mismatch = 0`（40154 条 store） |
| 5 | 引擎读错页表（walk 有 bug） | ✅ 排除 | `hv_walk_dbg` 与手工逐级遍历完全一致 |
| 6 | 故障页是 guard page（故意不映射） | ✅ 排除 | 内核 boot-exclusions E13 |
| 7 | PTE 写了又被谁清掉 | ✅ 排除 | E10 |
| 8 | PTE 写错槽（索引算错） | ✅ 排除 | E11（16 KiB 页索引已验证） |
| 9 | 我们的 TLB 用了陈旧翻译 | ✅ 排除 | E12 |
| 10 | 内核内存分配器静默失败 | ✅ 排除 | E16 |
| 11 | 跨核一致性 / TLB 广播时序竞争 | ✅ 排除 | E9 |
| 12 | 非对齐 / 跨页内存访问被翻译错 | ✅ 排除 | E4 + 本地 memops 差分测试 |
| 13 | 位运算 / 移位 / 位域被翻译错 | ✅ 排除 | E3 + `test-bitops.sh` |
| 14 | 乘除 / 进位 / 条件比较被翻译错 | ✅ 排除 | E5 |
| 15 | 独占指令 / LSE 原子被翻译错 | ✅ 排除 | E14；`translate_excl` 用宿主 `casal` |
| 16 | guest 内存里混进了宿主指针 | ✅ 排除 | E15（并修正了一个哨兵误报） |

> **这一整条线（16 项）已经关闭。** 缺页不是当前卡点的原因。

---

## 2. 调度、锁与并发

| # | 假设 | 结论 | 证据 |
|---|---|---|---|
| 17 | PV ticket lock 导致 | ✅ 排除 | 日志只有 3 条 `0x8300ff01`（OEM UID），**无** `0xc3000005/6`（WFK/KICK）→ guest 从未使用 PV lock |
| 18 | unowned turnstile（丢失唤醒） | ⚠️ 前提已变 | 原结论建立在读错结构上（`thread+0x28` 是 waitq，不是 turnstile）；更正后 `tsid = 0`，线程**没挂在锁上** |
| 19 | `lck_mtx` 残留 `needs_wakeup && owner == 0` | ✅ 排除（单时刻） | 扫 18601 个候选，命中 **0 条**；14603 条 orphan 是数组误报（pa 步长 0x18、tsid 递增） |
| 20 | CAS / exclusive lost update（ABA） | ✅ 排除 | 宿主 `casal` 按值比较判成败，只有 ABA 会漏，而 guest 的用法是单调计数 |
| 21 | 存在锁环（A 等 B 持有、B 等 A） | 🔶 未定 | 单时刻采样没找到；但线程扫描**只映射到 3 个线程**，内核 task 的线程没扫到 |
| 22 | workqueue 线程池耗尽 | 🔶 未定 | 观察到 `workq_kernreturn`/`bsdthread_ctl` 在跑，但没有完整线程计数 |

---

## 3. 设备与 I/O

| # | 假设 | 结论 | 证据 |
|---|---|---|---|
| 23 | USB 链路故障 | ✅ 排除 | `virtio-usb` 13161 requests / 13161 responses 完全配对 |
| 24 | PCIe 不工作 | ✅ 排除 | ecam 有读写；`ApplePCIe=0` 是误判（kext 没有 `p_comm`） |
| 25 | 块设备 inflight / 瓶颈 | ✅ 排除 | `HV_BLK_STATS=1`：root 1 req/s、aux 1 req/s 0.5 MB/s、flush 3.6ms、busy 0% |
| 26 | 磁盘 I/O（ramdisk 不在内存） | ✅ 排除 | ramdisk 已在内存；故障与磁盘无关 |
| 27 | 网络 | ✅ 排除 | 收发 0/0，`dns_proxy` 无请求 |
| 28 | 外部 abort | ✅ 排除 | ESR 为 translation fault、`EA=0` |
| 29 | `bdif` 只报 online 不报 offline | ✅ 已知限制 | 不是 bug：这是 `usb_handoff_to_kernel()` 主动 bounce 的原因 |
| 30 | xHCI 的 4 个 bug | ✅ 无关 | restore 走 virtio-usb/bdif，不走 xhci |
| 31 | 显示/PVG 导致卡住 | ✅ 排除（作为卡点原因） | ① 卡在 NVRAM 会话的 mach 消息（body `ramrod-nvram-session`），还没走到显示；② 安装阶段显示只需**设备模型**，`pvg_lite.c` 的模式/ack/swap 完整，stub 只替换"画"（`pvg_metal_*`）；③ 上游 `BootDisplay.swift` 注释：虚拟 Mac 的固件**不画 logo**，boot framebuffer 只有进度条 |

---

## 4. 时钟

| # | 假设 | 结论 | 证据 |
|---|---|---|---|
| 32 | 时间基频率不对（假设 24 MHz 有误） | ✅ 排除 | 实测 **23.9998 MHz**（`timebase measured`），软件缩放无溢出 |
| 33 | 时间不对（uptime 为负 / 时间是 2087）导致卡住 | ✅ **根因，已修复** | 根因与修复见 §8.6：guest 的 CNTVCT 从**宿主开机**起算（实测 6.16 天），commpage offset=0，于是 uptime 变成宿主开机时长、`restore-step-uptime` 溢出为负（`-756316507015` ns）。**修复后**：`restore-step-uptime = 8`、时间戳与宿主 UTC 一致，恢复流程从 `set_progress_0` 一路推进，**ASR 进度到 90%**（此前稳定卡在 `set_progress_0`，从未越过后面的 checkpoint） |

---

## 5. 用户态启动链

| # | 假设 | 结论 | 证据 |
|---|---|---|---|
| 34 | kext 没 start | ✅ 排除 | 219 kext、`PCI configuration end`、键盘/触控板枚举 |
| 35 | ramdisk 挂载失败 | ✅ 排除 | `md0s1 mount-complete`、`rootfs mount flags 0x480d001` |
| 36 | `shared_region` 映射失败 | ✅ 排除 | 每个失败分支都会 `SHARED_REGION_TRACE_ERROR`，日志 0 条 |
| 37 | launchd bootstrap 失败 | ✅ 排除 | `ignition sequence complete` |
| 38 | `restored_external` 没跑起来 | ✅ 排除 | 19 万+ 块、2.8 万 syscall、写出了 CHECKPOINT 日志 |
| 39 | 进程根本没创建 | ✅ 排除 | `restored_externa` / `launchd` / `kernelmanagerd` 都在，各有线程 |

---

## 6. JIT / 翻译器

| # | 假设 | 结论 | 证据 |
|---|---|---|---|
| 40 | `translate.c` 与上游差异导致 | ✅ 排除 | 换回上游版本，现象完全不变（`restore-progress-2` §3） |
| 41 | superblock / K2 优化导致语义偏差 | ✅ 排除 | `HV_SUPERBLOCKS=0` / `HV_K2=0` 结果相同 |
| 42 | SIMD clobber（不保存 q0-q31） | ⚠️ 前提已变 | 已修复（我们现在保存）；修复后卡点仍在 |
| 43 | PAC strip 宽度（`BFM #16,#6` vs `#14`） | ⚠️ 前提已变 | 已改为 `#14`；改后卡点仍在。上游正式版二进制确认是 `#6` |
| 44 | `candidates[]` 借用 x16/x17 | ⚠️ 前提已变 | 已移除；移除后卡点仍在。上游 release 0.3 仍含 16/17 且可用 |
| 45 | `.hidden` / GOT 取地址 | ✅ 不解释卡点 | 实测上游 `adrp/add`、我们 `adrp x0 + GOT`；两边都**不用 x16**（`docs/jit-core-diff-2026-11-19.md` §2.1.6/§2.1.7） |

---

## 7. 恢复栈与宿主层（已修，不是当前卡点）

| # | 项 | 状态 |
|---|---|---|
| 46 | usbmuxd 不认恢复模式 PID `0x1280..0x1283` | 已修（否则设备不可见） |
| 47 | 设备树缺 `/chosen/memory-map` | 已修（注入空节点） |
| 48 | 枚举前的 vendor reset 破坏内核侧接收缓冲 | 已改（默认不发，`HV_RESET_ENUM=1` 对照） |
| 49 | USB 控制器 ACKNOWLEDGE 时不拉 INTx | 已加（否则协商停在 0x1） |
| 50 | `HV_RESTORE_BOOT_ARGS` / `HENGE_TIMEOUT_SCALE` | 已加（调试/慢速环境） |

---

## 8. 统计

| 类别 | ✅ 排除 | ⚠️ 前提已变 | 🔶 未定 |
|---|---|---|---|
| 缺页与页表 | 16 | 0 | 0 |
| 调度/锁 | 3 | 1 | 2 |
| 设备与 I/O | 8 | 0 | 0 |
| 时钟 | 2 | 0 | 0 |
| 用户态启动链 | 6 | 0 | 0 |
| JIT/翻译器 | 3 | 3 | 0 |
| 恢复栈 | 5（已修） | — | — |
| **合计** | **43**（含第 33 项：根因已修） | **4** | **2** |

---

## 8.6 时间偏差：根因、修复，以及它不解释卡点（2026-11-19）

### 现象

```
guest 串口时间戳   [22:35:37.0470-GMT] / 另一轮 launchd 显示 2087-06-30
restore-step-uptime = -756316507015          ← 负值，且每轮都相同
```

### 根因（gdb 实测，未重跑进程）

| 检查 | 结果 |
|---|---|
| 宿主 `date` / `/proc/uptime` | 2026-11-19，uptime 552 498 秒（6.4 天）✓ |
| `hv_counter()` | 12 778 900 511 786 ticks；÷24e6 = **532 454 秒 = 6.16 天** |
| guest commpage `_COMM_PAGE_TIMEBASE_OFFSET` | **0**（用户态 `mach_absolute_time` = CNTVCT − 0） |
| `g_board.rtc.offset` | 0（guest 从未写 PL031 → RTC 本身是 2026） |
| 设备树 `timebase-frequency` | 24000000（20+ 处，全部正确） |
| 内核镜像里的 `{1e9,24e6}` / `{1e9,1}` | 都不存在（那 3 处 `{1e9,1}` 是 libsystem 的常量对） |

**根因**：`core/sysreg.c` 的 `counter()` 直接返回 `mach_absolute_time()`，而 compat 把它实现为
宿主 `CLOCK_MONOTONIC_RAW` 的 24 MHz 缩放 —— **从宿主开机起算**。真实硬件的 `CNTVCT_EL0`
从芯片上电起算，XNU 在 boot 时记录 `rtclock_base_abstime`，用户态算的是
`CNTVCT − base`。我们这边 base 读到 0，于是 guest 的 uptime = 宿主开机时长（6.16 天），
ramrod 把它按纳秒读就成了负数（−756 秒）。

### 修复

`counter()` 第一次被调用时锁定当时的 `mach_absolute_time()` 作为基准，之后返回差值：

```c
static uint64_t cntv_base;
static int cntv_base_set;
static uint64_t counter(void) {
    uint64_t now = mach_absolute_time();
    if (!__atomic_load_n(&cntv_base_set, __ATOMIC_ACQUIRE)) { /* CAS 锁定 now */ }
    return now - __atomic_load_n(&cntv_base, __ATOMIC_RELAXED);
}
```

### 验证（设备实测）

| 项 | 修复前 | 修复后 |
|---|---|---|
| `restore-step-uptime` | `-756316507015` | **`8`** |
| 串口时间戳 | 22:35 / 2087 | **16:08-GMT（与宿主 UTC 一致）** |
| 到 CHECKPOINT 的时间 | 180 秒仍在缺页 | **25 秒内 18 条 CHECKPOINT** |
| 卡点 | `BEGIN set_progress_0` | **仍是 `BEGIN set_progress_0`，无 END** |

### 结论：时间偏差**就是卡点原因**（2026-11-19 更正）

之前的判断是"修复后卡点不变"，那是**只看了一眼（25 秒）**得出的错误结论：当时
`set_progress_0` 还没过，而它在随后几秒内就继续了。完整观察结果：

| 阶段 | 修复前 | 修复后 |
|---|---|---|
| `restore-step-uptime` | `-756316507015` | **`8`** |
| 串口时间戳 | 22:35 / 2087 | **16:08-GMT（= 宿主 UTC）** |
| `set_progress_0`（0x0406） | **稳定卡住，无 END** | **通过** |
| 后续 checkpoint | 从未到达 | `update_fillmore` → `fdr_verify_sealed_manifest` → `commit_sep_os` → `await_system_image_invert_retry` |
| 系统镜像传输 | — | `Transfer: 11,733,565,440 bytes → /mnt1/apfs_invert_asr_img` |
| ASR 进度 | 0% | **90%**（显示进度 54%） |

**机制（推断，未直接观测到校验器拒绝）**：恢复流程里的个性化/签名材料带有时间窗口
（TSS 票据、证书有效期、`restore-outcome` 的时间戳）。guest 的时间漂到 2087（或
uptime 为负），这些材料在 guest 看来不在有效期内，于是它在第一个实质动作
`set_progress_0` 处停住等待。时间修正后（2026、uptime 为正），同一套材料就落在有效窗口里，
流程继续。

**证据级别**：`时间错误 → 卡住` 是**实测**（修复前后对照，且修复只改了 `counter()` 的基准）；
`因为签名/证书时间窗口不被认可` 是**推断**——还没有直接读到签名校验器的拒绝记录。
两者要分开引用。

### 方法论教训（已写入 §10）

**"改了一个变量后卡点还在"这个判断，必须等足够长时间再下。** 25 秒时 `set_progress_0`
仍在原地，但那只是"还没到继续的那一刻"；差几分钟的观察就得出了相反的结论。

## 8.5 显示在安装阶段的真实语义（补充说明）

用户问过"上游安装时显示是正常处理还是桩就够了"。结论：**桩够用**，但前提是设备模型在。

| 层 | 上游怎么处理 | 我们的现状 |
|---|---|---|
| guest 的显示设备 | PVG 设备提供 display（`pvg_lite.c`：一种模式、共享状态页、`ack`、`swap`） | **有**（`devices/vmapple.c` 无条件创建 `b->pvg`） |
| guest 的"找显示" | IOKit/CoreDisplay 找到 PVG 的 display 服务 | 设备模型在，服务应该在 |
| 真实渲染（Metal replay） | `pvg_metal.m` 把 FIFO 重放到 Metal | **桩**：MMIO/FIFO 仍被消费，只是不画 |
| boot 画面 | 固件**不画 logo**，framebuffer 里只有进度条；logo 是 App 叠的（`ios/Henge/BootDisplay.swift` 注释） | 无画面，但不影响 guest |
| 安装进度 | idevicerestore 的 `RESTORE_STEP_*` 回调 → App 界面 | CLI 只打日志 |

所以：
- **安装阶段不需要真实渲染**，guest 也不需要"看到"自己画的东西；
- 我们卡住的位置在 `set_progress_0` 的 **NVRAM 会话**（mach body 已解出），还没到 display；
- 只有等到"macOS 起来、WindowServer 需要真实帧"时，stub 才会成为问题 —— 那不是当前阶段。

## 9. 现在真正剩下的（只有这些）

### 9.1 需要做（按优先级）

| # | 要确认的事 | 需要什么 | 为什么它能回答"内核在等什么" |
|---|---|---|---|
| A | **完整等待图** | 代码：把线程扫描从"3 个线程"扩到**所有 task（含 `kernel_task`）**；每个线程记 `wait_event` / `waitq` / `turnstile` / 持有锁 / 内核栈 | 只有完整的等待图才能区分"**存在环**"（A 等 B、B 等 A）与"**事件源消失**"（没人会再唤醒） |
| B | `set_progress_0` 内部停在哪一段 | 已解出 mach body 是 `ramrod-nvram-session`；还需知道**服务端是谁**（port `0x507`/`0x7fb` 属于哪个 task） | 决定是"等 NVRAM 服务"还是"等显示/其他"，直接指向卡点 |
| C | restored 阶段 USB 通道是否重建 | 一轮日志：`HV_LOG=2 HV_VIRTIO_CFG_LOG=1 HV_LIBUSB_LOG=1`，看 `status`（DRIVER_OK=0x4）、`queue enable`、`no receive buffer posted`、`guest bus on/off` | 如果 `pci_on` 或队列没起来，就能解释"读了 USB 但没传回" |
| D | uptime 为负（`-756316507015`） | 正常设备是 `1`；需要在"内核可判断状态"时读 `time_since_reset` | 低优先：只影响超时/时间戳，不解释全局停滞 |

### 9.2 需要工具

| 工具 | 用途 | 现状 |
|---|---|---|
| 引擎内全 task 线程扫描 | A | 已有 `hv_threads_dump()`，但只扫到 3 个线程 —— **需要扩** |
| mach port → task 映射 | B | 未实现（需要解 guest 的 IPC space，或从 port name 反查） |
| `HV_VIRTIO_CFG_LOG` / `HV_LIBUSB_LOG` | C | 已实现，只差跑一轮 |
| `HV_THREADS` + `HENGE_STOP_ON` | A/B | 已实现 |

### 9.3 目前**无法**确认的

| 项 | 为什么 |
|---|---|
| 上游在 iOS 上为什么不需要这些修复 | 没有苹果设备；只有 release IPA 可反汇编，能确认"是什么"，不能确认"运行时行为" |
| 上游的 `restored_external` 是否也会等 NVRAM 会话 | 同上，没有运行环境 |

> 其余方向都已经有硬证据排除，不要再重走。
> 新增的排除项请追加到本表，并注明证据与日期。
