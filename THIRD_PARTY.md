# 引入的上游 / Third-party components

本文件是**唯一的"我们引入了哪些上游"清单**：每一项都给出**上游仓库链接**（点进去即可访问）、
引入形态（vendored 源码 / gitlink / 仅头文件 / 下载物）、**本仓库是否改过**、以及**许可证**。

> 许可与义务的完整说明见 [`docs/licensing.md`](docs/licensing.md)（含组合作品的判断）。
> 本文件不重复那里的法务分析，只保证**清单与链接准确、可核对**。

**本仓库自身许可：GPL-2.0-or-later**（见 [`LICENSE`](LICENSE)）。
因为 usbmuxd（GPL-2.0/GPL-3.0）与 Husk 的代码被编译进同一个二进制，发行的二进制是**合并的 GPL 作品**，
**不能上架 App Store**。

---

## 0. 先看这一条：这不是一个能"纯 clone 就完事"的仓库

本仓库**没有 `.gitmodules`**，依赖以三种方式记录，`git clone` 只能拿到其中两种：

| 形态 | 是什么 | clone 后 | 需要 `tools/clone-deps.sh` |
|---|---|---|---|
| **vendored 源码树** | 文件直接入库 | ✅ 在 | 不需要 |
| **gitlink**（mode `160000`） | 只有一条 commit 指针，无 `.gitmodules` | ⚠️ **目录存在但是空的** | **需要** |
| **下载物** | 不入库（体积/来源原因） | ❌ 不在 | 另有脚本 / 手动 |

```sh
tools/clone-deps.sh            # 填两个 gitlink 目录
tools/clone-deps.sh --list     # 打印下面这些表格的实时版
tools/clone-deps.sh --verify   # 逐个核对 third_party/*/UPSTREAM_COMMIT 是否仍在上游存在
```

---

## 1. 与引擎核心同处一棵树（上游 Henge 本体）

| 项目 | 上游 | 许可 | 位置 | 我们改过 |
|---|---|---|---|---|
| **Henge**（iOS 版，上游） | https://github.com/Leviidev/Henge | GPL-2.0-or-later | 本仓库的前身 | ✅ 整个 Android 移植（`dev` 分支） |
| Husk 的代码 | https://github.com/Leviidev/Husk | GPL-2.0-or-later | `ios/Henge/JIT/` 等 | ❌ |
| QEMU（`hw/vmapple/*`，对拍参照） | https://github.com/qemu/qemu | GPL-2.0-or-later；`tcg-apple-jit.h` 为 LGPL-2.1-or-later | `devices/aes.c`、`ios/Henge/Native/` | ✅ 参照实现重写 |

> 上游 iOS 版**没有** Android 目录；`android/` 全部是本仓库新增。

---

## 2. 恢复协议栈（`third_party/`，vendored 源码树）

全部为**源码快照**（目录内**没有** `.git`），每棵树带一个 `UPSTREAM_COMMIT` 文件记录来源版本。
`git clone` 就能拿到，**不需要额外下载**。

| 项目 | 上游 | 记录的版本 | 许可 | 我们改过 |
|---|---|---|---|---|
| **idevicerestore** | https://github.com/libimobiledevice/idevicerestore | `ae8cfb4a` | LGPL-3.0（`COPYING`）；源码头写 LGPL-2.1-or-later | ✅ **+120 行**（见 §6） |
| **libimobiledevice** | https://github.com/libimobiledevice/libimobiledevice | `fa0f7919` | LGPL-2.1（`COPYING` 为 GPL-2.0，工具部分） | ❌ |
| **libplist** | https://github.com/libimobiledevice/libplist | `fe3dc34d` | GPL-2.0（`COPYING`）+ LGPL 例外（`COPYING.LESSER`） | ❌ |
| **libusbmuxd** | https://github.com/libimobiledevice/libusbmuxd | `93eb168b` | LGPL-2.1 | ❌ |
| **libirecovery** | https://github.com/libimobiledevice/libirecovery | `046344f7` | LGPL-2.1 | ❌ |
| **libimobiledevice-glue** | https://github.com/libimobiledevice/libimobiledevice-glue | `10ddafa1` | LGPL-2.1 | ✅ **+13 行**（见 §6） |
| **libtatsu** | https://github.com/libimobiledevice/libtatsu | `e7d6ad13` | LGPL-2.1 | ❌ |
| **usbmuxd** | https://github.com/libimobiledevice/usbmuxd | `3ded00c9` | **GPL-2.0 或 GPL-3.0**（`COPYING.GPLv2` / `COPYING.GPLv3`） | ✅ **+10 行**（见 §6） |
| **Mbed TLS** | https://github.com/mbed-tls/mbedtls | `mbedtls-3.6.2`（tag） | **Apache-2.0 或 GPL-2.0-or-later**（双许可，任选其一） | ❌ |
| **libzip** | https://github.com/nih-at/libzip | `36d89355` | BSD（3 条款风格，见 `LICENSE`） | ❌ |

