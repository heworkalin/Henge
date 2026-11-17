# 排查树：走过的分支、站得住的结论、被推翻的结论

> 这份文档是**排查索引**，不是新结论。目的是：任何时候想知道"这个问题我们查过没有、
> 当时的结论是什么、后来有没有被推翻"，先看这里。
>
> 锚点来源：
> - 会话摘要（`compaction`）：`2026-11-13T04:58` … `2026-11-15T14:39` 共 14 份
> - `docs/` 下各专题文档（文件名见每一节）
> - `git log`（提交号可直接 `git show` 复核）
>
> 规则：**只有实测过的才算"成立"；推导一律标注；被推翻的保留，不删** —— 因为
> 重复走过一次死路，比没走过更浪费时间。

---

## 0. 一张图看位置

```
L0 移植/构建      ✅ 完成
L1 引导链         ✅ 完成  AVPBooter → iBoot Stage1/2 → iBEC → kernelcache
L2 内核启动       ✅ 完成  fixup / SIMD / kicker / PAC / hypercall / DT
L3 kext + IOKit   ✅ 完成  kext 全部 start，USB/XHCI/virtio 协商到 ACKNOWLEDGE
L4 用户态启动     ✅ 部分  launchd bootstrap 完成，restored_external 执行 19 万+ 块
L5 用户态卡死     🔍 当前  缺页 → 未建立映射 → 再缺页 → 全系统 WFI
```

---

## 1. L0 移植 / 构建

| 分支 | 做法 | 结论 | 状态 |
|---|---|---|---|
| 图形路径选型 | 对比 FCL / UniXNU / DroidVM | `steelbrain/reims-vgpu` 为 P0；PVG ≠ Metal API，不用自写 PVG→Vulkan | ✅ 成立 |
| W^X | 测 memfd split / anon W+X | memfd split 保留（任何时刻无 W+X） | ✅ 成立 |
| 驱动加载 | libadrenotools + androidnsbypass | 五步全通，Turnip 实测可用（1.3.296） | ✅ 成立 |
| commpage | guest 读 `0xfffffc090` 段错误 | 在 `0xfffffc000` 合成一页（`MAP_FIXED_NOREPLACE`） | ✅ 成立 |
| **"Turnip 零导出 = 不可用"** | 查 `.dynsym` 只导出 `HMI` | **推断作废**：经 adrenotools 加载后实测可用 | ❌ 推翻 |

---

## 2. L1 引导链

| 分支 | 做法 | 结论 | 状态 |
|---|---|---|---|
| Boot ROM 来源 | 在 IPSW 里找 `AVPBooter` | **不在 IPSW**：在系统镜像 1 MiB LZFSE 块（偏移 5,349,700,747）内再套 APFS fork，最终 214,904 字节 | ✅ 成立 |
| iBEC 跳错入口 | 反汇编 / 寄存器追踪 | 已修（`host/enter.S` 全部 `.hidden`） | ✅ 成立 |
| **"`0x7489xxxx` 是 iBEC"** | 以为那段代码属于 iBEC | **作废**：是**内核自己的 identity 开机代码**（PA `0x74898000` = 文件 `0xd80000` 逐字节相同） | ❌ 推翻 |
| chained fixup | 内核是 fileset collection（220 entry），iBEC 一个都没补 | 引擎在第一个块补：`DYLD_CHAINED_PTR_64_KERNEL_CACHE`，`target` 为相对偏移，`next` 步长 **4 字节** | ✅ 成立（`85a4f89`） |

---

## 3. L2 内核启动

