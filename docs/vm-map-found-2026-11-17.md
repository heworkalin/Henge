# 找到了：4 个 vm_map，pmap 偏移确认为 +0x40

## 1. 扫描设计

一次扫描同时检验两个假设（用户要求"两个条件同时跑，并说明是哪个成立"）：

- **条件一**：`pmap` 在 `vm_map+0x40`（`VM_MAP_STORE_USE_RB` 启用）
- **条件二**：`pmap` 在 `vm_map+0x38`（未启用）

基础特征只依赖**数据字段**，不依赖任何指针（因此不受 PAC 签名影响）：

```
byte[+0x03] == 0x33        lck_rw_type == LCK_TYPE_RW（locks_internal.h:154）
u32 [+0x04] == 0           lck_rw_padding
u64 [+0x20] <  0x1000000000  min_offset
u64 [+0x28] >  0x1000000000  max_offset
start < end
u32 [+0x30] <  0x100000     nentries
```

再用 `strip_pac(pmap) = (p & 0xffffffffffff) | 0xffff000000000000` 是否落在已独立定位的
pmap 区域 `0xfffffe1e55f00000..0xfffffe1e56000000` 做二次校验。

## 2. 结果

```
base signature hits : 761
CONDITION 1 (pmap at +0x40) : 4      ← 成立
CONDITION 2 (pmap at +0x38) : 0      ← 不成立
```

**所以这台机器的内核启用了 `VM_MAP_STORE_USE_RB`，`pmap` 位于 `+0x40`。**

## 3. 四个 vm_map 的实测内容

```
@pa=0x83fb8220  min=0x100c84000  max=0x7ffffe000000  nentries=1092  page_shift=14  pmap=0xfffffe1e55f54170
@pa=0x83fb8590  min=0x104a0c000  max=0x7ffffe000000  nentries=1142  page_shift=14  pmap=0xfffffe1e55f54430
@pa=0x83fbae20  min=0x102200000  max=0x7ffffe000000  nentries=155   page_shift=14  pmap=0xfffffe1e55f567f0
@pa=0x83fbb2f0  min=0x104200000  max=0x7ffffe000000  nentries=1133  page_shift=14  pmap=0xfffffe1e55f56ab0
```

以第一个为例的完整字段：

```
+0x00 0000000033000000   lock: lck_rw_type = 0x33 (LCK_TYPE_RW)
+0x08 0000000000420000   lock data
+0x10 fffffe11260be700   hdr.links.prev
+0x18 fffffe11260be520   hdr.links.next
+0x20 0000000100c84000   min_offset
+0x28 00007ffffe000000   max_offset
+0x30 0001000e00000444   nentries=0x444(1092) | page_shift=0x0e(14)
+0x38 fffffe11260b46a0   rb_head_store.rbh_root
+0x40 fffffe1e55f54170   pmap          ← 条件一命中处
+0x48 00000065d2e70000   size
+0x50 7fffffffffffffff   size_limit
+0x58 7fffffffffffffff   data_limit
+0x60 7fffffffffffffff   user_wire_limit
+0x68 0000000000000000   user_wire_size
+0x70 0000000000000001   map_refcnt
```

## 4. 交叉验证（三条独立证据）

1. `max_offset = 0x7ffffe000000` —— 正是 arm64 macOS 的用户空间上界，
   与 `vm_map_xnu.h` 里 `#define max_offset hdr.links.end` 一致；
2. `page_shift = 14` —— 与 TCR.TG0=2（16 KiB）一致；
3. `pmap` 落在 `0xfffffe1e55f5xxxx` —— 与先前**独立**通过 `ttb0 → 唯一物理页 → 页表反查`
   得到的 pmap 区域完全吻合。

第 3 条尤其强：那条路径完全没用到 `vm_map` 结构，两条独立路径互相印证。

顺带修正一个小错误：之前从 ttb0 反推的 pmap 起点 `0xfffffe1e55f56ab8` 偏了 8 字节，
真正起点是 `0xfffffe1e55f56ab0`（ttb0 字段在 pmap 结构内 `+0x08`，不是 `+0x00`）。

## 5. 下一步（现在才算真的能问"有没有登记"）

对每个 `vm_map`：

1. 从 `+0x10`（`hdr.links.prev`）/ `+0x18`（`next`）沿链遍历 `vm_map_entry`；
2. 对每个 entry 读 `links.start` / `links.end`，找覆盖卡住页（如 `0x100fc0000`）的那一个；
3. 读该 entry 的 `protection` / `flags` / `vme_object`，判断：
   - entry **不存在** ⇒ 程序访问了它没权访问的地址（guest 自身问题）；
   - entry 存在但 `protection = NONE` ⇒ 这是 guard/保留区，缺页本就该失败；
   - entry 存在且 `protection` 正常 ⇒ 是 page-in 没完成（要查 pager/object）。

这就是"档案馆到底有没有登记"的答案，而且现在判据是可靠的。
