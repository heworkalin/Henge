# AGENTS.md — Henge

本文件是**项目级**给 agent 的说明。全局规则（授权边界、破坏性操作、语气、语言）见
`$HOME/.pi/agent/AGENTS.md`，这里不重复，只写"Henge 这个项目该怎么干活"。

文档用中文，代码注释用英文。绝对路径优先。

---

## 1. 项目是什么

**Henge**：在非特权环境里跑 macOS 虚拟机的模拟器。

- 上游：`https://github.com/Leviidev/Henge`（iOS 版，作者 Leviidev）
- 本仓库：`dev` 分支 = 上游 + 我们的 **Android 移植**（`android/`）与内核启动调试
- 模拟对象：Apple **VMAPPLE** 平台（Virtualization.framework 的 guest 侧设备模型）
  引导链：`AVPBooter → iBoot Stage1 → Stage2(iBEC) → ramdisk + device tree + kernelcache → XNU → restored_external`
  （`restored` 与 `restored_external` 是**同一个 task** 的两个 `p_comm` 视图，后者被截成 `restored_externa`）
- 我们实现的是 **EL2 角色**：自有 arm64 JIT、软件 MMU、PVG 半虚拟 GPU、virtio/bdif/xhci/aes/rtc/pvpanic/cfg 设备

管线：`PVG 设备模型 → reims-vgpu → metal2vulkan → Vulkan(Turnip/原厂) → SurfaceView`

**主目标：把「安装 macOS」(DFU + idevicerestore) 跑通。**

## 2. 目录与职责

| 路径 | 内容 |
|---|---|
| `core/` | JIT 翻译器 `translate.c`、dispatcher `dispatch.c`、异常/HVC `exception.c`、软件 MMU `mmu.c`、内存慢路径 `memop.c`、sysreg、机器与锁 `machine.c` |
| `devices/` | VMAPPLE 设备模型：`vmapple.c`(tick/MMIO 总表)、gicv3、pl011、pl031、pl061、aes、cfg、bdif、xhci、pvg*、virtio* |
| `host/` | 上游宿主层：`vm.c`(线程/kicker/WFI)、disk、hvsparse、libusb_shim、dns_proxy |
| `ios/` | 上游 iOS App（SwiftUI）。**UI 结构/交互的参照实现**，改 Android UI 前先看这里 |
| `android/` | Android 移植：`install.c`(宿主入口)、`henge_vm.c`、`probe_jni.c`、`stderr_to_logcat.c`(统一日志)、`app/`(Java UI)、`build-*.sh` |
| `tools/` | 宿主侧工具：`build-install-host.sh`、`run-install-host.sh`、`host-install.c`、`disasm-raw.sh`；内核/内核态分析：`lcfs.py`（`LC_FUNCTION_STARTS` 函数索引）、`ksym.py`（kernelcache 符号）、`tsbase.py`（guest 页表遍历 + proc/walk 基础）、`dump-*.py`（线程/waitq/vmmap/栈） |
| `tests/` | 本地脚手架（寄存器/位运算/memops/mathops/atomics/LZFSE） |
| `third_party/` | idevicerestore/libimobiledevice、reims-vgpu、mbedtls、libzip、lzfse |
| `docs/` | 见第 9 节，**先读再改** |

## 3. 构建与运行

### Android（已装设备）

```bash
./android/build-restore.sh      # idevicerestore/libimobiledevice（很少需要）
./android/build-c.sh            # 引擎 + JNI → libhenge.so
./android/build-apk.sh          # 打包 APK
adb -s emulator-5554 install -r android/app/build/outputs/apk/debug/app-debug.apk
adb -s emulator-5554 shell am force-stop dev.henge.probe   # 装完必须重启 App
```

`android/env.sh` 提供 NDK（`/home/he/Android/Sdk/ndk/29.0.14206865`）与 C/CXX/AR。
**注意**：日志首行 `HENGEBUILD` 来自 `android/install.c`，该文件没改时不会重编，
**不能用它判断构建是否新鲜** —— 要验证就 `unzip -p …app-debug.apk lib/arm64-v8a/libhenge.so | strings | grep <新增字符串>`。

### 宿主（首选！几十秒一轮，不需要设备）

