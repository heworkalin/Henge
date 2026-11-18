# henge-gdb — 现场探针（gdb 内嵌 Python）

gdb attach 到引擎进程（`host-install`）后，用来读 guest 现场的只读脚本。
**不是产品代码**，只在调试时用；机器必须停在现场（`HENGE_STOP_ON` 或 `stop_cpus()`）。

## 与 `tools/dump-*.py` 的分工

| 目录 | 内容 |
|---|---|
| `tools/dump-*.py` | 早先写的、针对**阻塞链路**的脚本（线程/栈/turnstile/vmmap-entry/syscall 路径）。地址从引擎记录的环里取，已入库。 |
| `tools/henge-gdb/` | 这里。**同一批现场探针整理后的参数化版本**：不再硬编码某一轮的 map/pmap/VA 地址，改为自动发现 + 环境变量传参。 |

两者共用同一个 `tsbase.py`（设备上路径 `/data/local/tmp/henge/tsbase.py`，
`dump-*.py` 通过 `exec(...split('TH = {')[0])` 引入它）。

## 部署

```bash
adb push tools/henge-gdb/*.py /data/local/tmp/henge/
```

运行（设备上已有 `gdb-bundle/bin/gdb`）：

```bash
adb shell "cd /data/local/tmp/henge && PID=\$(pidof host-install) && \
  HENGE_VA=0x100fc0000 ./gdb.sh -p \$PID -batch -ex 'set print thread-events off' \
  -x walkva.py"
```

## 脚本与参数

所有地址参数都接受十进制或 `0x` 十六进制；未给时尽量从引擎状态自动推断。

| 脚本 | 作用 | 参数 |
|---|---|---|
| `faults.py` | 打印最后 N 条 EL0 缺页：ESR 类型、pc/far/ttbr0，以及该页所在 `vm_map_entry` 与当前 PTE | `HENGE_FAULTS=8` |
| `walkva.py` | 逐级打印页表 descriptor（16 KiB granule），可扫一段 VA | `HENGE_VA=`（逗号分隔）、`HENGE_RANGE=lo:hi[:step]`、`HENGE_TTBR0=`、`HENGE_ENTRY=1` |
| `vmmap.py` | 按签名扫描 `vm_map`/`pmap`，打印 min/max/nentries/page_shift/**ttb0** | `HENGE_MAP_PA=`、`HENGE_PMAP=`、`HENGE_VA=`、`HENGE_DETAIL=1` |
| `entries.py` | 逐 entry 统计驻留率（full/partial/**零驻留**），并报某个 VA 落在哪个 entry | `HENGE_MAP_PA=`、`HENGE_VA=`、`HENGE_MAX_PAGES=64`、`HENGE_LIMIT=20` |
| `images.py` | 指名某个 PC 属于哪张 Mach-O image；可解 adrp 指向的 GOT 槽 | `HENGE_PC=`、`HENGE_IMAGE_RANGE=lo:hi[:step]`、`HENGE_GOT_PC=`、`HENGE_GOT_OFF=` |
| `procs.py` | 列出进程（pid/ppid/stat/comm），可选线程状态与 turnstile | `HENGE_COMM=`、`HENGE_THREADS=1`、`HENGE_TURNSTILE=1` |
| `blk.py` | `root_blk`/`aux_blk` 的 requests/submitted/completed/inflight | — |
| `kcdump.py` | 把运行期内核镜像从 guest RAM 拷出来（喂 `tools/lcfs.py`/`ksym.py`） | `HENGE_KC_PA=`、`HENGE_KC_LEN=`、`HENGE_OUT=` |
| `setdbg.py` | 开关 `hv_walk_dbg`（引擎的页表遍历诊断输出） | `HENGE_WALK_DBG=stderr\|0` |

## 地址为什么能自动发现

`vmmap.py`/`entries.py`/`faults.py` 不硬编码地址。`vm_map` 靠字段签名找：

```
+0x03 lck_rw_type == 0x33 (LCK_TYPE_RW)   +0x04 padding == 0
+0x20 min_offset < +0x28 max_offset，跨越 0x1000000000
+0x30 nentries    +0x34 page_shift==14(16 KiB)    +0x40 pmap（内核堆指针）
```

entry 链两种布局都试（RB 根在 `map+0x18`，链表头在 `map+0x08`），entry 内是
`{prev+0x00, next+0x08, start+0x10, end+0x18}`。pmap 的 ttb0 在 `pmap+0x08`。
这些偏移是 **xnu-11417.140.69 / VMAPPLE** 的实测值，见
`docs/vm-map-structure-2026-11-17.md`、`docs/locating-vm-structures-2026-11-17.md`。

## 三条使用铁律

1. **`walk()` 用的是 engine 里那个 CPU 的 TTBR0。** CPU 在 idle 时那是内核页表，
   拿它查用户 VA 必然 UNMAPPED。查用户页要传 `HENGE_TTBR0=`（fault 环里每条自带）。
2. **按 16 KiB granule 算**（`TCR.TG0=2`），不要按 4 KiB 心算索引。
3. **gdb attach 时进程是暂停的。** 同一次会话内多次采样看到的是同一时刻；
   要观察“变化”必须 detach → 等 → 再 attach。

## 历史

2026-11-18 前的 161 个一次性现场脚本（`vmfind/vmsig/cmpall/entperm/ucal/flt3/...`）
已归档在 `work/henge-py-archive-2026-11-18.tar.gz`（不进 git），只保留其中仍有
复用价值的思路，整理为本目录。
