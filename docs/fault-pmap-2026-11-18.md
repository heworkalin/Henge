# 缺页到底走没走到 pmap_enter（2026-11-18）

> 目标：回答 `docs/investigation-tree.md` §9 那个问题 —— 缺页处理跑完了，为什么 PTE 没建立。
> 手段：`HV_TRACE_PC`（真实执行记录）+ `HV_PCHK`（独立计数）+ `tools/lcfs.py` 函数边界。

## 0. 先纠正两个前提

| 前提 | 实测 |
|---|---|
| `HV_TRACE_PC` 只记前 4M 块 | 一轮只有 ~38 万块，够用；另加了 `HV_TRACE_TAIL=1`（环形，保留最后 4M 块）作为保险 |
| `pmap_enter_options` 地址 0x1ca2206c..0x1ca233ac 的地址空间 | 与 `work/kernel_functions.json` 的函数边界**逐字节吻合**，就是 run VA；本轮 **slide = 0**（PCHK 打印 `slide 0`） |

## 1. 结论一：`pmap_enter_options` 确实被调用，而且正常返回

- 入口 `0x1ca2206c` 命中 **33 次**（PCHK 与 trace 一致）。
- 反汇编里 6 条早退分支（`+0xcec`、`+0xd30`、`+0xd5c`、`+0x11c0`、`+0x11e0`、`+0x1240`）
  **一次都没有执行** —— 不是参数校验失败。
- 调用序列平均 10–34 个基本块，是完整路径；返回地址分布在多个调用者
  （0x1c902074、0x1ca396f4、0x1c904cf4 …），说明不是同一条错误路径。

## 2. 结论二：用户态的缺页几乎不进入它

按 trace 分区统计（一条 trace 覆盖整个引导，按地址自然分区）：

| 区域 | 块数 |
|---|---|
| 固件（< 0x100000000） | 89 814 |
| 内核（≥ 0xffff000000000000） | 276 474 |
| 用户态（0x100000000…） | 18 546 |

用户态开始（索引 307 621）之后：

| 指标 | 值 |
|---|---|
| EL0 同步异常（向量 `0x1c893400`） | **6 515** |
| 进入内核异常入口后续（`0x1c894448`） | 1 599 |
| `pmap_enter_options` 入口 | **2** |

即 **6515 次缺页里只有 2 次走到 pmap_enter_options**。这否定了
“调用过但写没生效”这个分支的**主要形态** —— 绝大多数缺页在更早的地方就返回了。

## 3. 结论三：ESR 里混着两类故障，`pmap_enter` 只服务其中一类

同一轮日志的 data abort 分布：

| ESR | 次数 | 类型 |
|---|---|---|
| `92000007` | 2664 | translation fault L3（读） |
| `92000047` | 324 | translation fault L3（写） |
| `9200004f` | 1155 | **permission fault L3** |
| `9200004b` | 492 | access-flag fault L2 |
| `9600004f` / `96000047` | 267 | **same-EL abort（内核自己）** |

permission fault 不走 `pmap_enter`（走 `pmap_protect`／保护错误路径），
access-flag fault 也不走。只有 translation fault 才应该走 `pmap_enter`。
所以“fault 数 vs pmap_enter 数”不能直接比；**要按 ESR 分开算**。

## 4. 结论四（方法教训）：全局 trace 是多 CPU 交错的

`hv_trace_note()` 写的是**所有 CPU 共用的一个缓冲区**，两条 CPU 流按执行顺序交错。
因此“向量块的下一条是哪个块”这种相邻分析**不成立** —— 下一条可能是另一个 CPU 的块。

可用的只有**计数**（PCHK 或 trace 的频次），不可用的是**相邻关系**。
要还原单 CPU 路径，必须先让 trace 按 CPU 分离（`hv_pc_hist_note_va(index, va)` 已经有
`index`，改成每 CPU 一个文件即可，改动很小）。

## 5. 用户态阶段的内核热点

| 函数起点 | 块数 | 猜测（未证实） |
|---|---|---|
| `0x1c893000` | 9 817 | 异常/中断向量页 |
| `0x1ca35d68` | 7 919 | 疑似 idle/自旋（trace 最后一块 `0x1ca35e68` 也在这里） |
| `0x1ca42818` | 1 689 | 未知 |
| `0x1c8943e4` | 1 611 | EL0 同步异常入口（`fleh_/sleh_synchronous`） |
| `0x1ca431e0` | 821 | 未知 |
| `0x1e70b0d8` | 545 | `com.apple.iokit.IOUSBHostFamily`（USB 路径） |

