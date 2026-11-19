# 调试开关总表

本文档是 Henge 全部环境变量开关的**唯一清单**。新增开关请同时更新这里。
代码来源：`grep -rn 'getenv("' --include=*.c android core devices host tools ios`。
每条的准确性以「来源」列的文件为准——这里只写它现在*实际*做什么，不写设计意图。

---

## 0. 怎么用

**Android CLI（首选，1 分钟一轮）**

```bash
ZLIB=1 API=28 ./tools/build-install-android.sh
adb -s emulator-5554 push build/andcli/host-install /data/local/tmp/henge/
adb -s emulator-5554 shell 'cd /data/local/tmp/henge && rm -rf workN && \
  HV_LOG=1 HV_EXTRA_BOOT_ARGS="serial=3" setsid nohup ./host-install \
  AVPBooter.vmapple2.bin /data/local/tmp/ipsw/24G90.ipsw workN 2048 2 \
  > /data/local/tmp/henge/run.log 2>&1 < /dev/null &'
```

参数顺序：`booter ipsw workdir RAM_MB cpus [ecid]`。**少一个会静默跑错**（`workN 3` 会被当成 RAM=3 MB）。
每次换一个新的 `workN`，否则会因锁文件直接退出。

**宿主（几十秒一轮，PVG 是 stub）**

```bash
./tools/build-install-host.sh
CURL_CA_BUNDLE=$PWD/work/ca-bundle.pem ./build/host/host-install \
  work/AVPBooter.vmapple2.bin android/ipsw/24G90.ipsw work 4096 2
```

**APK**：`android/install.c` 从 `getenv` 读，App 进程没有 shell 环境变量，需要用
`setprop` 或在入口里注入；调试请优先用 CLI。

约定：**「默认关」= 不设置该变量时行为与上游/正常路径一致**。凡是会改 guest 数据的开关，
默认必须是关的。

---

## 1. 日志与输出

| 开关 | 取值 | 默认 | 作用 |
|---|---|---|---|
| `HV_LOG` | `0..3` | Android 入口 `2`，宿主工具更安静 | 引擎日志级别。`1`=关键事件，`2`=每块 pc 历史（`hv_pc_hist_note`）、`3`=最啰嗦。**同时决定 `pc_hist` 与 `hv_last_el0_*` 是否记录（历史遗留，见 §4）** |
| `HV_LOGPC` | 地址十六进制 | 关 | 只记录/打印指定 pc 附近的日志 |
| `HENGE_LOG_FILE` | 路径 | `files/vm/henge.log` | 全量日志文件。调试时可设 `/dev/null` 避免 GB 级文件（`android/stderr_to_logcat.c:121`） |
| `HENGE_LOG_RATE` | 行/秒 | `120` | logcat 限速；关键词行不受限（`android/stderr_to_logcat.c:125`） |
| `HENGE_SYSCALL_LOG` | `1` | 关 | 把 guest 系统调用打到 stderr（`core/exception.c:137` 附近） |
| `HV_STATS` | `1` | 关 | 周期性打印引擎统计（块数/异常数等） |
| `HV_TRACE` | `1` | 关 | 每个块打一行 `block <pc> el<n>`（`core/dispatch.c:1494`） |
| `HV_BLOCKTIME` | `1` | 关 | 单块耗时采样（每块两次读计数器，慢） |
| `HV_HZ` | 数字 | 设备默认 | 覆盖 `pvg_lite` 的时钟频率（`devices/pvg_lite.c:193`） |

---

**默认输出（无开关）**：引擎启动时打印一行 `hengevm: timebase measured <MHz> (<ticks> in <s>)`，
实测 guest 看到的时间基。counter 是 `CLOCK_MONOTONIC_RAW` 的软件缩放（不是硬件 `CNTFRQ`），
换设备不会改变频率，但缩放代码本身可能出错，所以每次启动都测一次。

## 2. 引导参数与内核镜像

