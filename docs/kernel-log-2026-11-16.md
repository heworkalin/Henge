# 如何让内核日志（kprintf / IOLog）可见（2026-11-16）

> 目的：Henge 里内核的 `kprintf`/`IOLog` 默认**全部丢弃**，导致"内核自己说的话"看不见。
> 本文是打通它的完整配方（含踩过的坑），一次成功约 60 秒。

## 1. 结论：三个条件必须同时满足

| 条件 | 作用 | 取值 |
|---|---|---|
| boot-arg `serial=3` | 让 `arm_init` 调 `switch_to_serial_console()` | `SERIALMODE_INPUT(0x1) \| SERIALMODE_OUTPUT(0x2)` |
| boot-arg `debug=0xA` | 打开 `disable_serial_output` | `DB_PRT(0x2) \| DB_KPRT(0x8)` |
| boot-args 真的写进了 `boot_args->CommandLine` | 否则上面两条都是空谈 | `core/dispatch.c` 的注入点有坑，见第 3 节 |

三条缺一不可：

- 只给 `serial=3`：串口会被配置（能看到 `pl011 +0x0024/+0x0030` 的写），但 `kprintf` 仍被
  `disable_serial_output = true` 挡住，**一个字符都不发**。
- 只给 `debug=0xA`：`disable_serial_output` 开了，但 `serialmode = 0` → 不
  `switch_to_serial_console()` → 字符走未初始化的 video console → 仍看不到。
- 两个都给但注入代码没生效：等于什么都没给（见第 3 节，本项目长期如此）。

## 2. 源码依据（xnu-11417.140.69，与 24G90 的 kernelcache 一致）

`pexpert/arm/pe_kprintf.c`：

```c
SECURITY_READ_ONLY_LATE(bool) disable_serial_output = true;   /* 默认就是关 */

__startup_func static void PE_init_kprintf_config(void) {
    if (debug_boot_arg & (DB_KPRT | DB_PRT)) {
        disable_serial_output = false;                        /* 只有 debug 位才开 */
    }
}
STARTUP(TUNABLES, STARTUP_RANK_LAST, PE_init_kprintf_config);

void kprintf(const char *fmt, ...) {
    if (!disable_serial_output && !disable_kprintf_output) { ... }   /* 否则全丢 */
}
```

`osfmk/kern/debug.h`：`DB_PRT = 0x2`、`DB_KPRT = 0x8`。

`osfmk/arm/arm_init.c`：

```c
const unsigned int serial_exists = serial_init();          /* 从 DT 枚举 uart0 */
kernel_startup_initialize_upto(STARTUP_SUB_KPRINTF);
kprintf("kprintf initialized\n");                          /* 此时门还没开，看不到 */
serialmode = 0;
if (PE_parse_boot_argn("serial", &serialmode, sizeof serialmode)) { ... }

if ((serialmode & SERIALMODE_OUTPUT) && serial_exists) {
    serial_console_enabled = true;
    (void)switch_to_serial_console();
    disableConsoleOutput = FALSE;                          /* 这一行之后 printfs 才出得去 */
}
PE_create_console();
```

注意 `kprintf("kprintf initialized")` 在 `switch_to_serial_console()` **之前**，
所以这句本身永远看不到 —— 不要拿它当"日志没通"的判据。

`serial_init()` 的前置条件（`pexpert/arm/pe_serial.c` / `pe_identify_machine.c`）：

```c
soc_base = pe_arm_get_soc_base_phys();      /* = arm-io 的 ranges[1]，为 0 就直接 return 0 */
if (soc_base == 0) return 0;
...  /* 再用 DT 的 /defaults/serial-device → uart0 的 compatible "arm,pl011" 选驱动 */
```

本机 DT 实测（`tools/adt.py`）：

```
/arm-io/ranges              = 0x0 → 0x1000000000, size 0x700000000   （非零，OK）
/arm-io/uart0/compatible    = "arm,pl011"
/arm-io/uart0/reg           = 0x10010000, size 0x10000
/defaults/serial-device     = 0x38 (uart0 的 phandle)
```

## 3. 注入点与那个坑（本项目长期失效的原因）

`core/dispatch.c`，在内核第一次执行时改 `boot_args`（PA `0x837f0000`，`CommandLine` 在
偏移 **`0x6c`**）：

```c
uint64_t *q = hv_phys_ptr(m, 0x837f0000ull, 0x1000);
char *cmd = (char *) q + 0x6c;            /* boot_args->CommandLine */
```

**原来的写法有 bug**：注入代码被放在了
`if (was && was < (16u << 20)) { *len = 16u << 20; ... 注入 ... }`
这个分支里。而它上面那个"扫描找 boot_args"的循环**已经**把 `deviceTreeLength` 改成了 16 MB，
于是这里的 `was == 16777216`，条件不成立，走 `else` 只打印
`hengevm: boot_args at 837f0000 has dt length 16777216; scan found none`
—— **注入从来没执行过，`-v` 也一直没加上**。