| 分支 | 做法 | 结论 | 状态 |
|---|---|---|---|
| **SIMD clobber** | `put_helper` 只存 x30，不存 guest v0-v31；`mmu.c` 漏 `-mgeneral-regs-only` | **根因确认并修复**：设备实测 `kt_flags` `0x2 → 0x00000071`，与 IPSW 一致 | ✅ 成立（`f6dfb3e`） |
| LZFSE 解码正确性 | 本地差分测试 | `native vs reference: 0 of 75,546,624 differ` | ✅ 成立 |
| **"iBEC 写坏 `__DATA_CONST` 10,144 槽"** | 观察到的坏值 | **作废**：那是我们自己旧构建的 `__DATA_CONST` pass 写的；交接时是干净的 | ❌ 推翻 |
| **"EL1 data abort 是 panic"** | 一连串崩溃推论 | **作废**：是被正常处理的缺页（L3 表项 5→6→7→8 递增，`vm_fault` 执行、指令重试成功） | ❌ 推翻 |
| **"HV_NO_KFIX=1 会崩"** | 实验 A | **证伪**：不补 rebase 与补过行为一致；补 rebase 现在是对照开关 `HV_KFIX=1` | ❌ 推翻 |
| `__DATA` 必须我们补 | 离线：fileoff `0x36e08f0` 是原始 fixup 字 | 成立过，**后来被推翻**：默认不补，内核自己走完 fixup | ⚠️ 后推翻 |
| **反汇编基址** | 曾用 `0x1b3dc000` | **修正为** `0xfffffe001bb18000`（PA `0x73b18000`），差 `0x144000`；此前判读作废 | ⚠️ 修正 |
| 缺 1 ms kicker | `android/install.c` 起 CPU 线程时漏了 kicker | **修掉**：irq `0 → 27 万`，内核建完 4 GB 页表进 idle | ✅ 成立（`3c1d375`） |
| 磁盘 fd 被提前关闭 | teardown 在开盘之后调 | **修掉**：`Bad file descriptor` 归零 | ✅ 成立（`e73c115`） |
| 强制 `slow_mem=1` | 遗留诊断代码 | **修掉**：icount `1000/s → 2,000,000/s`（2000×） | ✅ 成立（`f3cb9fa`） |
| PAC strip 位宽 | 原 `BFM #16,#6` 只清 bits[54:48] | **修掉**：`BFM #16,#14`（覆盖 bits[62:48]），消除权限异常风暴 | ✅ 成立（`fc36d00`） |
| VMAPPLE OEM hypercall | 从 XNU `hv_hvc.h` 读 ABI | 补全 `0xc3000003..06`、UID `3B878185-…`、REVISION major=1 | ✅ 成立（`ee65f7d`） |
| **"PAC CPU 服务区间没实现"** | grep 被 `head -12` 截断 | **误判**：上游 `5e6f75d` 早有（`core/exception.c:622` 的 `psci()` switch） | ❌ 推翻 |
| PV ticket lock | 按 `lock_ticket_pv.c` 语义 | WKFK park / KICK 唤醒 已实现 | ✅ 成立（`f3e3549`） |

---

## 4. L3 kext / IOKit

| 分支 | 做法 | 结论 | 状态 |
|---|---|---|---|
| **"kext 一个都没 start"** | 早期 `__PRELINK_TEXT` 0 块 | **推翻**：打通内核日志（`serial=3 debug=0xA`）后可见 `AppleARMLightEmUp::start`、`AppleMobileApNonce::start`、`IOMobileFramebufferLegacy::start` 等大量 IOLog | ❌ 推翻 |
| 内核日志不可见 | 注入点在长度修补的 if/else 内 → 永不执行 | 修成独立执行 + `HV_EXTRA_BOOT_ARGS` | ✅ 成立 |
| XHCI 四个 bug | 逐条修 | `did not generate event` 归零、`device descriptor fragment is invalid` 消失 | ✅ 成立（`59384ae`/`f503cdb`/`fa4f1ac`） |
| **"按 ERDP 同步 event index"** | 我一度加的修法 | **撤销**：producer index 必须独立前进 | ❌ 推翻（自纠） |
| **shared_region 是根因** | 日志里 `vm_shared_region_start_address() failed` | **红鲱鱼**：dyld4 用普通 mmap，`check_np` 失败无害（`4e658d8`） | ❌ 推翻 |
| **"MSI controller 没创建"** | grep 前缀错 | **推翻**：`gic-msi +0x0008` 在 MMIO 直方图里，controller 确实创建了 | ❌ 推翻 |
| usbmuxd PID 白名单 | 加 `0x1280..0x1283` | `Found new device with v/p 05ac:1281` | ✅ 成立 |
| vusb 停在 `ACKNOWLEDGE(0x1)` | 长期认为是根因 | **降级为下游表现**（见 L4） | ⚠️ 重新定位 |

