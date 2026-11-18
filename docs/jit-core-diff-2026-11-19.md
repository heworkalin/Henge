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

### 2.1.5 寄存器使用约定：Darwin 与 bionic 的 ABI 对照（2026-11-19 查证）

用户提出的假设是"可能只是 iOS 在寄存器分配上没有限制，所以上游能正常使用"。查证结论：
**两个平台的规定完全相同，差异不在这里。**

| 寄存器 | AAPCS64 角色 | Darwin (iOS) | Android (bionic) |
|---|---|---|---|
| x0–x7 | 参数/返回值 | 同 | 同 |
| x8 | 间接结果位置 | 同 | 同 |
| x9–x15 | 临时（caller-saved） | 同 | 同 |
| **x16/x17** | **IP0/IP1；链接器可在 PLT/veneer 中使用** | 同 | 同 |
| **x18** | **平台保留** | thread state（编译器不用） | **ShadowCallStack（NDK：不要碰）** |
| x19–x28 | callee-saved | 同 | 同 |
| x29/x30 | FP / LR | 同 | 同 |

来源：ARM AAPCS64 原文（"R16, R17 — IP0, IP1: intra-procedure-call temporary registers.
The linker may use these in PLT code"）、Android NDK「Android ABIs」（x18 保留给
ShadowCallStack）、Apple「Writing ARM64 code for Apple platforms」（x18 平台保留）。

**链接器实际怎么用 x16/x17**（这就是两侧的差别所在，不是"允许不允许"）：

```
ELF (glibc/LLD):  adrp x16, … ; ldr x17, [x16, …] ; add x16, x16, … ; br x17
Mach-O (Apple):   adrp x16, … ; ldr x16, [x16, …] ; br x16
```

**两者都占用 x16**（ELF 还用 x17）。所以真正的变量是**链接方式与符号可见性**：
一个自家符号是否需要经 GOT 取地址。我们代码里正有一条修复对着这里（`enter.S` 的
`.hidden`，注释原文：*"That is how a .got.plt address turned up in a guest's x16 and the
firmware died on it"*）。它把地址变成链接期常量，不再走 GOT。

（该候选解释已被 §2.1.6 的实测推翻：我们自己的 ELF 构建**也**经 GOT 取地址，
只是用 x0 且在初始化阶段，所以"经不经 GOT"不能解释 guest x16 的污染。）

对上游/我们的实际结论：

- `x18` 必须排除 —— 两边都保留，没有争议；
- `x16/x17` 不能借给别的 guest 寄存器 —— 这是**引擎自身的保存逻辑**（`SAVE_X2_X29`
  按编号存），与操作系统无关；
- 但**"借用后是否立即出事"**取决于宿主代码在这期间是否用了 x16/x17（PLT/GOT 就会），
  这就是平台/链接差异能放大的地方。

### 2.2 两处架构级偏离——但都还欠"上游为什么能跑"的解释

| # | 上游 | 我们 | 为什么这是上游的问题（架构级） |
|---|---|---|---|
| **D-1 ⚠️** | `candidates[]` 含 `16, 17`，即宿主 `x16`/`x17` 可被借去保存别的 guest 寄存器 | 移除 16/17 | `SAVE_X2_X29` 按**编号**把宿主 `x16` 存进 guest `x16` 的 slot。若宿主 `x16` 正替别的 guest 寄存器打工，这个值就写进了 guest x16 的槽；下一个块从槽里读出的 guest x16 就变成了宿主地址。实测：固件拿到该地址后死亡。**机制与 OS 无关**，但**上游为何从未触发仍未解释**（可能 iOS 上寄存器压力/分配顺序从未选中 16/17，或污染后的地址恰好没被立即使用） |
| **D-2 ⚠️** | `put_strip()` 用 `BFM #16,#6`（清 bits[54:48]） | `BFM #16,#14`（清 bits[62:48]） | ARM 的 `XPAC`/`AUT` strip 语义是"签名字段整段变成 bit55 的符号扩展"；QEMU 的软件 PAC（`target/arm/tcg/pauth_helper.c: pauth_strip`）是唯一开源参照。只清后半段时签名上半残留，guest 往 `0xfffffe90...` 写，异常处理里再缺页，形成权限异常风暴。**机制是架构语义，但上游为何不触发仍未解释**（可能该 AUT 路径在 iOS 的恢复流程里走不到，或被别的行为掩盖） |

