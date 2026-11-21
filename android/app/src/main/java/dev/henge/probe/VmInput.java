package dev.henge.probe;

import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;

/**
 * The person's fingers, mouse and keyboard, as the guest sees them.
 *
 * **What the guest has.** A real USB keyboard (Apple's own vendor/product, so macOS needs no
 * keyboard setup assistant) and a real Magic Trackpad 2 — both in `devices/xhci.c`. There is no
 * touch screen and no absolute pointing device in the guest, so a touch can only become *pad*:
 * a finger on the trackpad whose movement macOS turns into pointer movement with its own
 * acceleration curve. That is why nothing here has an acceleration curve of its own: adding one
 * would accelerate the same movement twice.
 *
 * **The two modes**, which exist because one of them is always wrong for somebody:
 *
 * - {@link Mode#TOUCH} — the pointer moves as far as the finger has moved *since it went down*
 *   (an anchor, not a per-frame delta). Moving a finger three centimetres moves the pointer a
 *   predictable amount however fast it went, which is what a touch screen feels like and what
 *   makes a distant button reachable on a small screen.
 * - {@link Mode#MOUSE} — a trackpad: every frame's movement is added, so a short stroke of the
 *   finger is a short stroke of the pointer, and repeating the stroke keeps going. Precise, and
 *   able to cross the screen in several strokes rather than one.
 *
 * Both send the same device reports (`Native.nativeTouch*`), so switching modes changes the
 * maths, not the guest.
 *
 * **Gestures**, the trackpad's own: one finger moves, a tap without movement clicks, holding a
 * finger still for {@link #HOLD_MS} holds the button down (so moving after it is a drag), and two
 * or more fingers are reported as two or more fingers — the guest's own gesture engine then does
 * scrolling, pinch and swipes, because it is a trackpad and not a mouse.
 *
 * **The keyboard** is two things: {@link #keyDown}/{@link #keyUp} for a key that already exists as
 * an Android key event (a hardware keyboard, or the on-screen board), and {@link #type} for text
 * an input method composed, character by character on the guest's keyboard. The guest's own input
 * method is what turns letters into text (a Chinese input method in the guest wants the letters
 * of pinyin, not the characters), so nothing here tries to send characters the guest's keyboard
 * cannot type.
 */
final class VmInput {

    /** How the finger becomes the pointer: directly (an anchor) or like a trackpad (per frame). */
    enum Mode {
        /** 触控: the pointer follows the finger's distance from where it went down. */
        TOUCH,
        /** 鼠标: the pointer takes every frame's movement, as a trackpad does. */
        MOUSE,
    }

    private static final int FINGER = 1;
    private static final int SECOND_FINGER = 2;
    /** A tap: shorter than this and it moved less than {@link #TAP_SLOP_DP}. */
    private static final long TAP_MS = 300;
    private static final float TAP_SLOP_DP = 18f;
    /**
     * Holding still this long presses the button; moving after it is a drag.
     *
     * The timer is re-armed on every move, so the two ways a person drags both work: stay still and
     * then move, or move and then stop for a moment and carry on. It used to be cancelled for good
     * by the first movement, which made the second way impossible.
     */
    private static final long HOLD_MS = 280;
    /**
     * Pointer travel per pixel of finger travel, in the pad's sensor units.
     *
     * The pad is 8134 units across, about 160 mm of physical glass. A phone's screen is smaller
     * than that and a thumb travels less far than a finger on a pad, so the same physical speed
     * gives macOS a much slower finger: at a gain that matched the pad exactly (about 2.9 units a
     * pixel) the pointer crawled, because the guest's own acceleration reads small, slow deltas and
     * damps them further. These are deliberately above 1:1 — the pad's own acceleration curve still
     * shapes fast movements, and the gain only makes ordinary ones usable.
     */
    private static final float TOUCH_GAIN = 6.0f;
    private static final float MOUSE_GAIN = 12.0f;

    private final View view;
    private final Runnable onHold;

