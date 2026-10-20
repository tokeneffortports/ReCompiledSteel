// The game's UI strings, with overrides (see realsteel_ui_text.h).
//
// sub_82083638 (the string manager's lookup, object at *0x8247C878) takes
// (this r3, id r4) and returns a pointer to the text, "Text_<id>" when no
// table has it. Overridden ids get a pointer into a guest-memory pool of
// fixed slots instead; a slot keeps its address for the session, so a
// screen that kept the pointer still shows the latest text.

#include "realsteel_ui_text.h"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "generated/default_final/realsteel_init.h"

REXCVAR_DEFINE_BOOL(realsteel_ui_text_log, false, "Real Steel",
                    "Development: log which UI string ids the game looks up (per second)");
REXCVAR_DEFINE_STRING(realsteel_ui_text_test, "", "Real Steel",
                      "Development: overrides as id=text pairs separated by |");

namespace realsteel::ui_text {
namespace {

constexpr uint32_t kSlots = 256;
constexpr uint32_t kSlotBytes = 256;

std::mutex g_mutex;
std::map<uint32_t, std::string> g_text;   // id -> override
std::map<uint32_t, uint32_t> g_slot;      // id -> slot index
uint32_t g_pool = 0;                      // guest address of the slots
uint32_t g_next_slot = 0;
bool g_dirty = false;
std::atomic<int> g_chosen_stage{-1};    // stage record of the last stage-menu choice
uint8_t* g_base = nullptr;                // guest memory base, from the hook
std::vector<std::pair<uint32_t, uint32_t>> g_pinned;  // [first, end) ranges

bool Pinned(uint32_t id) {
  for (const auto& [first, end] : g_pinned) {
    if (id >= first && id < end) return true;
  }
  return false;
}

// Writes |text| into |id|'s slot when it has one. Screens keep the pointer
// the lookup gave them, so this changes text already on screen.
void WriteSlot(uint32_t id, const std::string& text) {
  const auto slot = g_slot.find(id);
  if (g_base == nullptr || g_pool == 0 || slot == g_slot.end()) return;
  uint8_t* dst = g_base + g_pool + slot->second * kSlotBytes;
  std::memset(dst, 0, kSlotBytes);
  std::memcpy(dst, text.data(), std::min<size_t>(text.size(), kSlotBytes - 1));
}

// Log mode: lookups per id, written out every two seconds.
std::map<uint32_t, uint32_t> g_counts;
std::chrono::steady_clock::time_point g_last_log{};

}  // namespace

void Set(uint32_t id, const std::string& text) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_text[id] = text.substr(0, kSlotBytes - 1);
  g_dirty = true;
  WriteSlot(id, g_text[id]);
}

void Clear(uint32_t id) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_text.erase(id);
}

void Pin(uint32_t first, uint32_t count) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pinned.push_back({first, first + count});
}

}  // namespace realsteel::ui_text

using namespace realsteel::ui_text;

