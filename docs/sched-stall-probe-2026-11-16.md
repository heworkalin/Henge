# restore 停滞：从 guest 内存里读出 launchd 的 proc/task/thread（2026-11-16）

本文记录上一轮之后的进展：不再靠外部探针猜，而是**用 gdb 直接读 guest 内存里的
`proc` / `task` / `thread` 结构**，并在每一步用结构自身的回指做校验。

前一轮的结论（`docs/restore-userland-stall-2026-11-16.md`）是"launchd 在一次
page-in 里进内核再没回来"。这一轮把它推进到**可以逐字段读出内核眼里的状态**。

**证据等级**：【实测】= 有具体地址/字节；【推断】= 从机制推出，无源码或实测直接
支撑，不得当事实引用。本文所有 XNU 源码引用均指本地 `/tmp/xnu`（tag
`xnu-11417.140.69`）。

---

## 1. 结论摘要

【实测】停滞时（两个 CPU 都在 EL1 idle、`hv_last_el0_va` 数秒不变）：

| 层级 | 状态 |
|---|---|
| `proc` | 存在，`p_pid = 1`，`p_stat = SRUN(2)`（只表示未僵尸，不代表此刻可运行） |
| `task` | 存在，`proc_ro.pr_task → task`、`task.bsd_info_ro → proc_ro` 双向成立 |
| `thread` | launchd **3 个**，`state` 分别是 `TH_WAIT`、`TH_WAIT\|TH_UNINT`、`TH_WAIT\|TH_UNINT` |
| `allproc` | 只有 **2 个进程**：`launchd (pid 1)` 与 `kernel_task (pid 0)` |
| CPU | 两个都在 EL1 idle 循环（`dsb sy; wfi`），不再回 EL0；宿主占用只剩 kicker |

**因此：进程没死、没退出、没崩溃；三个线程全部阻塞在等待上，调度器行为正常。**
真正的问题是"等的事件不会来"。

---

## 2. 定位方法（每一步都自校验）

### 2.1 找 `struct proc`

`struct proc` 的第一个字段是 `union { LIST_ENTRY(proc) p_list; struct smr_node p_smr_node; }`
（`bsd/sys/proc_internal.h:272`），所以**一个 proc 的地址就是它 `p_list` 的地址**。
而 `p_comm`（`command_t`，`char[17]`）紧跟 `p_name`（`proc_name_t`，`char[33]`）
（`osfmk/mach/resource_monitors.h:75-76`），launchd 两个都是 `"launchd"`。

判据：内存里唯一满足 `"launchd\0"` + 17 字节后又是 `"launchd\0"` 的位置。
ramdisk / 二进制里不会有这种相邻双字段。

反推 proc 起始用 `LIST` 不变式：

```
proc.p_list.le_prev = V  ⇒  *(V) == proc 的地址
```

### 2.2 **坑：`p_comm` 地址 `% 8 == 4`**

`p_comm` 落在 `…0x7c`（`% 8 == 4`）。zone 分配的对象是 8 字节对齐的，所以回退量
必须是 `4 (mod 8)`。按 8 的倍数回退会**每 4 字节错位读 qword**，看到的是两个半字段
拼起来的"全零/怪值"——第一版探针因此得出"proc 前面没有指针"的错误结论。

### 2.3 找 task：`struct proc` 里**没有** task 指针

`struct proc`（271–503 行）里没有 `task` 字段；`proc_task()` 是从
`proc->p_proc_ro->pr_task` 取的（`bsd/kern/kern_proc.c:1645`）。而

```c
struct proc_ro {
    struct proc *pr_proc;    /* +0x00 */
    struct task *pr_task;    /* +0x08 */
    ...
};
```

`proc.p_proc_ro` 在 `proc+0x18`（`p_list` 16 + `p_pptr` 8）。所以
`task = *(*(proc+0x18) + 8)`，校验：`*(proc_ro) == proc`。

### 2.4 找 thread：arm64 没有 `t_task`