| 开关 | 取值 | 默认 | 作用 |
|---|---|---|---|
| `HV_EXTRA_BOOT_ARGS` | 字符串 | 空 | **追加**到 iBoot 的 boot-args。让内核说话用 `"serial=3 debug=0xA"`；`serial=3` 触发 `switch_to_serial_console`，`debug` 的 `DB_PRT=0x2`/`DB_KPRT=0x8` 打开串口输出 |
| `HV_BOOT_ARGS_VERBOSE` | `1` | 关 | 自动追加 `-v`（旧行为，已改成显式开关） |
| `HV_DT_LEN_PATCH` | `1` | 关 | 把 `boot_args` 里的 device tree 长度改写成 16 MB。**会改 guest 数据**：iBEC 的原值是 555200，改写后内核会走过真实 tree。默认必须关 |
| `HV_KFIX` | `1` | 关 | 引擎自己 rebase 内核镜像的 `__DATA`。`docs/kext-start-matching-2026-11-16.md` 记录：补与不补行为一致，内核会自己走 fixup。默认必须关 |
| `HV_KFIX_DC` | `1` | 关 | 强制补 `__DATA_CONST`（**危险**：内核自己也会补，补两次得到 `base+pointer`） |
| `HV_DT_DUMP` | 路径 | 关 | dump 解析后的 device tree |
| `HV_DUMP_KC` | `1` | 关 | dump 运行期 `__PRELINK_TEXT` + `__PRELINK_INFO` |
| `HV_DUMP_RAMDISK` | `1` | 关 | dump ramdisk 到工作目录 |
| `HENGE_RANDOM_SEED` | `1` | 关 | 用宿主熵填 `/chosen/random-seed`。**默认关才可复现**（关时 slide 固定 `0x14b14000`） |
| `HV_CFG_POKE` | `"0x8=0,0x2c=0"` | 空 | 覆盖 cfg 设备（`devices/cfg.c:116`）里的 32 位字段，用于探测未知字段含义 |
| `HV_NO_PVLOCK` | `1` | 关 | 关掉 PV ticket lock 的 HVC 协处理（内核退回普通自旋） |
| `HENGE_OEM` | `1` | 关 | **应答 vmapple OEM 超调用**（UID/REVISION/FEATURES、MABS offset、boot session UUID、WFK/KICK）。默认关=内核拿到 `NOT_SUPPORTED`，走普通路径。开启等于告诉 guest「这是 OEM 设备」，而应答不完整比不应答更容易把 guest 带进死等 |

---

## 3. 断点与执行追踪

| 开关 | 取值 | 默认 | 作用 |
|---|---|---|---|
| `HV_PCHK` | `start:end`（**run VA 低 32 位**，十六进制） | 关 | 执行断点。源码比较 `(uint32_t)cpu->pc`，所以传的是低 32 位。**传 link VA 会差一个 slide，永不命中** |
| `HV_PCHK_CPU` | `all` / `-1` / `N` | `0` | 断点在哪些 CPU 上生效。`all`/`-1`=全部，`N`=指定核。用户态不一定在 cpu0，所以断用户态时要设 `all` |
| `HV_PCHK_MAX` | 数字 | `400` | 最多打印多少次命中 |
| `HV_PCHK_DST` | `start:end` | 关 | 附加过滤：只有 `x22` 落在该范围才打印（用于热函数如 memcpy） |
| `HV_PCHK_LEN` | 十六进制 | 关 | 附加过滤：`x24` 小于该值则跳过 |
| `HV_TRACE_PC` | 文件路径 | 关 | 记录**每个块**的 run VA（u64 数组，上限 4M 块 = 32 MB）。判「某函数执行没执行」用它，别用 `HV_PCHK` |
| `HV_BLOCK_DUMP` | 文件路径 | 关 | dump 翻译后的宿主指令；`HV_BLOCK_DUMP_EVERY`（默认 64）控制间隔 |
| `HV_BLOCK_DUMP_EVERY` | 数字 | `64` | 见上 |
| `HV_STACKSCAN` | `1` | 关 | 卡住时扫描并打印内核栈 |
| `HV_SCAN_PROC` | `1` | 关 | 周期性扫描内存里的进程名字符串。**注意：这只是字符串匹配，不能证明进程存在** |

