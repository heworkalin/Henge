# GPU：图层内容坏成"砖块噪点"（2026-11-22，已复现，未修）

## 结论（今天的实测）

1. **离屏可复现**：CLI 跑 `workB`，用自动输入（`HV_AUTO_INPUT`）点击 Setup Assistant 的
   Continue 箭头，`HV_GRAB_GPU=1 HV_GRAB_EVERY=5` 抓帧。5 分钟 ~51 帧里有 **3 帧坏**
   （`gpu-037/046/058`），其余正常 —— 与你看到的"偶发、要鼠标动才出现"一致。
2. **encode 是忠实的**：`surf-0300`（guest 显示表面的原始字节）与
   `gpu-059..065` 的统计**完全一致**（white=0.226 dark=0.765 dx=0.90）。
   → 屏幕上看到的坏内容，**当且仅当 guest 的显示表面里就是坏的**。
   上屏（swapchain/present）不是元凶；`docs/gpu-black-tiles-2026-11-21.md` 里
   "部分提交/清屏"的猜测作废。
3. **坏的是"某一层"**：坏帧里对话框白底整块变成砖块噪点，而**叠在上面的文字、地球图标、
   蓝色高亮列表项都是对的**。所以是**某个图层的内容错**，不是整屏错。
4. **形态是字形图集**：实测坏帧的图案周期 —— **列 ≈15 px、行 ≈32 px**（FFT 峰值）。
   这是字形图集（glyph atlas）的格子尺寸，不是 stride/tile 的周期。
   → 最可能是**这次 draw 采样到了"另一张纹理"**（图集），即绑定/描述符层面的错，
   而不是几何、stride、UBWC。

## 已排除

| 假设 | 证据 |
|---|---|
| 显示 surface 的 stride/格式搞错 | `present #N: mapped format 0x52476841 1920x1080 stride 15360 bpe 8`，**全程不变**（几次运行 2600+ present 都是同一元组）→ 没有"格式动态切换"发生在显示表面上 |
| 导入长度不够（surface 被截断） | 新增检查：`needs = bytes_per_row * height` 与导入长度比较，全程**无告警**；显示表面 needs `0xfd2000` = 16.6 MB，导入就是它 |
| 上屏路径（清屏/区域/缩放） | 见结论 2：encode 与 surface 字节一致 |
| 原厂驱动对照 | 原厂 Adreno 驱动在本路径上直接 fatal（`HostMemory` 需要 `VK_EXT_external_memory_host`，它没有）→ 对照不可做，本机只能是 Turnip |

## 现场参数（重要，复现用）

guest 用到的 surface 形状（`REIMS_DIAG=1` 的 `DIAG surface buffer` 计数）：

| 形状 | 次数 | 推断 |
|---|---|---|
| `1920x1080 stride 15360 bpe 8` | 305 | **显示表面**（`'RGhA'` = RGBA16Float） |
| `3840x2160 stride 16384 bpe 4` | 120 | 2× 图层（壁纸/大图，1920x1080 的 HiDPI 版） |
| `512x512 stride 512 bpe 1` | 120 | **8 位单通道 512×512 = 字形/蒙版图集** |
| `512x512 stride 2048 bpe 4` | 69 | RGBA8 512×512 |
| `1920x24 stride 7680 bpe 4` | 7 | 菜单栏条带 |

## 复现命令（设备侧 `/data/local/tmp/henge`）

```bash
REIMS_SURFACE_DUMP=/data/local/tmp/henge/workB/surf \
REIMS_DIAG=1 \
HV_RAM_NO_DMABUF=1 REIMS_ROG_UNORDERED=1 HV_VK_DRIVER=/data/local/tmp/henge/driver \
HV_BOOT_ONLY=1 HV_LOG=1 HV_AUTO_INPUT=10 HV_GRAB_GPU=1 HV_GRAB_EVERY=5 \
  ./host-install AVPBooter.vmapple2.bin workB/disk.img workB 2048 2 > run.log 2>&1
```

