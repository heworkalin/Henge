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
  引导链：`AVPBooter → iBoot Stage1 → Stage2(iBEC) → ramdisk + device tree + kernelcache → XNU → restored`
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
| `tools/` | 宿主侧工具：`build-install-host.sh`、`run-install-host.sh`、`host-install.c`、`disasm-raw.sh` |
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
  ./build/host/host-install work/AVPBooter.vmapple2.bin android/ipsw/24G90.ipsw work 4096 2
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

- 设备：`emulator-5554`（PJE110 / SM8550 / Adreno 740 / Android 15 / 4 KB 页）
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

### 5.1 JIT 调试参数与地址空间（最易出错，先看这里）

同一份内核镜像有**四个**地址，参数必须传对的那一个：

| 名字 | 定义 | 24G90 实测（slide 固定） |
|---|---|---|
| `fileoff` | 镜像内文件偏移 | `__TEXT_EXEC` = `0xd78000` |
| **link VA** | `fileoff + 0xfffffe0007004000` | `__TEXT_EXEC` = `0xfffffe0007d7c000` |
| **run VA** | `link VA + slide`（slide = `0x14b14000`，`HENGE_RANDOM_SEED` 关时固定） | `__TEXT_EXEC` = `0xfffffe001c890000` |
| PA | `kimage_pa + fileoff`（`kimage_pa = 0x73b18000`） | `__TEXT_EXEC` = `0x74090000` |

`slide` 可由 boot_args 反推：`virtBase 0xfffffe0018a04000 − physBase 0x70a04000 = 0x14b14000`。

| 参数 | 该传的地址空间 | 说明 |
|---|---|---|
| `HV_PCHK=start:end` | **run VA 的低 32 位**（源码比较 `(uint32_t)cpu->pc`） | 执行断点；仅 `cpu->index==0`、最多 400 次。**传 link VA 会差一个 slide，永不命中** |
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
| `HV_TRACE_PC` 统计 | `__PRELINK_TEXT` `0x1bb20000..0x1c210000` | 0 次 = kext 真没执行 |

注意 `com.apple.kernel` 的 `vmaddr` 就是 `0xfffffe000700c000`（`__PRELINK_TEXT` 起点），
它属于 kernel fileset entry，所以"该段 0 块"等于"内核早期段没跑"，含义要单独判断。

**不要手写 ARM64 分支解码器**（`b.cond`/`cbz`/`bl` 的 imm 位域极易错，本项目已因此误判多轮）。
用 NDK 的 `llvm-objdump`：裸 binary 先用 `llvm-objcopy -I binary -O elf64-littleaarch64
--rename-section .data=.text,code` 包一层 ELF 再 `-d`；`--adjust-vma` 对裸 binary 的分支
目标显示不可靠，跳转目标按 `imm` 手算后再用 trace 核对。

**kext 一个都不 start 的排查现状与完整地址表见 `docs/kext-start-matching-2026-11-16.md`**
（已排除项、已确认执行链路、复现命令）。

**内核日志（kprintf/IOLog）默认全部丢弃**，要看到必须同时给
`HV_EXTRA_BOOT_ARGS="serial=3 debug=0xA"`（`serial=3` 触发 `switch_to_serial_console`，
`debug` 的 `DB_PRT=0x2`/`DB_KPRT=0x8` 打开 `disable_serial_output`）。
注入点在 `core/dispatch.c` 的 `boot_args->CommandLine`（PA `0x837f0000`，偏移 `0x6c`），
**注入必须写在长度修补的 if/else 之外**，否则会被跳过。完整配方与排错表见
`docs/kernel-log-2026-11-16.md`。日志行形如 `hengevm: serial: ...`。

## 6. 调试方法论（本项目最贵的教训）

1. **推导 ≠ 证据。** 讲清机制要有 XNU 源码/实测支撑；否则标"不确定"。
2. **先本地，后设备。** 宿主几十秒一轮；设备一轮要等人工点按。
3. **不要猜规则直接补。** 先读源码：
   - XNU（tag `xnu-11417.140.69`，本地 `/tmp/xnu`）：`osfmk/arm/arm_init.c`、`osfmk/prng/prng_random.c`、`pexpert/gen/pe_gen.c`、`osfmk/arm64/{hv_hvc.h,arm64_hypercall.c,pac_asm.h,start.s,locore.s}`、`bsd/kern/bsd_init.c`
   - QEMU（本地 `/tmp/qemu`）：`hw/vmapple/*`（**设备模型对拍金矿**）、`hw/display/apple-gfx.m`、`target/arm/tcg/pauth_helper.c`（软件 PAC 唯一参照）
   - QEMU 的 vmapple 用 `-accel hvf`，**hypervisor 侧（PAC/HVC）没有开源参照**
4. **轮询而不是 sleep**：等进程退出用 `kill -0` 轮询；先 `pkill -x host-install` 并等它消失，
   否则会撞 `another installer is using work` 并污染证据。
5. **一次只改一个变量**，跑完立刻把结论写进 `docs/`。
6. **反汇编基线**：镜像 VA 基址 `0xfffffe001bb18000` ↔ PA `0x73b18000`，
   工具 `tools/disasm-raw.sh <blob> <base> [from] [to]`。PC 打印值即文件内偏移（工具按 base 载入）。

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

## 10. 当前状态（2026-11，随时会变，以最新提交与 docs/ 为准）

**已修好**：`hv_enter` 丢失、强制 `slow_mem`（1000→200 万 block/s）、`__DATA` 漏 rebase、
PAC strip 宽度、OEM/PV-lock hypercall、UI 状态失联若干、按文件记录校验。

**未解决**（按版本）：

- **24G90**：iBoot 全通，内核跑进 `arm_init` 后，guest 自己的页表把内核栈页标成只读
  （`ap=2`，`code_page=0`，非引擎误伤）→ 处理异常时写栈 → 嵌套 → `halt x2 "MACH Reboot"`，
  console（PL011）从未初始化。下一步：异常嵌套环形记录，找**第一个**异常。
- **25G83**：iBoot Stage1 打印 `Memory image not valid` 后卡住（固件拒绝镜像；Stage2 都没到）。
  不要在它上面调内核；跑它会产出 200+ MB 日志。
- UI：`InstallTab` 的状态应彻底收敛为单一 `Pipeline.State`（版本号 + 单一渲染函数），
  现在虽已用静态快照+广播收口，但仍是多 writer 结构。

## 11. 设备上跑 CLI（首选调试方式，1 分钟一轮）

内核/引擎的调参、探针、对照实验**不要走 APK**。把安装器编译成 Android CLI，推到
`/data/local/tmp` 直接跑：bionic、真机 CPU、4 KB 页，**并且没有 proot 的 ptrace 干扰**
（glibc 宿主上出现的 SIGSEGV 在 bionic 上不出现，实测过）。

```bash
# 1. 编译（约 1 分钟）。API 28 起才有 aligned_alloc / pthread_gettid_np；
#    ZLIB 必须有，libhengerestore 用 gzread。
ZLIB=1 API=28 ./tools/build-install-android.sh

# 2. 推送
adb -s 127.0.0.1:5555 push build/andcli/host-install /data/local/tmp/henge/

# 3. 跑（参数顺序：booter ipsw 工作目录 RAM_MB log）
adb shell "cd /data/local/tmp/henge && chmod 755 host-install && \
  ./host-install AVPBooter.vmapple2.bin /data/local/tmp/ipsw/24G90.ipsw workN 4096 2 > run.log 2>&1"
```

要点：

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
