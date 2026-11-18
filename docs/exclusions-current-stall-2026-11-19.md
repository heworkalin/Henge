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
| 33 | `restore-step-uptime` 为负数导致超时算错 | 🔶 未定 | 我们的值是 `-756316507015`，正常设备是 `1`；但在"内核状态不可信"的前提下，这个值本身不能当证据 |

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
| 设备与 I/O | 7 | 0 | 1 |
| 时钟 | 1 | 0 | 1 |
| 用户态启动链 | 6 | 0 | 0 |
| JIT/翻译器 | 3 | 3 | 0 |
| 恢复栈 | 5（已修） | — | — |
| **合计** | **41** | **4** | **4** |

---

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

1. **完整等待图**：线程扫描只覆盖 3 个线程，必须扩到所有 task（含 `kernel_task`），
   才能判断是"存在环"还是"事件源消失"（✅ 未定的 #21/#22 都依赖这一步）；
2. **`set_progress_0` 内部到底停在哪一段**：NVRAM 会话 / 显示 / 其他，需要在能读到
   用户态结构的前提下确认；
3. **USB 在 restored 阶段是否真的建立了通道**：`pci_on` / `REQQ` / `RSPQ` 三个闸门，
   需要带 `HV_VIRTIO_CFG_LOG` 的一轮日志（文档里没有这一轮的记录）；
4. **uptime 负数**：只有一个无解释的观测，且受"日志不可信"限制。

> 其余方向都已经有硬证据排除，不要再重走。
> 新增的排除项请追加到本表，并注明证据与日期。
