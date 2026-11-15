# restore 用户态停滞：探针体系与定位结论（2026-11-16）

本文记录本轮工作：建成一组"零成本"探针，纠正了两个错误的判据，
并把 restore 失败的位置从"不知道卡在哪"推进到"launchd 在一次 page-in
里进入内核后再也没回来"。

**证据等级**：文中凡标注【实测】的，都有对应的文件或日志行；标注【推断】的
是从机制推出来的理解，**没有**源码或实测直接支撑，不得当作事实引用。

---

## 1. 一句话结论

`launchd`（pid 1）加载到 `Accounts.framework` 之后，在一个**普通的分页缺页**
（data abort from EL0，translation fault L3）里进入内核，
此后内核再没有返回用户态、也没有 panic、也没有任何新 syscall。
两个 CPU 都停在 idle 循环，宿主 CPU 占用从 193%/s 掉到 32%/s。

因此 `restored_external` 永远不会启动，vusb 停在 `ACKNOWLEDGE(0x1)` 只是
这条链的下游表现。

---

## 2. 两个被纠正的判据（本轮最重要的教训）

### 2.1 不能用 `pc < 4 GB` 判定"用户态"

旧的探针写法是：

```c
if (cpu->pc < 0x100000000ull) hv_last_el0_va = cpu->pc;
```

**错在两处，方向相反**：

| 问题 | 后果 |
|---|---|
| 内核早期**以物理地址（identity）执行 kernelcache**，`0x74898000` 也在 4 GB 以下 | 记录到的是 EL1 的 `__TEXT_EXEC` 第一页，被当成"用户态最后一块" |
| macOS arm64 用户程序基址就是 `0x100000000` | 真正的 launchd 代码（`0x100xxxxxx`、`0x104xxxxxx`）被**排除在外** |

**症状**：dump 出来的页 `va 0x74898000 -> pa 0x74898000`（identity 巧合），
寄存器是 `x22 = 0xfffffe0018a04000`（boot_args 的 virtBase）、
`x23 = 0x70a04000`（physBase）。这两个值其实是内核 slide 修正代码里的，
不是用户态。

**验证**：`0x74898000 - 0x73b18000 = 0xd78000` = kernelcache `__TEXT_EXEC` 的
fileoff。页内容确实是 kernelcache。

**修正**：只用 `cpu->el == 0`，不做任何地址判断。

```c
if (cpu->el == 0) { /* 记录 */ }
```

改完后第一次就拿到了真实数据：`last EL0 0x70081cc0`、
`sp = 0x3ffed9ffbe0`（正常用户栈）、`ttbr0 = 0x70000000`。

### 2.2 事后走页表取页内容不可行

一开始 `dump_last_el0()` 用 `hv_mmu_walk()` 事后翻译。实测输出：

```
hengevm: last EL0 block 0x74898370 (ttbr0 0x74228000)
hengevm: page 0x74894000 not mapped
hengevm: page 0x74898000 not mapped
hengevm: page 0x7489c000 not mapped
```

原因：**拥有该地址空间的进程（launchd）已经退出/阻塞，页表项在事后不可靠**。

**修正**：在 EL0 执行**当时**就把"页 VA → 页 PA"记下来（每页只 walk 一次，
不是每块），dump 时直接读物理内存：

```c
uint64_t pg = cpu->pc & ~0x3fffull;
if (pg != hv_last_el0_page_va) {
    hv_last_el0_page_va = pg;
    hv_mmu_walk(cpu, pg, HV_ACC_EXEC, 0, &w);
    hv_last_el0_page_pa = w.fsc ? 0 : w.pa;
}
```

实测输出 `page 0x10098c000 -> pa 0xd872c000`（真实映射，非 identity），
页内容反汇编得到合法 ARM64 代码。

---

## 3. 建成的探针（全部零成本或可忽略，`HV_LOG=2` 下生效）

| 探针 | 产物 | 说明 |
|---|---|---|
| EL0 块轨迹（16384 环形） | `/tmp/el0_trace.txt` | 每条 `序号 va x16`，最后 16K 个用户态块 |
| syscall 全记录 | `/tmp/svc_trace.txt` | `pc / svc#imm / x16(号) / 参数 / **返回值**`，另附 **open/execve 的路径字符串** |
| userland 独立异常环（64） | `/tmp/uexc_tail.txt` | 只记 `cpu->el == 0` 的异常 |
| 内核最后 512 块 | `/tmp/kernel_tail.txt` | 内核停在哪些 pc |
| 全部异常（128 环） | `/tmp/exc_tail.txt` | 会被 idle 中断冲掉，见下 |
| 进程名扫描 | stderr | 注意：**不能证明进程活着**（内存里也有 ramdisk 上的 plist/二进制字符串） |
| 宿主 CPU ticks（每 2s） | stderr `hengevm: cpu-ticks +N` | **唯一能证伪"看起来 idle"的手段** |

