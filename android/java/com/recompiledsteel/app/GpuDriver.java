package com.recompiledsteel.app;

import android.content.Context;
import android.net.Uri;

import org.json.JSONObject;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/**
 * The optional custom Vulkan driver (launcher: "Graphics driver"). Off by
 * default; only a driver file the player picks, after accepting the warning,
 * is used. It is copied into the app's private storage (Android only loads
 * native code from there) and RealSteelActivity hands it to the game, which
 * loads it through libadrenotools. Nothing is downloaded or suggested here.
 */
final class GpuDriver {
    private GpuDriver() {}

    private static File dir(Context context) {
        return new File(context.getFilesDir(), "gpu_driver");
    }

    /** The chosen driver library, or null for the system driver. */
    static File selected(Context context) {
        String name = RunState.read(context, "gpu_driver_file");
        if (name.isEmpty()) return null;
        File file = new File(dir(context), name);
        return file.isFile() ? file : null;
    }

    /** The chosen driver's name: its meta.json name and version, else the file name. */
    static String name(Context context) {
        File file = selected(context);
        if (file == null) return "";
        String name = RunState.read(context, "gpu_driver_label");
        return name.isEmpty() ? file.getName() : name;
    }

    /** "system (default)" or "CUSTOM: <name> (<file>, <size>)". */
    static String label(Context context) {
        File file = selected(context);
        if (file == null) return "system (default)";
        String name = name(context);
        String size = String.format(java.util.Locale.US, "%.1f MB", file.length() / 1048576.0);
        return "CUSTOM: " + name + (name.equals(file.getName()) ? " (" + size + ")"
                                                              : " (" + file.getName() + ", " + size + ")");
    }

    static void useSystem(Context context) {
        RunState.write(context, "gpu_driver_file", "");
        RunState.write(context, "gpu_driver_label", "");
        File[] old = dir(context).listFiles();
        if (old != null) for (File f : old) f.delete();
    }

    /**
     * Takes a picked file: a driver library (.so) or a zip holding one (a
     * meta.json "libraryName" names it when there are several). Returns ""
     * on success or what was wrong.
     */
    static String install(Context context, Uri uri, String displayName) {
        byte[] lib = null;
        String libName = null;
        String label = displayName;
        try (InputStream in = context.getContentResolver().openInputStream(uri)) {
            byte[] data = readAll(in, 256 << 20);
            if (data == null) return "The file is too large to be a driver.";
            if (isElf(data)) {
                lib = data;
                libName = displayName.endsWith(".so") ? displayName : "vulkan.custom.so";
            } else {
                // A zip: find meta.json and the libraries in it.
                String wanted = null;
                java.util.Map<String, byte[]> libs = new java.util.HashMap<>();
                try (ZipInputStream zip = new ZipInputStream(new java.io.ByteArrayInputStream(data))) {
                    ZipEntry e;
                    while ((e = zip.getNextEntry()) != null) {
                        String name = new File(e.getName()).getName();
                        if (e.isDirectory()) continue;
                        if (name.equals("meta.json")) {
                            byte[] meta = readAll(zip, 1 << 20);
                            if (meta != null) {
                                JSONObject json = new JSONObject(new String(meta, StandardCharsets.UTF_8));
                                wanted = json.optString("libraryName", null);
                                String n = json.optString("name", "");
                                String v = json.optString("driverVersion", "");
                                if (!n.isEmpty()) label = n + (v.isEmpty() ? "" : " " + v);
                            }
                        } else if (name.endsWith(".so")) {
                            byte[] so = readAll(zip, 256 << 20);
                            if (so != null) libs.put(name, so);
                        }
                    }
                }
                if (wanted != null && libs.containsKey(wanted)) {
                    libName = wanted;
                } else if (libs.size() == 1) {
                    libName = libs.keySet().iterator().next();
                } else if (libs.isEmpty()) {
                    return "No driver library (.so) was found in that file.";
                } else {
                    return "That zip holds several libraries and no meta.json naming the driver.";
                }
                lib = libs.get(libName);
            }
        } catch (Exception e) {
            return "Could not read that file: " + e.getMessage();
        }
        if (!isElf(lib) || lib.length < 20 || lib[4] != 2 || (lib[18] & 0xff) != 183) {
            return "That is not a 64-bit ARM (arm64) library, so it cannot be a driver for this device.";
        }
        if (!libName.matches("[A-Za-z0-9._-]+")) libName = "vulkan.custom.so";
        useSystem(context);
        File folder = dir(context);
        folder.mkdirs();
        try (OutputStream out = new FileOutputStream(new File(folder, libName))) {
            out.write(lib);
        } catch (Exception e) {
            return "Could not store the driver: " + e.getMessage();
        }
        RunState.write(context, "gpu_driver_file", libName);
        RunState.write(context, "gpu_driver_label", label);
        return "";
    }

    private static boolean isElf(byte[] data) {
        return data != null && data.length > 4 && data[0] == 0x7f && data[1] == 'E' && data[2] == 'L'
            && data[3] == 'F';
    }

    private static byte[] readAll(InputStream in, int limit) throws java.io.IOException {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        byte[] buf = new byte[1 << 16];
        int n;
        while ((n = in.read(buf)) > 0) {
            out.write(buf, 0, n);
            if (out.size() > limit) return null;
        }
        return out.toByteArray();
    }
}
