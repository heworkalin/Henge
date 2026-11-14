# Henge on Android — 状态交接（安装 macOS 卡点）

> 写于 2026-11-14 凌晨。下一次继续时，从「第四节：下一步」开始看即可。
> 详细证据链在 [`android-install-debug.md`](android-install-debug.md)，本文是**摘要 + 待办**。

---

## 一、现在到哪一步了

安装流程（DFU + idevicerestore）已经能从「iBSS 一发就崩」推进到：

```
iBSS ✓ → iBEC/recovery ✓ → RestoreRamDisk(188 MB) ✓ → RestoreDeviceTree ✓
→ RestoreKernelCache(21 MB) ✓ → 内核启动 → 停机
```

卡在最后一步：**内核启动后立刻停在 `wfe`**，主机等不到 restore 模式，30 秒后报
`Failed to place device in restore mode`，`restore finished: -2`。

---

## 二、已经解决的问题

### 1. 客户机寄存器被写入宿主地址（已修复，已提交 `27ab79c`）

- **症状**：iBSS 发出后固件在 `0x12163c` panic，`x16` 是 `.got.plt` 地址。
- **根因**：`x16` 既是客户机寄存器又是 AArch64 的 IP0；引擎里 C 代码取汇编入口地址时
  走 GOT/PLT，那条序列用 `x16` 当临时寄存器，`.got.plt` 地址就留在了宿主 `x16`，
  下一个保存点把它写进客户机权威槽位。
- **修复**：`host/enter.S` 里所有 JIT 用到的汇编符号加 `.hidden`；
  删掉死字段 `helper_routine`（唯一取 `hv_helper_entry` 地址的地方）。
- **回归测试**：`tests/test_regs.c` + `android/test-regs.sh`（35 项，宿主侧跑，秒级）。
- **效果**：安装从 iBSS 崩溃 → 推进到内核阶段。

### 2. App 侧三个崩溃（已修复，未提交）

| 问题 | 修复 |
|---|---|
| `Prefs.edit()` 递归调用自己 → 改任何设置都 StackOverflow | 改为 `sp(c).edit()` |
| `InstallService` / `DownloadService` 触发 `ForegroundServiceDidNotStartInTimeException` | 两个服务都在 `onCreate` 里先 `startForeground()` |

---

## 三、第二个问题：现在的状态（**已修正，前一版结论作废**）

### 作废的旧结论

> ~~"iBEC 把内核入口算成 arm_init 前 4 字节，跳过了 start.s 的 per-CPU/页表建立"~~
> 错因两个：把「内核以物理地址 identity 运行的开机代码」当成 iBEC；
> 外加一处 0x1000 的地址算错。

### 实测：入口完全正常

| 事实 | 证据 |
|---|---|
| PA `0x74898000` 的代码 = 内核镜像文件偏移 `0xd80000`（`__TEXT_EXEC+0x8000`）| 逐字节相同 |
| `LC_UNIXTHREAD` pc `0xfffffe0007d82488` | 首条指令 `b 0xfffffe0007d8405c` = `__TEXT_EXEC+0x805c` |
| 实际执行起点 `0x7489805c` | 同一个位置，MMU 开启前以 PA identity 运行 |
| 段内 `adrp/add/sub` 三连算出的 `0x75176ae8` | 是**内核自己**的 PA→VA 重定位（`adrp` 立即数=镜像字节，已核对） |
| `0x1d176ae8` 处（= `__TEXT+0x165EAE8`）| `pacibsp / sub sp,sp,#0xb0 / stp x24,x23`，**栈正常** |

**即：`_start → start_first_cpu → arm_init` 一路是对的，入口/栈/重定位全部无问题。**

### 真正的卡点：一个没被解码的 chained fixup 槽

崩溃指令 `str x20,[x8]`，`x8` 来自全局槽（链接期 VA `0xfffffe000a6e48f0`，运行时 `0x1f1f88f0`）：

- 内核镜像里该槽的值就是 `0xc000000379cf90`
- 崩溃时读出来**还是** `0xc000000379cf90`
- 该值 **bit63 置位 = authenticated pointer 类型的 chained fixup 编码值**，低 48 位 `0x379cf90`
  不是内核 VA ⇒ 它**从未被解码**

⇒ 内核把编码值当指针用 → 数据异常（EC 0x25）+ `TPIDR_EL1 == 0` → 判定不可恢复 → `wfe`。

已知 fixup **跑过一部分**：运行时 `__DATA_CONST` 与镜像有 220 KB 差异；
`__DATA` 只有 170 字节差异。这个槽在漏掉的那一批里。

### 附带：一个高度可疑的实现选择

引擎把 PAC 签名/验签实现为**空操作**，并在 `AUT*`/`XPAC*` 时用
`core/translate.c:put_strip()` 清掉 bit 54..48。
但 chained fixup 的 authenticated pointer 把**链信息存在高位**
（`ubfx #51,#11`、`tbnz #62/#63`）。若 fixup 循环里出现 `aut*`，
`put_strip` 会破坏这些高位 —— 与"槽未被解码"的现象吻合，优先查这条。

## 四、下一步（按推荐顺序）

### B. 先做（快）：确认 iBEC 是否「自改代码」