三个地址换算式（slide 固定 `0x14b14000`）：

```
fileoff = link_va - 0xfffffe0007004000
run_va  = link_va + slide
HV_PCHK = (link_va + slide) & 0xffffffff
```

---

## 4. 用户态与内存/MMU 诊断

| 开关 | 取值 | 默认 | 作用 |
|---|---|---|---|
| `HV_EL0_DUMP` | `1` | 关 | EL0 data abort 时把故障页 dump 到 `/data/local/tmp/henge/el0_XX_<pc>_<pa>.bin`（最多 12 页） |
| `HV_NO_EL0_DUMP` | `1` | 关 | 关掉上面的行为（当默认值被改过时使用） |
| `HV_DUMP_RAM` / `HENGE_DUMP_RAM` | `1` | 关 | dump 整块 guest RAM |
| `HV_DUMP_RAM_AT` / `HENGE_DUMP_RAM_AT` | 十六进制/秒 | 关 | 在指定时刻 dump |
| `HV_DUMP_TAG` | 字符串 | — | dump 文件名标签 |
| `HV_WATCH_VA` / `HV_WATCH_PA` | 地址 | 无默认（不设=不布防） | 监视某虚拟/物理地址的访问 |
| `HV_WATCH_ATTACH` | `1` | 关 | 附加到已有进程上布防 |
| `HV_PTW_LOG` / `HV_PTW_SNAP` | `1` | 关 | 页表写历史 / 快照 |
| `HV_WALK_LOG` | `1` | 关 | 打印每次 MMU 走页 |
| `HV_KVA_BASE` / `HV_KVA_SHIFT` / `HV_KVA_HIST` | 数值 | — | kva 直映射区域参数覆盖（`host/main.c`） |
| `HV_RAM_ANON` / `HV_RAM_FILE` / `HV_RAM_GREP` / `HV_RAM_GREP_OUT` | — | — | 载体/内容检查（`host/main.c`） |
| `HV_RAM_NO_LEDGER` / `HV_RAM_NO_PREFAULT` | `1` | 关 | 关掉 RAM 记账 / 预取（性能对照） |
| `HV_EL0_LAST`（规划） | — | — | 未实现；现由 `hv_el0_note()` 无条件记录 |
| `HV_THREADS` | `1` | 关 | 把每个进程（`launchd`/`restored_externa`/`loginsupport`/`mobile_restore`）的线程链表 dump 出来：`state` / `wait_event` / `turnstile`，并对每个被等待的 turnstile 打印尾部内核指针（`ts_proprietor` 的候选）。**在引擎内读 guest RAM，不需要 gdb**（proot 下 gdb 会把 PIE 全局变量解析成未重定位地址）。由 SIGUSR1 或 `hv_android_scan_procs()` 触发。偏移：`proc+0x18`→`proc_ro`、`proc_ro+8`→`task`、`task+0x50`→线程链头、`thread+0x10`→`wait_event`、`+0x28`→**waitq**、`+0x30`→**turnstile**、`+0x1c8`→`state`（`+0x28`/`+0x30` 只差 8 字节且外观都是内核指针，必须用 `WAITQ_FLAGS.type` 区分：1=waitq、2=turnstile） |
| `HV_TH_STACK` | `1` | 关 | 配合 `HV_THREADS`：对每个线程扫它的 16 KiB 内核栈（从 `struct thread` 里找页对齐的栈顶指针，不依赖 `kernel_stack` 偏移），打印栈上的内核代码地址。用于回答"这个阻塞线程停在哪个函数"。算法同 `tools/dump-stacks.py`。**注意栈上会有旧数据**，短栈（只有两三个地址交替）应先怀疑残留而不是调用链 |
| `HV_TS_FULL` | `1` | 关 | 配合 `HV_THREADS`：把每个 turnstile 的 `+0x300..+0x400` 与 `+0x00..+0x40` 全字段打印出来（不只是内核指针）。`ts_proprietor` 的偏移随构建而变，用这个对照而不是假定 |
| `HV_PCOMM_OFF` | `1` | 关 | 配合 `HV_THREADS`：打印扫描到的 `p_comm` 在 `struct proc` 内的偏移（名字长度 ≥8 才打印）。用来确认固定偏移；`0x8/0x30/0x50` 这类小偏移是随机数据的误报 |
| `HV_FAULT_PROBE` | `lo:hi[,lo:hi,...]`（最多 4 段，十六进制，32 位写法自动补成 `0xfffffe00...`） | 关（不设=不统计） | 对每个 EL0 同步异常，在**处理完、回 EL0 前**做三件事：① 用块入口 `trace_ring`（`put_trace_entry`，不经 dispatcher，chain/superblock/`bl` 直调全覆盖）判断路径是否经过各段；② 用 `hv_mmu_walk()` 按 **fault 时刻的 TTBR0** 遍历该 fault 地址，统计 PTE 是否已建立；③ 按 `(TTBR0, VA 页)` 统计重复缺页与热点页。结果由 `hv_fault_stats_dump()` 打印（SIGUSR1 或 CLI 结束时）。只统计，不改 guest 语义。**前提 `HV_LOG>=2`**（`trace_ring` 只在 `log>=2` 时由 JIT 写入） |

