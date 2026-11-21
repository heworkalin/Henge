package dev.henge.probe;

import android.app.Activity;
import android.content.res.Configuration;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.drawable.GradientDrawable;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.widget.FrameLayout;
import android.widget.LinearLayout;

/**
 * The display screen's controls, as a ball that lives on the edge of the guest's screen.
 *
 * **Why a ball and not a bar.** A bar that stays on the screen takes the guest's screen: on a phone
 * held sideways that is the guest's own menu bar, and any permanent strip is a piece of macOS the
 * person cannot see. A small ball on the edge costs almost nothing, is where a thumb already is,
 * and the controls are one tap away when they are wanted.
 *
 * **What it does.** Tapping the ball shows its icons beside it and hides them again after
 * {@link #AUTO_HIDE_MS} — the same bargain as a video player's controls, for the same reason. The
 * ball can be dragged anywhere and settles on the nearer side edge, so the hand holding the phone
 * decides which side it is on. In landscape that is the left or right strip, out of the guest's way.
 *
 * **What the icons are.** No text: the state is a shape. ▶ start, ■ stop, a fingertip for the touch
 * mode and a pointer for the trackpad mode (in the mode's own colour, so which one is on is
 * readable at a glance), a keyboard for the on-screen control keys, a 文 for the phone's input
 * method, and ← to put the screen away.
 *
 * The icons are drawn, not images: the project ships no drawables, and five shapes are cheaper than
 * five pngs that have to be made for every density.
 */
final class InputBall {

    /** What the ball's icons do, as the screen implements them. */
    interface Actions {
        /** ▶ or ■: start the machine, or stop it. */
        void primary();

        /** The fingertip/pointer icon: switch between the touch and the trackpad input mode. */
        void mode();

        /** The keyboard icon: the on-screen control keys (Esc, ⌘, arrows, …). */
        void keys();

        /** The 文 icon: the phone's input method, whose characters are typed into the guest. */
        void ime();

        /** →: put this screen away; the machine keeps running. */
        void back();

        /** Whether the machine is up: the first icon is ▶ when it is not, ■ when it is. */
        boolean running();

        /** Whether the touch mode (rather than the trackpad mode) is on. */
        boolean touchMode();
    }

    private static final long AUTO_HIDE_MS = 4_000;
    /** The ball's size, and the size of an icon's circle. */
    private static final int BALL_DP = 46;
    /** The ball's own colour, and the lighter body the menu takes when it is open. */
    /**
     * The ball's colour: dark, but opaque enough and outlined (see {@link IconView}) to be seen on a
     * black desktop as well as a white window — at 55% over black it simply was not there.
     */
    private static final int BALL_COLOR = 0xE618181C;
    private static final int MENU_COLOR = 0xF02E2E36;
    private static final int MENU_INK = 0xFFF2F2F5;
    private static final int ICON_DP = 44;

    private final Activity activity;
    private final FrameLayout parent;
    private final Actions actions;
    private final Handler ui = new Handler(Looper.getMainLooper());

    /** Everything the ball owns: the icons, then the ball itself (or the reverse on the right). */
    private final LinearLayout container;
    /**
     * The control as a window of its own, inside the app, above whatever the guest shows.
     *
     * A popup is touch-modal by default — it takes every touch outside itself as well — and while
     * that was so the guest received no finger at all (the engine's counters read
     * `pad 0 (+0, 0 queued)` with a finger on the screen). `setTouchModal(false)`, which is hidden
     * from the SDK and called reflectively below, is what lets a touch that misses the ball belong
     * to the window behind it.
     */
    private final android.widget.PopupWindow popup;
    /** Where the control is, in the screen's pixels: where the window is put. */
    private float posX, posY;
    private final LinearLayout panel;
    private final IconView ball;
    /** The two icons whose shape is a state, not an action. */
    private IconView primaryIcon, modeIcon;

