package io.github.wootbeer.ortarecomp;

import android.annotation.SuppressLint;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PixelFormat;
import android.graphics.PorterDuff;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;

/**
 * The on-screen touch controls, laid over the game's surface. They live in native code
 * (src/android/touch_controls.c, on top of descore's touch module; their settings in Options >
 * Gameplay); this view forwards touches to it and draws the controls it reports, a flat
 * translucent gray. From Sin and Punishment Recompiled's Android build, by the same author.
 *
 * While the game reads its controller and no options are open, every touch comes here, finger by
 * finger, so the game window beneath never sees it (SDL would turn it into mouse clicks).
 * Otherwise touches go on to the window, for the launcher and the options. The controls hide
 * while a gamepad is connected, as the Touch buttons setting says.
 *
 * A SurfaceView of its own, layered just above the game's (setZOrderMediaOverlay), rather than a
 * plain view: in BattleTanx a drawing view over SDL's SurfaceView made the window composite over
 * the whole game, and its navigation bar backdrop then showed as a black strip across the bottom.
 */
public class TouchControlsView extends SurfaceView implements SurfaceHolder.Callback {
    private static final int FLOATS_PER_SHAPE = 7;
    private static final int POLL_MS = 250;

    /** Called when the MENU button is tapped. */
    interface MenuListener {
        void onMenuRequested();
    }

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint label = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float[] shapes = new float[FLOATS_PER_SHAPE * 64];
    private final String[] labels = new String[32];
    private MenuListener menuListener;

    private boolean active;
    private boolean gamepadConnected;
    private boolean laidOut;
    private boolean surfaceReady;
    /** The current gesture began while the game took input, so it is ours to the end. */
    private boolean tracking;

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            int state = nativePoll();
            boolean nowActive = (state & 1) != 0;
            if ((state & 2) != 0) {
                relayout();
            }
            if (nowActive != active) {
                active = nowActive;
                if (!active) {
                    nativeReleaseAll(); // a menu opened under a finger: nothing stays held
                }
                redraw();
            }
            handler.postDelayed(this, POLL_MS);
        }
    };

    public TouchControlsView(Context context) {
        super(context);
        getHolder().setFormat(PixelFormat.TRANSLUCENT);
        getHolder().addCallback(this);
        setZOrderMediaOverlay(true);
        fill.setStyle(Paint.Style.FILL);
        label.setTextAlign(Paint.Align.CENTER);
        label.setFakeBoldText(true);
    }

    void setMenuListener(MenuListener listener) {
        menuListener = listener;
    }

    /** Whether a gamepad is connected (a handheld's built-in one too). */
    void setGamepadConnected(boolean connected) {
        if (connected == gamepadConnected) {
            return;
        }
        gamepadConnected = connected;
        nativeSetGamepadConnected(connected);
        redraw();
    }

    private void relayout() {
        if (getWidth() <= 0 || getHeight() <= 0) {
            return;
        }
        nativeLayout(getWidth(), getHeight(), getResources().getDisplayMetrics().density);
        if (!laidOut) {
            laidOut = true;
            nativeSetGamepadConnected(gamepadConnected);
        }
        redraw();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        handler.post(poll);
    }

    @Override
    protected void onDetachedFromWindow() {
        handler.removeCallbacks(poll);
        super.onDetachedFromWindow();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        relayout();
    }

    @Override
    public void surfaceCreated(SurfaceHolder holder) {
        nativeLog("overlay surface created");
        surfaceReady = true;
        redraw();
    }

    @Override
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        nativeLog("overlay surface changed to " + width + "x" + height);
        redraw();
    }

    @Override
    public void surfaceDestroyed(SurfaceHolder holder) {
        nativeLog("overlay surface destroyed");
        surfaceReady = false;
    }

    /** Draws the buttons into this view's surface (cleared when they are hidden). */
    private void redraw() {
        if (!surfaceReady) {
            return;
        }
        Canvas canvas = getHolder().lockCanvas();
        if (canvas == null) {
            nativeLog("overlay: no canvas to draw on");
            return;
        }
        try {
            canvas.drawColor(0, PorterDuff.Mode.CLEAR);
            if (active && laidOut) {
                drawControls(canvas);
            }
        } finally {
            getHolder().unlockCanvasAndPost(canvas);
        }
    }

    private void drawControls(Canvas canvas) {
        int count = nativeGetShapes(shapes);
        for (int i = 0; i < count; i++) {
            int o = i * FLOATS_PER_SHAPE;
            int kind = (int) shapes[o];
            int button = (int) shapes[o + 1];
            float x = shapes[o + 2], y = shapes[o + 3], w = shapes[o + 4], h = shapes[o + 5];
            float alpha = shapes[o + 6];
            fill.setARGB(Math.round(alpha * 255), 179, 179, 179);
            if (kind == 1) {
                canvas.drawCircle(x, y, w, fill);
                continue;
            }
            canvas.drawRect(x, y, x + w, y + h, fill);
            if (button >= 0 && button < labels.length) {
                if (labels[button] == null) {
                    labels[button] = nativeButtonLabel(button);
                }
                label.setARGB(Math.round(Math.min(1.0f, alpha * 2.0f) * 255), 255, 255, 255);
                label.setTextSize(Math.min(h * 0.45f, w * 0.22f));
                canvas.drawText(labels[button], x + w * 0.5f,
                        y + h * 0.5f - (label.descent() + label.ascent()) * 0.5f, label);
            }
        }
    }

    @SuppressLint("ClickableViewAccessibility")
    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_DOWN) {
            tracking = active && laidOut;
        }
        if (!tracking) {
            return false; // to the game beneath, for its menus
        }
        if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL) {
            tracking = false;
        }
        if (!active) {
            return true; // a menu opened during the gesture; its fingers were let go
        }

        boolean menu = false;
        boolean buttons = false;
        if (action == MotionEvent.ACTION_CANCEL) {
            nativeReleaseAll();
            buttons = true;
        } else {
            int first = (action == MotionEvent.ACTION_POINTER_DOWN
                    || action == MotionEvent.ACTION_POINTER_UP) ? event.getActionIndex() : 0;
            int count = (action == MotionEvent.ACTION_MOVE) ? event.getPointerCount() : 1;
            for (int i = first; i < count + first; i++) {
                int result = nativeTouch(action, event.getPointerId(i), event.getX(i), event.getY(i));
                menu |= (result & 2) != 0;
                buttons |= (result & 4) != 0;
            }
        }
        if (buttons) {
            redraw();
        }
        if (menu && menuListener != null) {
            menuListener.onMenuRequested();
        }
        return true;
    }

    private static native void nativeLayout(int width, int height, float density);
    private static native int nativeTouch(int action, int pointerId, float x, float y);
    private static native void nativeReleaseAll();
    private static native void nativeSetGamepadConnected(boolean connected);
    private static native int nativeGetShapes(float[] out);
    private static native String nativeButtonLabel(int button);
    private static native int nativePoll();
    private static native void nativeLog(String what);
}
