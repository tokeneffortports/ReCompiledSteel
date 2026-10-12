#!/usr/bin/env bash
# Runs the Linux build from this tree with the game files in work/ (docs/building.md).
cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." || exit 1
BUILD=${REALSTEEL_BUILD:-out/build/linux-amd64-release}
export LD_LIBRARY_PATH="$PWD/$BUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
rm -f /dev/shm/xenia_memory_* 2>/dev/null
"$BUILD/realsteel" --game_data_root="$PWD/work/game" --update_data_root="$PWD/work/tu" \
  --realsteel_mods_dir="$PWD/mods" --log_file=rex_latest.log "$@"
