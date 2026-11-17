# 答案：卡住的那一页**在 vm_map 里有登记**

## 1. 方法

沿 `vm_map+0x18`（`hdr.links.next`）遍历 entry 链，每个 entry 读 `+0x10`（start）和
`+0x18`（end），找覆盖目标地址的那一条。

四个 map 的遍历完整性都做了校验：**链走出来的首尾范围与 map 的
`min_offset`/`max_offset` 完全吻合**（例如 map `0x83fb8220` 走 1093 条，范围
`[0x100c84000, 0x7ffffe000000)`，与 `min/max` 一致）。

## 2. 结果

| map | min_offset | 目标 0x100fc0000 |
|---|---|---|
| `0x83fb8220`（restored_external） | `0x100c84000` | **ENTRY `[0x100fc0000, 0x100fc4000)` 大小正好 `0x4000`** |
| `0x83fb8590` | `0x104a0c000` | no entry（目标低于 min，不在该 map） |
| `0x83fbae20` | `0x102200000` | no entry（同上） |
| `0x83fbb2f0` | `0x104200000` | no entry（同上） |

同一 map 里，**代码页**落在另一个 entry：

```
代码页 0x100f9c000 → ENTRY [0x100f78000, 0x100fc0000)  大小 0x48000
```

两个 entry 在内存中恰好相邻（`0x8e33ba60` 与 `0x8e33bab0`），且互相回指：

```
卡住页 entry (+0x00 prev) = 代码页 entry
代码页 entry (+0x08 next) = 卡住页 entry
```

## 3. 这个答案意味着什么

**"档案馆里有登记"** —— 所以这不是"程序访问了一个它根本没权访问的地址"。

那么三种可能里就排除了第一种：

| 可能 | 判定 |
|---|---|
| 程序访问了未登记的地址（guest 自身越界） | **排除**，entry 确实存在 |
| entry 存在但 `protection = NONE`（guard/保留） | **待查**（正是下一步） |
| entry 存在、权限正常，但页没装进来（page-in 未完成） | **待查** |

而且 entry 的**形状本身很可疑**：它正好一页大小（`0x4000`），孤零零地紧跟在一个
`0x48000` 的大区间之后。这种"一页高的独立区间"常见于：
- 某次 `vm_map_protect` / `mprotect` 把一页从大区间里切出来单独设权限；
- COW（写时复制）拆出来的一页；
- 特殊用途页（如需要单独保护的页）。

## 4. 还没做到的

**entry 内部的 `protection` / `flags` / `vme_object` 字段位置没能确认。**

从 `vm_map_xnu.h` 推出的布局是：

```
+0x00 links(0x20)  +0x20 store(0x20)  +0x40 object union
+0x48 alias|offset  +0x50 位域(protection/max_protection/inheritance...)
```

但实测 dump 与这个假设不符：`+0x38` 落在 `0x57bfe66000000000` 这种形态上
（像 `mach_absolute_time` 时间戳，而不是 `rb_color`），`+0x50` 是内核指针。
说明该版本编译出来的 entry 布局与开源默认不同（可能有
`MAP_ENTRY_CREATION_DEBUG` 之类的调试字段，或 `VM_MAP_STORE_USE_RB` 的结构差异）。

**因此"protection 是否 NONE"这件事，目前不能下结论**，需要先把 entry 的实际布局定下来
（方法与定 `vm_map` 时相同：找一个能自校验的字段组合，而不是靠猜）。

## 5. 下一步

1. 定 entry 布局：用"`start`/`end` 严格递增且与链一致"作为基础校验，
   再对候选偏移逐个测试 `protection` 是否落在合法值域（`VM_PROT_READ|WRITE|EXECUTE` 的子集，
   即 1..7），取能对全部 1093 条 entry 都成立的偏移；
2. 若 `protection` 正常，则问题落在 page-in（`vm_fault_enter`/`pmap_enter`），
   与之前"PTE 从未被写"的观察一致；
3. 若 `protection = 0`，则这一页本就不可访问，需要回头看 guest 为什么会读它。
