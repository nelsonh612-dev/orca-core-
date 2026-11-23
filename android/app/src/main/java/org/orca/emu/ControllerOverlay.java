package org.orca.emu;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/**
 * Touch controls drawn over the picture. GameCube: A B X Y Start Z L R, d-pad, main stick and
 * C stick. Wii: Wii Remote buttons (A B 1 2 + - Home, d-pad), the pointer (touch the free area of
 * the screen), and a Nunchuk (stick, C, Z) that can be plugged in or out with "N".
 */
final class ControllerOverlay extends View {
    // Wii Remote button bits (core: orca/wiimote/wiimote.hpp)
    private static final int W_LEFT = 0x0100, W_RIGHT = 0x0200, W_DOWN = 0x0400, W_UP = 0x0800, W_PLUS = 0x1000;
    private static final int W_TWO = 0x0001, W_ONE = 0x0002, W_B = 0x0004, W_A = 0x0008, W_MINUS = 0x0010, W_HOME = 0x0080;

    private static final class Button {
        final String label;
        final int bit;      // pad/remote bit, or special codes below
        final float fx, fy; // centre as a fraction of the view
        float x, y, r;
        boolean down;
        Button(String label, int bit, float fx, float fy) {
            this.label = label;
            this.bit = bit;
            this.fx = fx;
            this.fy = fy;
        }
    }
    private static final int TRIGGER_L = -1, TRIGGER_R = -2, NUNCHUK_C = -3, NUNCHUK_Z = -4, TOGGLE_NUNCHUK = -5;

    private final long handle;
    private final boolean wii;
    private final List<Button> buttons = new ArrayList<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    // sticks: centre fraction; current deflection -1..1
    private float stickX, stickY, cX, cY;
    private int stickPointer = -1, cPointer = -1, aimPointer = -1;
    private float pointerX = 0f, pointerY = 0f;
    private boolean nunchuk = true;
    private GamepadState physical;

    ControllerOverlay(Context context, long handle, boolean wii) {
        super(context);
        this.handle = handle;
        this.wii = wii;
        fill.setColor(0x55FFFFFF);
        text.setColor(0xCCFFFFFF);
        text.setTextAlign(Paint.Align.CENTER);
        if (wii) {
            buttons.add(new Button("A", W_A, 0.88f, 0.55f));
            buttons.add(new Button("B", W_B, 0.78f, 0.70f));
            buttons.add(new Button("1", W_ONE, 0.82f, 0.88f));
            buttons.add(new Button("2", W_TWO, 0.93f, 0.88f));
            buttons.add(new Button("+", W_PLUS, 0.62f, 0.92f));
            buttons.add(new Button("-", W_MINUS, 0.38f, 0.92f));
            buttons.add(new Button("H", W_HOME, 0.50f, 0.92f));
            buttons.add(new Button("C", NUNCHUK_C, 0.20f, 0.45f));
            buttons.add(new Button("Z", NUNCHUK_Z, 0.08f, 0.45f));
            buttons.add(new Button("N", TOGGLE_NUNCHUK, 0.04f, 0.08f));
        } else {
            buttons.add(new Button("A", GamepadState.A, 0.88f, 0.62f));
            buttons.add(new Button("B", GamepadState.B, 0.78f, 0.76f));
            buttons.add(new Button("X", GamepadState.X, 0.95f, 0.48f));
            buttons.add(new Button("Y", GamepadState.Y, 0.82f, 0.45f));
            buttons.add(new Button("S", GamepadState.START, 0.50f, 0.92f));
            buttons.add(new Button("Z", GamepadState.Z, 0.92f, 0.28f));
            buttons.add(new Button("L", TRIGGER_L, 0.06f, 0.12f));
            buttons.add(new Button("R", TRIGGER_R, 0.94f, 0.12f));
        }
        buttons.add(new Button("<", wii ? W_LEFT : GamepadState.LEFT, 0.22f, 0.80f));
        buttons.add(new Button(">", wii ? W_RIGHT : GamepadState.RIGHT, 0.32f, 0.80f));
        buttons.add(new Button("^", wii ? W_UP : GamepadState.UP, 0.27f, 0.70f));
        buttons.add(new Button("v", wii ? W_DOWN : GamepadState.DOWN, 0.27f, 0.90f));
    }

    void setPhysical(GamepadState p) {
        physical = p;
        send();
    }

