package com.recompiledsteel.app;

import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;

/**
 * The game: SDL's activity runs SDL_main in libmain.so. Started by
 * LauncherActivity once file access and the game files are in place.
 *
 * Before the game's libraries load, librscrash.so installs a crash catcher
 * (native crashes are written to logs/native_crash.txt), and every loading
 * step is recorded so the launcher can tell where a failed start stopped.
 */
public class RealSteelActivity extends SDLActivity {
    private static final String TAG = "RealSteel";

    private static native void nativeInstallCrashHandler(String reportPath);

    @Override
    protected String[] getLibraries() {
        // SDL itself is linked into the runtime library.
        return new String[] {"c++_shared", "rexruntime", "main"};
    }

    @Override
    public void loadLibraries() {
        RunState.step(this, "loading c++_shared");
        System.loadLibrary("c++_shared");
        RunState.step(this, "loading rscrash");
        System.loadLibrary("rscrash");
        File logs = RunState.logsDir(this);
        nativeInstallCrashHandler(new File(logs, "native_crash.txt").getAbsolutePath());
        RunState.step(this, "loading rexruntime");
        System.loadLibrary("rexruntime");
        RunState.step(this, "loading main");
        System.loadLibrary("main");
        RunState.step(this, "libraries loaded, starting the game");
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        RunState.started(this);
        // A custom GPU driver only when the player chose one in the launcher;
        // otherwise the variables are cleared and the game uses the system one.
        File driver = GpuDriver.selected(this);
        try {
            if (driver != null) {
                Log.w(TAG, "custom GPU driver chosen: " + GpuDriver.label(this));
                Os.setenv("REALSTEEL_GPU_DRIVER", driver.getAbsolutePath(), true);
                Os.setenv("REALSTEEL_GPU_DRIVER_LABEL", GpuDriver.name(this), true);
                Os.setenv("REALSTEEL_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            } else {
                Os.unsetenv("REALSTEEL_GPU_DRIVER");
                Os.unsetenv("REALSTEEL_GPU_DRIVER_LABEL");
            }
        } catch (Exception e) {
            Log.e(TAG, "could not pass the GPU driver choice: " + e);
        }
        super.onCreate(savedInstanceState);
    }

    // The game is landscape only (either way up). SDL would otherwise ask
    // for any orientation because the window is resizable, which on a phone
    // (portrait by nature, rotation locked) starts the game sideways.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    @Override
    protected void onDestroy() {
        Log.i(TAG, "game activity ending");
        RunState.ended(this);
        super.onDestroy();
    }
}
