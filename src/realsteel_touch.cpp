#include "realsteel_touch.h"

#include "realsteel_keyboard.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
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
// Adjustable controls (2026-10-15; the defaults are the original layout).
REXCVAR_DEFINE_DOUBLE(realsteel_touch_size, 1.0, "Real Steel",
                      "Touch controls size (0.6 - 1.6, 1 = original)");
REXCVAR_DEFINE_DOUBLE(realsteel_touch_opacity, 1.0, "Real Steel",
                      "Touch controls opacity (0.2 - 1, 1 = original)");
REXCVAR_DEFINE_STRING(realsteel_touch_layout, "", "Real Steel",
                      "Arranged touch controls: name:dx,dy,size,shape,degrees;... "
                      "(offsets as fractions of the screen; set by Arrange touch controls)");

namespace realsteel::touch {
namespace {

// Xbox 360 button bits (XINPUT_GAMEPAD_*).
constexpr uint16_t kUp = 0x0001, kDown = 0x0002, kLeft = 0x0004, kRight = 0x0008;
constexpr uint16_t kStart = 0x0010, kBack = 0x0020, kLB = 0x0100, kRB = 0x0200;
constexpr uint16_t kA = 0x1000, kB = 0x2000, kX = 0x4000, kY = 0x8000;

enum Role { kNone = -1, kStick = 0, kBtnA, kBtnB, kBtnX, kBtnY, kBtnLB, kBtnRB, kBtnLT, kBtnRT,
            kBtnBack, kBtnStart, kBtnSettings, kPadUp, kPadDown, kPadLeft, kPadRight, kRoleCount };

// What arrange mode moves, sizes, shapes and turns: every button on its own
// (2026-10-15), except the D-pad's four arrows, which go together.
enum Group { kGStick, kGPad, kGX, kGY, kGA, kGB, kGDodge, kGSway, kGTech, kGBlock,
             kGBack, kGStart, kGSettings, kGroupCount };
constexpr const char* kGroupNames[kGroupCount] = {"stick", "pad",   "x",     "y",     "a",
                                                  "b",     "dodge", "sway",  "tech",  "block",
                                                  "back",  "start", "settings"};
constexpr Group kRoleGroup[kRoleCount] = {
    kGStick, kGA, kGB, kGX, kGY, kGDodge, kGTech, kGSway, kGBlock,
    kGBack, kGStart, kGSettings, kGPad, kGPad, kGPad, kGPad};

// What the buttons do in a fight, from the game's own help pages (A/B/X/Y
// from the "Let circles charge" tip's glyph order; the shoulders tried one by
// one, 2026-10-11): A left heavy, B right heavy, X left light, Y right
// light, LT sway, RT guard, LB + direction dodge, RB + punch tech move,
// D-pad taunt.
struct Control {
  float x = 0, y = 0, r = 0;  // centre and radius (half width) in pixels
  const char* label = "";     // the action
  const char* button = "";    // the controller button, small
  uint16_t bit = 0;
  int shape = 0;              // 0 circle, 1 rounded square, 2 square, 3 kidney
  float scale = 1.0f;         // its group's size (labels follow)
  float angle = 0.0f;         // the shape's turn, radians clockwise
};

// The kidney (the GameCube pad's X and Y): a band bent around a centre below
// it, hollow side down, in units of the control's radius. kKidneyBend is the
// band's middle radius, kKidneyHalf half its thickness; it spans 2.2 radii.
constexpr float kKidneyBend = 1.3f, kKidneyHalf = 0.46f;
const float kKidneySpan = std::asin((1.1f - kKidneyHalf) / kKidneyBend);                     // half the bend's angle
const float kKidneyCentre = kKidneyBend * (1.0f + std::cos(kKidneySpan)) * 0.5f;  // centres the band

// A screen point in the control's own frame (its turn undone), from its centre.
ImVec2 Local(const Control& k, float x, float y) {
  const float dx = x - k.x, dy = y - k.y, c = std::cos(k.angle), s = std::sin(k.angle);
  return ImVec2(dx * c + dy * s, -dx * s + dy * c);
}

// A point of the control's own frame on the screen.
ImVec2 Screen(const Control& k, ImVec2 p) {
  const float c = std::cos(k.angle), s = std::sin(k.angle);
  return ImVec2(k.x + p.x * c - p.y * s, k.y + p.x * s + p.y * c);
}

// The kidney's spine at a bend angle (0 the middle, positive to the right).
ImVec2 KidneySpine(float r, float a, float out = 0.0f) {
  const float radius = (kKidneyBend + out) * r;
  return ImVec2(radius * std::sin(a), kKidneyCentre * r - radius * std::cos(a));
}

// How far a point is from a control, in its own shape's terms (a square's
// edge or the kidney's rim is as far as a circle's rim).
float Reach(const Control& k, float x, float y) {
  if (k.shape == 0) return std::hypot(x - k.x, y - k.y);
  const ImVec2 p = Local(k, x, y);
  if (k.shape != 3) return std::max(std::fabs(p.x), std::fabs(p.y));
  // The nearest point of the kidney's spine; its rim is kKidneyHalf away.
  const float a = std::max(-kKidneySpan, std::min(kKidneySpan,
                           std::atan2(p.x, kKidneyCentre * k.r - p.y)));
  const ImVec2 spine = KidneySpine(k.r, a);
  return std::hypot(p.x - spine.x, p.y - spine.y) + (1.0f - kKidneyHalf) * k.r;
}

// A non-round control's outline on the screen, clockwise.
int Outline(const Control& k, ImVec2* out, int capacity) {
  constexpr float kPi = 3.14159265f;
  int n = 0;
  auto add = [&](ImVec2 p) {
    if (n < capacity) out[n++] = Screen(k, p);
  };
  if (k.shape == 3) {
    const float r = k.r, half = kKidneyHalf * r;
    for (int i = 0; i <= 16; ++i) add(KidneySpine(r, -kKidneySpan + 2.0f * kKidneySpan * i / 16, kKidneyHalf));
    // Round ends: from the outer rim around the tip to the inner rim.
    auto end = [&](float a, float side) {
      const ImVec2 e = KidneySpine(r, a);
      const ImVec2 u(std::sin(a), -std::cos(a)), v(std::cos(a) * side, std::sin(a) * side);
      for (int i = 1; i < 10; ++i) {
        const float b = kPi * i / 10;
        const float cu = std::cos(b) * side, sv = std::sin(b);
        add(ImVec2(e.x + half * (u.x * cu + v.x * sv), e.y + half * (u.y * cu + v.y * sv)));
      }
    };
    end(kKidneySpan, 1.0f);
    for (int i = 16; i >= 0; --i) add(KidneySpine(r, -kKidneySpan + 2.0f * kKidneySpan * i / 16, -kKidneyHalf));
    end(-kKidneySpan, -1.0f);
    return n;
  }
  const float round = k.shape == 1 ? k.r * 0.35f : k.r * 0.08f, in = k.r - round;
  const float corners[4][2] = {{in, in}, {-in, in}, {-in, -in}, {in, -in}};
  for (int c = 0; c < 4; ++c) {
    for (int i = 0; i <= 6; ++i) {
      const float a = (c + i / 6.0f) * kPi * 0.5f;
      add(ImVec2(corners[c][0] + round * std::cos(a), corners[c][1] + round * std::sin(a)));
    }
  }
  return n;
}

// How far a control reaches from its centre, for keeping it on the screen.
float Extent(const Control& k) {
  if (k.shape == 0) return k.r;
  if (k.shape == 3) return 1.1f * k.r;
  return k.r * (std::fabs(std::cos(k.angle)) + std::fabs(std::sin(k.angle)));
}

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

// Each group's layout (realsteel_touch_layout): offset as fractions of the
// screen, size relative to the original, shape; and arrange mode's working
// copy. Format: name:dx,dy[,scale[,shape[,degrees]]];...
struct GroupLayout {
  float dx = 0.0f, dy = 0.0f, scale = 1.0f;
  int shape = 0;
  int degrees = 0;  // the shapes' turn, clockwise
  bool Moved() const {
    return dx != 0.0f || dy != 0.0f || scale != 1.0f || shape != 0 || degrees != 0;
  }
};
constexpr const char* kGroupTitles[kGroupCount] = {
    "STICK", "D-PAD", "LEFT LIGHT (X)", "RIGHT LIGHT (Y)", "LEFT HEAVY (A)", "RIGHT HEAVY (B)",
    "DODGE", "SWAY", "TECH", "BLOCK", "BACK", "START", "SETTINGS BUTTON"};
constexpr const char* kShapeNames[4] = {"CIRCLE", "ROUNDED", "SQUARE", "KIDNEY"};
GroupLayout g_layout[kGroupCount];
std::string g_layout_text;  // the cvar text g_layout was parsed from
std::atomic<bool> g_arranging{false};
std::atomic<bool> g_layout_saved{false};
GroupLayout g_edit[kGroupCount];
int g_selected = -1;  // arrange mode: the group the buttons change
std::map<SDL_FingerID, std::pair<int, std::pair<float, float>>> g_drag;  // finger -> group, last pos
// Arrange mode's buttons: x, y, w, h.
float g_button_done[4] = {}, g_button_reset[4] = {};
float g_button_smaller[4] = {}, g_button_bigger[4] = {}, g_button_shape[4] = {}, g_button_turn[4] = {};

void ParseLayout() {
  const std::string text = REXCVAR_GET(realsteel_touch_layout);
  if (text == g_layout_text) return;
  g_layout_text = text;
  for (GroupLayout& l : g_layout) l = GroupLayout{};
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find(';', at);
    if (end == std::string::npos) end = text.size();
    const std::string item = text.substr(at, end - at);
    at = end + 1;
    const size_t colon = item.find(':');
    if (colon == std::string::npos) continue;
    float dx = 0, dy = 0, scale = 1.0f;
    int shape = 0, degrees = 0;
    if (std::sscanf(item.c_str() + colon + 1, "%f,%f,%f,%d,%d", &dx, &dy, &scale, &shape, &degrees) < 2) {
      continue;
    }
    const std::string name = item.substr(0, colon);
    for (int g = 0; g < kGroupCount; ++g) {
      const bool old_group = (name == "punch" && g >= kGX && g <= kGB) ||
                             (name == "menu" && g >= kGBack && g <= kGSettings);
      if (name == kGroupNames[g] || old_group) {
        g_layout[g].dx = std::max(-1.0f, std::min(1.0f, dx));
        g_layout[g].dy = std::max(-1.0f, std::min(1.0f, dy));
        g_layout[g].scale = std::max(0.5f, std::min(2.0f, scale));
        g_layout[g].shape = std::max(0, std::min(3, shape));
        g_layout[g].degrees = ((degrees % 360) + 360) % 360;
      }
    }
  }
}

float SizeScale() {
  return std::max(0.6f, std::min(1.6f, float(REXCVAR_GET(realsteel_touch_size))));
}

void Layout(float w, float h) {
  const float s = std::max(0.6f, std::min(w, h) / 720.0f) * SizeScale();
  const float stick_r = 115.0f * s, btn_r = 52.0f * s, gap = 58.0f * s;
  g_controls[kStick] = {200.0f * s, h - 200.0f * s, stick_r, "", "", 0};
  // Punches as a 2 x 2 grid: left fist on the left, light on top.
  const float fx = w - 200.0f * s, fy = h - 200.0f * s;
  g_controls[kBtnX] = {fx - gap, fy - gap, btn_r, "L LIGHT", "X", kX};
  g_controls[kBtnY] = {fx + gap, fy - gap, btn_r, "R LIGHT", "Y", kY};
  g_controls[kBtnA] = {fx - gap, fy + gap, btn_r, "L HEAVY", "A", kA};
  g_controls[kBtnB] = {fx + gap, fy + gap, btn_r, "R HEAVY", "B", kB};
  const float sr = 44.0f * s;
  // Dodge and sway (2026-10-12, tried on a handheld): held with the right
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
  // Each group's layout (the arrange mode's working copy while it is open):
  // sized about the group's centre, shaped, moved; every control stays on
  // the screen.
  const GroupLayout* layout = g_arranging.load() ? g_edit : g_layout;
  float cx[kGroupCount] = {}, cy[kGroupCount] = {};
  int count[kGroupCount] = {};
  for (int c = 0; c < kRoleCount; ++c) {
    cx[kRoleGroup[c]] += g_controls[c].x;
    cy[kRoleGroup[c]] += g_controls[c].y;
    ++count[kRoleGroup[c]];
  }
  for (int c = 0; c < kRoleCount; ++c) {
    Control& k = g_controls[c];
    const int g = kRoleGroup[c];
    const GroupLayout& l = layout[g];
    const float mx = cx[g] / float(count[g]), my = cy[g] / float(count[g]);
    k.x = mx + (k.x - mx) * l.scale + l.dx * w;
    k.y = my + (k.y - my) * l.scale + l.dy * h;
    k.r *= l.scale;
    k.scale = l.scale;
    k.shape = c == kStick ? 0 : l.shape;  // the stick stays round
    k.angle = c == kStick ? 0.0f : float(l.degrees) * 3.14159265f / 180.0f;
    const float e = Extent(k);
    k.x = std::max(e, std::min(w - e, k.x));
    k.y = std::max(e, std::min(h - e, k.y));
  }
}

// Arrange mode: fingers drag groups; Done saves, Reset puts the original
// layout back. Nothing reaches the game meanwhile.
void TickArrange(float w, float h) {
  struct Touch { SDL_FingerID id; float x, y; };
  Touch touches[16];
  int touch_count = 0, device_count = 0;
  SDL_TouchID* devices = SDL_GetTouchDevices(&device_count);
  for (int d = 0; d < device_count && touch_count < 16; ++d) {
    int finger_count = 0;
    SDL_Finger** fingers = SDL_GetTouchFingers(devices[d], &finger_count);
    for (int f = 0; f < finger_count && touch_count < 16; ++f) {
      touches[touch_count++] = {fingers[f]->id, fingers[f]->x * w, fingers[f]->y * h};
    }
    if (fingers != nullptr) SDL_free(fingers);
  }
  if (devices != nullptr) SDL_free(devices);
  for (auto it = g_drag.begin(); it != g_drag.end();) {
    bool alive = false;
    for (int i = 0; i < touch_count; ++i) alive = alive || touches[i].id == it->first;
    it = alive ? std::next(it) : g_drag.erase(it);
  }
  auto inside = [](const float b[4], float x, float y) {
    return x >= b[0] && x <= b[0] + b[2] && y >= b[1] && y <= b[1] + b[3];
  };
  for (int i = 0; i < touch_count; ++i) {
    const Touch& t = touches[i];
    auto found = g_drag.find(t.id);
    if (found == g_drag.end()) {
      int group = -1;
      if (inside(g_button_done, t.x, t.y)) {
        char text[768] = {};
        size_t used = 0;
        for (int g = 0; g < kGroupCount; ++g) {
          const GroupLayout& l = g_edit[g];
          if (!l.Moved()) continue;
          used += size_t(std::snprintf(text + used, sizeof(text) - used, "%s%s:%.4f,%.4f,%.2f,%d,%d",
                                       used != 0 ? ";" : "", kGroupNames[g], l.dx, l.dy, l.scale,
                                       l.shape, l.degrees));
          if (used >= sizeof(text)) break;
        }
        rex::cvar::SetFlagByName("realsteel_touch_layout", text);
        g_arranging.store(false);
        g_layout_saved.store(true);
        g_drag.clear();
        REXLOG_INFO("[touch] layout saved: {}", text[0] != 0 ? text : "(original)");
        return;
      }
      if (inside(g_button_reset, t.x, t.y)) {
        for (GroupLayout& l : g_edit) l = GroupLayout{};
        g_selected = -1;
      } else if (g_selected >= 0 && inside(g_button_smaller, t.x, t.y)) {
        g_edit[g_selected].scale = std::max(0.5f, g_edit[g_selected].scale - 0.1f);
      } else if (g_selected >= 0 && inside(g_button_bigger, t.x, t.y)) {
        g_edit[g_selected].scale = std::min(2.0f, g_edit[g_selected].scale + 0.1f);
      } else if (g_selected >= 0 && g_selected != kGStick && inside(g_button_shape, t.x, t.y)) {
        g_edit[g_selected].shape = (g_edit[g_selected].shape + 1) % 4;
      } else if (g_selected >= 0 && g_selected != kGStick && g_edit[g_selected].shape != 0 &&
                 inside(g_button_turn, t.x, t.y)) {
        g_edit[g_selected].degrees = (g_edit[g_selected].degrees + 15) % 360;
      } else {
        float best = 1e30f;
        for (int c = 0; c < kRoleCount; ++c) {
          const Control& k = g_controls[c];
          const float d = Reach(k, t.x, t.y);
          if (d <= k.r * 1.3f && d < best) {
            best = d;
            group = kRoleGroup[c];
          }
        }
        if (group >= 0) g_selected = group;  // the size / shape buttons change it
      }
      found = g_drag.emplace(t.id, std::make_pair(group, std::make_pair(t.x, t.y))).first;
    }
    const int group = found->second.first;
    if (group >= 0) {
      g_edit[group].dx = std::max(-1.0f, std::min(1.0f, g_edit[group].dx + (t.x - found->second.second.first) / w));
      g_edit[group].dy = std::max(-1.0f, std::min(1.0f, g_edit[group].dy + (t.y - found->second.second.second) / h));
    }
    found->second.second = {t.x, t.y};
  }
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
  ParseLayout();
  if (g_arranging.load()) {
    if (!g_visible.load()) {
      g_visible.store(true);
      Clear();
    }
    if (width > 0 && height > 0) {
      Layout(width, height);
      TickArrange(width, height);
    }
    return;
  }
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
        const float d = Reach(k, t.x, t.y);
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
  const float s_all = std::max(0.6f, std::min(width, height) / 720.0f) * SizeScale();
  const float s = s_all;
  const bool arranging = g_arranging.load();
  const float op = arranging ? 1.0f
                             : std::max(0.2f, std::min(1.0f, float(REXCVAR_GET(realsteel_touch_opacity))));
  auto alpha = [op](int a) { return int(float(a) * op + 0.5f); };
  const ImU32 fill = IM_COL32(255, 255, 255, alpha(38)), fill_on = IM_COL32(90, 220, 225, alpha(110));
  const ImU32 edge_all = arranging ? IM_COL32(90, 220, 225, 230) : IM_COL32(255, 255, 255, alpha(120));
  const ImU32 text = IM_COL32(255, 255, 255, alpha(200));
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
    const float s = s_all * k.scale;  // labels follow the group's size
    const ImU32 edge = arranging && kRoleGroup[c] == g_selected ? IM_COL32(255, 210, 90, 255) : edge_all;
    if (c == kStick) {
      dl->AddCircleFilled(ImVec2(k.x, k.y), k.r, fill, 48);
      dl->AddCircle(ImVec2(k.x, k.y), k.r, edge, 48, 2.0f * s);
      const ImVec2 knob(k.x + g_stick_x * k.r * 0.6f, k.y + g_stick_y * k.r * 0.6f);
      dl->AddCircleFilled(knob, k.r * 0.42f, g_pressed[kStick] ? fill_on : IM_COL32(255, 255, 255, 70), 40);
      continue;
    }
    if (k.shape == 0) {
      dl->AddCircleFilled(ImVec2(k.x, k.y), k.r, g_pressed[c] ? fill_on : fill, 36);
      dl->AddCircle(ImVec2(k.x, k.y), k.r, edge, 36, 2.0f * s);
    } else {
      ImVec2 outline[64];
      const int n = Outline(k, outline, 64);
      if (k.shape == 3) {
        dl->AddConcavePolyFilled(outline, n, g_pressed[c] ? fill_on : fill);
      } else {
        dl->AddConvexPolyFilled(outline, n, g_pressed[c] ? fill_on : fill);
      }
      dl->AddPolyline(outline, n, edge, ImDrawFlags_Closed, 2.0f * s);
    }
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
      centred(k.label, size, k.y + (k.button[0] && k.shape != 3 ? 6.0f * s : 0.0f), text);
    }
    if (k.button[0] && k.shape == 3) {
      const ImVec2 at = Screen(k, KidneySpine(k.r, -kKidneySpan * 0.85f));
      const ImVec2 ts = font != nullptr ? font->CalcTextSizeA(13.0f * s, 1e9f, 0.0f, k.button)
                                        : ImGui::CalcTextSize(k.button);
      dl->AddText(font, 13.0f * s, ImVec2(at.x - ts.x * 0.5f, at.y - ts.y * 0.5f),
                  IM_COL32(150, 220, 225, 200), k.button);
    } else if (k.button[0]) {
      centred(k.button, 13.0f * s, k.y - k.r + 13.0f * s, IM_COL32(150, 220, 225, 200));
    }
  }
  // Arrange mode: instructions and buttons over the controls.
  if (arranging && font != nullptr) {
    // Instructions and the two buttons, in the middle of the screen.
    const float ui = std::max(0.6f, std::min(width, height) / 720.0f);
    const char* line = "Drag a control to move it, tap one to change it";
    const float size = 26.0f * ui;
    const ImVec2 ts = font->CalcTextSizeA(size, 1e9f, 0.0f, line);
    const float cy = height * 0.42f;
    dl->AddRectFilled(ImVec2(width * 0.5f - ts.x * 0.5f - 24 * ui, cy - 20 * ui),
                      ImVec2(width * 0.5f + ts.x * 0.5f + 24 * ui, cy + ts.y + 186 * ui),
                      IM_COL32(12, 16, 20, 220), 12 * ui);
    dl->AddText(font, size, ImVec2(width * 0.5f - ts.x * 0.5f, cy), IM_COL32(255, 255, 255, 240), line);
    const float bw = 150 * ui, bh = 56 * ui, by = cy + ts.y + 28 * ui;
    const float done[4] = {width * 0.5f + 10 * ui, by, bw, bh};
    const float reset[4] = {width * 0.5f - 10 * ui - bw, by, bw, bh};
    std::memcpy(g_button_done, done, sizeof(done));
    std::memcpy(g_button_reset, reset, sizeof(reset));
    auto button = [&](const float b[4], const char* label, ImU32 colour) {
      dl->AddRectFilled(ImVec2(b[0], b[1]), ImVec2(b[0] + b[2], b[1] + b[3]), colour, 10 * ui);
      const ImVec2 ls = font->CalcTextSizeA(22 * ui, 1e9f, 0.0f, label);
      dl->AddText(font, 22 * ui, ImVec2(b[0] + (b[2] - ls.x) * 0.5f, b[1] + (b[3] - ls.y) * 0.5f),
                  IM_COL32(255, 255, 255, 255), label);
    };
    button(reset, "RESET", IM_COL32(60, 70, 78, 240));
    button(done, "DONE", IM_COL32(28, 127, 138, 240));
    // Second row: the selected group's size and shape.
    const float sw = 110 * ui, row2 = by + bh + 14 * ui, x0 = width * 0.5f - sw * 2.0f - 18 * ui;
    const float smaller[4] = {x0, row2, sw, bh};
    const float bigger[4] = {x0 + sw + 12 * ui, row2, sw, bh};
    const float shape[4] = {x0 + (sw + 12 * ui) * 2.0f, row2, sw, bh};
    const float turn[4] = {x0 + (sw + 12 * ui) * 3.0f, row2, sw, bh};
    std::memcpy(g_button_smaller, smaller, sizeof(smaller));
    std::memcpy(g_button_bigger, bigger, sizeof(bigger));
    std::memcpy(g_button_shape, shape, sizeof(shape));
    std::memcpy(g_button_turn, turn, sizeof(turn));
    if (g_selected >= 0) {
      button(smaller, "SMALLER", IM_COL32(60, 70, 78, 240));
      button(bigger, "BIGGER", IM_COL32(60, 70, 78, 240));
      if (g_selected != kGStick) {
        button(shape, kShapeNames[(g_edit[g_selected].shape + 1) % 4], IM_COL32(60, 70, 78, 240));
        if (g_edit[g_selected].shape != 0) button(turn, "ROTATE", IM_COL32(60, 70, 78, 240));
      }
      const ImVec2 ns = font->CalcTextSizeA(18 * ui, 1e9f, 0.0f, kGroupTitles[g_selected]);
      dl->AddText(font, 18 * ui, ImVec2(width * 0.5f - ns.x * 0.5f, row2 + bh + 6 * ui),
                  IM_COL32(255, 210, 90, 255), kGroupTitles[g_selected]);
    } else {
      const char* hint = "Tap a control to change its size, shape or angle";
      const ImVec2 hs = font->CalcTextSizeA(18 * ui, 1e9f, 0.0f, hint);
      dl->AddText(font, 18 * ui, ImVec2(width * 0.5f - hs.x * 0.5f, row2 + bh * 0.3f),
                  IM_COL32(200, 210, 215, 220), hint);
    }
  }
}

bool Visible() { return g_visible.load(); }

bool HasTouch() { return HasTouchScreen(); }

bool TakeSettingsTap() { return g_settings_tap.exchange(false); }

void BeginArrange() {
  ParseLayout();
  for (int g = 0; g < kGroupCount; ++g) g_edit[g] = g_layout[g];
  g_selected = -1;
  g_drag.clear();
  g_arranging.store(true);
  REXLOG_INFO("[touch] arranging the controls");
}

bool Arranging() { return g_arranging.load(); }

bool TakeLayoutSaved() { return g_layout_saved.exchange(false); }

void Merge(keyboard::PadState* state) {
  if (!g_visible.load() || g_arranging.load() || state == nullptr) return;
  state->buttons |= g_buttons.load();
  state->left_trigger = std::max(state->left_trigger, g_lt.load());
  state->right_trigger = std::max(state->right_trigger, g_rt.load());
  const int16_t lx = g_lx.load(), ly = g_ly.load();
  if (lx != 0) state->lx = lx;
  if (ly != 0) state->ly = ly;
}

}  // namespace realsteel::touch
