# 加了 fault 的 ttbr0 之后：缺页页确实"装入失败"，不是"没登记"

## 1. 新增的诊断字段

`core/exception.c` 的 EL0 fault 环原先只记录 pc/far/esr/sp/寄存器，**没有记录地址空间**。
多个用户进程交错运行时，fault 环是全局的，而 `hv_last_el0_ttbr0` 只代表"最后一次"那个进程
—— 所以之前一半的 fault 无法与其 vm_map 对上。

改动只有一行（纯记录，不影响 guest）：

```c
uint64_t hv_el0_flt_ttbr0[HV_EL0_FLT];          /* 新增数组 */
...
hv_el0_flt_ttbr0[fi] = cpu->ttbr0_el1;          /* 记录时多存一个值 */
```

`cpu->ttbr0_el1` 本来就在 `HvCpu` 里，不新增结构字段。

## 2. 精确匹配的结果（runS6，pid 21488）

把 5 个 vm_map 按各自 pmap 的 ttb0 建索引，每条 fault 用**它自己的 ttbr0** 找对应的 map：

```
far           kind      ttb0        entry                              PTE
0x16dac70f8   trans-L3  0x97b04000  [0x1023c0000, 0x7ffffe000000)      NONE
0x16da3f7f0   trans-L3  0x97b04000  [0x1023c0000, 0x7ffffe000000)      NONE
0x1027e9f9c   trans-L3  0x97b04000  [0x1023c0000, 0x7ffffe000000)      NONE
0x102f1db6c   trans-L3  0x97b04000  [0x1023c0000, 0x7ffffe000000)      NONE
0x1007401cc   trans-L3  0x72cc8000  [0x1006d8000, 0x7ffffe000000)      NONE
0x102864008   trans-L3  0x97b04000  [0x1023c0000, 0x7ffffe000000)      NONE
0x16f089250   trans-L3  0x72f10000  [0x100d74000, 0x7ffffe000000)      NONE
0x1007401cc   trans-L3  0x72cc8000  [0x1006d8000, 0x7ffffe000000)      NONE   ← 重复
```

## 3. 三个确定的事实

1. **每条 fault 都有对应的 vm_map entry** —— 登记是存在的，不是"野地址"；
2. **PTE 全部为 NONE** —— 页表里没有这一页；
3. **`0x1007401cc` 在 8 条里出现两次** —— 同一地址缺页两次，
   说明第一次缺页处理**没有建立映射**，于是再访问再缺页。

这就是"反复缺页"的完整机制：**访问 → 缺页 → 未建立 → 再访问 → 再缺页**。

## 4. 一个重要的形态特征

所有 fault 命中的 entry 都是**延伸到 `0x7ffffe000000`（用户空间上界）的巨型区间**：

```
[0x1023c0000, 0x7ffffe000000)   约 8.5 亿页
[0x1006d8000, 0x7ffffe000000)
[0x100d74000, 0x7ffffe000000)
```

这种"上界一路顶到地址空间顶端"的 entry，在 XNU 里是**预留区**的典型形态
（栈的 grows-down 预留、malloc/JIT 的地址预留、或 shared region 的占位）。
它们存在的目的是"占住地址空间，防止被别的映射抢走"，**内部页按需逐个兑现**。

所以缺页本身是**预期行为**，问题在于：**内核没有兑现这些页**。

## 5. 与之前结论的关系

| 之前的说法 | 现在的状态 |
|---|---|
| "VM Map 有登记" | ✅ 确认（精确匹配到 entry） |
| "PTE 缺失" | ✅ 确认（全部 NONE） |
| "反复缺页" | ✅ 确证（同一地址在 8 条里出现两次） |
| "可能是 guard 页，缺页本应失败" | 仍然可能：ESR 是 translation fault，而 guard 通常给 permission fault，但 guard 若根本没有 PTE 也会是 translation fault —— **需要看 entry 的对象字段才能定** |

## 6. 下一步（只剩一个岔路口）

读这些巨型 entry 的**对象字段**（`+0x40` object union / `+0x48` offset）：

| 读到什么 | 含义 | 结论 |
|---|---|---|
| `object_value = 0` | 匿名/内核对象（应装零页） | 零页装入路径没走成，问题在 `vm_fault` 的匿名分支 |
| 有非零对象 / 子映射 | 文件或子映射 | 需要 pager，问题可能在 pager 或子映射 |
| offset 异常（如超范围） | 登记本身可疑 | 回到 guest 侧的映射逻辑 |

这一步就能把"为什么一直死在那里"从"在某处失败"收敛到"在哪一步失败"。
