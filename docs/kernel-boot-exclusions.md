# 内核启动失败排查：已排除项与现存假设

本文记录从「内核跑到 `wfe` 自旋、restore 超时」到现在为止**已经排除掉的可能性**，以及
每一条是怎么排除的。目的是不再重复走同一条路，也让后来接手的人知道哪些方向已经关上了门。

结论先行：**问题已经收敛到内核自己的内存分配记账上**，不在传输、不在解码、不在我们
翻译器的算术、也不在页表翻译。剩下的候选只有两条，见文末。

- 内核：`xnu-11417.140.69 / Darwin 24.6.0 / RELEASE_ARM64_VMAPPLE`
- 镜像 VA 基址：`0xfffffe001bb18000` ↔ PA `0x73b18000`（**注意：不是 `0xfffffe001b3dc000`**）
- slide：`0x14b14000`
- 失败表现（设备与本地一致）：`Unable to place device into restore mode`，`step 101 (100%) restore failed`

---

## 一、已排除项

编号沿用下面的顺序，括号里是证据落在哪次提交或哪个文件。

### E1 传输侧把镜像弄坏了 —— 排除

USB 送出去的字节与 IPSW 里的 kernelcache **逐字节相同**：IM4P `rkrn` /
`KernelManagement_host-463.100.7`，LZFSE 解出 **75,546,624** 字节，与 IPSW 提取物完全一致。

补充验证：512 MB RAM 快照里，载荷前 **13.1 MB** 与 USB 送出的字节完全一致；首个不同位置
`0x73b14000` 正好是内核镜像落地覆盖载荷尾部的地方。

→ 发送链路干净，镜像没有损坏。（`3c31f20`）

### E2 我们的 LZFSE 解码器（JIT 执行真解码器）不对 —— 排除

把 Apple 开源 LZFSE 编进本地测试脚手架，整份可执行文件注册成 guest RAM，同一段代码
native 跑一遍、走 JIT 跑一遍，比对输出：

```
native decoded 75546624 bytes
native vs reference: 0 of 75546624 differ
jit returned 7 (x0 = 75546624)
output differs: 0 of 75546624
```

（`bf1e4d3`，`android/test-lzfse.sh`）

### E3 位运算 / 移位 / 位域被翻译错 —— 排除

`tests/bitops_prog.s` 覆盖 `ubfx/sbfx/bfxil/ubfiz/sbfiz/extr/ror/clz/cls/rbit/rev/rev32/
sbfm/ubfm/bfm` 与 NEON 的 `ushll/sshll/xtn/sqxtn/uqxtn/addv/addp/zip/uzp/trn/ext/tbl/tbx/
shl/sshr/ushr/sli/sri/movi/eor/and/orr/bic/not/cnt`：**0 差异**；`ROUNDS=2000` 随机操作数
fuzz 也 0 分歧。

### E4 非对齐 / 跨页的内存访问被翻译错 —— 排除

同一脚手架的 `memops` 程序：非对齐 16 字节 `stp/ldp` 跨页、`ldr/str q` 各种对齐、
post-index 写回，8192 字节结果 **0 差异**。

### E5 乘 / 除 / 进位 / 条件比较被翻译错 —— 排除

`mathops` 程序覆盖 `mul/madd/msub/mneg/smulh/umulh/smull/umull/smaddl/umaddl/smsubl/
umsubl/sdiv/udiv`（含除零与 `INT_MIN / -1`）、`adc/sbc/adcs/ngc`、`lslv/lsrv/asrv/rorv`、
`ccmp/cset`、`sxtw/uxtw/sxtb`：**0 差异**。

> 这一条中途出现过一次假警报：测试脚手架编译产物没有跟踪头文件，`machine.o` 比
> `machine.h` 旧，导致引擎和 harness 用了不同的结构体布局，表现和"翻译器有 bug"一模一样。
> 修掉脚手架的 staleness 判断后全绿。（`41b050c`）

### E6 内核代码 / 预链接信息放置错位 —— 排除

`__TEXT_EXEC`（约 40 MB 代码段）与 `__PRELINK_INFO` 与文件**零差异**，说明解压与放置都对。

### E7 固件（iBEC）把 `__DATA_CONST` 里的 kalloc 视图记录写坏了 —— 排除（此前的结论作废）

在内核接手前一刻抓 `__DATA_CONST`（`dc-boot.bin`，6.5 MB）与 kernelcache 逐槽比对：

