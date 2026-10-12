#include "realsteel_touch.h"

#include "realsteel_keyboard.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

#include <SDL3/SDL.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef __ANDROID__
#define RS_TOUCH_DEFAULT "auto"
#else
#define RS_TOUCH_DEFAULT "off"  // desktop: only when switched on
#endif
REXCVAR_DEFINE_STRING(realsteel_touch_controls, RS_TOUCH_DEFAULT, "Real Steel",
                      "On-screen touch controls: auto (only on a touch screen with no "
                      "controller connected), on, off");

namespace realsteel::touch {
namespace {

// Xbox 360 button bits (XINPUT_GAMEPAD_*).
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008;
constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kLB = 0x0100, kRB = 0x0200;
constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

enum Role { kNone = -1, kStick = 0, kBtnA, kBtnB, kBtnX, kBtnY, kBtnLB, kBtnRB, kBtnLT, kBtnRT,
            kBtnBack, kBtnStart, kBtnSettings, kPadUp, kPadDown, kPadLeft, kPadRight, kRoleCount };

// What the buttons do in a fight, from the game's own help pages (A/B/X/Y
// from the "Let circles charge" tip's glyph order; the shoulders tried one by
// one, 2026-10-11): A left heavy, B right heavy, X left light, Y right
// light, LT sway, RT guard, LB + direction dodge, RB + punch tech move,
// D-pad taunt.
struct Control {
  float x = 0, y = 0, r = 0;  // centre and radius in pixels
  const char* label = "";     // the action
  const char* button = "";    // the controller button, small
  uint16_t bit = 0;
};

std::atomic<bool> g_visible{false};
std::atomic<uint16_t> g_buttons{0};
std::atomic<int16_t> g_lx{0}, g_ly{0};
std::atomic<uint8_t> g_lt{0}, g_rt{0};
std::atomic<bool> g_settings_tap{false};

// UI-thread state.
Control g_controls[kRoleCount];
std::map<SDL_FingerID, int> g_roles;        // finger -> what it pressed down on
float g_stick_x = 0.0f, g_stick_y = 0.0f;   // knob offset, -1..1
bool g_pressed[kRoleCount] = {};
bool g_logged_visible = false;

void Layout(float w, float h) {
  const float s = std::max(0.6f, std::min(w, h) / 720.0f);
  const float stick_r = 115.0f * s, btn_r = 52.0f * s, gap = 58.0f * s;
  g_controls[kStick] = {200.0f * s, h - 200.0f * s, stick_r, "", "", 0};
  // Punches as a 2 x 2 grid: left fist on the left, light on top.
  const float fx = w - 200.0f * s, fy = h - 200.0f * s;
  g_controls[kBtnX] = {fx - gap, fy - gap, btn_r, "L LIGHT", "X", kX};
  g_controls[kBtnY] = {fx + gap, fy - gap, btn_r, "R LIGHT", "Y", kY};
  g_controls[kBtnA] = {fx - gap, fy + gap, btn_r, "L HEAVY", "A", kA};
  g_controls[kBtnB] = {fx + gap, fy + gap, btn_r, "R HEAVY", "B", kB};
  const float sr = 44.0f * s;
  // Dodge and sway (2026-10-12, the author on the Nova): held with the right
  // thumb while the left one steers, so they sit in the arc the right thumb
  // sweeps around the punches - dodge left of the grid, sway above it.
  const float er = 50.0f * s;
  g_controls[kBtnLB] = {fx - gap - btn_r - 22.0f * s - er, fy + 18.0f * s, er, "DODGE", "LB", kLB};
  g_controls[kBtnLT] = {fx - 30.0f * s, fy - gap - btn_r - 22.0f * s - er, er, "SWAY", "LT", 0};
  g_controls[kBtnRB] = {w - 196.0f * s, 75.0f * s, sr, "TECH", "RB", kRB};
  g_controls[kBtnRT] = {w - 80.0f * s, 75.0f * s, sr, "BLOCK", "RT", 0};
  const float mr = 38.0f * s;
  g_controls[kBtnBack] = {w * 0.5f - 95.0f * s, 52.0f * s, mr, "BACK", "", kBack};
  g_controls[kBtnStart] = {w * 0.5f + 95.0f * s, 52.0f * s, mr, "START", "", kStart};
  g_controls[kBtnSettings] = {w * 0.5f, 52.0f * s, mr, "...", "", 0};
  // The D-pad, for taunts (and menus), above the stick.
  const float dx = 200.0f * s, dy = h - 430.0f * s, dg = 46.0f * s, dr = 27.0f * s;
  g_controls[kPadUp] = {dx, dy - dg, dr, "", "", kUp};
  g_controls[kPadDown] = {dx, dy + dg, dr, "", "", kDown};
  g_controls[kPadLeft] = {dx - dg, dy, dr, "", "", kLeft};
  g_controls[kPadRight] = {dx + dg, dy, dr, "", "", kRight};
}

bool HasTouchScreen() {
  int count = 0;
  SDL_TouchID* devices = SDL_GetTouchDevices(&count);
  if (devices != nullptr) SDL_free(devices);
  return count > 0;
}

void Clear() {
  g_buttons.store(0);
  g_lx.store(0);
  g_ly.store(0);
  g_lt.store(0);
  g_rt.store(0);
  g_roles.clear();
  g_stick_x = g_stick_y = 0.0f;
  std::memset(g_pressed, 0, sizeof(g_pressed));
}

}  // namespace

void Tick(float width, float height, bool page_open) {
  const std::string mode = REXCVAR_GET(realsteel_touch_controls);
  bool want = false;
  if (mode == "on") {
    want = true;
  } else if (mode != "off") {
    want = HasTouchScreen() && !SDL_HasGamepad();
  }
  if (page_open) want = false;  // the pages take the taps themselves
  if (want != g_visible.load()) {
    if (want != g_logged_visible) {
      REXLOG_INFO("[touch] on-screen controls {}", want ? "shown" : "hidden");
      g_logged_visible = want;
    }
    g_visible.store(want);
    if (!want) Clear();
  }
  if (!want || width <= 0 || height <= 0) return;
  Layout(width, height);

  // Every finger on every touch device, in window pixels.
  struct Touch { SDL_FingerID id; float x, y; };
  Touch touches[16];
  int touch_count = 0;
  int device_count = 0;
  SDL_TouchID* devices = SDL_GetTouchDevices(&device_count);
  for (int d = 0; d < device_count && touch_count < 16; ++d) {
    int finger_count = 0;
    SDL_Finger** fingers = SDL_GetTouchFingers(devices[d], &finger_count);
    for (int f = 0; f < finger_count && touch_count < 16; ++f) {
      touches[touch_count++] = {fingers[f]->id, fingers[f]->x * width, fingers[f]->y * height};
    }
    if (fingers != nullptr) SDL_free(fingers);
  }
  if (devices != nullptr) SDL_free(devices);

  // Fingers that lifted give up their control.
  for (auto it = g_roles.begin(); it != g_roles.end();) {
    bool alive = false;
    for (int i = 0; i < touch_count; ++i) alive = alive || touches[i].id == it->first;
    it = alive ? std::next(it) : g_roles.erase(it);
  }

  std::memset(g_pressed, 0, sizeof(g_pressed));
  g_stick_x = g_stick_y = 0.0f;
  for (int i = 0; i < touch_count; ++i) {
    const Touch& t = touches[i];
    auto found = g_roles.find(t.id);
    if (found == g_roles.end()) {
      // A new finger takes whatever it landed on (the stick has a wide grab).
      int role = kNone;
      float best = 1e30f;
      for (int c = 0; c < kRoleCount; ++c) {
        const Control& k = g_controls[c];
        const float reach = c == kStick ? k.r * 1.6f : k.r * 1.3f;
        const float d = std::hypot(t.x - k.x, t.y - k.y);
        if (d <= reach && d < best) {
          best = d;
          role = c;
        }
      }
      found = g_roles.emplace(t.id, role).first;
      if (role == kBtnSettings) g_settings_tap.store(true);
    }
    const int role = found->second;
    if (role == kNone) continue;
    g_pressed[role] = true;
    if (role == kStick) {
      const Control& k = g_controls[kStick];
      float vx = (t.x - k.x) / k.r, vy = (t.y - k.y) / k.r;
      const float len = std::hypot(vx, vy);
      if (len > 1.0f) {
        vx /= len;
        vy /= len;
      }
      g_stick_x = vx;
      g_stick_y = vy;
    }
  }

  // The stick never presses the D-pad (it taunts in fights); the menus
  // follow the stick.
  uint16_t buttons = 0;
  for (int c = kBtnA; c < kRoleCount; ++c) {
    if (g_pressed[c]) buttons |= g_controls[c].bit;
  }
  g_buttons.store(buttons);
  g_lx.store(int16_t(std::lround(g_stick_x * 32767.0f)));
  g_ly.store(int16_t(std::lround(-g_stick_y * 32767.0f)));
  g_lt.store(g_pressed[kBtnLT] ? 255 : 0);
  g_rt.store(g_pressed[kBtnRT] ? 255 : 0);
}

void Draw(ImDrawList* dl, ImFont* font, float width, float height) {
  if (!g_visible.load() || dl == nullptr) return;
  (void)width;
  const float s = std::max(0.6f, std::min(width, height) / 720.0f);
  const ImU32 fill = IM_COL32(255, 255, 255, 38), fill_on = IM_COL32(90, 220, 225, 110);
  const ImU32 edge = IM_COL32(255, 255, 255, 120), text = IM_COL32(255, 255, 255, 200);
  {
    const Control& up = g_controls[kPadUp];
    const Control& down = g_controls[kPadDown];
    if (up.r > 0.0f && font != nullptr) {
      const char* caption = "TAUNT";
      const float size = 13.0f * s;  // the button letters' size (already in the atlas)
      const ImVec2 ts = font->CalcTextSizeA(size, 1e9f, 0.0f, caption);
      dl->AddText(font, size, ImVec2(up.x - ts.x * 0.5f, (up.y + down.y) * 0.5f - ts.y * 0.5f),
                  IM_COL32(255, 255, 255, 150), caption);
    }
  }
  for (int c = 0; c < kRoleCount; ++c) {
    const Control& k = g_controls[c];
    if (k.r <= 0.0f) continue;
    if (c == kStick) {
      dl->AddCircleFilled(ImVec2(k.x, k.y), k.r, fill, 48);
      dl->AddCircle(ImVec2(k.x, k.y), k.r, edge, 48, 2.0f * s);
      const ImVec2 knob(k.x + g_stick_x * k.r * 0.6f, k.y + g_stick_y * k.r * 0.6f);
      dl->AddCircleFilled(knob, k.r * 0.42f, g_pressed[kStick] ? fill_on : IM_COL32(255, 255, 255, 70), 40);
      continue;
    }
    dl->AddCircleFilled(ImVec2(k.x, k.y), k.r, g_pressed[c] ? fill_on : fill, 36);
    dl->AddCircle(ImVec2(k.x, k.y), k.r, edge, 36, 2.0f * s);
    if (c >= kPadUp && c <= kPadRight) {
      // An arrow pointing away from the pad's centre.
      const float a = k.r * 0.45f;
      ImVec2 p1, p2, p3;
      if (c == kPadUp) { p1 = {k.x, k.y - a}; p2 = {k.x - a, k.y + a * 0.6f}; p3 = {k.x + a, k.y + a * 0.6f}; }
      if (c == kPadDown) { p1 = {k.x, k.y + a}; p2 = {k.x + a, k.y - a * 0.6f}; p3 = {k.x - a, k.y - a * 0.6f}; }
      if (c == kPadLeft) { p1 = {k.x - a, k.y}; p2 = {k.x + a * 0.6f, k.y + a}; p3 = {k.x + a * 0.6f, k.y - a}; }
      if (c == kPadRight) { p1 = {k.x + a, k.y}; p2 = {k.x - a * 0.6f, k.y - a}; p3 = {k.x - a * 0.6f, k.y + a}; }
      dl->AddTriangleFilled(p1, p2, p3, text);
      continue;
    }
    auto centred = [&](const char* str, float size, float cy, ImU32 colour) {
      const ImVec2 ts = font != nullptr ? font->CalcTextSizeA(size, 1e9f, 0.0f, str)
                                        : ImGui::CalcTextSize(str);
      dl->AddText(font, size, ImVec2(k.x - ts.x * 0.5f, cy - ts.y * 0.5f), colour, str);
    };
    const bool two_words = std::strchr(k.label, ' ') != nullptr;
    const float size = (two_words ? 17.0f : (std::strlen(k.label) > 4 ? 15.0f : 18.0f)) * s;
    if (two_words) {
      // "L HEAVY" -> "L" over "HEAVY" so it fits the circle.
      const char* space = std::strchr(k.label, ' ');
      const std::string first(k.label, space), second(space + 1);
      centred(first.c_str(), size, k.y - 9.0f * s, text);
      centred(second.c_str(), size, k.y + 11.0f * s, text);
    } else {
      centred(k.label, size, k.y + (k.button[0] ? 6.0f * s : 0.0f), text);
    }
    if (k.button[0]) {
      centred(k.button, 13.0f * s, k.y - k.r + 13.0f * s, IM_COL32(150, 220, 225, 200));
    }
  }
}

bool Visible() { return g_visible.load(); }

bool TakeSettingsTap() { return g_settings_tap.exchange(false); }

void Merge(keyboard::PadState* state) {
  if (!g_visible.load() || state == nullptr) return;
  state->buttons |= g_buttons.load();
  state->left_trigger = std::max(state->left_trigger, g_lt.load());
  state->right_trigger = std::max(state->right_trigger, g_rt.load());
  const int16_t lx = g_lx.load(), ly = g_ly.load();
  if (lx != 0) state->lx = lx;
  if (ly != 0) state->ly = ly;
}

}  // namespace realsteel::touch