    private boolean expanded;
    private boolean right = true;
    /** Drag state: where the finger was, and whether this touch has moved the ball. */
    private float downRawX, downRawY, startX, startY;
    private boolean moved;

    private final Runnable autoHide = () -> {
        if (expanded) collapse();
    };

    InputBall(Activity activity, FrameLayout parent, Actions actions) {
        this.activity = activity;
        this.parent = parent;
        this.actions = actions;

        container = new LinearLayout(activity);
        container.setOrientation(LinearLayout.HORIZONTAL);
        container.setGravity(Gravity.CENTER_VERTICAL);

        /* No background of its own: the icons are part of the menu the ball becomes, not a second
           floating thing beside it. The lighter shade only appears when the menu is open. */
        panel = new LinearLayout(activity);
        panel.setOrientation(LinearLayout.HORIZONTAL);
        panel.setGravity(Gravity.CENTER_VERTICAL);
        panel.setVisibility(View.GONE);

        ball = new IconView(activity);
        ball.kind = Icon.MOUSE;
        ball.ring = 0xFF2B5FD9;
        ball.outline = true;
        ball.setContentDescription("控制");
        ball.setBackground(circle(BALL_COLOR));

        ViewGroup.LayoutParams ballLp = new LinearLayout.LayoutParams(
                Ui.dp(activity, BALL_DP), Ui.dp(activity, BALL_DP));
        container.addView(ball, ballLp);

        // The icons sit between the ball and the edge it is docked to: on the right edge that means
        // to the ball's left, which is what `installPanel` puts there.
        primaryIcon = icon(Icon.PLAY, actions::primary);
        panel.addView(primaryIcon, iconLp());
        modeIcon = icon(Icon.MOUSE, actions::mode);
        panel.addView(modeIcon, iconLp());
        panel.addView(icon(Icon.KEYS, actions::keys), iconLp());
        panel.addView(icon(Icon.IME, actions::ime), iconLp());
        panel.addView(icon(Icon.BACK, actions::back), iconLp());

        installPanel(true);

        container.setClipChildren(false);

        popup = new android.widget.PopupWindow(container,
                ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        popup.setBackgroundDrawable(null);
        popup.setTouchable(true);
        popup.setOutsideTouchable(false);
        popup.setFocusable(false);                 // the guest keeps the keyboard
        popup.setClippingEnabled(false);           // the ball may hang over an edge
        popup.setInputMethodMode(android.widget.PopupWindow.INPUT_METHOD_NOT_NEEDED);
        int margin = Ui.dp(activity, 6);
        posX = Math.max(margin, parent.getWidth() - Ui.dp(activity, BALL_DP) - margin);
        posY = parent.getHeight() / 2f;
        parent.addOnAttachStateChangeListener(new View.OnAttachStateChangeListener() {
            @Override public void onViewAttachedToWindow(View v) { showPopup(); }
            @Override public void onViewDetachedFromWindow(View v) { if (popup.isShowing()) popup.dismiss(); }
        });
        parent.post(this::showPopup);

        ball.setOnTouchListener(this::onBallTouch);
        ball.setOnClickListener(v -> toggle());

        // Where it starts: the right edge, vertically centred, which in landscape is the strip a
        // right thumb reaches without crossing the guest's screen.
        parent.addOnLayoutChangeListener((v, l, t, r, b, ol, ot, or, ob) -> {
            if (l == ol && t == ot && r == or && b == ob) return;
            dock(right, true);
        });
    }

    private LinearLayout.LayoutParams iconLp() {
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(
                Ui.dp(activity, ICON_DP), Ui.dp(activity, ICON_DP));
        lp.setMarginStart(Ui.dp(activity, 2));
        lp.setMarginEnd(Ui.dp(activity, 2));
        return lp;
    }

    private IconView icon(Icon kind, Runnable action) {
        IconView view = new IconView(activity);
        view.kind = kind;
        view.setBackground(circle(0x00000000));
        view.ink = MENU_INK;
        view.setOnClickListener(v -> {
            action.run();
            refresh();
            // Acting on an icon keeps the panel up: the next tap is usually another icon (mode, then
            // a key, then back), and hiding between them makes every action two taps.
            ui.removeCallbacks(autoHide);
            ui.postDelayed(autoHide, AUTO_HIDE_MS);
        });
        return view;
    }

    private GradientDrawable circle(int color) {
        GradientDrawable d = new GradientDrawable();
        d.setShape(GradientDrawable.OVAL);
        d.setColor(color);
        return d;
    }

    /** Puts the icons on the docked side: left of the ball on the right edge, right of it on the left. */
    private void installPanel(boolean onRight) {
        container.removeView(panel);
        if (onRight) container.addView(panel, 0);
        else container.addView(panel);
    }

    /* ---- state the screen drives ---- */

    /** The machine's state changed (started, stopped, stopping, mode): the icons follow it. */
    void refresh() {
        primaryIcon.kind = actions.running() ? Icon.STOP : Icon.PLAY;
        modeIcon.kind = actions.touchMode() ? Icon.TOUCH : Icon.MOUSE;
        modeIcon.ring = actions.touchMode() ? 0xFFB4560C : 0xFF2B5FD9;
        modeIcon.filled = true;
        /* The ball itself says which mode is on, in its own shape and colour: with the panel hidden
           it is the only thing on the screen that can. */
        ball.kind = actions.touchMode() ? Icon.TOUCH : Icon.MOUSE;
        ball.ring = actions.touchMode() ? 0xFFB4560C : 0xFF2B5FD9;
        invalidateAll();
    }

    private void invalidateAll() {
        for (int i = 0; i < panel.getChildCount(); i++) panel.getChildAt(i).invalidate();
        ball.invalidate();
    }

    void show() {
        container.setVisibility(View.VISIBLE);
        showPopup();
        refresh();
    }

    void hide() {
        collapse();
        if (popup.isShowing()) popup.dismiss();
    }

    /* ---- the ball's own behaviour ---- */

    private void toggle() {
        if (expanded) collapse();
        else expand();
    }

    private void expand() {
        if (expanded) return;
        expanded = true;
        refresh();
        /* One body: the ball and the icons share a rounded, lighter surface while the menu is open,
           and the ball's own circle goes away so there is nothing that reads as a second window. */
        int pad = Ui.dp(activity, 3);
        container.setPadding(pad, pad, pad, pad);
        container.setBackground(Theme.cardBackground(activity, MENU_COLOR, 26f));
        ball.setBackground(circle(0x00000000));
        for (int i = 0; i < panel.getChildCount(); i++) ((IconView) panel.getChildAt(i)).ink = MENU_INK;
        panel.setVisibility(View.VISIBLE);
        panel.setAlpha(0f);
        panel.setTranslationX(menuGrow() * Ui.dp(activity, 10));
        panel.animate().alpha(1f).translationX(0f).setDuration(140).start();
        ball.invalidate();
        ball.setAlpha(1f);
        /* The container just got wider: without this the side it grows away from would push the
           menu past the screen edge, which is what made it look like the menu vanished. */
        resize();
        redockAfterLayout(right);
        ui.removeCallbacks(autoHide);
        ui.postDelayed(autoHide, AUTO_HIDE_MS);
    }

    private void collapse() {
        ui.removeCallbacks(autoHide);
        if (!expanded) {
            ball.setAlpha(0.9f);
            return;
        }
        expanded = false;
        panel.animate().alpha(0f).translationX(menuGrow() * Ui.dp(activity, 10)).setDuration(120)
                .withEndAction(() -> {
                    if (expanded) return;
                    panel.setVisibility(View.GONE);
                    // Back to one dark ball: the lighter body was the open menu's, not the ball's.
                    container.setPadding(0, 0, 0, 0);
                    container.setBackground(null);
                    ball.setBackground(circle(BALL_COLOR));
                    ball.invalidate();
                    // Narrower again: back onto the edge it is docked to, with its collapsed width.
                    redockAfterLayout(right);
                }).start();
        // Half transparent when idle: it is furniture, not content.
        ball.setAlpha(0.9f);
    }

    /**
     * Dock again once the container has the width it will actually have.
     *
     * A view that has just been made wider or narrower keeps its old X and its old size until the
     * next layout, so docking straight after the change puts the container where the *previous*
     * width says it should be: the menu was closing and the icons jumped to the middle of the
     * screen, because the collapsed ball was placed as if the five icons were still there.
     */
    private void redockAfterLayout(boolean onRight) {
        container.getViewTreeObserver().addOnGlobalLayoutListener(
                new android.view.ViewTreeObserver.OnGlobalLayoutListener() {
                    @Override
                    public void onGlobalLayout() {
                        container.getViewTreeObserver().removeOnGlobalLayoutListener(this);
                        dock(onRight, false);
                    }
                });
    }

    /** Which way the menu grows: away from the edge the ball is docked to. */
    private float menuGrow() {
        return right ? -1f : 1f;
    }

    private boolean onBallTouch(View v, MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                downRawX = e.getRawX();
                downRawY = e.getRawY();
                startX = posX;
                startY = posY;
                moved = false;
                ui.removeCallbacks(autoHide);
                return true;
            case MotionEvent.ACTION_MOVE: {
                float dx = e.getRawX() - downRawX;
                float dy = e.getRawY() - downRawY;
                if (!moved && Math.hypot(dx, dy) < Ui.dp(activity, 6)) return true;
                moved = true;
                /* No clamping to the screen here: a clamped position is a window that stops under
                   the finger and then jumps when it is released. The clamp happens at `dock`. */
                place(startX + dx, startY + dy);
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                if (moved) {
                    dock(posX + container.getWidth() / 2f > parent.getWidth() / 2f, false);
                    resize();
                } else if (e.getActionMasked() == MotionEvent.ACTION_UP) {
                    v.performClick();
                } else {
                    ui.postDelayed(autoHide, AUTO_HIDE_MS);
                }
                return true;
            default:
                return false;
        }
    }

    /**
     * Where the control sits, in the parent's pixels.
     *
     * Laid out through the layout parameters rather than moved with `setX`/`setY`: a translation is
     * applied on top of a layout position the parent chose, and when the container then changes
     * width — the menu opening, or closing again — the two disagree and the thing lands somewhere
     * its own bounds are not. Margins are the position, so there is nothing to disagree.
     */
    private void place(float x, float y) {
        posX = Math.max(0, x);
        posY = Math.max(0, y);
        if (popup.isShowing()) {
            popup.update(Math.round(posX), Math.round(posY),
                    Math.max(1, container.getWidth()), Math.max(1, container.getHeight()));
        }
    }

    /** Puts the control on the screen, once there is a window to put it in. */
    private void showPopup() {
        if (popup.isShowing() || !parent.isAttachedToWindow() || parent.getWindowToken() == null) {
            return;
        }
        try {
            java.lang.reflect.Method method =
                    android.widget.PopupWindow.class.getMethod("setTouchModal", boolean.class);
            method.invoke(popup, false);
        } catch (Throwable t) {
            android.util.Log.w("HENGE", "the ball cannot be made non-modal: " + t);
        }
        try {
            container.measure(
                    View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED),
                    View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
            popup.showAtLocation(parent, Gravity.TOP | Gravity.START,
                    Math.round(posX), Math.round(posY));
        } catch (RuntimeException e) {
            android.util.Log.w("HENGE", "the control ball was not shown yet: " + e);
        }
    }

    /** The control's size changed (the menu opened or closed): the window follows it. */
    private void resize() {
        container.measure(
                View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED),
                View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        if (popup.isShowing()) {
            popup.update(Math.round(posX), Math.round(posY),
                    container.getMeasuredWidth(), container.getMeasuredHeight());
        }
    }

    /** Settles on the nearer side edge, keeping its height (or centring it, when first placed). */
    private void dock(boolean onRight, boolean centre) {
        if (onRight != right) {
            right = onRight;
            installPanel(onRight);
        }
        int margin = Ui.dp(activity, 6);
        container.measure(
                View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED),
                View.MeasureSpec.makeMeasureSpec(0, View.MeasureSpec.UNSPECIFIED));
        float width = container.getMeasuredWidth() > 0 ? container.getMeasuredWidth()
                : Ui.dp(activity, BALL_DP);
        float x = onRight ? parent.getWidth() - width - margin : margin;
        float height = Math.max(container.getMeasuredHeight(), Ui.dp(activity, BALL_DP));
        float y = centre ? (parent.getHeight() - height) / 2f : container.getTop();
        // A menu that grows past an edge is a menu with icons nobody can press.
        float maxY = Math.max(margin, parent.getHeight() - height - margin);
        posX = Math.max(margin, Math.min(x, Math.max(margin, parent.getWidth() - width - margin)));
        posY = Math.max(margin, Math.min(y, maxY));
        if (popup.isShowing()) {
            popup.update(Math.round(posX), Math.round(posY),
                    Math.round(width), Math.round(height));
        }
    }

