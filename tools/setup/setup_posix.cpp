// Linux / Steam Deck front end: KDE's kdialog (the Deck's desktop mode) or
// zenity when present, the terminal otherwise.
#include "setup_common.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using namespace rs::setup;

enum class Tool { kNone, kKdialog, kZenity };

Tool FindTool() {
  if (std::getenv("DISPLAY") == nullptr && std::getenv("WAYLAND_DISPLAY") == nullptr) return Tool::kNone;
  if (std::system("command -v kdialog >/dev/null 2>&1") == 0) return Tool::kKdialog;
  if (std::system("command -v zenity >/dev/null 2>&1") == 0) return Tool::kZenity;
  return Tool::kNone;
}

std::string Quote(const std::string& s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else out.push_back(c);
  }
  return out + "'";
}

// Runs a dialog command; returns its stdout (trimmed) and sets |ok| from the
// exit status (cancel = not ok).
std::string Capture(const std::string& command, bool* ok) {
  std::string out;
  FILE* p = popen(command.c_str(), "r");
  if (p == nullptr) {
    *ok = false;
    return out;
  }
  char buf[4096];
  while (std::fgets(buf, sizeof(buf), p) != nullptr) out += buf;
  const int status = pclose(p);
  *ok = status == 0;
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
  return out;
}

void Message(Tool tool, const std::string& title, const std::string& text, bool error = false) {
  if (tool == Tool::kKdialog) {
    std::system(((error ? "kdialog --error " : "kdialog --msgbox ") + Quote(text) + " --title " + Quote(title)).c_str());
  } else if (tool == Tool::kZenity) {
    std::system(((error ? "zenity --error --text=" : "zenity --info --text=") + Quote(text) +
                 " --title=" + Quote(title) + " --width=420")
                    .c_str());
  } else {
    std::printf("%s\n", text.c_str());
  }
}

bool Question(Tool tool, const std::string& text) {
  if (tool == Tool::kKdialog) return std::system(("kdialog --yesno " + Quote(text)).c_str()) == 0;
  if (tool == Tool::kZenity) return std::system(("zenity --question --text=" + Quote(text) + " --width=420").c_str()) == 0;
  return false;
}

std::string PickFile(Tool tool, const std::string& title, bool folder, const std::string& start) {
  bool ok = false;
  std::string cmd;
  if (tool == Tool::kKdialog) {
    cmd = std::string(folder ? "kdialog --getexistingdirectory " : "kdialog --getopenfilename ") +
          Quote(start.empty() ? std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") : start) +
          " --title " + Quote(title);
  } else {
    cmd = std::string("zenity --file-selection ") + (folder ? "--directory " : "") + "--title=" + Quote(title);
    if (!start.empty()) cmd += " --filename=" + Quote(start + "/");
  }
  const std::string picked = Capture(cmd, &ok);
  return ok ? picked : std::string();
}

int RunWithDialogs(Tool tool, Args args) {
  const std::string title = "ReCompiled Steel setup";
  Message(tool, title,
          "This copies your own Real Steel files from your Xbox 360 into the game folder:\n"
          "the game, its title update (required) and any add-ons.\n\n"
          "Next: pick the folder you copied them to (everything in it is found automatically).");
  std::string home = std::getenv("HOME") ? std::getenv("HOME") : ".";
  Plan plan;
  std::string error;
  for (;;) {
    if (args.scan.empty() && args.game.empty()) {
      const std::string folder = PickFile(tool, "Folder holding your Real Steel files", true, home);
      if (folder.empty()) return 1;
      args.scan = folder;
    }
    plan = Plan();
    if (PlanFromArgs(args, &plan, &error)) break;
    if (!Question(tool, error + "\n\nPick another folder?")) return 1;
    args.scan.clear();
  }
  if (plan.update.empty() &&
      Question(tool, "No title update found there. It is required - pick the title update package?")) {
    const std::string tu = PickFile(tool, "The title update package", false, home);
    if (!tu.empty()) plan.update = tu;
  }
  if (plan.addons.empty() &&
      Question(tool, "No add-on packages found there. Do you have a folder of add-on packages?")) {
    const std::string dlc = PickFile(tool, "Folder with the add-on packages", true, home);
    if (!dlc.empty()) plan.addons = AddonsIn(dlc);
  }
  std::string summary = "Game: " + Describe(plan.game) + "\nTitle update: " +
                        (plan.update.empty() ? "none" : Describe(plan.update)) + "\nAdd-ons: " +
                        std::to_string(plan.addons.size()) + " package(s)\n\nInstall into " +
                        plan.dest.string() + " ?";
  if (!Question(tool, summary)) return 1;

  // Progress: zenity has a pipe-driven bar; with kdialog the copy is quick
  // enough (seconds) to just report the result.
  FILE* bar = nullptr;
  if (tool == Tool::kZenity) {
    bar = popen("zenity --progress --auto-close --no-cancel --title='ReCompiled Steel setup' --text='Copying...' --width=420",
                "w");
  }
  std::string log;
  const bool ok = Run(
      plan, [&](const std::string& line) { log += line + "\n"; std::printf("%s\n", line.c_str()); },
      [&](double fraction, const std::string& what) {
        if (bar != nullptr) {
          std::fprintf(bar, "%d\n# %s\n", int(fraction * 100.0), what.c_str());
          std::fflush(bar);
        }
      },
      &error);
  if (bar != nullptr) {
    std::fprintf(bar, "100\n");
    pclose(bar);
  }
  if (!ok) {
    Message(tool, title, "Setup failed:\n" + error, true);
    return 1;
  }
  Message(tool, title, "Done.\n\n" + log + "\nStart the game with start.sh (add it to Steam as a non-Steam game).");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args_text(argv, argv + argc);
  Args args = ParseArgs(args_text);
  if (args.help || !args.error.empty()) {
    if (!args.error.empty()) std::printf("%s\n\n", args.error.c_str());
    std::printf("%s", Usage().c_str());
    return args.help ? 0 : 2;
  }
  // only importing fighters from another game (Real Steel already installed)
  if (!args.fighters.empty() && args.game.empty() && args.scan.empty()) return RunImportConsole(args);
  const bool interactive = args.game.empty() && args.scan.empty();
  if (!args.cli && interactive) {
    const Tool tool = FindTool();
    if (tool != Tool::kNone) return RunWithDialogs(tool, args);
  }
  if (interactive || args.cli) {
    if (!isatty(STDIN_FILENO) && interactive) {
      std::printf("%s", Usage().c_str());
      return 2;
    }
    if (interactive && !AskOnTerminal(&args)) return 1;
  }
  Plan plan;
  std::string error;
  if (!PlanFromArgs(args, &plan, &error)) {
    std::printf("%s\n", error.c_str());
    return 1;
  }
  const int code = RunConsole(plan);
  if (code != 0 || args.fighters.empty()) return code;
  return RunImportConsole(args);
}
