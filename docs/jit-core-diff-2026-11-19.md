# JIT 与核心模块：上游怎么写，我们改了什么

基线：`origin/main` = **`e98920c`**（上游 Leviidev/Henge）。本文只对比**引擎本身**
（`core/` + `host/enter.S` + `host/jit_region.c`），不含恢复栈与设备模型
（那部分见 `docs/upstream-restore-semantics-2026-11-19.md`）。

**分级原则（本轮被纠正过一次，必须遵守）**：

> **差异 ≠ 上游有 bug。** iOS 与 Android 的 libc、编译器、信号/JIT 入口、SIGSEGV
> 处理都不同。只有**架构级证据**（ARM64 规范、ABI、QEMU 参照实现）才能判 D；
> 凡是依赖运行环境的，一律记 U 或"平台差异"。
>
> 更强的一条：**上游是已发布、能正常跑 macOS 安装的产品。** 任何"上游错"的结论
> 都必须同时回答 **"那它为什么能跑？"** 回答不了，就只能记"我们的环境需要偏离"。

行数（重测）：

| 文件 | 上游 | 我们 | 差 |
|---|---|---|---|
| `core/translate.c` | 2120 | 2244 | +124 |
| `core/dispatch.c` | 1145 | 3032 | +1887 |
| `core/memop.c` | 742 | 1137 | +395 |
| `core/exception.c` | 273 | 1398 | +1125 |
| `core/mmu.c` | 1292 | 1336 | +44 |
| `core/machine.c` | 653 | 736 | +83 |
| `core/machine.h` | 380 | 416 | +36 |
| `core/cpu.h` | 308 | 337 | +29 |
| `host/enter.S` | 434 | 460 | +26 |
| `host/jit_region.c` | 137 | 222 | +85 |

---

## 1. 上游 JIT 的写法

### 1.1 区域与块布局

- JIT 区域按 **1 MB chunk** 分配（`JIT_CHUNK_WORDS = 256K` words），因为 `CBZ`、`B.cond`、
  `LDR (literal)` 在 1 MB 内可达，块内分支不用重定位。
- 每个块分**两部分**：**hot**（每次都跑的翻译指令，从底部向上长）与 **cold**
  （未链接的退出、内存慢路径、间接跳转未命中、literal pool，从顶部向下长）。
  `HV_K2=1` 时合成一部分、按书写顺序。
- 上限：`MAX_WORDS 16384`、`MAX_LITS 2048`、`MAX_OOL 1024`、每条指令最多 96 words。
- 翻译先在**静态缓冲**里生成，最后一次性拷进 JIT 区域；块间分支在
  `hv_translate_place()` 时回填。

### 1.2 进入与退出

`host/enter.S`：

```
hv_enter(cpu, block):
  保存宿主的 callee-saved 寄存器
  x28 = cpu          ← HvCpu 基址，翻译代码全程用它
  恢复 guest x0..x30、sp、NZCV、q0..q31
  跳进块
块退出 → hv_exit：把 guest 寄存器写回 HvCpu，恢复宿主寄存器，ret
```

寄存器约定（上游）：

| 宿主寄存器 | 用途 |
|---|---|
| `x28` | `HvCpu *`（翻译代码的所有字段访问都基于它） |
| `x6`、`x7`、`x15` | **纯 scratch**（guest x6/x7/x15 存在 HvCpu 里，不在宿主寄存器） |
| `x18` | 保留（不可用） |
| 其余 | 按 `candidates[]` 分配给 guest 寄存器 |

`.hidden` 是**上游没有的**（见 §2.2）：`.hidden` 让符号地址成为链接期常量，否则取地址会走
GOT，而走 GOT 的 `adrp/ldr` 会用 `x16`——那正是 guest 的寄存器。

### 1.3 寄存器分配

```c
static const uint8_t candidates[] = {6,7,15, 9,10,11,12,13,14, 0,1,2,3,4,5,8,
                                     16,17, 19,20,21,22,23,24,25,26,27,29,30};
static int is_free(int reg)   { return reg == 6 || reg == 7 || reg == 15; }
static int is_stolen(int reg) { return reg == 18 || reg == 28 || is_free(reg); }
slot_of(reg) = reg == 31 ? HV_CPU_SP : HV_CPU_X(reg)
```

- 被选中的 guest 寄存器住在宿主同名寄存器里；没被选中的住在 `HvCpu` 的 slot。
- `spill`/`unspill` 在需要 scratch 时把占用者写回 slot。
- `SAVE_X2_X29` 按**寄存器编号**把宿主 `x16`、`x17` 直接存进 guest `x16`、`x17` 的 slot
  —— 这是 §2.2 D-1 的关键前提。

