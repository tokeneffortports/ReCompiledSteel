#!/usr/bin/env bash
# Rebuilds the patched XenosRecomp, writes its changes back to
# thirdparty/XenosRecomp-realsteel.patch, and regenerates
# generated/shader_cache.cpp (all platforms use it, not just Android).
# XenosRecomp checkout: tools/shaders/setup_xenosrecomp.py --dir <XENOS>.
#
#   XENOS=~/android/XenosRecomp android/dev/shader_cache.sh
set -uo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
R="$(cd -- "$SCRIPT_DIR/../.." && pwd)"
X="${XENOS:-$HOME/android/XenosRecomp}"
LOG="${ANDROID_ROOT:-$HOME/android}"
cmake --build "$X/build" > "$LOG/xr-build.log" 2>&1 || {
  grep -n "error" "$LOG/xr-build.log" | head; echo "XENOSRECOMP BUILD FAILED"; exit 1; }
echo "XenosRecomp built"
(cd "$X" && git diff -- . ':(exclude)XenosRecomp/xenos_vfetch_scan.h' > "$R/thirdparty/XenosRecomp-realsteel.patch")
echo "patch: $(wc -l < "$R/thirdparty/XenosRecomp-realsteel.patch") lines"
cd "$R"
python3 tools/shaders/build_shader_cache.py --game work/game --tu work/tu \
    --xenosrecomp "$X/build/XenosRecomp/XenosRecomp" --out generated/shader_cache.cpp > "$LOG/shader-cache.log" 2>&1
echo "shader cache exit $?"
tail -5 "$LOG/shader-cache.log"
echo "warnings: $(grep -c "unhandled type\|error" "$LOG/shader-cache.log")"
