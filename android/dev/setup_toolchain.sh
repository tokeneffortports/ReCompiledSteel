#!/usr/bin/env bash
# Fetches the Android toolchain into ~/android (WSL or Linux x64): JDK 17,
# the command-line tools, then platform-tools, platforms;android-35,
# build-tools;35.0.0 and ndk;29.0.14206865. sdkmanager asks you to accept the
# Android SDK licences.
set -euo pipefail
A="${ANDROID_ROOT:-$HOME/android}"
mkdir -p "$A/dl" "$A/sdk/cmdline-tools"
cd "$A/dl"

if [ ! -x "$A/jdk/bin/java" ]; then
  curl -fsSL -o jdk17.tar.gz \
    "https://api.adoptium.net/v3/binary/latest/17/ga/linux/x64/jdk/hotspot/normal/eclipse"
  mkdir -p "$A/jdk"
  tar -xzf jdk17.tar.gz -C "$A/jdk" --strip-components=1
fi
"$A/jdk/bin/java" -version 2>&1 | head -1

if [ ! -x "$A/sdk/cmdline-tools/latest/bin/sdkmanager" ]; then
  curl -fsSL -o cmdline-tools.zip \
    "https://dl.google.com/android/repository/commandlinetools-linux-11076708_latest.zip"
  rm -rf "$A/sdk/cmdline-tools/latest" "$A/dl/ct"
  # unzip drops the exec bits on some systems; keep them.
  python3 - "$A/dl/cmdline-tools.zip" "$A/dl/ct" <<'PY'
import os, sys, zipfile
with zipfile.ZipFile(sys.argv[1]) as z:
    for info in z.infolist():
        path = z.extract(info, sys.argv[2])
        mode = (info.external_attr >> 16) & 0o777
        if mode:
            os.chmod(path, mode)
PY
  mv "$A/dl/ct/cmdline-tools" "$A/sdk/cmdline-tools/latest"
fi

export JAVA_HOME="$A/jdk"
SDKM="$A/sdk/cmdline-tools/latest/bin/sdkmanager"
"$SDKM" --sdk_root="$A/sdk" --licenses
"$SDKM" --sdk_root="$A/sdk" "platform-tools" "platforms;android-35" "build-tools;35.0.0" "ndk;29.0.14206865"
echo "== toolchain in $A (ANDROID_HOME=$A/sdk, JAVA_HOME=$A/jdk)"