### 1.4 链接（chain / IJC / xjump / superblock）

- **slot table**：`m->slots[]`，每个槽对应"某个块的这个退出点"；`chain()` 把目标块的地址
  写进槽，退出点直接 `br` 过去，不再回 C。
- **间接跳转缓存（IJC）**：`IJC_BITS 18`，按目标地址哈希，命中就直接进块。
- **`xjump_thunk`**：跨块跳转的统一入口（查表 + 记录 `miss_site`）。
- **superblock**：条件分支在同页且块不太长时继续翻译下去（`go_on()`：`(pc+4)` 同页且
  `cur_i + 1 < 48`），减少 dispatcher 往返。

### 1.5 helper 与 TLB refill

翻译代码不直接调用 C：它写一个 `HvHelperInfo{pc, insn|op<<32}` 到 literal pool，
`blr` 到 `hv_helper_thunk`（在 `enter.S`）：

```
SAVE_X2_X29                 把 guest x2..x29 存进 HvCpu
str x30 / sp
x0 = cpu, x1 = info
bl hv_helper                （C）
x0 = [x28]                  helper 的返回值
恢复 guest 寄存器
```

内存慢路径多一层：`hv_tlb_refill` 可能补好 TLB 并让翻译代码**重试原指令**
（`rflags`/`retry` 编码在 op word 的 bit 18..8 与 31..19）。

### 1.6 SIMD / 浮点

- **guest 的 `v0-v31` 就是宿主的 `v0-v31`**：SIMD 指令原样执行，不翻译。
- `hv_enter`/`hv_exit` 用 `SAVE_FLAGS_FP`/`LOAD_FLAGS_FP` 保存/恢复 `q0-q31` + `NZCV`。
- `FPCR`/`FPSR` 不在 thunk 里读写（注释：读写会 stall 流水线），由
  `hv_fp_enter`/`hv_fp_leave` 在 `hv_run()` 期间一次性装进宿主寄存器。
- **上游的 `hv_helper_thunk`/`hv_ijc_thunk`/`hv_xjump_thunk` 只恢复 `NZCV`，不恢复 `q0-q31`**
  —— 见 §2.2 与 U-10。

### 1.7 内存路径

```
翻译指令 → TLB 快速检查（tag + addend）→ 命中：宿主 ldr/str
                                      → 未命中：cold 慢路径 → hv_tlb_refill → 重试
                                                        → 仍失败：hv_helper(HV_OP_MEM)
```

- **组（group）**：同一基址寄存器、地址相近的一串 load/store，第一次检查 TLB 并把页
  的宿主 addend 放进 `cpu->gaddend[slot]`，后续直接用；检查失败就整组退回逐条翻译
  （`plan_groups` / `emit_group_copies`）。
- **独占/原子**：`translate_excl` 用宿主 `casal`（值比较判成败）；`do_exclusive` /
  `do_casp` / `do_atomic` 处理慢路径。

### 1.8 JIT 区域

- iOS：`hv_jit_region_open()` 用 debugger 给的区域（一张可执行、一张可写），`vm_remap`
  把同一物理页映射两次。理由：TXM 设备上应用不能把新页变成可执行。
- Android（我们新增）：`memfd_create` + 两次 `mmap`（`MAP_SHARED`，一张 `R-X`、一张 `R-W`），
  同一 page cache，任何时刻都没有 W+X 映射；并按顺序探测哪种形状可用。

---

## 2. 我们相对上游的改动

### 2.1 `core/translate.c`（+124）

