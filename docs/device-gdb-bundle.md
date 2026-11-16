# 设备端 gdb：自包含 bundle（2026-11-16）

本文记录 `tools/build-gdb-bundle.sh` 与 `tools/gdbwrap.c`：把设备上的 Termux gdb
打成**一个可整体搬移、不经过 shell 的目录**。

**证据等级**：标注【实测】的都在 `emulator-5554` 上跑过，命令与输出见 §4。

---

## 1. 为什么换掉 `gdb.sh`

原来的启动器是设备上的 `/data/local/tmp/henge/gdb.sh`：

```sh
P=/data/local/tmp/termux/files/usr
export LD_LIBRARY_PATH=$P/lib
export GUILE_LOAD_PATH=$P/share/guile/site:$P/share/guile/site/3.0
export GUILE_LOAD_COMPILED_PATH=$P/lib/guile/3.0/ccache:$P/share/guile/site/3.0
exec $P/bin/gdb "$@"
```

两个问题，都与 gdb 无关：

| 问题 | 后果 |
|---|---|
| 前缀是**写死的绝对路径** | Termux 包更新换掉库版本、或目录被搬动，`LD_LIBRARY_PATH` 就指向不存在的 soname |
| 每个参数都**过一次 shell** | 寄存器 dump、gdb 命令里的引号 / `$` / `;` / `!` / 通配符会被 shell 重新解释，gdb 收到的不是原样 |

## 2. 现在是什么

一个目录，`bin/gdb` 是**入口二进制**（`tools/gdbwrap.c`），真正的 gdb 在
`bin/gdb.real`：

```
gdb-bundle/
  bin/gdb          ← 入口（C 程序，只链系统 libc）
  bin/gdb.real     ← Termux 的 gdb 18.1（bionic 动态链接）
  lib/*.so         ← 它的整棵依赖树（17 个直接/间接依赖）
  lib/guile/       ← guile 编译缓存（缺了每次启动重编，很慢）
  lib/python3.14/  ← Python 标准库（缺了 gdb 的 python 命令不可用）
  share/guile/     ← guile Scheme 源（缺了 gdb 在 main() 前 abort）
  share/gdb/       ← gdb 的 data-directory（guile 的 boot.scm、python 支持库）
```

入口做的事就四件：

1. 用 `readlink("/proc/self/exe")` 取自己的路径，上跳两级得到 bundle 根
2. 用这个根设 `LD_LIBRARY_PATH` / `GUILE_LOAD_PATH` / `GUILE_LOAD_COMPILED_PATH` /
   `PATH` / `PYTHONHOME`
3. 在 `argv[1]` 插入 `--data-directory=<root>/share/gdb`（调用方自己传了就不插）
4. `execv(root/bin/gdb.real, argv)` —— **不经过任何 shell**

所以 bundle 可以 tar 走、改名、挪到任意路径；参数逐字节到达 gdb。

### 2.1 为什么不是"静态链接成一个文件"

`gdb` 本身做不到：Termux 提供的是 bionic 动态链接的可执行文件
（`interpreter /system/bin/linker64`，`RUNPATH /data/data/com.termux/files/usr/lib`）【实测】。
要做单文件得先交叉编译 gdb + guile + Python，成本远大于收益。

本方案的效果等价：**一个目录就是一个整体**，入口二进制只依赖系统 libc，
所以即使 bundle 里的库坏了，也还能看到 `gdbwrap: cannot exec ...` 而不是静默失败。

## 3. 构建与推送

```bash
./tools/build-gdb-bundle.sh              # 编到 build/gdb-bundle，再打 build/gdb-bundle.tar.gz
./tools/build-gdb-bundle.sh --push       # 构建 + 推送 + 冒烟测试
./tools/build-gdb-bundle.sh --push-only  # 只推送现有 tar
```

环境变量：`DEVICE`（默认 `emulator-5554`）、`HENGE_TERMUX_PREFIX`
（默认 `/data/local/tmp/termux/files/usr`）、`HENGE_BUNDLE_DIR`
（默认 `/data/local/tmp/henge/gdb-bundle`）。

脚本会：BFS 解析 `gdb` 的 NEEDED 并逐个从设备拉取（**必须 `readlink -f`**：
Termux 的 `lib/*.so` 是符号链接，`adb pull` 拒绝跟随，会报
`failed to stat remote object ... Permission denied`）；guile 与 Python 两个目录
用 `tar` 流传输（symlink 多、小文件多，`adb pull` 又慢又会跳过 symlink）。

体积【实测】：`build/gdb-bundle` 128 MB，tar.gz 36 MB，设备上同样是 128 MB。

## 4. 验证（全部【实测】）

```console
$ /data/local/tmp/henge/gdb-bundle/bin/gdb --version | head -1
GNU gdb (GDB) 18.1                     # 无 Python 警告、无 Guile abort

$ sleep 300 & gdb -p $! -batch -ex "info registers sp"
sp             0x7fc8615f50            # ptrace attach 可用

$ gdb --batch -ex 'printf "arg with \"quotes\" and $dollar and ; semicolon\n"'
arg with "quotes" and $dollar and ; semicolon    # 参数原样到达

$ cp -r gdb-bundle /data/local/tmp/bundle-moved && /data/local/tmp/bundle-moved/bin/gdb --version
GNU gdb (GDB) 18.1                     # 换路径仍可用（自定位生效）

$ gdb --batch -ex "python print(1+1)"
2                                     # Python 支持可用（PYTHONHOME 生效）
```

`gdb -p` 会打一批 `section .xxx not found in .gnu_debugdata for /system/bin/toybox`
警告，是 toybox 的 debugdata 不完整，**无害**，与 bionic 无关。

## 5. 设备上的旧名字

`/data/local/tmp/henge/gdb.sh` 已替换成指向 `gdb-bundle/bin/gdb` 的符号链接
（原文件备份为 `gdb.sh.orig`），所以之前记在别处的 `./gdb.sh ...` 调用照常工作，
但**不再经过 shell**。

## 6. 已知限制

- bundle 里没有 binutils（`objdump`/`readelf`），gdb 自己的反汇编不需要它们；
  真要 gdb 调外部工具时，`PATH` 里的 Termux `bin/` 仍是后备（入口把 `$root/bin`
  放在最前，其后保留原 `PATH`）
- 依赖递归只看 `NEEDED`，`dlopen` 按名字打开的库（如 `libthread_db.so`）靠脚本里
  的显式清单补，新增此类库时要往 `build-gdb-bundle.sh` 的 `extra` 里加
- 重新构建必须清空 `lib/`（脚本已 `rm -rf`）：旧 soname 会"满足"新 gdb 的依赖，
  然后在缺符号时以别的方式失败
