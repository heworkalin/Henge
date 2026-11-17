# 重要教训：HV_PCHK 的命中数**不能**当作调用次数

## 1. 现象

三轮实测（runS11/S12/S13/S15）：

| 断点位置 | 命中数 | 同期 fault 数 |
|---|---|---|
| `pmap_enter_options`（0x1ca2206c..0x1ca233ac） | 241 | 17181 |
| `vm_fault`（0x1ca3eea0..0x1ca3f39c） | 25 | 21646 |
| `fleh_synchronous`（0x1c8943e4..0x1c8945bc） | 241 | 27674 |
| **EL0 同步向量入口（0x1c893400..0x1c893420）** | **12253** | 14929 |

## 2. 为什么这些数字自相矛盾

`0x1c893400` 处的指令是 `140003f9`，即 **`b 0x1c8943e4`** ——
也就是说，**只要执行了 EL0 同步向量入口，就必然跳到 `fleh_synchronous`**。

实测却显示：入口命中 12253 次，而目标 `fleh_synchronous` 只命中 241 次。

**唯一解释：`HV_PCHK` 的检查点在每个"新 block 进入"时执行，
而引擎的 chaining / superblock 会把连续块串成一条链，跳转若落在链内部就不经过检查点。**

因此：

- 热路径（被频繁调用、且被链接进块链）的函数，命中数会**严重偏低**；
- 冷路径（很少执行、每次都要新建块）的函数，命中数接近真实。

`pmap_enter_options`、`vm_fault`、`fleh_synchronous` 都属热路径，所以它们的 241/25/241
**远小于真实调用次数**。

## 3. 据此必须撤回的结论

| 曾据此得出的结论 | 状态 |
|---|---|
| "大部分 fault 没走到 `pmap_enter`" | **撤回**，计数不可靠 |
| "`vm_fault` 只被调用 25 次" | **撤回** |
| "只有 241 次异常真正进入 guest" | **撤回** |

**仍然成立**（不依赖 pchk 计数）：
- `pmap_enter_options` **确实被执行过**（这是存在性证明，不是计数问题）；
- 该函数所在的 link 地址 `0xfffffe001ca2206c..0x1ca233ac`，通过 `pmap_enter_options` 字符串
  `adrp/add` 交叉引用定位，可信；
- 这一轮 run 的 **slide = 0**（pchk 输出里 `slide 0`，且 `cpu->pc` 与 kernelcache 的 link VA 直接对应）。

## 4. 正确的计数方法

要统计某个内核函数的**真实调用次数**，不能用 `HV_PCHK`。可选：

1. **`HV_TRACE_PC`**：记录每个块的 run VA 到文件，离线统计落在函数范围内的比例。
   代价是文件很大（每块 8 字节），但它是**真实执行记录**，不受 chaining 影响。
2. **禁用 chaining**（`work/knobs.txt` 的 `HV_SUPERBLOCKS=0` 等），再跑 `HV_PCHK`，
   让每个块都经过检查点。代价是变慢。
3. **观察副作用**：例如看某个计数器/内存单元的变化，而不是看执行。

## 5. 对后续的影响

之前"pmap_enter 被调用 241 次 vs fault 17181 次"看起来像"绝大多数缺页没走到 pmap_enter"，
但那是假象。真实情况**未知**，需要重做。

**下一步应改用 `HV_TRACE_PC`**，它能给出两件确定的事：

- `pmap_enter_options` 范围内到底执行了多少块；
- 每次 fault 之后，执行流是否进入了 `pmap_enter_options`（时间上相邻）。