| # | 改动 | 内容 | 分级 |
|---|---|---|---|
| T-1 | `put_trace_entry()` 新增 | 每个块入口把 `{block VA, guest x0, guest x4, 宿主 x16}` 写进 `HvCpu.trace_ring`（仅在 `log >= 2` 时生成代码）。只允许碰 `x6`/`x15`，避免破坏 guest 寄存器（注释记录了早期版本因此让固件复位） | **E**（诊断） |
| T-2 | `put_branch_exit()` 加 `slots == NULL` 保护 | slot 表被 flush 释放后仍被写会空指针解引用；改为打印 + 停止链接 | **E**（健壮性，不改变正常路径） |
| T-3 | `put_helper()` 保存/恢复 `q0-q31` | helper 调用前后用 `STP_Q`/`LDP_Q` 存全部 32 个向量寄存器；`put_helper` 改为返回 `ldr x0` 的位置（供 refill 重入） | **U-10**（见 §2.2） |
| T-4 | `candidates[]` 去掉 `16, 17` | 见 §2.2 D-1 | **D-1** |
| T-5 | `slot_of()` 上移 | 纯位置调整（`put_trace_entry` 之前要用） | **E** |
| T-6 | `put_strip()` 的 `BFM` 改 `#16,#14`（两处） | 见 §2.2 D-2 | **D-2** |
| T-7 | `nosuper_va()` + `go_on()` | `HV_NOSUPERBLOCK_VA=lo:hi`：在这个范围不再跨条件分支继续组块（调试用）；32 位写法自动补 `0xfffffe00...` | **E**（默认关） |
| T-8 | `translate_counter()` 注释 | commpage 的 `0xfffffc090` 是**宿主地址**，Android 由 `compat_android.c` 建同样的映射；行为不变 | **E** |
| T-9 | `HV_INSN_WAIT` 注释 | 说明 `SEV`/`SEVL` 走 `HV_INSN_COPY` 原生执行，所以 `WFE` 能被唤醒 | **E** |
| T-10 | `translate_emit()` 调 `put_trace_entry()` | 见 T-1 | **E** |

### 2.2 两处架构级偏离——但都还欠"上游为什么能跑"的解释

| # | 上游 | 我们 | 为什么这是上游的问题（架构级） |
|---|---|---|---|
| **D-1 ⚠️** | `candidates[]` 含 `16, 17`，即宿主 `x16`/`x17` 可被借去保存别的 guest 寄存器 | 移除 16/17 | `SAVE_X2_X29` 按**编号**把宿主 `x16` 存进 guest `x16` 的 slot。若宿主 `x16` 正替别的 guest 寄存器打工，这个值就写进了 guest x16 的槽；下一个块从槽里读出的 guest x16 就变成了宿主地址。实测：固件拿到该地址后死亡。**机制与 OS 无关**，但**上游为何从未触发仍未解释**（可能 iOS 上寄存器压力/分配顺序从未选中 16/17，或污染后的地址恰好没被立即使用） |
| **D-2 ⚠️** | `put_strip()` 用 `BFM #16,#6`（清 bits[54:48]） | `BFM #16,#14`（清 bits[62:48]） | ARM 的 `XPAC`/`AUT` strip 语义是"签名字段整段变成 bit55 的符号扩展"；QEMU 的软件 PAC（`target/arm/tcg/pauth_helper.c: pauth_strip`）是唯一开源参照。只清后半段时签名上半残留，guest 往 `0xfffffe90...` 写，异常处理里再缺页，形成权限异常风暴。**机制是架构语义，但上游为何不触发仍未解释**（可能该 AUT 路径在 iOS 的恢复流程里走不到，或被别的行为掩盖） |

> 结论：这两条**严格说也只能证明"我们的环境必须偏离"**。要升级为完整的"上游错误"，
> 还需要解释上游产品为什么能跑（例如在 iOS 构建上复现，或给出它不触发该路径的证据）。

### 2.3 SIMD 保存：**不是 D，是 U-10**

事实：

- 上游的三个 thunk（helper/refill/ijc/xjump）只恢复 `NZCV`，不恢复 `q0-q31`；
  `hv_enter`/`hv_exit` 会存 `q0-q31`，所以"块与块之间"是安全的，**"helper 调用"这一段不安全**。
- 我们的 C helper 按 AAPCS 会破坏 `v0-v7`、`v16-v31`（以及 `v8-v15` 上半）。
- 在我们的构建上实测：iBEC 的 LZFSE 解码器把 FSE 状态放在 `q0-q2`，跨一个走慢路径的
  store 后状态丢失，`match distance` 取到垃圾，解出的 kernelcache 字节错误
  （`kalloc_type` 视图的 `kt_flags` 读成 `0x2` 而不是 `0x71`），随后 `validate_flags` panic。

**但结论只能是"我们的构建需要这段保存"**：上游用同一份源码在 iOS 上长期运行，说明它
依赖了我们不具备的前提（helper 编译产物不同 / 走不到这条慢路径 / 库与 ABI 差异 /
SIGSEGV 与 JIT 入口的处理不同）。**不能据此说上游有 bug。**

要升级为 D 需要：在 iOS/上游构建上复现同一现象，或者证明上游的 helper 同样会破坏
`v0-v31` 且该路径在 iOS 上也执行。

### 2.4 `host/enter.S`（+26）