### 3.1 为什么必须有 userland 独立异常环

`hv_exc_ring` 只有 128 项，而 idle 循环每秒产生上万次定时器中断。
实测最后 128 项是：

```
7  el1 t1（IRQ from EL1）
121 el1 t2（FIQ from EL1）
```

**全是 idle 中断**，用户态故障早被冲掉了。因此另开一个只记 `el == 0` 的环，
立刻抓到 **4344 个 userland 异常**。

### 3.2 为什么必须有 CPU ticks 探针

一度怀疑"launchd 在 chained superblock 里死循环，宿主在跑但所有 C 探针都冻结"。
链式块确实完全不经过 C 代码，`cpu->pc`、块计数、EL0 轨迹都会假装静止。

用 `/proc/self/stat` 的 utime+stime 增量证伪：

```
+387  (pc 0x102b306b0)   ← launchd 在跑
+389  (pc 0xfffffe001c890b10)
+250  (pc 0xfffffe001ca35e68)
...
+65   (pc 0xfffffe001ca35e68)   ← 降到 32%/s，只剩 kicker 开销
```

**结论：不是链式死循环，是真的停了。**

---

## 4. 定位过程（按时间顺序）

### 4.1 launchd 确实起来了

内核日志【实测】：

```
libignition: 1:  hello: hello from launchd.1
libignition: 1:  goodbye: ignition sequence complete
shared_region: [1(launchd)] check_np(...) vm_shared_region_start_address() failed
ASP: AMFI pointer initialized
Failed to open swap file 2
```

- `hello from launchd.1` 证明 **launchd 真的执行了**
- `shared_region ... check_np failed`：**新进程首次调用必然是失败**，
  之后 dyld 才用 `shared_region_map_and_slide_np` 建立。**这条是红鲱鱼**
  （此前 commit `4e658d8` 的判断仍然成立）
- `Failed to open swap file 2`：无 swap，正常

另有一个可选的缺失文件（不致命）：

```
ReadFileWrapper::open failed (errno 2) /System/Library/Security/HardeningExceptions.plist
ReadFileWrapper::fileSize error, no file
```

### 4.2 没有 fork / execve

对全部 1400~2900 条 syscall 统计【实测】：

```
x16=0xc5 (197 mmap)     761
x16=0x5c (92  fcntl)    512
x16=0x5  (5   open)     330
x16=0x152(338)          229
x16=0x6  (6   close)    216
x16=0x49 (73  munmap)   203
x16=0x99 (153)          202
...
```

**`fork`(2) 与 `execve`(59) 一次都没有** → launchd **从未进入"启动服务"阶段**，
一直停在 dyld 加载自身依赖库的过程中。

### 4.3 launchd 最后打开的文件

从 `hv_svc_path` 探针【实测】：

```
/System/Library/Frameworks/SystemConfiguration.framework/Versions/A/SystemConfiguration
/usr/lib/libapple_nghttp2.dylib
/usr/lib/libcompression.dylib
/usr/lib/libsqlite3.dylib
/usr/lib/libz.1.dylib
/System/Library/Frameworks/Accounts.framework/Versions/A/Accounts      ← 最后一个
```

【推断】因为 shared region 没建立，dyld 走"逐个 dylib 从文件加载"的
fallback 路径，所以看到大量 open/mmap。

### 4.4 所有 syscall 都返回了

加了 per-CPU 的 `pending` 标记（全局标记会被另一个 CPU 清掉）：

```
hengevm: syscalls total 1700, pending cpu0=0 cpu1=0
```

最后几条【实测】：

```
1697 open(0x16fc254c8, 0x100000)     ret=0x2
1698 338(0x152)                      ret=0x2
1699 mach trap -10 (port_insert_right) ret=0
```

**没有卡在 syscall 上。**

### 4.5 真正的失败点：一次 page-in

`/tmp/uexc_tail.txt` 末尾【实测】：

