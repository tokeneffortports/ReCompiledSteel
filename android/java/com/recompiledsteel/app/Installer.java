package com.recompiledsteel.app;

import java.util.HashMap;
import java.util.Map;

/**
 * The setup programs' installer (tools/setup, built into librssetup.so):
 * finds the game, its title update and the add-ons in a folder the player
 * picked, and lays them out in the RealSteel folder.
 */
final class Installer {
    static {
        System.loadLibrary("rssetup");
    }

    interface Listener {
        void onProgress(double fraction, String what);
        void onLog(String line);
    }

    private static native String scan(String folder);
    private static native String install(String folder, String dest, String userRoot, Listener listener);
    private static native String scanFighters(String folder);
    private static native String importFighters(String folder, String gameDir, String userRoot, Listener listener);

    /** Fighters of another Yuke's game in the folder: "game" (name or "") and "fighters" (names joined by |). */
    static Map<String, String> findFighters(String folder) {
        Map<String, String> found = new HashMap<>();
        for (String line : scanFighters(folder).split("\n")) {
            int eq = line.indexOf('=');
            if (eq > 0) found.put(line.substring(0, eq), line.substring(eq + 1));
        }
        return found;
    }

    /** Imports them into userRoot/imported; "" on success. Not on the UI thread. */
    static String runFighters(String folder, String gameDir, String userRoot, Listener listener) {
        return importFighters(folder, gameDir, userRoot, listener);
    }

    /** What the folder holds: game, update (descriptions or ""), addons, looked, other. */
    static Map<String, String> find(String folder) {
        Map<String, String> found = new HashMap<>();
        for (String line : scan(folder).split("\n")) {
            int eq = line.indexOf('=');
            if (eq > 0) found.put(line.substring(0, eq), line.substring(eq + 1));
        }
        return found;
    }

    /** Installs; "" on success, else why it stopped. Runs for minutes - not on the UI thread. */
    static String run(String folder, String dest, String userRoot, Listener listener) {
        return install(folder, dest, userRoot, listener);
    }

    private Installer() {
    }
}