> 结论：这两条**严格说也只能证明"我们的环境必须偏离"**。要升级为完整的"上游错误"，
> 还需要解释上游产品为什么能跑。§2.1.6 的实测已经排除"经不经 GOT"这个解释；
> D-1 与 D-2 目前**都没有解释**，保持未解。

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
| E-2 | 给 9 个入口加 `.hidden` | **E**（不导出、不可抢占；实测未消除 GOT 访问，见 §2.1.6） |
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
| U-12 | `hv_el0_note` 每个 EL0 块约 17 次 store 的时序影响 | 13 个寄存器快照 + 2 个环 + 计数（代码注释 "Five stores" 已过时）；没有"开/关"对照跑，无法判断对恢复时序的影响 |
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

---

# 5. 当前代码的完整 JIT 路径（端到端，不是 diff）

本节按**我们改动后的代码**重走一遍：从宿主进入 `hv_run` 到块执行、helper 调用、退出。
目的是把"改了哪里"放回完整上下文里，而不是对着一堆 diff 猜。

## 5.1 进入执行

```
hv_run(cpu)                              core/dispatch.c
  hv_big_lock(m)                         拿机器大锁（多 CPU 串行进入）
  hv_fp_enter(cpu)                       把 HvCpu.FPCR/FPSR 装进宿主寄存器
  r = run_loop(cpu, 0)
  hv_fp_leave(cpu)                       FPCR/FPSR 写回
  hv_big_unlock(m)
```

`run_loop` 每一轮（`core/dispatch.c:1346` 起）：

```
for (;;) {
    if (m->stop) return HALT;                       ← 我们新增（D-6）
    if (flush_pending || stw_active) { stop_the_world(); continue; }
    hv_cpu_process_requests(cpu)
    if (halted) return;  if (off) { wait_while_off(); continue; }
    kicked = exchange(cpu->exit_req, 0)             ← 1 ms kicker / 设备中断
    if (kicked) { cpu->excl_addr = ~0; hv_big_yield(m); }
    if (tick && (kicked || 每 64 块 || 定时器到期)) tick(m, cpu)
    why = ...; hv_check_interrupts(cpu)
    if (cpu->pc == stop_pc) { stop_hook; pending_chain = NULL; }

    code = hv_mmu_fetch(cpu, cpu->pc, &pa, &avail)   ← 取 guest 指令；失败=已取 abort
    b = lookup(cpu, cpu->pc, pa, block_flags(cpu), code, avail)
    ...
    if (pending_chain && b->va == chain_pc && b->flags == chain_flags) chain(m, pending_chain, b);
    hv_el0_note(cpu)                                 ← 我们新增（EL0 记录，见 §5.7 注）
    ... (log>=2 的诊断)
    hv_enter(cpu, b->code)                           ← 进入翻译代码
    ... (退出分类；可链则记 pending_chain)
}
```

## 5.2 lookup：块缓存与翻译

```
lookup(va, pa, flags, code, avail):
  ① 每 CPU 的直接缓存 jc[]（va 命中且 pa/flags/gen 一致）→ 直接返回
  ② 全局 hash find()
  ③ 未命中：
     - 若该页第一次当代码：置 ram->code_pages[pg]=1，并给所有 CPU 的 TLB
       hv_tlb_protect_code_page()（之后对该页的写走慢路径，能发现自修改）
     - hv_translate_emit(m, &nb, code, avail)       ← 在**本线程的静态 Emit E** 里生成
     - hv_translate_place(m, &nb)                   ← 回填分支、写进 JIT 区域
     - 加 tr_mu 锁发布到 hash；若别的 CPU 抢先做了，丢弃自己的
```