**引擎内部记录（gdb 直接读，不需要开关）**

| 全局 | 内容 |
|---|---|
| `hv_last_el0_va/page_pa/x0/x19/x20/x21/x22/x23/lr/sp` | 用户态**最后进入**的块及其寄存器；`page_pa` 是那一刻的物理页 |
| `hv_el0_trace_va[] / hv_el0_trace_x16[]` | 用户态块环形轨迹 + 系统调用号 |
| `hv_el0_flt_pc/ret/far/esr/sp/x[][31]`、`hv_el0_flt_n` | EL0 同步故障（非 SVC）的完整寄存器快照，环形 8 条 |
| `hv_svc_n / hv_svc_imm / hv_svc_x16 / hv_svc_x0 / hv_svc_x1 / hv_svc_ret / hv_svc_path[]` | 系统调用环形（`HV_SVC_CAP=4096`）。**按时间序读**：`slot = i % 4096`，i 从 `hv_svc_n-1` 倒推 |
| `hv_uexc_n / hv_uexc_pc/elr/far/esr/type[]` | 最近异常，环形 64 |
| `pc_hist[][4096] / pc_hist_at[]` | 每块 pc（仅 `HV_LOG>=2` 记录） |
| `hv_net_tx_frames / hv_net_rx_frames` | 网络帧计数 |

---

## 5. USB（restore 的关键路径）

| 开关 | 取值 | 默认 | 作用 |
|---|---|---|---|
| `HV_VUSB_LOG` | `1` | 关 | vusb 设备事件日志 |
| `HV_VIRTIO_CFG_LOG` | `1` | 关 | virtio 配置空间读写（`vusb-cfg: status/read +0x14/isr`）——判断驱动走到 feature 协商的哪一步靠它 |
| `HV_VUSB_INTX` | `1` | 关 | 恢复旧的 INTx 触发行为（`devices/virtio.c:151`） |
| `HV_VUSB_MSI` | `1` | **关** | 给 vusb 加 MSI capability。**上游没有这个**，实测去掉后配置行为不变 |
| `HV_VUSB_LATE` | `1` | 关 | 把 vusb 放到 PCI 槽位最后（默认放到块设备之前，因为它的驱动用 INTx，槽位顺序是唯一差异） |
| `HV_VUSB_PROBE` | `1` | 关 | 探测 vusb 应答 |
| `HENGE_USB_REENUM` | `1` | **关** | userland 起来 8 秒后 `hv_vusb_force_rescan()`。**曾导致 `Killed`**：8 秒后 `d` 可能已被 installer 释放。只用于对照实验 |
| `HENGE_STOP_ON` | 串口子串，可用 `\|` 分隔多个 | 关 | **串口同步断点**：console 出现匹配行时 `raise(SIGSTOP)` 停住整个进程，gdb 附加读内存后 `kill -CONT` 继续。串口是 guest 与引擎唯一的共同同步点，所以这是「在已知时刻抓状态」的唯一无轮询办法。`devices/vmapple.c` 的 `vmapple_serial_out()`。**命中时会先 dump syscall 环（含 `mach_msg2_trap` 的 header/body）和线程表，再 `SIGSTOP`** |
| `HENGE_STOP_EVERY` | `1` | 关 | 每次匹配都停（否则只停第一次）。配合多个模式用它走一条事件阶梯，每级拿一次快照 |
| `HV_RESET_ENUM` | `1` | 关 | 恢复「枚举前先 reset」的旧行为（`host/libusb_shim.c:93`） |
| `HV_LIBUSB_LOG` | `1` | 关 | 宿主 libusb shim 日志（arrived/left/enumerate failed）——**看 host 是否看到设备切换就靠它** |
| `HV_USB_CTRL_CHUNK` / `HV_USB_OUT_CHUNK` | 字节数 | 设备默认 | 控制/OUT 传输分块大小（调试吞吐） |
| `HV_XHCI_LOG` | `1` | 关 | xhci 日志（**注意：restore 走 virtio-usb/bdif，不走 xhci**） |