- `HV_AUTO_INPUT=<秒>`：每 N 秒移动一次指针（tablet 绝对坐标 + 相对移动），每 4 步点击
  Continue 箭头（(1560,793) @1920x1080）。没有鼠标信号时坏帧不出现（你也这么说过），
  这个开关就是离屏版的"鼠标在动"。
- `REIMS_SURFACE_DUMP=<前缀>`：每 20 个 present 写一张 guest 表面原始字节的 BMP，
  另外**一检测到噪点就立刻写**并打印 `surface noise:` 行。
- 判读脚本 `/tmp/noisy.py`：按"相邻像素差"给 BMP 打分，快速从几十张里挑出坏帧
  （注意：**这个指标抓不到"黑底+字母碎片"型损坏**，那种要靠肉眼看或按白像素分布判断）。

## 下一步（按成本）

1. **定位那一层的 draw**：在 runtime 里加"只对渲染到显示表面的 pass 打印其采样纹理
   （格式/尺寸/指针）"的探针，抓一好一坏两帧对比。若坏帧里白底 draw 绑的是
   `512x512 bpe 1` 的图集 → 绑定/描述符缓存问题，去查描述符缓存 key 与资源 ID 复用。
2. 若确认是资源 ID 复用：过渡期 surface 释放/重建，ID 被复用而缓存没失效。
3. 备选：`MTLHeap`/argument buffer 路径单独做单元测试（同一 slot 换不同纹理，看是否取到旧的）。

## 记住的教训

- **只看一部分图就会得出错结论**（今天先说"surf 全是干净的"，其实是抽样太少 + 打分指标
  只对"高频噪点"敏感，对"黑底碎片"失灵）。
- 判断"谁坏了"要靠**同一时刻的两份字节**（surface 与 encode），不要靠时间上错开的两次采样。

---

## 2026-11-22 下午：进一步实测与两次自我纠错

### 已经站得住的（有同刻对照数据）

| 结论 | 证据 |
|---|---|
| **坏帧不是"读页面读早了"** | `REIMS_SURFACE_DUMP_LATE=50`：present 时立即 dump 一份、50 ms 后再 dump 一份，坏帧的两份**逐字节相同**（diff 0.00）。时序/回写竞争假设作废 |
| **坏帧在计数器上毫无特征** | `fr.log` 里好帧与坏帧的 present 间隔、surface 创建/释放数、事件 seq 增量**完全相同**（5 present / 5 create / 5 release / 11 seq） |
| **坏区间起点 = 对话框第一次淡入** | 好帧 0–13（present 0–65，启动进度条），坏帧从 present 70 起 —— 即 guest 第一次做真正的分层合成 |
| **显示表面的格式全程不变** | `mapped format 0x52476841 1920x1080 stride 15360 bpe 8`（RGhA），数千次 present 一个元组 |

### 我自己犯的两个错（记下来，避免再犯）

1. **dump 工具的通道顺序错**：`'RGhA'` 是 **R,G,B,A**，我按"先取到的当蓝"写成了 `(b,g,r)=(ch0,ch2,ch4)`，
   结果 **B/R 交换、alpha 进了红通道**。对照 `gpu-072` 与 `fr-0053` 量出来：
   `fr.R = 0.22*gpu.R + 185`、`fr.B = 0.58*gpu.B + 97` —— 就是这个错。已修（`(ch4, ch2, ch0)`）。
   **教训**：dump 工具本身要先和"引擎自己 encode 的同一帧"对齐校验，否则会把工具 bug 当成引擎 bug。
2. **图层 dump 的语义**：`REIMS_LAYER_DUMP` 打的是**纹理创建那一刻**的表面字节，而 guest 常常
   "先建纹理、后往里画"。所以图层里"只写了一半/竖条纹"**不能**当作丢写证据 —— 它可能只是还没画。

### 仍然未知

- 坏帧是哪一个 draw 产生的、它采样了哪些纹理（需要 runtime 层探针）。
- 为什么"鼠标要移动、且要移得远"才会出现（现象已多次确认，机制未知）。