| # | 改动 | 分级 |
|---|---|---|
| E-1 | 非 Apple 目标下用 `#define` 把 `_hv_*` 映射成 `hv_*`（Mach-O 下划线） | **E**（平台） |
| E-2 | 给 9 个入口加 `.hidden` | **E**（链接方式；见 §1.2，避免 GOT 用 x16） |
| U-3 | 三个 thunk 里 `ldr x0,[x28,#CPU_NZCV]; msr nzcv,x0` 换成 `SAVE_FLAGS_FP`/`LOAD_FLAGS_FP`（即连带 `q0-q31`） | **U-10**（同上） |

### 2.5 `core/dispatch.c`（+1887）

**语义项（默认都关，行为与上游一致）**：

| # | 内容 | 默认 | 分级 |
|---|---|---|---|
| D-1 | `kernel_fixups()`：chained-fixup rebase（`DYLD_CHAINED_PTR_64_KERNEL_CACHE`，跳过 `__DATA_CONST`） | **关**（`HV_KFIX=1` 开） | **E**（上游没有这段；默认关时等效） |
| D-2 | `derive_seed()` + `/chosen/random-seed` 填充 | **关**（`HENGE_RANDOM_SEED=1` 开） | **X**（上游不做；默认关时等效） |
| D-3 | `nochain_va()`（`HV_NOCHAIN_VA`，按页关链） | 关 | **E** |
| D-4 | `hv_flush_blocks` 的 slot 表保护（`slots` 缺失时不链接） | 总是 | **E**（防崩溃，正常路径不变） |
| D-5 | `hv_run`/`hv_enter` 外面加计时（`HV_BLOCKTIME`） | 关 | **E** |
| D-6 | `stop` 原子标志检查（`m->stop`） | 总是 | **E**（提前返回，行为不变） |

**诊断项（全部只读，或仅在 `log >= 2/3` 生效）**：

- `jit_fault` / `jit_fault_watch`：JIT 段 SIGSEGV 记录（总是安装 handler）
- `wait_site()`：第一次在某个 pc 上 WFE/WFI 时打印
- `host_name()` / `hv_looks_host()`：宿主地址按符号名打印
- `hv_helper` 的 helper 环（`log >= 2`）、x16/x4 变化（`log >= 3`）
- `print_stats` 扩展、`ktrace`、`fw_rec`、`pc_hist`、`trace_ring`、`hv_el0_trace`
- `run_loop` 里大量一次性 dump（device tree、boot_args、kernel.bin、栈扫描、panic 文本）
- `hv_el0_note()`：**每个 EL0 块一次记录**（`hv_el0_trace_va/x16`，16K 环）
- boot-args 注入：`HV_EXTRA_BOOT_ARGS`、`HV_BOOT_ARGS_VERBOSE`、`HV_DT_LEN_PATCH`（默认全关）

> `hv_el0_note` 是"总是开"的，但只写两个 `uint64_t`，不改 guest 语义；它记录的是
> "用户态最后在哪"，而不是控制流。

### 2.6 `core/memop.c`（+395）

全部是诊断：

- `watch_store()` 扩展：VA 粒度 → 页粒度 + 物理形式（`HV_WATCH_PA`，固件关 MMU 时用）
- `ptw_note()` / `hv_ptw_sample()`：页表写历史（`HV_PTW_LOG`/`HV_PTW_SNAP`）
- `hv_looks_host()`（定义在这里）
- 原子/cas 路径的额外打印

**没有改**：`hv_mem_decode`、`do_exclusive`、`do_atomic`、`do_casp`、`do_vector` 的语义。

### 2.7 `core/mmu.c`（+44）

- `hv_walk_dbg`（走表日志）
- `HV_PTW_LOG` 的采样
- **权限 fault 的诊断**（`perm fault pc/va/pa/L/desc/ap/ro/code_page`），只在 `log >= 2` 且最多 8 条

**语义未变**：`if (!ok) out->fsc = HV_FSC_PERMISSION(level);` 仍然是原来的判定，只是加了日志。

### 2.8 `core/machine.c` / `machine.h` / `cpu.h`

- `machine.h` 新增字段（全部只读/诊断）：`log_pc`、`ptw_*`、`halt_dumped`、`trap_dumped`、
  `kimage_pa/va`、`stop`、`watch_va/pa`、`slow_mem` 相关
- `cpu.h` **只在末尾追加**（遵守 AGENTS §7 的 ABI 规则）：
  `trace_idx`、`trace_ring[256*4]`、`helper_hist_idx/helper_hist[64]`、`exc_*`/`exc_ring[8]`
- `machine.c`：`m->chain = getenv("HV_NO_CHAIN") ? 0 : 1;`（默认仍是 1，与上游一致）、
  MMIO 直方图（诊断）

