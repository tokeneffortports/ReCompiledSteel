// realsteel - on-screen touch controls (2026-10-11).
//
// A virtual pad for touch screens: left stick, A/B/X/Y, shoulders and
// triggers, Back/Start, and a button that opens the settings page (L3 + R3
// cannot be pressed on glass). Shown by realsteel_touch_controls: "auto"
// (default) only when the device has a touch screen and no controller is
// connected, "on", or "off". The buttons reach the game through the keyboard
// module's pad state, so menus and fights see them like a controller.

#pragma once

#include <cstdint>

struct ImDrawList;
struct ImFont;

namespace realsteel::keyboard {
struct PadState;
}

namespace realsteel::touch {

// UI thread, every overlay tick: decides whether the controls show and reads
// the fingers (window size in pixels).
void Tick(float width, float height, bool page_open);
// UI thread, inside an ImGui frame: draws the controls.
void Draw(ImDrawList* draw_list, ImFont* font, float width, float height);
// The controls are on screen.
bool Visible();
// The device has a touch screen (pages widen their scrollbars).
bool HasTouch();
// The settings button was tapped since the last call.
bool TakeSettingsTap();
// Guest thread: adds the touched buttons and stick to a pad state.
void Merge(keyboard::PadState* state);
// Arrange mode (Settings > Controls > Arrange touch controls): the controls
// show with outlines, fingers drag them in groups, DONE saves the layout to
// realsteel_touch_layout, RESET restores the original. UI thread.
void BeginArrange();
bool Arranging();
// The arrange mode saved a layout since the last call (the settings file
// should be written).
bool TakeLayoutSaved();

}  // namespace realsteel::touch