`0x75176ae8` 是 **`adrp`/`add` 指令里的立即数**。若它真是运行时被改成我们内核的入口，
iBEC 必然写过固件代码页 `0x74898000`。

**做法**：在 `core/dispatch.c` 的分发循环里，对固件阶段（`pc < 0xfffffe0000000000`）
监视对 `0x74898000..0x74899000` 的写；命中就记录 `pc / 地址 / 写入值` 到文件。

- **有写** ⇒ 抓到 patch 者与写入值，问题在「它按什么规则算这个值」
- **没写** ⇒ 这个常量是 iBEC 编译期固定的 ⇒ **iBEC 与我们 boot 的内核不是同一套构建**，
  问题变成组件组合

### A. 其次：截获真正发出去的 iBEC

个性化后的 iBEC 由 `idevicerestore` 经 `host/libusb_shim.c:244` 的
`hv_vusb_send(g_usb, HV_VUSB_REQ_DATA, ep, ...)` 发出。
在该处按环境变量落盘（追加到 `usb_out_<ep>.bin`），再与 IPSW 里
`Firmware/dfu/iBEC.vma2.RELEASE.im4p` 对比。

⚠️ 已知：IPSW 原件（DER 包裹的 `bvx2`，LZFSE 解出 548 KB AArch64 代码）与
**运行中的 iBEC 字节不一致**，所以必须截获个性化后的那一份才有意义。

### C. 需要外部资源：带符号的内核（Apple KDK）

能一次性给出 `0x1d176ae8`、`0x1f1f88f0` 的真实符号名，但需要下载 KDK。

---

## 五、怎么继续（操作面）

### 构建与安装

```sh
./android/build-c.sh          # libhenge.so
./android/build-apk.sh        # APK
adb -s 127.0.0.1:5555 install -r android/app/build/outputs/apk/debug/app-debug.apk
```

### 录制日志

```sh
nohup timeout 1500 adb -s 127.0.0.1:5555 logcat -v time > /tmp/henge-$(date +%H%M%S).log 2>&1 &
```

日志里找 `HENGENATIVE` / `hengevm:`。**注意**：内核启动那一刻 logcat 会丢输出，
关键探针必须写文件（见下）。

### 设备上的取证文件（`/sdcard/Android/data/dev.henge.probe/files/vm/work/`）

| 文件 | 内容 |
|---|---|
| `blocks.bin` | 每个翻译块的 `{guest_va, ninsns, nwords}` + 客户机字 + 宿主字 |
| `kernel.bin` | 停机时从 RAM 挖出的内核镜像（80 MB，Mach-O） |
| `ktrace.bin` | 内核地址空间里按序执行的 8192 个块地址 |
| `entry.txt` | 内核入口现场：`pc/prev/x0..x3/el/spsr` + 固件最后 1024 个块的 `x20/x22/x23/x30` |
| `dt.bin` | 设备树（ADT 私有格式，见下） |

宿主侧分析脚本在 `/tmp`（`adtq.py` 解析 ADT、`delzfse` 解 LZFSE、`refs.py` 扫字符串引用）；
如需长期保留，应挪进 `tools/`。

### 两个调试陷阱（务必记住）

1. **内核启动瞬间 stderr → logcat 会丢输出**；同一段代码写文件正常。
   所有关键探针都应写文件。
2. **一次性 `static` 标志在同一进程内重试时不重置**，会让探针在「真正重要的那次」静默。
   探针应写成无状态/幂等，或显式随 machine 重置。

---

## 六、工作区状态

- 已提交（本地 `dev` 分支）：`27ab79c` 修复 + 测试；`ac2b7f4`、`617b2b2` 更早的 trace ring。
- **未提交**（都是诊断/实验代码，按约定不提交）：
  `core/{dispatch,exception,machine,memop,sysreg,translate}.c`、`core/cpu.h`、
  `devices/vmapple.c`（avp 器件日志）、`host/usb_host.c`、
  `android/install.c`（构建戳）、以及 App 侧三个修复
  （`Prefs/InstallService/DownloadService/InstallTab`）与 `IpswIntegrity.java`。
- `docs/android-install-debug.md` 已补齐十节，含全部排除项与证据。
- `docs/android-install-status.md`（本文）是摘要。

### 恢复前建议先做的清理

1. 撤掉 `core/dispatch.c` 里「强制入口 `0x1c896488`」那段实验（`continue` 分支）——
   它是绕过手段，留着会掩盖真实行为。
2. 撤掉 `android/install.c` 里的构建戳（或保留，用于对齐日志与构建）。
3. App 侧三个修复**建议单独提交**（它们与模拟器无关，且确实有效）。

---

## 七、一句话总结

> 第一个问题（寄存器被宿主指针污染）已修复并提交，安装因此推进到内核。
> 第二个问题已定位为：**iBEC 把内核入口算成了 `arm_init` 前 4 字节**（跳过 `start.s`
> 的 per-CPU/页表/向量建立），内核因此在 `TPIDR_EL1 == 0` 的状态下用未初始化的
> per-CPU 指针而停机。下一步只需回答一个问题：**那个硬编码入口是 iBEC 运行时改的，
> 还是它本来就该指向别处。**
