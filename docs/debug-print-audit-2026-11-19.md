# 调试打印 / 内存扫描清点

清点对象：`core/ devices/ host/ android/` 下的 C 代码。目标不是"打印更好看"，而是
**每次运行都自动付的代价**：无条件的扫描、热路径上的打印、以及默认日志级别下就会跑的取证代码。

## 1. 每次运行自动执行的内存扫描（代价最大）

| 位置 | 做了什么 | 触发条件（整改前） | 代价 | 现状 |
|---|---|---|---|---|
| `core/dispatch.c` ktrace 块 | 0x70000000..0xf0000000 逐页 `hv_phys_read` 找 kernel header，选中后 rebase | `kimage_pa` 未定 **或 `log>=2`**；Android 默认 log=2 | 最多 2 GB 逐页读 | **保留**：`kimage_pa` 与 rebase 是功能，不是诊断 |
| `core/dispatch.c` `map_done` | 0x83000000..0x84000000 逐页读，扫 `KernelCache`/`RAMDisk` 等字样并打印内存映射 | 同上，每次运行 | 4096 页读 + 打印 | **改**：需 `HV_KMAP_SCAN=1` |
| `core/dispatch.c` `dt_done` | 0x78323c00 的 64 行寄存器 dump、0x78300000..0x78340000 扫 FDT magic、`dt magic`/`dt+` 16 行 | 同上，每次运行 | 256 页读 + 约 94 行日志 | **改**：需 `HV_DT_PROBE=1`（`HV_DT_DUMP` 的树 dump 仍照旧） |
| `devices/vmapple.c` prefault 线程 | 2 GB RAM 每 16 KiB touch 一次 | 每次建机（后台线程） | 131072 次原子操作，实测打印耗时 | **保留**：避免 guest 首次访问慢页 |
| `android/install.c` `scan_proc_names` / `scan_kernel_log` | 全 RAM 扫进程名、msgbuf 里找内核日志关键词 | `HV_SCAN_PROC`、或调用 `hv_android_scan_procs`（SIGUSR1 / 安装结束） | 2 GB 扫 | **保留**：不再在 boot 循环里自动调用 |
| `android/henge_vm.c` `hv_android_boot_framebuffer` | 前 1 GB 每 16 KiB 找 boot_args | **仅** `HV_GRAB_EVERY` 时 | 16384 次检查 | **保留**：默认不抓 |
| `core/exception.c` bvx2 扫描 | fault 时扫前 64 MB 找帧缓冲签名 | 每次 EL0 fault，**限 4 次** | 64 MB | **保留**（已限流） |

## 2. 热路径上无条件打印的（已降级）

| 位置 | 打印 | 整改前 | 现在 |
|---|---|---|---|
| `devices/xhci.c:346` | `xhci: tp request ...`（每笔触摸板控制传输） | 无条件，前 80 次 | `xlog()` 控制 |
| `core/exception.c:1376` | `hvc #N fn ...`（每个 hypercall） | 无条件，前 32 次 | `log>=2` |
| `core/dispatch.c` dt probe | 见上表 | 每次运行 | `HV_DT_PROBE` |

## 3. 路径相关的打印（18 处，按性质）

多数是**写文件成功后打印路径**，属于一次性事件，不是循环里的：

- `core/dispatch.c:2114/2154/2244/2290/2878/2894` — ktrace / kernel dump / dt dump / pctrace，全部由 `HV_*_DUMP`/`HV_TRACE_PC` 触发
- `core/exception.c:505` — EL0 页 dump，由开关触发
- `devices/bdif.c:221` — `log>=2` 或错误
- `devices/vmapple.c:425` — 仅在 `mmap` 失败时
- `host/share.c:846/888/895` — `HV_SHARE_LOG` / 错误
- `android/install.c:880/912/1713` — dump 到文件时
- `android/reims_pvg.c:428` — `p->log`（`HV_REIMS_LOG`）
- `host/net_slirp.c:96/99` — **每包一行**，但有 `HV_NET_LOG` 守护，默认关 ✓

结论：路径打印不是当前的代价来源；真正的代价是第 1 节的扫描和第 2 节的热路径。

## 4. 规则

```
诊断代码可以留在树里，但不能留在默认路径上。
  - 每次运行都付的代价 → 必须有关闭开关，默认关
  - 一次性事件（写文件、错误）→ 可以直接打印
  - 热路径（每块/每包/每请求/每 fault）→ 必须有 log 级别或限流
功能性的扫描（rebase、prefault）不算诊断，不能因为"像诊断"而关掉。
```

## 5. 保留但需要知道代价的

- `HV_LOG=2`（Android 默认）仍会跑 ktrace 的**功能部分**（找 kernel header + rebase），这是启动必需。
- `HV_LOG=3` 打开全部诊断，日志可达千万行，只在定位到具体 pc 时短跑。
- 二进制产物（`HV_TRACE_PC` 等）见 `docs/debug-switches.md` 的对应分节，用完即删。
