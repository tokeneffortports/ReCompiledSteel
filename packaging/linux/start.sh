#!/usr/bin/env bash
# Real Steel on Linux / Steam Deck: native renderer, no GPU emulation.
# Expects the game data in game/ and tu/ beside this script. Settings live on
# the in-game page (both stick clicks, or F1) and are saved in
# realsteel_settings.cfg; extra arguments given here win over the saved ones.
cd -- "$(dirname -- "${BASH_SOURCE[0]}")" || exit 1
chmod +x ./realsteel 2>/dev/null
export LD_LIBRARY_PATH="$PWD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# The runtime leaves its guest memory image in /dev/shm (RAM) when it exits;
# clear it before and after so runs do not pile up in memory.
rm -f /dev/shm/xenia_memory_* 2>/dev/null
./realsteel \
  --game_data_root="$PWD/game" \
  --update_data_root="$PWD/tu" \
  --license_mask=1 \
  --log_file=rex_latest.log \
  --log_level=info \
  --fullscreen=true \
  --realsteel_native_gpu=true \
  --realsteel_native_present=true "$@"
status=$?
rm -f /dev/shm/xenia_memory_* 2>/dev/null
exit $status
