# 安装（DFU + restore）排查记录

现状：**安装未跑通**。本文只记录已经**实测排除**的假设、以及已经确认的事实，避免重复劳动。

## 一、流程走到哪一步

```
Macs → 安装 → 「下载并安装」
  → 镜像就绪（本地导入或从 Apple 下载）
  → BootRom.prepare：从 Apple 14.6.1 镜像取 860 KB，LZFSE+LZVN 解出 AVPBooter
  → 建板（installer 模式）→ 客户机 ROM 进入 DFU
  → usbmuxd/idevicerestore 枚举到 DFU 设备            ← 通了
  → TSS 签名（ApNonce）                              ← 通了
  → 发送 iBSS（243187 字节）                          ← 通了
  → 客户机执行 ROM 里的解压器，解 iBSS 载荷
  → panic：ARM synchronous abort at 0x12163c（ROM 内） ← 卡在这
```

崩点 `0x12163c` 在 **AVPBooter 自身**（ROM 基址 `0x100000`，长 `0x34738`），不是 iBSS。该块是一个位流/霍夫曼式解码循环：读 64 位字、按位偏移右移、查三张表（`x0+0x88` / `+0x288` / `+0x488`），即 ROM 在解压它刚收到的 iBSS 载荷。

## 二、已排除（均为实测）

| 假设 | 排除方式 |
|---|---|
| `AVPBooter` 是 IPSW 里的文件 | IPSW 里没有；它在系统镜像的一个 LZFSE 块里，按固定偏移 + 哈希取 |
| Apple CDN 证书链被 Android 拒绝 | `Tls.java` 放行 SHA-1 链（仅此一项），TSS 随即通过 |
| 前台服务被系统杀 | `DownloadService` / `InstallService` 均正常进前台 |
| 链式跳转（chained entry）导致寄存器错 | `g_machine.chain = 0` 后同样崩 |
| 控制传输把 iBSS 截断 | 加尺寸告警后，一次都没触发（无 >16 KiB 的控制 OUT） |
| 客户机 `x4` 是它自己算坏的 | `x4` 槽位全程是合理的位偏移（`0x21`/`0x23`），坏的是宿主寄存器 |
| `x0` 指向的结构被宿主指针污染 | 用 `el=0` dump，内容全是合法客户机地址 |
| 诊断代码引入了崩溃 | 加诊断之前的构建里同样崩 |
| AES / cfg / bdif 设备的寄存器语义 | 与 QEMU `hw/vmapple/{aes,cfg,bdif}.c` 逐字段核对，除 `REG_NEXT_DEVICE` 外一致 |

## 三、已确认的事实

- 出错指令是 `ldr x3, [x2]`，`x2` 已是非规范地址（`0xe000000f…`）。
- 崩溃瞬间 `x4`/`x16` 是**宿主地址**，且 `x4 = x16 + 0xC`（`.got.plt` 相邻槽位形态）。
- 用该 `x4` 代入 `sub x16, x16, x4` → `sub x2, x2, x16, lsr #3` 的算术，反推 `x2_before ≈ 0x7065cbfb`（完全正常的客户机地址），说明**就是它参与运算**。
- 该块自身不含任何写 `x4` 的指令（只 `add` / `sub x4, x4, x16`），`x16` 的来源都是 ≤32 位的表值，**算术上不可能**产生宿主地址 ⇒ 生成的代码路径有问题。
- `enter.S` 中 `_hv_helper_entry`、`_hv_helper_thunk`（refill 慢路径）、`_hv_ijc_thunk`、`_hv_xjump_thunk` **都完整保存/恢复了 x16/x17**（offset 128）。

## 四、可用的诊断开关

