package com.recompiledsteel.app;

import android.os.Bundle;
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
        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onDestroy() {
        Log.i(TAG, "game activity ending");
        RunState.ended(this);
        super.onDestroy();
    }
}
