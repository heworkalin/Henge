package dev.henge.probe;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.os.Bundle;
import android.text.Editable;
import android.text.TextWatcher;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.util.List;

/**
 * The log, as a console: a filter on top, the lines below, newest at the bottom.
 *
 * This follows the shape of a console application rather than a document — search, a level floor,
 * and a stream — because that is what a log is used for: narrowing to the moment something went
 * wrong. Reading it top to bottom is the rare case, and a filter makes that cheaper instead of
 * more expensive.
 *
 * Two things are not here on purpose. Colours per level are a single alpha shuffle, not a theme.
 * And the log is fed by {@link AppLog}, not by the system: Android gives an app no access to
 * another process's log, so a screen claiming to show "system logs" would be showing a subset.
 */
public class LogsActivity extends Activity implements AppLog.Listener {

    private TextView view;
    private ScrollView scroll;
    private EditText search;
    private TextView levelButton;

    /** The lowest level shown: DEBUG by default, i.e. everything the app writes. */
    private int floor = Log.DEBUG;
    private String query = "";

    private static final int[] LEVELS = {Log.DEBUG, Log.INFO, Log.WARN, Log.ERROR};
    private static final String[] LEVEL_NAMES = {"全部", "信息+", "警告+", "仅错误"};

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // Window theme and window background first: everything below draws on top.
        Theme.apply(this);

        LinearLayout root = Ui.column(this);
        root.addView(Ui.title(this, "日志"));

        LinearLayout filters = new LinearLayout(this);
        filters.setOrientation(LinearLayout.HORIZONTAL);
        int pad = Ui.dp(this, 16);
        filters.setPadding(pad, 0, pad, Ui.dp(this, 4));

        search = new EditText(this);
        search.setHint("搜索");
        search.setSingleLine(true);
        search.setTextSize(14f);
        search.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) { }
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) { }
            @Override public void afterTextChanged(Editable s) {
                query = s.toString().trim();
                render();
            }
        });
        filters.addView(search, new LinearLayout.LayoutParams(0,
                ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        levelButton = new TextView(this);
        levelButton.setTextSize(13f);
        levelButton.setPadding(Ui.dp(this, 12), Ui.dp(this, 12), Ui.dp(this, 12), Ui.dp(this, 12));
        levelButton.setOnClickListener(v -> cycleLevel());
        filters.addView(levelButton);
        root.addView(filters);

        view = Ui.mono(this, "", 10f);
        scroll = new ScrollView(this);
        scroll.addView(view, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        LinearLayout buttons = Ui.row(this);
        Ui.rowButton(this, buttons, "复制", this::copy);
        Ui.rowButton(this, buttons, "清空", () -> {
            AppLog.clear();
            render();
        });
        root.addView(buttons);

        setContentView(root);
        cycleLevelText();
        render();
    }

    private void cycleLevel() {
        int at = 0;
        for (int i = 0; i < LEVELS.length; i++) if (LEVELS[i] == floor) at = i;
        floor = LEVELS[(at + 1) % LEVELS.length];
        cycleLevelText();
        render();
    }

    private void cycleLevelText() {
        int at = 0;
        for (int i = 0; i < LEVELS.length; i++) if (LEVELS[i] == floor) at = i;
        levelButton.setText(LEVEL_NAMES[at]);
        levelButton.setAlpha(floor == Log.DEBUG ? 0.6f : 1f);
    }

    private void copy() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        cm.setPrimaryClip(ClipData.newPlainText("Henge log", visibleText()));
        Toast.makeText(this, "已复制筛选后的日志", Toast.LENGTH_SHORT).show();
    }

    /** The lines that pass the filter, as text: what the screen shows is what gets copied. */
    private String visibleText() {
        StringBuilder sb = new StringBuilder();
        for (AppLog.Entry e : AppLog.snapshot()) {
            if (e.level < floor) continue;
            String line = e.format();
            if (!query.isEmpty() && !line.toLowerCase().contains(query.toLowerCase())) continue;
            sb.append(line).append('\n');
        }
        return sb.toString();
    }

    private void render() {
        view.setText(visibleText());
        scroll.post(() -> scroll.fullScroll(ScrollView.FOCUS_DOWN));
    }

    @Override
    protected void onResume() {
        super.onResume();
        AppLog.addListener(this);
        AppLog.i("日志页: 打开");
        render();
    }

    @Override
    protected void onPause() {
        AppLog.removeListener(this);
        super.onPause();
    }

    @Override
    public void onLine(AppLog.Entry entry) {
        runOnUiThread(this::render);
    }
}
