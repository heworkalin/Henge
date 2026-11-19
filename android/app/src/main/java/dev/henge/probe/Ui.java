package dev.henge.probe;

import android.app.Activity;
import android.graphics.Insets;
import android.os.Build;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The few view helpers every screen needs, in one place because they encode two things learned
 * the hard way rather than taste.
 *
 * **Insets.** From Android 15 the platform enforces edge-to-edge for apps targeting SDK 35+, so
 * the window is laid out behind the status and navigation bars whether the app asked or not.
 * Every screen has to inset itself; `fitsSystemWindows` is not a substitute. Doing it in one
 * helper means a new screen cannot forget it.
 *
 * **Text size and monospace.** The diagnostic screens show machine output, and proportional text
 * turns a report into something that cannot be read or compared.
 */
final class Ui {

    private Ui() {
    }

    /** Density-scaled dp, the only unit any of this uses. */
    static int dp(Activity a, float value) {
        return Math.round(value * a.getResources().getDisplayMetrics().density);
    }

    /** Pads `root` by the system bars and cutout, now and whenever they change. */
    static void inset(View root) {
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            int top, bottom, left, right;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                Insets bars = insets.getInsets(
                        WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                top = bars.top;
                bottom = bars.bottom;
                left = bars.left;
                right = bars.right;
            } else {
                top = insets.getSystemWindowInsetTop();
                bottom = insets.getSystemWindowInsetBottom();
                left = insets.getSystemWindowInsetLeft();
                right = insets.getSystemWindowInsetRight();
            }
            v.setPadding(left, top, right, bottom);
            return insets;
        });
    }

    /** A vertical root with the insets applied, the page colour, and one column of children. */
    static LinearLayout column(Activity a) {
        LinearLayout root = new LinearLayout(a);
        root.setOrientation(LinearLayout.VERTICAL);
        inset(root);
        Theme.paint(a, root);
        return root;
    }

    /** A screen title, above the content. Sized and coloured by {@link Theme}. */
    static TextView title(Activity a, String text) {
        TextView v = new TextView(a);
        v.setText(text);
        Theme.title(a, v);
        int pad = dp(a, 16);
        v.setPadding(pad, dp(a, 20), pad, dp(a, 8));
        return v;
    }

    /** Body text: proportional, for reading. */
    static TextView body(Activity a, String text) {
        TextView v = new TextView(a);
        v.setText(text);
        Theme.body(a, v);
        int pad = dp(a, 16);
        v.setPadding(pad, 0, pad, dp(a, 8));
        v.setTextIsSelectable(true);
        return v;
    }

    /** Machine text: monospace, selectable, no wrapping surprises it cannot recover from. */
    static TextView mono(Activity a, String text, float sizeSp) {
        TextView v = new TextView(a);
        v.setText(text);
        Theme.mono(a, v, sizeSp);
        v.setTextIsSelectable(true);
        int pad = dp(a, 16);
        v.setPadding(pad, 0, pad, dp(a, 4));
        return v;
    }

    /** A full-width button with `text`, adding it to `parent`. */
    static Button button(Activity a, LinearLayout parent, String text, Runnable onClick) {
        Button b = new Button(a);
        b.setText(text);
        b.setAllCaps(false);
        b.setOnClickListener(v -> onClick.run());
        int pad = dp(a, 16);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(pad, dp(a, 4), pad, dp(a, 4));
        parent.addView(b, lp);
        return b;
    }

    /** A horizontal row of equal-width buttons. */
    static LinearLayout row(Activity a) {
        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        int pad = dp(a, 16);
        row.setPadding(pad, dp(a, 4), pad, dp(a, 4));
        return row;
    }

    /**
     * A button that fills an equal share of `row`.
     *
     * Built the same way as outlinedButton(), not as a default android.widget.Button: the platform
     * button carries its own tint that follows neither our accent nor our surfaces, so a row of
     * them read as belonging to another app. This was visible on the diagnostics screen, where
     * 复制 and 分享 sat under a themed list looking nothing like it.
     */
    static Button rowButton(Activity a, LinearLayout row, String text, Runnable onClick) {
        Button b = outlinedButton(a, text, onClick);
        row.addView(b, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));
        /* Equal shares need a gap between them, or the outlines touch. */
        LinearLayout.LayoutParams lp = (LinearLayout.LayoutParams) b.getLayoutParams();
        lp.setMarginStart(dp(a, 4));
        lp.setMarginEnd(dp(a, 4));
        b.setLayoutParams(lp);
        return b;
    }

    /**
     * The one emphasised action on a screen, filled with the accent colour.
     *
     * Emphasis is a scarce thing: more than one filled button on a screen and none of them reads
     * as the next step. Everything else is an outlined or plain button.
     */
    static Button primaryButton(Activity a, String text, Runnable onClick) {
        Button b = new Button(a);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 16f);
        b.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        b.setTextColor(Theme.onPrimary(a));
        // A filled button that is only as tall as its text looks like a piece of text with a
        // colour behind it. 54dp is a target you can hit without aiming, and it reads as a control.
        b.setBackground(Theme.fillBackground(a, Theme.primary(a), 16f));
        b.setMinHeight(Ui.dp(a, 54));
        b.setMinimumHeight(Ui.dp(a, 54));
        b.setPadding(Ui.dp(a, 16), Ui.dp(a, 14), Ui.dp(a, 16), Ui.dp(a, 14));
        b.setOnClickListener(v -> onClick.run());
        return b;
    }

    /** An outlined button: an action that matters less than the filled one. */
    static Button outlinedButton(Activity a, String text, Runnable onClick) {
        Button b = new Button(a);
        b.setText(text);
        b.setAllCaps(false);
        b.setTextSize(android.util.TypedValue.COMPLEX_UNIT_SP, 15f);
        b.setTextColor(Theme.primary(a));
        android.graphics.drawable.GradientDrawable bg = new android.graphics.drawable.GradientDrawable();
        bg.setCornerRadius(dp(a, 14));
        bg.setColor(android.graphics.Color.TRANSPARENT);
        bg.setStroke(Math.max(1, dp(a, 1)), Theme.withAlpha(Theme.primary(a), 0.5f));
        b.setBackground(bg);
        b.setMinHeight(Theme.touch(a));
        b.setOnClickListener(v -> onClick.run());
        return b;
    }
}
