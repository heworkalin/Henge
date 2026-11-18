# L6：丢失唤醒（unowned turnstile）

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

## 5. 下一步

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
