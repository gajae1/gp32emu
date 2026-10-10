package org.gp32emu.app;

import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;

import java.util.HashMap;
import java.util.Map;

/** Combines independent input sources into a libretro joypad bit mask. */
public final class InputState {
    public static final int A = 1 << 8;
    public static final int B = 1 << 0;
    public static final int L = 1 << 10;
    public static final int R = 1 << 11;
    public static final int START = 1 << 3;
    public static final int SELECT = 1 << 2;
    public static final int UP = 1 << 4;
    public static final int DOWN = 1 << 5;
    public static final int LEFT = 1 << 6;
    public static final int RIGHT = 1 << 7;

    private static final int BUTTONS = A | B | L | R | START | SELECT
            | UP | DOWN | LEFT | RIGHT;
    private static final float DEAD_ZONE = 0.20f;

    private int touch;
    private final Map<Long, Integer> keys = new HashMap<>();
    private final Map<Integer, Integer> axes = new HashMap<>();

    public synchronized int mask() {
        int result = touch;
        for (int held : keys.values()) result |= held;
        for (int held : axes.values()) result |= held;
        return result;
    }

    public synchronized void setTouch(int mask) {
        touch = mask & BUTTONS;
    }

    /** True while Start and Select are both held on physical keys. */
    public synchronized boolean menuChord() {
        int held = 0;
        for (int mask : keys.values()) held |= mask;
        return (held & (START | SELECT)) == (START | SELECT);
    }

    /** Releases every source, for example when the Activity loses focus. */
    public synchronized void clear() {
        touch = 0;
        keys.clear();
        axes.clear();
    }

    public synchronized boolean key(KeyEvent e) {
        int button = buttonForKey(e.getKeyCode());
        if (button == 0) return false;
        long identity = ((long) e.getDeviceId() << 32) | (e.getKeyCode() & 0xffffffffL);
        switch (e.getAction()) {
            case KeyEvent.ACTION_DOWN:
                keys.put(identity, button); // Repeats replace this key, never add a hold.
                return true;
            case KeyEvent.ACTION_UP:
                keys.remove(identity);
                return true;
            default:
                return false;
        }
    }

    public synchronized boolean motion(MotionEvent e) {
        if (!e.isFromSource(InputDevice.SOURCE_JOYSTICK)
                && !e.isFromSource(InputDevice.SOURCE_GAMEPAD)
                && !e.isFromSource(InputDevice.SOURCE_DPAD)) return false;
        if (e.getActionMasked() == MotionEvent.ACTION_CANCEL) {
            axes.remove(e.getDeviceId());
            return true;
        }
        if (e.getActionMasked() != MotionEvent.ACTION_MOVE) return false;

        InputDevice device = e.getDevice();
        // A neutral hat must not release a held stick (or vice versa).
        int held = axis(e, device, MotionEvent.AXIS_X, LEFT, RIGHT)
                | axis(e, device, MotionEvent.AXIS_Y, UP, DOWN)
                | axis(e, device, MotionEvent.AXIS_HAT_X, LEFT, RIGHT)
                | axis(e, device, MotionEvent.AXIS_HAT_Y, UP, DOWN);
        if (held == 0) axes.remove(e.getDeviceId());
        else axes.put(e.getDeviceId(), held);
        return true;
    }

    private static int axis(MotionEvent event, InputDevice device, int axis,
                            int negative, int positive) {
        float deadZone = DEAD_ZONE;
        if (device != null) {
            InputDevice.MotionRange range = device.getMotionRange(axis, event.getSource());
            if (range != null) deadZone = Math.max(deadZone, range.getFlat());
        }
        float value = event.getAxisValue(axis);
        if (value < -deadZone) return negative;
        if (value > deadZone) return positive;
        return 0;
    }

    private static int buttonForKey(int code) {
        switch (code) {
            case KeyEvent.KEYCODE_Z:
            case KeyEvent.KEYCODE_BUTTON_A: return A;
            case KeyEvent.KEYCODE_X:
            case KeyEvent.KEYCODE_BUTTON_B: return B;
            case KeyEvent.KEYCODE_A:
            case KeyEvent.KEYCODE_BUTTON_L1:
            case KeyEvent.KEYCODE_BUTTON_L2: return L;
            case KeyEvent.KEYCODE_S:
            case KeyEvent.KEYCODE_BUTTON_R1:
            case KeyEvent.KEYCODE_BUTTON_R2: return R;
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_BUTTON_START: return START;
            case KeyEvent.KEYCODE_SHIFT_LEFT:
            case KeyEvent.KEYCODE_SHIFT_RIGHT:
            case KeyEvent.KEYCODE_BUTTON_SELECT: return SELECT;
            case KeyEvent.KEYCODE_DPAD_UP: return UP;
            case KeyEvent.KEYCODE_DPAD_DOWN: return DOWN;
            case KeyEvent.KEYCODE_DPAD_LEFT: return LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT: return RIGHT;
            case KeyEvent.KEYCODE_DPAD_UP_LEFT: return UP | LEFT;
            case KeyEvent.KEYCODE_DPAD_UP_RIGHT: return UP | RIGHT;
            case KeyEvent.KEYCODE_DPAD_DOWN_LEFT: return DOWN | LEFT;
            case KeyEvent.KEYCODE_DPAD_DOWN_RIGHT: return DOWN | RIGHT;
            default: return 0;
        }
    }
}
