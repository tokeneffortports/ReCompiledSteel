// realsteel - keyboard controls (2026-10-09).
//
// The game only reads Xbox pads. With --realsteel_keyboard=true (the
// "Keyboard controls" setting) keys mapped through the realsteel_key_*
// settings are folded into pad 0: the settings page shows one row per
// action and rebinds a key by pressing it. Key state comes from the window
// (UI thread) and is read by the game's input hooks (guest thread).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace realsteel::keyboard {

struct Action {
  const char* label;        // shown on the settings page
  const char* cvar;         // realsteel_key_...
  const char* default_key;  // key name
  const char* help;
};
const std::vector<Action>& Actions();

// Key name <-> Windows virtual-key code ("Space", "A", "Left", "F5", ...);
// KeyCode returns 0 for an unknown name.
std::string KeyName(uint16_t vk);
uint16_t KeyCode(const std::string& name);

// UI thread: a key went down or up.
void OnKey(uint16_t vk, bool down);
// Re-reads the realsteel_key_* settings (after one changed).
void ReloadMapping();
// The "Keyboard controls" setting.
bool Enabled();
// UI thread: while true (a text field has the keyboard) keys are not pad
// buttons.
void SetTyping(bool typing);

// Pad 0 as the keyboard currently holds it. Returns false when keyboard
// controls are off (out is untouched).
struct PadState {
  uint16_t buttons = 0;
  uint8_t left_trigger = 0, right_trigger = 0;
  int16_t lx = 0, ly = 0, rx = 0, ry = 0;
};
bool Read(PadState* out);

}  // namespace realsteel::keyboard
