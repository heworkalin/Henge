# 2026-11-15 调查记录：内核静默崩溃的真凶是引导阶段的数据覆盖

> ⚠️ **归因已被取代：读之前先看 `simd-clobber-kernelcache-2026-11-15.md`。**
>
> 本文的**观察**全部成立（iBEC 确实在 `pc 0x700a7fec` 把新产生的字节写进 `kernelcache + 0x9EF860`，
> `kt_flags` 确实变成 `0x2`），但**「坏数据来自 staging / 组件布局撞车」这个归因是错的**。
> 那些字节是 iBEC **自己解压出来**的：引擎没在 helper 和 TLB refill 两条路径上保存 guest 的
> `v0-v31`，iBEC 的 LZFSE 解码器状态（`q0-q2`）被破坏，解压结果就是错的。
> 这也正好解释本文自己记的那条反常事实——「坏值在 IPSW 里 0 命中，是新产生的字节」。
>
> 本文的**证据、地址、工具**仍然有效，继续当查表用；只有第六节的「未确证（还剩一条线索）」
> 和第七节的「下一步」作废。

> 本文记录一次完整排查的结论、证据等级与工具。**已确证**指有实测日志/断点/反汇编支撑；
> **未确证**单独标出。上一轮的排除清单见 `kernel-boot-exclusions.md`（E1–E18、H1/H2、X1–X6）。

## 一、一句话结论

内核不是被引擎算错的，是**启动时 iBoot 把镜像数据写进了内核镜像的中间**，破坏了
`kalloc_type` 的 view 记录，内核随后在 `kalloc_type_validate_flags()` 里断言失败并 panic；
而 panic 打印 `%s`（原因字符串）时又从这个被改坏的指针取地址，于是在异常向量里连环缺页、
最终停在 `wfe` 自旋 —— 串口从未初始化，所以外部看到的就是"静默 + MACH Reboot"。

## 二、已确证的因果链

```
iBEC 的 memcpy（pc 0x700a7fec，16 字节 stnp 一步）
  写到 PA 0x74507860        ← = 内核镜像基址 0x73b18000 + 0x9EF860
    覆盖了 ktyp view（VA 0x1c507850）的 kt_name
      kt_name = 0x00020000935144        （镜像文件在该处是 fffffe001bdf8f96）
        kalloc_type_validate_flags() 断言失败
          panic("kalloc_type_view(%s) from kext(%s) hasn't been rebuilt …" @kalloc.c:705)
            panic 打印 %s → 读 0x20000000935144 → level-0 转换失败（esr 96000004）
              进异常向量 → 写异常栈 sp_el0=0x1f2eff80（该页 ap=2 只读，esr 9600004f）
                处理该异常又写栈 → 栈一路下降 → TPIDR_EL1 为空
                  locore.s 的 "can only continue if TPIDR_EL1 is set" → wfe 自旋 → halt x2 "MACH Reboot"
```

## 三、每一环的证据

| # | 结论 | 怎么得到的 |
|---|---|---|
| 1 | panic 原文 | 从镜像里读出 `0x1bb5d921` 处的 `"panic(cpu %u caller 0x%lx): "`；从 `0x1d16e9f0` 附近的两条 `adrp+add` 读出 `kalloc_type_view(%s)…@kalloc.c:705` |
| 2 | 函数身份 | `w27 = 0x60` 与 `KT_CHANGED(0x20)\|KT_CHANGED2(0x40)` 吻合；`orr w8,w8,#0x4000` 与 `KT_SLID` 吻合（`osfmk/kern/kalloc.h`） |
| 3 | 完整调用链 | lvl0 fault 时按 `[x29+8]` 走帧链：`1c8e54b8 → 1ca3ffbc → 1ca3e514 → 1c894d10 → 1c8e5828 → 1d16c930 → 1d16e9f8 → 1d16ee58 → 1d16eaa0 → 1d16d030` |
| 4 | 犯规的 view | 在 panic 入口 `0x1d16e9cc` 下 `HV_PCHK` 断点，`x21 = 0x1c507850` |
| 5 | 内存与镜像不符 | 同一槽位：VA 读与 PA 读一致（都是 `0x20000000935144`），PA = `VA + 0x58000000` = `0x74507860`，换算无误；镜像在对应文件偏移处是 `fffffe001bdf8f96` |
| 6 | 写入者 | `hv_phys_write` 里的 watch：`write to pa 74507860 = 20000000935144 (16 bytes) by pc 700a7fec lr 700a8460` |
| 7 | 写入者是固件 | `pc/lr` 都在 `0x700xxxxx`（iBEC 区），反汇编 `0x700a7fec` 是 `stnp x8,x9,[x3]` 的拷贝循环 |
| 8 | 同一例程也写 DT | `write to pa 78324000 … lr 700a8460` —— DT 与组件由同一个 memcpy 包装搬入 |
| 9 | 数据来自 staging | 拷贝参数 `src` 落在 `0xb49d…`；该缓冲区的写入者是 iBEC 自己（`lr 0x700b37f8`），即 USB 收到的数据先落 staging 再分发 |
| 10 | 我们送的 DT 没有 memory-map | `tools/adt.py` 完整解析 `work/dt.bin`（262140/262144 字节）；`grep` 确认 `chosen` 存在但 `memory-map`/`RAMDisk`/`TrustCache`/`KernelCache` **一次都没有** |

## 四、顺带修掉的、会误导判断的坑