| 开关 | 作用 |
|---|---|
| `HV_LOG=0..3` | 安装默认 2：打印每次客户机异常 + 块环形记录；3 会打印每次 MMIO/mrs/msr（**会拖慢客户机到 DFU 枚举超时，勿用**） |
| 块环形记录 | `HvCpu.trace_ring[256]`，每块 `{块地址, x16槽位, x4宿主, x4槽位}`，由 JIT 在块开头写入（只用 x6/x15） |
| 数据异常摘要 | 每次 EL0 数据异常必打：`esr / far / pc / el` + `x0 x2 x4 x16` + 是否读到出错指令 |
| 压缩流扫描 | panic 时在前 64 MB RAM 找 `bvx2`/`bvxn`/`IM4P`/`IM4M`，并 dump `x2` 附近 |
| `HV_USB_CTRL_CHUNK=1` | 把超尺寸的控制 OUT 分片（默认只告警不分片） |
| `HV_CFG_POKE` / `HENGEVM_AES_LOG` | cfg 字段覆盖、AES 命令跟踪 |

## 五、尚未验证的方向（按可疑度）

1. **`x16` 是怎么变成宿主地址的**：它在 256 块窗口内恒定，说明污染发生在更早（ROM 初始化阶段）。下一步可把环形记录改成开机即启，或记录 `x16` 的写入点。
2. **bdif `REG_NEXT_DEVICE` 语义分歧**：QEMU 返回设备 id（ROOT `0x8000000` / AUX `0x10000`），我们返回容量扇区数。iBSS 会读它，值得做一次对照实验。
3. **ROM 拿到的 iBSS 载荷内容**：尺寸已排除，内容未验证。可在 DFU 写入点对载荷做哈希，与 `work/` 里的 iBSS 文件比对。
4. **AES 64 字节密钥分支**：若固件走该分支，我们的实现会静默不设密钥（QEMU 会保留 FIFO 等待）。加日志即可确认。

## 六、第一个问题：已修复（寄存器被写入宿主地址）

**症状**：iBSS 一发出，固件在 `0x12163c`（启动 ROM 的 iBSS 位流解码循环）panic，
`esr 0x92000004`、`far` 非规范、`x2` 由 `x16` 算出来，而 `x16` 是宿主地址。

**根因**：`x16` 是客户机寄存器，同时也是 AArch64 ABI 的跨库调用 scratch（IP0）。
引擎里 C 代码取汇编入口点地址（`cpu->helper_routine = (uint64_t)hv_helper_entry` 等）时，
由于这些符号是**默认可见性**，取地址要经过 GOT/PLT，而那条指令序列用 `x16` 当临时寄存器，
于是 `.got.plt` 的槽地址留在了宿主 `x16`；下一个保存点（`SAVE_X2_X29` / `hv_exit`）把它写进
客户机的权威槽位，固件随即拿它当位计数算出非规范地址。

**修复**（commit `27ab79c`）：
- `host/enter.S`：`_hv_enter/_hv_exit/_hv_helper_entry/_hv_helper_thunk/_hv_chain_thunk/_hv_ijc_thunk/_hv_xjump_thunk/_hv_fp_enter/_hv_fp_leave` 全部 `.hidden`
- `core/machine.c`：删掉 `helper_routine`（只写不读的死字段），它是唯一取 `hv_helper_entry` 地址的地方
- `tests/test_regs.c` + `android/test-regs.sh`：宿主侧回归（helper / 链式 / 跨页 / br / ret / SVC / data abort / 寄存器压力），35 项

**效果**：安装从「iBSS 立刻崩」推进到 **RestoreKernelCache 发完（约 70%）**。

## 七、第二个问题：内核早期停机（进行中）

**症状**：kernelcache 送完后客户机进入内核，随后永久停在 `wfe`：

```
beat pc fffffe001c89403c el1 ...        ← 每秒数百万元素空转
halt area: x0 fffffe001f2f3f50 x1 0 x2 94000000 ... elr fffffe001d177058 daif 3c0
```

**已定位**：
1. 停机点是内核异常向量的「数据异常 + `TPIDR_EL1 == 0`」分支（`1c894030: mrs x1, tpidr_el1; cbnz; wfe`）：
   极早期初始化里发生数据异常，线程尚未注册 ⇒ 判定不可恢复。
