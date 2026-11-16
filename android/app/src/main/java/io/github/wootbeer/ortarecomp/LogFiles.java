package io.github.wootbeer.ortarecomp;

import android.content.ClipData;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.net.Uri;
import android.os.Build;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * The game's logs, for players to send with a bug report. In the app's private storage (the
 * game's data folder, SDL's pref path): ortarecomp.log, which has the crash report too, and
 * ortarecomp-previous.log, the run before. The game rotates them itself at its start (src/main.c).
 * A marker file exists while the game is in the foreground; the game removes it when it quits
 * (src/main.c), so finding it at the next start means the last run crashed or was killed in use.
 * From Sin and Punishment Recompiled's Android build, by the same author.
 */
final class LogFiles {
    private static final String TAG = "Orta";

    static final String LOG = "ortarecomp.log";
    private static final String PREVIOUS_LOG = "ortarecomp-previous.log";
    /** Also removed by the game when it quits (src/main.c). */
    static final String RUNNING_MARKER = "running";

    /** Only so much of each log goes in the shared file: its end, where the trouble is. */
    private static final int MAX_LOG_BYTES = 1024 * 1024;

    private LogFiles() {
    }

    /** What the game's crash handler writes first (src/guest/crash_report.c). */
    private static final String CRASH_REPORT = "=== CRASH:";
    /** It is near the end of the log: only so much of it is read. */
    private static final int CRASH_SEARCH_BYTES = 256 * 1024;

    /**
     * At the start of a run, before the game rotates its logs: whether the last one crashed. The
     * marker alone only says it did not quit from the front: Android Studio's Run and Stop kill
     * the app there, and so can the system, and neither is the game's fault. A crash leaves the
     * game's own crash report at the end of its log too.
     */
    static boolean lastRunCrashed(Context context) {
        File marker = new File(context.getFilesDir(), RUNNING_MARKER);
        boolean ended = marker.exists();
        marker.delete();
        return ended && logHasCrashReport(new File(context.getFilesDir(), LOG));
    }

    private static boolean logHasCrashReport(File log) {
        try (RandomAccessFile f = new RandomAccessFile(log, "r")) {
            long length = f.length();
            int n = (int) Math.min(length, CRASH_SEARCH_BYTES);
            byte[] tail = new byte[n];
            f.seek(length - n);
            f.readFully(tail);
            return new String(tail, StandardCharsets.ISO_8859_1).contains(CRASH_REPORT);
        } catch (IOException e) {
            return false;
        }
    }

    /** The game is in the foreground: a crash from here on leaves the marker behind. */
    static void markRunning(Context context) {
        try {
            new File(context.getFilesDir(), RUNNING_MARKER).createNewFile();
        } catch (IOException e) {
            Log.w(TAG, "Couldn't create the running marker", e);
        }
    }

    /** In the background: being closed from there (recent apps, low memory) isn't a crash. */
    static void markNotRunning(Context context) {
        new File(context.getFilesDir(), RUNNING_MARKER).delete();
    }

    /** A share sheet for one text file with the device, the app's version and both runs' logs. */
    static Intent shareIntent(Context context) {
        File file;
        try {
            file = writeReport(context);
        } catch (IOException e) {
            Log.e(TAG, "Couldn't write the log for sharing", e);
            return null;
        }
        Uri uri = LogProvider.uriFor(context, file);
        Intent send = new Intent(Intent.ACTION_SEND);
        send.setType("text/plain");
        send.putExtra(Intent.EXTRA_STREAM, uri);
        send.putExtra(Intent.EXTRA_SUBJECT, "Orta Recomp log");
        // Some share targets read the file through the clip data, so it carries the grant too.
        send.setClipData(ClipData.newRawUri(file.getName(), uri));
        send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        Intent chooser = Intent.createChooser(send, "Share the game's log");
        chooser.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        return chooser;
    }

    private static File writeReport(Context context) throws IOException {
        File folder = LogProvider.folder(context);
        if (!folder.isDirectory() && !folder.mkdirs()) {
            throw new IOException("Couldn't create " + folder);
        }
        File report = new File(folder, "OrtaRecomp-log.txt");
        File dir = context.getFilesDir();
        try (OutputStream out = new FileOutputStream(report)) {
            write(out, header(context));
            section(out, "Log, latest run", new File(dir, LOG));
            section(out, "Log, run before", new File(dir, PREVIOUS_LOG));
        }
        return report;
    }

    private static String header(Context context) {
        String version = "unknown";
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            version = info.versionName + " (" + info.getLongVersionCode() + ")";
        } catch (Exception e) {
            // Leave it unknown.
        }
        StringBuilder text = new StringBuilder();
        text.append("Orta Recomp ").append(version).append('\n');
        text.append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL)
                .append(" (").append(Build.DEVICE).append(")\n");
        if (Build.VERSION.SDK_INT >= 31) {
            text.append("SoC: ").append(Build.SOC_MANUFACTURER).append(' ').append(Build.SOC_MODEL).append('\n');
        }
        text.append("Android ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        text.append("Shared: ").append(timestamp(System.currentTimeMillis())).append('\n');
        return text.toString();
    }

    private static void section(OutputStream out, String title, File file) throws IOException {
        if (!file.exists()) {
            write(out, "\n===== " + title + ": none =====\n");
            return;
        }
        write(out, "\n===== " + title + " (written " + timestamp(file.lastModified()) + ") =====\n");
        try (RandomAccessFile in = new RandomAccessFile(file, "r")) {
            long length = in.length();
            if (length > MAX_LOG_BYTES) {
                write(out, "[... the first " + (length - MAX_LOG_BYTES) + " bytes left out ...]\n");
                in.seek(length - MAX_LOG_BYTES);
            }
            byte[] buf = new byte[64 * 1024];
            int n;
            while ((n = in.read(buf)) > 0) {
                out.write(buf, 0, n);
            }
        }
    }

    private static String timestamp(long millis) {
        return new SimpleDateFormat("yyyy-MM-dd HH:mm:ss", Locale.ROOT).format(new Date(millis));
    }

    private static void write(OutputStream out, String text) throws IOException {
        out.write(text.getBytes(StandardCharsets.UTF_8));
    }
}