```bash
./tools/build-install-host.sh                 # gcc/glibc 版本
CURL_CA_BUNDLE=$PWD/work/ca-bundle.pem \
  ./build/host/host-install work/AVPBooter.vmapple2.bin android/ipsw/24G90.ipsw work 2048 2
```

宿主与设备的差异（结论只在宿主成立时必须说明）：RAM、CPU 数、**PVG 是 stub**（真图形只在
`android/`）、`android/install.c` 默认 `log=2` 而宿主工具默认更安静。

### 本地单元测试（秒级，改 JIT 前后必跑）

```bash
./android/test-regs.sh
./android/test-bitops.sh            # ONLY=bitops|memops|mathops|atomics  ROUNDS=N
./android/test-lzfse.sh
```

## 4. 设备与数据

- 设备：`emulator-5554`（PJE110 / SM8550 / Adreno 740 / Android 15 / **宿主 4 KB 页**；guest 是 **16 KiB 页**，见 §6.9）
- App 包名：`dev.henge.probe`（debuggable → 可用 `run-as`）
- 日志（**内部存储**）：`files/vm/henge.log`（全量，GB 级）
  `adb -s emulator-5554 shell run-as dev.henge.probe cat files/vm/henge.log > /tmp/x.log`
- 工作目录：`/storage/emulated/0/Android/data/dev.henge.probe/files/vm/`
  （`24G90.ipsw`、`disk.img` 64 GB sparse、`aux.img`、`work/` 下的 dump）
- 偏好：`shared_prefs/henge.xml`（`install.build`、`ipsw.verified:<path>=size:mtime`）

## 5. 日志架构（必须遵守）

引擎 stderr → 日志服务线程 → 1) 全量写文件 2) logcat 只发**限速样本**（`HENGE_LOG_RATE`，默认 120 行/秒）+ 关键词行不限速。
**不要在引擎里直接打高频 logcat**：Android 对进程有 logcat 配额，刷爆会导致关键行丢失。

常用开关（`android/install.c` 读环境变量；设备端可用 `setprop`/改造入口）：

| 开关 | 作用 |
|---|---|
| `HV_LOG=0..3` | 引擎日志级别（Android 默认 2） |
| `HENGE_LOG_FILE` / `HENGE_LOG_RATE` | 日志文件路径 / 限速 |
| `HENGE_KICKER=0` | 关掉 1 ms kicker（对照实验） |
| `HV_RESTORE_BOOT_ARGS` | iBoot boot-args（如 `serial=3 -v startup_debug=1`） |
| `HV_KFIX=1` / `HV_KFIX_DC=1` | 打开 rebase（默认不补，对照用）/ 强制补 `__DATA_CONST` |
| `HV_BLOCKTIME=1` | 单块耗时采样（默认关，因为每块两次读计数器） |
| `HV_STACKSCAN=1` | 卡住时 dump 内核栈 |
| `HV_PTW_LOG` / `HV_PTW_SNAP` / `HV_DUMP_DC` / `HV_BLOCK_DUMP` | 页表写历史 / 段 dump |
| `HENGE_RANDOM_SEED=1` | 用宿主熵填 `/chosen/random-seed`（**默认关**，关才可复现） |
| `HENGE_TIMEOUT_SCALE` | 放大 restore 的所有等待 |
| `HV_EXTRA_BOOT_ARGS` | 追加 boot-args（串口日志最关键，见 §5.0） |
| `HENGE_STOP_ON` / `HENGE_STOP_EVERY` | 串口同步断点：输出匹配即 `SIGSTOP` / 每次匹配都停（见 §5.0） |
| `HV_PCHK` / `HV_PCHK_CPU` / `HV_PCHK_MAX` | 执行断点（默认 cpu0、400 次；`CPU=all\|N`、`MAX=N` 可改）——**只作存在性证据，不能计数**（§6.7） |
| `HV_TRACE_PC=<file>` | 全量执行轨迹（每块 run VA），判“某函数跑没跑”用它 |
| `HV_SLOW_MEM=1` | 所有 load/store 走 C 慢路径（用于让 `HV_PTW_LOG` 看见 JIT 直接发出的写） |

### 5.0 串口（guest 的 kprintf / IOLog）—— 让内核日志可见

**默认状态下 guest 内核日志全部丢弃。** `serial=3` 是**必要条件**，`debug=0xA` 是**可选项且带副作用**：

