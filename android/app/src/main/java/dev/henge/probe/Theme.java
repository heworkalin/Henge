package dev.henge.probe;

import android.app.Activity;
import android.content.res.Configuration;
import android.graphics.Color;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.util.TypedValue;
import android.view.View;

/**
 * The app's design tokens, in one place and in both themes.
 *
 * Why this file exists: the screens used to say what they wanted in raw colours — white at a fifth
 * of its alpha, white at half — which works on one background and silently fails on another. The
 * platform theme here follows the system, so a device in light mode got white text on a white page.
 * Colour is therefore named by **role** (`surface`, `card`, `onSurface`, `variant`, `primary`) and
 * the value behind each role is chosen once, per theme.
 *
 * The values follow Material 3's roles rather than its exact palette: this app is a Mac's screen,
 * not a Material app, and its neutrals are darker and its accent bluer than Material's default.
 * What is borrowed is the structure — a surface ramp, one accent, an outline — and the two rules
 * that are not negotiable:
 *
 *   - body text keeps a contrast ratio of at least 4.5:1 against what it sits on;
 *   - anything touchable is at least 48dp in both directions.
 *
 * Sizes come from the five type roles the platform's guidance names (display, headline, title,
 * subtitle, body) collapsed to what a settings-shaped app needs: a title, a section label, body
 * text, and monospace for machine output. Spacing is the 4dp grid: 4, 8, 12, 16, 24.
 */
final class Theme {

    private Theme() {
    }

    /**
     * Whether this app is dark.
     *
     * The user's choice wins; "follow the system" is the default rather than the only option,
     * because a machine's screen has a look of its own and someone comparing two renders wants to
     * control it. The window's own theme is set from this too ({@link #apply}), so the platform's
     * widgets — dialogs, the text cursor — agree with the colours chosen here.
     */
    static boolean dark(Activity a) {
        String choice = Prefs.appearance(a);
        if ("dark".equals(choice)) return true;
        if ("light".equals(choice)) return false;
        int mode = a.getResources().getConfiguration().uiMode & Configuration.UI_MODE_NIGHT_MASK;
        return mode == Configuration.UI_MODE_NIGHT_YES;
    }

    /**
     * Sets the window theme to match {@link #dark}.
     *
     * Called first thing in every activity: a light window under dark content gives white dialogs
     * and a white flash on rotation, which is exactly the seam this exists to remove.
     */
    static void apply(Activity a) {
        a.setTheme(dark(a)
                ? android.R.style.Theme_DeviceDefault_NoActionBar
                : android.R.style.Theme_DeviceDefault_Light_NoActionBar);
        // The window's own background, so the first frame before the content is laid out is the
        // page colour rather than the platform's default.
        a.getWindow().setBackgroundDrawable(
                new android.graphics.drawable.ColorDrawable(surface(a)));
    }

    // ---- colour roles ----

    /**
     * The page behind everything.
     *
     * The two neutrals are kept far enough apart to be told apart, and a hairline outline is drawn
     * around every card on top of that. A page and a card at the same brightness is a screen with
     * no edges: the rows float and there is no way to see where one group ends.
     */
    static int surface(Activity a) {
        return dark(a) ? 0xFF0B0B0D : 0xFFEFF0F4;
    }

    /** A card or a row group on the page. */
    static int card(Activity a) {
        return dark(a) ? 0xFF18181C : 0xFFFFFFFF;
    }

    /** A card raised above another card: the hero, a fact tile. */
    static int cardHigh(Activity a) {
        return dark(a) ? 0xFF222227 : 0xFFF7F8FB;
    }

    /** Text and icons. */
    static int onSurface(Activity a) {
        return dark(a) ? 0xFFF2F2F5 : 0xFF121218;
    }

    /** Secondary text: subtitles, section labels, values. Above 4.5:1 in both themes. */
    static int variant(Activity a) {
        return dark(a) ? 0xFFA8A8B3 : 0xFF5B5B66;
    }

    /** The accent choices offered in Settings, in the order they are listed. */
    static final String[] ACCENTS = {"蓝", "紫", "绿", "橙", "红"};

    /**
     * The one accent: the start button, the selected tab, the emphasised row.
     *
     * Five choices rather than a colour wheel: each one is a pair (accent, container) that has been
     * checked against both themes for contrast, and a wheel would produce values that have not.
     */
    static int primary(Activity a) {
        String accent = Prefs.accent(a);
        boolean d = dark(a);
        switch (accent == null ? "蓝" : accent) {
            case "紫": return d ? 0xFFC4A6FF : 0xFF6B3FD4;
            case "绿": return d ? 0xFF7BD8A0 : 0xFF1F7A46;
            case "橙": return d ? 0xFFFFB870 : 0xFFB4560C;
            case "红": return d ? 0xFFFF9A9A : 0xFFB32D2D;
            default:   return d ? 0xFF6EA8FF : 0xFF2B5FD9;
        }
    }

