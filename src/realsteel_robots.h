// realsteel - full robots and the robot lab (see realsteel_robots.cpp).
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace realsteel::robots {

// The robot-wide figures the lab can set ("feel"): Core HP, attack, defense,
// speeds, recoveries. key = robot.txt [feel] key, stat = figure id of
// sub_820913D8, word = 4-byte word of the game's totals block.
struct LabFeel {
  const char* key;
  const char* label;
  int stat;
  int word;
  bool is_float;
};
const std::vector<LabFeel>& LabFeels();

struct LabRobot {
  int index = 0;
  std::string name;
  uint32_t ids[11] = {};
  std::map<int, double> game;     // stat id -> the game's own figure (without the lab)
  std::map<int, int> feel;        // stat id -> value set in the lab
  std::map<int, double> totals;   // stat id -> robot-wide figure as last shown
};

// Thread-safe copies / edits for the overlay page.
std::vector<LabRobot> LabRobots();
void LabSet(int index, int stat, int value);
void LabClear(int index, int stat);
void LabReset(int index);
void LabTotal(int robot, int stat, double value);
std::string LabSave(int index);
const char* LabFigureName(int stat);   // robot-wide figure ids 0..12 as on the versus screen

}  // namespace realsteel::robots
