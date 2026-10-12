#!/usr/bin/env bash
# Builds the ReXGlue SDK for Android (arm64) and installs it where
# android/build_android.sh looks for it. Run from WSL or Linux.
#
#   android/build_sdk.sh            fetch (first time), patch, configure, build, install
#
# The SDK source is the commit the desktop SDKs were built from (509ed5b) plus
# thirdparty/rexglue-android.patch. Needs: git, cmake, ninja, python3, and the
# Android NDK (ANDROID_HOME, default ~/android/sdk, with ndk;29.0.14206865).
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "$SCRIPT_DIR/.." && pwd)"
ANDROID_HOME="${ANDROID_HOME:-$HOME/android/sdk}"
NDK="${ANDROID_NDK:-$ANDROID_HOME/ndk/29.0.14206865}"
SRC="${REX_SDK_SRC:-$HOME/android/rexglue-sdk}"
OUT="${REX_SDK_BUILD:-$HOME/android/build-sdk}"
PREFIX="${REX_ANDROID:-$HOME/android/rexglue-android-arm64}"
COMMIT=509ed5bffcffefbdf77ad7132db8dbc2dd570b32
PATCH="$REPO/thirdparty/rexglue-android.patch"

[[ -f "$NDK/build/cmake/android.toolchain.cmake" ]] || { echo "NDK not found: $NDK" >&2; exit 1; }

if [[ ! -d "$SRC/.git" ]]; then
    git clone --quiet https://github.com/rexglue/rexglue-sdk.git "$SRC"
fi
cd "$SRC"
if ! git diff --quiet HEAD -- 2>/dev/null || [[ -n "$(git ls-files --others --exclude-standard -- include src)" ]]; then
    echo "== $SRC already has local changes: building them as they are"
else
    git checkout --quiet "$COMMIT"
    git submodule update --init --recursive --quiet
    git apply --whitespace=nowarn "$PATCH"
    echo "== SDK at ${COMMIT:0:7} with the Android patch"
fi

# No build-machine paths in the library (__FILE__ in log and assert lines).
PREFIX_MAP="-ffile-prefix-map=$SRC=rexglue-sdk -ffile-prefix-map=$HOME=~"
cmake -S "$SRC" -B "$OUT" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-30 -DANDROID_STL=c++_shared \
    -DCMAKE_BUILD_TYPE=Release \
    -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_BUILD_TESTS=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF \
    -DCMAKE_C_FLAGS="$PREFIX_MAP" -DCMAKE_CXX_FLAGS="$PREFIX_MAP"
cmake --build "$OUT" -j "${JOBS:-8}"
cmake --install "$OUT" --prefix "$PREFIX" > /dev/null
echo "== installed to $PREFIX"
