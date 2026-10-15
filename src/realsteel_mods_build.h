// realsteel - mod robots built by the game from their source files (2026-10-15).
//
// A robot folder holds only the creator's own files: robot.txt, one OBJ model
// and PNG textures per part (and optionally <part>_weights.txt and
// skeleton_scale.txt). The game files the robot needs are made here at
// start-up, each from the matching entry of the base set's own archives - the
// player's copy - so a mod never carries data taken from the game.
//
// C++ port of tools/mods/robot_folder.py build + jboy_write.py + pngio.py.
// The Python stays the reference: the files made here are byte for byte the
// ones robot_folder.py writes (checked with REALSTEEL_MODS_DUMP, see the .cpp).

#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace realsteel::mods {

class RobotSource {
 public:
  explicit RobotSource(std::filesystem::path folder);
  ~RobotSource();

  // The folder has source models (body.obj ...).
  static bool HasSource(const std::filesystem::path& folder);
  // The files that make up the robot (for the install signature).
  static std::vector<std::filesystem::path> SourceFiles(const std::filesystem::path& folder);

  // Reads and checks every model; problems (empty: all good) are what keeps
  // the robot out of the game, worded for the player.
  std::vector<std::string> Check();

  // One part file made from its template entry (the base set's file with the
  // same slot / kind / item number): slot 1..7, kind 'M' or 'T', item as in
  // ed<slot>_<kind>_<item>.bin. nullopt: an entry the robot does not replace.
  std::optional<std::vector<uint8_t>> Build(int slot, char kind, uint32_t item,
                                            const std::vector<uint8_t>& template_data);

  const std::filesystem::path& folder() const { return folder_; }

 private:
  struct Part;
  std::filesystem::path folder_;
  double skeleton_scale_ = 1.0;
  std::map<int, std::unique_ptr<Part>> parts_;  // by slot, after Check()
};

}  // namespace realsteel::mods
