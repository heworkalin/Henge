# L6：从"缺失的唤醒"到"restored_external 卡在 set_progress_0"

日期 2026-11-19。承接 `docs/fault-pmap-2026-11-18.md`（L5 四条结论被推翻）。
本文的结论是**直接观测**，不是从 trace 推断。

---

## 1. 现象

进入稳态（约 43 s）后：

```
stats 199s: blocks 0.00M/s, exc 18/s irq 26/s, wfi 105/s (98% idle)
el1 pc fffffe001ca35e68          ← 固定在 idle 循环
```

- 没有 runnable 线程：cpu0/cpu1 都停在自己的 idle 线程上（`g_cpus[i].tpidr_el1` 可对照）；
- 缺页仍在缓慢推进（唯一页 +1101 / 113 s），但那只是"偶尔被唤醒干一点活"；
- 用户态 syscall 也在推进（open/mmap/read），~0.4 次/秒，**没有 `NOT RETURNED`**。

所以不是"某个 syscall 卡住"，而是**所有线程都阻塞，且不会再有事件唤醒它们**。

## 2. 观测方法（不需要 gdb）

proot 下 gdb 不可用：host-install 是 PIE，gdb 把全局变量解析成**未重定位地址**
（`g_machine` 报 `0x281a000`，真实是 `0x5500000000 + 0x281a000`），并且加载 75 MB
符号会卡死。所以全部改为**引擎内读 guest RAM**：

| 开关 | 作用 |
|---|---|
| `HV_THREADS=1` | 扫 RAM 找 `p_comm`（跳过内核镜像区、`p_comm` 偏移必须 ≥ 0x100），验证 `struct proc`，遍历 `task+0x50` 的线程链表，打印每个线程的 `state`/`wait_event`/`waitq`/`turnstile` |
| `HV_TS_FULL=1` | turnstile 全字段 |
| `HV_TH_STACK=1` | 扫线程 16 KiB 内核栈上的代码地址（算法同 `tools/dump-stacks.py`） |

**结构偏移已从 XNU 源码核对**（`osfmk/kern/thread.h`），不是抄来的近似值：

