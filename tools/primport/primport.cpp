// Fighter import (see primport.h).
#include "primport.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <set>

#include "../setup/stfs.h"
#include "primport_icons.h"
#include "primport_motion.h"

namespace rs::primport {
namespace fs = std::filesystem;

namespace {

constexpr uint32_t kPacificRim = 0x584112C1;
constexpr uint32_t kTypeGame = 0x000D0000, kTypeAddon = 0x00000002;
constexpr double kRefHeight = 88.1;     // the first fighter converted (the size reference) is this tall in its game
constexpr double kRsHeight = 25.0, kMaxHeight = 27.0;
constexpr int kPrivateBase = 40000;     // the robots' own motions: 40000 + 1000 x (robot - 52) + n
constexpr uint32_t kZeus = 0x65, kNoisyBoy = 0x66;
// Move-list donors (CHAR/PRO indices; every act they use is in the base
// motion bank m.pac RM00). 0xD4-0xD7 are the game's fighting-style
// templates B-E (the move sets assembled robots get from their parts).
constexpr uint32_t kAtom = 0x64, kTwinCities = 0x67, kMetro = 0x68, kMidas = 0x69, kKongTron = 0x88,
                   kIdaTen = 0x8A, kStyleB = 0xD4, kStyleC = 0xD5, kStyleD = 0xD6, kStyleE = 0xD7;

// 2026-10-17: the fighters' own converted moves did not suit Real
// Steel; each fighter now gets a move list mixed from Real Steel robots, so
// it is not a copy of any one of them. CHAR/PRO = 66 u16 acts: 0 stance (+
// the base robot's reactions/poses in 35-65), 3-14 jab set, 15-26 heavy set,
// 27-30 specials, 31-34 supers.
struct MoveMix {
  uint32_t base, jabs, heavies, specials, supers;
};

// One importable fighter: its record in the game's robot database (chEtc
// /CHAR/DAT, misc /BATS/INIT/002A), its character pack, the sub its
// motions are filed under in the shared act layout, and where it goes in
// Real Steel (robot index, character number, roster position) - plus the
// Real Steel robot whose moves fill in what it has none of.
struct Fighter {
  const char* key;
  uint32_t record;
  const char* pack;
  int sub;
  int robot, character, sort;
  uint32_t gap;
  const char* motions_from;   // key of the fighter whose motions it uses
  MoveMix mix;
  const char* stages;   // the league copy's places in DLC stages 4-5 ("<stage>:<rank>", rank 1 = boss)
  // Strength (2026-10-17): like the game's own robots (Zeus 0x65 in
  // multiplayer, 0x6F in the league) each fighter has a multiplayer version
  // (~2500 battle rating) and a stronger league copy (~7000) that only
  // fights in the stages above: multipliers of its own level 1 figures, and
  // the league copy's robot slot.
  double mp_boost, league_boost;
  int league_robot;
};
const Fighter kFighters[] = {
    {"a", 0x64, "ch100", 0, 52, 106, 0x11, kZeus, "a", {kAtom, kAtom, kStyleC, kNoisyBoy, kZeus}, "5:1", 1.19, 1.95, 116},
    {"b", 0x66, "ch102", 3, 53, 107, 0x12, kZeus, "b", {kZeus, kMetro, kStyleD, kKongTron, kMetro}, "4:2 5:3", 1.06, 1.65, 117},
    {"c", 0x67, "ch103", 2, 54, 108, 0x13, kNoisyBoy, "c", {kNoisyBoy, kTwinCities, kStyleB, kTwinCities, kIdaTen}, "4:3 5:4", 1.13, 1.75, 118},
    {"d", 0x32, "ch50", 1, 55, 109, 0x14, kZeus, "d", {kMidas, kStyleB, kStyleE, kMidas, kMidas}, "4:1 5:2", 1.08, 1.7, 119},
    {"e", 0x33, "ch51", -1, 56, 110, 0x15, kZeus, "a", {kMetro, kStyleD, kAtom, kAtom, kTwinCities}, "4:4", 1.20, 1.8, 126},   // its pack has no motions of its own
};

// Real Steel move-list slot (CHAR/PRO) -> the act of the fighter's motion for
// it and the hand the slot strikes with (mirrored if the motion uses the other)
struct Move {
  int slot;
  int act;     // -1 = the stance set
  char hand;   // 'L', 'R' or 0
};
const Move kMoves[] = {
    {0, -1, 0},
    {3, 5000, 'L'},  {4, 5000, 'R'},  {5, 5300, 'L'},  {6, 5300, 'R'},  {7, 5900, 'L'},  {8, 5900, 'R'},
    {9, 5310, 'L'},  {10, 5310, 'R'}, {11, 5010, 'L'}, {12, 5010, 'R'}, {13, 5950, 'L'}, {14, 5950, 'R'},
    {15, 5100, 'L'}, {16, 5100, 'R'}, {17, 5110, 'L'}, {18, 5110, 'R'}, {19, 5410, 'L'}, {20, 5410, 'R'},
    {21, 5700, 'L'}, {22, 5700, 'R'}, {23, 5400, 'L'}, {24, 5400, 'R'}, {25, 5600, 'L'}, {26, 5600, 'R'},
    {27, 5710, 0},   {30, 5750, 0},   {31, 6200, 0},   {32, 6003, 0},
};
const std::pair<int, int> kStanceSubs[] = {{0, 0}, {10, 0}};   // stance sub -> the fighter's act (its fighting stance)

const char* const kWanted[] = {"pac/ch/chEtc.pac", "pac/misc.pac", "pac/ch/ch100.pac", "pac/ch/ch102.pac",
                               "pac/ch/ch103.pac", "pac/ch/ch50.pac", "pac/ch/ch51.pac"};

std::string pack_key(const std::string& path) {
  const fs::path p(path);
  return p.stem().string();      // "chEtc", "misc", "ch100", ...
}

bool has_motions(const Bytes& pack) {
  try {
    for (const PachItem& it : pach_items(pack_entry(pack, "")))
      if (it.id == 0x5D) return true;
  } catch (const std::exception&) {
  }
  return false;
}

// The files the import needs, from the packages (or an extracted copy) of a
// supported game.
struct Sources {
  std::map<std::string, Bytes> files;   // pack key -> bytes
  bool has(const std::string& k) const { return files.count(k) > 0; }
};

Sources collect(const fs::path& from, const Log& log) {
  Sources s;
  if (!fs::exists(from)) return s;
  std::vector<fs::path> files;
  if (fs::is_regular_file(from)) {
    files.push_back(from);
  } else {
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(from, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
      if (ec) break;
      if (it->is_regular_file(ec)) files.push_back(it->path());
    }
  }
  std::sort(files.begin(), files.end());
  // packages: the game's first (its database and stats are the base ones)
  std::vector<std::pair<int, fs::path>> packages;
  for (const fs::path& f : files) {
    const rs::stfs::Identity id = rs::stfs::Identify(f);
    if (!id.stfs || id.title_id != kPacificRim) continue;
    packages.emplace_back(id.content_type == kTypeGame ? 0 : id.content_type == kTypeAddon ? 1 : 2, f);
  }
  std::stable_sort(packages.begin(), packages.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for (const auto& [kind, f] : packages) {
    if (kind == 2) continue;
    rs::stfs::Package pkg;
    std::string err;
    if (!pkg.Open(f, &err)) {
      if (log) log("skipped " + f.filename().string() + ": " + err);
      continue;
    }
    for (const char* want : kWanted) {
      const std::string k = pack_key(want);
      if (s.has(k)) continue;
      const rs::stfs::Entry* e = pkg.Find(want);
      if (!e) continue;
      Bytes data;
      if (pkg.Read(*e, data, &err)) s.files[k] = std::move(data);
    }
  }
  // an extracted copy: pac/ch packs that carry motions (item 0x5D) and the
  // database / stats next to them
  for (const fs::path& f : files) {
    const std::string name = f.filename().string();
    if (f.extension() != ".pac" || f.parent_path().filename() != "ch") continue;
    const std::string k = f.stem().string();
    bool wanted = false;
    for (const char* w : kWanted) wanted = wanted || pack_key(w) == k;
    if (!wanted || k == "chEtc" || s.has(k)) continue;
    Bytes d = read_file(f);
    if (!has_motions(d) && k != "ch50" && k != "ch51") continue;
    if (k == "ch50" || k == "ch51") {
      // the add-on packs have no motions; take them only next to a game that does
      if (!s.has("ch100")) continue;
    }
    s.files[k] = std::move(d);
    const fs::path root = f.parent_path().parent_path().parent_path();
    if (!s.has("chEtc") && fs::is_regular_file(root / "pac" / "ch" / "chEtc.pac")) s.files["chEtc"] = read_file(root / "pac" / "ch" / "chEtc.pac");
    if (!s.has("misc") && fs::is_regular_file(root / "pac" / "misc.pac")) s.files["misc"] = read_file(root / "pac" / "misc.pac");
  }
  return s;
}

std::string hex4(uint32_t v) {
  char b[16];
  std::snprintf(b, sizeof(b), "%04X", v);
  return b;
}

// name and model scale from the game's robot database
bool fighter_record(const Archive& chetc, uint32_t record, std::string* name, double* scale) {
  Bytes d;
  if (!chetc.find("/CHAR/DAT/" + hex4(record), &d) || d.size() < 70) return false;
  *name = std::string(reinterpret_cast<const char*>(&d[38]), strnlen(reinterpret_cast<const char*>(&d[38]), 32));
  const uint16_t s = le16(&d[28]);
  *scale = s ? s / 4096.0 : 1.0;
  return !name->empty();
}

std::string folder_name(const std::string& name) {
  std::string out;
  for (char c : name) out.push_back(std::isalnum(uint8_t(c)) ? char(std::tolower(uint8_t(c))) : '_');
  return out;
}

double fighter_height(const Character& J, double scale) {
  double lo = 1e30, hi = -1e30;
  for (const Vec3& p : body_points(J.meshes(true))) lo = std::min(lo, p[1]), hi = std::max(hi, p[1]);
  return std::min(kMaxHeight, kRsHeight * (hi - lo) * scale / kRefHeight);
}

struct Prepared {
  std::unique_ptr<Character> J;
  std::unique_ptr<Posed> posed;
  std::unique_ptr<Retarget> rt;
};

// the fighter's own moves for the gap robot's move list (2026-10-16; not
// used since the mixed move lists, kept for reference)
[[maybe_unused]] void build_moves(const Fighter& f, const Fighter& src, const Prepared& p, const Character& B, const MotionBank& rs_bank,
                 const Bytes& gap_pro, const fs::path& dir) {
  if (!p.J->items.count(0x5D)) throw Error("no motions in the fighter's pack");
  const MotionBank bank(p.J->items.at(0x5D));
  if (gap_pro.size() < 132) throw Error("Real Steel move list too short");
  std::vector<int> gap_acts(66);
  for (int i = 0; i < 66; ++i) gap_acts[i] = le16(&gap_pro[i * 2]);
  const MotionBank::Motion* stance_rs = rs_bank.find(gap_acts[0], 0, 0);
  const MotionBank::Motion* stance_j = bank.find(0, src.sub, 0);
  if (!stance_rs || !stance_j) throw Error("no stance motion");
  const auto rs0 = rs_bank.sample(*stance_rs, 0), j0 = bank.sample(*stance_j, 0);
  const double hip = rs0.at("root").t[1] / j0.at(p.J->names[0]).t[1];
  BankWriter writer(rs_bank, B);
  std::map<int, int> own;   // gap robot act -> private act
  std::set<int> seen;       // the first slot of an act decides its motion
  for (const Move& mv : kMoves) {
    const int act = gap_acts[mv.slot];
    if (!act || !seen.insert(act).second) continue;
    const int priv = kPrivateBase + 1000 * (f.robot - 52) + int(own.size());
    if (mv.act < 0) {
      for (const auto& [sub, jact] : kStanceSubs) {
        const MotionBank::Motion* rm = rs_bank.find(act, sub, 0);
        const MotionBank::Motion* pm = bank.find(jact, src.sub, 0);
        if (!rm || !pm) continue;
        const std::vector<double> t = resample_times(pm->frames, rm->frames);
        writer.add(priv, sub, retarget_motion(*p.rt, bank, *pm, &t, hip));
      }
      own[act] = priv;
      continue;
    }
    const MotionBank::Motion* pm = bank.find(mv.act, src.sub, 0);
    if (!pm) continue;
    const auto [side, jpeak] = hit_frame(B, retarget_motion(*p.rt, bank, *pm, nullptr, hip));
    const bool mirror = mv.hand && char(std::toupper(side)) != mv.hand;
    bool any = false;
    for (int sub : {0, 100}) {
      const MotionBank::Motion* rm = rs_bank.find(act, sub, 0);
      if (!rm) continue;
      const int rpeak = hit_frame(B, bank_frames(B, rs_bank, *rm)).second;
      const std::vector<double> t = warp_times(rm->frames, rpeak, pm->frames, jpeak);
      Frames fr = retarget_motion(*p.rt, bank, *pm, &t, hip);
      if (mirror) fr = mirror_frames(B, fr);
      writer.add(priv, sub, std::move(fr));
      any = true;
    }
    if (any) own[act] = priv;
  }
  write_file(dir / "motions.bin", writer.build());
  std::string map = "# act of the move -> act in motions.bin (this robot only)\n";
  for (const auto& [a, pr] : own) map += std::to_string(a) + " " + std::to_string(pr) + "\n";
  write_file(dir / "motion_map.txt", Bytes(map.begin(), map.end()));
  write_file(dir / "pro.bin", gap_pro);
}

}  // namespace

Found Scan(const fs::path& from) {
  Found out;
  try {
    const Sources s = collect(from, nullptr);
    if (!s.has("chEtc") || !s.has("ch100")) return out;
    out.game = "Pacific Rim";
    const Archive chetc(s.files.at("chEtc"));
    for (const Fighter& f : kFighters) {
      std::string name;
      double scale;
      if (s.has(f.pack) && fighter_record(chetc, f.record, &name, &scale)) out.fighters.push_back(name);
    }
  } catch (const std::exception&) {
    out = Found{};
  }
  return out;
}

bool Import(const fs::path& from, const fs::path& rs_game_dir, const fs::path& out_dir, const Log& log, const Progress& progress,
            std::string* error) {
  try {
    auto say = [&](const std::string& s) {
      if (log) log(s);
    };
    if (progress) progress(0.0, "Looking for the game files");
    const Sources s = collect(from, log);
    if (!s.has("chEtc") || !s.has("ch100")) throw Error("no supported game found in " + from.string());
    const Archive chetc(s.files.at("chEtc"));
    std::unique_ptr<Archive> misc = s.has("misc") ? std::make_unique<Archive>(s.files.at("misc")) : nullptr;

    // Real Steel's own pieces: the movie robot the bodies are built on (Atom),
    // its motions, the move lists and the select icons
    const Bytes rs_ch100 = read_file(rs_game_dir / "pac" / "ch" / "ch100.pac");
    const Character B(rs_ch100, "");
    const Bytes base_entry = pack_entry(rs_ch100, "00010002");
    Bytes bank_data;
    if (!Archive(rs_game_dir / "pac" / "m.pac").find("/MOT/RM00/0000", &bank_data)) throw Error("Real Steel's motions not found");
    const MotionBank rs_bank(std::move(bank_data));
    const Archive rs_chetc(rs_game_dir / "pac" / "ch" / "chEtc.pac");
    const Archive stock_icons(rs_game_dir / "pac" / "DLC_HD.pac");

    std::map<std::string, Prepared> prepared;
    auto prepare = [&](const Fighter& f) -> const Prepared& {
      auto it = prepared.find(f.key);
      if (it != prepared.end()) return it->second;
      std::string name;
      double scale = 1.0;
      fighter_record(chetc, f.record, &name, &scale);
      Prepared p;
      p.J = std::make_unique<Character>(s.files.at(f.pack), "");
      const double height = fighter_height(*p.J, scale);
      p.posed = std::make_unique<Posed>(*p.J, fist_turns(p.J->names));
      p.rt = std::make_unique<Retarget>(*p.posed, B, height);
      return prepared.emplace(f.key, std::move(p)).first->second;
    };

    std::vector<const Fighter*> todo;
    for (const Fighter& f : kFighters)
      if (s.has(f.pack)) todo.push_back(&f);
    int done = 0;
    for (const Fighter* fp : todo) {
      const Fighter& f = *fp;
      std::string name;
      double scale = 1.0;
      if (!fighter_record(chetc, f.record, &name, &scale)) continue;
      const fs::path dir = out_dir / folder_name(name);
      if (progress) progress(double(done) / todo.size(), "Importing " + name);
      say("importing " + name);
      const Prepared& p = prepare(f);
      // body
      std::vector<std::vector<uint32_t>> colours;
      for (const MeshData& m : p.rt->meshes) colours.push_back(m.colour);
      const Bytes model = build_model(*p.rt, colours);
      const auto [item_a, item_c] = build_textures(*p.J, B);
      fs::create_directories(dir);
      write_file(dir / "character.bin", character_entry(base_entry, model, item_a, item_c));
      // icons
      for (const auto& [size, dds] : make_icons(stock_icons, *p.posed)) write_file(dir / ("icon_" + size + ".dds"), dds);
      // moves: a move list mixed from Real Steel robots (see MoveMix)
      {
        auto pro_of = [&](uint32_t index) {
          Bytes b;
          if (!rs_chetc.find("/CHAR/PRO/" + hex4(index), &b) || b.size() < 132)
            throw Error("Real Steel move list not found");
          return b;
        };
        Bytes pro = pro_of(f.mix.base);
        auto take = [&](uint32_t donor, int first, int last) {
          const Bytes d = pro_of(donor);
          std::copy(d.begin() + first * 2, d.begin() + (last + 1) * 2, pro.begin() + first * 2);
        };
        take(f.mix.jabs, 3, 14);
        take(f.mix.heavies, 15, 26);
        take(f.mix.specials, 27, 30);
        take(f.mix.supers, 31, 34);
        write_file(dir / "pro.bin", pro);
        std::error_code ec;   // converted motions from earlier imports
        for (const char* old : {"motions.bin", "motion_map.txt"}) fs::remove(dir / old, ec);
      }
      int hp = 2800, atk = 90, arm = 75, aspd = 5, mspd = 6;
      Bytes st;
      if (misc && misc->find("/BATS/INIT/002A/" + hex4(f.record), &st) && st.size() >= 16) {
        hp = le16(&st[4]), atk = le16(&st[6]), arm = le16(&st[10]), aspd = le16(&st[12]), mspd = le16(&st[14]);
      }
      // robot.txt: figures from the game's own level 1 stats (HP, attack x 28,
      // armour x 28, attack speed, movement speed), times the version's multiplier
      auto feel = [&](double k, int speed_up, bool league) {
        char b[512];
        std::snprintf(b, sizeof(b),
                      "[feel]\ncore_hp = %d\nattack = %d\ndefense = %d\npunch_speed = %d\nmovement_speed = %d\n",
                      int(hp * k), int(atk * 28 * k), int(arm * 28 * k), std::min(9, aspd + speed_up),
                      std::min(9, mspd + speed_up));
        std::string out = b;
        if (league) {
          std::snprintf(b, sizeof(b), "power_charge = %d\nmobility = 8\nresilience = 8\ndamage_recovery = 7\npower_recovery = 8\n",
                        int(1600 * (1.0 + (k - 1.0) / 2)));
          out += b;
        }
        return out;
      };
      char text[1024];
      std::snprintf(text, sizeof(text),
                    "# Made by Real Steel's fighter import from the player's own copy of the game.\n"
                    "name = %s\nkind = full\nrobot = %d\ncharacter = %d\ntitle = IMPORTED FIGHTER\nsort = 0x%X\n\n"
                    "# Multiplayer strength (the league fights a stronger copy, folder *_league).\n",
                    name.c_str(), f.robot, f.character, f.sort);
      const std::string t = std::string(text) + feel(f.mp_boost, 0, false);
      write_file(dir / "robot.txt", Bytes(t.begin(), t.end()));
      say("  " + dir.string());
      // the league copy: same body, moves and icons, stronger, not in character
      // select, its own registry key; fights in DOWNLOADED STAGES 4 and 5
      // (made when all their fighters are imported; rank 1 = the boss)
      const fs::path ldir = out_dir / (folder_name(name) + "_league");
      fs::create_directories(ldir);
      for (const char* file : {"character.bin", "pro.bin", "icon_0000.dds", "icon_0001.dds", "icon_0002.dds"})
        fs::copy_file(dir / file, ldir / file, fs::copy_options::overwrite_existing);
      std::snprintf(text, sizeof(text),
                    "# Made by Real Steel's fighter import: the league copy of %s.\n"
                    "name = %s\nregistry = LEAGUE %s\nkind = full\nrobot = %d\ncharacter = %d\n"
                    "title = IMPORTED FIGHTER\nselect = 0\n\n"
                    "stages = %s\nstage_4_name = JAEGER 1ST STAGE\nstage_5_name = JAEGER 2ND STAGE\n\n",
                    name.c_str(), name.c_str(), name.c_str(), f.league_robot, f.character, f.stages);
      const std::string lt = std::string(text) + feel(f.league_boost, 1, true);
      write_file(ldir / "robot.txt", Bytes(lt.begin(), lt.end()));
      say("  " + ldir.string());
      ++done;
    }
    if (!done) throw Error("no fighters could be imported");
    if (progress) progress(1.0, "Done");
    return true;
  } catch (const std::exception& e) {
    if (error) *error = e.what();
    return false;
  }
}

}  // namespace rs::primport
