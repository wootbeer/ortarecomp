package io.github.wootbeer.ortarecomp;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;
import android.util.Log;

import java.io.File;

/**
 * Starts the game again in a new process, on the program the one before asked for (Pandora's
 * Box's PANZER DRAGOON, and back to Orta from its title). The game runs once per process
 * (src/guest/relaunch.c), as the PC builds restart theirs, so the game's process ends and this
 * activity, in a process of its own (":relaunch"), waits for it to be gone and then starts
 * OrtaActivity with the arguments it was handed (OrtaActivity.getArguments).
 */
public class RelaunchActivity extends Activity {
    private static final String TAG = "Orta";
    static final String EXTRA_ARGS = "io.github.wootbeer.ortarecomp.ARGS";
    static final String EXTRA_PID = "io.github.wootbeer.ortarecomp.PID";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        final String[] args = getIntent().getStringArrayExtra(EXTRA_ARGS);
        final int pid = getIntent().getIntExtra(EXTRA_PID, 0);
        new Thread(() -> {
            // At most 5 s: the game's process normally ends at once.
            File proc = new File("/proc/" + pid);
            for (int i = 0; i < 100 && pid > 0 && proc.exists(); i++) {
                try {
                    Thread.sleep(50);
                } catch (InterruptedException e) {
                    break;
                }
            }
            runOnUiThread(() -> {
                Intent game = new Intent(this, OrtaActivity.class);
                game.putExtra(EXTRA_ARGS, args);
                game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
                startActivity(game);
                finish();
                overridePendingTransition(0, 0);
                Process.killProcess(Process.myPid());
            });
        }, "relaunch").start();
    }

    /**
     * Called by the game (src/guest/relaunch.c, any thread) to start again with these arguments:
     * starts this activity and ends the game's process. Returns only if it could not.
     */
    static void restart(Activity from, String[] args) {
        try {
            Intent intent = new Intent(from, RelaunchActivity.class);
            intent.putExtra(EXTRA_ARGS, args);
            intent.putExtra(EXTRA_PID, Process.myPid());
            intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            from.startActivity(intent);
        } catch (RuntimeException e) {
            Log.e(TAG, "Couldn't start the game again", e);
            return;
        }
        Process.killProcess(Process.myPid());
    }
}