`hv_translate_emit`（`core/translate.c:2077`）做的第一件事（我们加的）是
`put_trace_entry(e, b->va)`（仅 `log>=2` 时生成指令），随后逐条 `translate_one()`。

## 5.3 hv_enter：宿主与 guest 的寄存器交接

`host/enter.S:154`：

```
hv_enter(cpu, block):
    stp x29,x30 / x19-x28 / d8-d15 (低 64 位) → 宿主栈      ← 宿主 callee-saved 只保这些
    mov x28, x0                                            ← HvCpu *
    LOAD_FLAGS_FP                                          ← 恢复 guest q0-q31 + NZCV
    LOAD_X1_X30                                            ← 恢复 guest x1..x30
    ldr x15, [sp, #160]                                    ← 块入口地址
    mov x0, sp; str x0, [x28, #CPU_HOST_SP]                ← 存宿主 SP
    ldr x0, [x28, #CPU_SP]; mov sp, x0                     ← 换成 guest SP
    ldr x0, [x28, #0]                                      ← guest x0
    isb
    br x15
```

寄存器归属（当前代码）：

| 宿主 | 内容 |
|---|---|
| `x28` | `HvCpu *`（永不外借） |
| `x18` | 平台保留（Darwin thread state / Android SCS），**永不分配** |
| `x6`、`x7`、`x15` | 纯 scratch，**永不分配**（guest x6/x7/x15 住 slot） |
| `x16`、`x17` | **我们不再分配给 guest**（D-1）：`SAVE_X2_X29` 按编号把它们存进 guest x16/x17 的槽，所以宿主 x16/x17 必须一直就是 guest x16/x17 |
| 其余（`candidates` 顺序） | 分配给被选中的 guest 寄存器；未选中的住 `HvCpu.X[reg]` |

## 5.4 块内：一条 load/store 的完整路径

`translate_mem()`（`core/translate.c:1063`）为每条访问生成：

```
1. 选 scratch: A = s[0], T = s[1], U = s[2]（pick + spill）
2. 算有效地址到 A（立即数/寄存器偏移/字面量/写回形式）
3. TLB 检查（用 T/U，不碰条件标志）：
     LDR  U, [T, #tag_off]          ← 表项的 tag（物理页号基址，已调整）
     SUB  U, addr, U
     UBFM U, U, #14, #63            ← 取页号，非本页则为非零
     CBNZ U -> cold 慢路径
4. 命中：直接生成宿主 LDR/STR（addend 从表项 +24 取）
5. 未命中：cold 里的 OOL_SLOW，blr 到 hv_helper_thunk（op word 带 rflags/retry）
```

- **组**（`role 1/2`）：同一基址的连续访问只检查一次 TLB，addend 存
  `cpu->gaddend[slot]`；检查失败整组退回逐条翻译（`emit_group_copies`）。
- **写回形式 / RMW / 原子 / 向量**：不走 refill，直接进 helper（`can_refill` 为假）。

## 5.5 helper 调用：两层保存（我们的改动所在）

翻译代码这一侧（`put_helper()`，`core/translate.c:558`）：

```
str x30, [x28, X30]                  ← 保存 guest x30
STP_Q q0,q1 … q30,q31 → HvCpu.V      ← 【我们加的】保存全部 guest 向量寄存器
ldr x15, [x28, HELPER_THUNK]
adr x6, <HvHelperInfo 在 pool 里>
blr x15
ldr x0, [x28]                        ← resume 点（put_helper 返回它的下标，供 refill 重试）
LDP_Q q0,q1 … q30,q31 ← HvCpu.V      ← 【我们加的】恢复
```

thunk 这一侧（`host/enter.S`）：