```c
queue_chain_t   task_threads;
#if __x86_64__ || __arm__     /* arm64 不成立 */
	struct task *t_task;
#endif
struct thread_ro *t_tro;
```

`thread_ro` 是
`{ struct thread *tro_owner; ... struct proc *tro_proc; ... struct task *tro_task; }`。
所以 `t_tro` 指过去的对象，其首字必须等于**持有该 `t_tro` 的 thread 地址**，且其中
能读到本 task 的地址。这一对回指（thread→thread_ro→thread，以及 tro_task→task）
就是判据。

### 2.5 **坑：`/tmp/xnu` 的 `thread.h` 布局与内核实际不一致**

第一版探针把 `thread+0x08` 当成 `t_tro`，并用"它指向的结构首字 == 本 thread"做校验。
**这个校验有假阳性**：`task->threads` 是一条**双向循环链表**，三个线程的
`+0x00`/`+0x08` 正好互相指向，于是"指向的结构首字是自己"对链表邻居同样成立。

修正后的实测布局（多个线程一致，与 `osfmk/kern/thread.h` 的**字段顺序**吻合）：

| 偏移 | 内容 | 依据 |
|---|---|---|
| `+0x00` | `task->threads` 队列的 `next` | 三个 launchd 线程互相串成闭环 |
| `+0x08` | 同队列的 `prev` | 同上 |
| `+0x10` | `wait_event` | 每个线程值不同（`0x2500` / `0x5d00` / `0x5d00`） |
| `+0x18` | `__runq` | 指向全局区（`0xfffffe001f…`），两台线程相同 |
| `+0x20` | `waitq` | 实测全为 `0x0` |
| `+0x1c8` | **`state`** | idle 线程 `0x84`（`TH_RUN\|TH_IDLE`）；launchd 线程 `0x1` / `0x9` |
| `+0x28` | `turnstile` | 堆指针 |
| `+0x30` | `inheritor` | 堆指针 |

`t_tro` 的真实偏移仍未确定 —— 不能用它做校验。**用 `task->threads` 队列本身**
定位线程是可靠的（队列头指向元素，元素互相闭合）。

---

## 3. 一处实测数据（一次运行，地址每次不同）

```
proc   0xfffffe1d35f44740  pa 0x84268740   p_comm +0x5cc  p_pid +0x60  p_stat +0x64
       p_list.next 0xfffffe1d35f40e80   p_list.prev 0xfffffe001f2a2a50（allproc 头）
task   0xfffffe1d35f44ed8  pa 0x84268ed8   bsd_info_ro 在 task+0x3f0
thread 0xfffffe2301e05030  pa 0x99c45030   t_tro 在 +0x08
       wait_event 0x2e00   wait_links {0xfffffe2301fa4838, 0xfffffe2301fa4838}
```

`proc+0x60 = 01 00 00 00`、`proc+0x64 = 02 00 00 00` —— `p_pid = 1`、
`p_stat = 2`。XNU 的 `p_stat`：`SIDL=1, SRUN=2, SSLEEP=3, SSTOP=4, SZOMB=5`
（`bsd/sys/proc.h`），所以 **`p_stat = SRUN`**。布局自证：`p_pptr` 在 `+0x10`、
`p_proc_ro` 在 `+0x18`，与源码一致。

---

## 4. `state` 的偏移：已用 idle 线程**校准成功**

`thread->state` 是 `TH_*` 位的 `int`（`osfmk/kern/thread.h:493`，`TH_WAIT=0x01`、
`TH_SUSP=0x02`、`TH_RUN=0x04`、`TH_UNINT=0x08`、`TH_TERM=0x10`、`TH_IDLE=0x80`、
`TH_WAKING=0x100`）。内核实际布局与 `/tmp/xnu` 不一致，所以只能校准。

**走了两次弯路，第三次才对**：

