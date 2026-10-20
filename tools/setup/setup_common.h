// Shared by the Windows and the Linux front ends of the setup program.
#pragma once

#include "install.h"

#include <filesystem>
#include <string>
#include <vector>

namespace rs::setup {

struct Args {
  std::filesystem::path game;     // --game <package>
  std::filesystem::path update;   // --update <package>
  std::filesystem::path addons;   // --addons <folder or package>
  std::filesystem::path scan;     // --scan <folder>: find all three in it
  std::filesystem::path dest;     // --dest <folder> (default: the program's folder)
  std::filesystem::path user_root;  // --user-root <folder> (default: the game's)
  std::filesystem::path fighters; // --import-fighters <folder>: another Yuke's game to import fighters from
  bool cli = false;               // --cli: questions on the terminal, no dialogs
  bool help = false;
  bool yes = false;               // --yes: no confirmation
  std::string error;              // a bad option
};
Args ParseArgs(const std::vector<std::string>& argv);
std::string Usage();

// The folder the setup program runs from (where the game lives).
std::filesystem::path ProgramDir();

// Fills the plan from the arguments (scan folder first, explicit choices
// win). Returns false with |error| when nothing usable was given.
bool PlanFromArgs(const Args& args, Plan* plan, std::string* error);

// Runs a plan with output on stdout; the exit code of the program.
int RunConsole(const Plan& plan);

// Imports the fighters of another Yuke's game (--import-fighters) into the
// player's data, from an installed Real Steel; output on stdout.
int RunImportConsole(const Args& args);

// Terminal questions (--cli, or no dialog tool): fills what the arguments
// left empty. Returns false when the user gave up.
bool AskOnTerminal(Args* args);

}  // namespace rs::setup
