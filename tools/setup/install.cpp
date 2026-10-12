#include "install.h"

#include "stfs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <memory>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace rs::setup {
namespace {

namespace fs = std::filesystem;

std::string Hex8(uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%08X", v);
  return buf;
}

// ASCII / BMP UTF-8 -> UTF-16BE, NUL-padded to |bytes|.
std::vector<uint8_t> Utf16Be(const std::string& text, size_t bytes) {
  std::vector<uint8_t> out;
  for (size_t i = 0; i < text.size() && out.size() + 2 <= bytes - 2;) {
    const uint8_t c = uint8_t(text[i]);
    uint32_t code = c;
    size_t len = 1;
    if (c >= 0xF0) {
      code = c & 0x07;
      len = 4;
    } else if (c >= 0xE0) {
      code = c & 0x0F;
      len = 3;
    } else if (c >= 0xC0) {
      code = c & 0x1F;
      len = 2;
    }
    for (size_t k = 1; k < len && i + k < text.size(); ++k) code = (code << 6) | (uint8_t(text[i + k]) & 0x3F);
    i += len;
    if (code > 0xFFFF) code = '?';
    out.push_back(uint8_t(code >> 8));
    out.push_back(uint8_t(code));
  }
  out.resize(bytes, 0);
  return out;
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

bool IsTitle(const stfs::Identity& id) { return id.stfs && id.title_id == kTitleId; }

bool Fail(std::string* error, const std::string& text) {
  if (error) *error = text;
  return false;
}

// The content manager's header file: an XCONTENT_AGGREGATE_DATA (0x148
// bytes, big-endian: device id, content type, display name UTF-16, 42-char
// file name, xuid, title id) followed by the licence mask. Not the STFS
// header itself (that made the game see a device called 'LIVE').
bool WriteHeader(const fs::path& file, const stfs::Identity& id, const std::string& name,
                 std::string* error) {
  std::vector<uint8_t> data;
  Put32(data, 1);
  Put32(data, id.content_type);
  const std::vector<uint8_t> display = Utf16Be(id.display_name, 256);
  data.insert(data.end(), display.begin(), display.end());
  std::string file_name = name.substr(0, 42);
  file_name.resize(44, '\0');
  data.insert(data.end(), file_name.begin(), file_name.end());
  data.resize(0x138, 0);
  Put32(data, 0);
  Put32(data, 0);  // xuid (none: not tied to a profile)
  Put32(data, id.title_id);
  Put32(data, 0);
  if (data.size() != 0x148) return Fail(error, "internal: header size");
  Put32(data, 0xFFFFFFFFu);  // licence mask
  std::ofstream f(file, std::ios::binary | std::ios::trunc);
  if (!f || !f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()))) {
    return Fail(error, "cannot write " + file.string());
  }
  return true;
}

}  // namespace

Scan ScanFolder(const fs::path& dir) {
  Scan scan;
  std::error_code ec;
  if (fs::is_regular_file(dir, ec)) {
    const stfs::Identity id = stfs::Identify(dir);
    scan.files_looked_at = 1;
    if (IsTitle(id)) {
      if (id.content_type == kTypeGame) scan.game = dir;
      else if (id.content_type == kTypeUpdate) scan.update = dir;
      else if (id.content_type == kTypeAddon) scan.addons.push_back(dir);
    } else if (id.stfs) {
      ++scan.other_titles;
    }
    return scan;
  }
  for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
       it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    const uint64_t size = it->file_size(ec);
    if (ec || size < 0xB000) continue;  // smaller than a package header
    ++scan.files_looked_at;
    const stfs::Identity id = stfs::Identify(it->path());
    if (!id.stfs) continue;
    if (!IsTitle(id)) {
      ++scan.other_titles;
      continue;
    }
    if (id.content_type == kTypeGame) {
      if (scan.game.empty() || size > fs::file_size(scan.game, ec)) scan.game = it->path();
    } else if (id.content_type == kTypeUpdate) {
      // The newest title update is the largest, in Real Steel's case the
      // only one that matters (the recompiled code is the 2015 update's).
      if (scan.update.empty() || size > fs::file_size(scan.update, ec)) scan.update = it->path();
    } else if (id.content_type == kTypeAddon) {
      scan.addons.push_back(it->path());
    }
  }
  std::sort(scan.addons.begin(), scan.addons.end());
  return scan;
}

std::vector<fs::path> AddonsIn(const fs::path& dir) {
  std::vector<fs::path> out;
  std::error_code ec;
  if (fs::is_regular_file(dir, ec)) {
    out.push_back(dir);
    return out;
  }
  for (fs::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
    if (ec) break;
    if (!it->is_regular_file(ec)) continue;
    const stfs::Identity id = stfs::Identify(it->path());
    if (IsTitle(id) && id.content_type == kTypeAddon) out.push_back(it->path());
  }
  std::sort(out.begin(), out.end());
  return out;
}

fs::path DefaultUserRoot() {
#ifdef _WIN32
  PWSTR documents = nullptr;
  fs::path root;
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, KF_FLAG_DEFAULT, nullptr, &documents))) {
    root = fs::path(documents);
  }
  if (documents != nullptr) CoTaskMemFree(documents);
  if (root.empty()) {
    if (const char* profile = std::getenv("USERPROFILE")) root = fs::path(profile) / "Documents";
  }
  return root / "realsteel";