> **注意许可差异**：`usbmuxd` 是 **GPL**（不是 LGPL），这是"整个二进制成为合并 GPL 作品"的原因之一。
> `Mbed TLS` 可 **二选一**；本仓库整体按 GPL-2.0-or-later 分发，取 GPL 一侧即自洽。

### 2.1 仅有头文件 / 素材（`third_party/`，vendored）

| 组件 | 上游 | 位置 | 许可 |
|---|---|---|---|
| **curl** 头文件 | https://github.com/curl/curl | `third_party/curl_include/curl/` | curl（MIT 风格） |
| **libusb** 头文件 | https://github.com/libusb/libusb | `third_party/libusb-1.0/libusb.h` | LGPL-2.1 |
| restore 配置素材 | 本仓库 / Apple 恢复流程派生 | `third_party/restore_config/` | 见 `docs/licensing.md` |

> 这里**只有头文件**，没有链接进任何库；对应的目标文件/静态库由构建脚本另行编出。

---

## 3. 图形链路（`android/third_party/`）—— Android 移植新引入

这部分**上游 iOS 版不用**，是 Android 侧新增的四项。

| 项目 | 上游 | 形态 | 许可 | 我们改过 |
|---|---|---|---|---|
| **reims-vgpu** | https://github.com/steelbrain/reims-vgpu | **gitlink**（需 `clone-deps.sh`） | **GPL-2.0-or-later / GPL-3.0-or-later / LGPL-3.0-or-later 三选一** | ✅ **12 个提交**，分支 `rebuild` |
| **metal2vulkan**（runtime） | https://github.com/steelbrain/metal2vulkan | **vendored 源码树**（已入库） | **GPL-2.0 / GPL-3.0 / LGPL-3.0 三选一**（见 `LICENSE-LGPL-3.0`） | ✅ **10 个文件** |
| **metal2vulkan**（translator） | https://github.com/steelbrain/metal2vulkan | 构建期物化到 `build/m2v-compile-layer/`（不入库） | 同上 | ✅ 一个补丁 `tools/patches/m2v-rog-unordered.patch` |
| **libadrenotools** | https://github.com/bylaws/libadrenotools | **gitlink**（需 `clone-deps.sh`） | **BSD-2-Clause** | ❌ |
| **LZFSE** | https://github.com/lzfse/lzfse （Apple） | vendored 源码树 | **BSD-3-Clause**（Apple Inc.） | ❌ |
| **Mesa / Turnip**（Vulkan 驱动） | https://gitlab.freedesktop.org/mesa/mesa | **下载物**，不入库 | MIT | — |
| **qemu-reims-vgpu**（C API 参照） | https://github.com/steelbrain/qemu-reims-vgpu | 仅作参考，未引入 | GPL-2.0 | — |

### 3.1 关于 `reims-vgpu` 与 `metal2vulkan` 的三重许可

两者都给出 **GPL / LGPL** 的多个选项。这一点很重要：

- 若取 **LGPL-3.0** 一侧，静态链接时需要满足 LGPL 的重链接义务；
- 本仓库整体是 **GPL-2.0-or-later**，取 **GPL 一侧**即无兼容性问题；
- 本仓库**按 GPL 一侧分发**，见 `docs/licensing.md`。

> 这两个项目的许可文本在各自目录内：`LICENSES/`（reims-vgpu）、`LICENSE-GPL-2.0`
> `LICENSE-GPL-3.0` `LICENSE-LGPL-3.0`（metal2vulkan）。

---

## 4. 下载物（不入库，按需获取）

