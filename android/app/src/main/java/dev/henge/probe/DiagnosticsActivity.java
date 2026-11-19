package dev.henge.probe;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Bundle;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.ArrayList;
import java.util.List;

/**
 * Diagnostics, laid out the way an information app is: one group per subject, each a block of
 * machine-readable lines, with the whole thing available as one piece of text.
 *
 * The report arrives from the native side as a single string, and it is the ground truth — this
 * screen does not re-derive anything. It only splits the report at the "--- section ---" lines the
 * native side already writes, so that "which driver produced these numbers" and "which step of the
 * namespace bypass failed" are each findable without scrolling through everything. When a section
 * needs to be quoted, the raw report is one tap away, and copying always copies the raw text,
 * never the rendering.
 *
 * The driver is chosen above the report because every number below depends on it, and a number
 * whose library is unknown is worse than no number.
 */
public class DiagnosticsActivity extends Activity {

    private final List<Drivers.Driver> drivers = new ArrayList<>();
    private Drivers.Driver chosen;
    private String report = "";
    private LinearLayout sections;
    private TextView driverValue;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Window theme and window background first: everything below draws on top.
        Theme.apply(this);
        AppLog.i("诊断: 打开");

        drivers.addAll(Drivers.all(this));
        chosen = Drivers.resolve(drivers, Prefs.driverLabel(this));

        LinearLayout root = Ui.column(this);
        root.addView(Ui.title(this, "诊断"));

        LinearLayout group = Lists.group(this, root, "来源");
        driverValue = new TextView(this);
        // Read-only: the driver is chosen in 设置 → 渲染, and a second place to change it meant the
        // two could disagree. Choosing one here used to affect only this probe, which looked like
        // the setting not sticking.
        Lists.row(this, group, "驱动", "当前引擎会打开的 Vulkan 库（在 设置 → 渲染 中切换）",
                chosen == null ? "无" : chosen.label, false, null);
        Lists.divider(this, group);
        Lists.row(this, group, "重新测量", "重新跑一遍全部检查", null, true, this::run);
        Lists.divider(this, group);
        Lists.row(this, group, "原始报告", "整份文本，可直接复制", null, true, this::showRaw);

        sections = new LinearLayout(this);
        sections.setOrientation(LinearLayout.VERTICAL);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(sections, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout buttons = Ui.row(this);
        Ui.rowButton(this, buttons, "复制", this::copy);
        Ui.rowButton(this, buttons, "分享", this::share);
        root.addView(buttons);

        setContentView(root);
        run();
    }

    private void run() {
        if (chosen == null) {
            report = "没有可用的驱动。";
        } else {
            AppLog.i("诊断: 驱动 = " + chosen.label);
            try {
                String result = Native.nativeProbe(1 << 20, chosen.dir, chosen.lib,
                        getCacheDir() == null ? null : getCacheDir().getAbsolutePath());
                report = result == null ? "(native 探针返回 null)" : result;
            } catch (Throwable t) {
                report = "探针异常:\n" + android.util.Log.getStackTraceString(t);
                AppLog.w("诊断: 探针异常", t);
            }
        }
        render();
    }

    /** One group per section of the report. */
    private void render() {
        sections.removeAllViews();
        String title = null;
        StringBuilder body = new StringBuilder();

        for (String line : report.split("\n", -1)) {
            String trimmed = line.trim();
            boolean header = trimmed.startsWith("--- ") && trimmed.endsWith(" ---");
            if (header) {
                flush(title, body);
                title = trimmed.substring(4, trimmed.length() - 4);
                body.setLength(0);
            } else {
                body.append(line).append('\n');
            }
        }
        flush(title, body);
    }

    private void flush(String title, StringBuilder body) {
        String text = body.toString();
        if (title == null && text.trim().isEmpty()) return;
        LinearLayout card = Lists.group(this, sections, title == null ? "报告" : title);
        TextView mono = Ui.mono(this, text.trim().isEmpty() ? "(空)" : text, 10f);
        mono.setPadding(0, 0, 0, 0);
        int pad = Ui.dp(this, 12);
        mono.setPadding(pad, pad, pad, pad);
        card.addView(mono, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
    }

    private void showRaw() {
        TextView mono = Ui.mono(this, report, 10f);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(mono);
        new AlertDialog.Builder(this)
                .setTitle("原始报告")
                .setView(scroll)
                .setPositiveButton("关闭", null)
                .show();
    }

    private void copy() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("Henge probe", report));
        Toast.makeText(this, "报告已复制", Toast.LENGTH_SHORT).show();
    }

    private void share() {
        android.content.Intent send = new android.content.Intent(
                android.content.Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(android.content.Intent.EXTRA_SUBJECT, "Henge probe");
        send.putExtra(android.content.Intent.EXTRA_TEXT, report);
        startActivity(android.content.Intent.createChooser(send, "分享报告"));
    }
}
