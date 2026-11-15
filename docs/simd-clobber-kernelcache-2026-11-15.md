# 2026-11-15（晚）更正：内核崩溃的真因是 guest SIMD 跨 helper 调用丢失

> 本文**取代** `kernel-panic-investigation-2026-11-15.md` 的归因。那篇的**观察**全部成立
> （iBEC 确实把新产生的字节写进了 kernelcache 的 `+0x9EF860`，`kt_flags` 确实变成了 `0x2`），
> 但**归因错了**：那些字节不是"来自 staging 的坏数据"，也不是"布局撞车"，而是 iBEC
> **自己解压出来的坏数据** —— 它的 LZFSE 解码器状态在引擎的慢路径上被破坏了。
>
> 证据等级约定同旧文：**已确证** = 有实测/fixture 支撑；**未确证**单独标出。

## 一、一句话结论

引擎把 guest 的 `v0-v31` **直接当作宿主的 SIMD 寄存器**（SIMD 指令原样透传），但有两条从翻译
代码进入 C 代码的路径**没有保存它们**。iBEC 的 LZFSE 解码器把 FSE 状态放在 `q0-q2`，一次慢路径
就被 C 编译器用掉的向量寄存器覆盖 → 解码器取出错误的 match distance → kernelcache 被解压成
**错的数据** → `kt_flags` 是 `0x2` 而不是 `0x71` → `kalloc_type_validate_flags()` 断言失败 →
panic → 打印 `%s` 时又踩到坏指针 → 异常向量里连环缺页 → 静默 + MACH Reboot。

## 二、因果链（修订版）

```
guest 的 LZFSE 解码器把 FSE 状态放在 q0-q2
  一次 memcpy/memmove 走到引擎慢路径（helper 或 TLB refill）
    宿主 C 代码用掉 q0-q7 / q16-q31 和 q8-q15 的高半（AAPCS caller-saved）
      回到翻译代码时 q0-q2 已经是垃圾
        解码器的 match distance 取自垃圾 → 拷贝出错误的字节
          kernelcache 落到 PA 0x73b18000 时 +0x9EF860 等处是错值
            kt_flags = 0x00000002（缺 KT_CHANGED|KT_CHANGED2，期望 0x71）
              kalloc_type_validate_flags() 断言失败，panic @kalloc.c:705
                panic 打印 %s → 读 0x20000000935144 → level-0 转换失败
                  进异常向量 → 内核自己的页表把异常栈页标成 AP=2 只读（esr 9600004f）
                    处理该异常又写栈 → 栈一路下降 → TPIDR_EL1 为空
                      "can only continue if TPIDR_EL1 is set" → wfe 自旋 → halt x2 "MACH Reboot"
```

和旧文的差别只在**链条最前面**：坏数据是 iBEC **算**出来的，不是从外面**搬**进来的。
这一条把旧文的所有观察都解释掉了：

| 旧文的观察 | 在修复后的解释 |
|---|---|
| `write to pa 74507860 = 20000000935144 by pc 700a7fec`（iBEC 的 `stnp` 拷贝循环） | iBEC 正在把**它解压出来的** kernelcache 写到 `0x73b18000`，写入本身完全正常 |
| 坏值在 IPSW 里 **0 命中**，是"新产生的字节" | 正是 LZFSE 解压产生的新字节；解压错了才是新值 |
| 同一例程也写 device tree（`lr 700a8460`） | 同一个解压/分发包装在搬多个组件 |
| `kt_flags` 少了 `KT_CHANGED`\|`KT_CHANGED2` | 这两个标志在 view 的**第一个** 8 字节里，任何错位都先打掉它们 |
| 关掉 rebase（`HV_NO_KFIX`）没变化 | 与 rebase 无关 |

## 三、两条漏掉的路径（**已确证**）

guest 的 `v0-v31` **就是**宿主的 `v0-v31`：`translate_copy` 把 SIMD 指令原样 `put()`，所以任何从
翻译代码调进来的 C 函数都可能在 caller-saved 向量寄存器上留下垃圾。

### 1. `put_helper`（内存/系统寄存器慢路径）

`core/translate.c` 的 `put_helper` 只做了 `STR_X(30, 28, HV_CPU_X(30))` —— 只存 `x30`。
helper 是 C 函数，`memop.c` 用 `-O2` 编出来含 17 处 `q0` 访问。

### 2. `hv_tlb_refill`（TLB 未命中）

refill 走的是**另一个 thunk**，从翻译代码直接调用，**不经过 `put_helper`**；它的代码在
`core/mmu.c` 里。Makefile 早就给 `mmu.c` 加了 `-mgeneral-regs-only`，但 **Android 的两套构建
（`tools/build-install-android.sh`、`android/CMakeLists.txt`）漏了这个 flag**。实测：