```
_hv_helper_thunk:
    ldr w15, [x6, #12]; ubfx w15, w15, #8, #11     ← op 里有没有 refill 标志
    cbz w15, 3f                                     ← 没有：直接去 helper_entry

    （有 refill）
    str x30 → CPU_REFILL_RET; str x6 → spill[3]
    stp x0,x1 / x2,x3 / x4,x5 / x8,x9 / x10,x11 / x12,x13
    str x14; stp x16,x17                             ← 按编号保存
    mrs nzcv → CPU_NZCV; sp → CPU_SP; sp = CPU_HOST_SP
    SAVE_FLAGS_FP                                    ← 【我们加的】保存 q0-q31
    bl _hv_tlb_refill
    恢复 x0-x5,x8-x14,x16,x17,x30,x6
    LOAD_FLAGS_FP                                    ← 【我们加的】恢复 q0-q31
    if (x7 == 0) ret 到 retry 点（重跑原访问）
3:  stp x0,x1 → HvCpu; b _hv_helper_entry
```

`_hv_helper_entry`：

```
str x30 → CPU_HELPER_RET / CPU_HELPER_FROM
SAVE_X2_X29                                    ← 含 x16/x17，按编号进 guest 槽
sp → CPU_SP; sp = CPU_HOST_SP
SAVE_FLAGS_FP                                  ← 【我们加的】
bl _hv_helper (C)
if (返回非 0) 回宿主（Lrestore_host）
LOAD_FLAGS_FP / LOAD_X1_X30 / 换回 guest sp
ret 到 CPU_HELPER_RET（或下一块起点）
```

**结论（重新推理）**：`put_helper` 与 thunk 各存一次 q0-q31 是**冗余但正确**的：

- `put_helper` 的保存覆盖"helper 调用"这段；
- thunk 的保存覆盖 `hv_tlb_refill` 这段；
- `_hv_ijc_thunk` / `_hv_xjump_thunk` **没有** `put_helper` 外层，所以必须靠 thunk 自己保存；
- `_hv_chain_thunk` 不调用 C（只查 slot），不需要保存。

代价是每次 helper 调用多两次 512 字节的 STP/LDP；语义上没有问题。

## 5.6 退出与链接

```
块末尾（put_exit / put_branch_exit）：
    str pc → HvCpu.PC
    ldr x1, [x28, #CPU_EXIT]; br x1          ← 回到 hv_exit

hv_exit:
    guest sp → CPU_SP; 换回宿主 sp
    SAVE_X2_X29; str x30; SAVE_FLAGS_FP
    恢复宿主 d8-d15 / x19-x28 / x29,x30; ret  → 回到 run_loop
```

- **可链退出**：`put_branch_exit` 把 slot 地址留在 `CPU_LAST_STUB`，
  `run_loop` 记 `pending_chain`；下一个块正是目标时 `chain()` 回填（`hv_translate_place`
  里也会对同页已有块做早期链接）。`slots == NULL` 时我们拒绝链接并打印（T-2）。
- **间接跳转**：`_hv_ijc_thunk` → `hv_ijc_resolve`（C），命中则 `ret` 进块。
- **跨页直接跳转**：`_hv_xjump_thunk` 先查 IJC 表（18 位，两路），未命中再 resolve。
- **superblock**：`go_on()` 允许跨条件分支继续翻译（同页且 `cur_i+1 < 48`）；
  我们加了 `nosuper_va()`（`HV_NOSUPERBLOCK_VA`）按范围关掉。

## 5.7 我们改动在这条路径上的准确位置

