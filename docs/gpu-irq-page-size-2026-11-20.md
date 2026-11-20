# GPU 停住的两个真因：宿主/guest 页大小混用，和一条从未送达的中断

记录时间：2026-11-20 19:20 左右。对象：CLI 启动已安装的 macOS（`workB/disk.img`），
2 核 / 2048 MB，reims-vgpu 离屏显示。上一次的记录是 `docs/gpu-stall-2026-11-19.md`。

**结论先说**：11-19 记为"reims 视图/脏页生命周期不一致、崩溃点漂移"的现象，
以及"guest 停在 WFI、`reims-vgpu.fifo` 100% 空转"的现象，各自有一个具体原因，
都已在 reims 的 Android 适配层修掉：

1. **宿主页大小被当成 guest 页大小**（16 KiB）：reims 的 `HostVm::page_size()` 同时被用来
   计算 `mmap`/`mprotect`/`munmap` 的**宿主**粒度，和 guest 结构（描述符表、环）的**guest**
   粒度。Android 宿主是 4 KiB、VMAPPLE guest 是 16 KiB，一个 16 KiB 的粒度让
   `deallocate`/`allocate_fixed_overwrite` 的取整**越过分配边界**，unmap 掉相邻的宿主映射
   （包括 Rust 堆）——于是崩溃点在各处漂移，地址都已不在映射里。
2. **中断向量被当成 INTID**：reims 两个设备都只 `raise(0)`（库自己的编号），我们的回调把它
   原样交给 `hv_gicv3_pend`；GIC 对 `INTID < 32` 静默丢弃（`devices/gicv3.c:109`），
   所以 GPU 中断**一次都没有送达**。引擎自己的报告就是证据：`irq report (boot): (no SPI was
   ever pended)`。guest 的 GPU 驱动写完门铃后等一个永远不会到的中断 —— 这正是"等一个永远
   不可能到达的事件"。

---

## 1. 页大小：一个值服务了两层

`crates/reims-vgpu-vulkan/src/os/linux/vm.rs` 原注释写得很清楚：在 macOS 上宿主页和 guest 页
**都是 16 KiB**，所以一个 `page_size()` 同时服务两层。Android 上不成立。

11-19 的"修法"是把 `page_size()` 在 Android 上直接改成 16384，理由是 IOSurface 的描述符表/环
按 guest 页衡量，4 KiB 会触发 `memory.map.mapping_length_unaligned`。这个理由是对的，
**但改的层次是错的**：同一个函数还是宿主的 `mmap` 粒度。

机制（`vm.rs`）：

```rust
fn pages(addr, len)      { let mask = page_size()-1; first = addr & !mask; end = (addr+len+mask) & !mask }
fn length_pages(addr,len){ let mask = page_size()-1; first = addr & !mask; length = (len+mask) & !mask }
```

`mmap` 返回的地址只保证 4 KiB 对齐。把 `page_size()` 谎报成 16 KiB 后：

- `deallocate` 从"向下取整到 16 KiB 的地址"开始 munmap → 可能连带解掉分配点之前的 12 KiB；
- `allocate_fixed_overwrite` / `remap_shared_overwrite` 用 `MAP_FIXED` 覆盖同样一段 → 直接
  覆盖相邻映射。

受害对象是映射旁边的任何东西：Rust 堆上的 `SerialQueue::Shared`、`DirtyLog` 的排序切片、
guest 内存视图。这解释了 11-19 的三个崩溃符号、三个不同未映射地址：
`EfiDisplay::rebuild_mode`、`DirtyLog::new`、`GuestDisplay::sync_settings`。

### 实测对照

| 运行 | 页大小 | 结果 |
|---|---|---|
| 11-19 各次 | 16 KiB（谎报） | 崩溃点漂移，全是未映射地址 |
| 今天 run3 | 4 KiB（直接回退） | 稳定死在 `reims-vgpu: fatal: memory.map.mapping_length_unaligned`（写 `+0x1010` 描述符基址后） |
| 今天 run4 | 4 KiB 宿主 + core 用 `APV_PAGE_SIZE` | 1039 次 gfx 写、无崩溃、进程存活 |

run3 证明 guest 侧确实需要 16 KiB；run4 证明只需要在 **core 的 guest 语义处**用它。

### 实际改动（reims 子模块）

- `os/linux/vm.rs`：`page_size()` 恢复为 `sysconf(_SC_PAGESIZE)`，注释写明宿主粒度用途。
- `core/iosurface/device.rs`：
  - `HostDevice::page_size()/page_shift()` → `APV_PAGE_SIZE` / `APV_PAGE_SHIFT`（16384 / 14）；
  - `HostDevice::new` 的 `table_len`/`descriptors_per_page` → `APV_PAGE_SIZE`。
- `core/iosurface/descriptor.rs`：客户地址范围的粒度 → `APV_PAGE_SIZE`。

`core/tasks/mapping_task.rs` 的那一处保持用 VM 页大小是对的（注释明说"宿主页大小"）。

## 2. 中断：`vector` 不是 INTID

reims 的 C ABI 只说 `raise_interrupt(ctx, vector)` 是"raise the guest interrupt `vector`"；
实现里两个设备都只发 `0`：