    private Mode mode = Mode.MOUSE;

    /* The pad's own coordinate space, which is what the guest reads: 0..HV_TP_SENSOR_X/Y. */
    private static final int PAD_X = 8134;
    private static final int PAD_Y = 5206;
    /** Where the pointer "is" on the pad: the finger it reports is put here. */
    private float padX = PAD_X / 2f;
    private float padY = PAD_Y / 2f;
    /** The pad position the anchor mode counts from, and the finger's own position at the anchor. */
    private float anchorPadX, anchorPadY, anchorX, anchorY;
    private float lastX, lastY;
    private long downMs;
    private boolean down;
    private boolean moved;
    private boolean buttonHeld;
    private final Runnable hold = this::holdButton;
    /** The hold timer is armed (one at a time, not one a sample). */
    private boolean holdPending;

    VmInput(View view, Runnable onHold) {
        this.view = view;
        this.onHold = onHold;
    }

    Mode mode() {
        return mode;
    }

    void setMode(Mode value) {
        releaseTouch();
        mode = value;
    }

    /* ---- one door ---- */

    /**
     * Every motion event the screen gets, touch or mouse, through one door.
     *
     * The two used to be handled in two different places — touch on the SurfaceView, a mouse in the
     * activity's dispatch — and that is how events went missing: whichever layer handled first could
     * consume the event, and the layer below never saw it. Here the decision is made once, from the
     * event's own source, and the answer is therefore the same wherever it is called from.
     */
    boolean onMotion(MotionEvent e) {
        if (!Native.nativeIsUp()) return false;
        if (isMouse(e)) return onMouseEvent(e);
        return onTouch(null, e);
    }

    /**
     * A mouse, as Android reports one.
     *
     * Three shapes, not one: a hovering mouse (SOURCE_MOUSE), a **captured** mouse — which Android
     * re-sources as SOURCE_MOUSE_RELATIVE, the source this check used to miss, so every captured
     * movement was handled as a finger — a touchpad, and anything whose tool is reported as a
     * mouse. The tool check is what the platform's own guidance calls the safe test, because
     * Android re-emits a mouse's clicks as touch events when an app has no mouse handling.
     */
    static boolean isMouse(MotionEvent e) {
        int source = e.getSource();
        return (source & android.view.InputDevice.SOURCE_MOUSE) == android.view.InputDevice.SOURCE_MOUSE
                || (source & android.view.InputDevice.SOURCE_MOUSE_RELATIVE)
                        == android.view.InputDevice.SOURCE_MOUSE_RELATIVE
                || (source & android.view.InputDevice.SOURCE_TOUCHPAD)
                        == android.view.InputDevice.SOURCE_TOUCHPAD
                || e.getToolType(0) == MotionEvent.TOOL_TYPE_MOUSE;
    }

    /* ---- touch ---- */

    /** Attach as the view's touch listener. The screen's own gestures are consumed here. */
    boolean onTouch(View v, MotionEvent e) {
        if (!Native.nativeIsUp()) return false;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN:
                begin(e);
                return true;
            case MotionEvent.ACTION_POINTER_DOWN:
                // A second finger joins the pad where it is; the first one keeps its place.
                report(e, SECOND_FINGER, 0, e.getX(e.getActionIndex()), e.getY(e.getActionIndex()));
                Native.nativeTouchFrame(e.getEventTime());
                return true;
            case MotionEvent.ACTION_MOVE:
                drag(e);
                return true;
            case MotionEvent.ACTION_POINTER_UP: {
                int index = e.getActionIndex();
                report(e, SECOND_FINGER, 2, e.getX(index), e.getY(index));
                Native.nativeTouchFrame(e.getEventTime());
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                end(e);
                return true;
            default:
                return true;
        }
    }

