package dev.henge.probe;

import android.app.Activity;
import android.graphics.Bitmap;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.MotionEvent;
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
 * **Why the controls are a ball.** A Mac's screen is the content. A bar that stays on it is a
 * permanent loss of the screen — worse in landscape, where it covers the guest's own menu bar — so
 * the controls are a small ball on the nearer side edge ([`InputBall`]), whose icons appear on a
 * tap and hide again after a few seconds: the same rule a video player uses, for the same reason.
 */
public class DisplayActivity extends Activity implements SurfaceHolder.Callback {

    /** Set when the caller already asked for the machine to start (the Macs page's 启动). */
    public static final String EXTRA_BOOT = "boot";

    /**
     * How long 停止 waits for the engine before the app ends itself.
     *
     * 停止 must not wait synchronously on the screen: the join it makes can wait on a guest thread
     * that is itself waiting on a present, and a screen that waits there is an "isn't responding"
     * dialog rather than a machine being switched off. So the wait runs on a thread of its own, the
     * screen says what it is doing, and 15 seconds is the point at which the guest is declared
     * unable to stop: the process then ends itself, no matter what it was doing, and Android is
     * told the app is finishing. Up to that point nothing is told to the system, so a stop that
     * takes eight seconds is an ordinary stop.
     */
    private static final long STOP_TIMEOUT_MS = 15_000;

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
    private boolean fbLastOk;
    /* The surface outlives the moment the machine starts: the window arrives when the activity is
       created, which is before 启动 is pressed, so it must be handed over again once there is a
       machine to hand it to. */
    private SurfaceHolder pendingHolder;
    private TextView status;
    /** The controls: a ball on the edge of the guest's screen, whose icons appear on a tap. */
    private InputBall ball;
    private boolean booted;
    /* A boot takes seconds; without this, a second tap started a second machine. */
    private boolean booting;
    /** 停止 was pressed and the engine has not answered yet ({@link #stop}). */
    private boolean stopping;
    /** A boot requested before the surface existed; started as soon as there is one. */
    private boolean bootWhenReady;
    /** The person's fingers, mouse and keys, as the guest's own USB devices see them. */
    private VmInput input;
    /** The on-screen control keys (Esc, ⌘, arrows, …), shown by 键盘. */
    private View keys;
    /** A one-pixel field that exists to hold the input method's focus, so the IME can type. */
    private android.widget.EditText imeField;
    private boolean imeWriting;
    private boolean surfaceReady;
    private final Handler ui = new Handler(Looper.getMainLooper());
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // A guest that is running is the one case where the screen going dark is wrong: macOS
        // keeps working and the user loses sight of it.
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        /* Full screen, edges included.
         *
         * The screen is the guest's screen: a root padded away from the system bars and the cutout
         * leaves strips at the edges that belong to the system, and a pointer over one of them is
         * outside this window -- which is exactly where the capture was dropped and the system's own
         * cursor came back (measured: captured in the middle, the system's arrow at both edges). So
         * the layout covers the whole display and the cutout is laid out into. */
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            getWindow().getAttributes().layoutInDisplayCutoutMode =
                    WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                getWindow().getAttributes().layoutInDisplayCutoutMode =
                        WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
            }
        }
        getWindow().setDecorFitsSystemWindows(false);
        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(0xFF000000);   // the guest's own black, not the page colour

        surface = new SurfaceView(this);
        surface.getHolder().addCallback(this);
        /* The guest's keyboard and trackpad (devices/xhci.c); this is the only way a finger, a
           mouse or a key gets in. The hold callback tells the status line what a long press did. */
        input = new VmInput(surface, null);
        surface.setOnTouchListener((v, e) -> input.onMotion(e));
        /* A mouse attached to the phone is not a finger: its hover moves and its buttons arrive as
           generic motion, and it needs the pointer captured to give deltas rather than a system
           cursor. Without this the app ignored it entirely (measured: a Bluetooth mouse moved the
           system cursor over the guest's screen and nothing else). */
        surface.setOnGenericMotionListener((v, e) -> input.onMotion(e));
        surface.setOnHoverListener((v, e) -> input.onMotion(e));
        surface.setOnCapturedPointerListener((v, e) -> input.onMotion(e));
        surface.setFocusable(true);
        surface.setFocusableInTouchMode(true);
        /* The system cursor is not the guest's cursor: capture the pointer so the mouse's movement
           comes here as deltas, and hide the system arrow while it is over the guest's screen. */
        surface.setPointerIcon(android.view.PointerIcon.getSystemIcon(this,
                android.view.PointerIcon.TYPE_NULL));
        /* The capture is asked of the *decor view*, not of the SurfaceView: a captured pointer's
           events go to the view that holds the capture, and the decor view holds the whole window,
           so nothing the mouse does can fall between views. */
        getWindow().getDecorView().setOnCapturedPointerListener((v, e) -> {
            if (e.getActionMasked() == ACTION_CAPTURE_LOST) {
                /* The system takes the capture back when the pointer leaves this window -- over the
                   ball's own little window, or an edge the system reserves -- and then the system
                   cursor comes back and nothing is delivered (measured: the cursor was captured in
                   the middle and the system's own at both edges). Take it again at once. */
                captureHeldSince = 0;
                captureLost++;
                AppLog.i("显示页: 指针捕获被收回（第 " + captureLost + " 次），重新申请");
                ui.postDelayed(capturePointer, 120);
                return true;
            }
            noteMouse("captured", e);
            return input != null && input.onMotion(e);
        });
        ui.postDelayed(capturePointer, 500);
        root.addView(surface, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        /* After the SurfaceView, not before.
         *
         * A SurfaceView punches a hole in the window's content and shows its own surface through
         * it; anything added *before* it in the same FrameLayout sits below that hole and is covered
         * by it. Added first, this ImageView received a frame every 250 ms and was never once
         * visible -- which is what "the boot framebuffer is read but nothing is displayed" looked
         * like from outside.
         */
        fbView = new ImageView(this);
        fbView.setBackgroundColor(0xff000000);
        fbView.setScaleType(ImageView.ScaleType.FIT_CENTER);
        fbView.setVisibility(View.GONE);
        root.addView(fbView, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));

        status = Ui.mono(this, "", 11f);
        status.setTextColor(0xFFF2F2F5);
        FrameLayout.LayoutParams statusLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        statusLp.gravity = Gravity.TOP | Gravity.START;
        /* Top left, where a Mac's own notifications are not, and clear of the ball's edge strip. */
        statusLp.setMargins(Ui.dp(this, 16), Ui.dp(this, 14), Ui.dp(this, 16), 0);
        root.addView(status, statusLp);

        keys = buildKeys();
        FrameLayout.LayoutParams keysLp = new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        /* The control keys are a keyboard: they belong at the bottom edge, where thumbs are, and
           they are only there while 键盘 is on. */
        keysLp.gravity = Gravity.BOTTOM | Gravity.CENTER_HORIZONTAL;
        keysLp.setMargins(Ui.dp(this, 12), 0, Ui.dp(this, 12), Ui.dp(this, 14));
        keys.setVisibility(View.GONE);
        root.addView(keys, keysLp);

        /*
         * The field the input method types into. It has to be a real, focusable, laid-out field: a
         * zero-alpha view is allowed, a view the input method cannot focus is not, and the first
         * version of this was never focused at all, so 输入法 did nothing (measured).
         */
        imeField = new android.widget.EditText(this);
        imeField.setBackground(null);
        imeField.setTextColor(0x00000000);
        imeField.setCursorVisible(false);
        imeField.setAlpha(0.02f);          // not 0: some input methods skip an invisible field
        imeField.setFocusable(true);
        imeField.setFocusableInTouchMode(true);
        imeField.setImportantForAutofill(View.IMPORTANT_FOR_AUTOFILL_NO);
        imeField.setInputType(android.text.InputType.TYPE_CLASS_TEXT
                | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
                | android.text.InputType.TYPE_TEXT_FLAG_MULTI_LINE);
        imeField.addTextChangedListener(new android.text.TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) { }
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) { }
            @Override public void afterTextChanged(android.text.Editable text) { typeFromIme(text); }
        });
        FrameLayout.LayoutParams imeLp = new FrameLayout.LayoutParams(
                Ui.dp(this, 2), Ui.dp(this, 2));
        imeLp.gravity = Gravity.TOP | Gravity.START;
        root.addView(imeField, imeLp);

        ball = new InputBall(this, root, new InputBall.Actions() {
            @Override public void primary() { if (stopping) return; if (booted) stop(); else boot(); }
            @Override public void mode() { toggleMode(); }
            @Override public void keys() { toggleKeys(); }
            @Override public void ime() { showIme(); }
            @Override public void back() { moveTaskToBack(true); }
            @Override public boolean running() { return booted && !stopping; }
            @Override public boolean touchMode() { return input != null && input.mode() == VmInput.Mode.TOUCH; }
        });

        bootWhenReady = getIntent() != null && getIntent().getBooleanExtra(EXTRA_BOOT, false);

        // The guest's screen is not a tap target: a tap on it belongs to the guest. The ball on the
        // edge is the way to the controls, and it is always there.
        setContentView(root);
    }

    /**
     * The bar, built for the machine's current state.
     *
     * Two buttons, not three: 启动 only exists when there is nothing running, 停止 only when there
     * is. A bar that always shows both leaves one of them permanently dead, which reads as a bug
     * rather than as a state. 返回 is "put this away", not "end the machine".
     */
    /** The mode's own colour: warm for 触控 (a finger on the glass), cool for 鼠标 (a trackpad). */
    private int modeColor() {
        boolean touch = input != null && input.mode() == VmInput.Mode.TOUCH;
        return touch ? 0xFFB4560C : 0xFF2B5FD9;
    }

    /** 触控 ⇄ 鼠标: the same finger, twice the meaning. */
    private void toggleMode() {
        VmInput.Mode next = input != null && input.mode() == VmInput.Mode.TOUCH
                ? VmInput.Mode.MOUSE : VmInput.Mode.TOUCH;
        if (input != null) input.setMode(next);
        if (ball != null) ball.refresh();
    }

    /**
     * The control keys, for a phone with nothing plugged into it.
     *
     * Text is not here on purpose: typing goes through the input method (输入法), because the
     * guest's own input method is what turns letters into Chinese, Japanese or emoji, and no
     * keyboard of ours could do that for it. These are the keys an input method cannot send.
     */
    private View buildKeys() {
        android.widget.HorizontalScrollView scroll = new android.widget.HorizontalScrollView(this);
        scroll.setHorizontalScrollBarEnabled(false);
        scroll.setBackground(Theme.cardBackground(this, Theme.card(this), 16f));
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        int pad = Ui.dp(this, 6);
        row.setPadding(pad, pad, pad, pad);

        String[][] keys = {
                {"esc", "esc"}, {"tab", "tab"}, {"⌘", "cmd"}, {"⌃", "ctrl"}, {"⌥", "opt"},
                {"⇧", "shift"}, {"↵", "enter"}, {"⌫", "back"}, {"←", "left"}, {"↑", "up"},
                {"↓", "down"}, {"→", "right"},
        };
        for (String[] key : keys) {
            android.widget.TextView b = Ui.mono(this, key[0], 15f);
            b.setTextColor(Theme.onSurface(this));
            b.setGravity(Gravity.CENTER);
            int px = Ui.dp(this, 14);
            b.setPadding(px, Ui.dp(this, 8), px, Ui.dp(this, 8));
            b.setBackground(Theme.cardBackground(this, Theme.cardHigh(this), 10f));
            b.setOnTouchListener((v, e) -> {
                switch (e.getActionMasked()) {
                    case MotionEvent.ACTION_DOWN:
                        if (input != null) input.control(key[1], true);
                        return true;
                    case MotionEvent.ACTION_UP:
                    case MotionEvent.ACTION_CANCEL:
                        if (input != null) input.control(key[1], false);
                        return true;
                    default:
                        return true;
                }
            });
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            lp.setMarginEnd(Ui.dp(this, 6));
            row.addView(b, lp);
        }
        scroll.addView(row);
        return scroll;
    }

    /** 键盘: the control keys in and out of view. */
    private void toggleKeys() {
        boolean show = keys.getVisibility() != View.VISIBLE;
        keys.setVisibility(show ? View.VISIBLE : View.GONE);
        if (ball != null) ball.refresh();
        if (show) showControls();
    }

    /**
     * 输入法: the phone's own keyboard, for text.
     *
     * Its characters are typed on the guest's keyboard one key at a time ({@link VmInput#type}),
     * so the guest's input method composes them as if they had been typed there: a pinyin input
     * method in the guest is what makes Chinese, and this side only has to deliver the letters.
     */
    private void showIme() {
        if (imeField == null) return;
        /* An input method opens for a window that is focused and a view that is focused, and the tap
           that got here came from another window (the ball's popup), which can take the focus with
           it. So: take the focus back, tell the window to keep its own layout, and ask again once
           the window has settled — the first request often lands while the window is still not
           focused and is dropped without a word. */
        getWindow().setSoftInputMode(android.view.WindowManager.LayoutParams.SOFT_INPUT_ADJUST_NOTHING
                | android.view.WindowManager.LayoutParams.SOFT_INPUT_STATE_ALWAYS_VISIBLE);
        imeField.setFocusableInTouchMode(true);
        imeField.requestFocus();
        final android.view.inputmethod.InputMethodManager imm =
                (android.view.inputmethod.InputMethodManager) getSystemService(INPUT_METHOD_SERVICE);
        if (imm == null) return;
        imeField.post(() -> imm.showSoftInput(imeField, 0));
        imeField.postDelayed(() -> {
            if (!imm.isActive(imeField)) {
                imm.toggleSoftInput(android.view.inputmethod.InputMethodManager.SHOW_FORCED, 0);
            }
        }, 250);
    }

    /** The text an input method produced, as key presses on the guest's keyboard. */
    private void typeFromIme(android.text.Editable text) {
        if (imeWriting || input == null) return;
        String value = text.toString();
        if (value.isEmpty()) return;
        imeWriting = true;
        try {
            for (int i = 0; i < value.length(); i++) input.type(value.charAt(i));
            text.clear();
        } finally {
            imeWriting = false;
        }
    }

    /**
     * The firmware's screen, until the GPU has one.
     *
     * Before macOS programs the display, the guest draws its progress bar into a framebuffer in
     * RAM and nowhere else; nothing arrives at the window. Reading that buffer is what makes the
     * first half of a boot visible at all.
     */
    private final int[] renderStats = new int[3];
    private final long[] cpuSnapshot = new long[3 * 8];
    private long[] lastPc = new long[8];
    private int ticks;

    private final Runnable pollBoot = new Runnable() {
        @Override
        public void run() {
            if (!booted) return;
            /* Every four polls (a second), one line saying where the render path is. Without it a
               screen with no pixels reports nothing at all, and "no logs" was being read as "the
               code did not run" when it meant "the code ran and had nothing to say". */
            if (++ticks % 4 == 0) {
                Native.nativeRenderStats(renderStats);
                AppLog.i("显示页: 状态"
                        + " | 帧缓冲=" + (fbLastOk ? fbMeta[0] + "x" + fbMeta[1] : "无")
                        + " | 模式=" + renderStats[0]
                        + " | guest 呈现=" + renderStats[1]
                        + " | host 呈现=" + renderStats[2]);
            }
            /* Every five seconds, which CPUs moved. A guest that is working has pcs that change;
               a guest that stopped has one that does not, and the value names where it stopped.
               Without this, "nothing on screen" and "nothing running" looked the same. */
            if (ticks % 20 == 0) {
                int n = Native.nativeCpuSnapshot(cpuSnapshot);
                StringBuilder moved = new StringBuilder();
                for (int i = 0; i < n && i < lastPc.length; i++) {
                    long pc = cpuSnapshot[i * 3];
                    boolean changed = pc != lastPc[i];
                    lastPc[i] = pc;
                    if (changed) moved.append(i).append(' ');
                }
                StringBuilder where = new StringBuilder();
                for (int i = 0; i < n && i < lastPc.length; i++)
                    where.append(i).append("=0x").append(Long.toHexString(cpuSnapshot[i * 3]))
                         .append(cpuSnapshot[i * 3 + 2] != 0 ? "(halt) " : " ");
                AppLog.i("显示页: cpu 5s 内变化=" + (moved.length() == 0 ? "无（全部停住）" : moved.toString().trim())
                        + " | " + where.toString().trim());
            }
            if (Native.nativeDisplayReady()) {
                // The GPU owns the screen now: drop the copy, let the surface show through.
                AppLog.i("显示页: GPU 接管画面");
                fbView.setVisibility(View.GONE);
                fbBitmap = null;
                return;
            }
            if (fbPixels == null) fbPixels = new int[2048 * 1280];
            boolean have = Native.nativeBootFramebuffer(fbPixels, fbMeta);
            if (have != fbLastOk) {
                // The first display buffer is the only thing visible before macOS programs the GPU;
                // its absence is the difference between a black screen and a progress bar, so which
                // of the two it is gets a line in the log.
                AppLog.i(have ? "显示页: 帧缓冲 " + fbMeta[0] + "x" + fbMeta[1]
                              : "显示页: 帧缓冲尚未发布（固件还没写它）");
                fbLastOk = have;
            }
            if (have) {
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

    /**
     * Takes the mouse pointer, and keeps asking until it is taken.
     *
     * A captured pointer is the only way a mouse is usable over a guest's screen: uncaptured, the
     * system moves its own cursor and hands us hover positions, which the guest reads as a finger
     * jumping around; captured, the system's cursor stands still and the mouse's movement arrives as
     * deltas, which is what the guest's trackpad wants. The request is refused while the view is not
     * focused (and silently, like most Android refusals), so this repeats until it is granted.
     */
    private final Runnable capturePointer = new Runnable() {
        @Override
        public void run() {
            /* A capture that produces no events is worse than none: while it is held the system
               stops sending hover, so if the captured stream is empty there is no mouse at all
               (measured: "鼠标捕获 已获得", then not one event). Two seconds with nothing means the
               capture is not being delivered here — let it go and fall back to hover, which does
               arrive. */
            if (captureHeldSince != 0 && mouseEvents == mouseEventsAtCapture
                    && System.currentTimeMillis() - captureHeldSince > 2000) {
                try {
                    getWindow().getDecorView().releasePointerCapture();
                    AppLog.w("显示页: 捕获期间没有事件，已释放捕获，改用 hover", null);
                } catch (Throwable t) {
                    AppLog.w("显示页: 释放捕获失败", t);
                }
                captureHeldSince = 0;
                noCapture = true;
            }
            if (surfaceReady && booted && !isFinishing()) {
                android.view.View decor = getWindow().getDecorView();
                try {
                    if (!decor.hasPointerCapture() && !noCapture) {
                        /* Capture needs a focused view in a focused window, and the ball's own window
                           can take that focus away (it is touchable, and a mouse parked over it gives
                           it hover). So the decor view is made focusable and asked to take the focus
                           back before the request: without this the request was refused, silently,
                           every 500 ms (measured: "请求捕获鼠标（decor）" repeated, and the system
                           cursor kept moving while the guest's did not). */
                        decor.setFocusableInTouchMode(true);
                        if (!decor.hasFocus()) decor.requestFocus();
                        if (decor.hasWindowFocus()) {
                            /* Both views ask: the decor holds the whole window, the SurfaceView is
                               the view the mouse is actually over, and which one the system accepts
                               differs by build. The first that is granted wins; the rest are then
                               skipped by the hasPointerCapture check. */
                            decor.requestPointerCapture();
                            if (!decor.hasPointerCapture()) surface.requestPointerCapture();
                        }
                    }
                } catch (Throwable t) {
                    AppLog.w("显示页: 请求捕获鼠标失败", t);
                }
                if (captureTries++ % 20 == 0) {
                    AppLog.i("显示页: 鼠标捕获 " + (decor.hasPointerCapture() ? "已获得" : "未获得")
                            + " (attached=" + decor.isAttachedToWindow()
                            + " windowFocus=" + decor.hasWindowFocus()
                            + " focus=" + decor.hasFocus()
                            + " focusable=" + decor.isFocusable() + ")");
                }
                if (decor.hasPointerCapture()) {
                    if (captureHeldSince == 0) {
                        captureHeldSince = System.currentTimeMillis();
                        mouseEventsAtCapture = mouseEvents;   // count from the moment it was held
                    }
                } else {
                    captureHeldSince = 0;
                }
                ui.postDelayed(this, noCapture ? 10000 : (decor.hasPointerCapture() ? 4000 : 500));
            }
        }
    };

    /** The machine's state changed: the ball's icons follow it. */
    private void showControls() {
        if (ball != null) ball.show();
    }

    /** The same, for a caller that only changed what an icon says. */
    private void rebuildControls() {
        if (ball != null) ball.refresh();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        attachSurface(holder);
        ui.postDelayed(capturePointer, 500);
        surfaceReady = true;
        pendingHolder = holder;
        AppLog.i("显示页: surfaceCreated " + holder.getSurface().isValid());
        // A machine started before this screen existed shows up through this window as it is; it
        // must not be booted a second time just because the activity was recreated.
        if (!booted && VmHost.canReopen()) {
            booted = true;
            setStatus("");
            rebuildControls();
            bootWhenReady = false;
        }
        if (bootWhenReady && !booted && !booting) {
            bootWhenReady = false;
            boot();
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
        pendingHolder = null;
        AppLog.i("显示页: surfaceDestroyed");
        // The engine must let go of the window before the surface dies: a render thread presenting
        // into a window the compositor no longer consumes blocks under the presenting queue's
        // lock, and the guest's submissions wait behind it (the machine then looks frozen).
        try {
            Native.nativeDetachWindow();
        } catch (Throwable t) {
            AppLog.w("显示页: 交回 surface 失败", t);
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        AppLog.i("显示页: 打开");
        immerse(true);
    }

    /*
     * The mouse, at the window's own door.
     *
     * Listening on the SurfaceView was not enough: a mouse that is captured, or that hovers over the
     * display, is delivered to the window rather than to the view under the cursor, and the app saw
     * nothing at all (measured: the system cursor moved over the guest's screen, no event reached the
     * engine). These four hooks are where Android hands a mouse to an activity — captured pointer
     * events, generic motion (the wheel), hover, and plain touch from a mouse — so the mouse is
     * taken here, before any view can swallow it, and what arrives is logged so that "no event" and
     * "no handling" can be told apart.
     */
    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (VmInput.isMouse(event)) {
            noteMouse("generic", event);
            if (input != null && input.onMotion(event)) return true;
        }
        return super.dispatchGenericMotionEvent(event);
    }

    @Override
    public boolean dispatchTouchEvent(MotionEvent event) {
        /* Touch is not intercepted here at all: the SurfaceView's own listener is the door it goes
           through, and a finger that misses the ball reaches the guest by the normal path. Only a
           mouse's touch-shaped events are looked at, because some builds deliver a mouse's press
           that way. */
        if (VmInput.isMouse(event)) {
            noteMouse("mouse-touch", event);
            if (input != null && input.onMotion(event)) return true;
        }
        return super.dispatchTouchEvent(event);
    }

    /** Mouse traffic for the log: the first few, then a sample, so a busy mouse cannot flood it. */
    private int mouseEvents, mouseLogged;
    /* MotionEvent.ACTION_CAPTURE_LOST is hidden from the SDK but is delivered to a view that had the
       pointer and lost it; the value is the platform's own (0x4000). */
    private static final int ACTION_CAPTURE_LOST = 0x4000;

    /** Pointer capture: when it was taken, how many events it has produced, and whether it was given up. */
    private long captureHeldSince;
    private int mouseEventsAtCapture;
    private boolean noCapture;
    private int captureLost;
    private int captureTries;
    private void noteMouse(String from, MotionEvent event) {
        mouseEvents++;
        boolean log = mouseEvents <= 8 || (mouseEvents % 200 == 0);
        if (!log) return;
        mouseLogged++;
        AppLog.i("显示页: 鼠标 " + from + " " + MotionEvent.actionToString(event.getActionMasked())
                + " x=" + Math.round(event.getX()) + " y=" + Math.round(event.getY())
                + " src=0x" + Integer.toHexString(event.getSource())
                + (event.getActionMasked() == MotionEvent.ACTION_BUTTON_PRESS
                        ? " button=" + event.getActionButton() : "")
                + " (第 " + mouseEvents + " 个事件)");
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        // A hardware keyboard belongs to the guest, as it would on the Mac itself: the app only
        // keeps the keys that get back to its own screens (返回, and the volume keys).
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK) return super.dispatchKeyEvent(event);
        boolean handled = input != null
                && input.key(event, event.getAction() == KeyEvent.ACTION_DOWN);
        return handled || super.dispatchKeyEvent(event);
    }

    @Override
    protected void onPause() {
        super.onPause();
        // A finger still down when the screen goes away would stay down in the guest.
        if (input != null) input.releaseTouch();
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

    /**
     * A line of state, or nothing at all.
     *
     * Once the machine is up there is no state to report and the guest's screen is the content: a
     * "运行中" left in the corner is a label on a picture that explains itself. It stays for the
     * two moments it is worth something -- while starting, and when something went wrong.
     */
    private void setStatus(String text) {
        boolean show = text != null && !text.isEmpty();
        status.setText(text);
        status.setVisibility(show ? View.VISIBLE : View.GONE);
    }

    /**
     * Starts the guest from the three files the engine needs.
     *
     * Whatever happens is shown here and logged, including the ordinary case that a file is not
     * there yet — that is what an uninstalled machine looks like, and saying which file is missing
     * is more useful than a spinner.
     */
    private void boot() {
        if (booting || booted) {
            showControls();
            return;
        }
        // The machine may have been started earlier by this process; a second boot would build a
        // second machine over the same disk image.
        if (VmHost.canReopen()) {
            booted = true;
            VmHost.setRunning(true);
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
        booting = true;
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
            booting = false;
            if (Native.nativeIsUp()) {
                booted = true;
                VmHost.setRunning(true);
                setStatus("");
                // The earlier attach had no machine to give the window to (see pendingHolder).
                if (pendingHolder != null) attachSurface(pendingHolder);
                setStatus("");
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
        if (stopping) {
            showControls();
            return;
        }
        if (!booted) return;
        stopping = true;
        booted = false;
        VmHost.setRunning(false);
        ui.removeCallbacks(pollBoot);
        rebuildControls();
        showControls();
        // The engine's stop joins every guest thread that is still inside the GPU device, and
        // one of those can be waiting on a present. It therefore runs here, not on this thread.
        Thread stopThread = new Thread(this::stopInBackground, "henge-stop");
        stopThread.setDaemon(true);
        stopThread.start();
        ui.postDelayed(endIfStillStopping, STOP_TIMEOUT_MS);
    }

    /** [`stop`]'s work, off the screen's thread: the engine's join, then the screen's answer. */
    private void stopInBackground() {
        try {
            Native.nativeStop();
        } catch (Throwable t) {
            AppLog.w("显示页: 停止异常", t);
        }
        ui.post(() -> {
            ui.removeCallbacks(endIfStillStopping);
            stopping = false;
            VmHost.setRunning(false);
            fbView.setVisibility(View.GONE);
            fbBitmap = null;
            rebuildControls();
            showControls();
        });
    }

    /**
     * The last resort after {@link #STOP_TIMEOUT_MS}: this screen ends itself, and nothing else.
     *
     * Nothing is told to the system before this point: a stop that finishes in time is a stop, and
     * the screen stays open showing "已停止". Here the guest did not stop within the time given, so
     * this activity reports that it is finished and goes away — the activity alone, not the
     * process: the machine belongs to the process, and ending it is not this screen's decision. Its
     * threads may still be inside the GPU device, which is exactly what could not be joined.
     */
    private final Runnable endIfStillStopping = () -> {
        if (!stopping) return;
        AppLog.w("显示页: 停止超时（" + (STOP_TIMEOUT_MS / 1000) + " 秒），结束本页（进程不结束）", null);
        stopping = false;
        try {
            Native.nativeDetachWindow();
        } catch (Throwable t) {
            AppLog.w("显示页: 超时后交回 surface 失败", t);
        }
        try {
            finishAndRemoveTask();
        } catch (Throwable t) {
            AppLog.w("显示页: finishAndRemoveTask 失败", t);
        }
    };

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