| 什么 | 怎么来 | 为什么不入库 | 记录在哪 |
|---|---|---|---|
| **Turnip** `libvulkan_freedreno.so` | `android/fetch-turnip.sh <url> [sha256]` | 约 9.4 MB 二进制，非本仓库源码；quietly pulling "latest" 不可接受 | `PROVENANCE.txt`（记录 url / sha256 / size / time） |
| **AVPBooter** `AVPBooter.vmapple2.bin` | 安装时从 Apple 的 macOS 恢复镜像取 | 是 Apple 的 boot ROM | 不在仓库 |
| **macOS 恢复镜像**（IPSW，约 16 GB） | 从 Apple 下载 | Apple 的字节 | `.gitignore` 排除 `android/ipsw/` |
| **Rust 依赖**（cargo） | `cargo` 按 `Cargo.lock` 拉取 | 标准 Cargo 流程 | `android/third_party/reims-vgpu/Cargo.lock` |

**不归属本仓库许可的部分**：macOS、boot ROM 都是 **Apple 的**；导出/导出的 Mac 目录含 macOS 与 ROM，
**仅供自己的设备使用，不要分发**。

---

## 5. 本仓库的权限声明（Usage / redistribution）

| 项 | 声明 |
|---|---|
| 本仓库代码 | **GPL-2.0-or-later** —— 可自由使用、修改、再分发；**分发修改版须同样以 GPL 开放源码** |
| 发行二进制 | **合并的 GPL 作品**（含 usbmuxd、Husk 代码）→ **不能上架 App Store** |
| 上游 Henge 的权利 | 归 **Leviidev** 所有；本仓库是**衍生作品**，保留其版权声明 |
| 各上游组件的权利 | 归**各自版权人**所有，按各自许可使用；本仓库**不重新授权**它们 |
| Apple 的素材 | macOS、boot ROM **不属于**本仓库，不适用上述任何许可；使用者需自行遵守 Apple 的条款 |
| 商标 | 本项目与 Apple Inc. **无关联**，未获其背书 |

**分发前请自查**（这是义务，不是建议）：

1. 保留所有上游的版权声明与许可文本（本仓库各 `third_party/*/COPYING`、`LICENSE`、
   `LICENSES/` 均已随源码保留）；
2. 若你修改了任何 **LGPL** 组件并静态链接，需满足其重链接义务（提供目标文件或等效手段）；
3. **不要**把 macOS 镜像、boot ROM、导出的 Mac 目录一起分发；
4. 若重新分发 Turnip，请一并附上其 `PROVENANCE.txt` 与 Mesa 的 MIT 声明。

---

## 6. 本仓库对上游源码的本地补丁（必须保留）

这些改动**不在**上游，是 Android 移植必需的；**用上游原版替换对应目录会导致安装失败**，
且失败现象与补丁无关（像是超时 / 设备列表为空），排查成本极高。

| 文件 | 改动量 | 作用 |
|---|---|---|
| `third_party/idevicerestore/src/restore.c` | +109 | 在 iBoot 看到设备树之前补上 memory-map |
| `third_party/idevicerestore/src/recovery.c` | +11 −1 | 同一时间只允许一个安装器 |
| `third_party/usbmuxd/src/usb.c` / `usb.h` | +10 | 认 Apple **restore 模式**产品 ID `0x1280..0x1283`；没有这条，`usb_device_add()` 提前返回、`idevicerestore` 等设备超时 |
| `third_party/libimobiledevice-glue/src/thread.c` | +13 | 拉长等待时间 |
| `android/third_party/metal2vulkan-runtime/src/*`（10 个文件） | +200 −19 | `diag.rs` 探针（受 `REIMS_DIAG` 门控）与 no-copy 路径的 import gate |
| `tools/patches/m2v-rog-unordered.patch` | — | translator 侧：`REIMS_ROG_UNORDERED=1` 让设备无 fragment interlock 时按 unordered 降级 |
| `android/third_party/reims-vgpu` 分支 `rebuild` | 12 个提交 | Android 窗口 / swapchain / 页面大小 / 驱动 bring-up / `REIMS_DIAG` 探针 |

---

## 7. 如何核对这份清单没有过期

```sh
# 1) third_party 里记录的版本是否仍在上游存在
tools/clone-deps.sh --verify

# 2) 我们到底对上游改了多少
git log --oneline -- third_party/
git diff --stat bb36f6f HEAD -- third_party/
git -C android/third_party/reims-vgpu log --oneline origin/master..rebuild

# 3) 依赖的三种形态是否如实
git ls-tree HEAD android/third_party/    # 160000 = gitlink, 040000 = vendored tree
```
