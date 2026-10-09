package org.gp32emu.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.TypedValue;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewParent;

/** Transparent, full-size FrameLayout overlay; the game surface stays separate. */
public final class TouchPadView extends View {
    private static final int[] BUTTONS = {
            InputState.UP, InputState.DOWN, InputState.LEFT, InputState.RIGHT,
            InputState.A, InputState.B, InputState.L, InputState.R,
            InputState.START, InputState.SELECT
    };
    private static final String[] LABELS = {
            "\u2191", "\u2193", "\u2190", "\u2192", "A", "B", "L", "R", "Start", "Select"
    };

    private final InputState input;
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF[] bounds = new RectF[BUTTONS.length];
    private final RectF dpad = new RectF();
    private final float density;
    private final float labelTextSize;
    private final float buttonTextSize;
    private int touchMask;
    private boolean tracking;

    public TouchPadView(Context context, InputState input) {
        super(context);
        if (input == null) throw new IllegalArgumentException("input must not be null");
        this.input = input;
        density = getResources().getDisplayMetrics().density;
        labelTextSize = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 14,
                getResources().getDisplayMetrics());
        buttonTextSize = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 20,
                getResources().getDisplayMetrics());
        for (int i = 0; i < bounds.length; i++) bounds[i] = new RectF();
        setFocusable(false);
        setClickable(true);
        setContentDescription("GP32 방향 패드, A, B, L, R, Start, Select");
    }

    /** Releases this overlay only; physical keys and sticks remain held. */
    public void release() {
        boolean wasTracking = tracking;
        tracking = false;
        touchMask = 0;
        // View can dispatch visibility callbacks from its own constructor.
        if (input != null) input.setTouch(0);
        if (wasTracking) {
            ViewParent parent = getParent();
            if (parent != null) parent.requestDisallowInterceptTouchEvent(false);
        }
        invalidate();
    }

    /** Returns a defensive copy in local view coordinates, or an empty rectangle. */
    RectF boundsFor(int button) {
        for (int i = 0; i < BUTTONS.length; i++) {
            if (BUTTONS[i] == button) return new RectF(bounds[i]);
        }
        return new RectF();
    }

    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        release();
        layoutControls(w, h);
    }

    private void layoutControls(int width, int height) {
        for (RectF rect : bounds) rect.setEmpty();
        dpad.setEmpty();
        float margin = 8 * density;
        float gap = 8 * density;
        float availableWidth = width - getPaddingLeft() - getPaddingRight() - 2 * margin;
        float availableHeight = Math.min(240 * density,
                height - getPaddingTop() - getPaddingBottom()) - 2 * margin;
        float preferred = Math.min(56 * density, Math.max(44 * density, availableWidth / 8));
        float unit = Math.min(preferred,
                Math.min((availableWidth - gap) / 6, (availableHeight - gap) / 4));
        if (unit <= 0) return;

        float left = getPaddingLeft() + margin;
        float right = width - getPaddingRight() - margin;
        float bottom = height - getPaddingBottom() - margin;
        dpad.set(left, bottom - 3 * unit, left + 3 * unit, bottom);
        bounds[0].set(left + unit, dpad.top, left + 2 * unit, dpad.top + unit);
        bounds[1].set(left + unit, bottom - unit, left + 2 * unit, bottom);
        bounds[2].set(left, dpad.top + unit, left + unit, bottom - unit);
        bounds[3].set(left + 2 * unit, dpad.top + unit, left + 3 * unit, bottom - unit);

        float actionSize = 1.35f * unit;
        bounds[4].set(right - actionSize, dpad.top, right, dpad.top + actionSize);
        bounds[5].set(right - 3 * unit, bottom - actionSize,
                right - 3 * unit + actionSize, bottom);

        float rowTop = dpad.top - gap - unit;
        float rowBottom = rowTop + unit;
        float center = (left + right) / 2;
        bounds[6].set(left, rowTop, left + 1.2f * unit, rowBottom);
        bounds[7].set(right - 1.2f * unit, rowTop, right, rowBottom);
        bounds[8].set(center + gap / 2, rowTop, center + gap / 2 + 1.5f * unit, rowBottom);
        bounds[9].set(center - gap / 2 - 1.5f * unit, rowTop, center - gap / 2, rowBottom);
    }

    @Override protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (dpad.isEmpty()) return;
        paint.setStyle(Paint.Style.FILL);
        paint.setColor(Color.argb(65, 30, 38, 50));
        canvas.drawRoundRect(dpad, 12 * density, 12 * density, paint);
        for (int i = 0; i < BUTTONS.length; i++) {
            RectF rect = bounds[i];
            boolean pressed = (touchMask & BUTTONS[i]) != 0;
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(pressed ? Color.argb(210, 45, 135, 205) : Color.argb(150, 26, 34, 46));
            drawButton(canvas, rect, i == 4 || i == 5);
            paint.setStyle(Paint.Style.STROKE);
            paint.setStrokeWidth(density);
            paint.setColor(Color.argb(170, 220, 230, 240));
            drawButton(canvas, rect, i == 4 || i == 5);
            paint.setStyle(Paint.Style.FILL);
            paint.setColor(Color.argb(245, 255, 255, 255));
            paint.setTextAlign(Paint.Align.CENTER);
            paint.setTextSize(Math.min(i >= 8 ? labelTextSize : buttonTextSize, rect.height() * 0.42f));
            float textWidth = paint.measureText(LABELS[i]);
            if (textWidth > rect.width() - 8 * density && textWidth > 0) {
                paint.setTextSize(paint.getTextSize() * Math.max(0, rect.width() - 8 * density) / textWidth);
            }
            float baseline = rect.centerY() - (paint.ascent() + paint.descent()) / 2;
            canvas.drawText(LABELS[i], rect.centerX(), baseline, paint);
        }
    }

    private void drawButton(Canvas canvas, RectF rect, boolean round) {
        if (round) canvas.drawOval(rect, paint);
        else canvas.drawRoundRect(rect, 8 * density, 8 * density, paint);
    }

    private int hit(float x, float y) {
        if (dpad.contains(x, y)) {
            float cell = dpad.width() / 3;
            int col = (int) ((x - dpad.left) / cell);
            int row = (int) ((y - dpad.top) / cell);
            return (col == 0 ? InputState.LEFT : col == 2 ? InputState.RIGHT : 0)
                    | (row == 0 ? InputState.UP : row == 2 ? InputState.DOWN : 0);
        }
        for (int i = 4; i < BUTTONS.length; i++) {
            if (bounds[i].contains(x, y)) return BUTTONS[i];
        }
        return 0;
    }

    @Override public boolean onTouchEvent(MotionEvent event) {
        if (!isEnabled()) {
            release();
            return false;
        }
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL) {
            release();
            return true;
        }
        if (action == MotionEvent.ACTION_DOWN) {
            float x = event.getX(0);
            float y = event.getY(0);
            if (!dpad.contains(x, y) && hit(x, y) == 0) return false;
            tracking = true;
            ViewParent parent = getParent();
            if (parent != null) parent.requestDisallowInterceptTouchEvent(true);
        }
        if (!tracking) return false;
        if (action == MotionEvent.ACTION_UP) {
            release();
            performClick();
            return true;
        }
        if (action != MotionEvent.ACTION_DOWN && action != MotionEvent.ACTION_POINTER_DOWN
                && action != MotionEvent.ACTION_POINTER_UP && action != MotionEvent.ACTION_MOVE) return false;

        int next = 0;
        int released = action == MotionEvent.ACTION_POINTER_UP ? event.getActionIndex() : -1;
        for (int pointer = 0; pointer < event.getPointerCount(); pointer++) {
            if (pointer != released) next |= hit(event.getX(pointer), event.getY(pointer));
        }
        touchMask = next;
        input.setTouch(next);
        invalidate();
        return true;
    }

    @Override public boolean performClick() {
        super.performClick();
        return true;
    }

    @Override protected void onVisibilityChanged(View changedView, int visibility) {
        super.onVisibilityChanged(changedView, visibility);
        if (visibility != VISIBLE) release();
    }

    @Override protected void onWindowVisibilityChanged(int visibility) {
        super.onWindowVisibilityChanged(visibility);
        if (visibility != VISIBLE) release();
    }

    @Override public void onWindowFocusChanged(boolean hasWindowFocus) {
        super.onWindowFocusChanged(hasWindowFocus);
        if (!hasWindowFocus) release();
    }

    @Override protected void onDetachedFromWindow() {
        release();
        super.onDetachedFromWindow();
    }
}