---

## 5. L4 用户态

| 分支 | 做法 | 结论 | 状态 |
|---|---|---|---|
| **"用户态从未启动"** | `HV_TRACE_PC`：`0x100000000` 区 0 块 | **后来推翻**：记录方式修正后实测 `restored_external` 执行 16077 次 syscall、8392 次 EL0 缺页，`objc[5]` 加载 `DiskImages2` | ❌ 推翻 |
| **"ignition complete = 用户态已启动"** | 串口行 | **推翻**：只证明内核侧正常 | ❌ 推翻 |
| **"`restored` 与 `restored_external` 是两个进程"** | 任务扫描 | **推翻**：同一 task 的两个 `p_comm` 视图 | ❌ 推翻 |
| `task+0x50` 是 `task->threads` | 结构推断 | **推翻**：三个 task 走出同一个环 | ❌ 推翻 |
| `task+0x58` 是 `vm_map` | 同上 | **推翻**：指向线程链节点 | ❌ 推翻 |
| `{prev,next,start,end}` 节点是"记录数组" | 见 §6 | **后来又纠回**：它确实是 `vm_map_entry`（首批误判是"数组"） | ⚠️ 反复 |
| 磁盘是卡点 | 读 `root_blk` 计数 | **排除**：11 个请求全部完成，`inflight=0` | ✅ 排除 |
| 网络是卡点 | 读 `hv_net_tx/rx` | **排除**：`0/0` | ✅ 排除 |
| 全系统停在 `WQT_QUEUE` | 按 XNU 源码定性 `thread+0x28` | 等待对象类型 = `WQT_QUEUE`（不是锁、不是 port） | ✅ 成立 |
| **"线程都等在同一个 turnstile"** | 早期判读 | **修正**：`thread+0x28` 是 waitq、`thread+0x30` 才是 turnstile | ⚠️ 修正 |
| 两个 CPU 都在 `WFI` | 读 `g_cpus[*].pc` + 反汇编 | `dsb; wfi` 之后，`x29 == sp+0x30`（栈空）→ 调度器认为无活可干 | ✅ 成立 |

---

## 6. L5 缺页（当前主战场）

