package dev.henge.probe;

import android.app.Activity;
import android.app.AlertDialog;
import android.os.Bundle;
import android.text.InputType;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;

import java.io.File;

/**
 * One machine's own page: what it is called, what it is made of, and what it is using.
 *
 * On iOS this is `MacSettingsView`, pushed from the Macs tab. It is the only place a machine's
 * configuration is edited — the Macs tab shows the values, this page changes them — because a
 * setting that can be changed in two places is a setting that will one day disagree with itself.
 *
 * The disk figures are read from the filesystem when the page opens rather than stored: a cached
 * "used" number would be wrong from the moment the guest wrote to the disk, which is always.
 */
public class MacSettingsActivity extends Activity {

    private LinearLayout root;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Window theme and window background first: everything below draws on top.
        Theme.apply(this);
        root = Ui.column(this);
        root.addView(Ui.title(this, Prefs.instanceName(this)));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(root, new android.view.ViewGroup.LayoutParams(
                android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                android.view.ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(scroll);
    }

    @Override
    protected void onResume() {
        super.onResume();
        rebuild();
    }

    private void rebuild() {
        while (root.getChildCount() > 1) root.removeViewAt(root.getChildCount() - 1);

        LinearLayout general = Lists.group(this, root, "通用");
        Lists.row(this, general, "名称", "在 Macs 列表里显示", Prefs.instanceName(this), true,
                () -> editName());
        Lists.divider(this, general);
        Lists.row(this, general, "内存", "下次启动生效", Prefs.ramMb(this) + " MB", true,
                () -> choose("内存", new String[]{"1024", "2048", "3072", "4096", "6144", "8192"},
                        String.valueOf(Prefs.ramMb(this)), v -> Prefs.setRamMb(this, Integer.parseInt(v))));
        Lists.divider(this, general);
        Lists.row(this, general, "CPU", "下次启动生效", Prefs.cpus(this) + " 核", true,
                () -> choose("CPU 核数", new String[]{"1", "2", "3", "4", "5", "6", "7", "8"},
                        String.valueOf(Prefs.cpus(this)), v -> Prefs.setCpus(this, Integer.parseInt(v))));

        LinearLayout storage = Lists.group(this, root, "存储");
        File dir = new File(getExternalFilesDir(null), "vm");
        Lists.row(this, storage, "磁盘", "macOS 写在这里", diskText(new File(dir, "disk.img")), false, null);
        Lists.divider(this, storage);
        Lists.row(this, storage, "辅助存储", "NVRAM 一类的引导状态",
                fileText(new File(dir, "aux.img")), false, null);
        Lists.divider(this, storage);
        // The image is wherever it was downloaded or imported to -- "macOS.ipsw" was a name nothing
        // ever created, so this row always said 无 while the real file sat next to the disk.
        File image = installImage(dir);
        Lists.row(this, storage, "IPSW", image == null ? "还没有可用的安装镜像" : image.getName(),
                image == null ? "无" : fileText(image), false, null);
    }

    /** The IPSW an install would use: the imported one if it still exists, else the first in vm. */
    private File installImage(File dir) {
        String imported = Prefs.localIpsw(this);
        if (imported != null) {
            File f = new File(imported);
            if (f.isFile()) return f;
        }
        File[] found = dir.listFiles((d, n) -> n.endsWith(".ipsw"));
        return found == null || found.length == 0 ? null : found[0];
    }

    private static String diskText(File disk) {
        if (!disk.exists()) return "未安装";
        long bytes = disk.length();
        if (bytes <= 0) return "空";
        return (bytes >> 30) + " GB";
    }

    private static String fileText(File f) {
        if (!f.exists()) return "无";
        long bytes = f.length();
        if (bytes >= (1L << 30)) return (bytes >> 30) + " GB";
        if (bytes >= (1L << 20)) return (bytes >> 20) + " MB";
        return bytes + " B";
    }

    private void editName() {
        EditText field = new EditText(this);
        field.setInputType(InputType.TYPE_CLASS_TEXT);
        field.setText(Prefs.instanceName(this));
        new AlertDialog.Builder(this)
                .setTitle("名称")
                .setView(field)
                .setPositiveButton("保存", (d, w) -> {
                    String value = field.getText().toString().trim();
                    if (!value.isEmpty()) Prefs.setInstanceName(this, value);
                    rebuild();
                })
                .setNegativeButton("取消", null)
                .show();
    }

    private void choose(String title, String[] options, String current,
                        java.util.function.Consumer<String> sink) {
        int checked = 0;
        for (int i = 0; i < options.length; i++) if (options[i].equals(current)) checked = i;
        new AlertDialog.Builder(this)
                .setTitle(title)
                .setSingleChoiceItems(options, checked, (dialog, which) -> {
                    sink.accept(options[which]);
                    dialog.dismiss();
                    rebuild();
                })
                .setNegativeButton("取消", null)
                .show();
    }
}
