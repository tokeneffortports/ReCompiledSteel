#!/usr/bin/env bash
# Development loop on a USB-connected device (Git Bash on Windows, or Linux):
# install the APK (app data and /sdcard/RealSteel are kept), start the game
# through the launcher (taps Start at its position on a 1280x960 landscape
# screen - adjust START_X/START_Y for others), wait, save the device log.
#
#   android/dev/nova_run.sh [seconds] [install|noinstall]
#
# ADB: adb on PATH, or set ADB (the Windows platform-tools adb.exe from Git
# Bash: paths given to it must be Windows-style, hence MSYS_NO_PATHCONV).
export MSYS_NO_PATHCONV=1
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
A="${ADB:-adb}"
VERSION=$(sed -n 's/.*kVersion = "\(.*\)".*/\1/p' "$REPO/src/realsteel_version.h")
APK="$REPO/release/ReCompiledSteel-android-arm64-$VERSION.apk"
command -v cygpath > /dev/null && APK="$(cygpath -m "$APK")"
OUT="$REPO/rs_renders/nova"
WAIT=${1:-40}
mkdir -p "$OUT"
if [ "${2:-install}" = install ]; then
  "$A" install -r "$APK" | tail -1
fi
"$A" shell am force-stop com.recompiledsteel.app
"$A" logcat -c
"$A" shell am start -n com.recompiledsteel.app/.LauncherActivity > /dev/null
sleep 3
"$A" shell input tap "${START_X:-100}" "${START_Y:-458}"
sleep "$WAIT"
"$A" logcat -d -v threadtime > "$OUT/logcat.txt"
echo "pid: $("$A" shell pidof com.recompiledsteel.app)"
grep -a "Abort message\|F DEBUG   :     #0\|native-present\]" "$OUT/logcat.txt" | head -20
echo "renderer: $("$A" shell tail -2 /sdcard/RealSteel/rs_native_print.log)"
