package com.recompiledsteel.app;

import android.content.Context;
import android.os.Build;
import android.os.Environment;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Arrays;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipOutputStream;

/**
 * Where the app keeps things, and what the last start did. The run state
 * lives in private storage (always writable): "running <start ms>" while the
 * game runs, "ended <start ms> <end ms>" after it closed normally, plus the
 * last loading step.
 */
final class RunState {
    private RunState() {}

    static File dataRoot() {
        return new File(Environment.getExternalStorageDirectory(), "RealSteel");
    }

    static boolean hasFileAccess() {
        return Build.VERSION.SDK_INT < Build.VERSION_CODES.R || Environment.isExternalStorageManager();
    }

    /** RealSteel/logs when the app may write there, else its own external folder. */
    static File logsDir(Context context) {
        File logs = new File(dataRoot(), "logs");
        if (hasFileAccess() && (logs.isDirectory() || logs.mkdirs()) && logs.canWrite()) {
            return logs;
        }
        File fallback = new File(context.getExternalFilesDir(null), "logs");
        fallback.mkdirs();
        return fallback;
    }

    static void write(Context context, String name, String text) {
        try (FileOutputStream out = new FileOutputStream(new File(context.getFilesDir(), name))) {
            out.write(text.getBytes(StandardCharsets.UTF_8));
        } catch (Exception ignored) {
        }
    }

    static String read(Context context, String name) {
        try {
            return new String(Files.readAllBytes(new File(context.getFilesDir(), name).toPath()),
                              StandardCharsets.UTF_8).trim();
        } catch (Exception e) {
            return "";
        }
    }

    static void started(Context context) {
        write(context, "run_state", "running " + System.currentTimeMillis());
        write(context, "run_step", "starting");
    }

    static void step(Context context, String step) {
        write(context, "run_step", step);
    }

    static void ended(Context context) {
        String state = read(context, "run_state");
        String start = state.startsWith("running ") ? state.substring(8) : "0";
        write(context, "run_state", "ended " + start + " " + System.currentTimeMillis());
    }

    /**
     * Everything a bug report needs in one zip in logs/: the app's log lines,
     * the newest game logs, the renderer's log (rs_native_print.log, which
     * names the GPU and driver), a native crash report and the settings.
     */
    static File saveDiagnostics(Context context, String prefix) {
        File logcat = saveLogcat(context, "logcat");
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        File logs = logsDir(context);
        File out = new File(logs, prefix + "_" + stamp + ".zip");
        java.util.List<File> files = new java.util.ArrayList<>();
        if (logcat != null) files.add(logcat);
        File[] game = logs.listFiles((d, n) -> n.startsWith("realsteel_") && n.endsWith(".log"));
        if (game != null) {
            Arrays.sort(game, (a, b) -> Long.compare(b.lastModified(), a.lastModified()));
            for (int i = 0; i < Math.min(3, game.length); ++i) files.add(game[i]);
        }
        files.add(new File(logs, "native_crash.txt"));
        files.add(new File(dataRoot(), "rs_native_print.log"));
        files.add(new File(dataRoot(), "realsteel_settings.cfg"));
        try (ZipOutputStream zip = new ZipOutputStream(new FileOutputStream(out))) {
            for (File f : files) {
                if (!f.isFile() || f.length() > (8 << 20)) continue;
                String text = redact(new String(Files.readAllBytes(f.toPath()), StandardCharsets.UTF_8));
                zip.putNextEntry(new ZipEntry(f.getName()));
                zip.write(text.getBytes(StandardCharsets.UTF_8));
                zip.closeEntry();
            }
            zip.putNextEntry(new ZipEntry("graphics_driver.txt"));
            zip.write(("graphics driver: " + GpuDriver.label(context) + "\n").getBytes(StandardCharsets.UTF_8));
            zip.closeEntry();
            return out;
        } catch (Exception e) {
            return logcat;
        }
    }

    // Settings that identify the player or their online matches.
    private static final java.util.regex.Pattern PRIVATE_SETTING = java.util.regex.Pattern.compile(
        "(?m)^(realsteel_net_password|realsteel_net_peers|realsteel_lobby_server|realsteel_online_name"
        + "|realsteel_online_xuid)\\s*=.*$");
    // IPv4 addresses (other players' and the player's own, from online play),
    // except loopback / unspecified and 0.x (the game's version numbers).
    private static final java.util.regex.Pattern IPV4 = java.util.regex.Pattern.compile(
        "(?<![\\d.])(?!127\\.)(?!0\\.)\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}(?![\\d.])");

    /** Diagnostics are posted publicly: drop online details and addresses. */
    static String redact(String text) {
        text = PRIVATE_SETTING.matcher(text).replaceAll("$1 = (removed)");
        return IPV4.matcher(text).replaceAll("x.x.x.x");
    }

    /** The app's own log lines (logcat -d) into logs/, returns the file or null. */
    static File saveLogcat(Context context, String prefix) {
        String stamp = new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date());
        File out = new File(logsDir(context), prefix + "_" + stamp + ".txt");
        try {
            Process process = new ProcessBuilder("logcat", "-d", "-v", "threadtime")
                                  .redirectErrorStream(true).start();
            try (BufferedReader reader = new BufferedReader(
                     new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8));
                 FileOutputStream file = new FileOutputStream(out)) {
                String header = "ReCompiled Steel " + BuildConfigVersion.NAME + " on "
                        + Build.MANUFACTURER + " " + Build.MODEL + ", Android " + Build.VERSION.RELEASE
                        + " (API " + Build.VERSION.SDK_INT + "), last step: " + read(context, "run_step")
                        + ", run state: " + read(context, "run_state")
                        + ", graphics driver: " + GpuDriver.label(context) + "\n\n";
                file.write(header.getBytes(StandardCharsets.UTF_8));
                String line;
                while ((line = reader.readLine()) != null) {
                    file.write((line + "\n").getBytes(StandardCharsets.UTF_8));
                }
            }
            process.waitFor();
            return out;
        } catch (Exception e) {
            return null;
        }
    }
}