内核函数**没有符号**（`tools/ksym.py` 只覆盖 kext，kernel 段解析出来是
`_sSortedProcessWhitelist + 0x19446c` 这类数据符号），所以上表只能给函数边界，不能给名字。

## 6. 下一步（按优先级）

1. **trace 按 CPU 分离**（改 `core/dispatch.c` 的 `hv_trace_note`，每 CPU 一个文件）。
   这是唯一能把“某次缺页之后内核走到了哪”还原出来的办法。
2. **按 ESR 分类统计**（trans-L3 / perm-L3 / access-flag 分开），再看每一类各自的
   后续路径；现在把三类混在一起算比例是没有意义的。
3. 用分离后的 trace 检查：translation fault 之后，内核是否进入了 `vm_fault` 一类的函数
   （函数边界可用 `tools/lcfs.py`，但没有名字，需要靠调用 pmap_enter 的返回地址反推）。

## 7. 工具

- `tools/trace-stats.py`：trace 离线统计（范围/热点/上下文）。
  注意：**32 位内核地址会自动提升到 0xfffffe00xxxxxxxx**，否则 `0x1ca2206c` 永远 0 命中
  （这个坑已经踩过，脚本里已修）。
- `tools/henge-gdb/`：现场探针（faults / walkva / vmmap / entries / images / procs / blk …）。
- `SIGUSR1` → `hv_trace_dump()`：卡住时随时把 trace 落盘，不用等超时退出
  （`tools/host-install.c`）。

## 8. 复现

```bash
./tools/build-install-host.sh
CURL_CA_BUNDLE=$PWD/work/ca-bundle.pem \
  HV_TRACE_PC=/tmp/tr.bin HV_TRACE_TAIL=1 \
  HV_PCHK=1ca2206c:1ca233ac HV_PCHK_CPU=all HV_PCHK_MAX=200000 \
  ./build/host/host-install work/AVPBooter.vmapple2.bin android/ipsw/24G90.ipsw work 2048 2
# 等到 fault 阶段（约 90 s），然后：
kill -USR1 $(pgrep -x host-install)
python3 tools/trace-stats.py /tmp/tr.bin --range 0x1ca2206c:0x1ca233ac
```

---

# 追加：per-CPU trace 与一个更根本的记录局限（同日）

## 做了什么

`HV_TRACE_CPU_SPLIT=1`：每个 CPU 一个缓冲，落盘为 `<file>.c0`、`<file>.c1`。
`HV_NOCHAIN_VA=lo:hi`：对指定地址区间不建立链（`core/dispatch.c` 的 `chain()` 开头）。

两个都**默认关闭**，只用于读现场；关闭时引擎行为与之前完全一致，正式版可以整段删除。

## 结果一：交错确实存在，但不是主因

分 CPU 后仍有大量 "向量块之后直接是 EL0"：

| CPU | 块数 | EL0 同步异常 | 异常后进内核入口 | 异常后直接回 EL0 |
|---|---|---|---|---|
| cpu0 | 328 510 | 3 305 | 1 019 | 2 286 |
| cpu1 | 64 383 | 3 065 | 432 | 2 633 |

## 结果二：真正的原因是 **16 KiB 页内联**

guest 页是 **16 KiB**，`0x1c893000..0x1c896fff` 是同一页。向量表（`0x1c893400`）、
`fleh/sleh` 入口（`0x1c8943e4`）、第一层分发（`0x1c894448`）**全在这一页里**。

- 链只在同一 16 KiB 页内建立 → 整页的 handler 可以不经过 dispatcher；
- superblock（`HV_SUPERBLOCKS=1` 默认）让条件分支不结束块 → 一段直线 handler 被并进一块。

所以 trace 记录的**不是"执行的块"**，而是"进入 dispatcher 的块"。
对同页内的异常处理，这几乎等于没有记录。`HV_NOCHAIN_VA` 只挡住新的链，
挡不住已经内联进块的内容。

**这条要写进方法论**：`HV_TRACE_PC` 与 `HV_PCHK` 都不能给出块级路径，
除非彻底关掉 chain 和 superblock；而彻底关掉会让 DFU 阶段超时（实测两次）。

## 结果三：结论反而更硬了

用"函数范围内出现的块数"作为下界（superblock 只会**减少**块数，不会凭空增加）：

| 阶段 | EL0 同步异常 | `pmap_enter_options` 范围内的块 | 其中入口 |
|---|---|---|---|
| 用户态（cpu0） | 3 305 | **16** | 0 |
| 用户态（cpu1） | 3 065 | **10** | 2 |

一个函数被调用 N 次，其内部至少会出现 N 个块（每次调用都要取指令）。
26 块对 6449 次异常，差了三个数量级 —— **用户态的缺页处理基本不进入
`pmap_enter_options`**，这个结论不受 superblock/链的影响。

