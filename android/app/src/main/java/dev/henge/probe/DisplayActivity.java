package dev.henge.probe;

import android.app.Activity;
import android.graphics.Bitmap;
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
import android.widget.ImageView;
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
    /* The firmware's own framebuffer, which is the only thing there is to look at before macOS
       programs the GPU. A SurfaceView's surface sits *behind* the window's content, so an opaque
       ImageView covers it; when the GPU takes over, the ImageView goes away and the surface shows
       through. That is the whole "first display buffer" the app was missing. */
    private ImageView fbView;
    private Bitmap fbBitmap;
    private int[] fbPixels;
    private final int[] fbMeta = new int[3];
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

        fbView = new ImageView(this);
        fbView.setBackgroundColor(0xff000000);
        fbView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        fbView.setVisibility(View.GONE);
        root.addView(fbView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

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

        // The guest's screen is not a tap target: a tap on it belongs to the guest (once pointer
        // input is wired up, it will). Only the pill at the bottom edge brings the controls back,
        // which is also why the pill is always there while they are hidden.
        setContentView(root);
        setStatus("等待 Surface");
    }

    /**
     * The bar, built for the machine's current state.
     *
     * Two buttons, not three: 启动 only exists when there is nothing running, 停止 only when there
     * is. A bar that always shows both leaves one of them permanently dead, which reads as a bug
     * rather than as a state. 返回 is "put this away", not "end the machine".
     */
    private View buildControls() {
        LinearLayout bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setBackground(Theme.cardBackground(this, Theme.card(this), 16f));
        int pad = Ui.dp(this, 6);
        bar.setPadding(pad, pad, pad, pad);

        if (booted) Ui.rowButton(this, bar, "停止", this::stop);
        else Ui.rowButton(this, bar, "启动", this::boot);
        Ui.rowButton(this, bar, "返回", () -> moveTaskToBack(true));
        return bar;
    }

    /** Swaps the bar for one that matches the state, without leaving the screen. */
    private void rebuildControls() {
        if (controls instanceof LinearLayout && ((LinearLayout) controls).getParent() instanceof FrameLayout) {
            FrameLayout parent = (FrameLayout) ((LinearLayout) controls).getParent();
            int index = parent.indexOfChild(controls);
            FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) controls.getLayoutParams();
            parent.removeView(controls);
            controls = buildControls();
            parent.addView(controls, index, lp);
        }
    }

    /**
     * The firmware's screen, until the GPU has one.
     *
     * Before macOS programs the display, the guest draws its progress bar into a framebuffer in
     * RAM and nowhere else; nothing arrives at the window. Reading that buffer is what makes the
     * first half of a boot visible at all.
     */
    private final Runnable pollBoot = new Runnable() {
        @Override
        public void run() {
            if (!booted) return;
            if (Native.nativeDisplayReady()) {
                // The GPU owns the screen now: drop the copy, let the surface show through.
                AppLog.i("显示页: GPU 接管画面");
                fbView.setVisibility(View.GONE);
                fbBitmap = null;
                return;
            }
            if (fbPixels == null) fbPixels = new int[2048 * 1280];
            if (Native.nativeBootFramebuffer(fbPixels, fbMeta)) {
                int w = fbMeta[0], h = fbMeta[1];
                if (w > 0 && h > 0) {
                    if (fbBitmap == null || fbBitmap.getWidth() != w || fbBitmap.getHeight() != h)
                        fbBitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
                    fbBitmap.setPixels(fbPixels, 0, w, 0, 0, w, h);
                    fbView.setImageBitmap(fbBitmap);
                    fbView.setVisibility(View.VISIBLE);
                }
            }
            ui.postDelayed(this, 250);
        }
    };

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
        attachSurface(holder);
        surfaceReady = true;
        AppLog.i("显示页: surfaceCreated " + holder.getSurface().isValid());
        // A machine started before this screen existed shows up through this window as it is; it
        // must not be booted a second time just because the activity was recreated.
        if (!booted && VmHost.canReopen()) {
            booted = true;
            setStatus("运行中");
            rebuildControls();
        }
        // The surface is the machine's display; without it the engine has nowhere to draw, so a
        // boot started before this point is started now.
        showControls();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        attachSurface(holder);
        AppLog.i("显示页: surfaceChanged " + width + "x" + height);
    }

    /* The engine draws through this window; a surface that was recreated must be handed over
       again, or frames go wherever the previous one was. */
    private void attachSurface(SurfaceHolder holder) {
        try {
            android.view.Surface s = holder.getSurface();
            if (s != null && s.isValid()) {
                android.graphics.Rect r = holder.getSurfaceFrame();
                Native.nativeAttachWindow(s);
                AppLog.i("显示页: surface 已交给引擎 " + r.width() + "x" + r.height());
            }
        } catch (Throwable t) {
            AppLog.w("显示页: 交出 surface 失败", t);
        }
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
        // The machine may have been started earlier by this process; a second boot would build a
        // second machine over the same disk image.
        if (VmHost.canReopen()) {
            booted = true;
            VmHost.setRunning(true);
            setStatus("运行中（画面已接回）");
            rebuildControls();
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

        // The boot report is a log line, not a screen: it names files, RAM size and CPU count,
        // which is what an engineer wants and what a person starting a Mac should never see. The
        // screen says whether it started; the report goes to the log.
        setStatus("正在启动…");
        ui.post(() -> {
            String result;
            try {
                result = Native.nativeBoot(rom.getAbsolutePath(), disk.getAbsolutePath(),
                        aux.getAbsolutePath(), Prefs.ramMb(this), Prefs.cpus(this),
                        getCacheDir() == null ? null : getCacheDir().getAbsolutePath());
            } catch (Throwable t) {
                result = "引导异常: " + android.util.Log.getStackTraceString(t);
                AppLog.w("显示页: 引导异常", t);
            }
            for (String line : result.split("\n")) {
                if (!line.trim().isEmpty()) AppLog.i("显示页: boot | " + line.trim());
            }
            if (Native.nativeIsUp()) {
                booted = true;
                VmHost.setRunning(true);
                setStatus("运行中");
                immerse(true);
                rebuildControls();
                showControls();
                ui.removeCallbacks(pollBoot);
                ui.post(pollBoot);
            } else {
                setStatus("启动失败");
                AppLog.w("显示页: 启动失败（原因见上面的 boot 报告）", null);
                rebuildControls();
                showControls();
            }
        });
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
        VmHost.setRunning(false);
        ui.removeCallbacks(pollBoot);
        fbView.setVisibility(View.GONE);
        fbBitmap = null;
        setStatus("已停止。");
        rebuildControls();
        ui.removeCallbacks(hideControls);
        showControls();
    }

    /**
     * Back leaves the machine running.
     *
     * A window onto a Mac is not the Mac: closing it used to join every CPU thread on the way out,
     * which left the process with nothing to keep it alive, so Android reclaimed it and a back
     * press looked like the app dying. The machine now outlives the screen; the 停止 button is the
     * only thing that powers it off.
     */
    @Override
    public void onBackPressed() {
        AppLog.i("显示页: 返回（guest 继续运行）");
        moveTaskToBack(true);
    }

    @Override
    protected void onDestroy() {
        // The surface is going away with this activity, and the engine must not present into a dead
        // window. Stop only if the machine cannot survive without one -- i.e. only when this screen
        // is closing for good with a machine that is still up.
        if (booted && VmHost.isRunning()) {
            AppLog.w("显示页: 画面销毁但 guest 仍在运行（引擎按进程存活）", null);
        }
        ui.removeCallbacks(pollBoot);
        ui.removeCallbacksAndMessages(null);
        super.onDestroy();
    }
}