**「放行」的两个现有触发点**

| 位置 | 触发条件 | 默认 |
|---|---|---|
| `android/install.c:450` `usb_handoff_to_kernel()` | guest 首次执行内核代码（`g_cpus[0].pc >= 0xfffffe00...`）时 rescan | **开（无开关）** |
| `devices/vmapple.c:290` `usb_reenumerate_after_userland()` | 串口出现 `ignition sequence complete` 后 8 秒 | `HENGE_USB_REENUM` |

已实测：guest **从不**自己发 PSCI `SYSTEM_RESET`（0 次），所以「设备断开」只能由 vusb 的 `session++` 模拟。

---

## 6. 其它设备

| 开关 | 取值 | 默认 | 作用 |
|---|---|---|---|
| `HV_NO_NET` | `1` | 关 | 不创建 virtio-net |
| `HV_NET_LOG` | `1` | 关 | 网络日志 |
| `HV_NET_FWD` | `"hostport:guestport"` | 空 | 从宿主 loopback 转发到 guest（`host/net_slirp.c:308`） |
| `HV_DNS_PROXY` | `1` | 关 | 启用 DNS 代理（`host/dns_proxy.c`） |
| `HV_PCI_LOG` / `HV_PCI_IRQ_LOG` | `1` | 关 | PCI 配置 / 中断路由日志 |
| `HV_NO_MSI` | `1` | 关 | 全局关掉 MSI capability（`devices/virtio.c:250`） |
| `HV_BLK_STATS` | `1` 或 秒数 | 关 | 每 N 秒打印块设备真实 IO（`req/s`、`MB/s`、flush、busy）——**判断 guest 有没有读写磁盘靠它** |
| `HV_BLK_SEG_MAX` | 数值 | 设备默认 | 覆盖块设备的 `seg_max` |
| `HENGE_DISK_LOG` | `1` | 关 | 磁盘层日志（`android/install.c:224`） |
| `HV_NO_SOUND` / `HV_SND_LOG` | `1` | 关 | 关掉 virtio-snd / 声音日志 |
| `HV_BALLOON` | `1` | 关 | 启用 virtio-balloon |
| `HV_AVP_LOG` | `1` | 关 | AVP（AppleVirtualPlatform）日志 |
| `HENGEVM_AES_LOG` | `1` | 关 | AES 设备日志（`devices/aes.c:116`） |
| `HV_INPUT_ORDER` | `"keyboard"` / `"swap"` | 默认 | 输入设备顺序实验（`devices/vmapple.c:454`） |
| `HV_KBD_PRODUCT` | 数值 | `1` | 键盘 product id 实验（`devices/virtio_input.c:44`） |
| `HV_CLICKS` | `"秒:x,y;秒:x,y"` | 空 | 定时点击，无窗口驱动 guest UI（`host/main.c:323`） |
| `HV_TYPE_AT` | 秒 | 空 | 到点输入 "abc"，端到端验证输入（`host/main.c:905`） |
| `HV_PVG_LITE_LOG` / `HV_PVG_NO_METAL` / `HV_PVG_OPS` / `HV_PVG_TALLY` / `HV_PVG_TRACE_FILE` / `HV_PVG_VBLANK` | 各种 | 关 | PVG/图形（reims-vgpu → metal2vulkan） |
| `HV_REIMS_LOG` | `1` | 关 | reims-vgpu 日志（`android/reims_pvg.c:192`） |
| `HV_COLOR_PROBE*` | 见代码 | 关 | 颜色/合成验证（`HV_COLOR_PROBE_DIR`、`_MAX`、`_SECS`、`_METRICS_SECS`） |