| 编译方式 | `mmu.c` 里的 SIMD 指令数 |
|---|---|
| clang `-O2`，无 flag | 60 处 `q0` + 16 处 `v0.16b` |
| clang `-O2 -mgeneral-regs-only` | 0（编译通过） |
| gcc `-O2`（宿主） | 24 处（`memop.c` 在 gcc 下是 0，所以宿主复现不了 `memop` 路径） |

块边界本身是安全的：`host/enter.S` 已经保存/恢复了 `q0-q31`（`[x28,#320]`…`[x28,#800]`，
即 `HV_CPU_V = 0x140` 起 512 字节）。坏的只是**块内部的 helper 调用点**。

## 四、修复（**已验证有效**）

```
core/translate.c
  + STP_Q / LDP_Q 宏（opc=10，imm7 按 16 缩放）
  put_helper: 调用前 16 条 stp q（q0..q31），ldr x0 之后 16 条 ldp q
  返回值从 void 改成 uint32_t，返回 ldr x0 的索引
  put_helper_refill 改用这个返回值（原来靠 e->n - 1 猜，加了指令后就猜错了）

tools/build-install-android.sh
  + build_mmu()：core/mmu.c 单独用 -mgeneral-regs-only；build_core() 跳过 mmu.c

android/CMakeLists.txt
  + set_source_files_properties(core/mmu.c ... -mgeneral-regs-only)

core/exception.c（调试增强）
  + permission fault 时 dump 完整 walk（原来只有 translation fault 会 dump）
  + data abort 打印 x29/x30（调用者）
```

`-mgeneral-regs-only` **不能**全局加：`dispatch.c` 的 `fprintf` 需要 double ABI，否则报
`'fprintf' requires 'double' type support, but ABI 'aapcs' does not support it`。所以走
"保存/恢复 + 单文件 flag" 两条路。

## 五、证据（**已确证**）

### 1. 设备实测：`kt_flags` 从 `0x2` 变回 `0x71`

`HV_WATCH_PA=74507878`（`kernelcache + 0x9EF878`）：

| | `+0x9EF860` | `+0x9EF878`（`kt_flags`/`kt_size`） |
|---|---|---|
| IPSW 原始 | `0x00200000002e0f96` | `0x000402f000000071` |
| 修复前 | `0x0020000000935144` | `0x00043c020000000002` |
| **修复后** | **`0x00200000002e0f96`** | **`0x000402f000000071`** |

逐字节一致。

### 2. JIT 代码反汇编：`stp q`/`ldp q` 0 → 16

把 `hv_run` 产生的 JIT 代码 dump 下来按 opc 统计：

| 版本 | `stp q` (0xad0*) | `ldp q` (0xad4*) |
|---|---|---|
| 修复前 | 0 | 0 |
| 修复后 | **16** | **16** |

16 对 = 32 个向量寄存器，正好覆盖 `q0-q31`。

### 3. 进程推进：从"iBEC 阶段失败"到"内核完整引导 + 拉起 EL0"

修复前 kernelcache 是坏的，panic 在 `kalloc_type_validate_flags`。
修复后内核跑完整个引导，`pmap` 的按需映射产生了 465 次 data abort（全是内核自己在
`pmap_fault` 里处理掉的），最终 `install: restore finished: -2` —— 这个返回值在修复前**从未出现过**。

## 六、回测方法论（这一层的通用做法）

`tests/` 里的 native-vs-JIT 对比测试原本只能编在 Darwin（汇编用了 `sym@PAGE`/`sym@PAGEOFF`，
C 符号假定 `_` 前缀）。这次把它们改成按平台选语法，于是能在 Linux/bionic 上编译、也能推到设备跑。

**踩过的坑，别再踩**：

| 坑 | 现象 | 正确做法 |
|---|---|---|
| ELF 上 `.set foo, _foo` | 别名是**段内偏移**，guest 从 `pc 0x200` 起跑 | 用**同地址双标签**（`_foo:` 和 `foo:` 各一行） |
| `.macro` 体内的 `#if` | cpp 不展开，宏里会留下字面 `#if` | 宏体里两个标签都无条件发；平台选择放在宏**外** |
| `run_native` 栈上放两个 `HvCpu` | 2 MB+ × 2，默认 8 MB 栈直接 SIGSEGV（宿主和 bionic 都一样） | `ulimit -s 65520`（Makefile 早就这么干了） |
| bionic 的 `mmap` 落点 | `slots 0xb400007c74c38c00`，与 JIT 代码区超过 ADRP ±4 GB | 不是引擎逻辑问题；测试侧别依赖绝对落点 |
| 用 `-O1` 造"能过的对照组" | `test_bitops.c` 自己在 `-O2` 有 UB，`-O1` 就过，会误判成引擎 bug | 对照组要固定**被测文件**，别把测试代码的 UB 算进去 |

**关键教训：宿主 gcc 编不出这个 bug**（`memop.c` 在 gcc `-O2` 下 0 处 SIMD）。所以"宿主测试全过"
不能证明 helper 路径安全，只有 **JIT 代码反汇编**（数 `stp q`）和 **`kt_flags` 实测**能证明。

