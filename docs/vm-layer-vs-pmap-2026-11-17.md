# `0x100fc0000` 的故障层次：VM Map 有 entry，PTE 没建立（2026-11-17）

按"区分实测/推导/假设"的方式回答三个问题，结论是**故障在 VM 层与 pmap 层之间**，
不是"VM Map 里没有描述"，也不预设是模拟加载器的问题。

## 问题 1：这一页属于哪个 Segment

| 类型 | 内容 |
|---|---|
| **实测** | 不在 restore ramdisk 的 1098 个文件（特征字节全扫）；不在 `usr/lib/dyld`；不在 `libsandbox.1.dylib` / `libMatch.1.dylib` |
| **实测** | 代码页 `0x100f9c000` → PA `0xafc38000`，`+0x568` 处是 `d503237f`(`pacibsp`)，内容正确 |
| **推导** | 属于 `0x100f9c568` 所在 image（运行期基址约 `0x100f00000`），落在其 `__TEXT` 之后，形态符合**文件支持的数据段**（`__DATA`/`__DATA_CONST`），而非零填充区 |
| **假设（不可验证）** | 精确的 vmaddr/vmsize/fileoff/filesize 需要该 image 的 Mach-O header，而它在 dyld shared cache 中，只存在于 **加密的 `043-18152-097.dmg.aea`（9.3 GB AEA）**。ramdisk 不含 shared cache，所以这四项拿不到 |

## 问题 2：VM Map / Object / Pager / Pmap 的状态

**这是本次最重要的结果 —— 不能用 PTE 缺失推断 VM Map 缺失。**

用 `vm_map_entry` 的真实布局（首字段 `vm_map_links { prev, next, start, end }`）
在 RAM 里定位，`0x100fc00e0` 命中唯一一个节点，且它**确实在一条双向链表上**：

```
节点 @ VA 0xfffffe11260bfa60 (PA 0x8e33ba60)
  +0x00 prev = 0xfffffe11260bfab0
  +0x08 next = 0xfffffe11260bfc40
  +0x10 start= 0x100fc0000      ← 我们的页
  +0x18 end  = 0x100fc4000
  +0x38      = 0x57bfe66000000000   (mach_absolute_time 形态)
  +0x48      = 0x2002b980
```

沿 `next` 走，节点依次覆盖：

```
[0x100fc0000,0x100fc4000) [0x100fc4000,0x100fd8000) [0x100fd8000,0x100fdc000)
[0x100fdc000,0x100fe0000) [0x100fe0000,0x100fe4000) [0x100fe4000,0x101014000)
[0x101014000,0x101018000) [0x101018000,0x10101c000)
```

每个节点的 `prev`/`next` 互相回指，`start`/`end` 首尾相接 —— **这是 map entry 链，不是栈上临时结构**
（先前在 `0x8e33ba00` 附近看到的正是这些节点本身；早前把它误判为"栈局部变量"是因为
`prev/next` 恰好指向同一片内存，而这片内存的 PA 在 `0x8e33xxxx`）。

| 层 | 状态 | 依据 |
|---|---|---|
| **VM Map** | **有映射描述**（实测） | 上述链表节点，`start=0x100fc0000`、`end=0x100fc4000`，prev/next 互指 |
| **Pmap / 页表** | **无驻留页**（实测） | `L3[1008] = 0x8000000000000000`，bit0=0；间隔 1 秒两次采样一致 |
| **VM Object / Pager** | **未知** | `+0x48=0x2002b980` 与 `+0x38` 时间戳只能说明"该 entry 最近被创建/修改"，不足以判定是 vnode-backed 还是匿名 |

**因此：`translation fault L3` 不是"VM Map 没有描述"，而是"VM 层认为这一段映射存在，但最后一级 PTE 没有被写入"。**

## 问题 3：缺页处理前后页面状态是否变化

| 实测 | 结果 |
|---|---|
| `L3[1008]`，间隔 1 秒两次 | 都是 `0x8000000000000000`，**无变化** |
| `hv_uexc_n` / `hv_el0_flt_n` | 稳定 53022 / 21150，不再增长 |
| 两个 CPU | 都在 `wfi`，`x29 == sp+0x30`（栈空） |

**ESR 全字段解码（实测）**：

```
EC = 0x24  Data abort, lower EL
IL = 1
DFSC = 0x07  translation fault L3
WnR  = 0    read
EA   = 0    不是 external abort   ← 排除"设备/I/O 返回错误"
FnV  = 0    FAR 有效
S1PTW= 0    不是页表遍历自身出错
```

**`EA=0` 是关键**：若缺页由 I/O 失败引起，通常会以 external abort 形式出现；这里不是。
所以故障来自页表层，而不是设备返回错误。

## 最可能的故障层次

