// realsteel - full robots (movie-robot style: one skinned body in a pac/ch
// character pack), 2026-10-16. Research stage.
//
// What is known so far:
// - robot index 0..6 = the player's garage robots (records in the save);
//   7..224 map through a u16 table at 0x824302E0 to slots of a table of
//   420-byte robot definitions (pointer at 0x824B0628), filled one record at
//   a time by sub_82116B90(slot, source) from stored records (sub_82407508);
// - byte 412 of a definition is the character number (100 = Atom ...);
//   sub_820CAFE0 builds the pack entry name "/EMD/%06d%02d" from it.
//
// --realsteel_dump_robots=true appends every definition the game loads to
// rs_robotdefs.bin (u32 slot, u32 caller, 420 bytes) and logs the entry
// names it asks for.

#include "generated/default_final/realsteel_init.h"
#include "realsteel_mods_pack.h"
#include "realsteel_robots.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

namespace realsteel::robots {
extern thread_local uint32_t t_lab_body;   // robot lab: robot being worked out on this thread
}

REXCVAR_DEFINE_BOOL(realsteel_dump_robots, false, "Real Steel",
                    "Development: log the robot definitions the game loads (rs_robotdefs.bin)");

namespace {

uint32_t LoadBE32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

std::mutex g_dump_mutex;

}  // namespace

// Copies a 420-byte robot definition into its slot: r3 = slot, r4 = source.
extern "C" REX_FUNC(sub_82116B90) {
  const uint32_t slot = ctx.r3.u32, caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82116B90(ctx, base);
  if (!REXCVAR_GET(realsteel_dump_robots)) return;
  const uint32_t table = LoadBE32(base, 0x824B0628u);
  if (table == 0) return;
  std::lock_guard<std::mutex> lock(g_dump_mutex);
  if (FILE* f = std::fopen("rs_robotdefs.bin", "ab")) {
    const uint32_t head[2] = {slot, caller};
    std::fwrite(head, sizeof(head), 1, f);
    std::fwrite(base + table + slot * 420u, 420, 1, f);
    std::fclose(f);
  }
  const uint8_t* rec = base + table + slot * 420u;
  char name[33] = {};
  for (int i = 0; i < 32 && rec[30 + i]; ++i) name[i] = static_cast<char>(rec[30 + i]);
  REXLOG_INFO("[robots] slot {} <- record index {} '{}' ch {} id {:#x}", slot, rec[28], name, rec[412], rec[398]);
  if (slot == 0) {
    std::string map;
    for (uint32_t i = 0; i < 225; ++i) {
      const uint32_t v = (uint32_t(base[0x824302E0u + 2 * i]) << 8) | base[0x824302E0u + 2 * i + 1];
      if (v != 512) map += std::to_string(i) + ":" + std::to_string(v) + " ";
    }
    REXLOG_INFO("[robots] index->slot map: {}", map);
  }
}

// Pack entry name of a robot: r3 = archive, r4 = output, r5 = robot index *
// 100 + variant.
extern "C" REX_FUNC(sub_820CAFE0) {
  const uint32_t out = ctx.r4.u32, id = ctx.r5.u32;
  __imp__sub_820CAFE0(ctx, base);
  if (!REXCVAR_GET(realsteel_dump_robots)) return;
  char name[32] = {};
  for (int i = 0; i < 31 && base[out + i]; ++i) name[i] = static_cast<char>(base[out + i]);
  REXLOG_INFO("[robots] entry for robot {} variant {}: '{}'", id / 100, id % 100, name);
}

// Definition by robot index (r3) -> pointer: logs each distinct (caller,
// index) pair once, to find the code that lists robots.
extern "C" REX_FUNC(sub_82117868) {
  const uint32_t index = ctx.r3.u32, caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_82117868(ctx, base);
  if (!REXCVAR_GET(realsteel_dump_robots)) return;
  static std::mutex m;
  static std::set<uint64_t> seen;
  std::lock_guard<std::mutex> lock(m);
  if (seen.insert((uint64_t(caller) << 32) | index).second) {
    const uint32_t rec = ctx.r3.u32;
    std::string tail;
    if (rec) {
      char b[48];
      std::snprintf(b, sizeof(b), " [412..419] %02x %02x %02x %02x %02x %02x %02x %02x", base[rec + 412], base[rec + 413],
                    base[rec + 414], base[rec + 415], base[rec + 416], base[rec + 417], base[rec + 418], base[rec + 419]);
      tail = b;
    }
    REXLOG_INFO("[robots] definition {} asked by {:#x}{}", index, caller, tail);
  }
}

