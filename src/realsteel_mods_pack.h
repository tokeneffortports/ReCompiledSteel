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

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <rex/rex_app.h>

namespace realsteel::mods {

// Installs the robots and adjusts paths.update_data_root. Never throws;
// problems are logged and the game starts without the mods.
void Install(rex::PathConfig& paths);

// Robots that were not loaded and why, for the player (empty: all fine).
std::vector<std::string> Notices();

// Robot indices of the installed full robots (kind = full), for the robot
// database hook to enable (realsteel_robots.cpp).
std::vector<int> FullRobotIndices();

// True for a full robot that stays out of character select (robot.txt
// select = 0: e.g. a boosted copy that only fights in a single-player stage).
bool FullRobotHidden(int robot_index);

// A full robot's own part ids (slots 1..10 in ids[1..10]); false if the
// robot index is not a full robot's.
bool FullRobotParts(int robot_index, uint32_t ids[11]);

// The installed full robots: (robot index, name).
std::vector<std::pair<int, std::string>> FullRobots();

// A full robot's folder (its robot.txt is there); empty if not a full robot.
std::string FullRobotFolder(int robot_index);

// The full robots' motions (one bank, one event bank; empty if none), for
// the robots hook to install as motion slot 2.
const std::vector<uint8_t>& FullRobotMotions();
const std::vector<uint8_t>& FullRobotMotionEvents();
// A full robot's own motion for a move (act): true and its act in the robots'
// bank, or false (the move's own motion).
bool FullRobotMotionAct(int robot_index, uint32_t act, uint32_t* own);

}  // namespace realsteel::mods