| 运行 | 不同槽 | 那条 `zv_name` 记录 |
|---|---|---|
| 本地 4096 MB | **252**（全是 iBEC 写的 ECID/board 数据） | `0x00200000002e0f96` **与文件相同** |
| 本地 2048 MB | **124** | 同上 |

此前"被改坏 10,144 槽、差值不相等"的数据来自**我们自己**的旧构建 dump —— 那时
`__DATA_CONST` 的重定位 pass 还没限制到 `__DATA`。

### E8 硬件/配置相关的变量 —— 排除（结果全同，确定性）

CPU 数量、RAM 大小（2048 / 4096 都复现同一失败）、设备 MMIO 重叠、系统寄存器返回值、
PAC、`avp-ctrr`，以及开关 `HV_K2` / `HV_SUPERBLOCKS` / `HV_K1` / `HV_GROUPS` /
`HV_HOIST_CHECK` / `HV_MOVIMM` / `HV_NO_CHAIN`。

### E9 是"跨核一致性 / TLB 广播时序竞争" —— 排除

每次启动精确踩中同一个位置，100% 必现；竞争条件不会这样。

### E10 是"内核把 PTE 写了，后来又被谁清掉" —— 排除

对页表页做周期快照、只记差异（`dffbea0`），得到故障那张表页（`0x84914000`）的完整写历史：

```
故障【前】只有 5 次写入：槽 5、6、24、25、26
故障【后】才出现槽 33/39/45/47/160..170/180/181/184/185/205
```

**故障槽（idx 由故障地址推出）在故障前没有任何变更记录。**

### E11 是"PTE 写错槽了"（索引算错）—— 排除

同一份写历史里 196 条变更**全部落在预期槽位**，没有系统性错位。

### E12 是"我们的 TLB 用了陈旧翻译，PTE 被写到另一个物理页" —— 排除

在每次表页写入时，把"快速路径实际用的 PA"与"绕过缓存冷走一遍页表得到的 PA"对比
（`ed70434`）：**0 次不一致**。

### E13 故障页是 guard page（故意不映射）—— 排除

`zalloc.c` 里 guard page 也走 `kmem_alloc` / `kernel_memory_populate` 与
`zone_meta_populate`，只是打上 `ZM_PGZ_GUARD` 标记 —— 它们是**被映射的**。
所以故障页不是 guard page，而是根本没有映射。


### E14 独占指令 / LSE 原子被翻译错（含争用导致双重发放）—— 排除

`tests/bitops_prog.s` 的 `atomics_prog` 覆盖：`ldxr/stxr`（同址成功、中间插入普通写必须失败、
无 `ldxr` 直接 `stxr` 必须失败、`clrex` 之后必须失败）、`ldxp/stxp`（成功与失配）、
`ldaddal`、`ldsmaxal` / `lduminal`（有符号与无符号不是同一条）、`casal`（匹配与不匹配）、
`swpal`、`ldaddalb`、`caspal`。**0 差异**。

争用方面：本引擎的 `STXR` 实现是宿主 `casal`（比较并交换），比较的是"LDXR 当时看到的值"，
所以结构上不可能出现"值已变却成功"。**双重发放这条路可以关掉。**

### E15 guest 内存里混进了宿主指针 —— 排除（并修正一个哨兵误报）

`core/dispatch.c` 有个哨兵：某块退出时 `x16` 看起来像宿主地址就报警。它在一次运行里报过：

```
block 700cab20 left x16 5500000004 but entered with 6168702c4c504141 ("AAPL,hap")
```

查下来是**假阳性**：

1. 该块的指令是 `ldp x15, x16, [x17]; stp x15, x16, [x1]` —— guest **自己**从内存里读进 `x16`，
   不是引擎改的。
2. `hv_looks_host()` 的判据只是"离宿主代码 ±256 MB"，而 guest 的数据里本来就充满这种位型。
3. 抓一份 512 MB RAM 扫描该窗口：落在内核镜像范围内的 **113 个值全部与镜像文件逐字节相同**
   （是 Apple 自己的数据，多半是 PAC 签名指针）；镜像外的 79 个候选是 `0x5400000000`、
   `0x5500300001`、`0x5400100001` 这类整数（类型/长度/标志位），不是指针。

→ 没有证据表明宿主指针漏进 guest 内存。


### E16 `kernel_memory_populate` 静默失败、页照样发出去 —— 排除