    /** One tablet report from a screen point: the absolute pointer's own 0..32767 range. */
    private void tabletAt(MotionEvent e, int buttons) {
        float w = Math.max(1, view.getWidth());
        float h = Math.max(1, view.getHeight());
        int x16 = Math.round(Math.max(0, Math.min(1f, e.getX() / w)) * 32767f);
        int y16 = Math.round(Math.max(0, Math.min(1f, e.getY() / h)) * 32767f);
        Native.nativeTablet(x16, y16, buttons, 0);
    }

    private void begin(MotionEvent e) {
        if (mode == Mode.TOUCH) {
            // Absolute: the pointer is where the finger is, from the first report.
            down = true;
            moved = false;
            buttonHeld = false;
            downMs = e.getEventTime();
            anchorX = lastX = e.getX();
            anchorY = lastY = e.getY();
            tabletAt(e, 0);
            holdPending = true;
            view.postDelayed(hold, HOLD_MS);
            return;
        }
        down = true;
        moved = false;
        buttonHeld = false;
        downMs = e.getEventTime();
        lastX = anchorX = e.getX();
        lastY = anchorY = e.getY();
        anchorPadX = padX;
        anchorPadY = padY;
        holdPending = true;
        view.postDelayed(hold, HOLD_MS);
        // The finger goes down where the pad's pointer is: on a trackpad a finger landing anywhere
        // touches nothing until it moves, and the click is a separate report.
        Native.nativeTouch(FINGER, 0, Math.round(padX), Math.round(padY), (int) e.getEventTime());
    }

    /**
     * One frame's worth of movement, including the samples the system batched into it.
     *
     * Android coalesces touch moves: one `ACTION_MOVE` can carry several historical points, and
     * reading only its last position throws the rest away. On screen that reads as the pointer
     * lagging behind the finger and arriving in jumps — the guest is given two or three positions a
     * frame instead of eight, and its own acceleration smooths what is left into a crawl. Replaying
     * the history costs nothing and is the difference between following the finger and chasing it.
     */
    private void drag(MotionEvent e) {
        if (!down) return;
        if (mode == Mode.TOUCH) {
            float x = e.getX();
            float y = e.getY();
            if (Math.abs(x - anchorX) > dp(TAP_SLOP_DP) || Math.abs(y - anchorY) > dp(TAP_SLOP_DP)) {
                moved = true;
            }
            tabletAt(e, buttonHeld ? 1 : 0);
            return;
        }
        int index = e.findPointerIndex(0);
        if (index < 0) index = 0;
        /* The batched samples are folded into this one report rather than sent one each: the pad's
           interrupt endpoint asks for a report every 4 ms (250 a second), and a finger sampled at
           120 Hz with eight history points would queue four times that, which is a backlog, not
           smoothness. What the guest needs is the distance, and the distance is the same either
           way. */
        step(e.getX(index), e.getY(index), e.getEventTime(), e.getPointerCount());
    }

    /** One sample: move the pad's finger to where the screen's finger is now. */
    private void step(float x, float y, long when, int pointers) {
        float dx, dy;
        if (mode == Mode.TOUCH) {
            // From the anchor: the pointer is where the finger is, not where the frames added up.
            float wantedX = anchorPadX + (x - anchorX) * TOUCH_GAIN;
            float wantedY = anchorPadY + (y - anchorY) * TOUCH_GAIN;
            dx = wantedX - padX;
            dy = wantedY - padY;
            padX = wantedX;
            padY = wantedY;
        } else {
            dx = (x - lastX) * MOUSE_GAIN;
            dy = (y - lastY) * MOUSE_GAIN;
            padX += dx;
            padY += dy;
        }
        lastX = x;
        lastY = y;
        if (Math.abs(x - anchorX) > dp(TAP_SLOP_DP) || Math.abs(y - anchorY) > dp(TAP_SLOP_DP)) {
            moved = true;
        }
        if (!buttonHeld && !holdPending) {
            // Still still for HOLD_MS and the button goes down: a drag from here, whether the finger
            // has moved already or not. Armed once, not on every sample: a timer that is cancelled
            // and re-posted eight times a frame is work on the screen's thread for nothing.
            holdPending = true;
            view.postDelayed(hold, HOLD_MS);
        }
        clampPad();
        if (dx != 0f || dy != 0f || pointers > 1) {
            Native.nativeTouch(FINGER, 1, Math.round(padX), Math.round(padY), (int) when);
            for (int i = 1; i < pointers && i < 4; i++) {
                float px = padX + (i * dp(1) * MOUSE_GAIN);
                Native.nativeTouch(SECOND_FINGER + i - 1, 1, clamp(Math.round(px), PAD_X),
                        clamp(Math.round(padY), PAD_Y), (int) when);
            }
            Native.nativeTouchFrame((int) when);
        }
    }