| 分支 | 做法 | 结论 | 状态 |
|---|---|---|---|
| 缺页循环定位 | 读 `hv_el0_flt_*` 环 | `hv_uexc_n=52977` 但 PC 集中在两条相邻指令上 → **是循环，不是等待** | ✅ 成立 |
| **"数据页从未映射"** | 用户页表遍历 | **部分修正**：代码页正常；数据页 `0x100fc0000` 确实无 PTE | ⚠️ 修正（详见下） |
| granule | 读 TCR | `TG0=2` → **16 KiB 页、3 级页表**（不是 4 KiB/4 级） | ✅ 成立（关键前提） |
| 页表遍历 | 手工逐级打印描述符 | `L1[0]=0x9266c003`、`L2[128]=0x92dd0003`、`L3[999]=0x200000afc38ec3` | ✅ 成立 |
| 引擎 walk 是否正确 | `hv_walk_dbg` 三级轨迹 | 与手工遍历**完全一致**（L1/L2 有效、L3 为 0）→ 排除"引擎读错" | ✅ 成立 |
| PTE 写到错误物理页 | `HV_SLOW_MEM=1` 下看 `TLB-fresh mismatch` | **0 次**（40154 条 store）→ 排除 | ✅ 排除 |
| **"VM Map 里没有 entry"** | 扫 `{prev,next,start,end}` 形状 | **先认为有 → 后撤回 → 最终确认有**（见下） | ⚠️ 反复 |
| 故障分布 | 按 16 KiB 页聚合 16819 次故障 | 5920 页；**667 页 ≥5 次**，最多 136 次；fault 次数多的页**至今未映射** | ✅ 成立 |
| 故障地址形态 | 按页内偏移聚合 | 117 个"卡住页"**全部** fault 在 offset 0x0 | ✅ 成立 |
| **"pmap_enter 从未被调用"** | `HV_PCHK` 命中 241 vs fault 17181 | **撤回**：`HV_PCHK` 在 chaining 下会漏计（入口 12253 次、其唯一目标只 241 次，自相矛盾） | ❌ 撤回 |
| `pmap_enter` 存在性 | 字符串 `adrp/add` 交叉引用 | 确认在 `0xfffffe001ca2206c..0x1ca233ac`，**确实被执行过** | ✅ 成立 |
| slide | 读 `cpu->pc` 与索引对照 | **这一轮 slide = 0**（不是 AGENTS 里写的 `0x14b14000`） | ✅ 成立（本轮） |
| vm_map 定位 | `lock.byte[3]==0x33`（`LCK_TYPE_RW`）+ 边界 | **成功**：4 个 map，`pmap` 在 `+0x40`（`VM_MAP_STORE_USE_RB` 已启用） | ✅ 成立 |
| map 是否对应进程 | `pmap+0x08` = ttb0 | 与 `hv_last_el0_ttbr0` 交叉验证一致 | ✅ 成立 |
| "卡住页有没有登记" | 沿 `hdr.links` 遍历 entry | **有登记**：`[0x100fc0000, 0x100fc4000)`，正好一页 | ✅ 成立 |
| entry 大小 | 相邻 entry 互相回指 | **0x50 字节**（之前把下一个 entry 的 links 当成"时间戳"了） | ⚠️ 修正 |
| 登记状态 | 对比卡住页/代码页/数据页/栈 | 可见字段上**无差异**（都不 `is_sub_map`，`object_value` 都是 0） | ✅ 成立 |
| 精确匹配 fault↔进程 | 新增 `hv_el0_flt_ttbr0[]` | 8 条 fault **全部**匹配到 entry，**PTE 全 NONE**，`0x1007401cc` **出现两次** | ✅ 成立 |
| 故障横跨两类 entry | 读 entry 的 object union | 4 条 `object=0`（匿名零页）、2 条 `is_sub_map=1`（线程栈） | ✅ 成立 |
| 推论 | 两条不同路径都失败 | 故障点在**共同下游**（写页表），而非某一条分支 | 🟡 推导 |

---

## 7. 被推翻的结论汇总（按类型）

### 7.1 观察对、归因错
| 曾经的说法 | 实际 |
|---|---|
| iBEC 覆盖了 kernelcache | 是 iBEC 自己的 LZFSE 解压结果错误（后来 SIMD 修复后正确） |
| `0x7489xxxx` 是 iBEC 代码 | 是内核自己的 identity 开机代码 |
| iBEC 写坏 `__DATA_CONST` 10,144 槽 | 是我们自己旧构建的 DC pass |
| EL1 data abort 导致 panic | 是被正常处理的缺页 |
| `vm_shared_region_start_address() failed` 是根因 | 红鲱鱼，dyld4 不用该 API |
| kext 一个都没 start | kext 一直在 start，只是日志没打通 |
| 用户态从未启动 | `restored_external` 跑过 16077 次 syscall |
| `restored` 与 `restored_external` 是两个进程 | 同一 task |
| vusb 停在 ACKNOWLEDGE 是根因 | 是更上层问题的下游表现 |