// The robot database loader (reads chEtc /CHAR/DAT records for the robot
// indices the game knows). Indices whose byte in the validity table at
// 0x82430618 is 0 (116-119, 126-129, 203-219) are skipped; extra robots get
// theirs set first.
REXCVAR_DEFINE_STRING(realsteel_extra_robots, "", "Real Steel",
                      "Development: robot indices to enable (comma separated)");

// Character select's robot list (sub_82152C68(?, list): +0 count, robot
// index at +4 * (i + 1), flag at +4 * (i + 226), sorted by the database's
// +398 key). Full robots with select = 0 (2026-10-17: the boosted copies
// that only fight in a single-player stage) are taken out after the game
// builds it; they stay in the database so stage ladders show their names.
extern "C" REX_FUNC(sub_82152C68) {
  const uint32_t list = ctx.r4.u32;
  __imp__sub_82152C68(ctx, base);
  const int n = int(REX_LOAD_U32(list));
  if (n <= 0 || n > 225) return;
  int kept = 0;
  for (int i = 0; i < n; ++i) {
    const uint32_t index = REX_LOAD_U32(list + 4 * (i + 1));
    const uint32_t flag = REX_LOAD_U32(list + 4 * (i + 226));
    if (realsteel::mods::FullRobotHidden(int(index))) continue;
    REX_STORE_U32(list + 4 * (kept + 1), index);
    REX_STORE_U32(list + 4 * (kept + 226), flag);
    ++kept;
  }
  REX_STORE_U32(list, uint32_t(kept));
}

extern "C" REX_FUNC(sub_82407508) {
  for (int index : realsteel::mods::FullRobotIndices()) {
    if (index > 0 && index < 225 && base[0x82430618u + index] != 1) {
      base[0x82430618u + index] = 1;
      REXLOG_INFO("[robots] robot index {} enabled for a full robot", index);
    }
  }
  const std::string list = REXCVAR_GET(realsteel_extra_robots);
  size_t start = 0;
  while (start < list.size()) {
    size_t end = list.find(',', start);
    if (end == std::string::npos) end = list.size();
    const int index = std::atoi(list.substr(start, end - start).c_str());
    if (index > 0 && index < 225) {
      base[0x82430618u + index] = 1;
      REXLOG_INFO("[robots] robot index {} enabled", index);
    }
    start = end + 1;
  }
  __imp__sub_82407508(ctx, base);
}

// Part stat records: r3 = table object, r4 = group, r5 = slot, r6 = part id
// -> 196-byte record (or 0). sub_82092598 is the same with the slot taken
// from the table object (r5 = part id). Logged once per (caller, group,
// slot, part) to see which parts a robot's stats come from.
namespace {
void LogPartStats(const char* which, uint32_t caller, uint32_t group, uint32_t slot, uint32_t part, uint32_t rec, uint8_t* base) {
  static std::mutex m;
  static std::set<std::string> seen;
  char key[96];
  std::snprintf(key, sizeof(key), "%s %x %u %u %u", which, caller, group, slot, part);
  std::lock_guard<std::mutex> lock(m);
  if (!seen.insert(key).second) return;
  const uint32_t hp = rec ? ((uint32_t(base[rec]) << 8) | base[rec + 1]) : 0;
  REXLOG_INFO("[robots] part stats {} by {:#x}: group {} slot {} part {} -> {:#x} (u16[0] {})", which, caller, group, slot, part, rec, hp);
}
}  // namespace

extern "C" REX_FUNC(sub_82092678) {
  const uint32_t caller = static_cast<uint32_t>(ctx.lr), group = ctx.r4.u32, slot = ctx.r5.u32, part = ctx.r6.u32;
  __imp__sub_82092678(ctx, base);
  if (REXCVAR_GET(realsteel_dump_robots)) LogPartStats("A", caller, group, slot, part, ctx.r3.u32, base);
}


// Called from the sub_82092598 hook in realsteel_mods.cpp (slot, part id ->
// stat record of the current group).
void realsteel_robots_part_stats(uint32_t caller, uint32_t slot, uint32_t id, uint32_t rec, uint8_t* base) {
  if (REXCVAR_GET(realsteel_dump_robots)) LogPartStats("B", caller, 99, slot, id, rec, base);
}