## 下一步

要把"不进入 pmap_enter"往前追一步，需要**按页关 superblock**
（`go_on()` 里加一个 `HV_NOSUPERBLOCK_VA=lo:hi` 判断，与 `HV_NOCHAIN_VA` 同构），
把 `0x1c893000` 页完全展开，同时不影响其他页的性能。
这是唯一能在不拖慢 DFU 的前提下看到真实块级路径的办法。

---

# 追加二：整页关 superblock 之后（同日，仍未收尾）

## 新开关

`HV_NOSUPERBLOCK_VA=lo:hi`（`core/translate.c` 的 `go_on()`）：该范围内条件分支结束块。
与 `HV_NOCHAIN_VA`、`HV_TRACE_CPU_SPLIT` 一样**默认关闭**。
`HV_TRACEEXIT=lo:hi`（`core/dispatch.c`）：打印一个块执行后的 `exit_reason` 和目标 pc，
用来回答"这个块跑完之后去了哪"。

## 结果

对 `0x1c893000..0x1c897000` 同时关链、关 superblock，vector 块 `0x1c893400` 仍然：

| `exit_reason` | 次数 |
|---|---|
| 1（BRANCH） | 6 808 |
| 4 | 23 |

按目标 pc 分：

| 目标 | 次数 |
|---|---|
| `0x1c894448`（`fleh` 入口直线之后） | 1 391 |
| EL0 地址（`0x100928f90`、`0x100927ef4`、`0x10099aa2c` …） | 4 949 |

**没有任何一次的目标是 `0x1c8943e4`** —— 也就是 `b` 的静态目标。
而 `0x1c8943e4` 只作为块入口出现 0–2 次。

## 这意味着什么（仍属推断，未证实）

`0x1c893400: b 0x1c8943e4`。目标块 `0x1c8943e4` 与它同页，代码是直线到 `0x1c894444`，
所以"进入 `0x1c8943e4`"必然以 `0x1c894448` 作为下一个块。
观察到 1 391 次正是如此 —— 说明这条路径**被执行了，但目标块入口没有被记录**
（分支直接跳进了已编译代码，不经 dispatcher）。

但另外 4 949 次的目标是 **EL0 地址**，而 exit reason 仍是 BRANCH。
一个 `b` 不可能同时跳到内核和 EL0：这说明 **`0x1c893400` 这个 VA 存在不止一个翻译块**，
不同 flags（EL0/EL1、页表/ASID）下内容不同。

待验证的假设：EL0 也走到了 `0x1c893400`（错误的分支目标、或权限异常后重新进入），
在 EL0 的块里同一个地址被翻译成别的代码，于是"异常向量"变成了普通跳板。

## 下一步（明确）

用 `HV_WATCH=1c893400:1c893400` 在第一次命中该 pc 时 `SIGSTOP`，然后 gdb 读
`g_cpus[i].el / ttbr0_el1 / ttbr1_el1 / pc`，确认这次执行是 EL1（真异常注入）还是 EL0。
如果存在 EL0 走到内核向量的情况，那就是地址空间隔离的问题，而不是缺页处理的问题。

## 不变的结论

用户态阶段 `pmap_enter_options` 范围仍只有 26 块 / 2 次入口，对应 6 449 次 EL0 同步异常。
**缺页基本不进入 `pmap_enter_options`** —— 这一条不受上面任何机制解释的影响。

---

# 追加三：按 ESR 配对的真实数据（同日）

修了一个让开关"看起来没生效"的 bug：`HV_NOCHAIN_VA`/`HV_NOSUPERBLOCK_VA` 的范围是
32 位写法（日志习惯），却和 64 位 VA 比较 → 永远不匹配。现在会自动提升到
`0xfffffe00xxxxxxxx`。**之前"整页关 superblock 后仍然直接回 EL0"的观察，是开关没生效。**

修好后对 `0x1c893000..0x1c897000` 关链 + 关 superblock：vector 块执行后
**100% 进入 `0x1c8943e4`**（cpu0 694/694，cpu1 5758/5758）。

## ESR × 路径（cpu1，用户态，6451 次异常与 6367 个 vector 一一配对）

| ESR / 类型 | 次数 | 平均块数 | 到达 `pmap_enter_options` |
|---|---|---|---|
| `56000080` svc | 4803 | 37.6 | 0（应当为 0） |
| `92000007` trans-L3 | 1094 | 60.4 | **6** |
| `9200004f` perm-L2/L3 | 432 | 56.8 | **7** |
| `82000007` instruction abort | 30 | 131.5 | 1 |
| `92000006` trans-L2 | 8 | 54.1 | 0 |

