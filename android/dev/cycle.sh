#!/usr/bin/env bash
# One Android iteration from WSL/Linux: SDK (build, install, patch export),
# game library, APK. Logs go to ~/android/*.log.
#
#   android/dev/cycle.sh [game-only]
set -uo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ANDROID_DIR="$(cd -- "$SCRIPT_DIR/.." && pwd)"
LOGS="${ANDROID_ROOT:-$HOME/android}"
mkdir -p "$LOGS"
if [ "${1:-}" != "game-only" ]; then
  bash "$ANDROID_DIR/build_sdk.sh" > "$LOGS/sdk-build.log" 2>&1 || {
    grep -n "error:\|CMake Error\|FAILED:" "$LOGS/sdk-build.log" | head -20; echo "SDK BUILD FAILED"; exit 1; }
  echo "SDK built and installed"
  bash "$SCRIPT_DIR/export_sdk_patch.sh" | tail -1
fi
bash "$ANDROID_DIR/build_android.sh" > "$LOGS/game-build.log" 2>&1 || {
  grep -n "error:\|FAILED:" "$LOGS/game-build.log" | head -20; echo "GAME BUILD FAILED"; exit 1; }
echo "game library built"
bash "$ANDROID_DIR/build_apk.sh" 2>&1 | grep "signed:\|error" || { echo "APK FAILED"; exit 1; }