2. 故障指令：`1d177058: str x20, [x8]`，`x8` 来自全局槽 `0xfffffe001f1f88f0`，值为 `0x00c000000379cf90`（非规范）。
3. 该值就是**内核镜像 `__DATA` 里的原始常量**：运行期 RAM、页表解析、镜像 dump 三处一致。
4. 该槽位**不在** `LC_DYLD_CHAINED_FIXUPS` 的任何链里（`__DATA` 全段仅 54 处链，该页仅 `1f1f80e0`）；
   全 `__TEXT_EXEC` 扫描也**没有** `adrp+str` 写它。
5. 故障函数起点 `1d176aec`，开头读 `__PRELINK_TEXT` 基址，内部是早期 CPU 特性探测
   （`id_aa64pfr1_el1` / `id_aa64smfr0_el1` / `smcr_el1` / `smpri_el1` / `tpidr2_el0` / `rdsvl`）。
6. 串口在内核阶段**一行输出都没有**（`Darwin Kernel Version` 不在 RAM 里），说明它死在打印之前。
7. 与 CPU 数量无关（1 核同样停机），与 RAM 大小无关（2048 相同；1024 的实验被 App 崩溃干扰）。

**排除项**：镜像 fixup 缺失、ELF/段错位（镜像与运行时地址完全一致）。

**下一步候选**：
- 探针「该槽位是否被写过」当前仍受限于 `hv_mmu_walk` 在运行期对 49 位内核 VA 的判定（MMU 关闭时返回 `pa = va`，已加 RAM 区间过滤，但内核阶段解析仍未成功）。
- 若确认从未被写过 ⇒ 内核期望该槽位由**更早的装载/修补**填好 ⇒ 查 iBoot 侧对我们的机器参数是否走了不同分支。
- 备选：抽取 IPSW 内 `kernelcache.release.vma2` 原文与运行时镜像逐字节对比，确认 iBoot 究竟改动了哪些槽位。

## 八、App 侧修复（与模拟器无关）

| 问题 | 修复 |
|---|---|
| `Prefs.edit()` 递归调用自己 → `StackOverflowError`，任何设置改动都崩 | 改为 `sp(c).edit()` |
| `ForegroundServiceDidNotStartInTimeException`（`InstallService` / `DownloadService`） | 两个服务都在 `onCreate` 里先 `startForeground()` |

### 七点五、内核初始化轨迹与镜像对账（本轮新增事实）

**运行轨迹**（`ktrace.bin`，内核地址空间内按序记录，最多 8192 块）：

- 内核入口是 `0xfffffe001d176ae8`（`_start` / `arm_init` 区）
- 崩在前 **608 个块**内，全部在 `0x1d176ae8`–`0x1d1778xx` 这一个函数区域里，
  中间只调用了 `0x1d131cec`/`0x1d1322dc`/`0x1ca43xxx`/`0x1c890b10`（后者是 `memcpy`）
- **完全没有进入**平台初始化、设备树解析、kext/prelink、内存管理建立
- 崩溃前最后一步：`0x1d177010` 结构体初始化 → `memcpy` → `0x1d177048` 故障块
- 异常向量 `0x1c893000`/`0x1c894000` → `TPIDR_EL1==0` → `wfe` 停机

**镜像对账**（从 IPSW 内 `kernelcache.release.vma2` 解出原始镜像，方法与结果）：

1. zip 中央目录 → 条目 deflate，21,167,166 → 21,187,755 字节
2. IM4P 内 `bvx2`（LZFSE）→ 解出 75,546,624 字节的原始 Mach-O
3. 段表对比：运行时镜像**整体 relocate**，`slide = 0x14b14000`

```
__TEXT           orig 0xfffffe0007004000  run 0xfffffe001bb18000
__DATA           orig 0xfffffe000a6d4000  run 0xfffffe001f1e8000
```

4. 内容差异（按 8 字节）：