| 条件 | 必要性 | 作用 |
|---|---|---|
| `serial=3` | **必需** | 触发 `switch_to_serial_console()`，把 kprintf 改道到 PL011 |
| `debug=0xA` | 可选 | `DB_PRT`(0x2) + `DB_KPRT`(0x8) **解除** `disable_serial_output`；**会让运行有时走不到 `restored_external`**（观察扰动） |

**默认只用 `serial=3`。**

**三种注入方式（按用途选）：**

1. **恢复阶段（最常用）**
   ```bash
   HV_EXTRA_BOOT_ARGS="serial=3" ./host-install ...
   ```
   注入点在 `core/dispatch.c` 的 `boot_args->CommandLine`（PA `0x837f0000`，偏移 **`0x6c`**）。
   **注入代码必须写在长度修补的 `if/else` 之外** —— 曾因为放在分支内而永远不执行，
   导致 `-v` 和 `serial=3` 都没生效，白查了很久。

2. **iBoot 阶段**
   ```bash
   HV_RESTORE_BOOT_ARGS="serial=3 -v" ./host-install ...
   ```

3. **引擎自身日志**（不是 guest 串口）
   ```bash
   HV_LOG=1      # 0..3，Android 默认 2
   ```

**日志形态**：guest 串口行以 `hengevm: serial: ` 开头（引擎转发时加的），后面才是内核原文。

**实践经验（重要）：**

- **`serial=3` 单独就够了**；加上 `debug=0xA` 有时**反而走不到 `restored_external`**
  （观察扰动）。先只用 `serial=3`。
- 内核日志量大，必须走统一日志服务（见本节开头）：全量文件 + logcat 限速样本。
- `HENGE_STOP_ON=<串口行子串>` 可以在**某条串口输出出现时** `raise(SIGSTOP)`，
  相当于“串口同步断点”；支持 `|` 分隔多模式、`HENGE_STOP_EVERY` 每次匹配都停。
  读出后必须显式 `kill -CONT <pid>`（gdb `-batch` detach 不发 SIGCONT）。
- 配合**现场保护**：`on_install_progress` 在 `step >= HV_INSTALL_DONE` 时若设了
  `HENGE_STOP_ON` 就不 `stop_cpus()`，机器保持可 attach，能反复扫描同一现场。

### 5.1 JIT 调试参数与地址空间（最易出错，先看这里）

同一份内核镜像有**四个**地址，参数必须传对的那一个：

| 名字 | 定义 | 24G90 实测（假定 `slide = 0x14b14000`） |
|---|---|---|
| `fileoff` | 镜像内文件偏移 | `__TEXT_EXEC` = `0xd78000` |
| **link VA** | `fileoff + 0xfffffe0007004000` | `__TEXT_EXEC` = `0xfffffe0007d7c000` |
| **run VA** | `link VA + slide` | `__TEXT_EXEC` = `0xfffffe001c890000`（当 slide = `0x14b14000` 时） |
| PA | `kimage_pa + fileoff`（`kimage_pa = 0x73b18000`） | `__TEXT_EXEC` = `0x74090000` |

`slide` 可由 boot_args 反推：`virtBase 0xfffffe0018a04000 − physBase 0x70a04000 = 0x14b14000`。

**但不要假定 slide 就是 `0x14b14000`。** 实测有多轮是 **slide = 0**（`g_cpus[0].pc` 直接等于
kernelcache 里的 link VA，`HV_PCHK` 输出里也打印 `slide 0`）。**传错 slide 的后果是断点零命中，
而零命中看起来像“这段代码没执行”，极易误判。**

判断当前 run 的真实 slide（二选一）：

```
1) gdb: print/x g_cpus[0].pc   → 与 tools/lcfs.py 的输出对照，看它落在哪个函数的哪个偏移
2) HV_PCHK 命中时的日志里直接有 slide 字段（引擎会打印 x26 指向的 slide 槽）
```

