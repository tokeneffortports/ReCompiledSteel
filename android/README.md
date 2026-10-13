# ReCompiled Steel on Android (experimental)

An arm64 Android build of the same recompiled game: native code, Vulkan,
SDL3 for the window, controllers and sound. It runs at 60 fps on a Retroid
Pocket Nova (Snapdragon, Adreno 740, 1280x960 at 4:3); other arm64 devices
with Vulkan 1.3 should work but have not been tried.

## Installing

1. Copy your own Real Steel files from the Xbox 360 to the device's storage
   or SD card: the game (`000D0000`), its title update (`000B0000`,
   required) and any add-ons (`00000002`). The simplest is the whole
   `Content/0000000000000000/584111E0` folder from the console or USB stick.
2. Copy `ReCompiledSteel-android-arm64-<version>.apk` to the device and
   install it (allow installs from that file manager when Android asks).
3. Open ReCompiled Steel, press **Allow file access** (Android's "All files
   access" setting: allow it and go back), then **Install game files**, and
   pick the folder from step 1. The installer (the same code as the PC setup
   programs) finds the packages, even in sub-folders, and lays them out in
   `RealSteel/` at the top of the internal storage: `game/`, `tu/`, and the
   add-ons under `user/`. Afterwards the copied packages can be deleted.
4. Press **Start**.

Robot mods go in `RealSteel/mods/`, as on the PC. Saves are in
`RealSteel/user/`.

Controls: any controller works, including a handheld's built-in one. With
no controller connected, on-screen touch controls appear (Settings, Controls,
Touch controls: Automatic / On / Off); their "..." button opens the settings
page, which on a controller opens with L3 + R3.

Logs are written to `RealSteel/logs/`. If something goes wrong, the newest
`realsteel_*.log` there is what to send. Settings (F1 page on the PC) are
saved in `RealSteel/realsteel_settings.cfg`.

## Building

From WSL or Linux, with your game files in `work/` as for the desktop build:

1. `android/build_sdk.sh` - the ReXGlue SDK built for Android: the SDK
   source at the commit the desktop SDK was built from (509ed5b) plus
   `thirdparty/rexglue-android.patch`, cross-compiled with the NDK and
   installed to `~/android/rexglue-android-arm64`.
2. `android/build_android.sh` - cross-compiles the game to `libmain.so`
   (code generation runs with the host linux-amd64 SDK).
3. `android/build_apk.sh` - packages and signs
   `release/ReCompiledSteel-android-arm64-<version>.apk` with the Android
   build-tools (no Gradle). It uses a debug key, created on first use.

Tools: Android SDK with `ndk;29.0.14206865`, `build-tools;35.0.0`,
`platforms;android-35`, and JDK 17 (`android/dev/setup_toolchain.sh`
fetches them into `~/android`).

Development helpers in `android/dev/`: `cycle.sh` (SDK + game + APK in one
go), `nova_run.sh` (install, start and collect the log over adb),
`export_sdk_patch.sh` (SDK source edits back into the patch) and
`shader_cache.sh` (rebuild the patched XenosRecomp and regenerate
`generated/shader_cache.cpp`).

## What the port changes

- SDK (`thirdparty/rexglue-android.patch`): Android start-up
  (`rex::InitializeAndroid`: shared-memory and thread entry points), fibres
  without ucontext (a small aarch64 context switch), libc++ clock
  conversions, no robust mutexes, logging to logcat, the ANativeWindow
  surface, the `SDL_main` entry point, and Android branches in the build.
- Game: the renderer creates a `VK_KHR_android_surface`; everything lives
  in `/sdcard/RealSteel` instead of beside the executable; the game is a
  shared library (`libmain.so`) loaded by SDL's activity
  (`java/com/recompiledsteel/app/RealSteelActivity.java`).

Prior art: luisxl15's Skate 3 Android port showed the way and listed the
traps (credited in CREDITS.md).