`zalloc.c` 的 **运行时** 扩容路径（`zone_expand_locked`）是"先映射、再发布"：

```c
kernel_memory_populate_object_and_unlock(object, addr, ...);   /* 逐页 pmap_enter */
zcram_and_lock(z, addr, new_va, cur_pages, cur_pages + pages, 0);  /* 才进 freelist */
```

而 `kernel_memory_populate_pmap_enter()`（`vm_kern.c`）遇到 `KERN_RESOURCE_SHORTAGE`
会**去掉 NOWAIT 重试一次**，然后：

```c
assert(pe_result == KERN_SUCCESS);
```

⇒ 映射要么成功，要么当场 panic，不存在"静默失败继续发页"。

`zone_fill_initially()` 里那个被丢弃的返回值（`zalloc.c:4206`）只影响**开机首次填充**，
而那次填充发生在内核接管之前、故障页出现之后的大量扩容都是运行时的
`zone_expand_locked` 路径。


### E17 「EL1 data abort 就是内核 panic」—— 这个前提是错的（重要更正）

之前所有"panic 链"的推理都建立在这个前提上。实测把它推翻了：

- 故障指令是 `memmove` 的 `stp`，目标是 `KMEM_PAGEABLE`（`flags = 0x40`，见 E18）的内存；
- 四次"故障"其实是**四次不同的按需映射**：每次 fault 之后 L3 表条目 **5 → 6 → 7 → 8** 递增，
  说明内核的 `vm_fault` 真的执行了、页被映射、指令被重试并成功；
- 故障之后页表写入继续（还有 17 次），guest 一直在跑。

⇒ 这些 EL1 异常是**被正常处理的缺页**，不是崩溃。此前基于"内核 panic"的那些
（kalloc_type_view 视图指针、panic 打印踩坏 %s、越过栈 guard 等）结论全部作废 ——
它们另外还有一个原因是反汇编用错了镜像基址（见第五节第 3 条）。

唯一剩下的现象是：**restore 侧先超时**（`Unable to place device into restore mode`），
而 guest 此时仍在运行。所以问题从"内核崩了"变成了"**guest 为什么没在超时前进入 restore 模式**"。

### E18 那次分配的 flags —— 查清了，是按需映射

调用点 `0xfffffe001cfaae8c` 传的 `x3 = 0x40`，在 `vm_kern_xnu.h` 里：

```c
KMEM_PAGEABLE = 0x00000040,
```

所以这是 **pageable kernel memory**：先保留 VA，页在第一次触碰时由缺页处理映射。
"地址页对齐 + 状态成功 + 页当时没映射"三件事因此全部正常。

---

## 二、已经确认的事实（正向结论）

1. **本地可完整复现**，一条命令、**约 50 秒**（不是三分钟；三分钟只是上限）。
2. **故障指令**是 `memmove` 的第一次落盘：

   ```
   fffffe001c890b10  pacibsp
   fffffe001c890b5c  stp x12, x13, [x0]     ← 故障：目标页未映射
   ```

3. **调用者**在 `0xfffffe001cfaaea4`：先用 `bl 0xe8e0c4` 申请 `0x158` 字节，
   **检查状态寄存器成功之后**才 `memmove(dst, src, 0x158)`。
4. **分配是页粒度**：返回的地址每次**页对齐**（`HV_LOGPC` 读返回值）。
5. **故障时刻**：`x0 = far`（页对齐）、`x1 = 0xfffffe001f3cf6b8`（源，合法）、
   `x2 = 0x158`、`x27 = 0x14b14000`（slide）、`sp` 在合法内核栈区。
6. **guest 自己在故障那一刻**：L3 表 512 条里只有 5 条有效，目标落在没填的那一段。
7. **zone 扩容路径**（`zalloc.c`）：

   ```c
   kmem_alloc(zone_submap(zsflags), &addr, ptoa(pages + guard_pages), ...)
     或 zone_submap_alloc_sequestered_va(...) + kernel_memory_populate(...)
   zone_meta_populate(addr, ptoa(pages + guard_pages));
   zcram(zone, addr, pages, 0);
   ```

   而 `zone_fill_initially()` 里 `kernel_memory_populate(...)` 的**返回值被丢弃**
   （`zalloc.c:4206`）：失败不 panic、不回滚，仍然 `zcram` 把页挂进 freelist。
