#!/usr/bin/env bash
# Packages ReCompiled Steel for Android: libmain.so (android/build_android.sh)
# + the runtime + SDL's Java activity into a signed APK, with the Android
# build-tools directly (no Gradle).
#
#   android/build_apk.sh            -> release/ReCompiledSteel-android-arm64-<version>.apk
#
# Signing key: $APK_KEYSTORE (default ~/android/debug.keystore, created on
# first use; a debug key - fine for side-loading).
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ANDROID_HOME="${ANDROID_HOME:-$HOME/android/sdk}"
NDK="${ANDROID_NDK:-$ANDROID_HOME/ndk/29.0.14206865}"
BT="$ANDROID_HOME/build-tools/35.0.0"
PLATFORM_JAR="$ANDROID_HOME/platforms/android-35/android.jar"
REX_ANDROID="${REX_ANDROID:-$HOME/android/rexglue-android-arm64}"
SDL_JAVA="${SDL_JAVA:-$HOME/android/rexglue-sdk/thirdparty/sdl3/android-project/app/src/main/java}"
BUILD_ROOT="${REALSTEEL_ANDROID_BUILD_ROOT:-$HOME/realsteel-android-build}"
LIBMAIN="$BUILD_ROOT/realsteelrecomp/out/android-arm64/libmain.so"
WORK="$BUILD_ROOT/apk"
export JAVA_HOME="${JAVA_HOME:-$HOME/android/jdk}"
export PATH="$JAVA_HOME/bin:$PATH"
KEYSTORE="${APK_KEYSTORE:-$HOME/android/debug.keystore}"
STRIP="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
LIBCXX="$NDK/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so"

VERSION=$(sed -n 's/.*kVersion = "\(.*\)".*/\1/p' "$SRC/src/realsteel_version.h")
IFS=. read -r MAJ MIN PAT <<< "$VERSION"
VERSION_CODE=$(( MAJ * 10000 + MIN * 100 + PAT ))
OUT_APK="$SRC/release/ReCompiledSteel-android-arm64-$VERSION.apk"

for f in "$LIBMAIN" "$REX_ANDROID/lib/librexruntime.so" "$LIBCXX" "$PLATFORM_JAR" "$BT/aapt2"; do
    [[ -f "$f" ]] || { echo "missing: $f" >&2; exit 1; }
done
[[ -d "$SDL_JAVA/org/libsdl/app" ]] || { echo "SDL Java sources not found: $SDL_JAVA" >&2; exit 1; }

rm -rf "$WORK"
mkdir -p "$WORK/compiled" "$WORK/classes" "$WORK/dex" "$WORK/pkg/lib/arm64-v8a"

echo "== Resources and manifest (aapt2) =="
"$BT/aapt2" compile --dir "$SCRIPT_DIR/res" -o "$WORK/compiled/res.zip"
mkdir -p "$WORK/gen"
"$BT/aapt2" link -o "$WORK/base.apk" -I "$PLATFORM_JAR" \
    --manifest "$SCRIPT_DIR/AndroidManifest.xml" --java "$WORK/gen" \
    --min-sdk-version 30 --target-sdk-version 35 \
    --version-code "$VERSION_CODE" --version-name "$VERSION" \
    "$WORK/compiled/res.zip"

echo "== Java: SDL's activity + ours (javac, d8) =="
mkdir -p "$WORK/gen/com/recompiledsteel/app"
cat > "$WORK/gen/com/recompiledsteel/app/BuildConfigVersion.java" <<JAVA
package com.recompiledsteel.app;
final class BuildConfigVersion { static final String NAME = "$VERSION"; private BuildConfigVersion() {} }
JAVA
find "$SDL_JAVA" "$SCRIPT_DIR/java" "$WORK/gen" -name '*.java' > "$WORK/sources.txt"
javac -nowarn -source 17 -target 17 -encoding UTF-8 -classpath "$PLATFORM_JAR" \
    -d "$WORK/classes" @"$WORK/sources.txt" 2>&1 | grep -v "^Note:" || true
find "$WORK/classes" -name '*.class' > "$WORK/classes.txt"
"$BT/d8" --release --min-api 30 --lib "$PLATFORM_JAR" --output "$WORK/dex" @"$WORK/classes.txt"

