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
| `HV_NO_KFIX=1` / `HV_KFIX_DC=1` | 关掉整段 rebase / 强制补 `__DATA_CONST` |
| `HV_BLOCKTIME=1` | 单块耗时采样（默认关，因为每块两次读计数器） |
| `HV_STACKSCAN=1` | 卡住时 dump 内核栈 |
| `HV_PTW_LOG` / `HV_PTW_SNAP` / `HV_DUMP_DC` / `HV_BLOCK_DUMP` | 页表写历史 / 段 dump |
| `HENGE_RANDOM_SEED=1` | 用宿主熵填 `/chosen/random-seed`（**默认关**，关才可复现） |
| `HENGE_TIMEOUT_SCALE` | 放大 restore 的所有等待 |

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
- **内核镜像 rebase**：`__DATA` **必须我们补**（`arm_init` 前半段在 `kernel_collection_slide` 之前就读它）；
  `__DATA_CONST` **绝不能补**（内核自己会补，补两次得到 `base+pointer`）。
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
