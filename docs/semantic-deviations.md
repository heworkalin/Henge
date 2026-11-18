# 与上游的语义等效性审计

基线：`origin/main` = `e98920c`（上游 Leviidev/Henge）。
本文只回答一个问题：**我们的代码和上游相比，哪些改动对 guest 是等价的，哪些不是。**

判定口径（从上到下，"等效"的要求越来越高）：

| 等级 | 含义 |
|---|---|
| **E（等效）** | guest 看到的行为完全不变，只是多打了日志 |
| **X（扩展）** | 上游没有这条路径，我们补上；上游原本在这里会降级或返回错误 |
| **D（刻意偏离）** | 行为与上游不同，且有证据表明上游在这里是错的 |
| **U（未验证）** | 有偏离或可能有偏离，但没有证据判定等效 |

**判 D 之前必须先过这一关（2026-11-19 追加）**：

> **上游是已经发布、能正常跑 macOS 安装的产品，不是一个未经验证的代码快照。**
> 因此"上游有 bug"这种结论，必须同时回答一个问题：
>
> **那它为什么能正常运行？**
>
> 回答不了，就只能记成"我们的环境需要这个偏离"（U），不能记成"上游错了"（D）。
> 环境差异（libc、编译器、SIMD 直通假设、JIT 入口、信号处理、内存布局）
> 都足以让同一份代码在一侧触发、另一侧不触发。

行数（2026-11-19 重测，基线 `e98920c`）：`dispatch.c` +1887、`exception.c` +1125、
`memop.c` +395、`translate.c` +124、`machine.c` +83、`mmu.c` +44、`machine.h` +36、`cpu.h` +29、
`exception.h` +9、`sysreg.c` +6、`mmu.h` +2。

> 恢复链路的差异（第三方恢复栈、USB 设备模型、宿主层）不在本文范围，见
> `docs/upstream-restore-semantics-2026-11-19.md`。JIT/核心模块的逐项对比见
> `docs/jit-core-diff-2026-11-19.md`。

---

## E：等效（纯诊断，可安全忽略）

| 文件 | 改动 | 依据 |
|---|---|---|
| `core/sysreg.c` | 内核态 `mrs` 记录前 400 条 | 只有 `fprintf`，值不变 |
| `core/mmu.c` | `hv_walk_dbg` 走表日志、PTW 采样 | 只读 |
| `core/translate.c` | `put_trace_entry`（`log >= 2` 才发射） | 发射条件含 `log >= 2`，默认 0 时一条指令都不生成 |
| `core/dispatch.c` | beat / STALLED / hvc 计数 / MMIO 直方图 / per-block 计时 | 只读 |
| `core/exception.c` | HVC 轨迹前 32 条、hvc 计数器 | 只读 |
| `devices/*` | 移除 uart/timer/tick 的宿主探针 | 删掉的都是 `getenv` 门控的打印 |
| `core/dispatch.c` | `kernel_fixups`（chained fixup rebase 能力）默认关闭 | 上游**没有**这个函数（`git show origin/main:core/dispatch.c` 里 0 处），它是我们自己加的。默认关闭时行为与上游一致，只有 `HV_KFIX=1` 才启用 |

## X：扩展（上游没有，不改变原有路径）

