package dev.henge.probe;

import android.app.Activity;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.TextView;

import java.io.File;

/**
 * The guest's screen: a full-bleed surface, and controls that get out of the way.
 *
 * **Why a SurfaceView.** It gets its own compositor layer, so a frame the engine draws into that
 * surface reaches the display without a second copy. A TextureView would route every frame through
 * the view hierarchy as a texture, which is a copy per frame a guest running at its own refresh
 * rate cannot afford.
 *
 * **Why so little is here.** Starting, installing, and diagnosing all have a place of their own
 * (the Macs tab, the Install tab, Settings). This screen shows frames and offers the two things
 * that only make sense while looking at them: stop, and leave. Everything else on it used to be a
 * second copy of a screen that already existed.
 *
 * **Why the controls hide.** A Mac's screen is the content. A bar that stays on top of it is a
 * permanent loss of the bottom of the screen, so the bar fades after a few seconds and comes back
 * on a tap — the same rule a video player uses, for the same reason.
 */
public class DisplayActivity extends Activity implements SurfaceHolder.Callback {

    private static final String ROM_NAME = "AVPBooter.vmapple2.bin";
    private static final String DISK_NAME = "disk.img";
    private static final String AUX_NAME = "aux.img";

    private SurfaceView surface;
    private TextView status;
    private View controls;
    /** The sliver left behind when the controls hide: a way back that costs no screen. */
    private View handle;
    private boolean booted;
    private boolean surfaceReady;
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final Runnable hideControls = () -> controls.animate().alpha(0f).setDuration(200)
            .withEndAction(() -> {
                controls.setVisibility(View.GONE);
                // What is left is a two-pixel grip at the bottom edge, not an empty screen with no
                // way back to the controls.
                handle.setVisibility(View.VISIBLE);
                handle.animate().alpha(0.55f).setDuration(200).start();
            }).start();

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // A guest that is running is the one case where the screen going dark is wrong: macOS
        // keeps working and the user loses sight of it.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        FrameLayout root = new FrameLayout(this);
        Ui.inset(root);
        root.setBackgroundColor(0xFF000000);   // the guest's own black, not the page colour

        surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        root.addView(surface, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        status = Ui.mono(this, "", 11f);
        status.setTextColor(0xFFF2F2F5);
        FrameLayout.LayoutParams statusLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        statusLp.gravity = Gravity.TOP | Gravity.START;
        statusLp.setMargins(Ui.dp(this, 16), Ui.dp(this, 16), Ui.dp(this, 16), 0);
        root.addView(status, statusLp);

        controls = buildControls();
        FrameLayout.LayoutParams controlsLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        controlsLp.gravity = Gravity.BOTTOM | Gravity.START;
        controlsLp.setMargins(Ui.dp(this, 12), 0, Ui.dp(this, 12), Ui.dp(this, 12));
        root.addView(controls, controlsLp);

        // The handle is the reliable way back. Tapping the picture also works — but the picture
        // will belong to the guest's pointer soon, so the controls must not depend on it.
        handle = new View(this);
        android.graphics.drawable.GradientDrawable pill = new android.graphics.drawable.GradientDrawable();
        pill.setColor(0xFFF2F2F5);
        pill.setCornerRadius(Ui.dp(this, 2));
        handle.setBackground(pill);
        handle.setAlpha(0.55f);
        handle.setContentDescription("显示控制条");
        handle.setOnClickListener(v -> showControls());
        FrameLayout.LayoutParams handleLp = new FrameLayout.LayoutParams(
                Ui.dp(this, 44), Ui.dp(this, 4));
        handleLp.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
        handleLp.bottomMargin = Ui.dp(this, 10);
        handle.setVisibility(View.GONE);
        root.addView(handle, handleLp);

        surface.setOnClickListener(v -> showControls());

        setContentView(root);
        setStatus("等待 Surface");
    }

    private View buildControls() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setBackground(Theme.cardBackground(this, Theme.card(this), 16f));
        int pad = Ui.dp(this, 6);
        bar.setPadding(pad, pad, pad, pad);

