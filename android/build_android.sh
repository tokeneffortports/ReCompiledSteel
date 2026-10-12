#!/usr/bin/env bash
# Builds the Android (arm64) game library, libmain.so, from WSL or Linux.
#
#   android/build_android.sh            configure (first time) + build
#
# Needs (see android/README.md):
#   ANDROID_HOME   Android SDK with ndk;29.0.14206865 (default ~/android/sdk)
#   REX_ANDROID    the ReXGlue SDK built for Android and installed
#                  (default ~/android/rexglue-android-arm64)
#   REX_HOST       a host (linux-amd64) ReXGlue SDK for code generation
#                  (default: the copy build_linux.sh mirrors, or /mnt/c/rexglue/linux-amd64)
#   work/game, work/tu   your own game files, as for the desktop build
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
SRC="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ANDROID_HOME="${ANDROID_HOME:-$HOME/android/sdk}"
NDK="${ANDROID_NDK:-$ANDROID_HOME/ndk/29.0.14206865}"
REX_ANDROID="${REX_ANDROID:-$HOME/android/rexglue-android-arm64}"
BUILD_ROOT="${REALSTEEL_ANDROID_BUILD_ROOT:-$HOME/realsteel-android-build}"
PROJ="$BUILD_ROOT/realsteelrecomp"
OUT="$PROJ/out/android-arm64"

# Host code generator. NTFS loses exec bits, so use (or make) a Linux-side copy.
REX_HOST="${REX_HOST:-}"
if [[ -z "$REX_HOST" ]]; then
    for candidate in "$HOME/realsteel-deck-build/rexglue/linux-amd64" /mnt/c/rexglue/linux-amd64 "$HOME/rexglue/linux-amd64"; do
        [[ -f "$candidate/bin/rexglue" ]] && REX_HOST="$candidate" && break
    done
fi
[[ -n "$REX_HOST" ]] || { echo "Host ReXGlue SDK not found (set REX_HOST)" >&2; exit 1; }
HOST_REXGLUE="$BUILD_ROOT/host-rexglue/rexglue"
mkdir -p "$BUILD_ROOT/host-rexglue"
rsync -a "$REX_HOST/" "$BUILD_ROOT/host-rexglue/sdk/"
chmod +x "$BUILD_ROOT/host-rexglue/sdk/bin/"* 2>/dev/null || true
HOST_REXGLUE="$BUILD_ROOT/host-rexglue/sdk/bin/rexglue"

[[ -f "$NDK/build/cmake/android.toolchain.cmake" ]] || { echo "NDK not found: $NDK" >&2; exit 1; }
[[ -f "$REX_ANDROID/lib/cmake/rexglue/rexglueConfig.cmake" ]] || { echo "Android ReXGlue SDK not found: $REX_ANDROID" >&2; exit 1; }
[[ -f "$SRC/work/game/default.xex" ]] || { echo "Game files not found in work/game (see docs/building.md)" >&2; exit 1; }

echo "== Mirroring the project to $PROJ =="
mkdir -p "$PROJ/generated"
cd "$SRC"
rsync -a CMakeLists.txt CMakePresets.json realsteel_manifest.toml realsteel_overrides.toml "$PROJ/"
rsync -a --delete src thirdparty "$PROJ/"
rsync -a --delete --exclude='build' --exclude='build-*' tools "$PROJ/"
rsync -a --delete work/game work/tu "$PROJ/work/" --mkpath
rsync -a generated/rexglue.cmake "$PROJ/generated/"
[[ -f generated/shader_cache.cpp ]] && rsync -a generated/shader_cache.cpp "$PROJ/generated/"

echo "== Configuring (NDK $(basename "$NDK"), android-30, arm64-v8a) =="
# No build-machine paths in the library: the SDK's headers are spelled
# relative to its folder, anything else under $HOME from ~ (CMakeLists.txt
# maps the project itself).
PREFIX_MAP="-ffile-prefix-map=$REX_ANDROID=rexglue -ffile-prefix-map=$HOME=~"
cmake -S "$PROJ" -B "$OUT" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$REX_ANDROID" \
    -DCMAKE_FIND_ROOT_PATH="$REX_ANDROID" \
    -DRS_HOST_REXGLUE="$HOST_REXGLUE" \
    -DCMAKE_C_FLAGS="$PREFIX_MAP" -DCMAKE_CXX_FLAGS="$PREFIX_MAP"

echo "== Building libmain.so =="
cmake --build "$OUT" --target realsteel -j "${JOBS:-8}"
ls -la "$OUT/libmain.so"
