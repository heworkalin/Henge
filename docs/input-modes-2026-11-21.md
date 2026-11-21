# 输入方式（触控 / 鼠标 / 键盘 / 输入法）

> 这份文档是屏幕上的**唯一说明**。App 本身不弹任何输入相关的提示文字：状态由图标本身表达，
> 说明放在这里，供需要时查。

屏幕上边缘有一个**悬浮球**（可拖动，会贴到最近的一侧边沿；横屏时就是左右边条，不挡画面）。
点它，旁边出现五个图标，4 秒后自动收起：

| 图标 | 作用 |
|---|---|
| ▶ / ■ | 启动 / 停止机器 |
| 指尖 / 箭头 | 切换「触控」与「鼠标」两种输入模式（图标形状与颜色就是当前模式） |
| 键盘 | 显示/隐藏屏幕下方的控制键（Esc Tab ⌘ ⌃ ⌥ ⇧ ↵ ⌫ ← ↑ ↓ →） |
| 文 | 唤出手机自己的输入法（打字用） |
| ← | 返回（本屏退到后台，机器继续运行） |

悬浮球本身也画着当前模式：**指尖 + 橙色 = 触控**，**箭头 + 蓝色 = 鼠标**。

---

## 1. 两种指针模式

guest（macOS）里只有一台 **Magic Trackpad 2**（`devices/xhci.c`，Apple `05ac:0265`），
没有触摸屏、也没有绝对定位设备。所以手指只能变成"触控板上的手指"，两种模式的区别是**手指位移怎么算成指针位移**：

### 触控（橙色）

**以按下的那一点为锚点**：手指离按下点多远，指针就从按下时的位置走多远。
移动 3 厘米，指针就走固定的 3 厘米对应的距离——不管划得多快，结果一样。

- 适合：屏幕小、想直接"指到"远处目标。
- 轻点 = 点击。
- **按住不动 0.42 秒后再拖 = 拖拽**（按住时触控板的键已经按下）。
- 缺点：一次按下能移动的距离受手指行程限制。

### 鼠标（蓝色）

**每一帧的位移累加**：手指短划一下指针走一小段，重复划可以一直走；可以抬起手指再继续。

- 适合：精确操作（选文本、调滑块、点小按钮）。
- 轻点 = 点击；按住不动后再拖 = 拖拽（同上）。
- 这是触控板的原生手感。

> 两者都**不做指针加速**：macOS 自己会对触控板做加速曲线，我们再加一层就是双重加速。
> 增益只在 App 里调（`VmInput.TOUCH_GAIN` / `MOUSE_GAIN`）。

## 2. 多指手势（原生，不需要我们翻译）

两指/三指会作为**真正的多指**上报（id 1..4 各自独立），所以：

| 手势 | guest 里的行为 |
|---|---|
| 两指滑动 | 滚动 |
| 两指张合 | 缩放 / 智能缩放 |
| 两指轻点 | 次要点击（按系统设置） |
| 三指滑动 | Mission Control / 切换全屏 App（按系统设置） |
| 两指从右边缘滑 | 通知中心（按系统设置） |

这也是选真触控板而不是"鼠标模拟"的原因：`devices/xhci.c` 里那台 MT2 让 macOS **自己的**
手势引擎工作，我们不需要实现滚动、惯性、缩放。

## 3. 键盘

| 来源 | 路径 |
|---|---|
| 手机/蓝牙物理键盘 | `DisplayActivity.dispatchKeyEvent` → `VmInput.key()`：有 `getScanCode()` 的走 `hv_xhci_key_linux()`（复用 `devices/xhci.c` 里已有的 Linux→HID 表），没有的走内置 keycode→HID 表 |
| 屏幕控制键（键盘图标） | `VmInput.control("esc"/"cmd"/…)` → HID usage |
| 输入法（文图标） | 隐藏的 `EditText` 接 IME，`afterTextChanged` 把字符**逐键**打到 guest 键盘 |

要点：

- **中文/日文/emoji 由 guest 自己的输入法完成**：我们只发 ASCII 字母（拼音），
  guest 里选中的输入法负责候选与上屏。HID 键盘协议没有 Unicode，这是唯一正确的做法。
- 物理键盘的**按键重复由 guest 处理**，App 丢弃 `repeatCount > 0` 的事件。
- 返回键留给 App（`KEYCODE_BACK` 不转发给 guest）。

## 4. 已知边界

- **触控不是真正的绝对坐标**：它只是"锚点式"的相对位移。真"点哪打哪"需要给 xHCI 再加第三个
  设备——USB 绝对定位平板（QEMU 的 `usb-tablet`，`hw/usb/dev-hid.c` 有描述符可参考，
  GPL-2.0 与本项目兼容）。做了之后「触控」才算真触屏。
- 触控板按键只有**一个**（整块按下，Apple MT2 的语义），右键目前用 **Control + 点击**
  （macOS 惯例）；三键鼠标要另加设备或改描述符。

## 5. 实现位置

| 文件 | 内容 |
|---|---|
| `android/app/src/main/java/dev/henge/probe/InputBall.java` | 悬浮球、图标（全部用 Canvas 画，无图片资源）、拖动与贴边、自动收起 |
| `android/app/src/main/java/dev/henge/probe/VmInput.java` | 两种模式的手势数学、多指转发、键盘/IME 映射 |
| `android/app/src/main/java/dev/henge/probe/DisplayActivity.java` | 装配：Surface 的触摸监听、`dispatchKeyEvent`、输入法字段 |
| `android/probe_jni.c` | `nativeTouch / nativeTouchFrame / nativeTouchButton / nativeKey / nativeKeyLinux` |
| `android/henge_vm.c` | `hv_android_touch*` / `hv_android_key*` |
| `devices/xhci.c` | 真正的设备：Apple USB 键盘 + Magic Trackpad 2（报告格式、`hv_xhci_touch*`） |
