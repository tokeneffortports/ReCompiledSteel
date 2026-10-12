// What the setup program does: puts the player's own Real Steel files where
// the game looks for them.
//
//   <game folder>/game/   the game package (default.xex, pac/, sound/, ...)
//                         plus the title update's default.xexp
//   <game folder>/tu/     the title update's data files (pac/)
//   <user data>/0000000000000000/584111E0/00000002/<package>/   each add-on
//   <user data>/0000000000000000/584111E0/Headers/00000002/<package>.header
//
// <user data> is where the game keeps saves: Documents\realsteel on Windows,
// ~/.local/share/realsteel on Linux (the game's own default).
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace rs::setup {

constexpr uint32_t kTitleId = 0x584111E0;       // Real Steel
constexpr uint32_t kTypeGame = 0x000D0000;      // Xbox Live Arcade title
constexpr uint32_t kTypeUpdate = 0x000B0000;    // title update
constexpr uint32_t kTypeAddon = 0x00000002;     // marketplace content (DLC)

// Packages found under a folder (an Xbox 360 "Content" folder, a USB stick
// dump, ...), told apart by their headers.
struct Scan {
  std::filesystem::path game;
  std::filesystem::path update;
  std::vector<std::filesystem::path> addons;
  size_t other_titles = 0;   // packages of other games, ignored
  size_t files_looked_at = 0;
};
Scan ScanFolder(const std::filesystem::path& dir);

// Add-on packages directly inside |dir| (or |dir| itself when it is a file).
std::vector<std::filesystem::path> AddonsIn(const std::filesystem::path& dir);

// Where the game keeps saves and add-ons.
std::filesystem::path DefaultUserRoot();

struct Plan {
  std::filesystem::path game;                  // the game package (required)
  std::filesystem::path update;                // the title update package (optional)
  std::vector<std::filesystem::path> addons;   // add-on packages (optional)
  std::filesystem::path dest;                  // the game folder (realsteel.exe's)
  std::filesystem::path user_root;             // DefaultUserRoot() unless overridden
};

using Log = std::function<void(const std::string& line)>;
using Progress = std::function<void(double fraction, const std::string& what)>;

// Runs the plan. Every step logs a line; false with |error| set on failure.
bool Run(const Plan& plan, const Log& log, const Progress& progress, std::string* error);

// A one-line description of a package for the user ("REAL STEEL (game)").
std::string Describe(const std::filesystem::path& package);

}  // namespace rs::setup
