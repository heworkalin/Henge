# 汇编特性下限备忘（reims-vgpu-core / guest 内存访问）

> 状态：**备忘 + 待决策**。不是结论，先把事实和选项摊开。
> 起因：交叉编译 `aarch64-linux-android` 时撞到
> `error: instruction requires: lse`，而「直接加个 `+lse` 编译开关」不能算解决。

---

## 一、事实：哪些指令，各自要什么

`crates/reims-vgpu-core/src/guest/ptr/raw/aarch64.rs` 是**唯一**一处 guest 内存访问的
内联汇编（另一处是 testkit，同理）。用到的指令与架构特性：

| 指令 | 用途 | 需要的特性 | 引入版本 |
|---|---|---|---|
| `ldr` / `str` / `ldrh` / `strh` | 普通读写 | 基线 | ARMv8.0 |
| `ldp` / `stp` | 成对读写、清零 | 基线 | ARMv8.0 |
| `ldar` / `stlr` | seq_cst / release 读写（对齐） | 基线 | ARMv8.0 |
| `ldapr` | acquire（RCpc），比 `ldar` 弱 | **FEAT_LRCPC** | **ARMv8.3** |
| `ldsetal` | seq_cst `fetch_or`（唯一的内存 RMW） | **FEAT_LSE** | **ARMv8.1** |
| 上面三条 `*a*` 变体在**非对齐**地址上 | guest 可能读到非对齐地址 | **+ FEAT_LSE2** | **ARMv8.4** |

关键点：**这三条 `*al` 指令的自然对齐用法只需要 ARMv8.1/8.3，非对齐用法才需要 LSE2。**
而代码里对「非对齐」是显式处理的（`within_granule`），所以 LSE2 是真实下限。

---

## 二、这条路径什么时候才会被走到

这是最重要的部分 —— 它能决定成本可以有多低。

`crates/reims-vgpu-core/src/guest/ptr.rs` 的四个原子访问，全部是**先判对齐**：

```rust
pub fn load_acquire_u32(self, off: u64, death: impl Into<DeathId> + Copy) -> u32 {
    let a = check(self.addr, off, 4, death);
    if aligned(a, 4) {
        return raw::atomic_u32(a).load(Ordering::Acquire);   // ← Rust AtomicU32，完全可移植
    }
    if !within_granule(a) { fatal(death) }                   // ← 跨 16 字节 granule：本就按 guest 死亡处理
    raw::ldapr_u32_unaligned(a)                              // ← 只有这里落到汇编
}
```

所以：

| 情形 | 走哪条路 | 需要 LSE2 吗 |
|---|---|---|
| 对齐的原子访问（绝大多数） | `core::sync::atomic::AtomicU32` | **不需要** |
| 非对齐、跨 16 字节 granule | 上游自己 `fatal(death)`（guest 死亡路径） | 不需要，**这本来就是设计** |
| 非对齐、同一 granule 内 | 内联汇编 | **需要** |

也就是说：**非对齐原子访问是唯一的 LSE2 依赖，而且它本来就是「异常路径」。**

---

## 三、最终做法：默认特性编译 + 运行期判定（不加任何编译开关）

一条 `-C target-feature=+lse` 会让**整个 crate**要求 LSE，等于宣布「没有 LSE 的设备不配运行」。
这不接受。改用 Rust 的单函数特性：

```rust
#[target_feature(enable = "lse,rcpc")]
unsafe fn ldsetal_lse2(a: HostPtr, v: u32) -> u32 { /* 内联汇编 */ }

pub(super) fn ldsetal_u32_unaligned(a: HostPtr, v: u32) -> u32 {
    if lse2() {
        unsafe { ldsetal_lse2(a, v) }      // 设备支持：单条原子指令
    } else {
        locked::fetch_or(a, v)             // 不支持：granule 锁 + 普通读写
    }
}
```

要点：

| | 说明 |
|---|---|
| 编译 | **全局仍是 Android aarch64 基线**（`target_feature="neon"`），构建不需要任何 `-C target-feature` |
| 汇编器 | `ldsetal`/`ldapr` 只出现在带特性的函数里，汇编器因此接受 |
| 老设备 | 编译产物仍可运行，走锁路径 |
| 新设备 | 运行期命中快路径，等价上游行为 |