#else
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != 0) {
    return fs::path(xdg) / "realsteel";
  }
  const char* home = std::getenv("HOME");
  if (home == nullptr || *home == 0) {
    if (const passwd* pw = getpwuid(getuid())) home = pw->pw_dir;
  }
  return fs::path(home != nullptr ? home : ".") / ".local" / "share" / "realsteel";
#endif
}

std::string Describe(const fs::path& package) {
  const stfs::Identity id = stfs::Identify(package);
  if (!id.stfs) return package.filename().string() + " (not an Xbox 360 package)";
  std::string kind;
  if (id.content_type == kTypeGame) kind = "game";
  else if (id.content_type == kTypeUpdate) kind = "title update";
  else if (id.content_type == kTypeAddon) kind = "add-on";
  else kind = "content type " + Hex8(id.content_type);
  std::string text = id.display_name + " (" + kind;
  if (id.title_id != kTitleId) text += ", title " + Hex8(id.title_id) + " - not Real Steel";
  return text + ")";
}

bool Run(const Plan& plan, const Log& log, const Progress& progress, std::string* error) {
  std::error_code ec;
  if (plan.game.empty()) return Fail(error, "No game package chosen.");
  if (plan.dest.empty()) return Fail(error, "No destination folder.");
  const fs::path user_root = plan.user_root.empty() ? DefaultUserRoot() : plan.user_root;

  // Open everything first so a bad file stops the run before anything is written.
  stfs::Package game;
  if (!game.Open(plan.game, error)) return false;
  if (game.identity().title_id != kTitleId) {
    return Fail(error, plan.game.filename().string() + " is " + game.identity().display_name +
                           " (title " + Hex8(game.identity().title_id) + "), not Real Steel.");
  }
  if (game.Find("default.xex") == nullptr) {
    return Fail(error, plan.game.filename().string() + " is not the game itself (no default.xex inside): " +
                           Describe(plan.game));
  }
  stfs::Package update;
  if (!plan.update.empty()) {
    if (!update.Open(plan.update, error)) return false;
    if (update.identity().title_id != kTitleId || update.Find("default.xexp") == nullptr) {
      return Fail(error, plan.update.filename().string() + " is not a Real Steel title update: " +
                             Describe(plan.update));
    }
  }
  struct Addon {
    fs::path file;
    stfs::Identity id;
    std::unique_ptr<stfs::Package> package;
  };
  std::vector<Addon> addons;
  for (const fs::path& file : plan.addons) {
    Addon addon{file, stfs::Identify(file), std::make_unique<stfs::Package>()};
    std::string why;
    if (!addon.package->Open(file, &why) || addon.id.title_id != kTitleId ||
        addon.id.content_type != kTypeAddon) {
      log("Skipping " + file.filename().string() + ": " + (why.empty() ? Describe(file) : why));
      continue;
    }
    addons.push_back(std::move(addon));
  }

  uint64_t total = game.total_bytes() + (plan.update.empty() ? 0 : update.total_bytes());
  for (const Addon& addon : addons) total += addon.package->total_bytes();
  uint64_t done_before = 0;
  auto report = [&](const std::string& what) {
    return [&, what](uint64_t done, uint64_t) {
      if (progress && total != 0) progress(double(done_before + done) / double(total), what);
    };
  };

  // The game.
  const fs::path game_dir = plan.dest / "game";
  log("Game: " + game.identity().display_name + " -> " + game_dir.string());
  if (!game.Extract(game_dir, error, report("Copying the game"))) return false;
  done_before += game.total_bytes();

  // The title update: its data files beside the game, its code patch next to
  // the game's default.xex (the runtime applies it at load).
  if (!plan.update.empty()) {
    const fs::path tu_dir = plan.dest / "tu";
    log("Title update: " + update.identity().display_name + " -> " + tu_dir.string());
    if (!update.Extract(tu_dir, error, report("Copying the title update"))) return false;
    done_before += update.total_bytes();
    fs::copy_file(tu_dir / "default.xexp", game_dir / "default.xexp",
                  fs::copy_options::overwrite_existing, ec);
    if (ec) return Fail(error, "cannot copy default.xexp into " + game_dir.string());
  } else {
    log("No title update chosen: the game runs without the 2015 update's fixes.");
  }

  // Add-ons go where the game keeps saves, laid out the way its content
  // manager expects marketplace content (not tied to a profile).
  if (!addons.empty()) {
    const fs::path base = user_root / "0000000000000000" / Hex8(kTitleId);
    const fs::path header_dir = base / "Headers" / Hex8(kTypeAddon);
    fs::create_directories(header_dir, ec);
    if (ec) return Fail(error, "cannot create " + header_dir.string());
    for (Addon& addon : addons) {
      const std::string file_name = addon.file.filename().string();
      const fs::path target = base / Hex8(kTypeAddon) / file_name;
      log("Add-on: " + addon.id.display_name + " -> " + target.string());
      if (!addon.package->Extract(target, error, report("Copying " + addon.id.display_name))) return false;
      done_before += addon.package->total_bytes();
      if (!WriteHeader(header_dir / (file_name + ".header"), addon.id, file_name, error)) return false;
    }
    log(std::to_string(addons.size()) + " add-on package(s) installed in " + user_root.string());
  }
  if (progress) progress(1.0, "Done");
  log("Done. Start the game from " + plan.dest.string());
  return true;
}

}  // namespace rs::setup
