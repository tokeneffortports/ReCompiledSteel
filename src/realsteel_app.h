// realsteel - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include "realsteel_gpu.h"
#include "realsteel_host_window.h"
#include "realsteel_mods_pack.h"
#include "realsteel_native_present.h"
#include "realsteel_native_render.h"
#include "realsteel_overlay.h"
#include "realsteel_version.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>

#include <algorithm>
#include <ctime>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifndef _WIN32
#include <SDL3/SDL.h>  // message boxes (Linux, Android)
#endif
#ifdef __ANDROID__
#include <unistd.h>
#endif

class RealsteelApp : public rex::ReXApp {
 public:
  // Every launch copies the profiles (saves, achievements, the mod registry)
  // to <user data>/backups/<date>, keeping the newest five. The add-on
  // packages in 0000000000000000/ are not copied (large, and reinstallable).
  static void BackupSaves(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) return;
    std::vector<fs::path> profiles;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
      const std::string name = entry.path().filename().string();
      const bool hex16 = name.size() == 16 && name.find_first_not_of("0123456789ABCDEFabcdef") == std::string::npos;
      if (entry.is_directory(ec) && hex16 && name != "0000000000000000") profiles.push_back(entry.path());
    }
    if (profiles.empty()) return;
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", std::localtime(&now));
    const fs::path backups = root / "backups";
    const fs::path target = backups / stamp;
    fs::create_directories(target, ec);
    if (ec) return;
    for (const fs::path& profile : profiles) {
      fs::copy(profile, target / profile.filename(), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    }
    if (fs::is_regular_file(root / "mods_registry.txt", ec)) {
      fs::copy_file(root / "mods_registry.txt", target / "mods_registry.txt", fs::copy_options::overwrite_existing, ec);
    }
    std::vector<fs::path> old;
    for (const auto& entry : fs::directory_iterator(backups, ec)) {
      if (entry.is_directory(ec)) old.push_back(entry.path());
    }
    std::sort(old.begin(), old.end());
    while (old.size() > 5) {
      fs::remove_all(old.front(), ec);
      old.erase(old.begin());
    }
    REXLOG_INFO("Saves backed up to {} ({} kept)", target.string(), old.size());
  }

  using rex::ReXApp::ReXApp;

  // Where the game keeps everything: game/, tu/, settings, logs, mods/.
  // Desktop: the executable's folder. Android: a public folder the player
  // can fill over USB or with a file manager (the app has "All files
  // access"); REALSTEEL_ROOT overrides it.
  static std::filesystem::path AppRoot() {
#ifdef __ANDROID__
    if (const char* root = std::getenv("REALSTEEL_ROOT"); root != nullptr && *root != 0) {
      return root;
    }
    return "/sdcard/RealSteel";
#else
    return rex::filesystem::GetExecutableFolder();
#endif
  }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    // Everything the game keeps beside itself (settings, logs, the mods
    // folder) is addressed relative to the executable, whatever folder a
    // shortcut or Steam starts it in.
    std::error_code ec;
#ifdef __ANDROID__
    // Until "All files access" is granted the public folder cannot be used;
    // the app's private storage keeps logs and settings writable meanwhile
    // (the log file would otherwise fail to open).
    std::filesystem::create_directories(AppRoot(), ec);
    std::filesystem::current_path(AppRoot(), ec);
    if (ec || access(".", W_OK) != 0) {
      if (const char* internal = SDL_GetAndroidInternalStoragePath()) {
        std::error_code ec2;
        std::filesystem::current_path(internal, ec2);
      }
    }
    // Saves, add-ons and the cache under <data folder>/user unless the
    // command line says otherwise (the SDK's default is the home folder).
    if (rex::cvar::GetFlagSource("user_data_root") < rex::cvar::Source::kEnvironment) {
      std::error_code ec3;
      const auto user = std::filesystem::current_path(ec3) / "user";
      rex::cvar::SetFlagByName("user_data_root", user.string());
    }
#else
    std::filesystem::current_path(AppRoot(), ec);