已验证：`cargo check`/`cargo build --target aarch64-linux-android` 全程不带特性开关，
产物里 `ldsetal`、`ldapr` **各出现 1 次**（即只在快路径函数里），证明确实是「按函数」而非「全局」。

---

## 四、运行期怎么判定（这部分是可测的）

判据来自 ARM ARM，而不是猜：**FEAT_LSE2 由 `ID_AA64MMFR2_EL1.AT`（bits [35:32]）标识**。
（注意：不是 `ID_AA64ISAR1_EL1` —— 那个寄存器里的 `[23:20]` 是 LRCPC，不要读错。）

```text
HWCAP_ATOMICS (1 << 8)         LSE 指令是否存在
HWCAP_CPUID   (1 << 11)        EL0 能否读 ID 寄存器
ID_AA64MMFR2_EL1.AT != 0       是否有 FEAT_LSE2
```

`/proc/self/auxv` 里取 `AT_HWCAP`，无需 libc。

**实测（PJE110 / SM8550 / Android 15）**，两种互相独立的方法互证：

```
AT_HWCAP              = 0x00000000efbfffff
  ATOMICS(LSE) bit8   : yes
  CPUID       bit11   : yes
  LRCPC       bit15   : yes
ID_AA64MMFR2_EL1      = 0x0000000100000000
  AT [35:32] = 1      -> FEAT_LSE2 implemented

实测非对齐 ldsetal (+1 / +3，同 granule)：均正常返回，内存被正确置位
```

即：这台设备有 LSE2，运行期会走快路径；而没有 LSE2 的设备会走锁路径，**不会 SIGILL**。

---

## 五、等效写法（供将来若要彻底去掉汇编时参考）

### 5.1 `ldapr` → `ldar`　可移植，代价极小

`ldapr` 是 RCpc（更弱），`ldar` 是 RCsc。用更强的替换永远安全，只是略慢。

### 5.2 `ldsetal`（对齐）→ `ldaxr` / `stlxr` CAS 循环　可移植

```asm
retry:
    ldaxr w_old, [x_a]
    orr   w_new, w_old, w_v
    stlxr w_st,  w_new, [x_a]
    cbnz  w_st, retry
```

ARMv8.0 可用。注意 **LL/SC 要求对齐**，所以它只解决对齐情形。

### 5.3 非对齐（同一 granule）　无硬件等价

这是架构决定的：LL/SC 要地址对齐；ARMv8.0 的非对齐 `ldr`/`str` 不保证单拷贝原子性。
所以只能：

| 方案 | 说明 |
|---|---|
| **granule 锁**（当前降级实现） | 每 16 字节 granule 一把锁（64 条分片），锁内做普通读写。对「同样是原子访问的对手方」正确；**对 guest 侧的非原子写并发只能近似** |
| 拒绝 | 明确报错而非 SIGILL（当前不会发生，因为必定走锁路径） |

### 5.4 基线指令　无需处理

`ldp/stp/ldr/str/ldrh/strh` 只需 ARMv8.0。

---

## 六、现状

| 项 | 状态 |
|---|---|
| 默认特性可编译 | ✅ `cargo build --target aarch64-linux-android` 全程无特性开关 |
| 快路径保留 | ✅ 产物中 `ldsetal` / `ldapr` 各 1 处，仅在被判定的函数内 |
| 老设备可用 | ✅ 锁路径，无 SIGILL |
| 判定依据 | ✅ `ID_AA64MMFR2_EL1.AT`，设备实测与「真跑指令」互证 |
| 产物 | `libreims_vgpu_c.so` 209 MB（未剥离）→ `strip-unneeded` 后 **27 MB**，48 个 `rvg_*` 导出完整；NEEDED 仅 `libdl/libm/libc` |

**遗留的语义差距（如实记录）**：锁路径下，如果 guest 以非原子方式并发写同一批字节，
宿主侧的「原子」访问可能读到撕裂值。上游用硬件 LSE2 正是为了避免这一点。这个差距只在
「无 LSE2 的设备 + guest 真的做了非对齐原子访问」时出现，概率低但不为零。
