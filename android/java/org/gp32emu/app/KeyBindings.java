package org.gp32emu.app;

import android.content.SharedPreferences;
import android.view.InputDevice;
import android.view.KeyEvent;

/** Physical-key bindings; touch controls and analog axes keep their own mapping. */
final class KeyBindings {
    static final int[] BUTTONS = {InputState.UP, InputState.DOWN, InputState.LEFT, InputState.RIGHT,
            InputState.A, InputState.B, InputState.L, InputState.R, InputState.START, InputState.SELECT};
    private static final int[][] DEFAULTS = {
        {19, 20, 21, 22, KeyEvent.KEYCODE_Z, KeyEvent.KEYCODE_X, KeyEvent.KEYCODE_A,
            KeyEvent.KEYCODE_S, KeyEvent.KEYCODE_ENTER, KeyEvent.KEYCODE_SHIFT_LEFT},
        {19, 20, 21, 22, KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B,
            KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1,
            KeyEvent.KEYCODE_BUTTON_START, KeyEvent.KEYCODE_BUTTON_SELECT}
    };
    private final int[][] keys = {DEFAULTS[0].clone(), DEFAULTS[1].clone()};

    KeyBindings() {}
    KeyBindings(KeyBindings other) {
        for (int group = 0; group < 2; ++group) keys[group] = other.keys[group].clone();
    }
    private static String preference(int group, int index) { return "binding." + group + "." + index; }
    void load(SharedPreferences prefs) {
        for (int group = 0; group < 2; ++group) {
            boolean valid = true;
            for (int i = 0; i < BUTTONS.length; ++i) {
                keys[group][i] = prefs.getInt(preference(group, i), DEFAULTS[group][i]);
                valid &= allowed(group, keys[group][i]);
                for (int j = 0; j < i; ++j) valid &= keys[group][i] != keys[group][j];
            }
            if (!valid) reset(group);
        }
    }
    void save(SharedPreferences prefs) {
        SharedPreferences.Editor editor = prefs.edit();
        for (int group = 0; group < 2; ++group)
            for (int i = 0; i < BUTTONS.length; ++i) editor.putInt(preference(group, i), keys[group][i]);
        editor.apply();
    }
    int key(int group, int index) { return keys[group][index]; }
    void reset(int group) { keys[group] = DEFAULTS[group].clone(); }
    static int normalized(int code) {
        if (code == KeyEvent.KEYCODE_SHIFT_RIGHT) return KeyEvent.KEYCODE_SHIFT_LEFT;
        if (code == KeyEvent.KEYCODE_BUTTON_L2) return KeyEvent.KEYCODE_BUTTON_L1;
        if (code == KeyEvent.KEYCODE_BUTTON_R2) return KeyEvent.KEYCODE_BUTTON_R1;
        return code;
    }
    static boolean allowed(int group, int code) {
        code = normalized(code);
        if (code >= KeyEvent.KEYCODE_DPAD_UP && code <= KeyEvent.KEYCODE_DPAD_RIGHT) return true;
        if (group == 1) return KeyEvent.isGamepadButton(code) && code != KeyEvent.KEYCODE_BUTTON_MODE;
        // System navigation, volume, media and menu keys must remain available.
        return (code >= KeyEvent.KEYCODE_0 && code <= KeyEvent.KEYCODE_9)
                || (code >= KeyEvent.KEYCODE_A && code <= KeyEvent.KEYCODE_PERIOD)
                || code == KeyEvent.KEYCODE_SHIFT_LEFT || code == KeyEvent.KEYCODE_SPACE
                || code == KeyEvent.KEYCODE_ENTER || code == KeyEvent.KEYCODE_DEL
                || (code >= KeyEvent.KEYCODE_GRAVE && code <= KeyEvent.KEYCODE_AT);
    }
    boolean assign(int group, int index, int code) {
        code = normalized(code);
        if (!allowed(group, code)) return false;
        int previous = keys[group][index];
        for (int i = 0; i < BUTTONS.length; ++i)
            if (i != index && keys[group][i] == code) keys[group][i] = previous;
        keys[group][index] = code;
        return true;
    }
    private int mapped(int group, int code) {
        for (int i = 0; i < BUTTONS.length; ++i) if (keys[group][i] == code) return BUTTONS[i];
        return 0;
    }
    int button(KeyEvent event) {
        int code = normalized(event.getKeyCode());
        int group = KeyEvent.isGamepadButton(code) || event.isFromSource(InputDevice.SOURCE_GAMEPAD)
                || event.isFromSource(InputDevice.SOURCE_JOYSTICK) ? 1 : 0;
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_UP_LEFT: return mapped(group, 19) | mapped(group, 21);
            case KeyEvent.KEYCODE_DPAD_UP_RIGHT: return mapped(group, 19) | mapped(group, 22);
            case KeyEvent.KEYCODE_DPAD_DOWN_LEFT: return mapped(group, 20) | mapped(group, 21);
            case KeyEvent.KEYCODE_DPAD_DOWN_RIGHT: return mapped(group, 20) | mapped(group, 22);
            default: return mapped(group, code);
        }
    }
    static String label(int code) { return KeyEvent.keyCodeToString(code).replace("KEYCODE_", ""); }
}