#endif
    return std::unique_ptr<RealsteelApp>(new RealsteelApp(ctx, "realsteel",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // Saved settings-page values (realsteel_settings.cfg) apply before the
  // window and the runtime are created.
  void OnPostInitLogging() override {
    REXLOG_INFO("ReCompiled Steel {}", realsteel::kVersion);
    realsteel::host_window::RemoveStaleGuestMemoryFiles();
    realsteel::overlay::LoadSavedSettings();
    // A plain launch (no arguments) plays the player's own copy with its
    // add-ons unlocked; the command line still wins.
    if (rex::cvar::GetFlagSource("license_mask") < rex::cvar::Source::kEnvironment) {
      rex::cvar::SetFlagByName("license_mask", "1");
    }
  }
  // 2026-10-02: --realsteel_native_gpu=true replaces the GPU emulation
  // plugin with the built-in command-stream service (realsteel_gpu.h).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (realsteel::gpu::Enabled()) {
      config.graphics = realsteel::gpu::Create();
      config.gpu_plugin.clear();
    }
  }
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  void OnPostSetup() override { realsteel::native_render::Install(); }
  // With the built-in GPU service nothing draws into the SDK window, so the
  // native renderer presents there (one window, normal focus/fullscreen).
  void OnPreLaunchModule() override {
    if (realsteel::gpu::Enabled() && window() != nullptr) {
      realsteel::host_window::Attach(window());
      // Settings page + overlays, drawn by the native renderer.
      realsteel::overlay::Init(window(), &app_context(), runtime());
    }
  }
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    realsteel::native_present::SetHostWindowSize(pixel_width, pixel_height);
  }
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  void OnShutdown() override { realsteel::overlay::Shutdown(); }
  // Robots dropped into mods/ are written into the user's content folder and
  // a patched title update before the runtime mounts anything.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    (void)resume;
    rex::PathConfig paths = defaults;
    // Installed layout (what the setup program writes): game/ and tu/
    // beside the executable, unless --game_data_root / --update_data_root
    // point elsewhere.
    const std::filesystem::path exe = AppRoot();
    std::error_code ec;
#ifdef __ANDROID__
    if (paths.user_data_root.empty()) paths.user_data_root = exe / "user";
    std::filesystem::create_directories(paths.user_data_root, ec);
#endif
    if (paths.game_data_root.empty() &&
        std::filesystem::is_regular_file(exe / "game" / "default.xex", ec)) {
      paths.game_data_root = exe / "game";
    }
    if (paths.update_data_root.empty() &&
        std::filesystem::is_directory(exe / "tu" / "pac", ec)) {
      paths.update_data_root = exe / "tu";
    }
    // Nothing to play, or the game without its title update: the recompiled
    // code is the updated game's, so the original default.xex alone starts at
    // an address it does not have (2026-10-13, an Android report). Say what
    // is missing and stop, rather than crash further in.
    const bool no_game = paths.game_data_root.empty();
    const bool no_update = !no_game &&
        (!std::filesystem::is_regular_file(paths.game_data_root / "default.xexp", ec) ||
         paths.update_data_root.empty());
    if (no_game || no_update) {
      const char* what = no_game
          ? "The game files are not installed yet."
          : "The Real Steel title update is missing. ReCompiled Steel needs it: the game "
            "is built for the updated version.";
#ifdef _WIN32
      const char* how = "Run RealSteelSetup.exe in this folder and point it at your Xbox 360 "
                        "files: it installs the game, its title update and your add-ons.";
#elif defined(__ANDROID__)
      const char* how = "Press \"Install game files\" in the launcher and pick the folder with "
                        "your Xbox 360 files: it installs the game, its title update and your add-ons.";
#else
      const char* how = "Run realsteel-setup in this folder and point it at your Xbox 360 "
                        "files: it installs the game, its title update and your add-ons.";
#endif
      REXLOG_ERROR("{} {}", what, how);
      const std::string text = std::string(what) + "\n\n" + how;
#ifdef _WIN32
      MessageBoxA(nullptr, text.c_str(), "ReCompiled Steel", MB_OK | MB_ICONINFORMATION);
#else
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "ReCompiled Steel", text.c_str(), nullptr);
#endif
      rex::FlushLogging();
      std::exit(1);
    }
    BackupSaves(paths.user_data_root);
    realsteel::mods::Install(paths);
    return paths;
  }
};