| 位置 | 内容 | 依据 |
|---|---|---|
| `core/exception.c` `vmapple_hvc` | OEM 区间 `0xc3000003` MABS / `0xc3000004` BootSessionUUID / `0xc3000005` WFK / `0xc3000006` KICK，以及 `0x8100ff01`/`0x8300ff01` UID、`ff03` REVISION、`feff` FEATURES | XNU `osfmk/arm64/hv_hvc.h` + `arm64_hypercall.c`：`hvg_is_hcall_available` 先探 `PAC_NOP`、再核对 OEM UID 必须等于 `3B878185-AA62-4E1F-9DC9-D6799CBB6EBB`、REVISION major=1，任何一步失败就放弃全部 OEM 调用。上游对未知 FID 返回 `-1` ⇒ 上游一直在走降级路径 |
| `core/exception.c` WFK/KICK | WFK 按 WFI 方式 park 并按 `x1` 还原 PSTATE.I；KICK 唤醒目标 vCPU | `osfmk/arm64/lock_ticket_pv.c`：等锁前 `ml_set_interrupts_enabled(FALSE)`，等 `hvg_hc_wait_for_kick`，语义是"停住，直到被 kick 或中断唤醒，然后恢复中断状态"。上游立即返回成功 ⇒ 锁协议作废 |
| `core/dispatch.c` `kernel_fixups` | chained fixup 的 rebase 能力（默认关闭） | 见 X 节与 U-8 |
| `core/memop.c` | 每个 load/store 形式的 C 慢路径 | 上游只有内联快速路径；这是新增的兜底实现 |
| `core/dispatch.c` | `/chosen/random-seed` 用宿主 `/dev/urandom` 填充 | 上游不做这件事（0 处）。`osfmk/prng/prng_random.c:114 → PE_get_random_seed()` 读该属性（`pexpert/gen/pe_gen.c:145`），iBoot 留的是 64 个 0：不 panic，但所有派生密钥来自已知常量 |
| `core/dispatch.c` | 「不 rebase 内核镜像」的策略 | 上游也没有任何 rebase 代码 ⇒ 我们与上游一致。内核在 `osfmk/arm/arm_init.c:362 → :224 → kernel_collection_slide()` 自己 rebase 整个 collection；我们若补一次会让内核再加一次 slide（实测 `0x1d1645a0 → 0x38c7c5a0`，pc `0x1d177750` 写出该值） |

## D：刻意偏离（有证据表明上游在此处不对）

| # | 位置 | 上游 | 我们 | 证据 |
|---|---|---|---|---|
| D-1 ⚠️ | `translate.c` `is_stolen()` 候选寄存器集 | 含 `16, 17` | 移除 16/17 | 机制（与 OS 无关）：`SAVE_X2_X29` 按编号把宿主 x16 存进 guest x16 的槽；若宿主 x16 正替别的 guest 寄存器打工，该值就写进 guest x16 的槽，下个块读回的 guest x16 变成宿主地址。我们环境实测：固件拿到宿主地址后死亡。**但上游能正常跑——它为何未被触发仍未解释**。ABI 查证（`docs/jit-core-diff-2026-11-19.md` §2.1.5）：两平台对 x16/x17 的规定相同（IP0/IP1，链接器可用）；而 §2.1.6 的实测推翻了"经不经 GOT"这个解释——我们自己的 ELF 构建取 `hv_helper_thunk` 仍走 `.got`（`adrp x0; ldr x0`），用 x0 且在初始化阶段。因此 D-1 目前**没有任何解释**，只能证明"我们的环境必须偏离"。补充：上游 release 0.3 的 IPA 实测（`docs/jit-core-diff-2026-11-19.md` §2.1.7）显示其取地址是 `adrp x8/add`（不经 GOT），且其 `candidates[]` 仍含 16/17、thunk 不保存 q0-q31 —— 带着这两处差异发布了可用版本 |
| D-2 ⚠️ | `translate.c` `put_strip()`（XPAC/AUT） | 清 bits[54:48]（`BFM #16,#6`） | 清 bits[62:48]（`BFM #16,#14`） | 机制（架构语义）：strip 应把签名字段整段变成 bit55 的符号扩展，QEMU 的 `pauth_helper.c: pauth_strip` 是唯一开源参照。只清后半段时签名上半残留，我们环境里 guest 往 `0xfffffe90b4130000` 写 → 异常处理里再缺页 → 每轮数百万次权限异常风暴。**但上游能正常跑——为何 ios 上不触发仍未解释**（可能该 AUT 路径在 iOS 的运行中不走到，或被别的行为掩盖）。同样只能先记为"我们的环境必须偏离" |
| D-3 ⚠️ | `translate.c` `put_helper()`、`host/enter.S` 的三个 thunk | helper/refill/ijc/xjump 路径**不保存 guest v0-v31**（thunk 只恢复 NZCV；`hv_enter`/`hv_exit` 才存 q0-q31） | helper 调用前后、thunk 进入/返回时**保存并恢复全部 q0-q31** | **已降级为 U-10**：本条只证明"在我们的构建上必须保存"，**不能**据此判上游是 bug。iOS 与 Android 的 helper 编译产物、libc、信号/JIT 入口都不同，上游完全可能在其环境下不需要这段保存。差异 ≠ 上游错误 |


## U：未验证（仍是风险，不能声称等效）