```
4338 esr=0x92000007 pc=0x10287b0b8 elr=0x10287b0e0 far=0x102905aed
4339 esr=0x82000007 pc=0x10287b0e0 elr=0x10287b0e0 far=0x10287b0e0
4340 esr=0x82000007 pc=0x102897e78 elr=0x102897e78 far=0x102897e78
4341 esr=0x92000007 pc=0x10287a780 elr=0x10287a780 far=0x106ac930c
4342 esr=0x82000007 pc=0x10287a780 elr=0x10287a780 far=0x10287a780
4343 esr=0x92000007 pc=0x10287a780 elr=0x10287a784 far=0x1029058cf
```

ESR 解码：

| ESR | EC | 含义 |
|---|---|---|
| `0x92000007` | `0x24` | Data Abort from lower EL（EL0），DFSC=`7` translation fault, level 3 |
| `0x82000007` | `0x20` | Instruction Abort from lower EL，DFSC=`7` |

这些是**正常的按需分页缺页**（dyld 映射文件页后首次访问）。共 4344 次，
绝大多数都被内核成功处理（launchd 继续跑）。**最后一条之后再无 EL0。**

### 4.6 内核侧最后的动作

`/tmp/kernel_tail.txt` 末尾 16 块【实测】：

```
0xfffffe001c893100   ← 异常向量入口
0xfffffe001ca4325c   ← idle
0xfffffe001ca35e68   ← idle
0xfffffe001c915f84   ← idle 路径内部
```

反汇编 `0xfffffe001c915f84`（link VA `0xfffffe0007e01f84`）确认是 idle 路径：

```
ldrh w8, [x19, #0x3f8]     ; 检查待处理事件
sub  w8, w8, #1
mrs  x8, DAIF
tst  x8, #0x1c0
ldr  x8, [x21, #0x198]     ; runqueue
cmp  x8, #0
b    ...
```

**runqueue 为空** → launchd 线程不是 runnable，而是 waiting。

---

## 5. 当前判断

【实测】能确定的：

1. launchd 执行过（`hello from launchd.1`）
2. launchd 从未 fork/execve
3. 所有 syscall 都返回了
4. 没有 EL0 同步异常导致进程退出（否则 pid 1 退出会 panic，但没有 panic）
5. 最后一次 EL0 活动是一次 L3 translation fault，之后内核不再返回用户态
6. 内核 runqueue 为空、两个 CPU 在 idle 循环
7. 宿主 CPU 占用确认不是链式死循环

【推断】（**无源码/实测支撑，按不确定处理**）：

- 阻塞点在缺页处理（page-in）内部，线程在等 I/O 或等某个锁/wakeup
- 由于 `dump_last_el0` 里 `scan_proc_names` 只能证明内存里有名字字符串，
  **它不能证明 launchd 进程存在或不存在**，这一条至今没有可靠证据

---

## 6. 下一步（按优先级）

### A. 确认等待点（信息量最大）

设备版 gdb 已打通（见 §7），读 guest 内核 `allproc` 链表 / thread 结构，
直接看 launchd 线程的 `wait_event` / `state`。

需要：内核结构偏移（`proc`、`thread`、`task`）。kernelcache 里可查符号/结构，
但**遇到"推导出来的偏移"必须标不确定，并用实测校验**。

### B. 怀疑 lost wakeup（成本最低，直指我们自己的代码）

历史上有过同类 bug：`VCPU_KICK` 的 `&cpu->m->cpus[id]` 写成了指针取址。
本轮怀疑方向：page-in 完成后的 `wakeup()` 没有真正唤醒线程，
或注入的定时器中断在 WFI 期间被丢弃。

可加探针：记录 WFI 进入/退出与"是否有 pending 事件未投递"。

### C. 检查 page-in 是否触发了块设备 MMIO

若缺页全程无设备 MMIO，说明纯内存路径（md0），阻塞更可能在锁/唤醒；
若有 MMIO 且无完成中断，则是指向 I/O 的直接证据。

---

## 7. 附：设备端 gdb 环境（本轮打通）

### 7.1 Termux 的 gdb 需要三个 GUILE 环境变量

直接跑会 `Fatal signal: Aborted`，原因是 guile 找不到 Scheme 启动文件：

```
scm_load_startup_files → scm_misc_error → abort
```

必须同时设置（缺一不可）：

```sh
P=/data/local/tmp/termux/files/usr
export LD_LIBRARY_PATH=$P/lib
export GUILE_LOAD_PATH=$P/share/guile/site:$P/share/guile/site/3.0
export GUILE_LOAD_COMPILED_PATH=$P/lib/guile/3.0/ccache:$P/share/guile/site/3.0
exec $P/bin/gdb "$@"
```

包装脚本已放在设备 `/data/local/tmp/henge/gdb.sh`（源码在 `/tmp/gdbwrap.sh`）。