    /** Behind the accent, for the selected tab's indicator and filled states. */
    static int primaryContainer(Activity a) {
        boolean d = dark(a);
        switch (Prefs.accent(a) == null ? "蓝" : Prefs.accent(a)) {
            case "紫": return d ? 0xFF3A2C55 : 0xFFE8DDFF;
            case "绿": return d ? 0xFF1E3A2B : 0xFFD7F2E2;
            case "橙": return d ? 0xFF432C18 : 0xFFFFE6CC;
            case "红": return d ? 0xFF45201F : 0xFFFFDCDC;
            default:   return d ? 0xFF1D3357 : 0xFFDCE6FF;
        }
    }

    /** Text on `primary`. */
    static int onPrimary(Activity a) {
        return dark(a) ? 0xFF06111F : 0xFFFFFFFF;
    }

    /** Hairlines and dividers. */
    static int outline(Activity a) {
        return dark(a) ? 0xFF2C2C33 : 0xFFDCDCE2;
    }

    // ---- spacing, on the 4dp grid ----

    static int gap1(Activity a) { return Ui.dp(a, 4); }
    static int gap2(Activity a) { return Ui.dp(a, 8); }
    static int gap3(Activity a) { return Ui.dp(a, 12); }
    static int gap4(Activity a) { return Ui.dp(a, 16); }
    static int gap6(Activity a) { return Ui.dp(a, 24); }

    /** The minimum size of anything touchable, per the platform's accessibility guidance. */
    static int touch(Activity a) { return Ui.dp(a, 48); }

    /** A navigation bar destination: 80dp tall, as Material 3 specifies. */
    static int navHeight(Activity a) { return Ui.dp(a, 80); }

    // ---- type roles ----

    /** A screen's title: the largest thing on it. */
    static void title(Activity a, android.widget.TextView v) {
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, 24f);
        v.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        v.setTextColor(onSurface(a));
        v.setLetterSpacing(-0.01f);
    }

    /** A section label above a card: small, muted, spaced out. */
    static void sectionLabel(Activity a, android.widget.TextView v) {
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        v.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        v.setTextColor(variant(a));
        v.setLetterSpacing(0.06f);
        v.setAllCaps(true);
    }

    /** A row's name. */
    static void rowTitle(Activity a, android.widget.TextView v) {
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f);
        v.setTextColor(onSurface(a));
    }

    /** A row's explanation, or its current value. */
    static void rowSubtitle(Activity a, android.widget.TextView v) {
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        v.setTextColor(variant(a));
    }

    /** Reading text. */
    static void body(Activity a, android.widget.TextView v) {
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f);
        v.setTextColor(onSurface(a));
        v.setLineSpacing(Ui.dp(a, 3), 1f);
    }

    /** Machine text: monospace, and small — a report is dense on purpose. */
    static void mono(Activity a, android.widget.TextView v, float sp) {
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        v.setTypeface(android.graphics.Typeface.MONOSPACE);
        v.setTextColor(onSurface(a));
    }

    // ---- surfaces ----

    /** Rounded, filled, outlined: the shape every card and button in this app uses. */
    static Drawable cardBackground(Activity a, int color, float radiusDp) {
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(color);
        bg.setCornerRadius(Ui.dp(a, radiusDp));
        bg.setStroke(Math.max(1, Ui.dp(a, 1)), outline(a));
        return bg;
    }

    /** A card's fill without the outline, for something already inside another outlined card. */
    static Drawable fillBackground(Activity a, int color, float radiusDp) {
        GradientDrawable bg = new GradientDrawable();
        bg.setColor(color);
        bg.setCornerRadius(Ui.dp(a, radiusDp));
        return bg;
    }

    /** Paints an activity's window and root the page colour, so edge-to-edge insets look right. */
    static void paint(Activity a, View root) {
        int surface = surface(a);
        root.setBackgroundColor(surface);
        a.getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(surface));
    }

    /** The hairline between two rows of a card, inset so it starts where the text does. */
    static Drawable rowDivider(Activity a) {
        android.graphics.drawable.GradientDrawable line =
                new android.graphics.drawable.GradientDrawable();
        line.setColor(outline(a));
        line.setSize(1, Math.max(1, Math.round(a.getResources().getDisplayMetrics().density)));
        return new android.graphics.drawable.InsetDrawable(line, Ui.dp(a, 16), 0, 0, 0);
    }

    /** A press ripple, in the text colour, so a tap is visible without appearing to move anything. */
    static Drawable ripple(Activity a, int color) {
        GradientDrawable mask = new GradientDrawable();
        mask.setColor(Color.WHITE);
        mask.setCornerRadius(Ui.dp(a, 12));
        return new android.graphics.drawable.RippleDrawable(
                android.content.res.ColorStateList.valueOf(withAlpha(color, 0.12f)), null, mask);
    }

    /** `color` at `alpha`, keeping its RGB. */
    static int withAlpha(int color, float alpha) {
        return (color & 0x00FFFFFF) | (Math.round(255 * alpha) << 24);
    }
}