| 段 | 差异块 |
|---|---|
| `__TEXT_EXEC` | **0** / 5,011,456（代码完全一致） |
| `__DATA_CONST` | 220,737 / 851,968（大量 rebase，符合预期） |
| `__DATA` | 170 / 376,832（都是运行期写入的内核变量：`iBoot-11881.140.96`、`VMA2`、`MACOS`、`PE_state` 指针…） |

5. **我们的槽位（`0x1f1f88f0`）在原始镜像里就是 `0xc000000379cf90`**，且**不在任何 chained fixup 链里**
   （`__DATA` 全段仅 54 条链，该页只有 `…80e0 = 1` 一条）。原始镜像与运行时该处完全一致。

⇒ 结论：**它不是「装载器该修而没修」的指针**。内核是在**代码路径本身不该这么走**的情况下解引用了它。

由此，剩余假设集中在「我们的模拟在某条早期指令上产生了错误状态」，而不是机器参数或装载流程。
下一步的可选项：

- **A**：给引擎加一个**解释器模式**（不用 JIT），对同一段初始化做差分执行，找出行为不一致的指令 —— 这是定位此类 bug 的标准手段，工程量中等
- **B**：换一个 macOS 版本（内置 5 个）做对照：若老版本能走过这段，说明新内核依赖了我们缺的某个特性
- **C**：抽取带符号的 debug kernelcache（若有）以确认 `0x1d176ae8` 与 `0x1f1f88f0` 的真实符号名

### 七点六、结论（当前版本的收束）

**一句话**：内核**成功启动并执行到了**早期的 CPU 初始化；它在第 608 个基本块、
`0x1d177058` 处**取用一个链接期常量作为指针并解引用**，触发 EL1 数据异常；
异常处理发现 `TPIDR_EL1 == 0`（线程尚未注册），判定不可恢复，**进入 `wfe` 停机循环**。
主机侧因此等不到 restore 模式，30 秒后报 `Failed to place device in restore mode`。

**已经排除的（都有实测证据）**

| 曾经怀疑的 | 排除依据 |
|---|---|
| IPSW / iBSS / kernelcache 镜像损坏 | 16.1 GB 全条目 CRC32 通过；IPSW 内 kernelcache 解出后与运行时逐字节可对齐 |
| iBSS 载荷被截断 | 尺寸/内容已核对 |
| 模拟器寄存器被宿主指针污染（第一个问题） | 已定位并修复（commit `27ab79c`），安装因此从 iBSS 崩溃推进到 70% |
| 装载器该做而没做的指针修补（chained fixups） | 原始镜像与运行时该槽位一致；该槽**不在**任何 fixup 链里（`__DATA` 全段仅 54 条链） |
| 镜像段错位 / 导出映射错误 | `__TEXT_EXEC` 差异 0；页表解析、`boot_args` 换算、文件偏移三套算法得出同一地址 |
| 内核被错误地 slide | slide 恒为 `0x14b14000`，段表与 `__DATA` 写回都自洽 |
| `boot_args` 参数不对 | `physBase 0x70a04000` / `memSize 0x7eb38000` / 命令行 `… -progress -restore` 均自洽 |
| 设备 MMIO 与 RAM 重叠 | 全部设备在 `0x00400000`–`0x6FFF0000`，RAM 从 `0x70000000` 起，无重叠 |
| 多核相关（AP 启动路径） | 1 核与 4 核停机点完全相同 |
| RAM 大小 | 2048 MB 稳定复现同一点；1024 MB 的运行被 App ANR 干扰，不可用 |
| 我方返回了臆造的系统寄存器 | 崩溃前内核只读 `cntkctl_el1` / `actlr_el1` / `id_aa64pfr1_el1`，返回值 0 均为合法复位值 |
| 串口/panic 输出丢失 | 内核阶段串口**一行都没有**（`Darwin Kernel Version` 不在 RAM 中），它死在打印之前 |
| 内核走过平台初始化/DT/kext 后才崩 | `ktrace` 显示 608 个块全在一个函数区域内，**从未进入**平台初始化、DT 解析、kext/prelink、内存管理建立 |

**尚未排除的（也是唯一剩下的方向）**