| # | 项 | 为什么不能算等效 | 需要什么才能判定 |
|---|---|---|---|
| U-1 | PAC 模型（整体） | 我们**从不签名**，也**从不认证失败**。真机认证失败会把指针毒化（bits 63:56 置 0），我们原样返回。任何依赖"认证失败"来发现损坏的代码路径行为不同 | 找到 guest 里依赖认证失败的路径，或改用真正的 QARMA 实现 |
| U-2 | `PACGA` | 上游/我们都返回常量 0。等于"签名/校验对内部自洽"，但与外部产生（或写入镜像）的 pacga 值不等 | guest 内是否存在跨来源比较 pacga 的地方 |
| U-3 | D-2 的 bit 63 | 我们保留 bit63 原值，硬件把它作为签名段一并处理。只有当 bit63 恰好等于 bit55 的符号扩展时才等效（内核 VA 满足，EL0 的 48 位地址也满足，但 TBI/56 位 VA 不满足） | 确认 guest 是否使用 TBI 或非规范地址 |
| U-4 | MABS offset | `VMAPPLE_GET_MABS_OFFSET` 我们原样返回 `x1`（偏移 0）。真 host 可能返回非零绝对时间偏移 | XNU 里该值的用途（`ml_get_abstime_offset`）与 guest 时间基准是否已由我们虚拟化 |
| U-5 | BootSessionUUID / UID / REVISION | 值是我们编的（UID 按 ABI 字序构造以通过校验，UUID 随机） | guest 是否缓存/比较这些值（日志、matching、kext 绑定） |
| U-6 | WFK 的 park 粒度 | 我们用 WFI 的机制 park，上限 10 ms；真 hypervisor 是"直到被 kick 或中断"。可能产生多余的唤醒（应当无害，但不等价） | 在设备端观察 WFK 频率与唤醒延迟 |
| U-7 | `memop.c` 慢路径 | 407 行新增，覆盖所有 load/store 形式。`tests/bitops_prog.s` 的 memops 用例 0 差异，但覆盖的是形式，不是全部寻址模式/边界 | 扩展 memops 用例：非对齐、跨页、pre/post-index、writeback、LDXP/STXP 边界 |
| U-8 | "不 rebase"这一**与上游共享的**假设 | 依赖"iBoot 不 rebase、内核 `arm_init` 会 rebase"。只在 **24G90** 上实测过；换固件（如 25G83）未验证 | 其他固件版本重跑一次带 `HV_KFIX`/`HV_DC_PROBE` 的对照 |
| U-9 | 图形（PVG） | 宿主构建是 stub（`android/` 才是真的 reims-vgpu），宿主上所有图形结论对设备不成立 | 设备端验证 |
| U-10 | helper/thunk 是否必须保存 guest v0-v31 | 我们实测：不保存时 iBEC 的 LZFSE 解码器丢掉 q0-q2 里的 FSE 状态，解出的 kernelcache 字节错误（`kalloc_type.kt_flags` 读成 `0x2` 而不是 `0x71`），随后 `validate_flags` panic。**但这是在我们的构建上**。上游 release 0.3 的 IPA 实测（`docs/jit-core-diff-2026-11-19.md` §2.1.7）确认其 thunk 同样不保存 q0-q31 且是公开可用版本 —— 所以只能记"我们的构建需要它"，不是"上游写错了" | 在 iOS/上游构建上复现同一现象；或证明上游的 helper 同样会破坏 v0-v31 且该路径在 iOS 上也执行。在那之前只能说"我们的实现需要这段保存" |

## 一句话总结

- **纯诊断类（E）占了绝大部分行数差**，对 guest 没有语义影响
- **被标为 D 的只剩 2 处（D-1、D-2）**，机制上都有**架构级**依据（ARM64 ABI / PAC 位域，与操作系统无关）；
  **但两者都还欠一句"上游为什么能跑"**——上游是已发布可运行的产品，在解释清楚之前，
  严格说它们也只是"我们的环境必须偏离"，不是"上游错了"；另有 2 项原本被我误判成偏离——rebase 内核镜像是**我们自己加过又撤回**的（上游从没做过，撤回后即与上游一致），`random-seed` 填充是**我们新增**（上游没有）；
  **D-3（SIMD 保存）已降级为 U-10**：它是"我们的构建需要"，不能反推上游有 bug —— iOS 与 Android 的 helper 编译产物、libc、JIT 入口和信号行为都不同，**差异不等于上游错误**
- **仍然不能声称等效的有 9 项（U-1..U-9）**，其中 U-1（PAC 整体）、U-3（bit 63）、U-7（慢路径覆盖面）是当前最该继续收口的
