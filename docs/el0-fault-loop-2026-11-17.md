# 用户态缺页循环：`0x100f9c58c` 处 5 万次 data abort（2026-11-17）

## 1. 发现

从现场（pid 18499，`HENGE_STOP_ON` 保护）读异常环：

```
hv_uexc_n = 52977          ← 异常总数
hv_el0_flt_n = 21122       ← EL0 数据/取指故障数
```

最近若干条 EL0 故障的 PC **几乎全在同一个位置**：

```
#0 pc=0x100f9c58c  far=0x100fc00e0  esr=0x92000007  x8=0x100fc0000  sp=0x16f28ea40
#1 pc=0x100f9c594  far=0x100f9c594  esr=0x82000007  sp=0x16f202aa0
#7 pc=0x100f9c594  far=0x16f202aa8  esr=0x92000047  sp=0x16f202aa0  x29=0x16f202ad0
```

对照我们反汇编过的那段代码，一一对应：

```
100f9c588: adrp x8, ...           → x8 = 0x100fc0000
100f9c58c: ldr  x8, [x8, #0xe0]   → far = 0x100fc00e0   data abort, translation fault L3
100f9c594: stur x8, [x29,#-0x28]  → far = x29-0x28       data abort（写栈）
             同一条指令还会以 esr=0x82000007 出现        instruction abort（取指）
```

## 2. 关键：这不是快照，是**循环**

`hv_uexc_n` 5.3 万次、`hv_el0_flt_n` 2.1 万次，而 PC 集中在两条相邻指令上。
**它在同一个位置反复缺页**，不是在等某个事件。

## 3. 交叉核对：guest 页表里这一页真的没有

用引擎自己的 `walk()`（`tsbase.py`）会走**当前 CPU 的 TTBR0**，而 CPU 在 idle 时那是
内核页表，查用户地址必然无效 —— 这是一个反复踩的坑。正确做法是先读 TCR 判 granule：

```
TCR   = 0x800226511a511
  T0SZ  = 17        TG0 = 2 (16 KiB!)     TBI0 = 1
  T1SZ  = 17        TG1 = 1 (16 KiB)      EPD0 = 0
  → g = 14 (16 KiB), inputsize = 47, stride = 11, levels = 3, first = L1
```

**guest 用 16 KiB 页、3 级页表（L1/L2/L3）**，不是 4 KiB。用用户态 TTBR0
（`hv_last_el0_ttbr0 = 0x30000922d4000`，页表基址 `0x922d4000`）手工遍历
`0x100f9c000`：

```
L1 idx=0    desc=0x9266c003   (valid, table)
L2 idx=128  desc=0x92dd0003   (valid, table)
L3 idx=999  desc=0x0          ← 无效
```

**L3 表项是 0：这一页确实没有映射。** 同一 image 的另一个页 `0x100fc00e0`
反而映射到 `0xba3740e0`，所以不是整个 image 掉了。

## 4. 引擎记录过这一页，说明它曾经映射

`core/dispatch.c` 的 `hv_el0_note()`：

```c
uint64_t pg = cpu->pc & ~0x3fffull;        /* 16 KiB 对齐，与 granule 一致 */
if (pg != hv_last_el0_page_va) {           /* 只在页变化时才 walk */
    hv_last_el0_page_va = pg;
    hv_mmu_walk(cpu, pg, HV_ACC_EXEC, 0, &w);
    hv_last_el0_page_pa = w.fsc ? 0 : w.pa;
}
```

读出现场值：`hv_last_el0_page_va = 0x100f9c000`、`hv_last_el0_page_pa = 0x84df4000`。
因为只在**页变化时**才 walk，这个 `page_pa` 是"这一页第一次出现时"的结果。

**所以合起来是：**

```
0x100f9c000 曾经映射到 0x84df4000
   → 后来被 unmap（L3 表项清 0）
     → CPU 仍在执行这一页的代码
       → instruction abort / data abort 反复发生
```

## 5. 这条线索与 WQT_QUEUE 的关系

之前的结论是"全系统线程停在 `WQT_QUEUE` 等一个不来的 `wakeup()`"。
现在多了一个更靠前的事实：**同一时间，用户态在同一个 PC 上反复缺页 2 万次**。

两种解释都需要验证，不能先入为主：

| 解释 | 需要什么证据 |
|---|---|
| 缺页循环是**主因**，`WQT_QUEUE` 是缺页处理路径里的下游等待 | 缺页处理时内核栈上是否出现 wait 相关帧 |
| `WQT_QUEUE` 是主因，缺页循环是**线程被反复唤醒又立刻 fault** 的副作用 | 线程状态在 fault 与 wait 之间是否来回切换 |
| 两者由同一个更早的原因引起（如页表/TLB 语义偏差） | 引擎的 TLBI 处理与 guest 页表修改是否一致 |

## 6. 遗留的坑（记录，避免再踩）

1. **`walk()` 只走当前 CPU 的 `ttbr0_el1`**；查用户地址必须显式用
   `hv_last_el0_ttbr0`，并且 `hi = (va>>55)&1` 为 1 时要用 `ttbr1_el1`。
2. **granule 必须先读 TCR**：这版 guest 是 16 KiB（`TG0=2`），3 级页表；
   按 4 KiB 4 级算会得到完全错误的索引。
3. `hv_last_el0_page_pa` 是**首次出现时**的缓存值，不是当前值 —— 拿它当"现在的映射"
   会得出矛盾结论。
