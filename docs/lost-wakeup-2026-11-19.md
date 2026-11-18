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