所以缺页（trans）每次确实走了 **~60 个基本块**的内核处理，不是"快速返回"。

## 缺页路径的实际函数序列（trans-L3，典型）

```
0x1c8943e4   x6    EL0 同步异常入口（fleh/sleh）
0x1c894b4c   x9    汇编层分发
0x1ca3e22c   x8    C 层：lsr w8,w20,#26 → 按 EC 查表跳转（arm_exception_handler）
0x1ca3eea0   x1    （见下）
0x1ca3f034   x2    thread+0xcc 的 ldclr/tst，函数出口
```

`0x1ca3e22c` 是 ESR 解码 + EC 跳转表（表基址在 `...eef0`，掩码 `0xeef0` 覆盖
translation/access-flag/permission 三类 DFSC），确认它把 `0x92000007` 判为合法缺页。

`0x1ca3eea0` 内部 trans fault 只经过 **+0x194（`0x1ca3f034`）**：

```
add  x23, x22, #0xcc        ; x22 = TPIDR_EL1
ldclr w8, w8, [x23]         ; 清 thread+0xcc 的位
tst  w8, #0xff00
b.ne <错误路径>
cbz  w26, <正常返回>
```

而 `+0x194` 之前是 `bl 0xe82adc`。**注意 objdump 打印的是文件偏移**：
run VA = `0x1bb18000 + 0xe82adc = 0x1c9a00dc`，是 **`vm_fault` 入口**
（`0x1c9a0014..0x1c9a2300`），不是 `0x1ca9a0dc`。早期这里按 `0x1ca9a0dc` 去查过，
那个地址其实是 dtrace/machtrace 的一个 0xac 小函数，属于读错对象。

## 最后一个盲区

`bl` 的目标块执行时**不重新进入 dispatcher**（JIT 直接调用已编译代码并返回），
所以 `vm_fault`（`0x1c9a00dc`）及其内部（是否调用 `pmap_enter`）在 `HV_TRACE_PC` 里不可见。
（块入口 `trace_ring` 能看见，见下一节。）
同理，`pmap_enter_options` 的"26 块"这个数字也**可能偏低**，虽然 26 对 6000 的差距太大，
大概率仍然说明它没被大量调用，但**严格说不能由 trace 定论**。

## 要彻底定论，需要一次插桩（不是开关能解决的）

在 `core/translate.c` 生成块时，对**指定 VA** 的块入口插入一个计数器写
（保存/恢复两个寄存器，受环境变量控制，默认不插）。这样每次执行该块都能被计数，
与 chain/superblock/直接调用全部无关。

这是目前唯一能不依赖 dispatcher 的观测方式。工作量约：translate 侧 20 行 +
dispatch 侧一个计数槽 + 一个导出函数，风险在于插桩代码必须不破坏寄存器分配，
所以只对指定 VA 插、默认关闭。


---

# 追加（2026-11-19）：L5 的四个结论被推翻

用 `put_trace_entry` 的块入口 `trace_ring`（不经过 dispatcher，chain / superblock /
`bl` 直调全覆盖）加 `core/exception.c` 的临时探针，重新测了一轮。探针全部由
`HV_FAULT_PROBE=lo:hi,...` 控制，默认只探 `pmap_enter_options`。

## 1. 统计 bug：`pend_esr` 会被 IPI 覆盖

第一版测出 "translation fault 1035 次，到达 `pmap_enter_options` **0 次**"，并据此写了
"pmap 从来没被调用"。**这是假的**：`pend_esr` 在 `hv_take_exception` 里设置，在 `hv_eret`
里消费；缺页处理期间进来的 **IPI 中断**会把它覆盖成中断的 ESR，于是 `hv_eret` 那一刻
扫到的 ring 是**中断的路径**（`0x1ca36xxx` = cpu_xcall / machine_routines / locks_arm），
不是缺页路径。直方图当时显示 `0x1ca38000` 等页为热点，正是 IPI 代码。

修法（两处，缺一不可）：
- 只记第一个从 EL0 取的异常，且带 `pend_valid` 标志，不允许覆盖；
- `hv_eret` 里只在**目标 EL 是 EL0**（`spsr_el1 & 0xf == 0`）时消费。

修好后同一轮的真实数据：

```
translation n=1107  handler 98.6%  vm 层 98.6%  pmap_enter_options 95.2%
permission  n=431   handler 99.8%  vm 层 98.8%  pmap_enter_options 97.9%
```

**所以 `pmap_enter_options` 是正常被调用的。** 之前"26 块/几乎不调用"的判断是被
trace 盲区和这个统计 bug 双重误导。