8. **zone 页地址换算是 32 位截断 + 符号扩展**：

   ```c
   zone_pva_from_addr(addr) = (uint32_t)((intptr_t)addr >> 12)
   zone_pva_to_addr(pva)    = (vm_offset_t)(int32_t)pva << 12
   ```

---

## 三、剩下的问题

两条假设（H1 populate 静默失败、H2 原子导致双重发放）都已在 E14 / E16 里排除，
而"内核 panic"这个前提本身也已被 E17 推翻。现在剩下的问题是：

> **restore 超时之前，guest 到底卡在哪一步？** 它还在执行（页表写入在继续），
> 但没有进入 restore 模式。

要回答它需要一个新的工具：**一个不依赖块计数的、按时间的存活心跳**（现有
`cpu->icount` 和 `m->stats.blocks` 在链式执行下几乎不动，不能用来判断进度）。

其次才是旧问题（暂时不必追）：

> 内核在 `0xfffffe001cfaaea4` 调用一个函数（6 个参数，返回 `w0` 状态 + `x1` 地址），
> 状态为成功，`x1` 是一个**页对齐**的堆地址；随后 `memmove` 往这个地址写 0x158 字节时，
> 这一页在 guest 自己的页表里**从来没有被映射过**。

而 zone / `kmem_alloc` 两条分配路径都保证"先映射后发布、失败即 assert"。

下一步要确认的就是**那个被调函数是谁**：

- 参数形状：`(全局 map 指针, size, 0, 0x40, 6, 0)`
- 返回值形状：`w0` = 状态、`x1` = 地址（典型 16 字节结构体返回）
- 调用点：`0xfffffe001cfaae8c`，返回点 `0xfffffe001cfaaea8`

拿到它的名字，就能知道这类分配"本该"怎样映射，以及为什么这次没有。

### H2 分配器把同一段 VA 发了两次（freelist 记账坏了）—— 可能性已很低

独占/LSE 原子语义已通过差分测试（E14），且 STXR 是宿主 CAS，不会虚假成功。仍保留这条，
只因为 freelist 还用了非独占的普通读写，那部分尚未单独验证。

页表写历史显示这段 VA 从未被映射过，但内核认为它属于某个对象 —— 也可能是记账本身错了。

唯一没做差分测试的指令族就是这里的关键：`ldxr/stxr`、`ldxp/stxp`（独占对）与 LSE 原子
在**争用**下的行为。zone freelist 全靠这一族指令。

取证办法：给差分测试补独占 / LSE 原子的多线程争用场景，比较 native 与 JIT 在
"谁成功、成功几次、最终值"上是否一致。

---

## 四、工具

### 本地跑完整安装（不碰设备）

```sh
tools/build-restore-host.sh                 # 一次：出 build/host/lib/libhengerestore.a
tools/build-install-host.sh                 # 一次：出 build/host/host-install
tools/run-install-host.sh /tmp/hi.log 300 4
```

`run-install-host.sh` 是**轮询**的：每 2 秒看进程在不在，自己退出就立刻读日志；
超过给定秒数才算卡住。并非每次都能跑到内核故障（restore 自己的超时会让某些运行在 DFU
阶段就结束），所以默认重试到出现 `el1 data abort` 为止。

需要 `CURL_CA_BUNDLE=work/ca-bundle.pem`：本机 CA 库缺 Apple Root CA，
`gs.apple.com` 会报 `self-signed certificate in certificate chain`。

### 引擎侧诊断开关

| 开关 | 作用 |
|---|---|
| `HV_PTW_LOG` | 登记 walk 用过的所有页表页；写到这些页的 store 全部留痕（PA / 槽 / 值 / pc / 别名 VA） |
| `HV_PTW_SNAP=N` | 每 N 次 walk 对表页采样一次，只打印差异：每条 PTE 的完整写历史 |
| `HV_LOGPC=0x...` | 指定块入口打印寄存器（分配器返回值就是这么读的） |
| `HV_SLOW_MEM` | 内核开始跑之后把所有 load/store 改走 C 路径（切换点在"识别到内核镜像"那一刻） |
| `HV_DUMP_DC` | 内核接手前一刻抓 `__DATA_CONST`（`work/dc-boot.bin`） |
| `HV_TRACE` | 保留最近 256 个块入口（VA + x0 + x4） |
| `HV_LOG` | 1：未知系统寄存器 / 非法指令 / 未映射访问；2：另外每次异常 |