| 参数 | 该传的地址空间 | 说明 |
|---|---|---|
| `HV_PCHK=start:end` | **run VA 的低 32 位**（源码比较 `(uint32_t)cpu->pc`） | 执行断点。默认仅 `cpu->index==0`、最多 400 次；`HV_PCHK_CPU=all\|N` 指定 CPU、`HV_PCHK_MAX=N` 改上限。**传 link VA 会差一个 slide，永不命中**；**命中数不能当调用次数**（§6.7） |
| `HV_PCHK_DST=start:end` / `HV_PCHK_LEN=hex` | run VA 低 32 位（`x22`）/ 长度阈值 | 在 `HV_PCHK` 之上的附加过滤 |
| `HV_TRACE_PC=<file>` | **文件路径**，内容为每块 **run VA**（u64 数组，上限 4M 块） | 全量执行轨迹。**判"某函数执行没执行"用它，别用 `HV_PCHK`** |
| `HV_BLOCK_DUMP=<file>` | 文件路径 | 每 `HV_BLOCK_DUMP_EVERY`（默认 64）块 dump 翻译后的宿主指令 |
| `HV_DT_DUMP=<file>` / `HV_DUMP_KC=1` / `HV_DUMP_RAMDISK=1` / `HV_EL0_DUMP=1` / `HV_SCAN_PROC=1` | 路径 / 开关 | 设备树 / 运行期 `__PRELINK_TEXT`+`__PRELINK_INFO` / ramdisk / EL0 故障页 / 进程名+内核日志 |
| `tools/disasm-raw.sh <blob> <base> <from> <to>` | **link VA**（`base=0xfffffe0007004000`） | 反汇编镜像 |

三个换算式子：

```
fileoff = link_va - 0xfffffe0007004000
run_va  = link_va + slide
HV_PCHK = (link_va + slide) & 0xffffffff      # 低 32 位，不是 link VA
```

**铁律：`HV_PCHK` 零命中不能推出"没执行"。** 先按上式换算，再用 `HV_TRACE_PC`
统计同一范围交叉验证（trace 记的是真执行）。实测对照：

| 探针 | 值 | 结果 |
|---|---|---|
| `HV_PCHK`（link） | `0x84eb0e0`（`InitIOKit`） | 0 次 ← **错，差 slide** |
| `HV_PCHK`（run） | `0x1cfff0e0` | **16 次** ✓ |
| `HV_TRACE_PC` 统计 | `__PRELINK_TEXT` `0x1bb20000..0x1c210000` | 0 次 = kext 真没执行（**当时那套配置下**；见下方 B 类说明） |

注意 `com.apple.kernel` 的 `vmaddr` 就是 `0xfffffe000700c000`（`__PRELINK_TEXT` 起点），
它属于 kernel fileset entry，所以"该段 0 块"等于"内核早期段没跑"，含义要单独判断。

**不要手写 ARM64 分支解码器**（`b.cond`/`cbz`/`bl` 的 imm 位域极易错，本项目已因此误判多轮）。
用 NDK 的 `llvm-objdump`：裸 binary 先用 `llvm-objcopy -I binary -O elf64-littleaarch64
--rename-section .data=.text,code` 包一层 ELF 再 `-d`；`--adjust-vma` 对裸 binary 的分支
目标显示不可靠，跳转目标按 `imm` 手算后再用 trace 核对。

**关于"kext 一个都不 start"**：那是**历史现象**（SIMD clobber 在位 + 一批 opt-in 介入
掩盖），不是当前状态。完整地址表与当时的排查过程见 `docs/kext-start-matching-2026-11-16.md`；
`d272f3a` 把介入全部默认关掉之后 kext 就正常 start 了。**分类见
`docs/investigation-tree.md` 的 §7.4（B 类：前提已变）。**

**串口日志（guest kprintf/IOLog）怎么开、怎么停、怎么取，见上面的 §5.0。**
完整配方与排错表见 `docs/kernel-log-2026-11-16.md`。日志行形如 `hengevm: serial: ...`。

## 6. 调试方法论（本项目最贵的教训）

1. **推导 ≠ 证据。** 讲清机制要有 XNU 源码/实测支撑；否则标"不确定"。
2. **先宿主，后设备。** 宿主几十秒一轮；设备走 **CLI** 时 1~3 分钟一轮、**无需人工**（只有走 APK 安装才需要人工点按）。
3. **不要猜规则直接补。** 先读源码：
   - XNU（tag `xnu-11417.140.69`，本地 `/tmp/xnu`）：`osfmk/arm/arm_init.c`、`osfmk/prng/prng_random.c`、`pexpert/gen/pe_gen.c`、`osfmk/arm64/{hv_hvc.h,arm64_hypercall.c,pac_asm.h,start.s,locore.s}`、`bsd/kern/bsd_init.c`
   - QEMU（本地 `/tmp/qemu`）：`hw/vmapple/*`（**设备模型对拍金矿**）、`hw/display/apple-gfx.m`、`target/arm/tcg/pauth_helper.c`（软件 PAC 唯一参照）
   - QEMU 的 vmapple 用 `-accel hvf`，**hypervisor 侧（PAC/HVC）没有开源参照**
