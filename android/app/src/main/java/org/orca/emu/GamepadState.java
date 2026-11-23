package org.orca.emu;

import android.view.KeyEvent;
import android.view.MotionEvent;

/** A physical gamepad in GameCube terms (buttons as the core's PadButtons bits, axes 0..255). */
final class GamepadState {
    static final int A = 0x0100, B = 0x0200, X = 0x0400, Y = 0x0800, START = 0x1000;
    static final int LEFT = 0x0001, RIGHT = 0x0002, DOWN = 0x0004, UP = 0x0008, Z = 0x0010, R = 0x0020, L = 0x0040;

    int buttons;
    int stickX = 128, stickY = 128, cX = 128, cY = 128, trigL, trigR;
    boolean active;

    private static int axis(float v) {
        return Math.max(0, Math.min(255, Math.round(128 + v * 127)));
    }

    boolean key(int code, boolean down) {
        int bit;
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_A: bit = A; break;
            case KeyEvent.KEYCODE_BUTTON_B: bit = B; break;
            case KeyEvent.KEYCODE_BUTTON_X: bit = X; break;
            case KeyEvent.KEYCODE_BUTTON_Y: bit = Y; break;
            case KeyEvent.KEYCODE_BUTTON_START: bit = START; break;
            case KeyEvent.KEYCODE_BUTTON_SELECT:
            case KeyEvent.KEYCODE_BUTTON_R1: bit = Z; break;
            case KeyEvent.KEYCODE_BUTTON_L2: bit = L; trigL = down ? 255 : 0; break;
            case KeyEvent.KEYCODE_BUTTON_R2: bit = R; trigR = down ? 255 : 0; break;
            case KeyEvent.KEYCODE_DPAD_LEFT: bit = LEFT; break;
            case KeyEvent.KEYCODE_DPAD_RIGHT: bit = RIGHT; break;
            case KeyEvent.KEYCODE_DPAD_UP: bit = UP; break;
            case KeyEvent.KEYCODE_DPAD_DOWN: bit = DOWN; break;
            default: return false;
        }
        buttons = down ? (buttons | bit) : (buttons & ~bit);
        active = true;
        return true;
    }

    void motion(MotionEvent e) {
        stickX = axis(e.getAxisValue(MotionEvent.AXIS_X));
        stickY = axis(-e.getAxisValue(MotionEvent.AXIS_Y));
        cX = axis(e.getAxisValue(MotionEvent.AXIS_Z));
        cY = axis(-e.getAxisValue(MotionEvent.AXIS_RZ));
        float lt = Math.max(e.getAxisValue(MotionEvent.AXIS_LTRIGGER), e.getAxisValue(MotionEvent.AXIS_BRAKE));
        float rt = Math.max(e.getAxisValue(MotionEvent.AXIS_RTRIGGER), e.getAxisValue(MotionEvent.AXIS_GAS));
        trigL = Math.round(lt * 255);
        trigR = Math.round(rt * 255);
        buttons = trigL > 200 ? buttons | L : buttons & ~L;
        buttons = trigR > 200 ? buttons | R : buttons & ~R;
        float hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        buttons = hx < -0.5f ? buttons | LEFT : buttons & ~LEFT;
        buttons = hx > 0.5f ? buttons | RIGHT : buttons & ~RIGHT;
        buttons = hy < -0.5f ? buttons | UP : buttons & ~UP;
        buttons = hy > 0.5f ? buttons | DOWN : buttons & ~DOWN;
        active = true;
    }
}
