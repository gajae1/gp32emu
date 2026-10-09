package org.gp32emu.app;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.graphics.Bitmap;
import android.graphics.RectF;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import java.io.File;
import java.io.FileOutputStream;

/** Real framework input/lifecycle checks; no test-only components in the app APK. */
public final class InputTest extends Instrumentation {
    private MainActivity activity;
    private int checks;
    @Override public void onCreate(Bundle args) { super.onCreate(args); start(); }
    private void check(boolean condition, String why) { if (!condition) throw new AssertionError(why); ++checks; }
    private void ui(Runnable task) {
        final Throwable[] error = new Throwable[1];
        runOnMainSync(() -> { try { task.run(); } catch (Throwable t) { error[0] = t; } });
        if (error[0] != null) throw new AssertionError(error[0]);
    }
    private void touch(int action, int... masks) {
        MotionEvent.PointerProperties[] props = new MotionEvent.PointerProperties[masks.length];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[masks.length];
        for (int i = 0; i < masks.length; ++i) {
            RectF r = activity.touchPad.boundsFor(masks[i]);
            props[i] = new MotionEvent.PointerProperties(); props[i].id = i; props[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords(); coords[i].x = r.centerX(); coords[i].y = r.centerY();
            coords[i].pressure = 1; coords[i].size = 1;
        }
        long now = SystemClock.uptimeMillis();
        MotionEvent e = MotionEvent.obtain(now, now, action, masks.length, props, coords, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        activity.touchPad.dispatchTouchEvent(e); e.recycle();
    }
    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            Intent launch = getTargetContext().getPackageManager().getLaunchIntentForPackage("org.gp32emu.app");
            launch.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
            activity = (MainActivity)startActivitySync(launch);
            waitForIdleSync();
            if (activity.getPreferences(0).contains("game")) {
                java.lang.reflect.Field loaded = MainActivity.class.getDeclaredField("gameLoaded");
                loaded.setAccessible(true);
                long deadline = SystemClock.uptimeMillis() + 10000;
                while (!loaded.getBoolean(activity) && SystemClock.uptimeMillis() < deadline) SystemClock.sleep(25);
                check(loaded.getBoolean(activity), "imported game loads through JNI");
            }
            ui(() -> activity.touchSwitch.setChecked(true));
            long layoutDeadline = SystemClock.uptimeMillis() + 3000;
            final boolean[] laidOut = {false};
            do {
                waitForIdleSync();
                ui(() -> laidOut[0] = !activity.touchPad.boundsFor(InputState.A).isEmpty());
                if (!laidOut[0]) SystemClock.sleep(16);
            } while (!laidOut[0] && SystemClock.uptimeMillis() < layoutDeadline);
            check(laidOut[0], "controls must have completed layout before touches");
            ui(() -> {
                check(activity.touchPad.getVisibility() == View.VISIBLE, "Touch ON must show controls");
                touch(MotionEvent.ACTION_DOWN, InputState.LEFT);
                touch(MotionEvent.ACTION_POINTER_DOWN | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT), InputState.LEFT, InputState.A);
                check(activity.input.mask() == (InputState.LEFT | InputState.A), "two fingers must hold direction and A");
                touch(MotionEvent.ACTION_POINTER_UP | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT), InputState.LEFT, InputState.A);
                check(activity.input.mask() == InputState.LEFT, "lifting A must preserve left");
                touch(MotionEvent.ACTION_UP, InputState.LEFT);
                check(activity.input.mask() == 0, "last finger up must clear input");
                for (int key : new int[]{InputState.UP, InputState.DOWN, InputState.RIGHT, InputState.B, InputState.L, InputState.R, InputState.START, InputState.SELECT}) {
                    touch(MotionEvent.ACTION_DOWN, key);
                    check(activity.input.mask() == key, "button mapping " + key);
                    touch(MotionEvent.ACTION_CANCEL, key);
                    check(activity.input.mask() == 0, "cancel mapping " + key);
                }
                activity.input.key(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_Z));
                touch(MotionEvent.ACTION_DOWN, InputState.B);
                activity.touchSwitch.setChecked(false);
                check(activity.touchPad.getVisibility() == View.GONE, "Touch OFF must hide controls");
                check(activity.input.mask() == InputState.A, "Touch OFF must preserve keyboard input");
                activity.input.key(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_Z));
                check(activity.input.mask() == 0, "keyboard release");
                activity.input.key(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_A));
                activity.input.key(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_Z));
                activity.input.key(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_Z));
                check(activity.input.mask() == InputState.A, "keyboard release must preserve gamepad A");
                activity.input.key(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_A));
                check(activity.input.mask() == 0, "gamepad release");
                activity.touchSwitch.setChecked(true);
                touch(MotionEvent.ACTION_DOWN, InputState.L);
                activity.onWindowFocusChanged(false);
                check(activity.input.mask() == 0, "focus loss must clear input");
                activity.onWindowFocusChanged(true);
                touch(MotionEvent.ACTION_DOWN, InputState.A);
                activity.setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE);
            });
            long until = SystemClock.uptimeMillis() + 5000;
            while (activity.getResources().getConfiguration().orientation != android.content.res.Configuration.ORIENTATION_LANDSCAPE && SystemClock.uptimeMillis() < until) SystemClock.sleep(25);
            waitForIdleSync();
            ui(() -> {
                check(activity.getResources().getConfiguration().orientation == android.content.res.Configuration.ORIENTATION_LANDSCAPE, "rotation completed");
                check(activity.input.mask() == 0, "rotation must release held touch");
                touch(MotionEvent.ACTION_DOWN, InputState.RIGHT);
                callActivityOnPause(activity);
                check(activity.input.mask() == 0, "pause must clear input");
                callActivityOnResume(activity);
            });
            Bitmap screen = getUiAutomation().takeScreenshot();
            check(screen != null, "window screenshot available");
            try (FileOutputStream out = new FileOutputStream(new File(getTargetContext().getFilesDir(), "input-test.png"))) {
                screen.compress(Bitmap.CompressFormat.PNG, 100, out);
            }
            screen.recycle();
            result.putString("stream", "PASS: " + checks + " Android input/rotation/pause checks\n");
            finish(Activity.RESULT_OK, result);
        } catch (Throwable t) {
            result.putString("stream", "FAIL: " + t + "\n");
            finish(Activity.RESULT_CANCELED, result);
        }
    }
}
