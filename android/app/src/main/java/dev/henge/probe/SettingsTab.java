package dev.henge.probe;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;

import java.io.File;
import java.util.List;

/**
 * The Settings tab: what applies to the app as a whole.
 *
 * This is the iOS SettingsView's scope, minus the parts this platform has no equivalent for
 * (JIT switching: JIT here is granted to the process and needs no asking; mouse and trackpad
 * buttons; volume-key input modes). What is left is genuinely global — the Vulkan driver the
 * process opens, the default memory and CPU count a new machine is made with, and the two
 * diagnostic screens behind it:
 *
 *   - **日志** is a share sheet on iOS; here it is a screen, because Android has no share sheet
 *     for a file this app writes itself, and the log is worth reading on the device.
 *   - **诊断** has no iOS counterpart because iOS does not need one: its Vulkan path, its
 *     executable memory and its driver loading are not things an app on that platform negotiates.
 *     On Android each of those is a measurement.
 *
 * Per-machine configuration is not here. It lives on the machine's own page, which is what the
 * Macs tab opens — one place for one decision.
 */
final class SettingsTab {

    private SettingsTab() {
    }

    static View build(Activity a) {
        LinearLayout root = Ui.column(a);
        root.addView(Ui.title(a, "设置"));

        ScrollView scroll = new ScrollView(a);
        scroll.setBackgroundColor(Theme.surface(a));
        LinearLayout content = new LinearLayout(a);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setBackgroundColor(Theme.surface(a));
        scroll.addView(content, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        // ---- appearance ----
        LinearLayout look = Lists.group(a, content, "外观");
        String mode = Prefs.appearance(a);
        String modeLabel = "system".equals(mode) ? "跟随系统" : ("dark".equals(mode) ? "深色" : "浅色");
        Lists.row(a, look, "主题", "深色、浅色，或跟随系统", modeLabel, true,
                () -> new AlertDialog.Builder(a)
                        .setTitle("主题")
                        .setSingleChoiceItems(new String[]{"跟随系统", "浅色", "深色"},
                                "system".equals(mode) ? 0 : ("light".equals(mode) ? 1 : 2),
                                (dialog, which) -> {
                                    Prefs.setAppearance(a, which == 0 ? "system"
                                            : (which == 1 ? "light" : "dark"));
                                    dialog.dismiss();
                                    // Recreating re-applies the window theme, which is the point.
                                    a.recreate();
                                })
                        .setNegativeButton("取消", null)
                        .show());
        Lists.divider(a, look);
        Lists.row(a, look, "主题色", "按钮与选中项的强调色", Prefs.accent(a), true,
                () -> new AlertDialog.Builder(a)
                        .setTitle("主题色")
                        .setSingleChoiceItems(Theme.ACCENTS, indexOf(Theme.ACCENTS, Prefs.accent(a)),
                                (dialog, which) -> {
                                    Prefs.setAccent(a, Theme.ACCENTS[which]);
                                    dialog.dismiss();
                                    a.recreate();
                                })
                        .setNegativeButton("取消", null)
                        .show());

        // ---- rendering ----
        LinearLayout render = Lists.group(a, content, "渲染");
        List<Drivers.Driver> drivers = Drivers.all(a);
        Drivers.Driver current = Drivers.resolve(drivers, Prefs.driverLabel(a));
        String label = current == null ? "无" : current.label;
        Lists.row(a, render, "驱动", "引擎打开的 Vulkan 库", label, true, () -> {
            String[] labels = new String[drivers.size()];
            for (int i = 0; i < drivers.size(); i++) labels[i] = drivers.get(i).label;
            new AlertDialog.Builder(a)
                    .setTitle("驱动")
                    .setSingleChoiceItems(labels, Math.max(0, drivers.indexOf(current)),
                            (dialog, which) -> {
                                Prefs.setDriverLabel(a, drivers.get(which).label);
                                AppLog.i("设置: 驱动 = " + drivers.get(which).label);
                                ((LinearLayout) content).post(() -> { });
                                dialog.dismiss();
                                // The list is rebuilt by reopening the tab; value is stored either way.
                            })
                    .setNegativeButton("取消", null)
                    .show();
        });

        // ---- defaults for a new machine ----
        LinearLayout defs = Lists.group(a, content, "默认配置");
        Lists.row(a, defs, "内存", "新机器默认给的物理内存", Prefs.ramMb(a) + " MB", true,
                () -> choose(a, "内存", new String[]{"1024", "2048", "3072", "4096", "6144", "8192"},
                        String.valueOf(Prefs.ramMb(a)), v -> Prefs.setRamMb(a, Integer.parseInt(v))));
        Lists.divider(a, defs);
        Lists.row(a, defs, "CPU", "新机器默认的核数", Prefs.cpus(a) + " 核", true,
                () -> choose(a, "CPU 核数", new String[]{"1", "2", "3", "4", "5", "6", "7", "8"},
                        String.valueOf(Prefs.cpus(a)), v -> Prefs.setCpus(a, Integer.parseInt(v))));

        // ---- display ----
        LinearLayout display = Lists.group(a, content, "显示");
        Lists.row(a, display, "控制条自动隐藏", "运行中 3 秒后淡出，点画面唤回",
                Prefs.hideControls(a) ? "开" : "关", false, () -> {
                    Prefs.setHideControls(a, !Prefs.hideControls(a));
                    if (a instanceof MainActivity) ((MainActivity) a).reloadTab();
                });

        // ---- storage ----
        LinearLayout storage = Lists.group(a, content, "存储");
        File vm = new File(a.getExternalFilesDir(null), "vm");
        Lists.row(a, storage, "macOS 磁盘", "guest 实际写入的部分", sizeOf(new File(vm, "disk.img")),
                false, null);
        Lists.divider(a, storage);
        String images = imagesSummary(vm);
        Lists.row(a, storage, "安装映像", "已下载或导入的 IPSW 与未完成的下载", images, true,
                () -> cleanImages(a, vm));

        // ---- diagnostics ----
        LinearLayout diag = Lists.group(a, content, "诊断");
        Lists.row(a, diag, "设备能力", "W^X、Vulkan、驱动来源、namespace", null, true,
                () -> a.startActivity(new Intent(a, DiagnosticsActivity.class)));
        Lists.divider(a, diag);
        Lists.row(a, diag, "日志", "本应用的运行记录", null, true,
                () -> a.startActivity(new Intent(a, LogsActivity.class)));

        // ---- permissions ----
        LinearLayout perms = Lists.group(a, content, "权限");
        Lists.row(a, perms, "通知", "下载在后台继续的凭据", Permissions.summary(a),
                Permissions.startupGranted(a) ? false : true, () -> {
                    if (Permissions.startupGranted(a)) {
                        Permissions.openSettings(a);
                    } else {
                        Permissions.askAtStartup(a);
                        if (a instanceof MainActivity) ((MainActivity) a).reloadTab();
                    }
                });

        // ---- about ----
        LinearLayout about = Lists.group(a, content, "关于");
        Lists.row(a, about, "设备",
                android.os.Build.MODEL + " / API " + android.os.Build.VERSION.SDK_INT, null, false, null);

        return root;
    }

    private static int indexOf(String[] values, String value) {
        for (int i = 0; i < values.length; i++) if (values[i].equals(value)) return i;
        return 0;
    }

    /** The size of a file, or that it is not there. */
    private static String sizeOf(File f) {
        if (!f.exists()) return "无";
        return AppLog.bytes(f.length());
    }

    /** How many images are on disk and how much room they take. */
    private static String imagesSummary(File vm) {
        File[] files = vm.listFiles((dir, name) -> name.endsWith(".ipsw") || name.endsWith(".part"));
        if (files == null || files.length == 0) return "无";
        long bytes = 0;
        for (File f : files) bytes += f.length();
        return AppLog.bytes(bytes);
    }

    /**
     * Offers to delete the images, naming them and their sizes first.
     *
     * An IPSW is up to twenty-four gigabytes and a download that resumes is worth keeping, so this
     * never deletes on a single tap and never deletes anything the user did not see listed. It also
     * leaves an imported image alone when it lives outside the app's directory: that file is the
     * user's, not ours to remove.
     */
    private static void cleanImages(Activity a, File vm) {
        File[] files = vm.listFiles((dir, name) -> name.endsWith(".ipsw") || name.endsWith(".part")
                || name.endsWith(".part.chunks"));
        if (files == null || files.length == 0) {
            new AlertDialog.Builder(a).setMessage("没有可清理的安装映像。")
                    .setPositiveButton("好", null).show();
            return;
        }
        StringBuilder list = new StringBuilder();
        long total = 0;
        for (File f : files) {
            list.append(f.getName()).append("    ").append(AppLog.bytes(f.length())).append('\n');
            total += f.length();
        }
        String imported = Prefs.localIpsw(a);
        new AlertDialog.Builder(a)
                .setTitle("清理安装映像（" + AppLog.bytes(total) + "）")
                .setMessage(list + "\n会从本应用目录删除以上文件。已安装的 macOS 不受影响。")
                .setPositiveButton("删除", (d, w) -> {
                    int removed = 0;
                    for (File f : files) {
                        // An image the user imported is only deleted if it is ours; otherwise the
                        // path points outside this directory and the delete fails harmlessly.
                        if (f.delete()) removed++;
                    }
                    if (imported != null && !new File(imported).getParentFile()
                            .equals(vm)) {
                        Prefs.setLocalIpsw(a, null);
                    }
                    AppLog.i("设置: 清理安装映像，删除 " + removed + " 个文件");
                    if (a instanceof MainActivity) ((MainActivity) a).reloadTab();
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private static void choose(Activity a, String title, String[] options, String current,
                               java.util.function.Consumer<String> sink) {
        int checked = 0;
        for (int i = 0; i < options.length; i++) if (options[i].equals(current)) checked = i;
        new AlertDialog.Builder(a)
                .setTitle(title)
                .setSingleChoiceItems(options, checked, (dialog, which) -> {
                    sink.accept(options[which]);
                    dialog.dismiss();
                })
                .setNegativeButton("取消", null)
                .show();
    }
}