// Robot stats from its parts: r3 = stat tables, r4 = output (11 slots x 88
// bytes of floats), r5 = part ids (u32 per slot). Logged per distinct part
// list with the caller.
extern "C" REX_FUNC(sub_820C7E40) {
  const uint32_t caller = static_cast<uint32_t>(ctx.lr), out = ctx.r4.u32, ids = ctx.r5.u32;
  // the robot the totals (sub_820C8998) that follow on this thread are for
  realsteel::robots::t_lab_body = REX_LOAD_U32(ids + 4);
  __imp__sub_820C7E40(ctx, base);
  if (!REXCVAR_GET(realsteel_dump_robots)) return;
  std::string list;
  for (int s = 0; s < 11; ++s) list += std::to_string(REX_LOAD_U32(ids + 4 * s)) + " ";
  static std::mutex m;
  static std::set<std::string> seen;
  std::lock_guard<std::mutex> lock(m);
  if (!seen.insert(std::to_string(caller) + list).second) return;
  std::string f1;
  for (int k = 0; k < 22; ++k) {
    const uint32_t bits = REX_LOAD_U32(out + 88 + 4 * k);
    float v;
    std::memcpy(&v, &bits, 4);
    char b[24];
    std::snprintf(b, sizeof(b), "%g ", v);
    f1 += b;
  }
  REXLOG_INFO("[robots] stats from parts by {:#x}: ids {}| slot1 {}", caller, list, f1);
}

// A robot's part list (r3 = tables, r4 = output u32 x 11, r5 = robot index).
// A full robot's parts are its own (its stats): slots 1..7 from the mod
// loader; the chip slots 8..10 are Atom's (0) unless the loader gave others.
extern "C" REX_FUNC(sub_820C7350) {
  const uint32_t out = ctx.r4.u32, robot = ctx.r5.u32, caller = static_cast<uint32_t>(ctx.lr);
  __imp__sub_820C7350(ctx, base);
  uint32_t ids[11];
  if (realsteel::mods::FullRobotParts(int(robot), ids)) {
    for (int slot = 1; slot <= 10; ++slot) {
      const uint32_t id = slot <= 7 ? ids[slot] : 0;
      REX_STORE_U32(out + 4 * slot, id);
    }
    ctx.r3.u64 = 1;
  }
  if (!REXCVAR_GET(realsteel_dump_robots)) return;
  static std::mutex m;
  static std::set<uint32_t> seen;
  std::lock_guard<std::mutex> lock(m);
  if (!seen.insert(robot).second) return;
  std::string list;
  for (int s = 0; s < 11; ++s) list += std::to_string(int32_t(REX_LOAD_U32(out + 4 * s))) + " ";
  REXLOG_INFO("[robots] part list of robot {} (by {:#x}): {}", robot, caller, list);
}

// A fighter's part list (r3 = tables, r4 = output u32 x 11, r5 = fighter), used when a fight works the robots out
// (sub_820C94B8 -> sub_820C92E8). A full robot gets its own parts.
extern "C" REX_FUNC(sub_820C8388) {
  const uint32_t out = ctx.r4.u32, fighter = ctx.r5.u32;
  __imp__sub_820C8388(ctx, base);
  // +80: robot index * 100 + colour variant (5202), or a garage robot (0..6)
  const uint32_t id = REX_LOAD_U32(fighter + 80), robot = id >= 100 ? id / 100 : id;
  uint32_t ids[11];
  if (robot < 225 && realsteel::mods::FullRobotParts(int(robot), ids)) {
    for (int slot = 1; slot <= 10; ++slot) REX_STORE_U32(out + 4 * slot, slot <= 7 ? ids[slot] : 0);
    ctx.r3.u64 = 1;
  }
  if (REXCVAR_GET(realsteel_dump_robots)) {
    std::string list;
    for (int s = 0; s < 11; ++s) list += std::to_string(int32_t(REX_LOAD_U32(out + 4 * s))) + " ";
    REXLOG_INFO("[robots] fighter parts: robot {} -> {} (fighter {:#x})", robot, list, fighter);
  }
}

