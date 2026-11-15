package io.github.wootbeer.ortarecomp;

import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.View;

import org.libsdl.app.SDLActivity;

/**
 * Hosts the native game (libmain.so, built from the repo-root CMakeLists.txt, with SDL linked in)
 * through SDL's SDLActivity. From Sin and Punishment Recompiled's Android build, by the same author.
 */
public class OrtaActivity extends SDLActivity {
    private static final String TAG = "Orta";

    /** The game runs once per process; a crash is looked for once, at its start. */
    private static boolean runStarted;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        boolean lastRunCrashed = false;
        if (!runStarted) {
            runStarted = true;
            lastRunCrashed = LogFiles.lastRunCrashed(this);
        }
        super.onCreate(savedInstanceState);
        if (lastRunCrashed && !mBrokenLibraries) {
            offerLogAfterCrash();
        }
    }

    /**
     * Asks whether to share the log of the run that crashed, before the game starts again: SDL
     * starts the game once the activity's window has focus, which the dialog holds until it's
     * closed. So a game that crashes at once still lets the player share its log.
     */
    private void offerLogAfterCrash() {
        mHasFocus = false;
        new AlertDialog.Builder(this, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                .setTitle("The game closed unexpectedly")
                .setMessage("Share its log to help fix the problem? It has your device's model, its "
                        + "graphics driver and the game's messages, nothing personal.\n\n"
                        + "You can share it later too: long-press the app icon, then Share log.")
                .setPositiveButton("Share", (dialog, which) -> {
                    Intent share = LogFiles.shareIntent(this);
                    if (share != null) {
                        try {
                            startActivity(share);
                        } catch (ActivityNotFoundException e) {
                            Log.e(TAG, "Nothing to share the log with", e);
                        }
                    }
                })
                .setNegativeButton("Not now", null)
                .show();
    }

    @Override
    protected void onResume() {
        super.onResume();
        LogFiles.markRunning(this);
    }

    @Override
    protected void onPause() {
        LogFiles.markNotRunning(this);
        super.onPause();
    }

    // SDL hides the system bars once, when the game's window goes fullscreen (mFullscreenModeActive).
    // Hide them again whenever the window gets focus back.
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) {
            hideSystemBars();
        }
    }

    // SDL hides the bars again 2 s after they show; do it straight away.
    @Override
    public void onSystemUiVisibilityChange(int visibility) {
        super.onSystemUiVisibilityChange(visibility);
        if ((visibility & View.SYSTEM_UI_FLAG_HIDE_NAVIGATION) == 0
                || (visibility & View.SYSTEM_UI_FLAG_FULLSCREEN) == 0) {
            hideSystemBars();
        }
    }

    private void hideSystemBars() {
        if (!mFullscreenModeActive) {
            return;
        }
        getWindow().getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN
                | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
    }

    /** The options menu (F1 on PC) opens and closes on F1. */
    private void toggleGameMenu() {
        onNativeKeyDown(KeyEvent.KEYCODE_F1);
        onNativeKeyUp(KeyEvent.KEYCODE_F1);
    }

    // Back opens and closes the options menu instead of closing the app (SDL's default), which on a
    // handheld is one press away in the middle of a stage. The menu's Quit closes the app.
    // Not a gamepad's own Back (some controllers send B as one): SDL hands that to the game.
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (event.getKeyCode() == KeyEvent.KEYCODE_BACK && !isGamepadEvent(event.getSource())) {
            if (event.getAction() == KeyEvent.ACTION_UP) {
                toggleGameMenu();
            }
            return true;
        }
        return super.dispatchKeyEvent(event);
    }

    private static boolean isGamepadEvent(int source) {
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    // SDL is linked into libmain.so (the root CMakeLists.txt builds it static), so that is the
    // only library to load; SDL_main.h renames the game's main() to SDL_main, SDL's default.
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }
}
