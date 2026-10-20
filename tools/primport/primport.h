// Fighter import from other Yuke's Xbox 360 games: reads the player's own
// game packages and turns their robot fighters into Real Steel full robots
// (folders the game loads like mods: robot.txt, character.bin, icons, own
// motions and move list). Nothing of the other game ships with Real Steel;
// this code only knows the file formats and where things are.
//
// Supported: Pacific Rim (title 584112C1) - its five Jaegers (two of them
// from its add-ons, when present).
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace rs::primport {

using Log = std::function<void(const std::string& line)>;
using Progress = std::function<void(double fraction, const std::string& what)>;

struct Found {
  std::string game;                          // display name of the game found ("" = none)
  std::vector<std::string> fighters;         // names of the fighters that can be imported
};

// What a folder (searched recursively for Xbox 360 packages, or an extracted
// game folder) offers.
Found Scan(const std::filesystem::path& from);

// Imports every fighter found in |from| into |out_dir| (one folder each;
// existing imports are replaced). |rs_game_dir|: Real Steel's installed game
// files (pac/...), whose base robot, motions, move lists and icons the
// fighters are built on.
bool Import(const std::filesystem::path& from, const std::filesystem::path& rs_game_dir, const std::filesystem::path& out_dir,
            const Log& log, const Progress& progress, std::string* error);

// The folder imported fighters go in, under the player's Real Steel data.
inline std::filesystem::path ImportDir(const std::filesystem::path& user_root) { return user_root / "imported"; }

}  // namespace rs::primport