| 排序 | 层次 | 支持证据 |
|---|---|---|
| **1** | **VM → pmap 之间的页装入没有落地**（`vm_fault_enter` / `pmap_enter` 这一环没有把 PTE 写入） | VM Map 有 entry（实测）；PTE 无（实测）；ESR 非 external（实测）；处理后无变化（实测） |
| 2 | VM Map 有 entry 但 pager 从未被调用（entry 是新建/占位的） | entry 的 `+0x38` 时间戳暗示它较新；但 pager 状态未知 |
| 3 | 引擎侧 PTE 写入未生效 | 页表项确实没变；但引擎 walk 已多轮验证正确 |

**结论（推导）**：故障层次在 **VM 层之下、PMAP 之上** ——
"有人为这段地址建立了映射描述，但没有任何人把这一页的 PTE 填上"。

## 下一步最小验证实验

**实验 A（零改动，首选）**：把引擎已有的诊断变量 `hv_walk_dbg` 指向文件
（运行时 `set var hv_walk_dbg = fopen(...)`，不改代码），让 guest 再 fault 一次。
它会逐级打印 `walk va L? base idx desc`，从而判定 **PTE 到底是"从未被写"还是"写了但被清"**。

**实验 B（零改动）**：在 `0xfffffe11260bfa60` 这个 entry 上，检查 `vme_object`/`vme_offset` 字段
（需先确认该版本的字段偏移，可用 `vm_map_entry` 的 `links` 已对齐来反推），
判断是 vnode-backed 还是匿名 —— 前者缺页应触发 pager I/O，后者应按需补零。

**实验 C（零改动）**：确认 `hv_el0_flt_n` 这些故障的时间分布是否**集中在某一小段时间**
（若是，说明是一次性爆发的循环；若均匀分布，说明被反复重试）。

---

## 实验 A 结果（`hv_walk_dbg`，零代码改动）

### 做法

新一轮（pid 9328，`workS1`）起来后，`hv_el0_flt_n` 已到 27770 且仍在缓慢增长，
此时用 gdb 把引擎已有的诊断变量指向 stderr：

```
set var hv_walk_dbg = (void *)stderr
```

（`stderr` 在 gdb 里没有类型，必须 cast；`(FILE*)&__sF[2]` 也不行，`__sF` 同样无类型。）
故障期间引擎把每一次 `hv_mmu_walk` 的三级轨迹写进 `runS1.log`，随后置回 0。

### 结果

日志里 `^walk ` 共 172 449 行，其中用户态地址（`0x10xxxxxxx`）39 行，集中在 4 个地址，
每个地址都是同样的三级轨迹：

```
walk 0x102be5650 L1 base 0x974d0000 idx 0    desc 0x976ec003   ← 有效，指向下一级表
walk 0x102be5650 L2 base 0x976ec000 idx 0x81 desc 0x97c50003   ← 有效，指向 L3 表
walk 0x102be5650 L3 base 0x97c50000 idx 0x2f9 desc 0            ← 表项为 0
```

同一地址在 15 次 fault 中重复出现，**每次都是同样的 L1/L2/L3，L3 都是 0**。

### 这个结果排除了什么

| 假设 | 判定 |
|---|---|
| "引擎读错了页表 / 没看见 PTE" | **排除**：引擎自己打印的轨迹 L1、L2 都有效且指向正确的下一级表，L3 读出来是 0；与手工遍历（`L1[0]`、`L2[128]`、`L3[999]=0`）完全一致 |
| "PTE 写了又被清" | **排除**：15 次 fault 之间没有任何一次看到非 0 的 L3 |
| "VM Map 里没有这段地址" | **排除**（上一节）：`0x100fc0000` 确实在一条 `vm_map_entry` 双向链表上 |

### 因此剩下的层次

```
VM Map：有 entry                        （实测，链表可走通）
   ↓
vm_fault_enter / pmap_enter：**没有把 PTE 写进去**   ← 断在这里
   ↓
Pmap：L3 表项恒为 0                     （实测，引擎轨迹 + 手工遍历一致）
```

**两个独立 run（旧 `0x100fc00e0`、新 `0x102be5650`）表现完全一致**，
说明这不是某一次偶然，而是这条路径上的稳定行为。

### 下一步最小实验（仍是零改动）

在 fault 期间用 gdb 读 `cpu->m->stats` 里与 fault/pmap 相关的计数（若有），
或对内核里 `pmap_enter` 所在的函数下 `HV_PCHK`（地址可由 `tools/lcfs.py` 从
`LC_FUNCTION_STARTS` 索引里按"该函数被调用次数"筛出来），
确认 **`pmap_enter` 到底有没有被调用过**：

- 没被调用过 ⇒ 卡在 `vm_fault_enter` 之前的判断（例如 object/pager 状态不允许装入）；
- 被调用过但 L3 仍为 0 ⇒ 写入目标表不对，或写入后被立即清掉。
