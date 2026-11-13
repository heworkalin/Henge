package dev.henge.probe;

import android.app.Activity;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The list language the screens are built out of: a section header, then a card of rows.
 *
 * This is deliberately the shape Apple's settings and information apps use — a titled group, then
 * rows that carry a value and a disclosure chevron, opening a detail page rather than editing in
 * place. It is not imitation for its own sake: a grouped list puts a setting's name, its current
 * value and the way to change it on one line, so the screen can be read without opening anything,
 * and it scales to the point where a single form does not.
 *
 * Everything is a plain View. There is no RecyclerView here because these lists are short and
 * fixed, and dragging AndroidX in for them would add more machinery than the screens contain.
 */
final class Lists {

    private Lists() {
    }

    /** A titled group of rows, returning the container to add rows to. */
    static LinearLayout group(Activity a, LinearLayout parent, String title) {
        LinearLayout wrap = new LinearLayout(a);
        wrap.setOrientation(LinearLayout.VERTICAL);
        int pad = Ui.dp(a, 16);

        if (title != null && !title.isEmpty()) {
            TextView header = new TextView(a);
            header.setText(title);
            Theme.sectionLabel(a, header);
            header.setPadding(pad, Theme.gap3(a), pad, Theme.gap1(a));
            wrap.addView(header);
        }

        LinearLayout card = new LinearLayout(a);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setBackground(Theme.cardBackground(a, Theme.card(a), 14f));
        card.setClipToOutline(true);
        // The hairline between rows is the card's own, so a screen cannot forget one and end up
        // with three rows that read as one paragraph.
        card.setShowDividers(LinearLayout.SHOW_DIVIDER_MIDDLE);
        card.setDividerDrawable(Theme.rowDivider(a));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        lp.setMargins(pad, 0, pad, 0);
        wrap.addView(card, lp);

        parent.addView(wrap, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return card;
    }

    /**
     * One row: a title, an optional subtitle, an optional value on the right, and a disclosure
     * chevron when it opens something.
     */
    static View row(Activity a, LinearLayout card, String title, String subtitle, String value,
                    boolean disclosing, Runnable onClick) {
        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        int pad = Ui.dp(a, 16);
        row.setPadding(pad, Ui.dp(a, 12), pad, Ui.dp(a, 12));
        row.setMinimumHeight(Theme.touch(a));
        // Rows are targets: a touch anywhere on the line opens what it names, and the press has to
        // be visible — a list that does not react reads as a list that did not hear you.
        row.setClickable(onClick != null);
        row.setFocusable(onClick != null);
        if (onClick != null) {
            row.setOnClickListener(v -> onClick.run());
            row.setBackground(Theme.ripple(a, Theme.onSurface(a)));
        }

        LinearLayout text = new LinearLayout(a);
        text.setOrientation(LinearLayout.VERTICAL);

        TextView name = new TextView(a);
        name.setText(title);
        Theme.rowTitle(a, name);
        text.addView(name);

        if (subtitle != null && !subtitle.isEmpty()) {
            TextView sub = new TextView(a);
            sub.setText(subtitle);
            Theme.rowSubtitle(a, sub);
            sub.setPadding(0, Ui.dp(a, 2), 0, 0);
            text.addView(sub);
        }
        row.addView(text, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        if (value != null && !value.isEmpty()) {
            TextView v = new TextView(a);
            v.setText(value);
            Theme.rowSubtitle(a, v);
            v.setPadding(Ui.dp(a, 8), 0, 0, 0);
            row.addView(v);
        }

        if (disclosing) {
            TextView chevron = new TextView(a);
            chevron.setText("\u203A");   // ›
            chevron.setTextSize(20f);
            chevron.setTextColor(Theme.variant(a));
            chevron.setPadding(Ui.dp(a, 8), 0, 0, 0);
            row.addView(chevron);
        }

        card.addView(row, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return row;
    }

    /**
     * A row whose right-hand text the caller keeps a handle on.
     *
     * `row` above decides for itself whether there is a value at all; a row whose value changes
     * after it is built — a selection mark, a download's state — needs the view itself, and must
     * not go looking for it among the row's children, where the chevron also lives.
     */
    static final class LiveRow {
        final LinearLayout view;
        final TextView value;

        LiveRow(LinearLayout view, TextView value) {
            this.view = view;
            this.value = value;
        }
    }

    static LiveRow liveRow(Activity a, LinearLayout card, String title, String subtitle,
                           String value, boolean disclosing, Runnable onClick) {
        LinearLayout row = new LinearLayout(a);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        int pad = Ui.dp(a, 16);
        row.setPadding(pad, Ui.dp(a, 12), pad, Ui.dp(a, 12));
        row.setMinimumHeight(Theme.touch(a));
        row.setClickable(onClick != null);
        row.setFocusable(onClick != null);
        if (onClick != null) {
            row.setOnClickListener(v -> onClick.run());
            row.setBackground(Theme.ripple(a, Theme.onSurface(a)));
        }

        LinearLayout text = new LinearLayout(a);
        text.setOrientation(LinearLayout.VERTICAL);
        TextView name = new TextView(a);
        name.setText(title);
        Theme.rowTitle(a, name);
        text.addView(name);
        if (subtitle != null && !subtitle.isEmpty()) {
            TextView sub = new TextView(a);
            sub.setText(subtitle);
            Theme.rowSubtitle(a, sub);
            sub.setPadding(0, Ui.dp(a, 2), 0, 0);
            text.addView(sub);
        }
        row.addView(text, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        TextView mark = new TextView(a);
        mark.setText(value == null ? "" : value);
        mark.setTextSize(14f);
        mark.setAlpha(0.7f);
        mark.setPadding(Ui.dp(a, 8), 0, 0, 0);
        row.addView(mark);

        if (disclosing) {
            TextView chevron = new TextView(a);
            chevron.setText("\u203A");
            chevron.setTextSize(20f);
            chevron.setTextColor(Theme.variant(a));
            chevron.setPadding(Ui.dp(a, 8), 0, 0, 0);
            row.addView(chevron);
        }

        card.addView(row, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        return new LiveRow(row, mark);
    }

    /**
     * Kept for call sites written before cards drew their own hairlines; does nothing now.
     *
     * A manual separator plus the card's automatic one is two lines a pixel apart, which reads as
     * a mistake. Removing the call sites is a wider change than leaving this here, and this reads
     * as what it is.
     */
    @Deprecated
    static void divider(Activity a, LinearLayout card) {
    }

}