内核在这段早期代码里期望某个**前置状态**（寄存器或内存）已经就绪，而实际没有。
要确认它，需要下列之一：

1. **带符号的内核**（Apple KDK / debug kernelcache）——把 `0x1d176ae8`（入口）与
   `0x1f1f88f0`（那个全局槽）反查成符号名即可定性
2. **参考实现对照**（能启动同一 IPSW 的 QEMU vmapple 构建）
3. 给引擎加**解释器模式**做 JIT/解释器差分执行（工程量最大）

**已知的调试工具限制**（避免后来者重复踩）

- `m->slow_mem`（`HV_SLOW_MEM`）只能**翻译期**生效：安装过程中运行时置位无效，
  固件阶段全局开启会把 iBSS 上传拖到超时。因此「记录内核全部写操作」这条路当前不可用，
  除非改翻译器的刷新逻辑。
- `hv_mmu_walk` 在 MMU 关闭时按约定返回 `pa = va`，做探针时必须过滤掉非 RAM 的结果。
- 每次启动内核的物理落点不同，任何按固定物理地址布设的探针都不可靠。

## 九、UniXNU / UniXNU-qemu 带来的两个线索

参考项目：<https://github.com/UniXNU/UniXNU>、<https://github.com/xVanTuring/UniXNU-qemu>。

1. **ADT（Apple Device Tree）的私有格式**（此前无法解析）：

   ```
   节点 = u32 属性数, u32 子节点数,
          属性数 × { char key[32]; u32 length; data[length] 补齐到 4 }
          子节点递归
   ```

   据此已能完整解析客户机里的设备树（`dt.bin` / 内核 dump 内 `PA 0x78324000`），
   确认以下内容都合理：

   | 项 | 值 |
   |---|---|
   | `chosen/dram-base` / `dram-size` | `0x70a00000` / `0x7f606000` |
   | `cpus/cpu0..3/timebase-frequency` | `24000000`（24 MHz ✓） |
   | `chosen/memory-map/DeviceTree` | `0x78324000`, size `0x87e38` |
   | `chosen/memory-map/RAMDisk` | `0x783ac000`, size `0xb400000` |
   | `chosen/memory-map/TrustCache` | `0x73b14000`, 4 MB |
   | `chosen/memory-map/BootKC-*` | `-1`（无 boot kernel collection，正常） |

2. **`DARWIN_BOOT_ARGS` 字段布局**（来自 UniXNU 文档）：

   | 偏移 | 字段 |
   |---|---|
   | 0x08 / 0x10 / 0x18 / 0x20 | virtBase / physBase / memSize / topOfKernelData |
   | 0x28 | video（6 × u64） |
   | **0x60** | device tree address |
   | **0x68** | device tree length |
   | 0x6c | command line（1024 字节） |
   | 0x478 | memory size actual |

   **发现的差异**：`device tree length` 报 `0x87604`（554,500），而实际树长 `0x87e38`（556,600），
   尾部 2,100 字节（`product/usb-device`、`product/util`、`filesystems-props`、`port-virtual-1`）
   落在长度之外 —— 疑似 iBEC 追加节点后未更新长度。

   **该假设尚未验证**：尝试在内核启动前把该字段改成 16 MB 的实验探针**未能触发**
   （`hv_helper` 在内核阶段确实被调用 —— 有既有探针输出为证 —— 但新增代码的打印始终不出现，
   原因未查明）。实验代码已撤除，只保留结论与待办。

**结论**：第二个问题仍停在原处，需要**带符号的内核**或**参考实现**才能继续；
ADT 格式与 `boot_args` 布局这两个工具性问题已经解决，后续可直接复用于其它调试。

## 十、第二次定位（**已作废，见第十一节**）

> 本节结论是错的，保留作为记录：错因是把「内核以物理地址 identity 运行的开机代码」
> 误当成了 iBEC，并在此基础上有 0x1000 的地址算错。以第十一节为准。

## 十一、真正的状态：内核入口是对的，卡在 chained fixup

