# 「为什么这个地址反复缺页」——机制已定（2026-11-17）

## 1. 结论先说

同一段代码里有两次访问，状态完全不同：

| 访问 | 页 | 现状 | 结果 |
|---|---|---|---|
| **取指** `0x100f9c58c` | `0x100f9c000` | **已映射**，L3[999]=`0x200000afc38ec3`，PA=`0xafc38000`，+0x568 处正是 `d503237f`(`pacibsp`) | 正常执行 |
| **数据** `ldr x8,[x8,#0xe0]` | `0x100fc0000` | **未映射**，L3[1008]=`0x8000000000000000`（bit0=0） | **每次 data abort** |

所以循环是：

```
执行 0x100f9c58c 的 ldr x8, [x8, #0xe0]
        ↓  far = 0x100fc00e0，页 0x100fc0000 未映射
   data abort (esr=0x92000007, translation fault L3)
        ↓
   进 EL0 同步异常向量 (vbar_el1=0xfffffe001c893000, +0x400)
        ↓  b 0xfffffe001c8943e4 —— 这是标准 XNU fleh_synchronous
   处理完返回
        ↓
   再次执行 0x100f9c58c，页仍然未映射
        ↓
        （循环）
```

**`hv_uexc_n = 52977`、`hv_el0_flt_n = 21122` 就是这么攒出来的。**
采两次样（间隔 1 秒）`uexc_n` 都是 53022 —— 说明现场已冻结，这些计数是历史值。

## 2. 证据链

### 2.1 granule 必须先读 TCR

```
TCR = 0x800226511a511
  T0SZ=17  TG0=2 → **16 KiB 页**   T1SZ=17  TG1=1 → 16 KiB
  → g=14, inputsize=47, stride=11, levels=3, first=L1
```

按 4 KiB / 4 级算会得到完全错误的索引 —— 这是本仓库反复踩的坑。

### 2.2 代码页是好的（两次误判后的更正）

用用户 TTBR0（`hv_last_el0_ttbr0=0x30000922d4000`，表基址 `0x922d4000`）手工走：

```
L1[0]   = 0x9266c003   (valid, table)
L2[128] = 0x92dd0003   (valid, table)
L3[999] = 0x200000afc38ec3  → PA 0xafc38000, AF=1, UXN=0, PXN=1
```

在 PA `0xafc38000+0x568` 读到 `7f2303d5 f657bda9 ...` = `pacibsp; stp x22,x21,...`，
**与先前 dump 的 `0x100f9c568` 完全一致**。引擎记录的 `page_pa=0x84df4000` 在同一偏移
也是同样 16 字节 —— 两个 PA 内容相同，**代码页没有问题**。

（中途我两次读错：一次把 `0x100f9c000` 判成未映射，是把 L1 当成了 L2；一次读页头
`0x29016ab8` 就断言"内容不对"，其实代码在 +0x568。）

### 2.3 数据页确实是空的

```
L3[1008] = 0x8000000000000000     bit0=0 → 无效描述符
```

`0x100fc0000` 是 `adrp x8, ...` 的目标页（序言里 `100f9c588: adrp x8, ...` 得到
`x8=0x100fc0000`），而 `ldr x8,[x8,#0xe0]` 读的就是这一页的 `+0xe0`。

### 2.4 guest 的异常入口是标准的

```
vbar_el1 = 0xfffffe001c893000
+0x400 low_el_a64_sync: 140003f9  = b 0xfffffe001c8943e4
```

反汇编 `0xfffffe001c8943e4`：

```asm
stp  x0, x1, [sp, #-0x10]!
mrs  x0, SCTLR_EL1
tbnz w0, #0x1f, ...          ; PAN 处理
... msr SCTLR_EL1, x0 / isb
mrs  x0, TPIDR_EL1           ; per-CPU
add  x1, x0, #0x100
ldr  x0, [x1]                ; per-CPU 的异常栈
autda / xpacd                ; PAC 校验（失败则 brk #0xc472）
mrs  x1, SP_EL0
str  x1, [x0, #0x100]        ; 保存用户 SP
msr  SP_EL0, x0              ; 切到内核栈
msr  SPSel, #0x0
stp  x0,x1 / x2,x3 ...       ; 保存寄存器
mrs  x1, TPIDR_EL1
ldr  x0, [x1, #0x120]
cbz  x0, ...
```

这是 XNU 的 `fleh_synchronous`（`osfmk/arm64/locore.s`），**代码本身正常**。

## 3. 所以"为什么反复缺页"

**不是 TLB 失效问题，不是引擎的 walk 错误，也不是 stale mapping**：

- 代码页好好的，能执行；
- 数据页 `0x100fc0000` **从来没被映射**（不是"映射了又被清"—— 两次采样 L3[1008]
  都是 `0x8000000000000000`）；
- 每条经过 `0x100f9c58c` 的执行都会读它 → fault → handler → 返回 → 再 fault。

**唯一的未解问题是：这个 fault 处理为什么没有把 `0x100fc0000` 补上。**

三种可能，下一步各有判据：

| 可能 | 判据 |
|---|---|
| 该页属于某个 vnode，需要 I/O 才能装入，而 I/O 完成事件没到 | 查该页的 vm_object 是否有 pager |
| 该页是匿名零页，本应"按需补零"，但补页路径没走到 | 查 `vm_fault_enter` 在该 VA 上的行为 |
| 该页**本不该**被映射（guard / 已被 munmap），而代码仍在访问它 | 查该页是否在 vm_map 里有 entry |

## 4. 复现这条证据的最小实验（不用改引擎）

```
1. 读 TCR 判 granule（16K）与 levels（3）
2. 读 hv_last_el0_ttbr0 做用户页表遍历
3. 逐级打印 L1/L2/L3 的描述符，而不是只看最终 PA
4. 对同一个 L3 表项间隔 1 秒采样两次：值不变 ⇒ 不是"映射又被清"
5. 读 vbar_el1，反汇编 low_el_a64_sync 的跳转目标
```

五步全部只读内存，本次就是这样定下来的。工具化脚本见 `/tmp/ucal2.py`、`/tmp/sample.py`、
`/tmp/vbar.py` 的逻辑（`tools/lcfs.py` 可用于把 handler 地址命名）。