修复：把注入移到 `if/else` **之外**（只要拿到 `q` 就执行），并且用环境变量扩展：

```c
char *cmd = (char *) q + 0x6c;
if (!getenv("HV_QUIET_BOOT")) {
    size_t n = strnlen(cmd, 1000);
    if (!strstr(cmd, "-v") && n + 3 < 1024) { memcpy(cmd + n, " -v", 4); n += 3; }
    const char *extra = getenv("HV_EXTRA_BOOT_ARGS");      /* 例如 "serial=3 debug=0xA" */
    if (extra && *extra) {
        size_t en = strlen(extra);
        if (n + en + 2 < 1024) { cmd[n] = ' '; memcpy(cmd + n + 1, extra, en + 1); n += en + 1; }
    }
    fprintf(stderr, "hengevm: boot-args now \"%s\"\n", cmd);
}
```

跑起来后**第一件事**是确认这行：

```
hengevm: boot-args now "rd=md0 -progress -restore -v serial=3 debug=0xA"
```

没有这一行 ⇒ 注入没生效，后面都不用看。

## 4. 输出路径

`devices/pl011.c` 的默认输出直接写 stdout：

```c
static void default_output(void *opaque, uint8_t c) {
    fputc(c, stdout);
    if (c == '\n') fflush(stdout);
}
```

Henge 会给每行加前缀，所以在 run 日志里 greppable：

```
hengevm: serial: Darwin Kernel Version 24.6.0: ... RELEASE_ARM64_VMAPPLE
hengevm: serial: [ PCI configuration end, bridges 1, devices 9 ]
hengevm: serial: AppleARMLightEmUp::start(IOService *): starting...
```

## 5. 复现命令（一轮约 60 秒）

```bash
cd /home/he/Henge
ZLIB=1 API=28 ./tools/build-install-android.sh
adb -s 127.0.0.1:5555 push build/andcli/host-install /data/local/tmp/henge/

adb -s 127.0.0.1:5555 shell 'chmod 755 /data/local/tmp/henge/host-install; \
  cd /data/local/tmp/henge && HV_LOG=2 HV_EXTRA_BOOT_ARGS="serial=3 debug=0xA" \
  nohup ./host-install AVPBooter.vmapple2.bin /data/local/tmp/ipsw/24G90.ipsw workSR 4096 2 \
  > runSR.log 2>&1 & echo started'

# 只看日志是否通
adb -s 127.0.0.1:5555 shell 'cd /data/local/tmp/henge && \
  grep -a "hengevm: serial:" runSR.log | tail -20'
```

验证成功的三个标志：

1. `hengevm: boot-args now "... serial=3 debug=0xA"`
2. `hengevm: serial: Darwin Kernel Version ...`（内核版本行）
3. MMIO histogram 里出现 `pl011 +000000 r 0 w N`（`N > 0`，即 DR 写）

## 6. 排错表

| 现象 | 原因 |
|---|---|
| 没有 `boot-args now` 行 | 注入代码没执行（见第 3 节的 `if` 分支坑），或 `HV_QUIET_BOOT` 被设置 |
| 有 `boot-args now` 但无 `serial:` 行 | 缺 `debug=0xA`（`disable_serial_output` 仍为 true），或 boot-args 里 `serial=` 拼错 |
| 有 `serial:` 行但内容零散/乱码 | 波特率不匹配（PL011 侧配置），不影响文本判读 |
| `pl011 +0x0018`（FR）读很多但 `+000000`（DR）w 为 0 | 只有轮询没有字符 → 仍是 `disable_serial_output` 或 `serialmode` 未生效 |
| 完全没有 `pl011` 访问 | `serial_init()` 提前返回：检查 `arm-io` 的 `ranges` 是否解析出非零 `soc_base` |

## 7. 顺带纠正的一个错误结论

拿到日志后立刻发现：**kext 其实一直在 start**。

```
hengevm: serial: IOPortFamily::registerService(): ... (providerPath: IOService:/AppleVirtualPlatformARMPE/port-virtual-1, ...)
hengevm: serial: AppleARMLightEmUp::start(IOService *): starting...
hengevm: serial: AppleMobileApNonce::start(IOService *)
hengevm: serial: IOMobileFramebufferLegacy::start(IOService *)
```

所以此前"`__PRELINK_TEXT`（`0x1bb20000..0x1c210000`）执行 0 块 ⇒ kext 没跑"的推论**不成立**：
24G90 是 `MH_FILESET`，kext 文本并不落在那个区间。`hv_seg_note()` 的分段统计把 kext 归零，
是地址范围用错的假象。**以后判断"某代码有没有跑"一律以日志/实际 PC 轨迹为准，不要只信段范围。**