    /** Whether the person is holding the phone sideways: the ball then sits on a side edge strip. */
    boolean landscape() {
        return activity.getResources().getConfiguration().orientation
                == Configuration.ORIENTATION_LANDSCAPE;
    }

    /* ---- the icons, drawn ---- */

    /** Which shape an {@link IconView} draws. */
    enum Icon {
        PLAY, STOP, TOUCH, MOUSE, KEYS, IME, BACK;
    }

    /**
     * One drawn icon.
     *
     * Every shape is in a unit square and scaled to the view, so a 44 dp circle and a 46 dp ball
     * draw the same icon at slightly different sizes with no images and no density folders.
     */
    static final class IconView extends View {
        Icon kind = Icon.PLAY;
        /** A colour ring around the icon, used by the mode icon to say which mode is on. */
        int ring;
        /** The icons' colour: light on the ball, and the menu keeps it light too. */
        int ink = 0xFFF2F2F5;
        /** Whether the ring is filled: the mode icon's "this is on". */
        boolean filled;
        /** Whether to draw a light hairline around the view: the ball, over the guest's screen. */
        boolean outline;

        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final Path path = new Path();

        IconView(Activity activity) {
            super(activity);
            stroke.setStyle(Paint.Style.STROKE);
            stroke.setStrokeCap(Paint.Cap.ROUND);
            stroke.setStrokeJoin(Paint.Join.ROUND);
        }