    private void end(MotionEvent e) {
        if (!down) return;
        if (mode == Mode.TOUCH) {
            down = false;
            holdPending = false;
            view.removeCallbacks(hold);
            boolean tap = !moved && e.getEventTime() - downMs <= TAP_MS;
            if (buttonHeld) {
                tabletAt(e, 0);              // release the button where the finger is
            } else if (tap) {
                tabletAt(e, 1);              // down at this point...
                tabletAt(e, 0);              // ...and up, which is a click there
            }
            buttonHeld = false;
            return;
        }
        down = false;
        holdPending = false;
        view.removeCallbacks(hold);
        boolean tap = !moved && e.getEventTime() - downMs <= TAP_MS;
        // Two fingers, tapped without moving: the pad's own gesture for a secondary click. A
        // trackpad has one button, so the click is a Control-click, which macOS defines as the
        // secondary click everywhere (it needs no setting in the guest, unlike two-finger click).
        boolean secondary = tap && e.getPointerCount() >= 2;
        Native.nativeTouch(FINGER, 2, Math.round(padX), Math.round(padY), (int) e.getEventTime());
        for (int i = 1; i < e.getPointerCount() && i < 4; i++) {
            Native.nativeTouch(SECOND_FINGER + i - 1, 2, Math.round(padX), Math.round(padY),
                    (int) e.getEventTime());
        }
        Native.nativeTouchFrame(e.getEventTime());
        if (buttonHeld) {
            Native.nativeTouchButton(false);
            buttonHeld = false;
        } else if (secondary) {
            Native.nativeKey(0xe0, true);       // Control
            Native.nativeTouchButton(true);
            Native.nativeTouchButton(false);
            Native.nativeKey(0xe0, false);
        } else if (tap) {
            // A click where the pointer is: on a trackpad that is the pad's own button, not a
            // finger that moved somewhere and back. Two of these in quick succession are a double
            // click, because the guest applies its own interval to them.
            Native.nativeTouchButton(true);
            Native.nativeTouchButton(false);
        }
    }

    /** The finger stayed still: hold the button, so a movement from here is a drag. */
    private void holdButton() {
        holdPending = false;
        if (!down) return;
        buttonHeld = true;
        if (mode == Mode.TOUCH) {
            Native.nativeTablet(0, 0, 1, 0); // the press: a drag from here is a drag in the guest
            return;
        }
        Native.nativeTouchButton(true);
        if (onHold != null) onHold.run();
    }

    private void report(MotionEvent e, int id, int phase, float x, float y) {
        float px = padX + (x - lastX) * MOUSE_GAIN;
        float py = padY + (y - lastY) * MOUSE_GAIN;
        Native.nativeTouch(id, phase, clamp(Math.round(px), PAD_X), clamp(Math.round(py), PAD_Y),
                (int) e.getEventTime());
    }

    /** Lifts every finger: a screen that goes away must not leave one pressed. */
    void releaseTouch() {
        if (!down) return;
        down = false;
        holdPending = false;
        view.removeCallbacks(hold);
        long now = android.os.SystemClock.uptimeMillis();
        Native.nativeTouch(FINGER, 2, Math.round(padX), Math.round(padY), (int) now);
        Native.nativeTouchFrame((int) now);
        if (buttonHeld) {
            Native.nativeTouchButton(false);
            buttonHeld = false;
        }
    }