// Loadouts -> the 88-byte part tables (reads *(0x824AFD34) + 2052).
// Per robot slot (index -> slot through the u16 table at 0x824302E0) 11
// entries of 8 bytes (u16 part id, u16 ...): what fights build a robot from.
// The loadout records a full robot's index has are not loaded (the title
// update's 79 only), so its entries are Atom's (robot 100) with its own part
// ids in slots 1..7.
extern "C" REX_FUNC(sub_82118AB0) {
  __imp__sub_82118AB0(ctx, base);
  if (const uint32_t table = REX_LOAD_U32(0x824B0628u)) {
    auto slot_of = [&](int index) { return uint32_t(REX_LOAD_U16(0x824302E0u + 2 * index)); };
    const uint32_t atom = table + 0x17958 + slot_of(100) * 88;
    for (int index : realsteel::mods::FullRobotIndices()) {
      uint32_t ids[11];
      const uint32_t slot = slot_of(index);
      if (slot >= 225 || !realsteel::mods::FullRobotParts(index, ids)) continue;
      const uint32_t own = table + 0x17958 + slot * 88;
      std::memmove(base + own, base + atom, 88);
      for (int k = 1; k <= 7; ++k) REX_STORE_U16(own + k * 8, uint16_t(ids[k]));
      REXLOG_INFO("[robots] fight parts of robot {} (slot {}): {} {} {} {} {} {} {}", index, slot, ids[1], ids[2], ids[3], ids[4],
                  ids[5], ids[6], ids[7]);
    }
  }
  if (!REXCVAR_GET(realsteel_dump_robots)) return;
  const uint32_t holder = REX_LOAD_U32(0x824AFD34u);
  const uint32_t obj = holder ? holder + 2052 : 0;
  const uint32_t count = obj ? REX_LOAD_U32(obj + 4) : 0;
  const uint32_t table = REX_LOAD_U32(0x824B0628u);
  std::string slot2;
  for (int k = 0; k < 11 && table; ++k) {
    char b[24];
    std::snprintf(b, sizeof(b), "%u/%u ", REX_LOAD_U16(table + 0x17958 + 2 * 88 + k * 8), REX_LOAD_U16(table + 0x17958 + 2 * 88 + k * 8 + 2));
    slot2 += b;
  }
  std::string idx;
  const uint32_t arr = obj ? REX_LOAD_U32(obj + 20) : 0;
  for (uint32_t i = 0; arr && i < count && i < 120; ++i) idx += std::to_string(REX_LOAD_U16(arr + i * 8 + 2)) + " ";
  REXLOG_INFO("[robots] loadouts loaded: {} records; robot 52's table: {}; indices: {}", count, slot2, idx);
}

// ---------------------------------------------------------------------------
// Robot lab (the overlay's Debug tab): robot-wide figures of full robots.
//
// How the game works a robot out (sub_820C9220: tables, out, part ids, record,
// robot): sub_820C7E40 fills 11 blocks of 88 bytes (slots 1..10 = parts, 22
// words each), sub_820C8468 sums words 0..4 of the parts into the totals
// block (slot 11, out + 968), and sub_820C8998 adds the weight / power ratio
// brackets and works out the values fights use from the totals (out + 1064..:
// speed levels looked up from the clamped integer words). The figure getter
// sub_820913D8 reads the totals block:
//   word 0 Core HP, 1 power charge, 2 attack (arms), 3 defense (floats);
//   4 mobility, 5 movement speed, 6 punch speed, 8 resilience, 9 damage
//   recovery, 10 power recovery (integers); 18 weight.
// The lab runs sub_820C8998 twice for a full robot with values set: once to
// see what the brackets add, then with the integer words chosen so the
// result is the set value (so the fight's speed levels follow); float words
// are scaled afterwards together with the parts' own values (an arm's attack,
// a part's defense), since fights read those per part.
//
// Values come from the robot's robot.txt [feel] section (key = value) and
// can be changed live on the Debug tab; Save config writes them as a [feel]
// section to robot_lab_<name>.txt next to the game.
// ---------------------------------------------------------------------------
REXCVAR_DEFINE_STRING(realsteel_robot_lab_set, "", "Real Steel",
                      "Development: robot lab presets, robot:key:value;... (key as in [feel])");