        private void applyInk() {
            stroke.setColor(ink);
            paint.setColor(ink);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            float w = getWidth();
            float h = getHeight();
            float size = Math.min(w, h);
            float cx = w / 2f;
            float cy = h / 2f;
            applyInk();
            float unit = size * 0.52f;      // the icon's own square
            float half = unit / 2f;
            stroke.setStrokeWidth(size * 0.075f);

            if (outline) {
                // A hairline of light around the ball: it is drawn over whatever the guest has on
                // the screen, and a dark disc on a dark desktop is invisible without one.
                Paint edge = new Paint(Paint.ANTI_ALIAS_FLAG);
                edge.setStyle(Paint.Style.STROKE);
                edge.setStrokeWidth(Math.max(1f, size * 0.035f));
                edge.setColor(0x8CFFFFFF);
                canvas.drawCircle(cx, cy, size * 0.47f, edge);
            }

            if (ring != 0) {
                // The mode's colour, as a disc under the glyph: filled when that mode is the one on,
                // and the same colour as a thin ring when it is not, so the two read differently at
                // a glance without changing the icon's size.
                Paint ringPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
                if (filled) {
                    ringPaint.setColor(ring);
                    canvas.drawCircle(cx, cy, size * 0.44f, ringPaint);
                    stroke.setColor(0xFF121218);
                    paint.setColor(0xFF121218);
                } else {
                    ringPaint.setStyle(Paint.Style.STROKE);
                    ringPaint.setStrokeWidth(size * 0.05f);
                    ringPaint.setColor(ring);
                    canvas.drawCircle(cx, cy, size * 0.45f, ringPaint);
                }
            }

            switch (kind) {
                case PLAY: {
                    path.reset();
                    path.moveTo(cx - half * 0.55f, cy - half);
                    path.lineTo(cx + half * 0.9f, cy);
                    path.lineTo(cx - half * 0.55f, cy + half);
                    path.close();
                    canvas.drawPath(path, paint);
                    break;
                }
                case STOP: {
                    RectF box = new RectF(cx - half * 0.85f, cy - half * 0.85f,
                            cx + half * 0.85f, cy + half * 0.85f);
                    canvas.drawRoundRect(box, size * 0.06f, size * 0.06f, paint);
                    break;
                }
                case TOUCH: {
                    // A fingertip: an outer arc, an inner one, and the point that touched.
                    canvas.drawCircle(cx, cy - half * 0.25f, half * 0.95f, stroke);
                    canvas.drawCircle(cx, cy - half * 0.25f, half * 0.55f, stroke);
                    canvas.drawCircle(cx, cy - half * 0.25f, half * 0.2f, paint);
                    break;
                }
                case MOUSE: {
                    // A pointer: the classic arrow, in one closed path.
                    path.reset();
                    path.moveTo(cx - half * 0.45f, cy - half);
                    path.lineTo(cx - half * 0.45f, cy + half * 0.75f);
                    path.lineTo(cx - half * 0.05f, cy + half * 0.3f);
                    path.lineTo(cx + half * 0.25f, cy + half);
                    path.lineTo(cx + half * 0.55f, cy + half * 0.72f);
                    path.lineTo(cx + half * 0.25f, cy + half * 0.06f);
                    path.lineTo(cx + half * 0.8f, cy + half * 0.02f);
                    path.close();
                    canvas.drawPath(path, paint);
                    break;
                }
                case KEYS: {
                    // A keyboard: a rounded body, two rows of keys, and a space bar.
                    RectF body = new RectF(cx - half, cy - half * 0.62f, cx + half, cy + half * 0.62f);
                    stroke.setStrokeWidth(size * 0.06f);
                    canvas.drawRoundRect(body, size * 0.07f, size * 0.07f, stroke);
                    float key = size * 0.055f;
                    for (int i = 0; i < 3; i++) {
                        float x = cx - half * 0.62f + i * half * 0.62f;
                        canvas.drawCircle(x, cy - half * 0.24f, key, paint);
                    }
                    for (int i = 0; i < 3; i++) {
                        float x = cx - half * 0.62f + i * half * 0.62f;
                        canvas.drawCircle(x, cy + half * 0.06f, key, paint);
                    }
                    RectF space = new RectF(cx - half * 0.42f, cy + half * 0.26f,
                            cx + half * 0.42f, cy + half * 0.42f);
                    canvas.drawRoundRect(space, key, key, paint);
                    break;
                }
                case IME: {
                    Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
                    text.setColor(ink);
                    text.setTextSize(unit * 0.95f);
                    text.setTextAlign(Paint.Align.CENTER);
                    text.setFakeBoldText(true);
                    // 文: "text". Every device that shows Chinese shows this glyph.
                    float baseline = cy - (text.descent() + text.ascent()) / 2f;
                    canvas.drawText("文", cx, baseline, text);
                    break;
                }
                case BACK:
                default: {
                    stroke.setStrokeWidth(size * 0.085f);
                    path.reset();
                    path.moveTo(cx + half * 0.5f, cy - half * 0.7f);
                    path.lineTo(cx - half * 0.35f, cy);
                    path.lineTo(cx + half * 0.5f, cy + half * 0.7f);
                    canvas.drawPath(path, stroke);
                    break;
                }
            }
        }
    }
}
