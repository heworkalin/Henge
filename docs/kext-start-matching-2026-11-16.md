# kext 一个都不 start：已排除项、地址表与复现方法（2026-11-16）

> 本文只记录**实测证据**。凡是"因为机制推断出"的内容都已标注或删除。
> 全部结论来自 `HV_TRACE_PC` 的逐块执行轨迹 + `docs/simd-clobber-kernelcache-2026-11-15.md` 的逐字节比对。

## 1. 一句话现状

**整条 IOKit 匹配链完整跑通**（plist → 220 个 OSKext → personality 进 IOCatalogue →
`addDrivers` → `catalogNewDrivers`），**但 `__PRELINK_TEXT` 里没有一个 kext 被执行**
（kext 的 `probe`/`start` 是 kext 代码，0 块 = 没有任何驱动被启动）。
所以 `restored` 等不到 USB → restore `-2` 超时。

## 2. 已排除的假设（都有硬证据）

| # | 假设 | 怎么排除的 | 结论 |
|---|---|---|---|
| 1 | kernelcache 解压有残留错误 | dump 运行期 `__PRELINK_TEXT`（7.27 MB）与 IPSW 明文逐字节比对 | 3967 个差异 qword **全部等于 `ref + slide`**，其余 0 字节不同 |
| 2 | `__PRELINK_INFO`（plist）数据错 | dump 运行期 3.26 MB 与 IPSW 明文比对 | **0 字节差异** |
| 3 | 我们的 rebase 做错/多做 | `HV_NO_KFIX=1`（完全不做）与默认对比 | 行为完全一致（同样 `kext 0`、同样 MMIO）；内核自己会走完 fixup |
| 4 | device tree 缺失/写错 | `tools/adt.py` 解析运行期 ADT + 扫内存 | `/chosen/memory-map` 完整；root `compatible` = `VMA2MACOSAP, VirtualMac2,1, AppleVirtualPlatformARM` |
| 5 | personality 与 DT 不匹配 | 离线解析 238 个 kext 的 `IOKitPersonalities` | `AppleVirtualPlatformARMPE` 的 `IONameMatch` 正是 `AppleVirtualPlatformARM`，**条件成立** |
| 6 | `gRootNub` 没继承 DT 属性 | 扫内核堆找 `AppleVirtualPlatformARM` | 命中 `0x84e26000` → 属性表已复制进 `gRootNub` |
| 7 | SIMD 被 helper 破坏 | 前一轮修复 + `kt_flags` 实测 | `0x71`，与 IPSW 一致 |
| 8 | `HV_PCHK` 零命中 = 没执行 | 改正地址空间后重测 | **假阴性**：`HV_PCHK` 比的是运行时 pc 的低 32 位，见第 5 节 |
| 9 | IOKit 没初始化 | trace 计数 | `InitIOKit` 31 次、`IOCatalogue::initialize` 12 次、`attach` 61 块 |
| 10 | kext 加载（plist→OSKext）失败 | trace + `HV_PCHK` 抓 `getObject` 返回值 | `getObject` 返回**非 NULL**，类型匹配后进入 220 个 kext 的创建循环，最终走**正常** epilogue |
| 11 | `sendAllKextPersonalitiesToCatalog` 没执行 | trace（改正 slide 后） | **执行了**，并进入 `IOCatalogue::addDrivers`（47 块）→ `catalogNewDrivers` |
| 12 | 恢复模式跳过 kext | XNU 源码 `bsd_init.c` / `arm_init.c` | 恢复模式只影响 `restore_boot`、`is_ramdisk`，**没有**跳过 kext 的分支 |

## 3. 已确认执行的链路（trace 证据）

`HV_TRACE_PC` 记录的是**块起始 VA（运行时地址）**，下表 `run VA` 可直接在 trace 里 `grep`。