---

## 7. JIT 与引擎行为

| 开关 | 取值 | 默认 | 作用 |
|---|---|---|---|
| `HV_JIT_MB` | MB | 引擎默认 | JIT 区大小；调小可加速触发刷新（调试） |
| `HV_JIT_POISON` / `HV_JIT_SPLIT` | `1` | 关 | 毒化/拆分，用于定位 |
| `HV_IC_FLUSH` | `1` | 关 | 解释 `IC IVAU`（指令缓存刷新）行为 |
| `HV_IC_IVAU` | `1` | 关 | 变体 |
| `HV_SUPERBLOCKS` | `0/1` | `1` | 条件分支不结束块（`core/machine.c:95`） |
| `HV_NO_CHAIN` / `HV_NO_XCHAIN` | `1` | 关 | 关掉块链接（性能对照） |
| `HV_INLINE_XJUMP` | `1` | 开 | 内联跨块跳转（设 `0` 关闭，`core/translate.c:447`） |
| `HV_SLOW_MEM` | `1` | 关 | 强制所有内存访问走慢路径（诊断用，慢 1000 倍） |
| `HV_COPY_LOG` / `HV_MOVIMM` / `HV_C_EXCL` / `HV_HOIST_CHECK` / `HV_RECHECK` / `HV_EXIT_CHECKS` | `1` | 关 | 翻译器一致性检查 |
| `HV_IJC_PAGES` / `HV_IJC_DEBUG` / `HV_TLBI_LEAVES` / `HV_TLBT` | 各种 | `HV_TLBT=1` | 间接跳转缓存 / TLB 布局（`core/machine.c:80`） |
| `HV_WFE_SPIN` | `1` | 关 | WFE 用自旋而非睡眠（`core/dispatch.c:555`） |
| `HV_BIG_LOCK` | `1` | 关 | 单 CPU 进 guest 代码（`host/vm.c:272`） |
| `HV_NO_FAST_RESUME` / `HV_RESUME_CLOCK` / `HV_RESUME_SKEW` / `HV_NO_RAS` / `HV_NO_STASH` | 各种 | — | 恢复/暂停路径实验 |
| `HV_DC_PROBE` | `1` | 关 | `__DATA_CONST` 探测（`core/dispatch.c:262`） |
| `HV_QOS` | 数值 | 引擎默认 | 线程 QoS（`host/vm.c:374`） |
| `HV_PROF_ALL` / `HV_PROF_FN` / `HV_PROF_IOS` / `HV_PROF_IOS_SECS` / `HV_PROF_PCS` | 各种 | 关 | profiling |

---

## 8. 宿主编排 / 安装流程 / Android

| 开关 | 取值 | 默认 | 作用 |
|---|---|---|---|
| `HV_RUN_INSTALLER` | `1` | 依入口 | 宿主工具里启动 idevicerestore 线程 |
| `HV_INSTALL_LOG` | `1` | 关 | `FLAG_DEBUG`，抓 libirecovery debug |
| `HV_INSTALL_STOP_AT_END` | `1` | 关 | 到结尾停住不退出（便于附加 gdb） |
| `HENGE_KICKER` | `0` | 开 | 关掉 1 ms kicker（`android/install.c:375`）。guest 只在 CPU 跑时才推进，关掉会让它饿死 |
| `HENGE_HEARTBEAT` | `1` | 关 | 心跳日志（`android/install.c:379`） |
| `HENGE_NO_DUMP` | `1` | 关 | 结尾不 dump（避免 GB 级输出） |
| `HENGE_DUMP_RAM_AT` | 秒 | 关 | 到点 dump RAM（`android/install.c:561`） |
| `HV_AGENT` | `1` | 关 | 宿主侧 agent 通道（`host/main.c:1145`） |
| `HV_HOOK_*` / `HV_SHARE_LOG` / `HV_INPUT_LOG` / `HV_CFG_POKE` | 见代码 | 关 | 宿主共享目录 / 输入 / 配置调试 |