## 七、修复后暴露的下一层（**未解决**）

内核完整引导之后，走到**用户态**再卡住：

```
install: restore finished: -2
sending [setenv boot-args rd=md0 nand-enable-reformat=1 -progress -restore]
Failed to place device in restore mode
```

`-2` 对应 `third_party/idevicerestore/src/idevicerestore.c:1535` 的
`recovery_enter_restore()` 失败 —— 内核没进 restore 模式。

日志最后一条是 **EL0** 的缺页：

```
data abort: esr 92000007 far 102ae4000 pc 10284a9a0 el0 insn-ok 1
             x0 102ae4000  x2 4000  x30 102818eb8
```

`0x102ae4000` 是用户态地址，`ldr w8,[x0]` 读 16 KB，L3 转换失败（页表那格是空的）。
两种可能，还没区分：

- restored/launchd 的**正常按需映射**，本该由 guest 的 `vm_fault` 处理 —— 那问题在
  "内核的异常处理在这个上下文里没跑起来"；
- 或**我们漏了一个用户态映射**（比如某段 shared cache / dyld 的映射没建对）。

### 顺便否掉的三个方向（**已确证**，别重走）

| 假设 | 实验 | 结果 |
|---|---|---|
| rebase 把内核数据改坏 | `HV_NO_KFIX=1` 对照 | fault 分布 `2/6/100/598` vs `2/6/100/614`，**不是 rebase** |
| TLB 与页表不一致 | `HV_PTW_LOG=1` | `mismatch` **0 次** |
| 那个 `AP=2` 是引擎降权（`code_page`） | `perm fault` 日志 | `code_page 0`，desc 里 `ARM_PTE_WRITEABLE(bit59)=0`，内核自己也认为只读 |

### 顺带解码：`desc = 0x0460000084fa4683` 里那些"可疑位"

| 位 | 值 | 含义 |
|---|---|---|
| AP (7:6) | `10` | EL1 只读（真·只读） |
| bit 58 | 1 | **`ARM_PTE_WIRED`** —— XNU 的软件"已连线"位，正常 |
| bit 59 | 0 | **`ARM_PTE_WRITEABLE` = 0** —— 软件记账也说不可写 |

定义在 `/tmp/xnu/osfmk/arm64/proc_reg.h:1805-1808`。**不是污染**，别再追这条。

## 八、VMAPPLE 的 KVA 布局（备查）

`/tmp/xnu/osfmk/mach/arm/vm_param.h` 两条分支：

| 配置 | `VM_MIN_KERNEL_ADDRESS` | `VM_MAX_KERNEL_ADDRESS` |
|---|---|---|
| `ARM_LARGE_MEMORY` | `0xfffffe0000000000`（-2 TB） | `VM_MIN + 832 GB - 1` = `0xfffffe33ffffffff` |
| 默认 | `0xffffffe000000000` | `0xfffffffbffffffff` |

实测内核代码在 `0xfffffe00_1cxxxxxx`，只有 `ARM_LARGE_MEMORY` 分支能容纳 ⇒ **vmapple 走的是
`ARM_LARGE_MEMORY`**。注意：fault 地址 `0xfffffe370e084000` / `0xfffffe8f46130000` 都超过
`VM_MAX`，但内核没 panic —— 说明这些是内核自己算出来的指针，不是 pmap 的合法性检查失败，
所以别把"超范围"当成根因。

## 九、工具（本篇新增）

```bash
# JIT 代码 dump：数 stp q / ldp q（回测 SIMD 保存最硬的手段）
#   建 m.slow_mem = 1，跑一小段 guest，把 m.jit.base 前 m.jit.used 写到文件
#   然后按 (insn & 0xffc00000) == 0xad000000 / 0xad400000 统计

# permission fault 的完整 walk（core/exception.c，log >= 2）
grep "perm walk" run.log        # 最后一行给出 L3-table 的 PA
grep "walk " run.log            # 每级 base / idx / desc（hv_walk_dbg 打开时）

# 对照实验
HV_NO_KFIX=1        # 关 rebase，看 fault 分布是否变化
HV_PTW_LOG=1        # 页表页写历史 + TLB/页表 mismatch 检查（日志可能上百 MB）
HV_LOG=2            # 注意：HV_LOG=1 时有些 log>=2 才走的逻辑不执行，会改变行为
```

## 十、一次做完的验证清单（下次别重复问）

1. `HV_WATCH_PA=74507878` → 看 `kt_flags` 是不是 `0x71`
2. JIT dump 里 `stp q` 是否为 16
3. `grep -c "data abort: esr"` → 465 左右说明到了 pmap 阶段
4. `grep "restore finished"` → `0` 才是成功，`-2` 是 `recovery_enter_restore` 失败
5. 日志最后一条是不是 `el0` —— 是的话，问题已经在用户态，不在内核 mm
