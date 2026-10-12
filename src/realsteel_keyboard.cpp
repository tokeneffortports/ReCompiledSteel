// realsteel - keyboard controls. See the header.

#include "realsteel_keyboard.h"
#include "realsteel_touch.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(realsteel_keyboard, true, "Real Steel",
                    "Keyboard controls: mapped keys act as pad 0");

// One setting per action; the defaults are the Windows virtual-key names
// below. Left-hand movement, right-hand buttons.
REXCVAR_DEFINE_STRING(realsteel_key_up, "W", "Real Steel", "Key: move up / D-pad up / left stick up");
REXCVAR_DEFINE_STRING(realsteel_key_down, "S", "Real Steel", "Key: move down");
REXCVAR_DEFINE_STRING(realsteel_key_left, "A", "Real Steel", "Key: move left");
REXCVAR_DEFINE_STRING(realsteel_key_right, "D", "Real Steel", "Key: move right");
REXCVAR_DEFINE_STRING(realsteel_key_rs_up, "Up", "Real Steel", "Key: right stick up");
REXCVAR_DEFINE_STRING(realsteel_key_rs_down, "Down", "Real Steel", "Key: right stick down");
REXCVAR_DEFINE_STRING(realsteel_key_rs_left, "Left", "Real Steel", "Key: right stick left");
REXCVAR_DEFINE_STRING(realsteel_key_rs_right, "Right", "Real Steel", "Key: right stick right");
REXCVAR_DEFINE_STRING(realsteel_key_a, "Space", "Real Steel", "Key: A button");
REXCVAR_DEFINE_STRING(realsteel_key_b, "Backspace", "Real Steel", "Key: B button");
REXCVAR_DEFINE_STRING(realsteel_key_x, "J", "Real Steel", "Key: X button");
REXCVAR_DEFINE_STRING(realsteel_key_y, "K", "Real Steel", "Key: Y button");
REXCVAR_DEFINE_STRING(realsteel_key_lb, "U", "Real Steel", "Key: left bumper");
REXCVAR_DEFINE_STRING(realsteel_key_rb, "I", "Real Steel", "Key: right bumper");
REXCVAR_DEFINE_STRING(realsteel_key_lt, "Shift", "Real Steel", "Key: left trigger");
REXCVAR_DEFINE_STRING(realsteel_key_rt, "Ctrl", "Real Steel", "Key: right trigger");
REXCVAR_DEFINE_STRING(realsteel_key_l3, "C", "Real Steel", "Key: left stick click");
REXCVAR_DEFINE_STRING(realsteel_key_r3, "V", "Real Steel", "Key: right stick click");
REXCVAR_DEFINE_STRING(realsteel_key_start, "Enter", "Real Steel", "Key: Start");
REXCVAR_DEFINE_STRING(realsteel_key_back, "Tab", "Real Steel", "Key: Back");
// The D-pad: taunts in fights (2026-10-11: the move keys no longer press it,
// so walking stopped turning into taunts).
REXCVAR_DEFINE_STRING(realsteel_key_taunt_up, "1", "Real Steel", "Key: D-pad up (taunt)");
REXCVAR_DEFINE_STRING(realsteel_key_taunt_down, "2", "Real Steel", "Key: D-pad down (taunt)");
REXCVAR_DEFINE_STRING(realsteel_key_taunt_left, "3", "Real Steel", "Key: D-pad left (taunt)");
REXCVAR_DEFINE_STRING(realsteel_key_taunt_right, "4", "Real Steel", "Key: D-pad right (taunt)");

