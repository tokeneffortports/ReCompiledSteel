#include "setup_common.h"

#include <cstdio>
#include <iostream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace rs::setup {

namespace fs = std::filesystem;

std::string Usage() {
  return
      "ReCompiled Steel setup - copies your own Real Steel files into the game folder.\n"
      "\n"
      "Without options a window (or dialogs) asks for the files. Options:\n"
      "  --scan <folder>       look through a folder (your Xbox 360 Content folder,\n"
      "                        a USB stick) for the game, its update and add-ons\n"
      "  --game <package>      the game package (Content\\...\\584111E0\\000D0000\\...)\n"
      "  --update <package>    the title update package (...\\000B0000\\...)\n"
      "  --addons <folder>     folder of add-on packages (...\\00000002\\), or one package\n"
      "  --dest <folder>       where the game is (default: this program's folder)\n"
      "  --user-root <folder>  where saves and add-ons go (default: the game's)\n"
      "  --cli                 ask on the terminal instead of with dialogs\n"
      "  --yes                 do not ask for confirmation\n"
      "  --help\n";
}

Args ParseArgs(const std::vector<std::string>& argv) {
  Args args;
  for (size_t i = 1; i < argv.size(); ++i) {
    const std::string& a = argv[i];
    auto value = [&](fs::path* out) {
      if (i + 1 >= argv.size()) {
        args.error = a + " needs a value";
        return;
      }
      *out = fs::path(argv[++i]);
    };
    if (a == "--game") value(&args.game);
    else if (a == "--update") value(&args.update);
    else if (a == "--addons" || a == "--dlc") value(&args.addons);
    else if (a == "--scan") value(&args.scan);
    else if (a == "--dest") value(&args.dest);
    else if (a == "--user-root") value(&args.user_root);
    else if (a == "--cli") args.cli = true;
    else if (a == "--yes" || a == "-y") args.yes = true;
    else if (a == "--help" || a == "-h" || a == "/?") args.help = true;
    else args.error = "unknown option " + a;
    if (!args.error.empty()) break;
  }
  return args;
}

fs::path ProgramDir() {
#ifdef _WIN32
  wchar_t buf[MAX_PATH * 2];
  const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(sizeof(buf) / sizeof(buf[0])));
  if (n > 0 && n < sizeof(buf) / sizeof(buf[0])) return fs::path(buf).parent_path();
#else
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = 0;
    return fs::path(buf).parent_path();
  }
#endif
  return fs::current_path();
}

bool PlanFromArgs(const Args& args, Plan* plan, std::string* error) {
  if (!args.scan.empty()) {
    const Scan found = ScanFolder(args.scan);
    if (found.game.empty() && found.update.empty() && found.addons.empty()) {
      if (error) {
        *error = "No Real Steel packages under " + args.scan.string() + " (" +
                 std::to_string(found.files_looked_at) + " files looked at" +
                 (found.other_titles ? ", " + std::to_string(found.other_titles) + " packages of other games" : "") + ")";
      }
      return false;
    }
    plan->game = found.game;
    plan->update = found.update;
    plan->addons = found.addons;
  }
  if (!args.game.empty()) plan->game = args.game;
  if (!args.update.empty()) plan->update = args.update;
  if (!args.addons.empty()) plan->addons = AddonsIn(args.addons);
  plan->dest = args.dest.empty() ? ProgramDir() : args.dest;
  plan->user_root = args.user_root;
  if (plan->game.empty()) {
    if (error) *error = "No game package: give --game <package> or --scan <folder>";
    return false;
  }
  return true;
}

int RunConsole(const Plan& plan) {
  std::printf("Game:         %s\n", plan.game.string().c_str());
  std::printf("Title update: %s\n", plan.update.empty() ? "(none)" : plan.update.string().c_str());
  std::printf("Add-ons:      %zu package(s)\n", plan.addons.size());
  std::printf("Game folder:  %s\n", plan.dest.string().c_str());
  std::printf("Saves/add-ons: %s\n",
              (plan.user_root.empty() ? DefaultUserRoot() : plan.user_root).string().c_str());
  std::fflush(stdout);
  std::string error;
  int last_percent = -1;
  const bool ok = Run(
      plan, [](const std::string& line) { std::printf("%s\n", line.c_str()); std::fflush(stdout); },
      [&](double fraction, const std::string& what) {
        const int percent = int(fraction * 100.0);
        if (percent / 10 != last_percent / 10) {
          std::printf("  %3d%%  %s\n", percent, what.c_str());
          std::fflush(stdout);
        }
        last_percent = percent;
      },
      &error);
  if (!ok) {
    std::printf("FAILED: %s\n", error.c_str());
    return 1;
  }
  return 0;
}

namespace {

std::string Ask(const std::string& question) {
  std::printf("%s", question.c_str());
  std::fflush(stdout);
  std::string line;
  if (!std::getline(std::cin, line)) return std::string();
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
  while (!line.empty() && line.front() == ' ') line.erase(line.begin());
  // Paths pasted with quotes.
  if (line.size() >= 2 && (line.front() == '"' || line.front() == '\'') && line.back() == line.front()) {
    line = line.substr(1, line.size() - 2);
  }
  return line;
}

}  // namespace

bool AskOnTerminal(Args* args) {
  std::printf(
      "\nReCompiled Steel setup\n"
      "======================\n"
      "This copies your own Real Steel files from your Xbox 360 into the game\n"
      "folder. On the console they are under Content\\0000000000000000\\584111E0\\\n"
      "(000D0000 = the game, 000B0000 = the title update, 00000002 = add-ons).\n"
      "Paths can be typed or pasted; leave a line empty to skip.\n\n");
  if (args->game.empty() && args->scan.empty()) {
    const std::string folder = Ask("Folder to search for all of them (empty to name the files one by one): ");
    if (!folder.empty()) {
      args->scan = folder;
      const Scan found = ScanFolder(args->scan);
      std::printf("  game: %s\n  title update: %s\n  add-ons: %zu\n",
                  found.game.empty() ? "not found" : found.game.filename().string().c_str(),
                  found.update.empty() ? "not found" : found.update.filename().string().c_str(),
                  found.addons.size());
      if (!found.game.empty()) return true;
      args->scan.clear();
    }
    const std::string game = Ask("The game package (file): ");
    if (game.empty()) return false;
    args->game = game;
  }
  if (args->update.empty()) {
    const std::string tu = Ask("The title update package (file, recommended; empty for none): ");
    if (!tu.empty()) args->update = tu;
  }
  if (args->addons.empty()) {
    const std::string dlc = Ask("Folder with the add-on packages (empty for none): ");
    if (!dlc.empty()) args->addons = dlc;
  }
  return true;
}

}  // namespace rs::setup