1. 第一轮按"值落在 `TH_*` 位域内且随线程变化"筛，得到 `+0xf0 = 0x04/0x05` —— **错**。
2. 第二轮加"`TH_WAIT` ⇔ `waitq != 0`"校验，不通过，已足以怀疑。
3. 第三轮换参照物：**`TPIDR_EL1` 在 XNU arm64 上指向当前运行的线程**，停滞时
   它就是两个 CPU 的 **idle 线程**；而 **idle 线程的 `state` 必含 `TH_IDLE(0x80)`**。
   把 3 个 launchd 线程与 2 个 idle 线程的结构各 dump 0x800 字节，逐 4 字节差分：

```
+0x0b8: idle = 0x82/0x82      launchd = 0x1000002 / 0x2 / 0x2    ← 含非法位，排除
+0x1c8: idle = 0x84/0x84      launchd = 0x1 / 0x9 / 0x9          ← state
```

`0x84 = TH_RUN | TH_IDLE` 正是"正在运行的 idle 线程"；`0x01 = TH_WAIT`、
`0x09 = TH_WAIT | TH_UNINT` 正是"在等待的线程"。两个方向同时成立，
所以 **`state` 在 `thread+0x1c8`**。

**之前 `+0xf0 = 0x04/0x05` 的结论作废**（那是另一个字段，与线程状态无关）；
`+0x20 = 0x0` 也不再是"矛盾"，因为 `TH_WAIT` 与 `waitq` 的关系另有编码。

---

## 5. 当前能下的结论与不能下的

**能确认（不依赖偏移假设）**

1. `proc` / `task` 三层结构都在，回指自洽 → **进程活着，没退出**
2. `allproc` 能完整遍历，只有 2 个进程 → 用户态只起来了 launchd，与"没有 fork/execve"一致
3. 两个 CPU 在 EL1 idle，`hv_last_el0_va` 不再变化 → **没有任何代码在跑**
4. **launchd 有 3 个线程，kernel_task 有 1 个**，都通过 `task->threads` 闭环确认（不是偏移推断）
5. **`state`（`thread+0x1c8`）**：两个 idle 线程 `0x84 = TH_RUN|TH_IDLE`；
   launchd 的三个线程 `0x01 = TH_WAIT`、`0x09 = TH_WAIT|TH_UNINT`（两个）
6. **`TPIDR_EL1` = 当前运行线程**，停滞时是两个 idle 线程
7. idle 判断函数（`0xfffffe001ca35f24`）读 `[perCPU+0x1c0]`，实测 **0** → 与"没有可运行线程"一致

**结论：调度器侧没有异常。** idle 循环（`dsb sy; wfi`）正确地认为没活干；
`p_stat = SRUN` 只表示进程尚未变僵尸，不代表此刻有可运行线程。

**真正的问题**：**launchd 的三个线程全部阻塞在等待上**，其中两个是
`TH_WAIT|TH_UNINT`（不可中断等待，典型是等 I/O 完成或等内核锁）。
所以不是"该跑没跑"，而是"**等的事件不会来**"。这与前一轮"卡在一次 page-in"
方向一致，但更精确：线程已不在缺页处理里，而是已挂在某个等待上。

**不能确认**

1. `wait_event`（`0x2500` / `0x5d00`）具体指哪个结构 / 哪一路唤醒
2. 唤醒是"根本没发生"，还是"发生了但执行器没执行到位"
3. `+0xf0` 那个恒为 `0x4/0x5` 的字段是什么（已确认不是 `state`）

---

## 6. 下一步

调度器侧已排除，方向回到**唤醒/完成**：

1. **`wait_event` 是什么**：`0x2500` 出现在不可中断等待的线程上；`0x5d00` 被两个
   线程共享。把它们与内核结构对质（`lck_mtx` / `vm_page` / `buf_t` 的地址范围）。
2. **page-in 之后是否触发了块设备 I/O**：如果缺页要读文件（dyld 的 fallback 路径是
   逐个 dylib 从文件加载），那次缺页可能挂在 virtio-blk 的完成上。查停滞前最后一次
   blk 队列提交与 used-ring 回写是否配对：
   - 有提交、无完成 → **设备模型没发完成中断**（最可能）
   - 完全没有 MMIO → 纯内存路径，阻塞在锁/唤醒
