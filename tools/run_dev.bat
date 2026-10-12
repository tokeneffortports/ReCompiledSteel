@echo off
rem Runs the build from this tree with the game files in work/ (docs/building.md).
rem Extra arguments are passed through, e.g. --fullscreen=true.
cd /d "%~dp0.."
out\build\win-amd64-release\realsteel.exe ^
  --game_data_root=%CD%\work\game ^
  --update_data_root=%CD%\work\tu ^
  --realsteel_mods_dir=%CD%\mods ^
  --log_file=rex_latest.log %*
