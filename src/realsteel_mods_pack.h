// realsteel - mod robots loaded by the game itself (2026-10-09).
//
// At start-up the game looks for robot folders in <exe folder>/mods (or
// --realsteel_mods_dir) and, when they changed, writes the add-on package
// that holds them into the user data folder next to the official DLC plus a
// patched copy of the title update, then points the runtime at that copy.
// A robot folder is robot.txt ("name = ...") with the robot's OBJ models and
// PNG textures, made into game files here from the player's own copy of the
// base set (realsteel_mods_build.h); older mods may bring a built/ folder
// instead. This is the C++ port of tools/mods/build_part_package.py.

#pragma once

#include <string>
#include <vector>

#include <rex/rex_app.h>

namespace realsteel::mods {

// Installs the robots and adjusts paths.update_data_root. Never throws;
// problems are logged and the game starts without the mods.
void Install(rex::PathConfig& paths);

// Robots that were not loaded and why, for the player (empty: all fine).
std::vector<std::string> Notices();

}  // namespace realsteel::mods
