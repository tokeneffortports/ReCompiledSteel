#!/usr/bin/env bash
# After editing the SDK source for Android (~/android/rexglue-sdk), writes the
# changes back to thirdparty/rexglue-android.patch (new files included).
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
SRC="${REX_SDK_SRC:-$HOME/android/rexglue-sdk}"
cd "$SRC"
git ls-files --others --exclude-standard -- include src | xargs -r git add -N
git diff -- . ':(exclude)out' > "$REPO/thirdparty/rexglue-android.patch"
git diff --stat -- . ':(exclude)out' | tail -1