### 2.9 `core/exception.c`（+1125）

**语义扩展（X 类，上游没有）**：

- `vmapple_hvc()`：OEM hypercall（`0xc3000003` MABS / `0xc3000004` BootSessionUUID /
  `0xc3000005` WFK / `0xc3000006` KICK，以及 `0x8100ff01`/`0x8300ff01` UID、`ff03` REVISION、
  `feff` FEATURES）
- WFK 按 WFI 方式 park、KICK 唤醒目标 vCPU

**诊断**：`flt_class`、`fprobe_init`（`HV_FAULT_PROBE`）、`ring_has`、`hv_svc_dump_tail`
（含 `mach_msg2_trap` header/body 解码与 syscall 号分布）、`hv_fault_stats_dump`

上游对未知 FID 返回 `-1`（走降级路径），我们实现了 OEM 段 —— 这是 X，不是 D：XNU 的
`hvg_is_hcall_available` 会先探 `PAC_NOP`、再核对 UID/REVISION，任何一步失败就放弃全部
OEM 调用。

### 2.10 `host/jit_region.c`（+85）

Android 路径：`memfd_create` + 两次 `MAP_SHARED` 映射（`R-X` / `R-W`），并按顺序探测
可用形状；iOS 路径保持上游的 debugger 区域 + `vm_remap`。

分级：**E**（平台差异；guest 看到的是一个可执行区域，语义在两种实现下一致）。

---

## 3. 汇总

### 3.1 语义改动（E 类以外）

| 等级 | 项 | 说明 |
|---|---|---|
| **D-1 ⚠️** | `candidates[]` 去掉 16/17 | 机制是架构级；**上游为什么能跑仍待解释** |
| **D-2 ⚠️** | `put_strip()` `BFM #16,#14` | 机制是架构级；**上游为什么能跑仍待解释** |
| **X-1** | `exception.c` 的 OEM/WFK/KICK HVC | 上游没有，走降级；我们用 XNU 源码实现 |
| **X-2** | `dispatch.c` `/chosen/random-seed` 填充 | 上游不做；默认关 |
| **X-3** | `dispatch.c` `kernel_fixups()` | 上游不做；默认关，实测两种情形行为一致 |
| **U-10** | helper/thunk 保存 `q0-q31` | **我们的构建需要**，不能反推上游有 bug |
| **E** | 其余全部 | 诊断、平台适配、默认关闭的开关 |

### 3.2 与 `semantic-deviations.md` 的关系

那份文档是 E/X/D/U 总表，本文是 JIT/核心模块的**逐文件细节**。两处需要保持一致：

- `semantic-deviations.md` 的 D-3 已标注**降级为 U-10**（原先判成上游 bug，是错的）；
- 行数统计以本文 §0 的重测为准（旧统计写的是 `dispatch +1207` 等，已过时）。

### 3.3 未验证（U）

| # | 项 | 为什么未验证 |
|---|---|---|
| U-10 | helper/thunk 是否必须保存 `v0-v31` | 只能证明"我们的构建需要"；iOS 上游同一份源码长期正常 |
| U-11 | `kernel_fixups` 默认为关 | 只在 24G90 上对照过"开/关行为一致"，其他固件未验证 |
| U-12 | `hv_el0_note` 每块两次 store 的时序影响 | 数据量小，但没有做过"开/关"的对照跑 |
| U-13 | Android memfd 双映射在真机 SELinux 域下的可用性 | 运行期探测并记录，但没有覆盖所有 App 目标 SDK/SELinux 域 |

---

## 4. 一句话

上游的 JIT 是"**块 + hot/cold + slot 链 + IJC + helper thunk + TLB 快慢路径**"，
guest 的 SIMD 寄存器与宿主共用，`x28` 固定指向 `HvCpu`。我们**没有重写 JIT**，
改动集中在这几类：

1. **两处架构级修正**（D-1、D-2），与操作系统无关；
2. **一处我们的构建需要、但上游未必有问题**（U-10，SIMD 保存）；
   同理，D-1/D-2 也还欠"上游为什么能正常运行"的解释；
3. **一组默认关闭的调试开关与只读诊断**；
4. **平台适配**（Android memfd、符号名、日志）。

> 判定纪律：**iOS 与 Android 不一样，差异不等于上游 bug。**
> 而且**上游是已发布、能正常运行的产品**——所以判"上游错"必须同时解释它为什么能跑。
> 只有架构语义**加上**这个解释（或在对应平台上复现），才能把差异升级为 D。