`core/exception.c` 在 EL1 数据异常时会打印：全部寄存器、TTBR/TCR、故障地址的分级 walk、
所在 L3 表的全部条目、故障前执行的块、以及调用者用的 zone 全局。

### 差分测试（宿主本地，秒级）

```sh
./android/test-regs.sh          # 寄存器保存/恢复，35 项
./android/test-bitops.sh        # bitops + memops + mathops，native vs JIT
./android/test-lzfse.sh payload.lz [reference.bin]
```

---

## 五、方法论教训（都是踩过的坑）

1. **改过结构体头文件，必须检查构建产物是否重编。** 这个坑踩了两次：安装构建（表现是
   boot 阶段 stack smashing）和测试脚手架（表现是"翻译器有 bug"的 fault loop）。
   现在两处都用"源文件或任意头文件更新就重编"的规则。
2. **目标文件不能只按 basename 命名。** `devices/aes.c` 与 mbedtls 的 `aes.c` 抢同一个
   `aes.o`，链接时看谁最后写入。
3. **反汇编必须核对镜像基址。** 用错 `0x144000` 就会读成隔壁函数，并据此得出完全错误的
   结论（曾经把 `memmove` 读成 kalloc 视图校验）。
4. **别用固定的 `sleep` 等一个进程。** 轮询、提前退出、立刻读日志。
5. **诊断不能依赖"一次性 static 标志"**，要无状态、可重入，否则第一次命中之后就不再打印。
6. **判断"是不是 JIT 的错"，要把真实代码 + 真实数据搬进本地脚手架，native 与 JIT 对比。**
   设备上的间接现象（寄存器怪值、指针非法）不能作为证据。
7. **页表写历史比"挂监视器等它被写"更有效。** 快照 + 差分不依赖 store 路径，也不会被
   chaining 绕过。

## 六、第二轮排除（拿到异常环之后，2026-11）

设备端 CLI（`tools/build-install-android.sh` → `/data/local/tmp`）跑通后，`work/halt.txt` 里的
`exc t…` 环形记录直接回答了"第一个异常是什么"。据此**作废一批旧方向，并把它们标记为不必再查**。

### X1 「宿主 SIGSEGV 是我们的 bug」—— 排除
glibc/proot 下 3/3 段错误、gdb 下不崩、**bionic CLI 上 0 次崩溃并正常退出**。
⇒ 那是 proot(ptrace) + glibc 语义组合出来的产物，手机 App 不会那样崩。**不要再修这个 race。**

### X2 「内核栈页只读是根因」—— 排除（是后果）
异常环（`halt.txt` 第 70–79 条）显示：先有一个**读垃圾指针**的转换失败，同一个 pc
（`0x1c90cb7c`）、同一个 far（`0x20000000935144`）**重复 4 次**，每次 `sp` 稳定下降 `0x770`；
到第 78 条时 `sp_el0 = 0x1f2eff80` 已经走出栈页，才写 `0x1f2eff8c` 触发 permission fault。
⇒ 栈只读是**循环撑爆栈**的结果。之前所有围绕"谁把栈页标只读"的方向（页表保护来源、rebase 误伤、
guard page、PTE 写坏）都不必再查。

### X3 「我们的代码页只读机制误伤」—— 排除（`code_page 0`）
### X4 「诊断代码改变了行为」—— 排除（`slow_mem 0`、`log` 门控已撤、开关化）
### X5 「`HENGEBUILD` 时间戳能判断构建新鲜度」—— 排除（`android/install.c` 不重编就戳旧值）

### X6 「设备 12 次 hvc vs 宿主 32 次 = 环境差异」—— 排除
`halt.txt` 头行 `halt ... icount 66254`：机器**早期就 halt 并 MACH Reboot 过一轮**，之后才跑到
6100 万。计数差来自"跑了几轮重启"，不是 bionic 与 glibc 的差异。

## 七、剩下的唯一主线

| 编号 | 问题 | 状态 |
|---|---|---|
| P1 | `0x1c90cb7c` 从哪儿拿到垃圾指针 `0x20000000935144`（bit57 置起、低位像正常内核 VA） | **唯一主线** |
| P2 | 栈被走穿 | 已确定是 P1 的后果，不单独查 |
| P3 | 25G83 Stage1 `Memory image not valid` | 独立问题，24G90 通了再说 |

P1 的入口工具（都不需要设备）：

