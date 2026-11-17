# vm_map（"档案馆"）的结构：源码是公开的

## 1. 先回答"开放还是封闭"

**公开。** XNU 的 VM 子系统在 Apple 开源仓库里（`apple-oss-distributions/xnu`）。
`struct _vm_map` 的定义在新版中从 `osfmk/vm/vm_map.h` 挪到了：

| 文件 | 内容 |
|---|---|
| `osfmk/vm/vm_map_xnu.h` | `struct _vm_map` 本体（标为 XNU-only export，但**仍在开源发行版里**） |
| `osfmk/vm/vm_map_store_internal.h` | `struct vm_map_header`、`struct vm_map_links` |
| `osfmk/vm/vm_map.h` | 公开 API 声明与文档注释，不含结构体本体 |

这解释了为什么本地 `/tmp/xnu` 的 `vm_map.h`（569 行）里找不到结构 —— 那是较旧/裁剪的版本，
结构在新版被移到了上面两个文件。

## 2. 结构长什么样

```c
struct vm_map_links {
    struct vm_map_entry *prev;    /* +0x00 */
    struct vm_map_entry *next;    /* +0x08 */
    vm_map_offset_t      start;   /* +0x10 */
    vm_map_offset_t      end;     /* +0x18 */
};

struct vm_map_header {
    struct vm_map_links  links;          /* +0x00 .. +0x20 */
    int                  nentries;       /* +0x20 */
    uint16_t             page_shift;     /* +0x24  ← 16 KiB 页时值为 14 */
    uint16_t             entries_pageable : 1,
                         __padding        : 15;   /* +0x26 */
#ifdef VM_MAP_STORE_USE_RB
    struct rb_head       rb_head_store;  /* +0x28（8 字节） */
#endif
};

struct _vm_map {
    lck_rw_t             lock;        /* +0x00，arm64 上 16 字节（源码注释：arm 8 / arm64 16 / x86 16） */
    struct vm_map_header hdr;         /* +0x10 */
#define min_offset  hdr.links.start    /* 地址空间下界 */
#define max_offset  hdr.links.end      /* 地址空间上界 */
    pmap_t               pmap;        /* 物理映射 */
    vm_map_size_t        size;
    uint64_t             size_limit;
    uint64_t             data_limit;
    vm_map_size_t        user_wire_limit;
    vm_map_size_t        user_wire_size;
    os_ref_atomic_t      map_refcnt;
    /* CONFIG_MAP_RANGES: default_range / data_range / large_file_range / extra_ranges */
    union { vm_map_offset_t vmu1_highest_entry_end; vm_map_offset_t vmu1_lowest_unnestable_start; } vmu1;
    vm_map_entry_t       hint;
    ...                  /* 若干 union、位域、timestamp、owner 等 */
};
```

按此推算各字段偏移：

| 字段 | 偏移（启用 RB 时） |
|---|---|
| `lock` | +0x00（16 B） |
| `hdr.links.prev` | +0x10 |
| `hdr.links.next` | +0x18 |
| **`hdr.links.start` = min_offset** | **+0x20** |
| **`hdr.links.end` = max_offset** | **+0x28** |
| `hdr.nentries` | +0x30 |
| **`hdr.page_shift`** | **+0x34** |
| `hdr.rb_head_store` | +0x38 |
| **`pmap`** | **+0x50**（未启用 RB 时为 +0x38） |

## 3. 这解释了之前所有找错的地方

之前一直按 `{prev,next,start,end}` 的形状去找 vm_map，结果找到的是 **vm_map_entry**，
因为两者的 `links` 形状相同。区别在 `start`/`end` 的含义：

| | start | end |
|---|---|---|
| **vm_map_entry** | 某个具体区间起点，如 `0x100fc0000` | 区间终点，如 `0x100fc4000` |
| **vm_map** | **地址空间下界**（通常 `0` 或 `0x1000`） | **地址空间上界**（arm64 macOS 约 `0x7ffffe000000`） |

所以正确的判据是：**start 很小、end 接近地址空间上界**，而不是"两个相邻地址"。

## 4. 这次扫描仍未命中

按 `page_shift == 14`（16 KiB 页的强特征）+ `pmap` 指向内核堆试扫：

```
pmap offset 0x50 : 0 candidate(s)
pmap offset 0x38 : 0 candidate(s)
（其余偏移同样为 0）
```

可能原因（按可能性排序）：

1. `CONFIG_MAP_RANGES` / `VM_MAP_STORE_USE_RB` 的编译开关与开源默认不同，导致 `pmap` 偏移不是 0x50；
2. `hdr.page_shift` 在该版本中未必被写入 14（可能保持 0，由别处提供页大小）；
3. 该 vm_map 结构所在内存页当前未被我们扫到（进程已退出、页被回收）。

## 5. 下一步（有了结构定义之后）

1. **不依赖 `page_shift`**，改用 `{start≈0, end≈0x7ffffe000000}` 这个更强的边界特征定位 vm_map，
   再对每个候选遍历多个 `pmap` 偏移，检查该指针是否落在已定位的 pmap 区域
   `0xfffffe1e55f5xxxx` 内 —— 这是唯一能自校验的判据；
2. 或者用 `__PRELINK` 里的 `vm_map_create` 一类函数反汇编读出 `pmap` 的真实偏移
   （开源源码给的是默认假设，实际编译产物可能因配置不同而不同）。

**结论**：结构是公开的、布局已知，剩下的只是"这台机器上实际编译出来的偏移"需要实测确认。