### 7.2 结构/偏移猜错
| 曾经用的 | 实际 |
|---|---|
| `task+0x50` = `threads` | 三个 task 走到同一个环 |
| `task+0x58` = `vm_map` | 指向线程链节点 |
| `thread+0x28` = turnstile | 是 waitq（`turnstile` 在 `+0x30`） |
| `vm_map` 的 `pmap` 在 `+0x38`/`+0x50` | 实测在 **`+0x40`**（RB 启用） |
| `vm_map_entry` 有"时间戳"字段 | 那是**下一个 entry 的 links**（entry 只有 0x50 字节） |
| 反汇编基址 `0x1b3dc000` | `0xfffffe001bb18000` |

### 7.3 工具使用错误
| 错误 | 后果 |
|---|---|
| `HV_PCHK` 传 link VA（差一个 slide） | 零命中 → 假阴性 |
| **`HV_PCHK` 命中数当调用次数** | chaining 下严重漏计 → 结论撤回 |
| `walk()` 用当前 CPU 的 TTBR0 查用户地址 | 必然 UNMAPPED（CPU 在 idle 时是内核页表） |
| 按 4 KiB 算 16 KiB granule 的页表索引 | 索引全错，漏掉一半表项 |
| `ptw` 诊断按 4 KiB 匹配 | 16 KiB 表的后三个子页写入不可见 |
| `gdb` attach 时进程是**暂停**的 | "多次采样一致"不能证明"值不随时间变化" |
| `hv_last_el0_ttbr0` 当"当前进程" | fault 环是全局跨进程的，一半匹配错 |

---

## 8. 站得住的结论（可复用）

1. **XNU 版本**：`xnu-11417.140.69 / Darwin 24.6.0 / RELEASE_ARM64_VMAPPLE`
2. **引导链全通**，`__PRELINK_TEXT` 与 IPSW 逐字节一致（差异 = `ref + slide`）
3. **kext 全部 start**，IOKit 匹配链正常，ramdisk 挂载成功，launchd bootstrap 完成
4. **guest 是 16 KiB 页、3 级页表**（`TCR.TG0=2`）
5. **缺页是真实循环**：登记有、PTE 无、同一地址重复缺页
6. **fault 横跨匿名零页与子映射两类路径**
7. **`pmap_enter_options` 确实会执行**，地址 `0xfffffe001ca2206c..0x1ca233ac`
8. **两个 CPU 最终都在 `WFI`**（不是崩溃，是无活可干）

---

## 9. 当前未解的唯一问题

> **缺页处理跑完了，为什么页表项没有建立？**

已知它在 `pmap_enter_options` 这个函数里（或它之前）某处没有落地，但**还无法区分**：

- `pmap_enter` 被调用了但写入没生效 / 写到别处
- 还是根本没被调用（卡在 `vm_fault_enter` 之前的判断）

**下一步唯一可信的做法**：用 `HV_TRACE_PC`（记录每块 VA，不受 chaining 影响）
统计 `pmap_enter_options` 范围内的真实执行次数，并检查每次 fault 之后执行流是否进入它。

---

## 10. 方法论铁律（血泪换来的）

1. **计算用 Python，不心算**（手算错过 `0x837ef780−0x9EF760`）
2. **实测 > 推导**；推导必须标注"不确定"
3. **不要用 `HV_PCHK` 计数**（chaining 会漏）
4. **`gdb` attach 会冻结进程**，不能用它观察"变化"
5. **先读 TCR 判 granule**，再算页表索引
6. **区分"未驻留"和"出错"**：按需装入意味着大部分页本来就不在页表里
7. **多进程场景必须用 fault 自带的 ttbr0**，不能用"最后一次"
8. **一次只跑一个 VM 实例**（2 GB/个），分析完立刻 `kill -9`
9. **不删被推翻的结论**，标注状态而不是抹掉