| 环节 | link VA | run VA | trace |
|---|---|---|---|
| `KLDBootstrap::readStartupExtensions` 入口 | `0xfffffe0008668874` | `0xfffffe001d17c874` | 2 |
| `readPrelinkedExtensions(Primary)` 调用 | `0xfffffe00086688f0` | `0xfffffe001d17c8f0` | 2 |
| `loadKernelComponentKexts` 调用 | `0xfffffe000866891c` | `0xfffffe001d17c91c` | 2 |
| `loadKernelExternalComponents` 调用 | `0xfffffe0008668920` | `0xfffffe001d17c920` | 2 |
| `readBuiltinPersonalities` 调用 | `0xfffffe0008668924` | `0xfffffe001d17c924` | 2 |
| **`sendAllKextPersonalitiesToCatalog` 调用** | `0xfffffe000866892c` | `0xfffffe001d17c92c` | **2** |
| ↳ 该函数本体 | `0xfffffe00084a9e3c` | `0xfffffe001cfbde3c` | ✓ |
| `OSKext::addKextsFromKextCollection` 里 `getObject` 之后 | `0xfffffe00084a901c` | `0xfffffe001cfbd01c` | ✓（`x0 != 0`） |
| **`IOCatalogue::addDrivers`** | `0xfffffe0008544b50`(ref) | `0xfffffe001d058b50` | **47 块** |
| **`IOService::catalogNewDrivers`** | `0xfffffe0008505580` | `0xfffffe001d019580` | ✓ |
| `IOService::attach`（`gRootNub->attach(NULL)`） | `0xfffffe000850668c` | `0xfffffe001d01a68c` | 61 块 |
| ↳ `attach` 的 `provider==NULL` 分支 | `0xfffffe00085069bc` | `0xfffffe001d01a9bc` | ✓（`ok=true`） |
| `StartIOKitMatching` | `0xfffffe00084eb248` | `0xfffffe001cfff248` | ✓ |
| ↳ 内联的 `startIOServiceMatching`（`bl IOWorkLoop::workLoop`） | `0xfffffe00084eb280` | `0xfffffe001cfff280` | ✓ |
| `IOWorkLoop::workLoop` | `0xfffffe000855290c` | `0xfffffe001d06690c` | ✓ |
| `IOService::registerService`（虚调用目标） | `0xfffffe000850a124` | `0xfffffe001d01e124` | ✓ 完整路径 |

**反面对照（0 块）：**

| 目标 | run VA | 说明 |
|---|---|---|
| `__PRELINK_TEXT` 整段 | `0xfffffe001bb20000 .. 0xfffffe001c210000` | **0 块** = kext 代码一次都没跑 |
| 内核入口 `LC_UNIXTHREAD` 的 pc | `0xfffffe001c896488` | 0 块（iBoot 实际跳到 `arm_init`） |
| 内核第一条执行的指令 | `0xfffffe001d176ae8`（link `0x8662ae8`） | 1 块，`arm_init`（含 `"kprintf initialized"` @ link `0x8663280`） |

## 4. 镜像布局与地址空间（复现时必看）

`slide = 0x14b14000`（`HENGE_RANDOM_SEED` 关时固定）。

| 段 | link VA | fileoff | size |
|---|---|---|---|
| `__TEXT` | `0xfffffe0007004000` | `0x0` | `0x8000` |
| `__PRELINK_TEXT` | `0xfffffe000700c000` | `0x8000` | `0x6f0000` |
| `__DATA_CONST` | `0xfffffe00076fc000` | `0x6f8000` | `0x680000` |
| `__TEXT_EXEC` | `0xfffffe0007d7c000` | `0xd78000` | `0x263c000` |
| `__PRELINK_INFO` | `0xfffffe000a3b8000` | `0x33b4000` | `0x31c000` |
| `__DATA` | `0xfffffe000a6d4000` | `0x36d0000` | `0x2e0000` |
| `__LINKEDIT` | `0xfffffe000a9b4000` | `0x39b0000` | `0xe5c000` |

换算式（**每次都写出来，别心算**）：

```
fileoff = link_va - 0xfffffe0007004000
run_va  = link_va + 0x14b14000
PA      = 0x73b18000 + fileoff
```

镜像里还有 **220 个 `LC_FILESET_ENTRY`（`cmd 0x80000035`）**，是"kext 加载结构体"：

```c
struct fileset_entry_command {
    uint32_t cmd;              /* 0x80000035 */
    uint32_t cmdsize;
    uint64_t vmaddr;           /* 该 kext 的 link VA */
    uint64_t fileoff;
    uint32_t entry_id_offset;
    uint32_t reserved;
    char     entry_id[];       /* "com.apple.driver.AppleVirtualPlatform" 等 */
};
```

`com.apple.kernel` 的 `vmaddr` 就是 `0xfffffe000700c000`（`__PRELINK_TEXT` 起点）。
**kernelcache 没有 `LC_SYMTAB` / `LC_FUNCTION_STARTS`**，所以没有符号表；
函数边界只能靠 `PACIBSP`/`PACIASP`/`stp x29,x30,[sp,#-16]!` + `ret`/`retab` 扫描，
或按 `PACIBSP` 反查（**不要**手写分支解码器，见第 6 节）。

