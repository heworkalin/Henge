# 一次被拒绝的驱动（保留记录，避免重复尝试）

来源：`FCL-Team/FCLDriverPlugin` @ `e7ccbe711a12c0436da1faca9756fbbb5f1c7284`
文件：`app/src/main/jniLibs/arm64-v8a/libvulkan_freedreno.so`
大小：10,393,544 字节（未 strip）
自述版本：`Mesa 24.3.2 (git-e3b1a93aaa)`
sha256：`c423448d380830ee15599fa1c810b7bc7c28ea7d83546da97c6f05e70f5c06c8`
BuildID：`499b1b39b66c858a62c4dd856560c0df8212bf9a`

## 为什么拒绝

`.dynsym` 中已定义的符号只有 **1 个**：

```
HMI   OBJECT   GLOBAL
```

而 Vulkan 入口点是 **LOCAL**，不是动态符号：

```
vk_icdGetInstanceProcAddr      LOCAL
vk_icdGetPhysicalDeviceProcAddr LOCAL
vk_icdNegotiateLoaderICDInterfaceVersion LOCAL
```

`dlsym()` 只查 `.dynsym`，因此：

- `dlopen()` 会成功
- `dlsym(handle, "vkGetInstanceProcAddr")` 返回 NULL
- 系统 Vulkan loader（走 `vk_icdGetInstanceProcAddr`）同样加载不了它

也就是说这个二进制**不能作为 ICD 使用，也不能 dlopen 调用**。它之所以在 FCL 里能工作，
是因为 FCL 有自己的 linkerhook（不在公开的这三个仓库里），我们无法照抄，也不应该去逆向它。

## 结论

需要的是**导出 `vk_icdGetInstanceProcAddr`（ICD）或 `vkGetInstanceProcAddr`（可直连库）**的
Turnip 构建，例如 AdrenoTools 系列的预编译产物。

`android/fetch-turnip.sh` 现在会在安装前检查这一点，不合格直接拒绝，不会写进 jniLibs。
