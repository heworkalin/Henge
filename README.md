<p align="center">
  <img src="docs/icon.png" width="120" height="120" alt="Henge">
</p>

<h1 align="center">Henge — Android 移植版</h1>

<p align="center">在你自己的 Android 手机上跑 macOS。</p>

<p align="center">
  <a href="#安装">安装</a>
  ·
  <a href="#我们做了什么">我们做了什么</a>
  ·
  <a href="#自己编译">自己编译</a>
  ·
  <a href="docs/architecture.md">引擎结构</a>
  ·
  <a href="THIRD_PARTY.md">引入的上游</a>
  ·
  <a href="README_en.md">English</a>
</p>

<p align="center">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-GPL--2.0--or--later-2E6BFF?style=flat-square&labelColor=111111" alt="GPL-2.0-or-later"></a>
</p>

> **本仓库不是上游。**
> 上游是 [Leviidev/Henge](https://github.com/Leviidev/Henge)（作者 Leviidev），它做的是 **iPhone / iPad** 版。
> 本仓库在 `dev` 分支上，把同一套引擎移植到 **Android arm64**，并继续做内核启动与 GPU 渲染的调试。
> 上游的 `main` 是 iOS 线；我们的工作在 `dev`。

---

## 先说清楚测试范围（重要）

**这套 Android 移植只在一台设备上实测过：**

| 项 | 实测设备 |
|---|---|
| 型号 / 板 | **PJE110（OnePlus）** / `kalama`（**SM8550**） |
| GPU | **Adreno 740** |
| 系统 | **Android 15** |
| 内核 / 页面 | 宿主 **4 KB 页**（guest 仍是 **16 KiB 页**） |

在这台设备上，**安装流程（DFU + idevicerestore）跑通了**：

```
install: restore finished: 0
step 100 (100%)
disk.img  68,719,476,736 逻辑字节（64 GiB sparse，实际约 19.8 GB）
```

**但这不代表其他设备可以用。** 这不是免责声明式的套话，而是有具体原因的：

- **GPU 路线很窄。** 图形链路是 `PVG 设备模型 → reims-vgpu → metal2vulkan → Vulkan → SurfaceView`。
  其中一环要求 `VK_EXT_external_memory_host` + dma_buf；在本机实测，
  **厂商标定 Adreno 驱动直接 fatal**（缺 `external_memory_host`），**只能走 Turnip**。
- **页面大小是个硬约束。** 宿主 4 KB 页、guest 16 KiB 页这套组合是实测调通的；
  有些诊断（如页表写历史）在别的页大小下会失真。
- **能力探测是运行期做的，不是编译期假设的。** 但探测不过的地方只会退化或停下，不会"自己适配"。
- **每次安装会创建一个 64 GiB 的镜像**，设备存储不够就走不完。
- **只测过 AArch64**。`armeabi-v7a` 目标在构建脚本里，但没有任何实测。

**结论：请把本仓库当作"一台机器上验证过的移植实验"，而不是"通用 Android 版本"。**

在别的设备上遇到问题，**最可能的原因是 JIT 翻译器与那颗芯片的指令集不兼容**，而不是 GPU。
"同样是 arm64"并 **不等于** 能跑出同样的效果：

- JIT 直接生成本机指令，它假设的指令集下限、**PAC / 非规范地址细节**、缓存维护与
  自修改代码的可观测行为、SIMD 行为、sysreg 可见性，在不同 SoC 上并不一致；
- 这些差异只有在**具体芯片上实测**才会暴露 —— 写成特例处理还是绕开，要一台台扫、一台台适配；
- 本仓库目前的 JIT 路径只针对 **SM8550（Cortex-X3/A715/A510 组合）** 验证过；
  换芯片需要**重新测试并调整适配**，不能假定同构。

GPU 驱动能力与页面大小确实有限制，但那是 **我们在这台机器上把安装跑通之后**才能遇到的下一层问题，
不是"别人跑不起来"的首选解释。层次要分清：

```
JIT 指令集兼容  →  引擎能不能跑    （先决条件，最可能出问题）
GPU / 页大小    →  画面能不能对    （跑通之后才轮到）
```

---

## 我们做了什么

起点：上游 iOS 版能跑（iPhone 17 Pro 上 macOS 约 15 秒到锁屏）。
移植从 **2026-11-13** 开始，第一个提交是 `bb36f6f` —— *Android port: engine builds and runs, install stack, UI, driver loading*。

### 1. 引擎侧：三处移植几乎没改

JIT 翻译器、软件 MMU、设备模型本身是**纯用户态 C**，和平台无关：

| 模块 | 移植改动 |
|---|---|
| `core/`（JIT / dispatcher / 异常 / MMU） | 基本不动，只修了 ABI 层面的坑 |
| `devices/`（GICv3 / PL011 / PL031 / PL061 / AES / cfg / bdif / xHCI / PVG / virtio） | 不动 |
| `host/`（线程 / kicker / WFI / 磁盘 / usb shim） | 平台相关部分替换 |

真正需要新写的只有宿主层与入口（`android/install.c`、`android/henge_vm.c`、`android/probe_jni.c`）。

### 2. 移植期发现的三个真问题（都不是"平台不兼容"）

- **`x16` 被宿主 GOT 地址污染**（提交 `27ab79c`）。`x16` 既是 guest 寄存器又是 AArch64 的 IP0；
  引擎里 C 代码取汇编入口地址时走 GOT/PLT，那条序列用 `x16` 当临时寄存器，
  于是宿主 `.got.plt` 地址留在了 guest 的权威槽位。
  **修法**：所有 JIT 用到的汇编符号加 `.hidden`，删掉唯一取 `hv_helper_entry` 地址的死字段。
  **效果**：安装从"iBSS 一发就崩"推进到内核阶段。回归测试：`android/test-regs.sh`（35 项，宿主侧秒级）。
- **SIMD clobber**：`put_helper` / `refill` 没保存 `v0-v31`，内核一用 SIMD 就把 JIT 状态踩掉。
- **时间基准错误**（提交 `4720d2e`）。`counter()` 过去直接返回宿主 `CLOCK_MONOTONIC_RAW`
  缩放值，即**从宿主开机起算**，而 guest 的 commpage offset 读到 0；于是 guest uptime 溢出成负、
  时间漂到 2087。**Apple 的个性化恢复材料带时间窗口**，时间不符即验证不通过。
  改成"第一次调用锁定基准、之后返回差值"后，恢复从卡在 `set_progress_0` 一路推进到 **ASR 90%**，
  并最终走完 → `restore finished: 0`。

> 这里最容易走偏的一条：**时间在 Apple 生态里不是显示问题，是签名的前置条件。**
> 这跟 Linux 里"时间偏一点只是日志难看"完全不同。

### 3. 引入 / 移植的组件

**完整清单（含每个上游的仓库链接、许可、我们是否改过）见 [`THIRD_PARTY.md`](THIRD_PARTY.md)。** 概要：

`third_party/` 下（恢复协议栈）：

| 组件 | 用途 |
|---|---|
| `idevicerestore` / `libimobiledevice` / `libirecovery` / `libplist` / `libimobiledevice-glue` / `libtatsu` / `libusbmuxd` | 走 Apple 的恢复协议（DFU / recovery / restore） |
| `usbmuxd` + `libusb-1.0` | 与 guest 里的虚拟 USB 栈对接（`host/libusb_shim.c`） |
| `mbedtls` | idevicerestore 的 TLS |
| `libzip` + `lzfse` | IPSW 解包与 LZFSE 解压 |
| `curl` | 从 Apple 下载 IPSW |
| `restore_config` | restore 用的配置材料 |

`android/third_party/` 下（图形链路）：

| 组件 | 用途 |
|---|---|
| `reims-vgpu` | PVG 设备模型的 Rust 实现（Apple 的半虚拟 GPU 协议） |
| `metal2vulkan-runtime` (m2v) | Metal 命令 → Vulkan 的翻译层 |
| `libadrenotools` | 命名空间加载 Vulkan 驱动（Turnip），绕开系统驱动的限制 |
| `lzfse` | 同上，供 Android 侧使用 |

Vulkan 驱动本身（**Turnip 1.3.296**）是**下载的、不入库**；`PROVENANCE.txt` 记录它是什么版本。

> **注意**：`android/third_party/reims-vgpu` 与 `libadrenotools` 是 submodule。clone 后要
> `git submodule update --init --recursive` 才会填充；或者跑 `tools/clone-deps.sh`。

### 4. Android 应用

`android/app/` 是一个完整的原生 Android 应用（Java + Material，`dev.henge.probe`）：

- **Install 标签页**：下载 / 导入 IPSW，校验，跑安装（前台服务 + 通知）
- **Macs 标签页**：启动已安装的 macOS
- **DisplayActivity**：`SurfaceView` 承接 guest 画面，输入转发（触摸 = 触控板语义）
- **Settings / Logs / Diagnostics**：日志级别、驱动选择、诊断信息
- **驱动加载**：可选 Turnip（libadrenotools 命名空间）或系统驱动

### 5. 日志架构（刻意设计，不要绕过）

引擎 stderr → 日志服务线程 → **①全量写文件 `files/vm/henge.log`**（GB 级）+ **②logcat 只发限速样本**
（`HENGE_LOG_RATE`，默认 120 行/秒；关键词行不限速）。

**不要在引擎里直接打高频 logcat**：Android 对进程有 logcat 配额，刷爆会让关键行丢失 ——
这个坑踩过，看起来像"某段代码没执行"，实际是日志被吞了。

### 6. 当前进度与卡点

| 环节 | 状态 |
|---|---|
| 引擎编译 / 运行（arm64、bionic） | ✅ |
| 安装 macOS（DFU + idevicerestore） | ✅ `restore finished: 0`，`step 100 (100%)` |
| 已安装系统启动到锁屏 / 登录 | ✅ |
| 串口内核日志（`serial=3`） | ✅ |
| 呈现链路到 Surface（swapchain、`host presents` 0 → 109） | ✅ |
| **画面内容正确** | ⚠️ **未完**。当前有若干层合成错误（背景被画成 16×4 的砖块、图集纹理被按线性读等），见 `docs/gpu-layer-corruption-2026-11-22.md` |

**主目标是"已安装的 macOS 启动到能正确渲染画面"。** 其余一切让路；
UI 在安装流程走完之前不做（当前 UI 只用于驱动安装与观察）。

---

## 安装

> 目前需要自己编译 APK（见下一节）。没有发布包。
> 一台设备只放一个实例：单个实例 RSS ≈ 2.1 GB，安装过程还会写一个 64 GiB 的 sparse 镜像。

设备侧的准备：

1. **存储**：至少 25 GB 可用（IPSW 约 16 GB + sparse 磁盘实际占用约 20 GB）。
2. **IPSW**：从 Apple 下载的 `*.ipsw`（实测 `24G90`）。
3. **booter**：`AVPBooter.vmapple2.bin`（跟 APK 一起编译产生）。

然后：装 APK → 打开 → **Install** 标签页选 IPSW → 开始安装 → 等 `step 100 (100%)` → 回 **Macs** 启动。

---

## 自己编译

### 环境（本机实测值，供参照）

| 依赖 | 版本 |
|---|---|
| Android NDK | **r29**（`29.0.14206865`） |
| 宿主 | **linux-arm64**（原生，不走 x86_64 模拟） |
| JDK | 21 |
| Gradle / AGP | 8.13 / 8.13.2 |
| Rust（`reims-vgpu`） | 1.98.1，target `aarch64-linux-android` |
| 其他 | cmake 3.28 / ninja 1.11 / make 4.3 |

目标默认：ABI `arm64-v8a`、API 33（`minSdk 26`、`compileSdk 36`、`targetSdk 36`）。

### 三条命令

```bash
cd /home/he/Henge            # 仓库根目录

tools/clone-deps.sh          # 可选：不用 submodule 时用它填两个子模块目录

source android/env.sh        # NDK + 两个工具链的环境变量
./android/build-restore.sh   # idevicerestore / libimobiledevice（很少需要重编）
./android/build-c.sh         # 引擎 + JNI → libhenge.so
./android/build-apk.sh       # 打包 APK
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
adb shell am force-stop dev.henge.probe   # 装完必须重启 App
```

`android/README.md` 有更详细的环境说明，`docs/debug-switches.md` 是**全部环境变量开关的总表**
（日志 / 引导 / 断点 / 用户态 / USB / 设备 / JIT / 安装）。

### 不用设备也能跑（推荐先走这条）

宿主侧 CLI 几十秒一轮，不需要手机：

```bash
cd /home/he/Henge

./tools/build-install-host.sh
CURL_CA_BUNDLE=$PWD/work/ca-bundle.pem \
  ./build/host/host-install work/AVPBooter.vmapple2.bin android/ipsw/24G90.ipsw work 2048 2
```

**但宿主与设备有实质差异，结论只在宿主成立时必须说明：**

- **PVG 是 stub**（真图形只在 `android/`）
- RAM 与 CPU 数不同
- 宿主是 **4 KB 页**（guest 仍是 16 KiB 页）

### 设备上跑 CLI（1~3 分钟一轮，无需人工）

```bash
cd /home/he/Henge

ZLIB=1 API=28 ./tools/build-install-android.sh

adb -s <serial> push build/andcli/host-install /data/local/tmp/henge/
adb -s <serial> push work/AVPBooter.vmapple2.bin /data/local/tmp/henge/

adb -s <serial> shell "cd /data/local/tmp/henge && chmod 755 host-install && \
  ./host-install AVPBooter.vmapple2.bin /data/local/tmp/ipsw/24G90.ipsw workN 2048 2 > run.log 2>&1"
```

参数顺序：`booter ipsw 工作目录 RAM_MB 日志级别`。**少一个参数会静默跑错**
（`workN 3` 会把 3 当 RAM=3 MB，机器建不起来但不报错）。**每次换一个新的 `workN` 目录。**

### 本地单元测试（秒级，改 JIT 前后必跑）

```bash
cd /home/he/Henge

./android/test-regs.sh
./android/test-bitops.sh     # ONLY=bitops|memops|mathops|atomics  ROUNDS=N
./android/test-lzfse.sh
```

---

## 已知限制

- **只在 PJE110 / SM8550 / Adreno 740 / Android 15 上实测过**，其他设备不保证。
- **图形内容目前有合成错误**：背景、图集纹理等层还没画对（见上表）。
- **GPU 需要 Turnip**：厂商标定驱动缺 `VK_EXT_external_memory_host`，在本机上直接 fatal。
- **只支持 arm64**，`minSdk 26`。
- **一次只跑一个实例**，每轮换新的工作目录。
- **安装只跑一次**：成功后的 `disk.img`（64 GiB 逻辑 / ~20 GB 实际）是资产，不要删、不要重跑安装去覆盖它。
- **宿主 4 KB 页 vs guest 16 KiB 页**：按 4 KB / 4 级页表套公式算 guest 页表索引会全错。
- 这是软件模拟，比真 Mac 慢得多。

---

## 文档（接手先读）

| 文档 | 内容 |
|---|---|
| `docs/investigation-tree.md` | **排查总索引**：走过的分支、站得住的结论、被推翻的结论、方法论铁律 |
| `docs/exclusions-current-stall-2026-11-19.md` | 当前卡点的排除清单（43 项已排除） |
| `docs/gpu-render-bringup-2026-11-21.md` | GPU 渲染链路打通记录 |
| `docs/gpu-layer-corruption-2026-11-22.md` | 当前画面合成错误的定位 |
| `docs/kernel-boot-exclusions.md` | 内核启动已排除的假设 E1–E18 |
| `docs/debug-switches.md` | **全部环境变量开关总表** |
| `docs/android-install-debug.md` / `android-install-status.md` | Android 安装链路调试与状态 |
| `docs/architecture.md` | 引擎结构 |
| **`THIRD_PARTY.md`** | **引入的上游清单**：每个组件的仓库链接、形态、许可、我们是否改过 |
| `docs/licensing.md` | 许可与组合作品判断的完整说明 |

**新增环境变量（`getenv`）必须同步补进 `docs/debug-switches.md`**，那是"有哪些开关"的唯一出处。

---

## 许可

**GPL-2.0-or-later**，与上游一致。Henge 编译进 usbmuxd（GPLv2 或 GPLv3）与 Husk 的代码
（GPL-2.0-or-later），因此发行的二进制是合并的 GPL 作品，**不能上架 App Store**。

macOS 与 Mac 的 boot ROM 是 Apple 的，不属于本仓库；安装时从 Apple 下载。
各上游组件的版权归各自作者，**本仓库不重新授权**它们 —— 见 [`THIRD_PARTY.md`](THIRD_PARTY.md) 与
[`docs/licensing.md`](docs/licensing.md)。

## 致谢

### 开发方式说明

本仓库的 Android 移植**由人工智能完成**：开发 agent 是 **[pi.dev](https://pi.dev)** 的编码 agent 工具，
实际执行框架也是它；人工智能模型来自 **DeepSeek**（`deepseek-flash`，经 deepseek.com 的 API 调用），
另有少量建议来自 **OpenAI** 与 **claude.ai** 网页端的免费模型。
也就是说：**所有移植工作都是 AI 执行的，实际跑命令、改代码、跑实验的框架是 pi.dev。**

> 这不是免责声明：结论仍然按 `docs/` 里的实测证据判断，而不是按"谁说的"判断。
> 凡是推导出来的机制，一律标注为不确定，除非有源码或实测支撑。

### 上游与依赖

- **[Leviidev/Henge](https://github.com/Leviidev/Henge)** —— 上游引擎与 iOS 版。本仓库的全部核心工作都是它的。
- **QEMU** 的 `hw/vmapple/*` —— 设备模型对拍的参照。
- **mesa / Turnip**、**libadrenotools**、**reims-vgpu**、**metal2vulkan** —— 图形链路。
- **libimobiledevice 社区** —— 恢复协议栈。