### 修正：`0x7489xxxx` 不是 iBEC，是内核自己的开机代码

把运行中的 RAM（`k80.bin`，基址 `0x73b18000`）与**内核镜像逐字节比对**：

| 地址 | 结论 |
|---|---|
| PA `0x74898000` | 与内核镜像文件偏移 `0xd80000`（`__TEXT_EXEC + 0x8000`）**完全相同** |

而 `LC_UNIXTHREAD` 的 pc = `0xfffffe0007d82488` → 第一条指令是 `b 0xfffffe0007d8405c`
（`__TEXT_EXEC + 0x805c`），正是我们看到的 `0x7489805c`（同一个位置，物理地址形式）。

**所以整条链是正常的内核启动路径**：

```
_start (0x7d82488) → b → start_first_cpu (__TEXT_EXEC+0x805c，此时 MMU 前以 PA identity 运行)
  → 读 boot_args 的 virtBase/physBase，把下一跳从 PA 换算成 VA
  → 跳到 arm_init 的 VA 0xfffffe001d176ae8（= 镜像 __TEXT+0x165EAE8，合法函数序言 pacibsp/sub sp）
```

- 固件里那段 `adrp x30, <页>; add x30,#0xae8; add x30,x22; sub x30,x23` 是**内核自己的**
  PA→VA 重定位，不是 iBEC 的硬编码常量（因为 `adrp` 的立即数就是内核镜像里的字节，
  已核对一致）。
- `0x1d176ae8` 处字节 = `7f2303d5 ffc302d1 f85f07a9 f65708a9` = `pacibsp / sub sp,sp,#0xb0 /
  stp x24,x23 / stp x22,x21`，**栈是正常建立的**。昨天的"b.eq / 跳过栈调整"是算错地址。

**结论：入口、栈、重定位都没有问题，这部分全部作废。**

### 新的（也是真正要查的）卡点：一个没有被修复的 chained fixup 槽

崩溃点 `0x1d177058`：`ldr x8,[全局槽]; str x20,[x8]`，其中

- 全局槽链接期 VA = `0xa6e48f0`（运行时 `0x1f1f88f0`）
- 该槽在**原始内核镜像**（文件偏移 `0x36e08f0`）里就是 `0xc000000379cf90`
- 崩溃时读出来**仍然是** `0xc000000379cf90` —— 也就是说：

> 这个槽里存的是 **chained fixup 编码值（bit63 置位 = authenticated pointer）**，
> 而内核真正用它之前，本应有一次 fixup 把编码值解成合法指针。这次 fixup **没有覆盖它**。

`0xc000000379cf90` 的低 48 位是 `0x379cf90`，不是内核 VA，所以它确实还没被解码；
`str x20,[x8]` 于是写到非法地址，数据异常（EC 0x25）+ `TPIDR_EL1==0` ⇒ 内核判定不可恢复。

### 下一步（新的，两条）

1. **确认 fixup 该由谁做、有没有跑**：对比运行时 `__DATA`/`__DATA_CONST` 与镜像，
   看 fixup 覆盖了哪些槽、漏了哪些。已知：`__DATA_CONST` 有 220 KB 差异（fixup 跑过），
   `__DATA` 只有 170 字节差异 —— 差异分布本身就是线索。
2. **盯这个槽的写入**：解析 `0xa6e48f0` 的 PA，从内核第一个块起监视其写入；
   若从未写过，说明 fixup 流程压根没处理该区域。

### 附带：一个可疑的实现选择

引擎把 PAC 的签名/验签实现为**空操作**，并在 `AUT*`/`XPAC*` 时用
`translate.c:put_strip()` 把 bit 54..48 清零（"客户机指针从不签名"）。
但 chained fixup 的 authenticated pointer 恰恰把**链信息存在高位**（`ubfx #51,#11`、
`tbnz #62/#63`）。如果 fixup 循环里出现 `aut*`，`put_strip` 会破坏这些高位 ——
这与"槽没有被解码"的现象吻合，值得优先验证。