namespace realsteel::keyboard {
namespace {

// Pad bits (XInput) and what each action does.
enum : uint16_t {
  kDpadUp = 0x0001, kDpadDown = 0x0002, kDpadLeft = 0x0004, kDpadRight = 0x0008,
  kStart = 0x0010, kBack = 0x0020, kL3 = 0x0040, kR3 = 0x0080, kLB = 0x0100, kRB = 0x0200,
  kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000
};
enum ActionIndex {
  kUp, kDown, kLeft, kRight, kRsUp, kRsDown, kRsLeft, kRsRight,
  kActA, kActB, kActX, kActY, kActLB, kActRB, kActLT, kActRT, kActL3, kActR3, kActStart, kActBack,
  kTauntUp, kTauntDown, kTauntLeft, kTauntRight,
  kActionCount
};

const std::vector<Action>& ActionTable() {
  static const std::vector<Action> actions = {
      {"Move up", "realsteel_key_up", "W", "Left stick up: move, and menus."},
      {"Move down", "realsteel_key_down", "S", "Left stick down (with Guard: low guard)."},
      {"Move left", "realsteel_key_left", "A", "Left stick left."},
      {"Move right", "realsteel_key_right", "D", "Left stick right."},
      {"Right stick up", "realsteel_key_rs_up", "Up", "Right stick (camera, some moves)."},
      {"Right stick down", "realsteel_key_rs_down", "Down", "Right stick."},
      {"Right stick left", "realsteel_key_rs_left", "Left", "Right stick."},
      {"Right stick right", "realsteel_key_rs_right", "Right", "Right stick."},
      {"A: left heavy punch", "realsteel_key_a", "Space", "A button. Confirm in menus."},
      {"B: right heavy punch", "realsteel_key_b", "Backspace", "B button. Back in menus."},
      {"X: left light punch", "realsteel_key_x", "J", "X button."},
      {"Y: right light punch", "realsteel_key_y", "K", "Y button."},
      {"LB: dodge", "realsteel_key_lb", "U", "Left bumper; dodge with a direction."},
      {"RB: tech move", "realsteel_key_rb", "I", "Right bumper; hold with a punch for a tech move."},
      {"LT: sway", "realsteel_key_lt", "Shift", "Left trigger; crouch and sway."},
      {"RT: guard", "realsteel_key_rt", "Ctrl", "Right trigger; block (with down: low guard)."},
      {"Left stick click", "realsteel_key_l3", "C", "L3. L3 + R3 together open this page."},
      {"Right stick click", "realsteel_key_r3", "V", "R3."},
      {"Start", "realsteel_key_start", "Enter", "Start: pause, skip."},
      {"Back", "realsteel_key_back", "Tab", "Back (Select)."},
      {"Taunt (D-pad up)", "realsteel_key_taunt_up", "1", "D-pad up: a taunt in fights."},
      {"Taunt (D-pad down)", "realsteel_key_taunt_down", "2", "D-pad down."},
      {"Taunt (D-pad left)", "realsteel_key_taunt_left", "3", "D-pad left."},
      {"Taunt (D-pad right)", "realsteel_key_taunt_right", "4", "D-pad right."},
  };
  return actions;
}

struct Named {
  const char* name;
  uint16_t vk;
};
const Named kNamed[] = {
    {"Space", 0x20}, {"Enter", 0x0D}, {"Backspace", 0x08}, {"Tab", 0x09}, {"Escape", 0x1B},
    {"Shift", 0x10}, {"Ctrl", 0x11}, {"Alt", 0x12}, {"CapsLock", 0x14},
    {"Up", 0x26}, {"Down", 0x28}, {"Left", 0x25}, {"Right", 0x27},
    {"Insert", 0x2D}, {"Delete", 0x2E}, {"Home", 0x24}, {"End", 0x23}, {"PageUp", 0x21}, {"PageDown", 0x22},
    {"Numpad0", 0x60}, {"Numpad1", 0x61}, {"Numpad2", 0x62}, {"Numpad3", 0x63}, {"Numpad4", 0x64},
    {"Numpad5", 0x65}, {"Numpad6", 0x66}, {"Numpad7", 0x67}, {"Numpad8", 0x68}, {"Numpad9", 0x69},
    {"Numpad*", 0x6A}, {"Numpad+", 0x6B}, {"Numpad-", 0x6D}, {"Numpad.", 0x6E}, {"Numpad/", 0x6F},
    {";", 0xBA}, {"=", 0xBB}, {",", 0xBC}, {"-", 0xBD}, {".", 0xBE}, {"/", 0xBF}, {"`", 0xC0},
    {"[", 0xDB}, {"\\", 0xDC}, {"]", 0xDD}, {"'", 0xDE},
};

std::atomic<uint64_t> g_down[4];              // one bit per virtual-key code
std::atomic<uint16_t> g_mapping[kActionCount];  // action -> virtual key (0 = none)
std::mutex g_mutex;

// Left / right variants of the modifiers count as the plain key.
uint16_t Normalise(uint16_t vk) {
  if (vk == 0xA0 || vk == 0xA1) return 0x10;
  if (vk == 0xA2 || vk == 0xA3) return 0x11;
  if (vk == 0xA4 || vk == 0xA5) return 0x12;
  return vk;
}

bool IsDown(uint16_t vk) {
  return vk != 0 && ((g_down[vk >> 6].load(std::memory_order_relaxed) >> (vk & 63)) & 1) != 0;
}

}  // namespace

const std::vector<Action>& Actions() { return ActionTable(); }

std::string KeyName(uint16_t vk) {
  vk = Normalise(vk);
  if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, char(vk));
  if (vk >= 0x70 && vk <= 0x7B) return "F" + std::to_string(vk - 0x70 + 1);
  for (const Named& n : kNamed)
    if (n.vk == vk) return n.name;
  char b[16];
  std::snprintf(b, sizeof(b), "Key%02X", vk);
  return b;
}