namespace realsteel::robots {
namespace {
std::mutex g_lab_mutex;
std::map<int, LabRobot> g_lab;            // robot index -> state
bool g_lab_loaded = false;

const LabFeel* FeelByKey(const std::string& key) {
  for (const LabFeel& f : LabFeels())
    if (key == f.key) return &f;
  return nullptr;
}

const LabFeel* FeelByStat(int stat) {
  for (const LabFeel& f : LabFeels())
    if (f.stat == stat) return &f;
  return nullptr;
}

// [feel] of a robot.txt: key = value lines.
void ReadFeel(const std::string& folder, LabRobot& r) {
  if (folder.empty()) return;
  FILE* f = std::fopen((folder + "/robot.txt").c_str(), "r");
  if (f == nullptr) return;
  char buf[256];
  bool in_feel = false;
  while (std::fgets(buf, sizeof(buf), f)) {
    std::string line(buf);
    line = line.substr(0, line.find('#'));
    while (!line.empty() && std::isspace(uint8_t(line.back()))) line.pop_back();
    while (!line.empty() && std::isspace(uint8_t(line.front()))) line.erase(line.begin());
    if (line.empty()) continue;
    if (line.front() == '[') {
      std::string section;
      for (char c : line)
        if (std::isalpha(uint8_t(c))) section.push_back(char(std::tolower(uint8_t(c))));
      in_feel = section == "feel";
      continue;
    }
    const size_t eq = line.find('=');
    if (!in_feel || eq == std::string::npos) continue;
    std::string key;
    for (char c : line.substr(0, eq))
      if (!std::isspace(uint8_t(c))) key.push_back(char(std::tolower(uint8_t(c))));
    if (const LabFeel* feel = FeelByKey(key)) {
      r.feel[feel->stat] = std::atoi(line.c_str() + eq + 1);
    } else {
      REXLOG_WARN("[robots] {}/robot.txt: [feel] {} is not a figure the lab knows (ignored)", folder, key);
    }
  }
  std::fclose(f);
}

void LoadLabRobots() {
  if (g_lab_loaded) return;
  g_lab_loaded = true;
  for (const auto& [index, name] : realsteel::mods::FullRobots()) {
    LabRobot r;
    r.index = index;
    r.name = name;
    realsteel::mods::FullRobotParts(index, r.ids);
    ReadFeel(realsteel::mods::FullRobotFolder(index), r);
    g_lab[index] = r;
  }
  // --realsteel_robot_lab_set="robot:key:value;...": preset values (scripted
  // tests)
  const std::string preset = REXCVAR_GET(realsteel_robot_lab_set);
  size_t start = 0;
  while (start < preset.size()) {
    size_t end = preset.find(';', start);
    if (end == std::string::npos) end = preset.size();
    int robot = 0, value = 0;
    char key[40] = {};
    if (std::sscanf(preset.substr(start, end - start).c_str(), "%d:%39[a-z_]:%d", &robot, key, &value) == 3) {
      auto it = g_lab.find(robot);
      const LabFeel* feel = FeelByKey(key);
      if (it != g_lab.end() && feel != nullptr) it->second.feel[feel->stat] = value;
    }
    start = end + 1;
  }
}

float LoadF(uint8_t* base, uint32_t address) {
  const uint32_t bits = REX_LOAD_U32(address);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}

void StoreF(uint8_t* base, uint32_t address, float v) {
  uint32_t bits;
  std::memcpy(&bits, &v, 4);
  REX_STORE_U32(address, bits);
}

double Word(uint8_t* base, uint32_t totals, const LabFeel& f) {
  return f.is_float ? double(LoadF(base, totals + 4 * f.word)) : double(int32_t(REX_LOAD_U32(totals + 4 * f.word)));
}
}  // namespace

const std::vector<LabFeel>& LabFeels() {
  static const std::vector<LabFeel> kFeels = {
      {"core_hp", "Core HP", 0, 0, true},
      {"attack", "Attack (hits hurt)", 2, 2, true},
      {"defense", "Defense", 3, 3, true},
      {"punch_speed", "Punch speed", 7, 6, false},
      {"movement_speed", "Movement speed", 8, 5, false},
      {"mobility", "Mobility", 9, 4, false},
      {"resilience", "Resilience", 4, 8, false},
      {"damage_recovery", "Damage recovery", 5, 9, false},
      {"power_recovery", "Power recovery", 6, 10, false},
      {"power_charge", "Power charge", 1, 1, true},
  };
  return kFeels;
}

// Robot being worked out on this thread (its body part id), set by the
// sub_820C7E40 hook.
thread_local uint32_t t_lab_body = 0;

// sub_820C8998 (r3 = tables, r4 = out) with the lab's values for a full robot.
void LabTotals(PPCContext& ctx, uint8_t* base) {
  const uint32_t out = ctx.r4.u32, totals = out + 968;
  int index = -1;
  std::map<int, int> feel;
  {
    std::lock_guard<std::mutex> lock(g_lab_mutex);
    LoadLabRobots();
    for (const auto& [i, r] : g_lab)
      if (t_lab_body != 0 && r.ids[1] == t_lab_body) {
        index = i;
        feel = r.feel;
      }
  }
  if (index < 0) {
    __imp__sub_820C8998(ctx, base);
    return;
  }
  constexpr uint32_t kSize = 1100;   // the stat blocks and what sub_820C8998 derives
  std::vector<uint8_t> before(base + out, base + out + kSize);
  std::map<int, int32_t> pre;   // integer words before the brackets
  for (const LabFeel& f : LabFeels())
    if (!f.is_float) pre[f.word] = int32_t(REX_LOAD_U32(totals + 4 * f.word));
  const PPCContext saved = ctx;
  __imp__sub_820C8998(ctx, base);
  std::map<int, double> game;
  for (const LabFeel& f : LabFeels()) game[f.stat] = Word(base, totals, f);
  if (!feel.empty()) {
    // integer words: what the brackets add, taken off the set value
    std::map<int, int32_t> want;
    for (const auto& [stat, value] : feel) {
      const LabFeel* f = FeelByStat(stat);
      if (f == nullptr || f->is_float) continue;
      want[f->word] = value - (int32_t(game[stat]) - pre[f->word]);
    }
    std::memcpy(base + out, before.data(), kSize);
    for (const auto& [word, value] : want) REX_STORE_U32(totals + 4 * word, uint32_t(value));
    ctx = saved;
    __imp__sub_820C8998(ctx, base);
    // float words: scale the total and every part's value
    for (const auto& [stat, value] : feel) {
      const LabFeel* f = FeelByStat(stat);
      if (f == nullptr || !f->is_float) continue;
      const float now = LoadF(base, totals + 4 * f->word);
      if (now <= 0.0f) continue;
      const float scale = float(value) / now;
      for (int slot = 1; slot <= 11; ++slot) {
        const uint32_t a = out + slot * 88 + 4 * f->word;
        StoreF(base, a, LoadF(base, a) * scale);
      }
    }
  }
  std::lock_guard<std::mutex> lock(g_lab_mutex);
  auto it = g_lab.find(index);
  if (it != g_lab.end()) it->second.game = game;
}

std::vector<LabRobot> LabRobots() {
  std::lock_guard<std::mutex> lock(g_lab_mutex);
  LoadLabRobots();
  std::vector<LabRobot> out;
  for (const auto& [index, r] : g_lab) out.push_back(r);
  return out;
}

void LabSet(int index, int stat, int value) {
  std::lock_guard<std::mutex> lock(g_lab_mutex);
  LoadLabRobots();
  auto it = g_lab.find(index);
  if (it != g_lab.end()) it->second.feel[stat] = value;
}

void LabClear(int index, int stat) {
  std::lock_guard<std::mutex> lock(g_lab_mutex);
  auto it = g_lab.find(index);
  if (it != g_lab.end()) it->second.feel.erase(stat);
}

void LabReset(int index) {
  std::lock_guard<std::mutex> lock(g_lab_mutex);
  auto it = g_lab.find(index);
  if (it != g_lab.end()) it->second.feel.clear();
}

void LabTotal(int robot, int stat, double value) {
  std::lock_guard<std::mutex> lock(g_lab_mutex);
  LoadLabRobots();
  auto it = g_lab.find(robot);
  if (it != g_lab.end()) it->second.totals[stat] = value;
}

const char* LabFigureName(int stat) {
  static const char* kNames[] = {"Core HP", "Power charge", "Total attack", "Total defense", "Resilience",
                                 "Damage recovery", "Power recovery", "Punch speed", "Movement speed", "Mobility",
                                 "Battle rating", "Size", "Weight"};
  return stat >= 0 && stat < 13 ? kNames[stat] : "?";
}

// The set values as a robot.txt [feel] section, written to
// robot_lab_<name>.txt next to the game.
std::string LabSave(int index) {
  std::lock_guard<std::mutex> lock(g_lab_mutex);
  auto it = g_lab.find(index);
  if (it == g_lab.end()) return std::string();
  const LabRobot& r = it->second;
  std::string text = "# Robot lab values for " + r.name + " (robot " + std::to_string(r.index) +
                     "): the [feel] section of its robot.txt\n[feel]\n";
  for (const LabFeel& f : LabFeels()) {
    const auto v = r.feel.find(f.stat);
    if (v != r.feel.end()) text += std::string(f.key) + " = " + std::to_string(v->second) + "\n";
  }
  text += "\n# the game's own figures (without the lab):\n";
  for (const LabFeel& f : LabFeels()) {
    const auto g = r.game.find(f.stat);
    if (g != r.game.end()) text += "#   " + std::string(f.key) + " " + std::to_string(int(g->second + 0.5)) + "\n";
  }
  std::string file = "robot_lab_";
  for (char c : r.name) file.push_back(std::isalnum(uint8_t(c)) ? char(std::tolower(uint8_t(c))) : '_');
  file += ".txt";
  if (FILE* f = std::fopen(file.c_str(), "w")) {
    std::fputs(text.c_str(), f);
    std::fclose(f);
  }
  REXLOG_INFO("[robots] lab: {} saved to {}", r.name, file);
  return file;
}
}  // namespace realsteel::robots