## 5. 如何复现（设备端，一轮约 60 秒）

```bash
# 1) 构建（约 1 分钟）
cd /home/he/Henge
ZLIB=1 API=28 ./tools/build-install-android.sh

# 2) 推送
adb -s 127.0.0.1:5555 push build/andcli/host-install /data/local/tmp/henge/

# 3) 跑（HV_TRACE_PC = 每块执行记一次 run VA，4M 块上限）
adb -s 127.0.0.1:5555 shell 'chmod 755 /data/local/tmp/henge/host-install; \
  cd /data/local/tmp/henge && HV_LOG=2 \
  HV_TRACE_PC=/data/local/tmp/henge/pctrace.bin \
  nohup ./host-install AVPBooter.vmapple2.bin /data/local/tmp/ipsw/24G90.ipsw workXX 4096 2 \
  > runXX.log 2>&1 & echo started'

# 4) 等 restore finished（约 55~60 秒），拉轨迹
adb -s 127.0.0.1:5555 pull /data/local/tmp/henge/pctrace.bin /tmp/pctrace.bin
```

分析（本文件第 3 节所有数字都是这么来的）：

```python
import struct
d = open('/tmp/pctrace.bin','rb').read()
pcs = struct.unpack_from(f'<{len(d)//8}Q', d, 0)
slide = 0x14b14000
def hits(link_va):
    run = link_va + slide                 # 注意：不要用低 32 位去比
    return sum(1 for p in pcs if p == run)
print('sendAll call :', hits(0xfffffe000866892c))
print('attach entry :', hits(0xfffffe000850668c))
print('PRELINK_TEXT :', sum(1 for p in pcs if 0xfffffe001bb20000 <= p < 0xfffffe001c210000))
```

按段分类（判断"kext 有没有跑"最直接）：

```python
kext = sum(1 for p in pcs if 0xfffffe001bb20000 <= p < 0xfffffe001c210000)
kern = sum(1 for p in pcs if 0xfffffe001c890000 <= p < 0xfffffe001eecc000)
print(f'kext {kext}  kernel {kern}')      # 期望：kext 0
```

## 6. 两个必须记住的方法论教训

### 6.1 `HV_PCHK` 要传**运行时**地址的低 32 位

源码里是 `uint64_t plow = (uint32_t) cpu->pc;`，`cpu->pc` 是运行时 VA。
所以 `HV_PCHK = (link_va + slide) & 0xffffffff`。传 link VA 会永远零命中：

| 探针 | 值 | 结果 |
|---|---|---|
| `HV_PCHK`（错，link） | `0x84eb0e0` | 0 次 |
| `HV_PCHK`（对，run） | `0x1cfff0e0` | 16 次 |

### 6.2 不要手写 ARM64 分支解码器

手写 `b.cond`/`cbz`/`bl` 的 imm 位域极易错（本项目已因此误判多轮）。
用 NDK 的 `llvm-objdump`，raw binary 需先包一层 ELF：

```bash
BIN=/home/he/Android/Sdk/ndk/29.0.14206865/toolchains/llvm/prebuilt/linux-arm64/bin
python3 -c "
d=open('/tmp/kc/kc.img','rb').read(); BASE=0xfffffe0007004000
lo=0xfffffe0008668874; hi=0xfffffe0008668948
open('/tmp/x.bin','wb').write(d[lo-BASE:hi-BASE])"
$BIN/llvm-objcopy -I binary -O elf64-littleaarch64 --rename-section .data=.text,code /tmp/x.bin /tmp/x.o
$BIN/llvm-objdump -d /tmp/x.o | sed -n '6,60p'
```

注意 `--adjust-vma` 对裸 binary 的分支目标显示不可靠（指令本身正确），
**跳转目标请按 `imm` 手算后再核对**，或干脆以 trace 的实际执行顺序为准。

## 7. 当前唯一未解的点

`catalogNewDrivers`（run `0x1d019580` 附近）执行了，但没有产出任何 `probe`/`start`。
下一步只需要看它内部：

1. 遍历到的 nub 里有没有 `gRootNub`；
2. `matchService` / `compareNames` 对 `compatible` 的比较结果。

如果这一步也正常，那就是某条具体指令的 JIT 语义偏差（同 SIMD 一类），
用 `HV_BLOCK_DUMP` + `objdump` 逐条对拍。