3. `TH_UNINT` 的含义支持上面：不可中断等待通常就是"等 I/O 或等内核锁"，
   所以 `[perCPU+0x1c0] = 0`（无待处理 ast）是自洽的。

---

## 7. 工具

| 文件 | 作用 |
|---|---|
| `tools/build-gdb-bundle.sh`, `tools/gdbwrap.c` | 自包含 gdb bundle，见 `docs/device-gdb-bundle.md` |
| `tools/probe_proc.py` | 扫 `p_comm`+`p_name` 双字段 |
| `tools/probe_xnu.py` | 含 arm64 页表 walk（复刻 `core/mmu.c` 的 `hv_mmu_walk`）与 LIST 反推 |
| `tools/probe_task.py`, `probe_thread*.py`, `probe_link.py` | proc→task→thread 逐跳校验 |
| `tools/probe_state.py`, `probe_procs.py`, `probe_all.py` | 走 `allproc`、列进程、`state` 校准 |

复现：

```bash
adb -s emulator-5554 shell 'cd /data/local/tmp/henge && rm -rf workAn && \
  HV_LOG=2 setsid ./host-install AVPBooter.vmapple2.bin \
  /data/local/tmp/ipsw/24G90.ipsw workAn 2048 2 > runAn.log 2>&1 < /dev/null &'
# 等 ~100 s，确认两个 CPU 都在 EL1 且 hv_last_el0_va 不变，然后
adb push tools/probe_procs.py /data/local/tmp/henge/
adb shell '/data/local/tmp/henge/gdb-bundle/bin/gdb -p $(pidof host-install) -batch \
  -ex "set print thread-events off" -x /data/local/tmp/henge/probe_procs.py'
```

注意：`adb pull` **不跟随符号链接**（Termux 的 `lib/*.so` 全是 symlink），
拉文件前要先在设备上 `readlink -f`。

---

## 9. 死锁拓扑（2026-11-16 晚，实测）

用 `thread->turnstile`（`thread+0x28`）判定"在等哪把锁"：XNU 的 turnstile 是按**锁**
分配的（`ts_proprietor` 是锁地址的 hash key），所以**共享同一个 turnstile = 等同一把锁**。

实测（停滞时，一次运行）：

```
launchd   pid 1  thr 0xfffffe1974044838  state=0x09 (TH_WAIT|TH_UNINT)          ts=0xfffffe489e0bec88
launchd   pid 1  thr 0xfffffe1974071030  state=0x49 (TH_WAIT|TH_UNINT|WAITREP)   ts=0xfffffe489e0bec88
kernel_task pid 0 thr 0xfffffe197404c040 state=0x01 (TH_WAIT)                    ts=0xfffffe489e0bd848
kernel_task pid 0 thr 0xfffffe197405c838 state=0x09 (TH_WAIT|TH_UNINT)           ts=0xfffffe489e0bd848
```

**四个线程、两组、两个不同的 turnstile** —— 即 launchd 的两个线程等锁 A，kernel_task
的两个线程等锁 B，**四个线程全部阻塞，没有一个可运行**。CPU 在 `wfi` 是正确的。

### 9.1 已排除：virtio-blk / 磁盘 I/O

- 停滞时宿主 **48 个线程无一**在做 I/O：没有任何 `hv_disk_read` / `pread` / `pwrite` 栈帧
- `worker_main`（20 个）是 **`host/libusb_shim.c` 的 USB worker**，不是 blk
- blk 的 worker（`devices/virtio_blk.c` 的 `worker`）同样空闲
- 日志里 blk 的三条异常（`avail entry not ready` / `taken again` / `completion not outstanding`）**全为 0**

所以 `TH_UNINT` **不是**在等磁盘完成。

### 9.2 已拿到的：内核栈调用链