4. **轮询而不是 sleep**：等进程退出用 `kill -0` 轮询；先 `pkill -x host-install` 并等它消失，
   否则会撞 `another installer is using work` 并污染证据。
5. **一次只改一个变量**，跑完立刻把结论写进 `docs/`。
6. **反汇编基线**：镜像 VA 基址 `0xfffffe001bb18000` ↔ PA `0x73b18000`，
   工具 `tools/disasm-raw.sh <blob> <base> [from] [to]`。PC 打印值即文件内偏移（工具按 base 载入）。

7. **`HV_PCHK` 的命中数不是调用次数。** 引擎有 chaining/superblock，只在“进入新 block”时
   检查断点，跳转落在链内部就**绕过检查点**。实测反例：EL0 同步向量入口那条指令是
   `b 0x1c8943e4`，入口命中 12253 次而目标只 241 次。**热路径会严重偏低。**
   要计数就用 `HV_TRACE_PC`（每块 8 字节，真实执行记录）或关掉 superblock 后再断。
   `HV_PCHK` 只适合作**存在性**证据（“这段代码确实跑过”）。

8. **`gdb` attach 时被调试进程是暂停的。** 在**同一次 attach 会话里**做多次采样，
   看到的是同一个冻结时刻 —— “多次采样值一致”证明不了“值不随时间变化”。
   要看变化，必须 detach → 等 → 再 attach。

9. **先读 `TCR` 判 granule，再算页表索引。** guest 是 **16 KiB 页、3 级页表**
   （`TCR.TG0 = 2`），按 4 KiB/4 级套公式会得到全错的索引。
   同理，引擎里按 4 KiB 做的诊断（如 `ptw` 的页匹配与采样）在 16 KiB 表上
   只能看到第一个 4 KiB 子页，后四分之三是**盲区**。

10. **多进程场景必须用“事件自带的地址空间”。** fault 环是全局的，
    `hv_last_el0_ttbr0` 只代表“最后一次”那个进程；两者混用会有一半匹配错。
    为此 `core/exception.c` 已记录 `hv_el0_flt_ttbr0[]`，每条 fault 自带 ttbr0。

11. **“被推翻”要分三种**：① 观察对、归因错；② **前提已变**（当时确实如此，
    底层修好/开关关掉后不再复现）；③ 真误判（读错数据、用错工具）。
    ② 必须保留并记下它的有效前提，否则以后重新打开某个开关时会把历史现象当“新问题”重查一遍。
12. **死锁状态下不能用“日志/报错缺失”做排除**：打印日志本身要拿锁、要线程、要写 I/O；
    内核判断不了状态时，返回码也可能不可信。唯一可靠的形态是线程/CPU 状态本身。
13. **CLI 不等于能显示**：宿主与设备 CLI 都编 `android/compat/pvg_metal_stub.c`，而且 CLI
    进程没有窗口系统/Surface 权限 —— 显示与进度 UI 只能用 APK 验证。
14. **字符串不按 8 字节对齐**，要按连续字节解（`ramrod-nvram-session` 按 qword 会读成 `rmarod`）。

## 7. 关键不变量（改这些会静默爆炸）

- **`HvCpu` 布局是 ABI**：`x[0..30]`@0..248、`spill[4]`@0x118、`sp`@0x0f8、`tpidr_el1`@0x370、
  `elr_el1`@0x378、`sp_el0`@0x388、`esr_el1`@0x3b0、`far_el1`@0x3b8、`CPU_REFILL_RET`@0x438。
  **新字段只能追加在末尾**（有 `_Static_assert` 兜）。
- **寄存器分配**：`is_free={6,7,15}`；`is_stolen={18,28}∪is_free`；`candidates[]` **不含 16/17**
  （宿主 x16 保存 guest x16 到 slot，借用会污染 guest x16）。
