# LC_FUNCTION_STARTS 函数索引（2026-11-17）

## 1. 为什么做这个

调查已经收敛到"全系统停在 `WQT_QUEUE` 上等一个不来的 `wakeup()`"，
而要继续必须能**在内核主代码里下断点**。但：

| 能力 | 状态 |
|---|---|
| JIT 里下断点（`HV_PCHK`） | 有 |
| 下断点需要目标地址 | 需要 |
| kprobe / eBPF | **没有** |
| `ksym.py` 覆盖 kext | 有（181 781 符号） |
| `ksym.py` 覆盖内核主代码 | **没有** |

按名字反查 `msleep`/`wakeup`/`thread_wakeup`/`assert_wait`：**0 匹配**，
连符号名里含 `sleep` 或 `wakeup` 的都是 0 个 —— 因为这些函数在内核自身
（`com.apple.kernel`）的 `__TEXT_EXEC`，而 `ksym.py` 文件头就写明内核自身的代码
在这张表里没有符号。

`LC_FUNCTION_STARTS` 就是补这个缺口的：它不提供名字，但提供**函数起始地址**，
足以让 `HV_PCHK` 有的放矢。

## 2. 工具

`tools/lcfs.py`

```
tools/lcfs.py /tmp/kc72.bin --json work/kernel_functions.json --csv work/kernel_functions.csv
tools/lcfs.py /tmp/kc72.bin --symbol 0xfffffe001c890000
tools/lcfs.py /tmp/kc72.bin --only com.apple.kernel     # 只索引内核本体
```

输出每行：`image, start_va, end_va, size, start_offset, confidence`

**`end_va` 不是真实函数长度**：Mach-O 只记录起点，所以 `end_va` 取下一个起点
（最后一个取可执行段末尾），`confidence` 用 `high`/`low` 标出来。
**这些地址不是符号名，不得当作符号名使用。**

## 3. 四个陷阱（每个都会白跑一轮）

1. **MH_FILESET**：顶层 Mach-O 不是 image。每个 image 是一条
   `LC_FILESET_ENTRY`（cmd `0x80000035`），其 `entry_id` 是字符串偏移，
   **相对命令起始**，不是相对后面那 4 字节之后。按后者解析得到
   `"@"`、`"tform"`、`"orDriver"` 这种垃圾名。
2. **`_PrelinkExecutableLoadAddr` 会被 plist 读成负数**，必须
   `& 0xffffffffffffffff`（`ksym.py` 里的 `u64()` 就是干这个的）。
3. **`0x7fffffffffffffff` 是哨兵值**，表示"该 kext 没有可执行镜像"。
   17 个条目是它，当成合法 VA 会得到负的 file offset（本次实测 19 个
   "bad header" 就是它，过滤后为 0）。
4. **`dataoff`/`datasize` 是相对整个 kernelcache 的偏移**（和
   `symoff`/`stroff` 一样），不是相对 image。
5. **delta 相对 image 的 `__TEXT` vmaddr，且跨所有可执行段**
   （`__TEXT`、`__TEXT_EXEC`、`__KLD`）。内核第一个 delta 是 `0xd70000`，
   比 `__TEXT` 的 `0xf0000` 大得多 —— 它正好等于
   `__TEXT_EXEC(0xfffffe001c890000) − __TEXT(0xfffffe001bb20000)`。
   **不要按 `__TEXT` 的大小去截断。**

## 4. 实测统计（`/tmp/kc72.bin`，72 MB）

```
images with index        : 220
functions total          : 149938
bad header               : 0
no __TEXT                : 0
no exec segment          : 0
no LC_FUNCTION_STARTS    : 0
truncated data           : 0
ULEB128 errors           : 0
non-monotonic images     : 0
images with duplicates   : 0
starts outside exec segs : 0
```

最大的几个：

```
com.apple.kernel                          20929 functions  base=0xfffffe001bb20000
com.apple.iokit.IO80211Family             12199
com.apple.driver.AppleM2ScalerCSCDriver    7900
com.apple.iokit.IOAccessoryManager         4823
com.apple.driver.AppleBCMWLANCore          4259
```

## 5. 验证（阶段 2）

| 检查 | 结果 |
|---|---|
| ULEB128 是否完整解析 | 0 错误，无截断 |
| 函数起始是否落在可执行区域 | 0 个越界（220/220 image） |
| 地址是否单调递增 | 0 个非单调 |
| 越界 / 异常重复 | 0 个重复 |
| 抽样反汇编 | 6 个随机函数中 4 个 `pacibsp` 开头 + `ret`/`retab` 结尾；2 个 tail-call/循环风格，合理 |
| 已知地址核对 | `0xfffffe001c890000` → `com.apple.kernel + 0x0`，正是 `__TEXT_EXEC` 起点 |

## 6. 下一步（阶段 3）

有了函数索引，下一步不是"猜 msleep 在哪"，而是：

1. **先确认内核里是否真有 `msleep`/`wakeup` 这一族**（可能这个版本用
   `assert_wait` + `thread_block` + `waitq_wakeup64_*`）；
2. 用 `__LINKEDIT` 的字符串 + `adrp/add` 引用反查出名字与函数地址的对应
   （`msleep` 的 panic/断言字符串是很好的锚点）；
3. 拿到地址后再在 JIT 里下 `HV_PCHK`，做 T1（登记 wait_event）/ T2（wakeup 尝试）
   / T3（出队）的事件级对照。

**索引本身是纯离线产物**：`work/kernel_functions.json`（25 MB）和 `.csv`（13 MB）
不入库，需要时用 `tools/lcfs.py` 重新生成（几秒）。