---

## 9. 出问题时的最短路径

| 现象 | 先开 |
|---|---|
| 不知道 guest 停在哪 | `HV_LOG=1 HV_EXTRA_BOOT_ARGS="serial=3 debug=0xA"`，然后 gdb 读 `hv_last_el0_*`、`hv_el0_flt_*` |
| 想知道某个函数执行没执行 | `HV_TRACE_PC=<file>`（不要用 `HV_PCHK` 反推） |
| 想停在用户态某地址 | 同一次运行里先取 `hv_last_el0_va`，再 `HV_PCHK=<低32位> HV_PCHK_CPU=all` |
| 判断磁盘有没有被访问 | `HV_BLK_STATS=5` |
| 判断 USB 走到哪一步 | `HV_VIRTIO_CFG_LOG=1`（guest 侧） + `HV_LIBUSB_LOG=1`（宿主侧） |
| 判断系统调用/返回值 | gdb 读 `hv_svc_*`（按时间序）；注意 `hv_svc_ret` 只有在本轮代码（`hv_el0_note` 无条件记录）之后才可信 |
| 判断有没有崩溃 | gdb 读 `hv_uexc_*` 与 `hv_el0_flt_*`，**看 ESR 的 EC/ISS**：`0x20/0x24 + ISS 0x07` 是正常缺页，`0x0d..0x0f` 才是权限错误 |

### 只用于调试的二进制产物（默认全关，不要随正式版发出）

这些开关写的是几 MB 到几 GB 的文件，只在**复现一个问题**时打开；日常运行、产品构建、交付版本都不要开。
它们的内容是一次性的取证材料，不是日志的补充：日志可以长期保留，这些不行。

| 开关 | 产物 | 大小量级 | 什么时候值得开 |
|---|---|---|---|
| `HV_TRACE_PC=<file>` | 每块 run VA（u64） | 4M 块 = 32 MB | 只判"某段代码到底跑没跑"，必 `HV_PCHK` 零命中时用 |
| `HV_BLOCK_DUMP=<file>` | 翻译后的宿主指令 | 百 MB 起 | 怀疑某个 JIT 块译错 |
| `HV_DT_DUMP=<file>` | 设备树 | 几百 KB | 判 guest 拿到的硬件描述 |
| `HV_DUMP_KC=1` / `HV_DUMP_RAMDISK=1` | 内核 / ramdisk | 十 MB～百 MB | 判镜像是否被正确解出 |
| `HV_DUMP_DC=1` | `__DATA_CONST` 段 | 十 MB 级 | 判 rebase 结果（默认不补，见 §7） |
| `HV_DUMP_RAM=<file>` | 整块 guest RAM | 2 GB 起（2048 MB 配置） | 只在现场必须整体冻结时用 |
| `HV_PTW_LOG` / `HV_PTW_SNAP` | 页表写历史 | 数十 MB | 页表问题；可配合 `HV_SLOW_MEM=1` |
| `HV_BLOCKTIME=1` | 每块耗时采样 | 只是日志 | JIT 慢在哪；每块两次读计数器会扰动时序 |

**规则**：这些产物的用途是"回答一个问题然后删掉"。用完就 `rm`，不要留在 work 目录里等下次
（`workN/` 里一个 `dc-*.bin` 6 MB、`fb-*.bmp` 每张 3 MB，累积起来会占满设备）。

**正式版**：`android/` 的发行构建不应包含这些开关的默认开启，也不应把产物目录导出到外部存储；
用户要提交问题时，只需要 `henge.log` + `henge-serial.log` 两份文本（见 §5 日志架构）。