uint16_t KeyCode(const std::string& name) {
  if (name.size() == 1) {
    const char c = char(std::toupper(uint8_t(name[0])));
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return uint16_t(c);
  }
  if (name.size() >= 2 && (name[0] == 'F' || name[0] == 'f') && std::isdigit(uint8_t(name[1]))) {
    const int n = std::atoi(name.c_str() + 1);
    if (n >= 1 && n <= 12) return uint16_t(0x70 + n - 1);
  }
  for (const Named& n : kNamed) {
    if (std::strlen(n.name) == name.size()) {
      bool same = true;
      for (size_t i = 0; i < name.size(); ++i) same = same && std::tolower(uint8_t(n.name[i])) == std::tolower(uint8_t(name[i]));
      if (same) return n.vk;
    }
  }
  if (name.size() == 5 && name.rfind("Key", 0) == 0) return uint16_t(std::strtoul(name.c_str() + 3, nullptr, 16));
  return 0;
}

void OnKey(uint16_t vk, bool down) {
  {
    static std::atomic<int> logs{0};
    if (down && logs.fetch_add(1) < 6) REXLOG_INFO("[keyboard] key {:#x} ({}) down, enabled {}", vk, KeyName(vk), Enabled());
  }
  vk = Normalise(vk);
  if (vk == 0 || vk >= 256) return;
  const uint64_t bit = 1ull << (vk & 63);
  if (down) {
    g_down[vk >> 6].fetch_or(bit, std::memory_order_relaxed);
  } else {
    g_down[vk >> 6].fetch_and(~bit, std::memory_order_relaxed);
  }
}

void ReloadMapping() {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto& actions = ActionTable();
  for (size_t i = 0; i < actions.size() && i < kActionCount; ++i) {
    g_mapping[i].store(KeyCode(rex::cvar::GetFlagByName(actions[i].cvar)), std::memory_order_relaxed);
  }
}

bool Enabled() { return REXCVAR_GET(realsteel_keyboard); }

std::atomic<bool> g_typing{false};
void SetTyping(bool typing) { g_typing.store(typing, std::memory_order_relaxed); }

bool Read(PadState* out) {
  // On-screen touch controls feed the same pad state (realsteel_touch.cpp).
  if ((!Enabled() && !realsteel::touch::Visible()) || g_typing.load(std::memory_order_relaxed)) {
    return false;
  }
  static std::once_flag once;
  std::call_once(once, ReloadMapping);
  bool held[kActionCount];
  for (int i = 0; i < kActionCount; ++i) held[i] = IsDown(g_mapping[i].load(std::memory_order_relaxed));
  PadState s;
  // Moving is the left stick only: the D-pad taunts in fights. Menus follow
  // the stick as well (checked 2026-10-11).
  if (held[kTauntUp]) s.buttons |= kDpadUp;
  if (held[kTauntDown]) s.buttons |= kDpadDown;
  if (held[kTauntLeft]) s.buttons |= kDpadLeft;
  if (held[kTauntRight]) s.buttons |= kDpadRight;
  if (held[kActA]) s.buttons |= kA;
  if (held[kActB]) s.buttons |= kB;
  if (held[kActX]) s.buttons |= kX;
  if (held[kActY]) s.buttons |= kY;
  if (held[kActLB]) s.buttons |= kLB;
  if (held[kActRB]) s.buttons |= kRB;
  if (held[kActL3]) s.buttons |= kL3;
  if (held[kActR3]) s.buttons |= kR3;
  if (held[kActStart]) s.buttons |= kStart;
  if (held[kActBack]) s.buttons |= kBack;
  s.left_trigger = held[kActLT] ? 255 : 0;
  s.right_trigger = held[kActRT] ? 255 : 0;
  s.lx = int16_t((held[kRight] ? 32767 : 0) - (held[kLeft] ? 32767 : 0));
  s.ly = int16_t((held[kUp] ? 32767 : 0) - (held[kDown] ? 32767 : 0));
  s.rx = int16_t((held[kRsRight] ? 32767 : 0) - (held[kRsLeft] ? 32767 : 0));
  s.ry = int16_t((held[kRsUp] ? 32767 : 0) - (held[kRsDown] ? 32767 : 0));
  realsteel::touch::Merge(&s);
  *out = s;
  return true;
}

}  // namespace realsteel::keyboard