```
struct thread:
  +0x00 runq_links.next
  +0x08 runq_links.prev
  +0x10 wait_event      (event64_t，是事件值，不是指针)
  +0x18 __runq
  +0x20 waitq           (指针，在 waitq 上等待时非 0)
  +0x28 turnstile       (指针，在锁上等待时非 0)
  +0x1c8 state          (已被内核自身代码交叉验证：0x1c915e0c 处
                         `ldr w8,[x20,#0x1c8]; and w8,w8,#0x111; cmp w8,#1`)
```

turnstile 的 `ts_proprietor` 在 **+0x50**，推法（arm64 是 `WQ_OPAQUE_SIZE=40`，见
`osfmk/kern/waitq.h`）：

```
+0x00 ts_waitq              (40)
+0x28 ts_free_turnstiles    (16)   queue_chain_t
+0x38 ts_inheritor_queue    (8)    priority_queue_sched_max = { pq_root }
+0x40 ts_inheritor_links    (8)    priority_queue_entry_sched
+0x48 ts_htable_link        (8)
+0x50 ts_proprietor         (8)    ← thread 或 0
```

数据与布局吻合：`+0x38` 恒为 0（空 inheritor 队）或指向 thread 内嵌 entry，
`+0x50` 总是 thread 地址或 0。

## 3. 结论：unowned turnstile（丢失唤醒）

同一 run 的快照：

| 进程 | 线程 state | turnstile | `ts_proprietor` |
|---|---|---|---|
| `restored_externa` | 0x1 WAIT | `fffffe497e098768` | **0（NONE）** |
| `launchd` | 0x1 WAIT | `fffffe497e099f38` | thread `...daa818`，它自己 state 0x9，在等下一个 |
| `launchd` 链末端 | — | `fffffe497e099f80` | **0（NONE）** |
| `kernelmanagerd` ×2 线程 | 0x1 / 0x9 | 同一个 `...099d70` | **0（NONE）** |

判据：

- **死锁**：A 持锁等 B，B 持锁等 A → 两边 `ts_proprietor` 都**非 0**。
- **本现象**：锁的 `ts_proprietor` 是 **0**，锁没有任何持有者，而线程仍挂在它的 waitq 上。

所以锁已经被释放（`lck_mtx_unlock` 把归属清掉了），**但等待者没有被唤醒**。
这就是"丢失唤醒"：解锁路径判定"没有等待者"（或唤醒调用没发生/没生效），
而等待者确实在队列里。于是一串线程永久 WAIT，CPU 无 runnable 线程。

`kernelmanagerd` 的两个线程等同一个锁、且该锁无主，是最干净的一条证据。

## 4. 已经排除

- 不是缺页不写 PTE（L5 已推翻，eret 时 100% mapped）；
- 不是 `pmap_enter_options` 没被调用（命中 95%）；
- 不是死锁（proprietor 全 0）；
- 不是某个 syscall 没返回（`hv_svc_dump_tail` 无 `NOT RETURNED`）；
- 不是 gdb 读错（本次全部用引擎自己的 RAM 读 + 页表 walk，偏移从源码核对）。

## 4.5 确证：等待队列非空 + proprietor 为 0

`HV_TS_FULL=1` 打 turnstile 头（`waitq` 部分在 `+0x00..0x28`，arm64 布局：
`flags(4) + interlock(4) + queue(16) + links(16) = 40`）。turnstile 的 `queue` 是
**priority queue**，其 `pq_root` 在 **`+0x08`**，指向等待线程的 `wait_prioq_links`
（`struct thread` 的第一个 union 成员，即 `thread+0x00`）。

同一 run 的快照，每个被等待的 turnstile 都是同一形态：

```
ts+0x00 = flags/interlock
ts+0x08 = fffffe2f9f9ff808   (kptr)  ← 等待队列，非空
ts+0x10 = 0
ts+0x18 = links
...
ts+0x50 = 0                          ← ts_proprietor，无主
```

而且 `+0x08` 指向的正是排在队列里的那个线程本身（`thread+0x00` 就是
`wait_prioq_links`），两边可以对上：

| 检查 | 结果 |
|---|---|
| turnstile 的等待队列 `+0x08` | 指向线程（非空） |
| 同一个 turnstile 的 `ts_proprietor +0x50` | 0 |
| 线程自己的 `thread->turnstile` | 正是这个 turnstile |

**等待者确实在队列里，锁却没有任何持有者** —— 丢失唤醒成立，不是"队列为空所以没人
该被唤醒"。

### 完整链条（restored_external）

```
restored 线程 ...fd828
  └─ 等 turnstile ...88cd8        (+0x08 = ...fd828 ✓ 它自己)
       proprietor = thread ...bf2020   (state 0x9 = WAIT|UNINT)
         └─ 它等 turnstile ...88d20  (+0x08 = ...bf2020 ✓)
              proprietor = 0        ← 断点在这里
```

所以精确位置是：**turnstile `...88d20` 被释放（proprietor 清 0）时没有唤醒排在其
队列里的 thread `...bf2020`**；该 thread 同时是 restored_external 所等那个锁的持有者，
于是整条链冻结。

## 4.6 最后一环：turnstile 已与锁解绑，只剩线程自己引用它

`HV_TS_REF=1` 扫全部 guest RAM（按 1 MiB 块，比对 8 字节值）找所有指向该 turnstile 的
指针。结果：**每个 turnstile 都只有一处引用，就是线程结构自己的 `thread+0x28`**。
上下文把每个字段都对上了：

```
ref pa 84fdc860 ctx:
  ref-0x20 = fffffe28e3ebc838   -> thread+0x08  runq_links.prev（指向自己，单元素环）
  ref-0x18 = 1f00               -> thread+0x10  wait_event（就是之前读到的 0x1f00）
  ref-0x10 = fffffe2eaffa31d0   -> thread+0x18  __runq
  ref-0x08 = 0                  -> thread+0x20  waitq（0，即不在普通 waitq 上）
  [ref]    = fffffe46b00b5b78   -> thread+0x28  turnstile
  ref+0x08 = fffffe2317ea65c0   -> thread+0x30  inheritor
```

（`ref` 与 `thread+0x28` 对齐，因此 `ref-0x18` 必须是 `wait_event`：读到的正是 `0x1f00`，
与线程 dump 独立吻合。）

含义：

- turnstile 在 XNU 里是**按锁分配的**，锁对象/哈希表持有对它的引用；
- 现在**只有等待线程自己还指着它**，说明它**已经从锁上解绑**（锁释放流程执行过）；
- 同时 `ts_proprietor = 0`（无主）、线程 `state = WAIT`、`thread->turnstile` 未清、
  waitq 的 `+0x08` 仍挂着该线程。

三件事合起来只有一种解释：

> **解锁路径把锁状态清干净了（owner/proprietor/哈希绑定），唯独没有唤醒排队的线程。**

不是死锁（proprietor 全 0），不是队列为空（`+0x08` 非空），也不是没解锁（绑定已解除）。

## 4.7 排除：PV ticket lock 与这个卡点无关

arm64 XNU 的自旋锁是**半虚拟化 ticket lock**（`osfmk/arm64/lock_ticket_pv.c`），
park/唤醒走 OEM hypercall：

| 调用 | 编号 |
|---|---|
| `VMAPPLE_VCPU_WFK`（park） | `0xc3000005` |
| `VMAPPLE_VCPU_KICK`（唤醒） | `0xc3000006` |

两条都经过 `vmapple_hvc()`，而它的第一行是：

```c
if (!getenv("HENGE_OEM")) return 0;
```

即**默认不处理整个 OEM 范围**。未处理的返回值为 0 → 调用处转而调用 `psci()`：

```c
if (!hv) cpu->x[0] = psci(cpu, cpu->x[0]);
```

`psci()` 对 `0xc3000005/6` 落到 `default: return (uint64_t)-1;`（NOT_SUPPORTED），
所以 guest 看到的 `x0 = -1`（判据是 `hvc_5` 里的 `*(int64_t*)x0 >= 0`），
**正确地认为 PV 锁不受支持**，回退到普通自旋。

日志直接印证（`HV_LOG>=1` 会打 "unknown hypervisor call"）：

```
$ grep -c "unknown hypervisor call" host.log
3
$ ... | sed -E 's/.*call (0x[0-9a-f]+).*//' | sort | uniq -c
      3 0x8300ff01        ← OEM UID 查询，不是 WFK/KICK
```

**没有任何 `0xc3000005` / `0xc3000006` 调用** → guest 从未使用 PV ticket lock。

因此：

- 不是 PV 锁的 park/kick 丢失；
- 也不是"hypervisor 假报成功"（返回的是 `-1`，guest 走回退路径）。

卡点仍然是 **`lck_mtx`（睡眠锁）的 turnstile 唤醒**，与自旋锁无关。

### 附带的方法论注意

`hvg_hc_kick_cpu()` / `hvg_hc_wait_for_kick()` **不检查返回值**
（`__assert_only const bool success` + `assert`，release 构建里被编译掉）。所以假如某个
hypervisor 让这两个 HVC 返回"成功"但实际不 halt，XNU 不会报错也不会回退——它会认为已经
park 过并收到 kick。**Henge 目前不会踩这个坑**（返回 -1），但任何改动 `HENGE_OEM` 默认值
或返回值的工作都必须先看这一点。

## 4.8 试过的下一条路：监视 ts_proprietor 的写入（以及为什么不够）

思路：解锁把 `ts_proprietor` 写 0 的那条指令就是分叉点，抓到它就能看之后有没有进
`turnstile_wakeup`。

两件事必须知道：

1. **`HV_WATCH_PA` 看不见 JIT 的写。** 翻译后的块直接写 guest 内存，不走
   `hv_phys_write` 那条 C 路径（这也是 `HV_PTW_LOG` 需要 `HV_SLOW_MEM=1` 才看得到 JIT
   写的原因）。所以必须先开 `HV_SLOW_MEM=1`。
2. **开到 `HV_SLOW_MEM=1` 之后，170 秒还到不了稳态**，而"布防地址"必须先到稳态、
   从 `HV_THREADS` 拿到 turnstile 地址才能设置。于是出现矛盾：
   - 不 slow：能到稳态，但看不到写；
   - slow：看得到写，但到不了稳态。
   而且稳态本身**没有新的锁操作**（布防后 40 s 内 0 次写入）——丢失唤醒是**稳态之前
   一次性发生、之后不再重演**的事件。

用 `HV_TS_TRACE=1`（在 `hv_phys_write` 里检测 "`pa-0x48` 是内核指针且旧值是线程指针、
新值是 0"）确实抓到了 unlock 写入，`pc` 集中在 `0x1d019588` / `0x1cfccf28` /
`0x1c9636c8` / `0x1c890dbc` 等处。但**这个判据误报很多**：真正的 turnstile 在
`0x84235xxx`，而统计里出现的是 `0x8432exxx`（同一地址被写十几万次），说明那是别的结构。

**结论**：要缩小到真 turnstile，判据必须自洽验证 —— 取 `ts+0x08` 指向的线程，再读它的
`+0x28`，要求指回同一个 turnstile（`thread->turnstile == ts`）。这需要 op 一次
`hv_mmu_walk`，只能对**首次出现**的 `(ts, oldv)` 组合做（缓存），否则百万级 walk 太慢。

## 4.9 扫 lck_mtx：unlock 把状态清干净了

`HV_TS_MTX=1` 扫 `kalloc` 区找 `lck_mtx`。arm64 用 `osfmk/kern/lock_mtx.h` 的定义
（`LCK_MTX_USE_ARCH` 只在 x86_64 为 1）：

```
+0x00  uint32_t lck_mtx_tsid:24, lck_mtx_type:8      (LCK_TYPE_MUTEX = 0x22)
+0x04  uint32_t lck_mtx_grp
+0x08  lck_mtx_state: owner:28 | ilocked:1 | spin_mode:1 | needs_wakeup:1 | profile:1
```

**turnstile 是用 24 位 `tsid` 引用的，不是指针** —— 这解释了 4.6 里"扫不到锁对象指向
turnstile 的引用"：找的是指针，而内核存的是 ID。

结果（18601 个候选）：

| 判据 | 结果 |
|---|---|
| `needs_wakeup` 置位 + `owner == 0` | **0** |
| `tsid != 0` + `owner == 0`（ownerless 但挂着 turnstile） | 14603（**误报**） |

14603 那批是误报：`pa` 以 `0x18` 递增、`tsid` 同步递增（`1e4018/1e4030/1e4048…`），
是数组/指针表的特征，而 `lck_mtx` 本身只有 `0x10` 字节。`needs_wakeup && owner==0`
这个组合罕见，0 条是可信的。

含义：**unlock 把 `owner`、`tsid`、`needs_wakeup` 全清了**。也就是说解锁路径**认为没有
需要唤醒的等待者**，而不是"想唤醒但失败了"。

## 4.10 排除：引擎的 exclusive / CAS 模拟

`core/translate.c` 的 `translate_excl()` 把 `LDXR` 记为（地址、大小、值），`STXR` 则
用**宿主的 `casal`**（硬件 CAS）实现，靠"内存里的值是否还等于 LDXR 读到的值"判断成败；
注释也明说只有 ABA 会漏。所以：

- 不是 LDXR/STXR 的 lost update（那是真正原子 CAS，不是"地址对就算成功"）；
- 多个 CPU 同时改同一个 `lck_mtx_state` 的不同位时，值不同 → CAS 失败 → 重试，语义正确。

## 4.11 现在的处境

已排除：缺页/PTE、`pmap_enter` 未调用、死锁（proprietor 全 0）、syscall 卡住、
PV ticket lock、等待队列为空、锁未解锁（绑定已解除）、CAS lost update。

剩下的分歧点只有一个：

> **解锁路径认为没有等待者**，而等待线程确实在 turnstile 队列里。

要么 guest 内核在入队时没把"有等待者"这件事反映到它能看见的地方（`needs_wakeup`/`tsid`），
要么我们观察的 `thread->turnstile` 与那个锁不是同一个对象。要分清这两者，需要**在同一次
运行里**把一个 turnstile 和它的 `tsid` 对上——但 `tsid` 是 ID 而非地址，得先确认
XNU 的 `tsid → turnstile` 映射方式（`osfmk/kern/turnstile.c` 的 hash 表）。

## 4.12 重要更正：§4.5–§4.11 的"turnstile"读错了结构

读 `thread+0x28` 指向对象的 `WAITQ_FLAGS.type`（bits 0..2），结果是 **1 = WQT_QUEUE**；
而 `thread+0x30` 的对象是 **2 = WQT_TURNSTILE**，且其 `tsid`（flags bits 12..，CTSID_BITS=20）
为 **0**：

```
thread+0x20 = 0                                     (__runq)
thread+0x28 = fffffe43fa09a598  flags 148e809  type 1  → waitq
thread+0x30 = fffffe1e3807e9e0  flags 2        type 2  → turnstile, tsid = 0
```

所以：

| 结论 | 状态 |
|---|---|
| `thread+0x28` 是 turnstile | **错**，那是 waitq |
| `ts_proprietor` 在 `+0x50` 且为 0（§4.5–§4.9） | **作废**。那读的是 waitq，`+0x50` 落在**相邻的 waitq 对象**里（waitq 只有 40 字节），所以"thread 指针或 0"是相邻对象的 `pq_root`，不是锁的 proprietor |
| "线程在锁上等" | **错**，线程在 **waitq** 上等 |
| `thread->turnstile` 的 `tsid = 0` | turnstile **未与任何锁绑定** |

仍然成立的观测（它们不依赖 turnstile 布局）：

- `state = 0x1 (WAIT)`；
- `thread->waitq`（`+0x28`）非 0，其 `pq_root`（`waitq+0x08`）正指向该线程 → **等待队列确实挂着它**；
- `wait_event`（`thread+0x10`）= `0x1f00`（restored / launchd 是 `0x2500`）；
- 扫 18601 个 `lck_mtx` 里 `needs_wakeup && owner == 0` 为 **0**；
- 引擎的 exclusive/CAS 是硬件 `casal`，语义正确。

### 结论改成

> **线程在 waitq 上等待一个 event（`0x1f00`），而这个 event 从未被 post。**
> 不是"锁的唤醒丢了"——锁那一侧是干净的（`tsid = 0`，没有绑定）。

这也解释了为什么 `event` 值很小（`0x1f00`、`0x2500`）却不像指针：`event64_t` 允许任意值，
`assert_wait`/`wakeup` 的 event 不一定是地址。下一步要查的是**这两个 event 分别属于哪个
子系统、谁应该 `wakeup()` 它**。

### 方法论教训

结构偏移不能只靠"从源码数一遍"。`thread+0x28` 与 `+0x30` 只差 8 字节，但一个是 waitq、
一个是 turnstile，而两者都是"内核指针"，**外观完全一样**。判定它们的唯一可靠办法是读
`WAITQ_FLAGS.type`（1 vs 2）——这一步应该在第一次用某个偏移时就做，而不是等到多个结论
都建立在它上面之后。

## 4.13 转折：串口显示它卡在 set_progress_0，最后一条 syscall 是 kevent_id

`HV_EXTRA_BOOT_ARGS="serial=3"` 打开恢复内核日志后，restored_external 打的是
**CHECKPOINT 序列**：

```
CHECKPOINT BEGIN: MAIN:[0x0400] umask          → END ✓
CHECKPOINT BEGIN: MAIN:[0x0402] setvbuf        → END ✓
CHECKPOINT BEGIN: MAIN:[0x0406] set_progress_0 → 没有 END ✗
```

即它启动后只走了三步 libc 初始化，**第一个实质动作"设置恢复进度"就卡住了**。

用 `HENGE_STOP_ON="set_progress_0"` 在那一刻 dump syscall 环，最后几条是：

```
sys 338 stat64  "/var/log/asl"
sys 397 write_nocancel
sys 375 kevent_id
```

`kevent_id` 是 XNU 给 libdispatch（GCD）的事件接口。所以卡点可以一句话说清：

> **它写了一条日志，然后在 kqueue 上等一个事件；那个事件源再没触发。**

这也和前面所有内核层观测一致，而且现在它们都成了"结果"而不是"原因"：
`state = WAIT`、`waitq` 挂着它、`turnstile tsid = 0`（不是在等锁）、event 从不被 post、
CPU 98% idle。

顺带排除或确认的：

- **USB 链路是通的**：`virtio-usb` 13161 个请求 / 13161 个响应，完全配对，没有未完成；
- PCIe 工作（ecam 有读写），`ApplePCIe=0` 是误判（kext 没有 `p_comm`）；
- 块设备不是瓶颈（没有 inflight 差异）。

还有一个可疑项：串口里 `restore-step-uptime = -756316507015`，**guest 看到的 uptime 是负的**。
如果 `set_progress_0` 用它做时间判断/超时，值得单独查。

## 5. 下一步（新的方向）

1. **kqueue 监听的是什么**：从 guest 内存解出该 kqueue 的事件源（fd / dispatch source），
   看是 vnode、device、还是 libdispatch 的内部 pipe。这是"等谁"的直接答案。
2. **谁该触发它**：如果监听的是某个 IOKit user client 或设备 fd，那就要看该设备的完成路径
   是否上报了事件。
3. **时钟**：确认 `-756316507015` 的 uptime 是否会让超时/时间逻辑直接算错。

### 4.13 之前的 §4.5–§4.12 仍然有效的部分

那些章节里"读错结构"的部分已在 §4.12 更正；仍然成立的原始观测（state/WAIT、waitq 非 0、
lck_mtx 干净、CAS 正确、USB 配对完整）在本节结论下全部自洽。


1. **定位具体的锁对象**：从 `lck_mtx_t` 反查（`owner` 字段应为 0，`waiters` 指向该
   turnstile），得到"哪个 mutex 丢了唤醒"。turnstile 与锁对象的关系走
   `thread->turnstile` ← `lck_mtx.lck_mtx_waiters`，可在引擎里扫 RAM 反查。
2. **检查解锁路径依赖的东西**：XNU 的 `lck_mtx_unlock` → `turnstile_wakeup` 依赖
   `waitq` 链表与原子/内存序操作。若 guest 看到"waitq 为空"，那么在引擎的 JIT 下
   这些操作是否语义等价（原子指令、`dmb`/`isb`、`ldaxr/stxr`、`cas`）就是首要嫌疑。
3. 用 `HV_TS_FULL=1` 记录 turnstile 的 waitq 部分（`+0x00..0x28`），确认等待者链表是否
   真的挂在里面——如果链表里有线程而解锁方认为为空，问题就在原子/内存序；
   如果链表本身就空，问题在入队路径。

## 6. 工具状态

`android/install.c` 的 `hv_threads_dump()` / `th_dump_stack()` / `th_dump_turnstile()`，
全部由环境变量控制，默认关闭，只读不写 guest 状态。
`tools/host-install.c` 的 SIGUSR1 触发（`HV_THREADS=1`）。

---

# 7. 第二轮（同日）：kqueue 背后是 mach IPC，而整机已进入累积死锁

本节承接 §4.13。§4.13 的结论是"卡在 `kevent_id` 等事件"，本节把它往下推了一层，
同时纠正了本节过程中犯的两个错误（详见 §7.8、§7.9）。

## 7.1 时钟：实测，不假设

`core/sysreg.c` 的 counter 是 `mach_absolute_time()`，而 `android/compat/compat_android.c`
把它实现成 **`CLOCK_MONOTONIC_RAW` 的软件缩放**（`ns/1e9*HV_TICK_HZ + ns%1e9*24/1000`），
不是硬件 `CNTFRQ`。所以：

- 换设备不会改变这个频率（与 SoC 的 19.42 MHz 那类差异无关）；
- 但**缩放代码本身**可能溢出/精度错，所以仍需实测。

`android/install.c` 在 kicker 启动前加了一段测量（`hv_counter()` 前后取
`CLOCK_MONOTONIC_RAW`，中间 `nanosleep(200ms)`），实测：

```
hengevm: timebase measured 23.9998 MHz (4802009 ticks in 0.2001 s)
```

**结论：在这台设备 + 这份 compat 实现下，24 MHz 是对的**（误差 0.0008%）。
这段测量保留，换设备/换 compat 实现时可直接验证。

> 注意：这**不能**推出"guest 看到的 uptime 一定正确"。串口里
> `restore-step-uptime = -756316507015`，而公开的正常日志是 `1`。这个负值
> **仍未解释**；但在 §7.8 的前提下（死锁时内核给不出真实状态），它本身也不能直接当证据。

## 7.2 `kevent_id` 是调度层的等待，业务等待是 mach

`hv_svc` 环里存在大量 `sys` 号为负的记录，例如 `18446744073709551569` = `0xFFFFFFFFFFFFFFD1`
= **-47**。BSD syscall 表里没有负数，这些是 **mach trap**：

```
/tmp/xnu/osfmk/mach/syscall_sw.h:128
kernel_trap(mach_msg2_trap, -47, 8)
```

**`-47 = mach_msg2_trap`**，即 libdispatch / IOKit 的 IPC。给 `hv_svc_dump_tail()`
加了 mach_msg2 的 header 解码（用每条 syscall 自带的 ttbr0 读 guest 用户内存），得到：

```
pc 10cdc0c30  bits 1200  size 36  remote 0  local 507  voucher 0  id 2989
```

- `bits = 0x1200` → **`MACH_RCV_MSG`（接收模式）**
- `local = 0x507` → 等待的 port 名
- `id = 2989` → 消息 id
- 另有发送方向：`remote 0x7fb`

所以 §4.13 的 `kevent_id` 是 libdispatch 在**调度层**等事件；真正的**业务等待**是
`mach_msg2_trap` 的接收。两者不矛盾，前者是"workqueue 怎么拿任务"，后者是"任务等谁"。

## 7.3 body 解出 `ramrod-nvram-session`

把 body（header 之后 0x80 字节）按字节打印并解 ASCII，出现可读串：

```
ramrod-nvram-session
```

同一 body 里还出现 `0xe00002f0`。

**注意对齐**：最初按 8 字节 qword 解码得到 "rmarod"，是错的；必须按**连续字节**解，
字符串不保证 8 字节对齐。

## 7.4 `kIOReturnNotFound` 与 ramrod 是什么（联网核对）

| 查询 | 结果 |
|---|---|
| `0xe00002f0` | `/tmp/xnu/iokit/IOKit/IOReturn.h:200` → **`kIOReturnNotFound`**（"data was not found"） |
| `kIOReturnNotFound` 在 NVRAM 语境 | `OpenCorePkg/Utilities/LogoutHook/nvramdump.c:41`：`IORegistryEntryCreateCFProperty()` 返回 0 → `return kIOReturnNotFound`，即 **IORegistry 里没有该变量** |
| NVRAM 变量操作在哪 | XNU `iokit/Kernel/IONVRAM.cpp`（`IODTNVRAMVariables::setProperties/setVariables`），是**内核 IOKit 服务**，不是用户态 daemon |
| `ramrod` | Apple 的**固件更新/恢复工具**，位于恢复 ramdisk 的 `/usr/libexec/ramrod/`，由 `restored_external` 驱动 |

真实设备上同类失败会打出明确的错误行（libimobiledevice#1543）：

```
CHECKPOINT_INTERNAL_ERROR(checkpoint_nvram_set): failed to set NVRAM variable
restore-outcome=initial_engine_no_return -536870181
```

`-536870181` = `0xE00002DB` = **`kIOReturnNoSpace`**（NVRAM 空间不足）。

> **但我们的日志里没有这条错误行。** 在 §7.8 的前提下，这一"缺失"不能推出
> "没走到 checkpoint_nvram_set"。

## 7.5 与公开日志对比：格式相同，**参数不同**

`set_progress_0` 之后，正常设备的日志是：

```
BEGIN: MAIN:[0x0406] set_progress_0
  restore-step-ids = {0x11030406:3}
  restore-step-names = {0x11030406:set_progress_0}
  restore-step-uptime = 1
  found display: primary
  Display state is 1
  executing /usr/sbin/nvram ...
  inverting UI color / display-scale / found applelogo at /usr/share/progressui/
  ... 之后继续 is_host_compatible / cleanup_recovery_os_volume
```

我们的同段：

```
{1>5} ... {5>7}
  BEGIN: MAIN:[0x0400] umask            → END ✓
  BEGIN: MAIN:[0x0402] setvbuf          → END ✓
  BEGIN: MAIN:[0x0406] set_progress_0   → 无 END
```

**必须注意的差异**：

| 项 | 我们 | 公开日志 |
|---|---|---|
| 前缀 | `{1>5}` → `{5>7}` | `{3>6}` / `{3>7}` / `{4>7}` / `{4>18}` |
| 类型 | `BEGIN/END/NOTICE/PROGRESS/MONITOR` | 同左，但序列更全 |
| `restore-step-uptime` | **-756316507015** | **1** |

所以**不能拿公开日志去解释我们自己的参数**（`{a>b}` 是上下文，不是固定格式）。
`found display` / `applelogo` 之类的行我们从头到尾**没有出现过**。

> 另外一个容易误判的点：`grep applelogo` 在我们的日志里能命中 2 行，但那是
> **idevicerestore 解包 `applelogo@2x~mac-USBc.im4p`**，不是 guest 的显示日志。

## 7.6 `set_progress_0` 那一刻的线程表

`HENGE_STOP_ON="set_progress_0"` 命中时同时调用 `hv_threads_dump()`（新增），结果：

```
hengevm: threads: 64 processes, 3 threads mapped
proc kernelmanagerd   task ...  th ... state 0x1 wait_event 0
proc launchd          task ...  th ... state 0x1 wait_event 2500
proc restored_externa task ...  th ... state 0x1 wait_event 1f00
```

- 三个进程**各只有一个线程**，全部 `state = WAIT`
- **只映射到 3 个线程** → 扫描范围明显不足（内核 task 的线程没扫到）

这台机器上"等待图"小得不正常，所以**不能**据此断定"只有这三个在等"。要说明整机状态，
必须覆盖所有 task 的全部线程。

## 7.7 纠正：CLI 没有图形通路，不能用它验证显示

曾提出"在设备上用 CLI 跑，因为真 PVG 能验证显示"。**这是错的**，两条 CLI 构建都编 stub：

```
tools/build-install-host.sh:53    ... android/compat/pvg_metal_stub.c   （宿主 CLI）
tools/build-install-android.sh:80 ... android/compat/pvg_metal_stub.c   （设备 CLI）
```

`pvg_metal_stub.c` 的注释原文：

> *"Android has no Metal. The replacement is reims-vgpu ... until Stage 2 wires reims-vgpu in
> there is no replay to call. ... the FIFO is still consumed, and **nothing is drawn**."*

而且 `/data/local/tmp` 下的 CLI 进程**没有窗口系统连接、没有 SurfaceView、没有标准显示接口
权限**，即使有真实现也无处呈现：

| 路径 | PVG | 能显示 |
|---|---|---|
| 宿主 CLI | stub | ✗ |
| 设备 CLI | stub | ✗（且无权限） |
| APK | 真实现（iOS `pvg_metal.m` / Android reims-vgpu） | ✓（SurfaceView） |

**结论：显示/进度 UI 只能用 APK 验证，且需要用户在界面点"安装"。**

## 7.8 最重要的纠正：这不是崩溃，是累积死锁；此时日志与返回值都不可信

用户指出的事实（本节最重要）：

> **我们这边没有崩溃。执行到一半之后，一直出现各种死锁，最后内核无法判断具体状态，
> 给不出真实有效的内容和报错。**

这直接否定了本节此前（以及 §7.4）的一种推理方式：

| 错误推理 | 为什么在死锁下无效 |
|---|---|
| "日志里没有 `CHECKPOINT_INTERNAL_ERROR`，所以没走到那一步" | 打印这行要拿锁、要线程、要写日志——死锁时这条路径本身就坏 |
| "某个 syscall 没有返回，所以卡在它里面" | 可能返回了，但记录/输出机制已卡住 |
| "返回值 `kIOReturnNotFound` 说明变量不存在" | 内核状态不一致时，返回值本身未必可信 |

**日志缺失、报错缺失、返回码异常，都是死锁的"结果"，不是"原因"，也不能用来排除路径。**

仍然唯一可靠的事实，是多次实测得到的形态：

```
所有线程 WAIT、两个 CPU 都进 WFI（98% idle）、无 panic、无异常退出
```

## 7.9 本轮仍然站得住的观测（与 §7.8 不冲突）

这些是**结构性/配对性**证据，不依赖日志能否写出：

- 缺页路径正常：PTE 100% 建立，且在推进（见 `docs/fault-pmap-2026-11-18.md` 的更正）；
- `lck_mtx` 扫描：18601 候选，`needs_wakeup && owner == 0` **0 条**（单时刻采样）；
- `thread+0x28` 是 waitq（type=1）、`thread+0x30` 是 turnstile（type=2，tsid=0）；
- PV ticket lock 未启用（只有 3 条 `0x8300ff01` OEM 调用，无 WFK/KICK 的 `0xc3000005/6`）；
- virtio-usb `13161 requests / 13161 responses` 完全配对；
- 块设备 busy 0%，aux 有 0.5 MB/s +每秒 flush（NVRAM 落盘路径在工作）。

## 7.10 下一步（在 §7.8 前提下）

不再从日志/返回码反推，改为**直接画死锁现场**：

1. 把线程扫描从"3 个线程"扩到**所有 task（含 kernel_task）的全部线程**；
2. 每个线程记录 `wait_event` / `waitq` / `turnstile` / 持有的 `lck_mtx` / 内核栈；
3. 拼**等待图**，判断是"存在环"还是"事件源消失"；
4. **两次采样对比**（detach → 等 → 再采样），区分"卡死不动"与"慢速推进"。

在此之前，`set_progress_0` 的语义（写 NVRAM + 找显示）只能作为**待验证假设**，不能当结论。