| 路径位置 | 改动 | 影响 |
|---|---|---|
| `hv_translate_emit` 开头 | `put_trace_entry` | 仅 `log>=2` 生成指令；默认无影响 |
| `go_on`/`superblocks` | `nosuper_va` | 默认关；调试用 |
| `pick`/`candidates` | 去掉 16/17 | 块内不再把 guest 寄存器放进宿主 x16/x17 |
| `put_strip` | `BFM #16,#14` | AUT/XPAC 后 16 位签名字段清零 |
| `translate_mem` | 无 | 快慢路径与上游一致 |
| `put_helper` | 保存/恢复 q0-q31 | helper 调用期间 guest 向量寄存器不变 |
| `_hv_helper_thunk`（refill 分支） | `SAVE/LOAD_FLAGS_FP` | `hv_tlb_refill` 期间同上 |
| `_hv_ijc_thunk` / `_hv_xjump_thunk` | `SAVE/LOAD_FLAGS_FP` | `hv_ijc_resolve` 期间同上 |
| `_hv_chain_thunk` | 无 | 不调 C，不需要 |
| `hv_enter` / `hv_exit` | 无（上游已有 Q 保存） | — |
| `run_loop` | `m->stop` 检查、`hv_el0_note`、诊断 | 每块 1 次原子读；每个 **EL0** 块约 17 次 store（见下） |
| `lookup` | 无（只加计时变量） | — |
| `hv_translate_place` 前的 `slots` 检查 | 拒绝链接并打印 | 防空指针；正常路径不变 |
| `hv_run` | 计时包装（`HV_BLOCKTIME`） | 默认关 |
| `enter.S` 符号 | `.hidden` | 符号不导出、不可抢占；**实测仍经 GOT 取地址**（见 §2.1.6） |
| `jit_region` | Android memfd 双映射 | 区域分配方式；guest 不可见 |
| `exception.c` | OEM HVC（WFK/KICK/MABS/UUID） | 上游走降级；我们实现 |
| `dispatch.c` | `kernel_fixups` / `random-seed` | 默认关 |

### 5.7.1 一个需要如实记录的细节：`hv_el0_note` 并不便宜

`core/dispatch.c:2691` 的 `hv_el0_note()` 只对 `el == 0` 生效，但每个**用户态块**都做：

```
可选 1 次：把 x0 写进 hv_svc_ret[]（上一个 syscall 的返回值）
2 次：hv_last_el0_va / hv_last_el0_ttbr0
每页一次（不是每块）：hv_mmu_walk 解析该页的 PA
13 次：x0 x22 x23 x19 x20 x21 lr sp x16 x1 x2 x3 x8
2 次：hv_el0_trace_va[] / hv_el0_trace_x16[]（16K 环）
1 次：hv_el0_trace_n++
```

合计约 **17 次 store + 1 次原子/页判断**（页面不变时不做 walk）。

注意代码里的注释仍写着 *"Five stores."*，那是在只存 5 个寄存器时写的，后来陆续加到 13 个，
注释没有跟着改。**实现以代码为准，注释已过时** —— 这也是"完整上下文"能发现、
单看 diff 容易漏掉的东西。

`hv_el0_note` 在 `m->log` 的任何取值下都会执行（不像 `hv_pc_hist_note` 那样要求 `log>=2`）。
它的成本是有意的（换来"用户态最后停在哪"这一信息），但确实不是零成本；
如果要评估它对时序的影响，应做"开/关"对照（当前未做，见 U-12）。

## 5.8 现在能确定什么、不能确定什么

**能确定（代码事实）**：

1. 完整的进入/翻译/helper/退出路径如 §5.1–5.6，每一步都能在源码里指出行号；
2. 我们的改动集中在**向量寄存器保存**（put_helper + 三个 thunk）、**寄存器候选集**、
   **PAC strip**、**链接保护**、**`.hidden`**，外加一组默认关闭的诊断/开关；
3. `put_helper` 与 thunk 的双重 Q 保存是冗余但正确的。

**不能确定（且当前无法验证）**：

1. 上游在 iOS 上为什么不触发同一问题 —— 没有苹果设备，**不猜**；
2. `.hidden` 是否是 x16 污染的唯一来源 —— 需要对照运行才能确认，**不猜**。

> 结论：本节只陈述**当前代码怎么走**。凡涉及"上游行为"的部分，一律留空，
> 不用推理填补。

## 2.1.6 `.hidden` 的实测结果：它并没有消除 GOT 访问

之前的说法是"`.hidden` 让地址成为链接期常量、不再走 GOT"。**用当前宿主构建实测，这个说法不准确。**

### 检查方法（可复现）