`thread+0xe0` 是内核栈顶（16 KiB，VA 不逐页映射，要按**物理地址**线性偏移读；
`walk()` 对栈内地址会失败）。launchd0 的栈（`/tmp/stack0.bin`，`sp-0x4000..sp`）上：

```
sp-0x0f0 = 0xfffffe001c94a0d4
sp-0x0d8 = 0xfffffe001c898618
sp-0x318 = 0xfffffe001c91333c
sp-0x2f8 = 0xfffffe001c9178bc
```

这些是返回地址，按栈深度排就是调用链。**下一步是反汇编它们，确定卡在哪个函数**
（`lck_mtx_lock` / `waitq_wakeup` / `thread_block` …），这决定了归因方向。

### 9.3 仍未确定的

1. 锁 A / 锁 B 的**持有者**（`thread->turnstile` 里只有等待队列，没有 owner 字段；
   这个版本的 turnstile 无 `ts_owner`，只有 `ts_waitq` / `ts_proprietor` / `ts_inheritor`）
2. 因此"是 guest 逻辑等不到、还是我们执行器丢了唤醒"尚未区分
3. 归因到具体框架（中断注入 / WFI / 屏障 / 锁代码翻译）**还缺反汇编证据**

---

## 10. 丢的是哪一步：唤醒只做了一半（2026-11-16 深夜，实测）

遍历全部线程（不只是 `task->threads` 链上的）后，系统里至少有 **7 个** thread 结构。
除了 2 个 idle（`TH_RUN|TH_IDLE`）和 2 个正常在队列上的等待者，另有 **3 个**呈同一形态：

```
thread 0xfffffe19742f2818  state=0x09 (TH_WAIT|TH_UNINT)  turnstile=0xfffffe489e0befd0  wait_links{self, self}
thread 0xfffffe1974073010  state=0x09 (TH_WAIT|TH_UNINT)  turnstile=0xfffffe489e0bee98  wait_links{self, self}
thread 0xfffffe1974072818  state=0x09 (TH_WAIT|TH_UNINT)  turnstile=0xfffffe489e0bef10  wait_links{self, self}
```

对照**正常**在等待队列上的线程（launchd0/1）：`wait_links{launchd1, launchd1}` —— 互相指着，说明在
同一个 `waitq` 的环形队列上。而那三个是**自环**，即**不在任何等待队列上**。

### 10.1 XNU 的唤醒分三步，实测只做了第一步

```
1. 从 waitq 摘链        → 实测：做了（wait_links 自环，不在队列上）
2. state &= ~TH_WAIT    → 实测：没做（state = 0x09，TH_WAIT 仍在）
3. state |=  TH_RUN     → 实测：没做（state 无 TH_RUN）
```

**结果：线程既不在等待队列里（wakeup 再也找不到它），也没被标成可运行（调度器永远不会选它）。**
卡在中间态。若这其中一个持有锁，锁就永不释放，等它的线程全部死锁 —— 与 §9 的四线程全阻塞一致，
也解释了 `[perCPU+0x1c0]=0` 与 CPU `wfi`：内核账本里确实没有 `TH_RUN` 线程。

### 10.2 归因方向（【推断】）

- 不是"逻辑分支走错"：走错会三步全不做；这里是**做了第一步、漏了后两步**，形态上更像
  **中间某条 store 没有生效**。
- E14 已排除原子指令（`ldxr/stxr/LSE` 全 0 差异），但**屏障（`dmb`/`dsb`）与普通 store 路径未测**。
- 项目里已有同类先例：`VCPU_KICK` 索引写错、SIMD 寄存器跨 helper 丢失 —— 都是"一条指令/一次写没生效"。

### 10.3 下一步（能定位到具体指令）

对三个 `state` 地址（`thread+0x1c8`）与 `wait_links`（`thread+0x00`/`+0x08`）挂硬件 watchpoint，
`continue` 后在写发生的那一刻看 guest `pc` —— 那就是唤醒路径上那条写指令。反汇编它即可判断：
JIT 翻译丢失（指令本身）还是屏障/可见性。**必须在停滞发生之前 attach**。