1. `tools/disasm-raw.sh <kernel blob> 0x1c896488 <from> <to>` 看 `0x1c90cb7c` 是哪个 `ldr`、哪个寄存器
2. `halt.txt` 里该块的 `trace_ring`（记录 x0/x4）看垃圾值首次出现在哪个寄存器
3. 重点怀疑形态：**PAC 签名残留**（bit57/PAC 区残留）而不是"页表错"

## 八、panic 原文与判据（2026-11，内核能跑到 panic 之后）

异常环 + 帧链 + "从代码里反查它引用的字符串"这一套组合，把内核的死因还原到了源码行。

### 8.1 崩溃链（实测，四轮一致）

```
1c90cb7c  panic 的 %s 打印循环里 ldrsb 读到坏指针
   ↑ 帧链
1c8e54b8  panic 打印封装（pacia/paciza 传参，x0=输出回调）
1ca3ffbc  debugger 层
1ca3e514  debugger 层
1c894d10  locore 异常向量
1c8e5828  panic 本体
1d16c930  panic 调用 thunk（x6 = xpaci(x30) 作为 "caller"）
1d16e9f8  ★ 真正调用 panic 的位置
1d16ee58 / 1d16eaa0 / 1d16d030   同编译单元（kalloc.c）
```

### 8.2 panic 原文（从 0x1d16e9f4 前两条 adrp+add 读出）

```
kalloc_type_view(%s) from kext(%s) hasn't been rebuilt with required xnu headers
@kalloc.c:705
```

对应源码 `osfmk/kern/kalloc.c:695 kalloc_type_validate_flags()`：

```c
if (!(kt_flags & KT_CHANGED) || !(kt_flags & KT_CHANGED2)) panic(...);
```

### 8.3 现场反汇编（0x1d16e928，就是 `kalloc_type_slide()`）

```
ldr  w8, [x21, #0x48]     ; kt_flags 在 view+0x48
orr  w8, w8, #0x4000      ; KT_SLID
str  w8, [x21, #0x48]
bics wzr, w27, w8         ; 校验 w27 & ~(flags|KT_SLID)
b.ne panic
```

同一函数在加 slide 之前还做了**"已被 slide 过"检测**：若 `(orig ^ (orig+slide)) & 0xffc0000000000000`
非零，就把该字段直接写成 `0x0080000000000000`（毒化值）。这说明 XNU 预期这些指针**进来时是 raw
的**，由它自己 slide。

### 8.4 已排除

- **X5**：我们的 `__DATA` fixup 只落在 `seg5 __DATA`（19466 个槽，`already 0`、`outside 0`），
  段表里**没有** `__kalloc*` section（`kalloc_sections 0`）⇒ "按 section 名跳过"这条修法**无效**，
  但不代表我们的 fixup 没碰 kty view（view 可能就在 `__DATA,__data` 之类里面）。
- **X6**：不是宿主环境问题（bionic CLI 同样复现）。

### 8.5 下一步（唯一主线）

拿到 **kty view 数组的 VA 区间**（即 `kalloc_type_slide()` 里 `[x21, x23)`），
与 fixup 的 19466 个槽求交集：

- 交集非空 ⇒ 我们对内核自己会 slide 的结构做了 slide ⇒ 按区间排除，这条 panic 应消失
- 交集为空 ⇒ `kt_flags` 确实缺位 ⇒ 查 prelinked kext 与内核的头文件版本 / DT 的 kext 选择

拿区间的手段：在 pc `0x1d16e928` 做一次性 hook（记录 `x21` 与被写的 VA），
引擎目前没有通用 pc hook，需要新增。离线扫描 `0x50` 定长记录数组**未命中**，不要重复这条路。

## 十、后续（2026-11-15 的完整调查）

本轮把死因追到引导阶段的数据覆盖，另成一篇：
**`kernel-panic-investigation-2026-11-15.md`** —— 里面是完整的因果链、每环证据、
新增的排除项（PAC strip、双倍 slide、页表翻译、AES 解密、16 KB 块重叠的错误推断）
以及下一步唯一的动作（在固件拷贝包装处记录 `(dst, src, len)` 全表）。

要点：**panic 是次生现象**。真正的问题是 iBEC 把镜像数据写到了
`PA 0x74507860`（= 内核镜像 + 0x9EF860），破坏了 `kalloc_type` 的 view，
而那条 panic 消息本身也因为这个破坏打印不出来。