```bash
# 1) 符号可见性
llvm-readelf -s build/host/host-install | grep -E "hv_(helper_thunk|enter|exit)"
#   → hv_helper_thunk / hv_chain_thunk / hv_ijc_thunk / hv_xjump_thunk / hv_enter / hv_exit
#     全部是 GLOBAL HIDDEN；动态符号表 (--dyn-syms) 里没有 hv_*

# 2) 取地址的指令（hv_cpu_reset_index）
llvm-objdump -d --no-show-raw-insn build/host/host-install
#   310ac: adrp x0, 0x1cf000
#   310b0: ldr  x0, [x0, #0xdb0]     ← 仍然从 .got 加载
#   310b4: str  x0, [x20, #0x420]    ← HvCpu.helper_thunk
#   310b8: adrp x0, 0x1cf000
#   310bc: ldr  x0, [x0, #0xdd8]     ← chain_thunk
#   310c0: str  x0, [x20, #0x428]
#   310c4: adrp x0, 0x1cf000
#   310c8: ldr  x0, [x0, #0xf10]     ← ijc_thunk
#   310cc: str  x0, [x20, #0x430]
#   310d0: adrp x0, 0x1cf000
#   310d4: ldr  x0, [x0, #0xed8]     ← xjump_thunk
#   310d8: str  x0, [x20, #0x4e0]

# 3) 这些槽来自 RELATIVE 重定位
llvm-readelf -r build/host/host-install | grep -iE "5a93c|5aab8|5abe4|5aa80"
#   01cfdb0 R_AARCH64_RELATIVE  5a93c   (= hv_helper_thunk)
#   …
```

### 结果

| 说法 | 实测 |
|---|---|
| `.hidden` 后符号不导出 | **成立**：`GLOBAL HIDDEN`，`--dyn-syms` 无 `hv_*` |
| `.hidden` 后地址成为链接期常量、不走 GOT | **不成立**：取地址仍是 `adrp x0, 0x1cf000; ldr x0, [x0, #0xdb0]`，即从 `.got` 加载 |
| 取地址指令会用 x16 | **本构建里没有**：用的是 `x0` |
| 取地址发生在 guest 执行期间 | **不是**：在 `hv_cpu_reset_index` 里，CPU 初始化阶段 |

### 所以 `.hidden` 实际做了什么

1. **符号不进入动态符号表**，不会被别的 DSO 或 `LD_PRELOAD` 抢占（这是它真正的作用）；
2. **没有消除 GOT 访问**：PIE 需要运行期基址，链接器仍生成 `.got` 槽 +
   `R_AARCH64_RELATIVE`，取地址照样是一条 `ldr`。

`enter.S` 里那段注释（*"otherwise taking it goes through the GOT, and the instructions that
do that use x16 — which is a guest register here"*）在本构建的实测里**没有得到支持**：
GOT 访问用的是 x0，而且发生在初始化，不在 guest 运行时。

**这与"上游为什么能跑"直接相关**：既然我们自己的构建也经过 GOT，"Mach-O 不经 GOT"
就不能再当作 D-1 的解释。D-1 的候选解释因此撤回，重新回到"未解释"。

> 需要说明的是：`.hidden` 本身没有坏处（不导出更安全），保留它没有问题。
> 只是**它的理由描述与实测不符**，不能再用它解释 guest x16 的污染来源。

## 2.1.7 上游 release 0.3 的二进制实测（不需要苹果设备）

上游有**公开发布**：GitHub releases `0.3` / `0.2` / `0.1`，资产是 `Henge.ipa` 与 `Henge.tipa`。
IPA 是 unsigned/ad-hoc 打包，**二进制未加密**，可以直接反汇编。

```bash
curl -sSL -o /tmp/Henge.ipa https://github.com/Leviidev/Henge/releases/download/0.3/Henge.ipa
unzip -q /tmp/Henge.ipa -d /tmp/up030
# Mach-O 64-bit arm64 PIE，未 strip（18697 个符号）
llvm-nm /tmp/up030/Payload/Henge.app/Henge | grep hv_
#   _hv_cpu_reset_index 0x1000f17cc
#   _hv_enter 0x10011fb70   _hv_exit 0x10011fc40
#   _hv_helper_thunk 0x10011fe54   _hv_xjump_thunk 0x10011ffec
```

### 实测 1：取 thunk 地址 → `adrp/add`，**不经 GOT**