// A robot worked out from its part ids (r5, u32 per slot; the body's at +4).
extern "C" REX_FUNC(sub_820C9220) {
  if (REXCVAR_GET(realsteel_dump_robots)) {
    static std::mutex m;
    static std::set<std::string> seen;
    std::string list;
    for (int s = 0; s < 11; ++s) list += std::to_string(int32_t(REX_LOAD_U32(ctx.r5.u32 + 4 * s))) + " ";
    const std::string key = std::to_string(uint32_t(ctx.lr)) + " " + list + std::to_string(ctx.r7.u32);
    std::lock_guard<std::mutex> lock(m);
    if (seen.insert(key).second)
      REXLOG_INFO("[robots] stats by {:#x}: ids {}| record {:#x} robot {}", uint32_t(ctx.lr), list, ctx.r6.u32, ctx.r7.u32);
  }
  __imp__sub_820C9220(ctx, base);
}

// The totals with the brackets (r3 = tables, r4 = out).
extern "C" REX_FUNC(sub_820C8998) {
  realsteel::robots::LabTotals(ctx, base);
}

// A robot-wide figure: r3 = robot index, r4 = stat id -> f1.
extern "C" REX_FUNC(sub_820913D8) {
  const uint32_t robot = ctx.r3.u32, stat = ctx.r4.u32;
  __imp__sub_820913D8(ctx, base);
  realsteel::robots::LabTotal(int(robot), int(stat), ctx.f1.f64);
  if (REXCVAR_GET(realsteel_dump_robots)) {
    static std::mutex m;
    static std::set<uint64_t> seen;
    std::lock_guard<std::mutex> lock(m);
    const int64_t v = int64_t(ctx.f1.f64 * 100.0);
    if (seen.insert((uint64_t(robot) << 48) | (uint64_t(stat) << 40) | (uint64_t(v) & 0xFFFFFFFFFFull)).second)
      REXLOG_INFO("[robots] figure robot {} stat {} = {}", robot, stat, ctx.f1.f64);
  }
}