- **PAC 模型**：签名 no-op；`AUT*`/`XPAC*` = 把 bits[62:48] 置成 bit55 的符号扩展
  （`BFM #16,#14`；`#16,#6` 只清半段 → 权限异常风暴）。bit63 未处理，属已知缺口。
- **内核镜像 rebase**：**默认不补** —— 内核自己会走完 fixup，`HV_NO_KFIX=1`（完全不补）与补过的
  两种情形实测行为完全一致（`docs/kext-start-matching-2026-11-16.md` 排除项 #3）。补 rebase 现为
  对照用开关 `HV_KFIX=1`；打开时 `__DATA_CONST` 必须保持不补（补两次得到 `base+pointer`）。
- **HVC 成功判据是 `x0` 非负**；PAC 键的汇编是 `cbnz x0, .`（失败即静默自旋）。
  OEM 调用要求 UID 等于 `3B878185-AA62-4E1F-9DC9-D6799CBB6EBB`、REVISION major=1，
  否则内核放弃全部 OEM 调用。
- guest 代码不能碰宿主 libc：`-fno-stack-protector` + `--wrap=memcpy/memset/memmove/malloc/free`。
- 汇编入口一律 `.hidden`。

## 8. 交互与边界

- 用户操作设备（点"安装"等）**只由用户做**；agent 可装 APK、拉文件、读日志、看 logcat/uiautomator。
- **不做截图**；用 logcat / `uiautomator dump` / pull 文件验证。
- **不频繁提交**：只在结论确定或修复被验证有效时提交到本地 `dev`。
- 破坏性操作（删数据、`pm clear`、卸载）一律先问；第三方应用只读。
- 明确写清"我要做什么 / 你要做什么"。

## 9. 文档入口（先读，别重复造）

| 文档 | 内容 |
|---|---|
| **`docs/investigation-tree.md`** | **排查总索引**：走过的分支、站得住的结论、被推翻的结论（含"前提已变"类）、方法论铁律。**接手先看这份** |
| `docs/architecture.md` | 引擎结构 |
| `docs/kernel-boot-exclusions.md` | 内核启动已排除的假设 E1–E18 + H1/H2 + 工具清单（**排障先看这里，避免重走**） |
| `docs/semantic-deviations.md` | 与上游的语义等效性审计（E/X/D/U 四级） |
| `docs/android-install-debug.md` | Android 安装链路调试（设备端） |
| `docs/android-install-status.md` | 安装现状 |
| `docs/android-ui-plan.md` / `android-ui-design.md` | UI 结构与主题 |
| `docs/asm-feature-floor.md` | 指令集下限与运行期降级 |
| `docs/debug-switches.md` | **全部环境变量开关总表**（日志/引导/断点/用户态/USB/设备/JIT/安装），含默认值与最短排障路径 |

**新增开关必须同步维护 `docs/debug-switches.md`。** 加一个 `getenv` 就往对应分节补一行
（名称 / 取值 / 默认 / 作用）；凡是会改变 guest 所见的开关，默认必须是关的，并在表里写明。
这份表是「有哪些开关」的唯一出处，不要在别处另开清单造成两份不一致。

## 10. 当前状态（2026-11-17，随时会变，以最新提交与 docs/ 为准）

### 10.1 主目标与优先级

**主目标：把“安装 macOS”（DFU + idevicerestore）跑通。** 其余一切让路。

**短时间内不碰 UI。** 原因（用户明确）：安装流程没走完，UI 没有可展示的真实状态；
**只有安装流程走完之后才会碰 UI**。届时若需要用 `gdb` 推送 / `run-as` 之类手段调试，
那**只适用于调试**；真正控制 `.so` 的行为要走别的方案（不要在 UI 里依赖调试通道）。

### 10.2 当前卡点（唯一一个）

引导链、kext、IOKit、ramdisk、launchd bootstrap 全部通过，`restored_external` 已经真正在跑
（19 万+ 块、2.8 万 syscall）。**缺页这条线已经证明是正常的**（PTE 100% 建立并在推进），
旧的"PTE 恒为 0"是采样方式的问题。现在的现象是：

> **没有崩溃**。执行到一半之后各处**陆续卡住、累积成多重死锁**，最后内核已无法判断自身状态，
> 因此既给不出真实内容，也给不出有效报错。

形态（多次实测）：**所有线程 `WAIT`、两个 CPU 都进 `WFI`（98% idle）、无 panic、无异常退出。**