## 新增工具开关（全部默认关）

| 开关 | 作用 |
|---|---|
| `REIMS_PRESENT_LOG=1` | 每次 present 的路径（backing/mapped）+ 格式 + 尺寸 + stride + bpe，**只在元组变化时打印** |
| `REIMS_SURFACE_DUMP=<前缀>` | 写 guest 显示表面原始字节的 BMP；**噪点超阈值立刻写**并以 `NOISY` 标出 |
| `REIMS_SURFACE_DUMP_EVERY=<n>` / `_MAX=<n>` | 每 n 个 present 取一张 / 最多 n 张 |
| `REIMS_SURFACE_DUMP_LATE=<ms>` | 同一表面延迟 ms 再 dump 一份（`-lateNN` 后缀），用于排除"读早了" |
| `REIMS_PROBE_LOG=1` | surface 创建/释放事件带 seq 打印（`reims-vgpu-core::probe` 的统一时钟） |
| `REIMS_LAYER_DUMP=<前缀>` | 尺寸 ≥200×200 的图层表面在**建纹理时**的原始字节（跳过显示表面，1/20 采样，上限 400） |
| `HV_POINTER_MS=<ms>` | 指针**持续**随机移动（±80 像素/步，默认 8–10 ms 一次），并每 32 步补一次平板绝对坐标；这是让故障出现的前提 |
| `HV_AUTO_INPUT=<秒>` | 每 n 秒点一次 Setup Assistant 的 Continue 箭头（(1560,793)@1920x1080） |

每个 dump 都在 `<前缀>.log` 里留一行：`帧路径 | dump 序号 | 触发时 seq | present | probe seq |
surfaces created/released | 端口/格式/尺寸/stride/bpe | 噪点分`。所以"哪张图坏了"可以直接对到那一刻的状态。

---

## 下午第二轮：合成时抓到的图层对照（重要）

新的抓法（`REIMS_LAYER_DUMP` + `probe::watch_layer`）在**每个 present 那一刻**把"最近用到的图层表面"
从同一批指针再读一遍，写 `fr-NNNN-L{i}.bmp`，和显示表面 `fr-NNNN.bmp` **同一时刻**。
一次实测（显示正常的那一帧）：

| 表面 | 形状 | 内容 |
|---|---|---|
| 显示表面 | 1920×1080 `RGhA` bpe 8 | **Language 对话框，正确** |
| L4 壁纸 | 3840×2160 `BGRA` stride 16384 | **森林照片，正确** |
| L5 | 846×646 stride 3392 | 全黑 |
| L3 | 512×512 stride 2048 | **横向彩色条纹（垃圾）** |
| L0/L1/L2 | 512×512 stride 512 bpe 1（R8） | **横向条纹/虚线（垃圾）** |
| L6 | 800×600 stride 3200 | **横向条纹（垃圾）** |

**由此得到的方法论铁律（本文件的教训之一）**：
"某个图层里是垃圾"**不能**证明"这一帧合成错了"。上面这一帧显示完全正确，而同帧的
L0/L3/L6 就是垃圾 —— 它们是这一帧**没被采样**的过期/临时缓冲（阴影/模糊蒙版、旧窗口后备）。
要判"合成错"，必须知道**合成器这一帧实际采样了哪几张纹理**。

## 观察者给出的最强线索（尚未验证）

> **壁纸图层是好的，但最终画面里没有把它合进去** —— "明明已经有背景了，却没有正常把背景合成进
> 正视图片中"。

这只有两种可能，且两者指向不同代码：

| 可能 | 判据（pass 日志） |
|---|---|
| A. 合成器根本没采样壁纸（guest 的合成 pass 里没有这张纹理） | `fragTex` 列表**不含** 3840×2160 |
| B. 采样了但采到黑（我们的纹理里没内容 / 视图不对 / 图像没填） | `fragTex` **含** 3840×2160，但目标区域是黑 |

## 下一步探针规格（下一轮直接做）