```
1000f1a30: adrp x8, 0x10011f000
1000f1a34: add  x8, x8, #0xe54        ← 0x10011fe54 = hv_helper_thunk
1000f1a38: str  x8, [x19, #0x420]     ← HvCpu.helper_thunk
1000f1a3c: adrp x8, 0x10011f000
1000f1a40: add  x8, x8, #0xf10        ← chain_thunk
1000f1a48: adrp x8, 0x10011f000
1000f1a4c: add  x8, x8, #0xf48        ← ijc_thunk
1000f1a5c: adrp x9, 0x10011f000
1000f1a60: add  x9, x9, #0xfec        ← xjump_thunk
```

**用的是 x8，不是 x16；而且没有 `ldr`——链接器直接算出了地址。**

对照我们的 ELF 构建（§2.1.6）：

```
310ac: adrp x0, 0x1cf000
310b0: ldr  x0, [x0, #0xdb0]          ← 经 .got
310b4: str  x0, [x20, #0x420]
```

**所以"上游不经 GOT、我们经 GOT"这一点是实测成立的**（上一节暂时撤回的那半条解释，现在有了二进制证据；
但"GOT 指令用 x16"仍然不成立——两边都不是 x16）。

### 实测 2：上游 thunk **没有** q0-q31 的保存

```
10011fe54: ldr  w15, [x6, #0xc]
10011fe5c: cbz  w15, 0x10011ff04
10011fe60: str  x30, [x28, #0x438]
10011fe64: str  x6,  [x28, #0x130]
10011fe68: stp  x0, x1,  [x28]
10011fe6c: stp  x2, x3,  [x28, #0x10]
10011fe70: stp  x4, x5,  [x28, #0x20]
10011fe74: stp  x8, x9,  [x28, #0x40]
10011fe78: stp  x10, x11,[x28, #0x50]
10011fe7c: stp  x12, x13,[x28, #0x60]
10011fe80: str  x14, [x28, #0x70]
10011fe84: stp  x16, x17,[x28, #0x80]
10011fe8c: mrs  x15, NZCV
10011fe90: str  x15, [x28, #0x108]
...
10011feac: bl   0x1000f76f8 <_hv_tlb_refill>
10011febc: ldr  x0, [x28, #0x108]
10011fec0: msr  NZCV, x0
```

**没有任何 `stp q` / `ldp q`**。上游 0.3 确实是"thunk 只保存 NZCV 和通用寄存器"，
与我们 U-10 描述一致；我们的 Q 保存是后来加的。

### 实测 3：上游 0.3 的 `candidates[]` **含 16、17**

在二进制里按字节搜索候选表：

```
含 [6,7,15,9,10,11,12,13,14,0,1,2,3,4,5,8,16,17,19,…,30]  → 命中，文件偏移 0x2836a0
不含 16/17 的序列                                        → 未命中
```

也就是说：**上游 0.3 既允许借用 x16/x17，也不保存 q0-q31，而且这是它对外发布的、
能用的版本**（release 0.3，2026-11-10）。

### 这些事实合起来说明什么

| 事实 | 证据 |
|---|---|
| 上游 iOS 构建不经 GOT | §2.1.7 实测 1 |
| 我们的 ELF 构建经 GOT | §2.1.6 实测 |
| 两个构建都**不**用 x16 取这些地址 | 两处反汇编 |
| 上游 0.3 借用 x16/x17 且不保存 q0-q31 | §2.1.7 实测 2、3 |
| 上游 0.3 是可运行的产品 | 它是公开 release |

因此：

1. **D-1（移除 16/17）与 U-10（保存 q0-q31）都不能升级为"上游 bug"** ——
   上游带着这两处"问题"发布了可用版本；
2. **"上游不经 GOT"已被证实**，但它**不是 x16 污染的来源**（两边都未用 x16），
   所以 D-1 仍需要一个**别的**解释；
3. 那个解释目前**没有**，且**当前无法验证**（没有苹果设备）。文档不猜。

> 这一节的价值在于：它把"上游为什么不触发"从"无从下手"变成了**有二进制可查**。
> 以后任何关于上游构建细节的疑问，都可以先下 release IPA 反汇编，而不是靠推理。