echo "== Native libraries (stripped) =="
"$STRIP" --strip-debug -o "$WORK/pkg/lib/arm64-v8a/libmain.so" "$LIBMAIN"
"$STRIP" --strip-debug -o "$WORK/pkg/lib/arm64-v8a/librexruntime.so" "$REX_ANDROID/lib/librexruntime.so"
cp "$LIBCXX" "$WORK/pkg/lib/arm64-v8a/"
# libadrenotools and its hooks (optional custom GPU driver; only loaded when
# a player chooses a driver in the launcher, see src/realsteel_gpu_driver.cpp).
for lib in libadrenotools.so libhook_impl.so libmain_hook.so; do
    found=$(find "$(dirname "$LIBMAIN")" -name "$lib" -print -quit)
    [[ -n "$found" ]] || { echo "missing $lib (android/build_android.sh builds it)" >&2; exit 1; }
    "$STRIP" --strip-debug -o "$WORK/pkg/lib/arm64-v8a/$lib" "$found"
done
# The crash catcher, loaded before everything else (android/native/rscrash.c).
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" --target=aarch64-linux-android30     -shared -fPIC -O2 -funwind-tables -Wl,-z,max-page-size=16384     -o "$WORK/pkg/lib/arm64-v8a/librscrash.so" "$SCRIPT_DIR/native/rscrash.c" -llog
# The installer: the PC setup programs' code (tools/setup) behind a JNI bridge
# (android/native/setup_jni.cpp), for the launcher's "Install game files".
"$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/clang++" --target=aarch64-linux-android30 \
    -std=c++20 -shared -fPIC -O2 -static-libstdc++ -Wl,-z,max-page-size=16384 \
    -ffile-prefix-map="$SRC"=. -ffile-prefix-map="$HOME"=~ \
    -o "$WORK/pkg/lib/arm64-v8a/librssetup.so" \
    "$SRC/tools/setup/stfs.cpp" "$SRC/tools/setup/install.cpp" "$SCRIPT_DIR/native/setup_jni.cpp" -llog
"$STRIP" --strip-debug "$WORK/pkg/lib/arm64-v8a/librssetup.so"
cp "$WORK/dex/classes.dex" "$WORK/pkg/"
# Our licence and the third-party notices the libraries need, as on the PC.
mkdir -p "$WORK/pkg/assets/licenses"
cp "$SRC/LICENSE" "$WORK/pkg/assets/"
cp "$SRC/packaging/licenses/"* "$WORK/pkg/assets/licenses/"
ls -la "$WORK/pkg/lib/arm64-v8a"

echo "== Assembling, aligning, signing =="
python3 - "$WORK/base.apk" "$WORK/unsigned.apk" "$WORK/pkg" <<'PY'
import os, shutil, sys, time, zipfile
base, out, pkg = sys.argv[1:4]
shutil.copyfile(base, out)
with zipfile.ZipFile(out, 'a') as z:
    for root, _, files in os.walk(pkg):
        for name in sorted(files):
            path = os.path.join(root, name)
            arc = os.path.relpath(path, pkg).replace(os.sep, '/')
            # UTC entry times: nothing about the build machine's time zone.
            info = zipfile.ZipInfo(arc, date_time=time.gmtime(os.path.getmtime(path))[:6])
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            with open(path, 'rb') as src:
                z.writestr(info, src.read())
PY
"$BT/zipalign" -f -P 16 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"
if [[ ! -f "$KEYSTORE" ]]; then
    keytool -genkeypair -keystore "$KEYSTORE" -alias androiddebugkey \
        -storepass android -keypass android -keyalg RSA -keysize 2048 -validity 10000 \
        -dname "CN=Android Debug,O=Android,C=US" >/dev/null 2>&1
fi
mkdir -p "$SRC/release"
"$BT/apksigner" sign --ks "$KEYSTORE" --ks-pass pass:android --key-pass pass:android \
    --out "$OUT_APK" "$WORK/aligned.apk"
"$BT/apksigner" verify "$OUT_APK" && echo "signed: $OUT_APK"
ls -la "$OUT_APK"
