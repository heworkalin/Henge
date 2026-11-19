package dev.henge.probe;

import android.app.Activity;
import android.content.Intent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

/**
 * The Macs tab: the Mac as you left it, and one big button.
 *
 * This is the iOS app's home screen, in its essentials: a hero for the machine used last — its
 * name, what it is made of, and a single "start" that is the reason the app exists — then two
 * facts worth seeing without opening anything, then the other machines. Everything that *changes*
 * the machine is one tap away on the machine's own page, not on this one: a home screen that also
 * edits is a screen that has to be re-read every time you come back to it.
 *
 * The "add" action is the plus in the corner on iOS. Android has no navigation-bar plus here, so
 * it is a row at the bottom of the list, offering the one thing this build can do — install macOS.
 */
final class MacsTab {

    private MacsTab() {
    }

    static View build(Activity a, Runnable openInstall) {
        ScrollView scroll = new ScrollView(a);
        scroll.setBackgroundColor(Theme.surface(a));
        LinearLayout root = new LinearLayout(a);
        root.setBackgroundColor(Theme.surface(a));
        int pad = Ui.dp(a, 16);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        String name = Prefs.instanceName(a);
        int ram = Prefs.ramMb(a);
        int cpus = Prefs.cpus(a);
        String driver = Prefs.driverLabel(a);
        if (driver == null) {
            Drivers.Driver embedded = Drivers.embedded(a);
            driver = embedded == null ? "系统驱动" : embedded.label;
        }

        // ---- the machine, as a hero ----
        LinearLayout hero = new LinearLayout(a);
        hero.setOrientation(LinearLayout.VERTICAL);
        hero.setPadding(pad, pad, pad, pad);
        hero.setBackground(Theme.cardBackground(a, Theme.cardHigh(a), 16f));

        TextView title = new TextView(a);
        title.setText(name);
        Theme.title(a, title);
        hero.addView(title);

        TextView summary = new TextView(a);
        summary.setText(ram + " MB · " + cpus + " CPU · " + driver);
        Theme.rowSubtitle(a, summary);
        summary.setPadding(0, Ui.dp(a, 4), 0, 0);
        hero.addView(summary);

        TextView state = new TextView(a);
        state.setText("未运行");
        Theme.rowSubtitle(a, state);
        state.setPadding(0, Ui.dp(a, 8), 0, Ui.dp(a, 12));
        hero.addView(state);

        // The one filled button in the app: this is what the app is for.
        android.widget.Button start = Ui.primaryButton(a, "启动",
                () -> a.startActivity(new Intent(a, DisplayActivity.class)));
        hero.addView(start, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        LinearLayout.LayoutParams heroLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        root.addView(hero, heroLp);

        // ---- two facts, side by side ----
        LinearLayout facts = new LinearLayout(a);
        facts.setOrientation(LinearLayout.HORIZONTAL);
        facts.setPadding(0, Ui.dp(a, 12), 0, 0);
        facts.addView(fact(a, "磁盘", diskText(a)), new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        facts.addView(fact(a, "上次使用", "—"), new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(facts);

        // ---- the machine, and adding one ----
        LinearLayout group = Lists.group(a, root, "这台 Mac");
        // One way in. "显示" used to sit here as well, duplicating the hero's 启动 button: two rows
        // that did the same thing, so whichever you used, the other looked broken.
        Lists.row(a, group, "设置", "名称、内存、CPU、存储", null, true,
                () -> a.startActivity(new Intent(a, MacSettingsActivity.class)));

        LinearLayout add = Lists.group(a, root, "添加");
        // A tab, not a screen: installing is a subject of its own, and the tab bar is where it
        // lives. The callback keeps this view from having to know how the tabs are held.
        Lists.row(a, add, "安装 macOS", "从 IPSW 安装到这台设备", null, true, openInstall);

        scroll.addView(root, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return scroll;
    }

    /** What the disk image takes, by asking the filesystem — no cached counter to go stale. */
    private static String diskText(Activity a) {
        java.io.File disk = new java.io.File(
                new java.io.File(a.getExternalFilesDir(null), "vm"), "disk.img");
        if (!disk.exists()) return "未安装";
        long used = disk.length();
        return used > 0 ? (used >> 30) + " GB" : "空";
    }

    /** One fact: a small label over a large value, as the iOS screen has it. */
    private static View fact(Activity a, String label, String value) {
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(a, 12);
        box.setPadding(pad, pad, pad, pad);
        box.setBackground(Theme.cardBackground(a, Theme.card(a), 14f));

        TextView l = new TextView(a);
        l.setText(label);
        Theme.rowSubtitle(a, l);
        box.addView(l);

        TextView v = new TextView(a);
        v.setText(value);
        v.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 20f);
        v.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        v.setTextColor(Theme.onSurface(a));
        box.addView(v);
        return box;
    }
}
