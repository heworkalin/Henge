# boot-args 的两种注入方式，只有一种是"追加"（2026-11-19）

**一句话**：给 guest 内核加 boot-args，只能用 `HV_EXTRA_BOOT_ARGS`（追加到 boot_args 结构）。
`HV_RESTORE_BOOT_ARGS` 会把参数拼进 iBoot 自己的 `setenvnp boot-args ...` 命令里，
命令一变长，**iBoot 就不进恢复模式了**——boot_args 结构根本没被写，内核拿不到任何 boot-args。

本文记录实测对照与后果，因为这件事在一天里让我们误判了多轮。

---

## 1. 两条路不一样

| 开关 | 落点 | 语义 |
|---|---|---|
| **`HV_EXTRA_BOOT_ARGS`** | `core/dispatch.c` 里 boot_args 结构的 `CommandLine`（PA `0x837f0000` + `0x6c`） | **追加**：`memcpy(cmd + n + 1, extra, ...)`，原有内容完整保留 |
| **`HV_RESTORE_BOOT_ARGS`** | `third_party/idevicerestore/src/{dfu,recovery}.c` 发出的 iBoot 命令字符串 | **改命令**：`setenvnp boot-args rd=md0 nand-enable-reformat=1 -progress -restore <extra>` |

第二条看起来"更底层"，实际是**动 iBoot 的输入**，不是动内核的结构。

## 2. 实测对照（同一台设备、同一 booter、同一 IPSW、RAM 都是 2048）

### 2.1 CLI：只设 `HV_EXTRA_BOOT_ARGS`（正确）

```
setenvnp boot-args rd=md0 nand-enable-reformat=1 -progress -restore           ← 37 字符，无 serial
setenv   boot-args rd=md0 nand-enable-reformat=1 -progress -restore

hengevm: boot-args "rd=md0 -progress -restore serial=3 debug=0xA" (dt length 555200)
hengevm: boot_args rev 3 ver 2
hengevm: ba[ 0] 20003          ba[ 1] fffffe0018a04000   ← virtBase
hengevm: ba[ 2] 70a04000       ← physBase                ba[ 3] 7eb38000
```

结果：**ASR 12% → 90%**，disk 实际写入数 GB。

### 2.2 APK：额外设了 `HV_RESTORE_BOOT_ARGS`（错误）

```
setenvnp boot-args rd=md0 nand-enable-reformat=1 -progress -restore serial=3 debug=0xA   ← 56 字符
setenv   boot-args rd=md0 nand-enable-reformat=1 -progress -restore serial=3 debug=0xA

hengevm: boot-args " serial=3 debug=0xA" (dt length 0)
hengevm: boot_args rev 0 ver 0
hengevm: ba[ 0] 0   ba[ 1] 0   ba[ 2] 0   ba[ 3] 0
```

结果：**内核串口零输出**、`disk blocks=0`、恢复进度停在 step 1（prepare）。

`rev 0 ver 0` 是决定性证据：那是**全新零页**，不是被覆盖，而是**从来没被写**。
`" serial=3 debug=0xA"` 前面那个空格说明追加落在一个空 `CommandLine` 上。

### 2.3 修好之后（只留 `HV_EXTRA_BOOT_ARGS`）

```
setenvnp boot-args rd=md0 nand-enable-reformat=1 -progress -restore       ← 恢复成 37 字符
hengevm: boot-args "rd=md0 -progress -restore serial=3 debug=0xA" (dt length 556600)
hengevm: boot_args rev 3 ver 2

Darwin Kernel Version 24.6.0 ... xnu-11417.140.69~1/RELEASE_ARM64_VMAPPLE
com.apple.xpc.launchd|2026-11-18 17:35:29 <Notice>: Early boot complete. Continuing system boot.
```

内核日志、launchd、时间戳（与宿主 UTC 一致）全部正常。

## 3. 为什么"命令变长"就等于"不进恢复模式"

**推断，未核对 iBoot 源码**：iBoot 的 `setenvnp` 对 `boot-args` 有解析/长度约束，
被追加 `serial=3 debug=0xA` 之后它不再按 restore 模式装配 boot_args 结构，
于是内核收到的是一个空的 boot-args（rev 0 ver 0）。

**能确认的事实**：命令变长 → boot_args 结构未写 → 内核无 boot-args。
**不能确认的**：iBoot 内部具体是哪一步拒绝的。按项目规则，这里记为"实测现象"，
不是"机制解释"。

## 4. 结论与规矩

1. **给 guest 加 boot-args 只用 `HV_EXTRA_BOOT_ARGS`。**
2. **不要用 `HV_RESTORE_BOOT_ARGS` 去加 `serial=3`**——它改的是 iBoot 的命令，不是内核的结构。
   （CLI 的成功运行从头到尾只设了 `HV_EXTRA_BOOT_ARGS`。）
3. App 侧要加这两个参数，就在 `hv_android_install_start` 里 `setenv(..., 0)`：
   **overwrite = 0**，调用方显式设置的值永远优先。
4. **不要为了"让日志出来"去改 `core/dispatch.c` 的注入逻辑**。
   那次改动（等页填好再注入）虽然方向不错，但覆盖了原有行为，
   而真正的原因是上一条——把参数塞进了 iBoot 的命令里。
5. 判断内核有没有拿到 boot-args，看三个值就够：
   `dt length`（0 或 ~55 万）、`boot_args rev/ver`（0/0 还是 3/2）、`henge-serial.log` 有没有内核文本。
