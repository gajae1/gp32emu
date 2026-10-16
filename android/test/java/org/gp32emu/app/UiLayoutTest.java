package org.gp32emu.app;

import android.app.Activity;
import android.app.AlertDialog;
import android.app.Instrumentation;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.res.Configuration;
import android.graphics.Bitmap;
import android.graphics.Rect;
import android.graphics.RectF;
import android.os.Bundle;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import java.io.File;
import java.io.FileOutputStream;
import java.lang.reflect.Field;
import java.util.List;

/** Checks real sibling hit testing, pointer splitting and resumed input. */
public final class UiLayoutTest extends Instrumentation {
    private MainActivity activity;
    private Bundle args;
    private int checks;
    private long downTime;

    @Override public void onCreate(Bundle args) { super.onCreate(args); this.args = args; start(); }
    private void check(boolean ok, String why) { if (!ok) throw new AssertionError(why); ++checks; }
    private Object field(String name) {
        try { Field f = MainActivity.class.getDeclaredField(name); f.setAccessible(true); return f.get(activity); }
        catch (ReflectiveOperationException e) { throw new AssertionError(e); }
    }
    private void ui(Runnable task) {
        Throwable[] failure = {null};
        runOnMainSync(() -> { try { task.run(); } catch (Throwable t) { failure[0] = t; } });
        if (failure[0] != null) throw new AssertionError(failure[0]);
    }
    private Rect bounds(View view) {
        Rect rect = new Rect(); check(view.getGlobalVisibleRect(rect), "view is visible"); return rect;
    }
    private boolean menuShowing() { AlertDialog menu = (AlertDialog)field("menu"); return menu != null && menu.isShowing(); }
    private float[] padPoint(int button) {
        RectF r = activity.touchPad.boundsFor(button);
        int[] pad = new int[2], root = new int[2];
        activity.touchPad.getLocationOnScreen(pad); ((View)field("play")).getLocationOnScreen(root);
        return new float[]{pad[0] - root[0] + r.centerX(), pad[1] - root[1] + r.centerY()};
    }
    private float[] menuPoint() {
        Rect r = bounds((View)field("menuButton")); int[] root = new int[2];
        ((View)field("play")).getLocationOnScreen(root);
        return new float[]{r.exactCenterX() - root[0], r.exactCenterY() - root[1]};
    }
    private void pointer(int action, float[]... points) {
        if (action == MotionEvent.ACTION_DOWN) downTime = SystemClock.uptimeMillis();
        MotionEvent.PointerProperties[] properties = new MotionEvent.PointerProperties[points.length];
        MotionEvent.PointerCoords[] coords = new MotionEvent.PointerCoords[points.length];
        for (int i = 0; i < points.length; ++i) {
            properties[i] = new MotionEvent.PointerProperties(); properties[i].id = i;
            properties[i].toolType = MotionEvent.TOOL_TYPE_FINGER;
            coords[i] = new MotionEvent.PointerCoords(); coords[i].x = points[i][0]; coords[i].y = points[i][1];
            coords[i].pressure = 1; coords[i].size = 1;
        }
        MotionEvent event = MotionEvent.obtain(downTime, SystemClock.uptimeMillis(), action,
                points.length, properties, coords, 0, 0, 1, 1, 0, 0, InputDevice.SOURCE_TOUCHSCREEN, 0);
        ((ViewGroup)field("play")).dispatchTouchEvent(event); event.recycle();
    }
    private void rotate(boolean landscape) {
        ui(() -> activity.setRequestedOrientation(landscape ? ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
                : ActivityInfo.SCREEN_ORIENTATION_PORTRAIT));
        long end = SystemClock.uptimeMillis() + 5000;
        boolean[] ready = {false};
        do {
            waitForIdleSync();
            ui(() -> {
                View play = (View)field("play");
                ready[0] = (activity.getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE) == landscape
                        && (play.getWidth() > play.getHeight()) == landscape && !play.isLayoutRequested();
            });
            if (!ready[0]) SystemClock.sleep(25);
        } while (!ready[0] && SystemClock.uptimeMillis() < end);
        check(ready[0], "rotation and layout settled (landscape=" + landscape
                + ", orientation=" + activity.getResources().getConfiguration().orientation
                + ", play=" + ((View)field("play")).getWidth() + "x" + ((View)field("play")).getHeight() + ")");
        ui(() -> check(activity.input.mask() == 0, "rotation releases inputs"));
    }
    private void separation() {
        Rect menu = bounds((View)field("menuButton")), picture = bounds((View)field("screen"));
        View playView = (View)field("play");
        Rect pad = bounds(activity.touchPad), play = bounds(playView);
        play.left += playView.getPaddingLeft(); play.top += playView.getPaddingTop();
        play.right -= playView.getPaddingRight(); play.bottom -= playView.getPaddingBottom();
        check(play.contains(menu) && play.contains(picture) && play.contains(pad), "menu, picture and controls respect safe insets");
        check(!Rect.intersects(menu, picture) && !Rect.intersects(menu, pad), "menu is outside picture and controls");
        int gap = Math.max(Math.max(pad.left - menu.right, menu.left - pad.right),
                Math.max(pad.top - menu.bottom, menu.top - pad.bottom));
        float density = activity.getResources().getDisplayMetrics().density;
        check(gap >= Math.round(8 * density) && menu.width() >= Math.round(48 * density)
                && menu.height() >= Math.round(48 * density), "separate accessible menu target");
    }
    private void gestures() {
        ui(() -> {
            separation();
            float[] left = padPoint(InputState.LEFT), menu = menuPoint();
            pointer(MotionEvent.ACTION_DOWN, left);
            check(activity.input.mask() == InputState.LEFT, "root routes direction to pad");
            pointer(MotionEvent.ACTION_MOVE, menu);
            pointer(MotionEvent.ACTION_UP, menu);
            check(activity.input.mask() == 0 && !menuShowing(), "drag to menu releases direction without opening menu");
            pointer(MotionEvent.ACTION_DOWN, left);
            pointer(MotionEvent.ACTION_POINTER_DOWN | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT), left, menu);
            pointer(MotionEvent.ACTION_POINTER_UP | (1 << MotionEvent.ACTION_POINTER_INDEX_SHIFT), left, menu);
            check(activity.input.mask() == InputState.LEFT && !menuShowing(), "second finger on menu preserves held direction");
            pointer(MotionEvent.ACTION_UP, left);
            pointer(MotionEvent.ACTION_DOWN, left);
            pointer(MotionEvent.ACTION_MOVE, menu);
            pointer(MotionEvent.ACTION_CANCEL, menu);
            check(activity.input.mask() == 0 && !menuShowing(), "cancel releases gesture without pausing");
            if (activity.getResources().getConfiguration().orientation == Configuration.ORIENTATION_LANDSCAPE) {
                // Exercise the empty rail just outside the visible button.
                menu[0] -= ((View)field("menuButton")).getWidth() / 2f
                        + 4 * activity.getResources().getDisplayMetrics().density;
            }
            pointer(MotionEvent.ACTION_DOWN, menu);
            pointer(MotionEvent.ACTION_UP, menu);
        });
        waitForIdleSync();
        ui(() -> {
            check(menuShowing() && (Boolean)field("uiHold") && activity.input.mask() == 0, "intentional menu tap pauses and clears input");
            AlertDialog dialog = (AlertDialog)field("menu");
            dialog.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_MODE));
        });
        waitForIdleSync();
        ui(() -> {
            check(!menuShowing() && !(Boolean)field("uiHold"), "gamepad menu key resumes game");
            activity.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_Z));
            activity.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BUTTON_A));
            activity.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_Z));
            check(activity.input.mask() == InputState.A, "resumed keyboard release preserves gamepad input");
            activity.dispatchKeyEvent(new KeyEvent(KeyEvent.ACTION_UP, KeyEvent.KEYCODE_BUTTON_A));
            check(activity.input.mask() == 0, "resumed gamepad releases");
        });
    }
    private void capture(String name, boolean landscape) throws Exception {
        SystemClock.sleep(1000); // Include the compositor's rotation animation, not only View layout.
        Bitmap bitmap = getUiAutomation().takeScreenshot();
        check(bitmap != null && (bitmap.getWidth() > bitmap.getHeight()) == landscape, "screenshot orientation");
        try (FileOutputStream out = new FileOutputStream(new File(getTargetContext().getFilesDir(), name + ".png"))) {
            bitmap.compress(Bitmap.CompressFormat.PNG, 100, out);
        }
        bitmap.recycle();
    }
    @Override public void onStart() {
        Bundle result = new Bundle();
        try {
            activity = (MainActivity)startActivitySync(new Intent(getTargetContext(), MainActivity.class).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
            waitForIdleSync();
            ui(() -> {
                List<?> games = (List<?>)field("games"); check(!games.isEmpty(), "private test game is installed");
                activity.launch((File)games.get(0)); activity.setTouchEnabled(true); activity.touchPad.setHaptics(false);
            });
            long end = SystemClock.uptimeMillis() + 10000;
            while (!(Boolean)field("gameLoaded") && SystemClock.uptimeMillis() < end) SystemClock.sleep(25);
            check((Boolean)field("gameLoaded"), "game loads through JNI");
            boolean separate = !"false".equals(args.getString("separate"));
            String prefix = args.getString("prefix", "layout");
            rotate(false); SystemClock.sleep(1000); capture(prefix + "-portrait", false);
            if (separate) gestures();
            ui(() -> pointer(MotionEvent.ACTION_DOWN, padPoint(InputState.A)));
            rotate(true); capture(prefix + "-landscape", true);
            if (separate) gestures();
            ui(() -> activity.setTouchEnabled(false)); waitForIdleSync();
            if (separate) ui(() -> check(!Rect.intersects(bounds((View)field("menuButton")), bounds((View)field("screen"))), "hidden pad retains separate menu"));
            capture(prefix + "-gamepad", true);
            ui(() -> activity.setTouchEnabled(true));
            rotate(false); if (separate) gestures();
            result.putString("stream", "PASS: " + checks + " real layout/pointer/menu checks\n");
            finish(Activity.RESULT_OK, result);
        } catch (Throwable error) {
            result.putString("stream", "FAIL: " + error + "\n"); finish(Activity.RESULT_CANCELED, result);
        }
    }
}
