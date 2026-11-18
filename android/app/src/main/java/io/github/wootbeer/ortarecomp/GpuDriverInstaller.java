package io.github.wootbeer.ortarecomp;

import android.content.Context;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * Installs a custom Vulkan driver the player picked (src/android/gpu_driver/gpu_driver.h) into
 * the app's storage: filesDir/gpu_drivers/NAME/, with library.txt naming the library to load.
 * Takes the zips Turnip and other Adreno driver builds come in (meta.json gives the library's
 * name, as AdrenoTools-style releases have it) or a lone .so. Nothing here knows the game, so
 * other apps can take it as it is.
 */
public final class GpuDriverInstaller {
    private GpuDriverInstaller() {
    }

    static File driversDir(Context context) {
        return new File(context.getFilesDir(), "gpu_drivers");
    }

    /** Installs the driver at uri and returns its folder's name; IOException says why not. */
    static String install(Context context, Uri uri) throws IOException {
        String file = displayName(context, uri);
        byte[] data = readAll(context, uri);
        boolean zip = data.length > 4 && data[0] == 'P' && data[1] == 'K';
        boolean elf = data.length > 4 && data[0] == 0x7F && data[1] == 'E' && data[2] == 'L' && data[3] == 'F';
        if (!zip && !elf) {
            throw new IOException("not a driver zip or .so");
        }
        File tmp = new File(driversDir(context), ".installing");
        deleteTree(tmp);
        if (!tmp.mkdirs()) {
            throw new IOException("can't make " + tmp);
        }
        String library;
        String name;
        if (elf) {
            library = safeName(file.isEmpty() ? "libvulkan_custom.so" : file);
            write(new File(tmp, library), data);
            name = library.replaceAll("\\.so$", "");
        } else {
            String meta = unzip(data, tmp);
            library = null;
            name = null;
            if (meta != null) {
                try {
                    JSONObject json = new JSONObject(meta);
                    library = json.optString("libraryName", null);
                    String n = json.optString("name", "");
                    String v = json.optString("driverVersion", "");
                    name = (n + (v.isEmpty() ? "" : " " + v)).trim();
                } catch (Exception e) {
                    library = null;
                }
            }
            if (library == null || library.isEmpty()) {
                String[] sos = tmp.list((d, n) -> n.endsWith(".so"));
                if (sos == null || sos.length != 1) {
                    deleteTree(tmp);
                    throw new IOException("the zip has no meta.json naming its library");
                }
                library = sos[0];
            }
            library = safeName(library);
            if (!new File(tmp, library).isFile()) {
                deleteTree(tmp);
                throw new IOException("the zip has no " + library);
            }
            if (name == null || name.isEmpty()) {
                name = file.replaceAll("\\.zip$", "");
            }
        }
        name = safeName(name);
        if (name.isEmpty()) {
            name = "driver";
        }
        write(new File(tmp, "library.txt"), library.getBytes(StandardCharsets.UTF_8));
        File dest = new File(driversDir(context), name);
        deleteTree(dest);
        if (!tmp.renameTo(dest)) {
            deleteTree(tmp);
            throw new IOException("can't move it into " + dest);
        }
        return name;
    }

    /** A file or folder name: letters, digits, dot, dash and underscore, at most 48. */
    private static String safeName(String s) {
        String n = s.replaceAll("[^A-Za-z0-9._-]+", "_").replaceAll("^[._]+", "");
        return n.length() > 48 ? n.substring(0, 48) : n;
    }

    /** Every file of the zip into dir, flat (folders in the zip dropped); meta.json's text. */
    private static String unzip(byte[] data, File dir) throws IOException {
        String meta = null;
        try (ZipInputStream in = new ZipInputStream(new java.io.ByteArrayInputStream(data))) {
            ZipEntry e;
            while ((e = in.getNextEntry()) != null) {
                if (e.isDirectory()) {
                    continue;
                }
                String base = e.getName().replace('\\', '/');
                base = base.substring(base.lastIndexOf('/') + 1);
                if (base.isEmpty()) {
                    continue;
                }
                byte[] bytes = readStream(in);
                if (base.equals("meta.json")) {
                    meta = new String(bytes, StandardCharsets.UTF_8);
                }
                write(new File(dir, safeName(base)), bytes);
            }
        }
        return meta;
    }

    private static String displayName(Context context, Uri uri) {
        try (Cursor c = context.getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME },
                null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String n = c.getString(0);
                if (n != null) {
                    return n;
                }
            }
        } catch (RuntimeException ignored) {
            // no name: one is made up
        }
        String p = uri.getLastPathSegment();
        return p != null ? p.substring(p.lastIndexOf('/') + 1) : "";
    }

    private static byte[] readAll(Context context, Uri uri) throws IOException {
        try (InputStream in = context.getContentResolver().openInputStream(uri)) {
            if (in == null) {
                throw new IOException("can't open it");
            }
            return readStream(in);
        }
    }

    private static byte[] readStream(InputStream in) throws IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[65536];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
        }
        return out.toByteArray();
    }

    private static void write(File f, byte[] data) throws IOException {
        try (OutputStream out = new FileOutputStream(f)) {
            out.write(data);
        }
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) {
            for (File k : kids) {
                deleteTree(k);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