        Ui.rowButton(this, bar, "启动", this::boot);
        Ui.rowButton(this, bar, "停止", this::stop);
        Ui.rowButton(this, bar, "返回", this::finish);
        return bar;
    }

    /** Shows the controls, and starts the timer that takes them away again. */
    private void showControls() {
        ui.removeCallbacks(hideControls);
        handle.setVisibility(View.GONE);
        controls.setVisibility(View.VISIBLE);
        controls.animate().alpha(1f).setDuration(150).start();
        if (booted && Prefs.hideControls(this)) ui.postDelayed(hideControls, 3500);
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        surfaceReady = true;
        AppLog.i("显示页: surfaceCreated " + holder.getSurface().isValid());
        // The surface is the machine's display; without it the engine has nowhere to draw, so a
        // boot started before this point is started now.
        showControls();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        AppLog.i("显示页: surfaceChanged " + width + "x" + height);
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        surfaceReady = false;
        AppLog.i("显示页: surfaceDestroyed");
    }

    @Override
    protected void onResume() {
        super.onResume();
        AppLog.i("显示页: 打开");
        immerse(true);
    }

    @Override
    protected void onPause() {
        super.onPause();
        // The guest keeps running: this is a machine, not a video. Leaving the app is not a stop.
        AppLog.i("显示页: 退到后台（guest 继续运行）");
    }

    /** Hides the system bars, so the guest's screen is the screen. */
    private void immerse(boolean on) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c == null) return;
            if (on) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(
                        WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            } else {
                c.show(WindowInsets.Type.systemBars());
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(on
                    ? View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                    | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                    | View.SYSTEM_UI_FLAG_FULLSCREEN
                    : View.SYSTEM_UI_FLAG_VISIBLE);
        }
    }

    private void setStatus(String text) {
        status.setText(text);
    }

    /**
     * Starts the guest from the three files the engine needs.
     *
     * Whatever happens is shown here and logged, including the ordinary case that a file is not
     * there yet — that is what an uninstalled machine looks like, and saying which file is missing
     * is more useful than a spinner.
     */
    private void boot() {
        if (booted) {
            setStatus("已经在运行。");
            showControls();
            return;
        }
        File dir = new File(getExternalFilesDir(null), "vm");
        File rom = new File(dir, ROM_NAME);
        File disk = new File(dir, DISK_NAME);
        File aux = new File(dir, AUX_NAME);
        if (!rom.exists() || !disk.exists() || !aux.exists()) {
            StringBuilder missing = new StringBuilder();
            if (!rom.exists()) missing.append("\n  ").append(rom);
            if (!disk.exists()) missing.append("\n  ").append(disk);
            if (!aux.exists()) missing.append("\n  ").append(aux);
            String text = "缺少文件（放在 " + dir + "）:" + missing;
            AppLog.i("显示页: " + text.replace('\n', ' '));
            setStatus(text);
            showControls();
            return;
        }

        String result;
        try {
            result = Native.nativeBoot(rom.getAbsolutePath(), disk.getAbsolutePath(),
                    aux.getAbsolutePath(), Prefs.ramMb(this), Prefs.cpus(this),
                    getCacheDir() == null ? null : getCacheDir().getAbsolutePath());
        } catch (Throwable t) {
            result = "引导异常:\n" + android.util.Log.getStackTraceString(t);
            AppLog.w("显示页: 引导异常", t);
        }
        AppLog.i("显示页: boot → " + result.replace('\n', ' '));
        setStatus(result);
        booted = true;
        immerse(true);
        showControls();
    }

    private void stop() {
        if (!booted) {
            setStatus("没有在运行。");
            return;
        }
        try {
            Native.nativeStop();
        } catch (Throwable t) {
            AppLog.w("显示页: 停止异常", t);
        }
        booted = false;
        setStatus("已停止。");
        ui.removeCallbacks(hideControls);
        showControls();
    }

    @Override
    protected void onDestroy() {
        // The guest's threads belong to this screen: leaving it ends them, rather than leaving a
        // machine running with nowhere to draw.
        if (booted) {
            try {
                Native.nativeStop();
            } catch (Throwable ignored) {
            }
        }
        ui.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