namespace {
// Override for |id| into ctx.r3, or false (the game's own lookup runs).
bool Lookup(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*)) {
  // 2026-10-17: the pre-fight screen's stage title (sub_82409B48, return
  // 0x82409DA0) asks for strings 205 + DLC stage - 1 but stops at DLC stage
  // 3, the last the official add-ons have. For DLC stages 4-9 (the mod
  // loader's, stage records 9-14 as last chosen in the stage menu, see the
  // sub_82157790 hook) it gets that stage's own string instead.
  if (uint32_t(ctx.lr) == 0x82409DA0u && ctx.r4.u32 >= 205 && ctx.r4.u32 <= 213) {
    const int record = g_chosen_stage.load();
    if (record >= 9 && record <= 14) ctx.r4.u32 = uint32_t(205 + (record - 6));
  }
  const uint32_t id = ctx.r4.u32;
  g_base = base;
  // Development: "uitext.trigger" (with --realsteel_dev_triggers) holds
  // id=text lines, applied while the game runs.
  static uint32_t calls = 0;
  if ((++calls & 127) == 0 && REXCVAR_GET(realsteel_ui_text_log)) {
    std::error_code ec;
    if (std::filesystem::exists("uitext.trigger", ec)) {
      std::ifstream f("uitext.trigger");
      std::string line;
      while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) Set(uint32_t(std::strtoul(line.c_str(), nullptr, 10)), line.substr(eq + 1));
      }
      f.close();
      std::filesystem::remove("uitext.trigger", ec);
    }
  }
  static bool test_loaded = false;
  if (!test_loaded) {
    test_loaded = true;
    // The OPTIONS panel's labels, help lines, title and bottom-bar entry
    // (realsteel_native_settings.cpp pages our settings through it).
    Pin(20050, 13);
    Pin(20097, 1);
    Pin(25122, 1);
    const std::string spec = REXCVAR_GET(realsteel_ui_text_test);
    size_t at = 0;
    while (at < spec.size()) {
      size_t end = spec.find('|', at);
      if (end == std::string::npos) end = spec.size();
      const std::string item = spec.substr(at, end - at);
      const size_t eq = item.find('=');
      if (eq != std::string::npos) Set(uint32_t(std::strtoul(item.c_str(), nullptr, 10)), item.substr(eq + 1));
      at = end + 1;
    }
  }
  if (REXCVAR_GET(realsteel_ui_text_log)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    static std::map<uint64_t, bool> callers;
    const uint64_t key = (uint64_t(id) << 32) | uint32_t(ctx.lr);
    if (!callers[key]) {
      callers[key] = true;
      REXLOG_INFO("[ui-text] id {} first looked up from {:08X} (r31 {:08X})", id, uint32_t(ctx.lr), ctx.r31.u32);
    }
    ++g_counts[id];
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_log > std::chrono::seconds(2)) {
      std::string line;
      for (const auto& [i, n] : g_counts) line += " " + std::to_string(i) + "x" + std::to_string(n);
      REXLOG_INFO("[ui-text] lookups in the last 2 s:{}", line);
      g_counts.clear();
      g_last_log = now;
    }
  }
  bool pinned_without_text = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    pinned_without_text = Pinned(id) && g_text.find(id) == g_text.end();
  }
  if (pinned_without_text) {
    // The game's own text, copied into a slot from now on.
    const PPCContext saved = ctx;
    original(ctx, base);
    const uint32_t text = ctx.r3.u32;
    ctx = saved;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_text[id] = text != 0 ? std::string(reinterpret_cast<const char*>(base + text)).substr(0, kSlotBytes - 1)
                           : std::string();
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto it = g_text.find(id);
    if (it != g_text.end()) {
      if (g_pool == 0) {
        g_pool = rex::system::kernel_memory()->SystemHeapAlloc(kSlots * kSlotBytes);
        if (g_pool != 0) std::memset(base + g_pool, 0, kSlots * kSlotBytes);
      }
      auto slot = g_slot.find(id);
      if (slot == g_slot.end() && g_next_slot < kSlots) {
        slot = g_slot.emplace(id, g_next_slot++).first;
      }
      if (g_pool != 0 && slot != g_slot.end()) {
        uint8_t* dst = base + g_pool + slot->second * kSlotBytes;
        std::memset(dst, 0, kSlotBytes);
        std::memcpy(dst, it->second.data(), it->second.size());
        ctx.r3.u64 = g_pool + slot->second * kSlotBytes;
        return true;
      }
    }
  }
  return false;
}
}  // namespace

// The string manager's lookup in the class the game actually uses (vtable
// 0x82000E0C, slot 5: tables picked by id range, 20000+ menus, 25000+
// help, 30000+ misc) - and the base class's (vtable 0x820023A0, slot 6).
extern "C" REX_FUNC(sub_82083638) {
  if (!Lookup(ctx, base, __imp__sub_82083638)) __imp__sub_82083638(ctx, base);
}
extern "C" REX_FUNC(sub_82098EA8) {
  if (!Lookup(ctx, base, __imp__sub_82098EA8)) __imp__sub_82098EA8(ctx, base);
}

// Stage menu item -> stage record (0-4 main stages, 6-14 DLC stages 1-9,
// 15 free fight, 16 the DLC submenu). Remembered for the stage title above.
extern "C" REX_FUNC(sub_82157790) {
  __imp__sub_82157790(ctx, base);
  realsteel::ui_text::g_chosen_stage.store(int(ctx.r3.u32));
}