// Motion banks: sub_820A9ED8(manager, data, slot) installs a bank (/MOT/<x>
// items 0..3) and sub_820A9F38 the event bank of a slot; sub_820A9FA0(slots,
// act, sub, track, ...) finds a motion (0 = not found). When the game
// installs its own bank (slot 0), the full robots' merged bank (the mod
// loader's FullRobotMotions: free act ids in the stock categories) goes into slot 2 -
// slot 1 is the title update's / an add-on's.
namespace {
uint32_t GuestCopy(uint8_t* base, const std::vector<uint8_t>& bytes, uint32_t& cache) {
  if (bytes.empty()) return 0;
  if (cache == 0) {
    cache = rex::system::kernel_memory()->SystemHeapAlloc(uint32_t(bytes.size()));
    if (cache != 0) std::memcpy(base + cache, bytes.data(), bytes.size());
  }
  return cache;
}
uint32_t g_robot_bank = 0, g_robot_events = 0;
constexpr uint32_t kRobotSlot = 2;
}  // namespace

extern "C" REX_FUNC(sub_820A9ED8) {
  const uint32_t manager = ctx.r3.u32, data = ctx.r4.u32, slot = ctx.r5.u32;
  const PPCContext saved = ctx;
  __imp__sub_820A9ED8(ctx, base);
  if (data != 0) {
    const uint32_t dir = data + 0x1000 + REX_LOAD_U16(data + 0x1000);
    REXLOG_INFO("[robots] motion bank slot {}: {} motions", slot, REX_LOAD_U32(dir));
  }
  if (slot != 0 || data == 0) return;
  const uint32_t ours = GuestCopy(base, realsteel::mods::FullRobotMotions(), g_robot_bank);
  if (ours == 0) return;
  PPCContext c = saved;
  c.r3.u64 = manager;
  c.r4.u64 = ours;
  c.r5.u64 = kRobotSlot;
  __imp__sub_820A9ED8(c, base);
  REXLOG_INFO("[robots] robots' motion bank in slot {} ({} motions)", kRobotSlot,
              REX_LOAD_U32(ours + 0x1000 + REX_LOAD_U16(ours + 0x1000)));
}

extern "C" REX_FUNC(sub_820A9F38) {
  const uint32_t manager = ctx.r3.u32, data = ctx.r4.u32, slot = ctx.r5.u32;
  const PPCContext saved = ctx;
  __imp__sub_820A9F38(ctx, base);
  if (slot != 0 || data == 0) return;
  const uint32_t ours = GuestCopy(base, realsteel::mods::FullRobotMotionEvents(), g_robot_events);
  if (ours == 0) return;
  PPCContext c = saved;
  c.r3.u64 = manager;
  c.r4.u64 = ours;
  c.r5.u64 = kRobotSlot;
  __imp__sub_820A9F38(c, base);
  REXLOG_INFO("[robots] robots' motion events in slot {}", kRobotSlot);
}