    private float stickRadius() { return getHeight() * 0.11f; }
    private float stickCx() { return getWidth() * 0.12f; }
    private float stickCy() { return getHeight() * 0.62f; }
    private float cCx() { return getWidth() * 0.66f; }
    private float cCy() { return getHeight() * 0.70f; }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        for (Button b : buttons) {
            b.x = b.fx * w;
            b.y = b.fy * h;
            b.r = h * 0.06f;
        }
        text.setTextSize(h * 0.05f);
    }

    @Override
    protected void onDraw(Canvas c) {
        for (Button b : buttons) {
            fill.setAlpha(b.down ? 0xAA : 0x55);
            c.drawCircle(b.x, b.y, b.r, fill);
            c.drawText(b.label, b.x, b.y + text.getTextSize() / 3, text);
        }
        fill.setAlpha(0x33);
        if (!wii || nunchuk) {
            c.drawCircle(stickCx(), stickCy(), stickRadius(), fill);
            c.drawCircle(stickCx() + stickX * stickRadius(), stickCy() - stickY * stickRadius(), stickRadius() * 0.4f, fill);
        }
        if (!wii) {
            c.drawCircle(cCx(), cCy(), stickRadius() * 0.8f, fill);
            c.drawCircle(cCx() + cX * stickRadius() * 0.8f, cCy() - cY * stickRadius() * 0.8f, stickRadius() * 0.3f, fill);
        } else {
            fill.setAlpha(0x88);
            c.drawCircle((pointerX + 1f) / 2f * getWidth(), (1f - pointerY) / 2f * getHeight(), 10f, fill);
        }
    }

    private Button hit(float x, float y) {
        for (Button b : buttons) {
            float dx = x - b.x, dy = y - b.y;
            if (dx * dx + dy * dy <= b.r * b.r * 1.4f) return b;
        }
        return null;
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        for (Button b : buttons) b.down = false;
        final int action = e.getActionMasked();
        final int lifted = (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP
                || action == MotionEvent.ACTION_CANCEL) ? e.getActionIndex() : -1;
        boolean stickSeen = false, cSeen = false;
        for (int i = 0; i < e.getPointerCount(); ++i) {
            if (i == lifted) {
                int id = e.getPointerId(i);
                if (id == stickPointer) stickPointer = -1;
                if (id == cPointer) cPointer = -1;
                if (id == aimPointer) aimPointer = -1;
                continue;
            }
            final int id = e.getPointerId(i);
            final float x = e.getX(i), y = e.getY(i);
            Button b = hit(x, y);
            if (b != null && id != stickPointer && id != cPointer && id != aimPointer) {
                if (b.bit == TOGGLE_NUNCHUK) {
                    if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN && i == e.getActionIndex())
                        nunchuk = !nunchuk;
                } else {
                    b.down = true;
                }
                continue;
            }
            float r = stickRadius();
            boolean onStick = Math.hypot(x - stickCx(), y - stickCy()) < r * 1.6f && (!wii || nunchuk);
            boolean onC = !wii && Math.hypot(x - cCx(), y - cCy()) < r * 1.4f;
            if (id == stickPointer || (stickPointer < 0 && onStick)) {
                stickPointer = id;
                stickSeen = true;
                stickX = clamp((x - stickCx()) / r);
                stickY = clamp(-(y - stickCy()) / r);
            } else if (id == cPointer || (cPointer < 0 && onC)) {
                cPointer = id;
                cSeen = true;
                cX = clamp((x - cCx()) / (r * 0.8f));
                cY = clamp(-(y - cCy()) / (r * 0.8f));
            } else if (wii) {
                aimPointer = id;
                pointerX = clamp(x / getWidth() * 2f - 1f);
                pointerY = clamp(1f - y / getHeight() * 2f);
            }
        }
        if (!stickSeen) { stickX = 0; stickY = 0; stickPointer = -1; }
        if (!cSeen) { cX = 0; cY = 0; cPointer = -1; }
        send();
        invalidate();
        return true;
    }

    private static float clamp(float v) { return Math.max(-1f, Math.min(1f, v)); }
    private static int axis(float v) { return Math.max(0, Math.min(255, Math.round(128 + v * 127))); }

    private void send() {
        int bits = 0;
        boolean l = false, r = false, nc = false, nz = false;
        for (Button b : buttons) {
            if (!b.down) continue;
            if (b.bit == TRIGGER_L) l = true;
            else if (b.bit == TRIGGER_R) r = true;
            else if (b.bit == NUNCHUK_C) nc = true;
            else if (b.bit == NUNCHUK_Z) nz = true;
            else if (b.bit > 0) bits |= b.bit;
        }
        GamepadState p = physical;
        if (!wii) {
            int sx = axis(stickX), sy = axis(stickY), cx = axis(cX), cy = axis(cY);
            int tl = l ? 255 : 0, tr = r ? 255 : 0;
            if (l) bits |= GamepadState.L;
            if (r) bits |= GamepadState.R;
            if (p != null && p.active) {
                bits |= p.buttons;
                if (stickPointer < 0) { sx = p.stickX; sy = p.stickY; }
                if (cPointer < 0) { cx = p.cX; cy = p.cY; }
                tl = Math.max(tl, p.trigL);
                tr = Math.max(tr, p.trigR);
            }
            NativeLib.setPad(handle, 0, bits, sx, sy, cx, cy, tl, tr);
        } else {
            int sx = axis(stickX), sy = axis(stickY);
            if (p != null && p.active) {
                int pb = p.buttons;
                if ((pb & GamepadState.A) != 0) bits |= W_A;
                if ((pb & GamepadState.B) != 0) bits |= W_B;
                if ((pb & GamepadState.X) != 0) bits |= W_ONE;
                if ((pb & GamepadState.Y) != 0) bits |= W_TWO;
                if ((pb & GamepadState.START) != 0) bits |= W_PLUS;
                if ((pb & GamepadState.Z) != 0) bits |= W_MINUS;
                if ((pb & GamepadState.LEFT) != 0) bits |= W_LEFT;
                if ((pb & GamepadState.RIGHT) != 0) bits |= W_RIGHT;
                if ((pb & GamepadState.UP) != 0) bits |= W_UP;
                if ((pb & GamepadState.DOWN) != 0) bits |= W_DOWN;
                nz |= (pb & GamepadState.L) != 0;
                nc |= (pb & GamepadState.R) != 0;
                if (stickPointer < 0) { sx = p.stickX; sy = p.stickY; }
            }
            // remote held level: 1 g on Z (10-bit values, 0x200 = 0 g)
            NativeLib.setRemote(handle, 0, bits, pointerX, pointerY, 0x200, 0x200, 0x268, nunchuk, sx, sy, nc, nz);
        }
    }
}