`REIMS_PASS_LOG=1`：**只对"渲染到 1920×1080 显示表面"的 pass** 记录，每次一条结构化行：

```
pass seq=N colors=1 dst=<tex ptr> fmt=RGBA16F load=<Load|Clear|DontCare> store=Store
  fragTex[slot]=<tex ptr> fmt=<fmt> <w>x<h>      <- 实际被采样的图层
  viewport=x,y,w,h scissor=x,y,w,h
```

若"只对显示 pass 记录"在实现上有风险（encoder 可能在 lowering 线程执行），退一步用
**环形缓冲**：只记录"被采样的纹理宽 ≥1024"的绑定（合成器用的就是这类），每 N 条把环刷到文件，
坏帧出现后取文件尾部即为那次合成的绑定序列。

判据表（拿到好/坏两帧后逐项 diff）：

| 现象 | 结论 |
|---|---|
| 坏帧 `load=Load` 被翻成 Clear/丢弃 | 局部更新把区域外抹掉 → 与"合成丢失"吻合 |
| 坏帧 `fragTex` 是 R8 蒙版/过期缓冲 | 纹理绑定错（描述符/视图缓存） |
| 坏帧 `scissor`/render area 变成整屏 | 局部更新变成整屏覆盖 |
| `fragTex` 含壁纸但输出黑 | 我们的图像没内容（导入/上传/视图） |

---

## 决定性一帧：壁纸被画成"砖块"，文字正常（fr-0300，hola 欢迎屏）

`fr-0300.bmp`（Setup Assistant 的 "hola" 屏）：

- 文字 "hola" **完全正常**；
- **壁纸渲染成砖块/棋盘格**（右下角一片绿褐色方块，文字背后也有淡色砖块）。

与它同时刻的图层 dump（上一轮 `fr-0004-L4`）里，**guest 页面中那张 3840×2160 壁纸是完好的森林照片**。

于是这一条把因果定死了：

| 事实 | 含义 |
|---|---|
| guest 页面里壁纸正确 | guest 画的对，数据在内存里是对的 |
| 合成结果里壁纸是砖块 | **我们那张 4K 纹理在 GPU 上的内容/布局与页面不一致**（上传或采样布局错） |
| 同屏文字正常、只有壁纸块状 | 错在**大尺寸 + 填充 stride 那一条路径** |
| 三个显示缓冲轮转（见下） | 之前的"好帧/坏帧交替"是采到轮转中的不同缓冲，不是随机 |

### 三缓冲（新事实）

每个 present 换一个显示表面，三个 base 轮转：
`0x7545fb5000` / `0x74e35c6000` / `0x751d42e000`（各占约 1/3 的 present）。
同一轮次里三者的 `dark`/`coloured` 统计**完全一致**，所以这一轮没有"某块缓冲坏"。

### 现在的首要嫌疑：带填充的 4K stride

| 量 | 值 |
|---|---|
| 壁纸表面 | 3840×2160，`BGRA8`，**stride 16384** |
| 若按宽度算的 pitch | 3840×4 = **15360**（差 1024 字节 = 256 像素） |

**15360 与 16384 的混用**会产生逐行错位，在 256 像素的整倍数下正好表现为**规则方块**（砖块）。
这条还没查：4K/带填充 stride 的表面走的是哪条路径（`buffer_texture` / host-memory import /
texel buffer view），以及那条路径里 `bufferRowLength` 用的是 **texels** 还是 **bytes**。

### 下一步（唯一未查的分支）

1. 在 `new_texture_on_surface` 之后打印这条路径的 `bufferRowLength`/`imageHeight` 及其单位；
2. 对同一张壁纸：对比"guest 页面（已知是森林）"与"纹理内容（readback 或采样结果，已知是砖块）"，
   确认差异是**行错位**还是**块布局**；
3. 若确认行错位 → 修 pitch 的单位/取值；若确认块布局 → 查 `VK_IMAGE_TILING_OPTIMAL` 与
   写入方式是否匹配（线性写 tiled 读，或反之）。