## 2. PTE 不是 0：`pmap_enter_options` 确实写成功了

在 `hv_eret`（handler 已跑完、即将回 EL0）用 `hv_mmu_walk()` 按 **fault 时刻的 TTBR0**
遍历该 fault 地址：

```
translation n=1291  mapped=1291  trans-L3=0  （reached_pmap 的 1288 次全部 mapped）
permission  n=502   mapped=502   trans-L3=0  （reached_pmap 的 500 次全部 mapped）
```

**100% 已映射，一个 L3 translation 都没有。** "PTE 恒为 0、写了但没生效"不成立。

注意 walk 必须用 `pend_ttbr0`（fault 时的地址空间）而不是当前 `cpu->ttbr0_el1`，
否则 handler 切了进程就会答成别人的页表。

## 3. 不是死循环，是极慢推进

两次采样（t=75s / t=188s，同一轮）：

| | total | unique pages | repeats |
|---|---|---|---|
| t=75s | 930 | 403 | 527 |
| t=188s | 2867 | **1504** | 1363 |
| 增量 113s | 1937 | **+1101（≈10 页/s）** | +836 |

唯一页持续增长 → **在推进**，只是慢。重复 47% 且分散在同一进程
（`ttbr0 0x8583c000`）的几十个页上，每页 10~21 次，**不是单页死锁**。
（哈希键必须含 TTBR0；只用 VA 页号会把不同进程的同名 VA 算成重复。）

## 4. 真正的现象：系统 95% 空闲

引擎自己的 stats：

```
stats 199s: blocks 0.00M/s, exc 18/s irq 26/s, wfi 100/s (95% idle)
el1 pc fffffe001ca35e68      ← 固定在 idle 循环
idle daif 3c0 cntp_ctl 2 cntp_cval 0
```

每秒只有 ~18 次异常（≈缺页数）和 ~26 次中断，块执行速率是 0。也就是说：

> **没有 runnable 线程。CPU 在 idle 循环里等中断，靠已过期的物理定时器周期性醒来，
> 醒一次推进十几页又睡回去。**

这与 `docs/investigation-tree.md` 里"所有线程挂 `WQT_QUEUE`、两个 CPU 都 WFI"是同一件事。
缺页只是"醒来时干了点活"的副产品，**不是卡点本身**。

## 5. 顺带纠正：`bl 0xe82adc` 不是 `0x1ca9a0dc`

objdump 对裸 binary 打印的是**文件偏移**：`0x1bb18000 + 0xe82adc = 0x1c9a00dc`
（`vm_fault` 入口，函数 `0x1c9a0014..0x1c9a2300`）。`0x1ca9a0dc` 是 dtrace/machtrace
的一个 0xac 小函数，和缺页无关。

## 6. 函数命名方法（本次用的）

没有符号也能命名：`adrp` + `add` 的字符串引用。解析指令位域自己算目标，不要信
objdump 的地址显示（`--adjust-vma` 对裸 binary 不可靠）：

```
adrp: imm = (immhi<<2)|immlo（21 位有符号），目标页 = (pc & ~0xfff) + (imm<<12)
add : 目标 = 页 + (imm12 << (12*shift)), shift ∈ {0,1}
```

结果（同一 kernelcache，slide=0）：

| 地址 | 身份 |
|---|---|
| `0x1ca21f84` | `pmap_map`（冷 panic 块在 `0x1ca2203c`） |
| `0x1ca2206c` | `pmap_enter_options_internal`（字符串 `0x1ca22d68` 自证） |
| `0x1ca3e22c` | sleh.c 异常分发（`'Exception on 2-byte instruction'`） |
| `0x1ca3eea0` | sleh.c user abort handler（`'vm_fault() KERN_FAILURE from user fault'`） |
| `0x1ca36xxx..0x1ca39xxx` | machine_routines / cpu_common / locks_arm / arm_vm_init（**不是 pmap**） |
| `0x1ca9a06c` | dtrace/machtrace 小函数（与缺页无关） |

## 7. 下一步

卡点从"缺页不写 PTE"改为"**没有 runnable 线程**"。要查的是：

1. 内核 waitq 里到底谁在等谁（`tools/henge-gdb/procs.py`、`dump-waitq.py` 一类）；
2. 是否有本该到来的中断/完成事件没有送达（设备模型 vs 等待方）；
3. idle 期间 `exc 18/s` 的异常里，除缺页外还有什么。

`HV_FAULT_PROBE` 探针保留在 `core/exception.c`（默认只探 `pmap_enter_options`，
只统计不改变 guest 语义）。