**由此得到一条硬约束**：死锁状态下，**日志缺失 / 报错缺失 / 返回码异常都是“结果”而不是
“原因”，不能用来排除任何路径**。

已经落到的最具体位置（**待验证假设，不是结论**）：

- `restored_external` 的串口 CHECKPOINT 停在 `BEGIN: MAIN:[0x0406] set_progress_0`（无 END）；
  `0x0400 umask`、`0x0402 setvbuf` 都正常返回；
- 该时刻 `restored_externa` / `launchd` / `kernelmanagerd` 各只有一个线程，全部 `WAIT`
  （但线程扫描只映射到 3 个线程，覆盖不足）；
- 业务等待是 **mach IPC**：`-47 = mach_msg2_trap`（mach trap 表，不在 BSD syscall 表），
  `MACH_RCV_MSG`、local port `0x507`、id `2989`；`kevent_id` 只是 libdispatch 调度层；
- body 里解出 `ramrod-nvram-session`（`ramrod` = Apple 恢复工具，位于恢复 ramdisk）与
  `0xe00002f0`（`kIOReturnNotFound`）。

**下一步（唯一可信方向）**：不再从日志/返回码反推，直接画死锁现场 —— 把线程扫描扩到
**所有 task（含 `kernel_task`）的全部线程**，记录每个线程的 `wait_event` / `waitq` /
`turnstile` / 持有锁 / 内核栈，拼等待图，两次采样对比，区分"存在环"与"事件源消失"。

完整事实与过程见 `docs/lost-wakeup-2026-11-19.md` §7。

### 10.3 已验证可用的工具

| 工具 | 用途 |
|---|---|
| `tools/lcfs.py` | 从 `LC_FUNCTION_STARTS` 建函数索引（220 image / 149 938 函数），内核主代码没有符号时唯一可用 |
| `tools/ksym.py` | kernelcache 符号解析（181 781 符号，只覆盖 kext） |
| `hv_el0_flt_*` / `hv_el0_flt_ttbr0[]` | EL0 故障完整寄存器环 + 地址空间 |
| `hv_el0_trace_*` / `hv_svc_*` | 用户态块轨迹 / syscall 参数与返回值（含 `mach_msg2_trap` 的 header/body 解码） |
| `hv_threads_dump()` | 引擎内线程表 dump（不需要 gdb）；`HENGE_STOP_ON` 命中时会自动调用 |
| `HENGE_STOP_ON` + 现场保护 | 串口同步断点，停住后机器不拆 |

### 10.4 其它未解决

- **25G83**：iBoot Stage1 打印 `Memory image not valid` 后卡住（固件拒绝镜像；Stage2 都没到）。
  不要在它上面调内核；跑它会产出 200+ MB 日志。
- **UI**：`InstallTab` 的状态应彻底收敛为单一 `Pipeline.State`（版本号 + 单一渲染函数）。
  属于“安装流程走完之后”的工作，现在不动。

### 10.5 已修好（历史）

`hv_enter` 丢失、强制 `slow_mem`（1000→200 万 block/s）、`__DATA` 漏 rebase（**曾经必修；后来实测内核自己会走完 fixup，现改为默认不补，见 §7**）、
PAC strip 宽度、OEM/PV-lock hypercall、UI 状态失联若干、按文件记录校验、
SIMD clobber（`put_helper`/`refill` 未保存 v0-v31）、缺 1 ms kicker（irq 0→27 万）、
磁盘 fd 被提前关闭。

## 11. 设备上跑 CLI（首选调试方式，1 分钟一轮）

内核/引擎的调参、探针、对照实验**不要走 APK**。把安装器编译成 Android CLI，推到
`/data/local/tmp` 直接跑：bionic、真机 CPU、**宿主 4 KB 页**（guest 仍是 16 KiB 页，见 §6.9），
**并且没有 proot 的 ptrace 干扰**（glibc 宿主上出现的 SIGSEGV 在 bionic 上不出现，实测过）。