// Robot whose motion object is running on this thread (sub_8209FFD0 hook),
// -1 outside.
thread_local int t_motion_robot = -1;

extern "C" REX_FUNC(sub_820A9FA0) {
  const uint32_t act = ctx.r4.u32, sub = ctx.r5.u32 & 0xFF, track = ctx.r6.u32 & 0xFF, caller = uint32_t(ctx.lr);
  // a full robot's own motion for this move (its motion_map.txt), if it has one
  uint32_t own = 0;
  if (t_motion_robot >= 0 && realsteel::mods::FullRobotMotionAct(t_motion_robot, act, &own)) {
    const PPCContext saved = ctx;
    ctx.r4.u64 = own;
    __imp__sub_820A9FA0(ctx, base);
    if (ctx.r3.u32 != 0) {
      if (REXCVAR_GET(realsteel_dump_robots)) {
        static std::mutex m;
        static std::set<uint32_t> seen;
        std::lock_guard<std::mutex> lock(m);
        if (seen.insert((act << 16) | (sub << 8) | track).second)
          REXLOG_INFO("[robots] robot {} plays its own motion {} for act {} sub {}", t_motion_robot, own, act, sub);
      }
      return;
    }
    ctx = saved;
  }
  __imp__sub_820A9FA0(ctx, base);
  if (!REXCVAR_GET(realsteel_dump_robots)) return;
  static std::mutex m;
  static std::set<uint32_t> seen;
  std::lock_guard<std::mutex> lock(m);
  if (seen.insert((act << 16) | (sub << 8) | track).second)
    REXLOG_INFO("[robots] motion {} sub {} track {}: {} (by {:#x})", act, sub, track, ctx.r3.u32 ? "found" : "NOT FOUND", caller);
}

// Event bank lookup (act | 0x8000, sub, track), logged like the motions.
extern "C" REX_FUNC(sub_820AA0C8) {
  const uint32_t act = ctx.r4.u32, sub = ctx.r5.u32 & 0xFF, track = ctx.r6.u32 & 0xFF;
  __imp__sub_820AA0C8(ctx, base);
  if (!REXCVAR_GET(realsteel_dump_robots)) return;
  static std::mutex m;
  static std::set<uint32_t> seen;
  std::lock_guard<std::mutex> lock(m);
  if (seen.insert((act << 16) | (sub << 8) | track).second)
    REXLOG_INFO("[robots] events {} sub {} track {}: {}", act & 0x7FFF, sub, track, ctx.r3.u32 ? "found" : "NOT FOUND");
}

// A fighter's motion object M (class pointer 0x820028A4 at +0, itself at
// +24, the fighter at +32; fighter+80 = robot index * 100 + colour variant,
// or a garage robot 0..6) runs its current motion's event script
// (sub_8209FFD0(M); the pose is evaluated from there). Its motion state is
// at M+864 (act +68, sub +70, track +71).
namespace {
int RobotOfMotionObject(uint8_t* base, uint32_t m) {
  if (m < 0x10000 || REX_LOAD_U32(m) != 0x820028A4u || REX_LOAD_U32(m + 24) != m) return -1;
  const uint32_t fighter = REX_LOAD_U32(m + 32);
  if (fighter < 0x10000) return -1;
  const uint32_t id = REX_LOAD_U32(fighter + 80);
  return int(id >= 100 ? id / 100 : id);
}
}  // namespace

extern "C" REX_FUNC(sub_8209FFD0) {
  const int previous = t_motion_robot;
  t_motion_robot = RobotOfMotionObject(base, ctx.r3.u32);
  __imp__sub_8209FFD0(ctx, base);
  t_motion_robot = previous;
}

// A motion state S starts (looks its motion up): its motion object is
// S - 864 (the current state), else found by its markers just before S.
extern "C" REX_FUNC(sub_820A98A0) {
  const uint32_t state = ctx.r3.u32;
  int robot = RobotOfMotionObject(base, state - 864);
  for (uint32_t off = 4; robot < 0 && off <= 4096 && off < state; off += 4) robot = RobotOfMotionObject(base, state - off);
  const int previous = t_motion_robot;
  t_motion_robot = robot;
  __imp__sub_820A98A0(ctx, base);
  t_motion_robot = previous;
}
