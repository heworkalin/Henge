# APK 与 CLI 必须是同一个程序：编译选项一致性（2026-11-19）

**一句话**：同一个引擎，用 NDK 的 CMake 构建出的 `.so` 和用直接 clang 命令行构建出的 CLI 可执行文件
**不是同一个程序**——CMake 工具链注入了 4 个 CLI 没有的选项，其中 3 个改变语义。
结果是 APK 的 guest 停在 `restored_external` 刚启动的地方：磁盘 0 块写入、`restore-step` 不动，
而 CLI 同一份代码能跑到 ASR 90%。对齐之后 APK 立刻开始写盘（`format_media`、GPT/APFS 探测、进度 7%）。

---

## 1. 症状（对齐前）

| 指标 | CLI | APK |
|---|---|---|
| `restored_external` | 启动后继续跑 166,773 次 EL0 fault | 启动后停住 |
| `restore-step-uptime` | 10 → 46 持续更新 | 不更新 |
| disk blocks | 数百万 | **0** |
| 恢复进度 | ASR 90% | 无 |

排查中排除了：boot-args（两边都是 `rev 3 ver 2`）、路径（FUSE vs ext4，结果一致）、
RAM/CPU、PVG（安装模式都是 `pvg_lite`）、源文件清单（逐项对齐，`net_min.c` 曾漏过）。

## 2. 根因：NDK CMake 工具链注入的选项

`build/android-arm64-v8a/CMakeFiles/henge.dir/flags.make` 里的实际 `C_FLAGS`：

```
-fstack-protector-strong  -D_FORTIFY_SOURCE=2  -O3  -DNDEBUG   ← NDK/CMake 自动注入
-O2 -g -fPIC -fvisibility=hidden -Wall -Wextra -include hv_darwin_compat.h   ← 项目自己写的
```

而 `tools/build-install-android.sh` 构建 CLI 时的：

```
-std=gnu11 -O2 -g -w -D_GNU_SOURCE -D__ANDROID__ -include hv_darwin_compat.h
```

差异与后果：

| 选项 | CLI | APK | 后果 |
|---|---|---|---|
| `-fstack-protector-strong` | 无 | **有** | 每个函数插入栈 canary，改变栈帧布局。AGENTS §7 明确写着 guest 代码路径不能带 |
| `-DNDEBUG` | 无 | **有** | **引擎里所有 `assert()` 被编译掉**，等于跑了不同的程序 |
| `-D_FORTIFY_SOURCE=2` | 无 | **有** | `memcpy`/`strcpy` 等被换成带边界检查的变体 |
| `-D_GNU_SOURCE` | **有** | **缺** | 部分 POSIX 声明缺失，函数可能被隐式声明为返回 `int`（指针被截断） |
| `-O3` | 无 | 有 | 被后面的 `-O2` 覆盖，无影响 |

## 3. 修法

`android/CMakeLists.txt`，**必须放在 `add_library(henge ...)` 之前**（放后面只影响后续目标，
第一次就踩了这个坑——`flags.make` 里什么都没变）：

```cmake
add_compile_options(-D_GNU_SOURCE -fno-stack-protector -U_FORTIFY_SOURCE -UNDEBUG)
```

顺序很重要，且已验证：

```
flags.make:  -fstack-protector-strong ... -fno-stack-protector   ← 后者生效
             -DNDEBUG               ... -UNDEBUG                 ← 生效
             -D_FORTIFY_SOURCE=2    ... -U_FORTIFY_SOURCE         ← 生效
             -O3                    ... -O2                       ← 生效
```

另外去掉 APK 独有的 `-mbranch-protection=standard`（BTI/PAC 插桩，CLI 没有）。

## 4. 现在的等价关系

```
主体代码 : 相同（core/*.c、devices/*.c、host 的 5 个文件、android/compat 的 4 个）
编译选项 : 相同（-O2 -g -fPIC -fvisibility=hidden -D_GNU_SOURCE -fno-stack-protector -std=gnu11）
PVG      : 安装模式都用 pvg_lite（devices/vmapple.c 的 installer 分支）
唯一不同 : 入口 —— CLI 是 host/main.c，APK 是 probe_jni.c + install.c
```

验证手段：

```bash
# APK 里的 .so 必须和本地构建是同一份
unzip -p app-debug.apk lib/arm64-v8a/libhenge.so > /tmp/a.so
llvm-readelf -n /tmp/a.so | grep -A1 "Build ID"
llvm-readelf -n build/android-arm64-v8a/lib/arm64-v8a/libhenge.so | grep -A1 "Build ID"
```

**不要用 `HENGEBUILD` 时间戳判断版本**：它来自 `android/install.c`，那个文件不改就不会重编，
所以换 `exception.c` 之后仍是旧时间戳（这次因此误判过一轮）。

## 5. 结论

> **"外部包装不同"必须严格限定为入口不同。**
> 构建系统各自注入的编译选项（栈保护、NDEBUG、FORTIFY、GNU_SOURCE、branch-protection）
> 都是**程序语义的一部分**，不是"优化偏好"。任何一项不同，就不该用"同一份代码"来解释两边的行为差异。
>
> 排查顺序应该是：**先证明两边是同一个二进制（或至少同一个选项集），再去怀疑代码**。
> 这一条如果早做，可以省掉后面一大段在对齐前的各种误判。
