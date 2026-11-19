package dev.henge.probe;

import android.app.Activity;
import android.os.Bundle;
import android.widget.LinearLayout;
import android.widget.ScrollView;

/**
 * The open-source components this app is built on, with the licence each is used under.
 *
 * Every entry here is code that actually ships inside the APK or is linked into libhenge.so, not a
 * list of things the project merely mentions. The restore stack is the largest part: installing
 * macOS is done by Apple's own tools reimplemented as open source, and those carry the strongest
 * obligations (GPL/LGPL), so their sources are what a distribution has to make available.
 */
public class LicensesActivity extends Activity {
    private static final String[][] ITEMS = {
        // 恢复链路 / the restore stack
        {"idevicerestore", "LGPL-2.1+", "与恢复模式设备通信并驱动整个恢复流程",
         "https://github.com/libimobiledevice/idevicerestore"},
        {"libimobiledevice", "GPL-2.0", "设备服务与属性表",
         "https://github.com/libimobiledevice/libimobiledevice"},
        {"libimobiledevice-glue", "LGPL-2.1+", "libimobiledevice 的公共辅助代码",
         "https://github.com/libimobiledevice/libimobiledevice-glue"},
        {"libirecovery", "LGPL-2.1+", "恢复模式 / DFU 的 USB 命令",
         "https://github.com/libimobiledevice/libirecovery"},
        {"libplist", "GPL-2.0", "Apple 的属性列表格式",
         "https://github.com/libimobiledevice/libplist"},
        {"libtatsu", "LGPL-2.1+", "TSS 个性化（签名请求）",
         "https://github.com/libimobiledevice/libtatsu"},
        {"libusbmuxd", "LGPL-2.1+", "usbmux 多路复用协议",
         "https://github.com/libimobiledevice/libusbmuxd"},
        {"usbmuxd", "GPL-2.0", "在进程内运行的多路复用守护",
         "https://github.com/libimobiledevice/usbmuxd"},
        {"libzip", "BSD-3-Clause", "读写 IPSW（zip 容器）",
         "https://github.com/nih-at/libzip"},
        {"lzfse", "BSD (Apple)", "Apple 的压缩算法，解包镜像",
         "https://github.com/lzfse/lzfse"},
        // 密码学
        {"Mbed TLS", "Apache-2.0 (dual)", "AES / SHA-256，供 CommonCrypto 兼容层使用",
         "https://github.com/Mbed-TLS/mbedtls"},
        {"OpenSSL", "Apache-2.0", "HTTPS（访问 Apple 的签名与镜像服务器）",
         "https://github.com/openssl/openssl"},
        {"curl", "MIT", "HTTP 客户端",
         "https://github.com/curl/curl"},
        // 图形
        {"reims-vgpu", "GPL-2.0+ / LGPL-3.0+", "模拟 Apple 的半虚拟 GPU（VMAPPLE）",
         "https://github.com/ktemkin/reims-vgpu"},
        {"libadrenotools", "BSD-2-Clause", "在 App 的命名空间里加载 Vulkan 驱动",
         "https://github.com/bylaws/libadrenotools"},
        {"Mesa / Turnip", "MIT", "开源 Vulkan 驱动（Adreno）",
         "https://gitlab.freedesktop.org/mesa/mesa"},
        // 其它
        {"QEMU", "GPL-2.0", "VMAPPLE 设备模型的参考实现（仅作对拍，不链接）",
         "https://gitlab.com/qemu-project/qemu"},
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Theme.apply(this);
        ScrollView scroll = new ScrollView(this);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        Ui.inset(root);
        scroll.addView(root);

        add(root, "开源组件与许可证", true);
        add(root, "本应用包含下列开源代码。恢复相关组件（idevicerestore / libimobiledevice 等）"
                + "按其许可证要求，源代码可从各自上游仓库获取；分发本应用时应同时提供对应的源码与许可证文本。",
                false);
        for (String[] item : ITEMS) addLink(root, item);
        setContentView(scroll);
    }

    /* One component, tappable: the licence is what the page says, the link is where its source is.
       Opening the upstream is the point -- a licence page that only lists names cannot be checked. */
    private void addLink(LinearLayout root, String[] item) {
        android.widget.TextView v = new android.widget.TextView(this);
        v.setText(item[0] + "  —  " + item[1] + "\n" + item[2] + "\n" + item[3]);
        v.setTextSize(14);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        v.setPadding(pad, pad / 2, pad, pad / 2);
        v.setTextColor(Theme.primary(this));
        v.setOnClickListener(x -> {
            try {
                startActivity(new android.content.Intent(android.content.Intent.ACTION_VIEW,
                        android.net.Uri.parse(item[3])));
            } catch (Throwable t) {
                AppLog.w("许可证: 无法打开 " + item[3], t);
            }
        });
        root.addView(v);
    }

    private void add(LinearLayout root, String text, boolean title) {
        android.widget.TextView v = new android.widget.TextView(this);
        v.setText(text);
        v.setTextSize(title ? 20 : 14);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        v.setPadding(pad, pad / 2, pad, pad / 2);
        if (title) v.setTypeface(v.getTypeface(), android.graphics.Typeface.BOLD);
        root.addView(v);
    }
}