### 7.2 ptrace 可用

```
$ gdb -p <sleep pid> -batch -ex "info registers pc" -ex detach
[Thread debugging using libthread_db enabled]
warning: section .note.android.pad_segment not found ...   ← bionic 无害警告
```

### 7.3 能读 guest 状态

`build/andcli/host-install` 本身带 `-g`（`tools/build-install-android.sh`
的 CFLAGS 已含）。实测：

```
$ gdb -p $(pidof host-install) -batch -ex "p/x g_cpus[0].pc" -ex "p/x g_cpus[0].el" ...
$1 = 0xfffffe001c893000
$2 = 0x1
```

`HvCpu` 的字段名是 **`ttbr0_el1`**（不是 `ttbr0`），用户态栈指针是 **`sp_el0`**
（`sp` 是 SP_EL1）。

---

## 8. 本轮改动的代码

| 文件 | 改动 |
|---|---|
| `core/dispatch.c` | `hv_last_el0_*` 记录（va / ttbr0 / 页 PA / 关键寄存器）、EL0 轨迹环、syscall 返回值回填、`HV_WATCH` 断点、`hv_pc_hist_export()` |
| `core/exception.c` | syscall 环（含 open/execve 路径）、userland 独立异常环；`[x0+0x1c0]` 调试输出改为限流（只在 `log>=3` 或 `HV_EL0_DUMP` 且最多 8 次） |
| `android/install.c` | `dump_last_el0()` 改用记录下来的页 PA；各种环 dump 到 `/tmp`；停滞触发时附带进程名扫描；CPU ticks 采样 |

### 8.1 `HV_WATCH`（等价断点，不需要 gdb）

```bash
HV_WATCH=1db396ac ./host-install ...                  # 命中即打印，继续跑
HV_WATCH=1db396ac HV_WATCH_ATTACH=1 ./host-install ... # 命中即 SIGSTOP，kill -CONT 恢复
```

- 十六进制，**按低 32 位比较**（`0xfffffe001db396ac` 写 `1db396ac`）
- 打印 `x0..x30`、`sp/lr/pc/el/ttbr0`、`[sp+0..+88]`
- 只对 `cpu->index == 0`、只命中一次
- 实测：`HV_WATCH=1c893000` 在 +40s 命中，寄存器正确

### 8.2 "慢"的真实原因（澄清）

不是引擎慢。实测里程碑【实测】：

| 里程碑 | 时间 |
|---|---|
| `[ PCI configuration begin ]` | 24~52 s |
| `ignition sequence complete` | 33~60 s |
| launchd 最后一块 EL0 | 62~104 s |

之前"特别慢"的感觉来自 `core/exception.c` 里一段早期调试代码：
**每次 EL0 缺页都打一行 `hengevm: [x0+0x1c0] unreadable (fsc N)`**，
用户态启动每映射一页就缺一次页 → 几万行 `fprintf`。已改为限流。

另外 `HV_EXTRA_BOOT_ARGS="serial=3 debug=0xA"` 打开内核日志**并不是**慢的主因：
实测带它 15 s 就到 dyld、不带约 30 s（差异被启动路径噪声盖过）。

---

## 9. 复现命令

```bash
# 宿主：跑到 launchd 停滞并 dump（约 100 秒）
pkill -9 -x host-install; sleep 2
CURL_CA_BUNDLE=$PWD/work/ca-bundle.pem \
  HV_LOG=2 HENGE_HEARTBEAT=1 \
  HV_EXTRA_BOOT_ARGS="serial=3 debug=0xA" \
  ./build/host/host-install work/AVPBooter.vmapple2.bin android/ipsw/24G90.ipsw workX 2048 2 \
  > /tmp/hostX.log 2>&1 &

# 产物
#   /tmp/el0_trace.txt    用户态块轨迹
#   /tmp/svc_trace.txt    syscall + 返回值 + 路径
#   /tmp/uexc_tail.txt    用户态异常（关键）
#   /tmp/kernel_tail.txt  内核最后 512 块
#   /tmp/el0_page_<va>.bin 最后执行的那一页
```

判据：`grep "userland has not moved" /tmp/hostX.log`

---

## 10. 遗留未做

- 设备端跑一轮同样的探针（宿主与 bionic 行为需交叉验证）
- 清理测试目录：`workM2..M9`、`workN1..N9`、`workP1`、`workEL0*`、`workT_*`、
  `workW*`、`workD1`、`workG1/G2` 等（需确认后删除）
- 文档交叉引用需要更新到 `docs/restore-progress-2-2026-11-16.md` §36 之后
