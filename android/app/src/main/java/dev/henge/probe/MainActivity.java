package dev.henge.probe;

import android.app.Activity;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The app: three tabs, in the shape the iOS app has.
 *
 *   Macs      the machine as you left it, and one big start button
 *   Install   install macOS from an IPSW, with the installer's own progress
 *   Settings  what applies to the app: the Vulkan driver, defaults, diagnostics, the log
 *
 * The tab bar is built by hand rather than with a navigation library, because there is no
 * AndroidX here and three text tabs are not worth the dependency. What matters is the property
 * the iOS structure has and a pile of top-level screens does not: **each tab has one subject**,
 * and anything about a particular machine is on that machine's page rather than in the settings.
 *
 * Tabs are views, not activities: switching between them must not destroy and rebuild them, and a
 * tab that owns a running install must keep owning it while the user looks at something else.
 */
public class MainActivity extends Activity {

    private FrameLayout content;
    private final LinearLayout[] items = new LinearLayout[3];
    private final TextView[] labels = new TextView[3];
    private final android.widget.ImageView[] icons = new android.widget.ImageView[3];
    private static final String[] TITLES = {"Macs", "安装", "设置"};
    private static final int[] ICONS = {
            R.drawable.ic_macs, R.drawable.ic_install, R.drawable.ic_settings};
    private int current = -1;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Window theme and window background first: everything below draws on top.
        Theme.apply(this);
        AppLog.i("Macs: 打开");

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        Ui.inset(root);

        content = new FrameLayout(this);
        root.addView(content, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        // A Material 3 navigation bar: three destinations of equal weight, each an icon over a
        // label, the current one marked by a pill behind its icon. Three is the number because the
        // app has three subjects; a fourth would be a subject that does not exist yet.
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setBackgroundColor(Theme.card(this));
        bar.setMinimumHeight(Theme.navHeight(this));
        for (int i = 0; i < TITLES.length; i++) {
            final int index = i;
            LinearLayout item = new LinearLayout(this);
            item.setOrientation(LinearLayout.VERTICAL);
            item.setGravity(Gravity.CENTER);
            item.setMinimumHeight(Theme.navHeight(this));
            item.setClickable(true);
            item.setFocusable(true);
            item.setBackground(Theme.ripple(this, Theme.onSurface(this)));
            item.setContentDescription(TITLES[i]);

            // The pill: 64 by 32, filled only for the destination on show.
            FrameLayout pill = new FrameLayout(this);
            FrameLayout indicator = new FrameLayout(this);
            android.graphics.drawable.GradientDrawable pillShape =
                    new android.graphics.drawable.GradientDrawable();
            pillShape.setCornerRadius(Ui.dp(this, 16));
            pillShape.setColor(Theme.primaryContainer(this));
            indicator.setBackground(pillShape);
            indicator.setAlpha(0f);
            indicator.setTag("indicator");
            android.widget.ImageView icon = new android.widget.ImageView(this);
            icon.setImageResource(ICONS[i]);
            icon.setImageTintList(android.content.res.ColorStateList.valueOf(Theme.variant(this)));
            int size = Ui.dp(this, 24);
            FrameLayout.LayoutParams iconLp = new FrameLayout.LayoutParams(size, size, Gravity.CENTER);
            pill.addView(icon, iconLp);
            pill.addView(indicator, new FrameLayout.LayoutParams(
                    Ui.dp(this, 64), Ui.dp(this, 32), Gravity.CENTER));
            bar.addView(item, new LinearLayout.LayoutParams(0,
                    ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
            LinearLayout.LayoutParams pillLp = new LinearLayout.LayoutParams(
                    Ui.dp(this, 64), Ui.dp(this, 32));
            pillLp.topMargin = Ui.dp(this, 10);
            item.addView(pill, pillLp);

            TextView label = new TextView(this);
            label.setText(TITLES[i]);
            label.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 12f);
            label.setGravity(Gravity.CENTER);
            label.setTextColor(Theme.variant(this));
            label.setPadding(0, Ui.dp(this, 2), 0, Ui.dp(this, 10));
            item.addView(label);

            item.setOnClickListener(v -> select(index));
            items[i] = item;
            labels[i] = label;
            icons[i] = icon;
        }
        root.addView(bar, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        setContentView(root);
        select(0);
        // One permission, asked once, before anything needs it: without it a download cannot be a
        // foreground service, and asking at the moment of download would be asking too late.
        Permissions.askAtStartup(this);
    }

    /** Shows tab `index`, building its view the first time it is asked for. */
    private void select(int index) {
        if (index == current) return;
        current = index;
        content.removeAllViews();
        View view;
        switch (index) {
            case 1: view = InstallTab.build(this); break;
            case 2: view = SettingsTab.build(this); break;
            default: view = MacsTab.build(this, () -> select(1)); break;
        }
        content.addView(view, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        for (int i = 0; i < items.length; i++) {
            boolean active = i == index;
            labels[i].setTextColor(active ? Theme.onSurface(this) : Theme.variant(this));
            labels[i].setTypeface(active
                    ? android.graphics.Typeface.DEFAULT_BOLD : android.graphics.Typeface.DEFAULT);
            icons[i].setImageTintList(android.content.res.ColorStateList.valueOf(
                    active ? Theme.onSurface(this) : Theme.variant(this)));
            // The indicator animates because a destination that changes instantly looks like a
            // page that reloaded; the cross-fade says "same app, different place".
            View indicator = ((ViewGroup) items[i].getChildAt(0)).getChildAt(1);
            indicator.animate().alpha(active ? 1f : 0f).setDuration(180).start();
            items[i].setSelected(active);
        }
    }

    /**
     * The one place a screen's result comes back to: the IPSW picker.
     *
     * Kept in the activity because a tab is a view and a view cannot receive an activity result;
     * the tab is told about it once the activity has it.
     */
    @Override
    protected void onActivityResult(int requestCode, int resultCode, android.content.Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode != InstallTab.REQUEST_IPSW) return;
        if (resultCode != RESULT_OK || data == null || data.getData() == null) {
            AppLog.i("导入: 已取消选择");
            return;
        }
        try {
            getContentResolver().takePersistableUriPermission(data.getData(),
                    android.content.Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Throwable t) {
            // Not every provider grants this; the read below still works for this session.
            AppLog.i("导入: 无法持久化读取权限（" + t + "）");
        }
        InstallTab.imported(this, data.getData());
    }

    /** Rebuilds the tab on screen, for when something it shows has changed underneath it. */
    void reloadTab() {
        int keep = current;
        current = -1;
        select(keep < 0 ? 0 : keep);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // Coming back from a pushed screen can change what a tab shows (a machine renamed, a disk
        // installed): rebuild the one on screen rather than leaving a stale value.
        int keep = current;
        current = -1;
        select(keep < 0 ? 0 : keep);
    }
}