    private void clampPad() {
        padX = clamp(padX, PAD_X);
        padY = clamp(padY, PAD_Y);
    }

    private static float clamp(float v, int max) {
        return v < 0 ? 0 : (v > max ? max : v);
    }

    private static int clamp(int v, int max) {
        return v < 0 ? 0 : (v > max ? max : v);
    }

    private int dp(float value) {
        return Math.round(value * view.getResources().getDisplayMetrics().density);
    }

    /* ---- a mouse: a Bluetooth or USB one ---- */

    /** The mouse's own position, when it is not captured: hover moves give absolute coordinates. */
    private float mouseX = -1, mouseY = -1;
    /* A real mouse and a finger both become the trackpad's finger: the guest's macOS has a keyboard
       and a trackpad and no mouse (Apple's own macOS VMs are shaped that way), and a third device on
       a root port is a device its driver never enumerated. */
    /** Whether the pad currently reports a finger held down for the mouse. */
    private boolean mouseFinger;
    /** Lifts that finger once the mouse stops moving. */
    private final Runnable mouseIdle = this::mouseLift;
    private static final long MOUSE_IDLE_MS = 50;

    /**
     * A real mouse: a Bluetooth or USB one the person plugged in.
     *
     * It is not a touch: it has no finger, it moves whether or not anything is pressed, and its
     * buttons mean what they say. On the guest's side there is only the trackpad, so a mouse becomes
     * *one finger gliding on it* — held down while the mouse moves, lifted when it stops, which is
     * what makes macOS's pointer follow without also seeing a click. Buttons go to the pad's own
     * click (left) or to Control-click (right, macOS's secondary click everywhere), the wheel to a
     * two-finger scroll, because that is how a trackpad scrolls.
     *
     * Motion arrives two ways: with the pointer captured (`ACTION_MOVE`, the values are deltas) or
     * without it (`ACTION_HOVER_MOVE`, the values are where the system cursor is). Both are handled
     * here so the app works whether or not the system lets it take the pointer.
     */
    boolean onMouseEvent(MotionEvent e) {
        if (!Native.nativeIsUp()) return false;
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_MOVE:
            case MotionEvent.ACTION_HOVER_MOVE:
                return mouseMove(e);
            case MotionEvent.ACTION_BUTTON_PRESS:
                mouseButton(e.getActionButton(), true);
                return true;
            case MotionEvent.ACTION_BUTTON_RELEASE:
                mouseButton(e.getActionButton(), false);
                return true;
            /* Some mice (and captured pointers on some builds) report a click as a plain touch
               down/up with the button in `getButtonState()` rather than as a button press. */
            case MotionEvent.ACTION_DOWN:
                mouseButton(buttonOf(e), true);
                return true;
            case MotionEvent.ACTION_UP:
                mouseButton(buttonOf(e), false);
                return true;
            case MotionEvent.ACTION_SCROLL: {
                /* Three axis names for one wheel: AXIS_VSCROLL is the usual one, AXIS_WHEEL is what
                   a HID mouse's wheel reports on some builds, and AXIS_SCROLL is the generic name. A
                   wheel that arrives on one we do not read is a wheel that does nothing. */
                float dy = e.getAxisValue(MotionEvent.AXIS_VSCROLL);
                if (dy == 0f) dy = e.getAxisValue(MotionEvent.AXIS_WHEEL);
                if (dy == 0f) dy = e.getAxisValue(MotionEvent.AXIS_SCROLL);
                float dx = e.getAxisValue(MotionEvent.AXIS_HSCROLL);
                if (dx == 0f) dx = e.getAxisValue(MotionEvent.AXIS_HSCROLL) != 0f
                        ? e.getAxisValue(MotionEvent.AXIS_HSCROLL) : e.getAxisValue(MotionEvent.AXIS_Z);
                AppLog.i("显示页: 滚轮 dy=" + dy + " dx=" + dx
                        + " vscroll=" + e.getAxisValue(MotionEvent.AXIS_VSCROLL)
                        + " wheel=" + e.getAxisValue(MotionEvent.AXIS_WHEEL)
                        + " scroll=" + e.getAxisValue(MotionEvent.AXIS_SCROLL)
                        + " hscroll=" + e.getAxisValue(MotionEvent.AXIS_HSCROLL));
                scroll(dx, dy);
                return true;
            }
            default:
                return false;
        }
    }

    private boolean mouseMove(MotionEvent e) {
        float dx, dy;
        if (e.getActionMasked() == MotionEvent.ACTION_MOVE) {
            // Captured pointer: the values are the mouse's own movement since the last one.
            dx = e.getX() * MOUSE_GAIN;
            dy = e.getY() * MOUSE_GAIN;
        } else {
            // Hover: absolute coordinates, so the movement is the difference from the last hover.
            float x = e.getX();
            float y = e.getY();
            if (mouseX < 0) {
                mouseX = x;
                mouseY = y;
                return true;
            }
            dx = (x - mouseX) * MOUSE_GAIN;
            dy = (y - mouseY) * MOUSE_GAIN;
            mouseX = x;
            mouseY = y;
        }
        if (dx == 0f && dy == 0f) return true;
        /* A real mouse is a real mouse: the guest has one (devices/xhci.c's third device), so the
           movement goes as its own relative movement and the guest applies its own acceleration.
           Translating it into a finger on the pad -- which is what this did first -- meant the
           pointer was driven by two acceleration curves at once, and the wheel had to be faked as a
           two-finger gesture. */
        if (!mouseFinger) {
            mouseFinger = true;
            Native.nativeTouch(FINGER, 0, Math.round(padX), Math.round(padY),
                    (int) e.getEventTime());
        }
        padX += dx;
        padY += dy;
        clampPad();
        Native.nativeTouch(FINGER, 1, Math.round(padX), Math.round(padY), (int) e.getEventTime());
        Native.nativeTouchFrame((int) e.getEventTime());
        // The finger is lifted as soon as the mouse stops, so a moving mouse is not a held button.
        view.removeCallbacks(mouseIdle);
        view.postDelayed(mouseIdle, MOUSE_IDLE_MS);
        return true;
    }

    private void mouseLift() {
        if (!mouseFinger) return;
        mouseFinger = false;
        Native.nativeTouch(FINGER, 2, Math.round(padX), Math.round(padY),
                (int) android.os.SystemClock.uptimeMillis());
        Native.nativeTouchFrame((int) android.os.SystemClock.uptimeMillis());
    }

    /** Which button a plain touch event carries, when it says nothing better. */
    private static int buttonOf(MotionEvent e) {
        int state = e.getButtonState();
        if ((state & MotionEvent.BUTTON_SECONDARY) != 0) return MotionEvent.BUTTON_SECONDARY;
        if ((state & MotionEvent.BUTTON_TERTIARY) != 0) return MotionEvent.BUTTON_TERTIARY;
        int action = e.getActionButton();
        if (action != 0) return action;
        return MotionEvent.BUTTON_PRIMARY;
    }

    /**
     * A click.
     *
     * On a trackpad a click is not a report on its own: the pad's button goes down with a finger on
     * the glass, which is why a click is sent as "contact, then button", and released the other way
     * round. Sent as a button alone the guest's driver has no finger to attach it to, and nothing
     * happened at all (measured: left, right and middle clicks were all silent).
     *
     * The pad has one button, so a secondary click is Control-click — macOS's secondary click
     * everywhere, needing no setting in the guest — and a middle click becomes Command-click, which
     * is what a middle click means in the places that use one (open in a new tab, open in a new
     * window).
     */
    private void mouseButton(int button, boolean down) {
        // 1 = left, 2 = right, 3 = middle: the guest's mouse has all three.
        int hid = button == MotionEvent.BUTTON_SECONDARY ? 2
                : button == MotionEvent.BUTTON_TERTIARY ? 3 : 1;
        AppLog.i("显示页: 鼠标按键 hid=" + hid + (down ? " 按下" : " 抬起")
                + "（原始 button=" + button + "）");
        Native.nativeMouseButton(hid, down);
    }

    /**
     * The wheel: whole notches on the guest's own mouse.
     *
     * A wheel is not a gesture. This used to be sent as two fingers on the trackpad, in a timed
     * sequence, because the guest had no wheel to turn; now it does.
     */
    private void scroll(float dx, float dy) {
        if (dy == 0f) return;
        Native.nativeMouseWheel(Math.round(dy));
    }

    /* ---- keyboard ---- */

    /** A hardware or on-screen key, down or up, by Android key code. */
    boolean key(KeyEvent e, boolean down) {
        if (!Native.nativeIsUp()) return false;
        int usage = usageFor(e);
        if (usage == 0) return false;
        if (e.getRepeatCount() > 0 && down) return true;   // the guest repeats keys itself
        Native.nativeKey(usage, down);
        return true;
    }

    /** One character an input method produced: pressed and released on the guest's keyboard. */
    void type(char c) {
        if (!Native.nativeIsUp()) return;
        int packed = asciiUsage(c);
        if (packed == 0) return;   // not a key a US keyboard has: the guest's own input method does it
        int usage = packed & 0xff;
        boolean shift = (packed & 0x100) != 0;
        if (shift) Native.nativeKey(0xe1, true);
        Native.nativeKey(usage, true);
        Native.nativeKey(usage, false);
        if (shift) Native.nativeKey(0xe1, false);
    }

    /** A control key by name, for the on-screen board: "esc", "tab", "cmd", "left", … */
    void control(String name, boolean down) {
        int usage = controlUsage(name);
        if (usage != 0) Native.nativeKey(usage, down);
    }

    private static int controlUsage(String name) {
        switch (name) {
            case "esc": return 0x29;
            case "tab": return 0x2b;
            case "enter": return 0x28;
            case "back": return 0x2a;
            case "delete": return 0x4c;
            case "space": return 0x2c;
            case "cmd": return 0xe3;
            case "ctrl": return 0xe0;
            case "opt": return 0xe2;
            case "shift": return 0xe1;
            case "caps": return 0x39;
            case "left": return 0x50;
            case "right": return 0x4f;
            case "up": return 0x52;
            case "down": return 0x51;
            case "home": return 0x4a;
            case "end": return 0x4d;
            case "pgup": return 0x4b;
            case "pgdn": return 0x4e;
            default: return 0;
        }
    }

    /**
     * The HID usage a key event names, and its modifiers.
     *
     * The first choice is the event's own scan code: a hardware keyboard attached to Android
     * reports Linux key codes, and `devices/xhci.c` already maps those to HID usages (the table a
     * virtio keyboard driver uses). Only a key that arrives without one — an on-screen key, a
     * synthetic event — falls through to the key-code table below.
     */
    private static int usageFor(KeyEvent e) {
        int scan = e.getScanCode();
        if (scan > 0) {
            // The device maps Linux codes itself; sending it as a key event's own code keeps one
            // table in the project rather than two that can disagree.
            Native.nativeKeyLinux(scan, e.getAction() == KeyEvent.ACTION_DOWN);
            return 0;
        }
        switch (e.getKeyCode()) {
            case KeyEvent.KEYCODE_ESCAPE: return 0x29;
            case KeyEvent.KEYCODE_TAB: return 0x2b;
            case KeyEvent.KEYCODE_ENTER: return 0x28;
            case KeyEvent.KEYCODE_DEL: return 0x2a;
            case KeyEvent.KEYCODE_FORWARD_DEL: return 0x4c;
            case KeyEvent.KEYCODE_SPACE: return 0x2c;
            case KeyEvent.KEYCODE_SHIFT_LEFT: return 0xe1;
            case KeyEvent.KEYCODE_SHIFT_RIGHT: return 0xe5;
            case KeyEvent.KEYCODE_CTRL_LEFT: return 0xe0;
            case KeyEvent.KEYCODE_CTRL_RIGHT: return 0xe4;
            case KeyEvent.KEYCODE_ALT_LEFT: return 0xe2;
            case KeyEvent.KEYCODE_ALT_RIGHT: return 0xe6;
            case KeyEvent.KEYCODE_META_LEFT: return 0xe3;
            case KeyEvent.KEYCODE_META_RIGHT: return 0xe7;
            case KeyEvent.KEYCODE_CAPS_LOCK: return 0x39;
            case KeyEvent.KEYCODE_DPAD_UP: return 0x52;
            case KeyEvent.KEYCODE_DPAD_DOWN: return 0x51;
            case KeyEvent.KEYCODE_DPAD_LEFT: return 0x50;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return 0x4f;
            case KeyEvent.KEYCODE_MOVE_HOME: return 0x4a;
            case KeyEvent.KEYCODE_MOVE_END: return 0x4d;
            case KeyEvent.KEYCODE_PAGE_UP: return 0x4b;
            case KeyEvent.KEYCODE_PAGE_DOWN: return 0x4e;
            default: break;
        }
        int keyCode = e.getKeyCode();
        if (keyCode >= KeyEvent.KEYCODE_A && keyCode <= KeyEvent.KEYCODE_Z)
            return 0x04 + (keyCode - KeyEvent.KEYCODE_A);
        if (keyCode >= KeyEvent.KEYCODE_1 && keyCode <= KeyEvent.KEYCODE_9)
            return keyCode == KeyEvent.KEYCODE_1 ? 0x1e : 0x1e + (keyCode - KeyEvent.KEYCODE_1);
        if (keyCode == KeyEvent.KEYCODE_0) return 0x27;
        if (keyCode >= KeyEvent.KEYCODE_F1 && keyCode <= KeyEvent.KEYCODE_F12)
            return 0x3a + (keyCode - KeyEvent.KEYCODE_F1);
        return 0;
    }

    /**
     * A character as a US keyboard would type it: the HID usage, and bit 8 when it needs shift.
     * Zero for a character no key produces (the guest's own input method is what handles those).
     */
    static int asciiUsage(char c) {
        if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
        if (c >= 'A' && c <= 'Z') return 0x100 | (0x04 + (c - 'A'));
        if (c >= '1' && c <= '9') return 0x1e + (c - '1');
        if (c == '0') return 0x27;
        switch (c) {
            case ' ': return 0x2c;
            case '\n': return 0x28;
            case '\t': return 0x2b;
            case '-': return 0x2d;
            case '_': return 0x100 | 0x2d;
            case '=': return 0x2e;
            case '+': return 0x100 | 0x2e;
            case '[': return 0x2f;
            case '{': return 0x100 | 0x2f;
            case ']': return 0x30;
            case '}': return 0x100 | 0x30;
            case '\\': return 0x31;
            case '|': return 0x100 | 0x31;
            case ';': return 0x33;
            case ':': return 0x100 | 0x33;
            case '\'': return 0x34;
            case '"': return 0x100 | 0x34;
            case '`': return 0x35;
            case '~': return 0x100 | 0x35;
            case ',': return 0x36;
            case '<': return 0x100 | 0x36;
            case '.': return 0x37;
            case '>': return 0x100 | 0x37;
            case '/': return 0x38;
            case '?': return 0x100 | 0x38;
            case '!': return 0x100 | 0x1e;
            case '@': return 0x100 | 0x1f;
            case '#': return 0x100 | 0x20;
            case '$': return 0x100 | 0x21;
            case '%': return 0x100 | 0x22;
            case '^': return 0x100 | 0x23;
            case '&': return 0x100 | 0x24;
            case '*': return 0x100 | 0x25;
            case '(': return 0x100 | 0x26;
            case ')': return 0x100 | 0x27;
            default: return 0;
        }
    }
}