| 坑 | 后果 | 修法 |
|---|---|---|
| `android/install.c` 用 `setenv(..., 1)` 强制覆盖 `HV_WATCH_VA/PA` | 命令行传的监视点**从未生效** | 改为 `overwrite=0` |
| watch 只挂在 `hv_vmem_xfer`（guest 慢路径） | 设备/固件的 `hv_phys_write` 全部绕过 | 用 `hv_phys_write` 里已有的 watch（打印 `pc` 与 `lr`） |
| `HV_PCHK` 用完整 64 位 pc 比较 | 内核 pc 带 `0xfffffe00` 高半，永远不匹配 | 只比低 32 位 |
| `HV_PCHK` 只在**块入口**触发 | 循环体内的指令看不到，且扫描很快耗尽上限 | 加 `HV_PCHK_DST` / `HV_PCHK_LEN` 过滤 |
| 曾以为 `x24 = 0x4000` 是"一次 16 KB 写" | 推出"16 KB 块重叠"的错误结论 | 实际是循环的**剩余计数**；固件一次只写 16 字节 |

## 五、被否掉的假设（不要再走）

- **PAC 模型**：`put_strip` 只会产出 `0x0000`/`0xffff` 高位，永远造不出 `0x0002`（实测指令序列验证）
- **`__DATA` / `__DATA_CONST` rebase 方向**：`__DATA_CONST` 由内核自己 slide（`0x1d177750`），`__DATA` 必须我们补（`arm_init` 前面就要读）
- **双倍 slide**：`kalloc_type_slide()` 自己会打 `KT_SLID`，且我们 fixup 只落在 `seg5 __DATA`（19466 个槽），`__kalloc*` section 根本不存在
- **页表翻译**：walk 得到的 PA 与 `VA + 0x58000000` 精确相符
- **宿主的 SIGSEGV**：glibc/proot 产物；bionic CLI 上 0 次崩溃
- **AES 解密错**：DT 能解析成结构合法的 ADT，内核镜像能反汇编出与源码一致的代码
- **"内核栈页只读"是根因**：它是 panic 打印失败后栈被走穿的**后果**

## 六、未确证（还剩一条线索）

坏值 `0x00020000935144` 是**在 staging 里就坏**（我们发出去的数据有问题），还是
**固件把它写到了不该出现的位置**（布局/重叠）。

- 数据本身经过 TSS 签名校验，iBoot 也没报签名错 ⇒ 倾向后者
- 我们送的 DT **没有 `/chosen/memory-map`** ⇒ 所有组件的落点由 iBEC 自己决定，
  和内核镜像撞车是必然的

## 七、下一步（唯一动作）

在 `lr = 0x700a8460`（那个拷贝包装的返回地址）记录**每一次**调用的
`(x22 = dst, x23 = src, x24 = len, icount)`，按 `dst ∈ [0x73000000, 0x7a000000)` 过滤，
不做次数上限。然后找满足

```
dst <= 0x74507860 < dst + len
```

的那一条：

| 结果 | 结论 | 修法 |
|---|---|---|
| `dst = 0x73b18000` 附近，即 kernelcache 自己 | 数据/解密问题（但已被弱证据否定） | 查 personalize |
| `dst` 是另一组件的起点 | **布局冲突** | 让 DT 带上 `/chosen/memory-map`，或改 `dram-base` 把区间分开 |

## 八、工具与命令（今天新增，都可复用）

```bash
# 设备上跑 CLI（1 分钟一轮，bionic，无 proot 干扰）
ZLIB=1 API=28 ./tools/build-install-android.sh
adb -s 127.0.0.1:5555 push build/andcli/host-install /data/local/tmp/henge/
adb shell "cd /data/local/tmp/henge && ./host-install AVPBooter.vmapple2.bin \
           /data/local/tmp/ipsw/24G90.ipsw workN 4096 2 > run.log 2>&1"

# 探针
HV_PCHK=start:end         # 块入口寄存器快照（含 dst PA、x22/x24、slide）
HV_PCHK_DST=lo:hi         # 只记录 dst 落在区间内的命中
HV_PCHK_LEN=hex           # 只记录长度不小于该值的命中
HV_WATCH_PA=hex / HV_WATCH_VA=hex   # 物理写监视，打印 pc 与 lr
HV_COPY_LOG=1             # 记录 len >= 0x1000 的物理写（注意：固件只按 16 字节写，通常为空）

# 离线
./tools/adt.py work/dt.bin              # Apple device tree（ADT）解析，memory-map 自动成表
./tools/disasm-raw.sh <blob> <base> [from] [to]
```

## 九、关键地址备忘

| 项 | 值 |
|---|---|
| 内核镜像 | VA 基址 `0xfffffe001bb18000`，PA `0x73b18000`；`VA = PA + 0x58000000` |
| virtBase / physBase | `0xfffffe0018a04000` / `0x70a04000`（boot_args，rev 3 ver 2） |
| `boot_args` | PA `0x837f0000`：`memSize 0xfeb38000`、`topOfKernelData 0x837f4000`、`deviceTreeP 0xfffffe0020324000`（→ PA `0x78324000`）、`CommandLine "rd=md0 -progress -restore -v"` |
| 犯规 view | VA `0x1c507850`（+0x10 = kt_name，+0x28 = kt_flags，fixed 变体，步长 0x40） |
| 被写坏的槽 | PA `0x74507860` = 镜像 + `0x9EF860` |
| panic 调用 | `pc 0x1d16e9f4`；断言点 `0x1d16e9d8`（行号 `0x2c1` = 705） |
| `kalloc_type_slide` | `0x1d16e8d0` 起；`vm_kernel_slide` 从 `[x26+0x8c0]` 读，实测 `0x14b14000` |
| 固件拷贝 | 循环 `0x700a7fec`（`stnp`），调用者 `lr 0x700a8460` |
| `test-regs` 等 | `./android/test-regs.sh`、`test-bitops.sh`、`test-lzfse.sh` |