```
core/device/fifos.rs:431     raise(0);
core/iosurface/device.rs:423 raise(0);
```

而 `android/reims_pvg.c` 原来是：

```c
static void on_raise_interrupt(void *ctx, uint32_t vector) {
    hv_gicv3_pend(p->gic, (int)vector);   /* vector = 0 */
}
```

`devices/gicv3.c:108`：

```c
static void hv_gicv3_pend_body(HvGic *g, int intid) {
    if (intid < 32 || intid >= HV_GIC_NUM_IRQS) return;   /* 0 被丢掉 */
```

guest 的 INTID 来自设备树，由 `devices/vmapple.c` 传进来：
GPU = `32 + 0x11`（49），IOSurface = `32 + 0x10`（48）。修法是让回调 pend 设备的线，
不用库的编号：

```c
hv_gicv3_pend(p->gic, p->gfx_irq);    /* on_raise_interrupt */
hv_gicv3_pend(p->gic, p->iosfc_irq);  /* on_iosurface_interrupt */
```

两个设备的第一次抬起各打一行，避免再出现"中断在发生但日志里看不见"。

### 实测对照

| 运行 | 中断 | 形态 |
|---|---|---|
| run4（只修页大小） | `irq report: (no SPI was ever pended)` | 门铃写了 1000+ 次后停住；`reims-vgpu.fifo` 空闲/空转，guest 不再推进 |
| run5/run6（两个都修） | `irq report: 48:17 49:6500+` | guest 进入 EL0 用户态；fifo 线程 0% CPU；进度条继续推进 |

`henge: cpus` 快照（run6，每 30 s 一行）：

```
[0] pc=0000000000100000 el=1  [1] pc=0000000000100000 el=1     ← 复位向量（此时还没起来）
[0] pc=0000000183276df4 el=0  [1] pc=fffffe001c893400 el=1     ← cpu0 已在 EL0 用户态
[0] pc=fffffe001ca35e68 el=1  [1] pc=0000000183059800 el=0     ← 两核在 idle 与用户态之间切换
```

顺带推翻 11-19 记的"CPU1 的 pc=0x100000 是垃圾值"：那是 **arm64 复位向量**，
CPU1 还没被内核拉起时的正常值；拉起后它就是内核/用户态地址。它不是异常，
也不解释任何崩溃。

## 3. 现在的状态

- 无崩溃；GPU 中断在送达；`reims-vgpu.fifo` 不再空转（0% CPU）。
- guest 已进入 EL0 用户态（`0x1832xxxx`/`0x1834xxxx` 一带），在 idle 与用户态之间切换。
- 仍在 boot framebuffer 阶段（进度条），**还没有设置 GPU 显示模式**：
  日志里只有 `+0x1200 = 0`（EFI enable）、`+0x1220 = 0`（显示门铃）、`+0x122c = 1`（端口数），
  没有 `+0x1210/+0x121c/+0x1228`（framebuffer start/mode/stride）写入。
- 离屏显示判定已生效：`display kind = offscreen (no window: frames can be encoded and read back)`。

## 4. 复现与工具

```bash
cd /data/local/tmp/henge
REIMS_VGPU_LOG=warn HV_BOOT_ONLY=1 HV_LOG=1 HV_REIMS_LOG=1 \
  HV_GRAB_FRAME=300 HV_GRAB_EVERY=20 \
  ./host-install AVPBooter.vmapple2.bin workB/disk.img workB 2048 2 > run.log 2>&1 &
```

新增/相关的日志行：

| 行 | 含义 |
|---|---|
| `reims: display kind = …` | 显示判定：window / offscreen / NONE |
| `reims: raise (vector 0) -> SPI 49` | 第一次 GPU 中断抬起（以及 SPI 号） |
| `irq report (boot): 48:N 49:M` | 每 10000 s 的引擎统计；`(no SPI was ever pended)` 表示一条都没送达 |
| `henge: cpus [i] pc=… el=… halted=…` | CLI 每 30 s 的每核快照 |
| `reims: offscreen frame N (WxH)` | guest 已设模式并出帧（离屏） |

reims 重编（改动 Rust 后）：

```bash
cd android/third_party/reims-vgpu
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android33-clang
cargo build --release --target aarch64-linux-android -p reims-vgpu-c \
    --no-default-features --features vulkan     # 约 6.5 分钟
```

## 5. 教训

1. **一个值服务两层，在跨平台时一定会炸。** 页大小在 macOS 上"恰好相同"掩盖了这一点；
   Android 的 4 KiB/16 KiB 差异把它暴露成内存破坏，而不是参数错误。
2. **"崩溃点漂移"优先怀疑内存破坏，而不是"某模块生命周期不一致"。** 三个不同符号、三个
   未映射地址、且都能通过改无关配置改变位置 —— 这是 unmapped/覆盖型破坏的指纹。
3. **中断没送达要看 GIC 自己的计数**，不要从 `raise_interrupt` 的"调用成功"推断。
   `hv_gicv3_pend_body` 对 `INTID < 32` 静默丢弃，这是刻意的（SGI/PPI 不走这条路径），
   但库的 `vector` 不是 INTID。
4. **guest CPU 的 pc 要看两次以上**：复位向量 `0x100000` 是正常起点，不能当成"垃圾值"。