```bash
# 1. 编译（约 1 分钟）。API 28 起才有 aligned_alloc / pthread_gettid_np；
#    ZLIB 必须有，libhengerestore 用 gzread。
ZLIB=1 API=28 ./tools/build-install-android.sh

# 2. 推送。设备是**真机**（PJE110 / SM8550 / Android 15），通过
#    adb connect 127.0.0.1:5555 连上。adb 的代码里只要连到 5555 端口，就按约定
#    额外登记一条名为 emulator-5554 的 transport（并虚拟出控制台端口 5554），
#    所以 adb devices 会同时列出 emulator-5554 和 127.0.0.1:5555 ——
#    同一台机器、同一个连接，**名字带 emulator 不代表它是模拟器**。
adb -s emulator-5554 push build/andcli/host-install /data/local/tmp/henge/

# 3. 跑（参数顺序：booter ipsw 工作目录 RAM_MB log）
adb -s emulator-5554 shell "cd /data/local/tmp/henge && chmod 755 host-install && \
  ./host-install AVPBooter.vmapple2.bin /data/local/tmp/ipsw/24G90.ipsw workN 2048 2 > run.log 2>&1"
```

要点：

- **`adb shell "... &"` 里那个 `&` 是设备 shell 的后台，不是本地后台。**
  同一条 `nohup sleep 5`，实测三种写法：

  | 写法 | 本地耗时 | 机制 |
  |---|---|---|
  | `adb shell "nohup sleep 5 &"` | **5.33 s** | 远端 shell 把作业放后台，但子进程仍持有 adb 的 stdout/stderr，adb 客户端等 socket EOF → 阻塞到进程退出 |
  | `adb shell "nohup sleep 5 >/dev/null 2>&1 &"` | **0.31 s** | fd 全部重定向走，子进程不再持有 socket，adb 立刻返回 |
  | `adb shell "nohup sleep 5" &` | 立即返回，1 s 后 `pgrep -f` 仍能看到 sleep | `&` 在**引号外** = 本地 bash 挂后台 adb 客户端，设备进程照跑 |

  两种有效做法：**`&` 放引号外**（推荐，长跑时本地不阻塞），或**引号内 `&` + 重定向全部 fd**。
  只加引号内 `&` 而不重定向是本项目踩过的坑：看起来“挂后台”，本地那条命令其实一直在前台等；
  工具调用超时/中断后 adb 客户端被杀，设备进程成了孤儿（可能还在跑，也可能被系统回收）。
  **判断进程是否真在跑，只能用一条新的 adb shell 里 `pidof host-install`**，不能拿“命令返回了”当证据。
  实测补充：引号外 `&` 时 adb 客户端本身会活到远端命令结束（只是本地 shell 不阻塞）；
  `kill -9` 掉本地 adb 客户端后**设备进程仍继续跑**（成孤儿，只是没人收输出）。
  所以设备上有没有东西在跑，与本地那条命令是否被中断无关。
- **RAM 用 `2048`，且一次只开一个实例。** 设备总内存约 15 GB，单个实例实测 RSS ≈ 2.1 GB；
  以前同时留两个（一个“现场”+ 一个新实验）就占了 4 GB+。**分析完立刻 `kill -9`，不要留“现场”**
  （分析数据落进 docs 就够了）。开了新的先确认旧的已消失。
- **每次换一个新的 `workN` 目录**。同一个目录被占用时会直接退出并打印
  `another installer is using workN`。用完的目录记得删（`aux.img` 30 MB、`dc-*.bin` 各 6 MB）。
- **参数少一个会静默跑错**：给 `workN 3` 会把 3 当成 RAM=3 MB，机器建不起来但不报错。
- IPSW 已放在设备上：`/data/local/tmp/ipsw/24G90.ipsw`（16 GB，别重复推）。
- booter 必须一起推：`build/andcli/host-install` 只带代码，booter 是独立文件。
- 依赖（只需一次，已备好）：
  - `work/andcli/curl/lib/libcurl.a` —— curl 8.23.0 静态，`--with-openssl`，无其他依赖
  - `work/andcli/ossl/lib/libssl.a|libcrypto.a` —— openssl 3.0.15，`android-arm64` 配置
  - 三者都是用 NDK `aarch64-linux-android26-clang` 编的 bionic 库；**宿主 glibc 的 openssl
    拿来链会报 `__errno` / `__FD_SET_chk` 未定义**，别混用
- 日志直接 `adb shell grep`，不用 `run-as`（`/data/local/tmp` 对 shell 可读）。

设备侧常用环境变量同第 5 节；`HV_LOG=3` 会产出千万行级日志，只在定位到具体 pc 时短跑。
