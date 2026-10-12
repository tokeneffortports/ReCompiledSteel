// realsteel - in-game settings page and UI overlay (2026-10-02).
//
// With the built-in GPU service (realsteel_gpu.h) the SDK has no presenter to
// draw its Dear ImGui overlays with, so the game owns a small overlay stack:
// an rex::ui::ImGuiDrawer whose draw lists are recorded on the UI thread and
// handed to the native renderer, which draws them over the picture in its
// present pass (rs::vk::OverlayFrame).
//
// The settings page is opened with F1 or by clicking both sticks (L3 + R3);
// while it is open the game does not see the controller. Every entry is a row
// of the table in realsteel_overlay.cpp bound to a cvar - add a row to add a
// setting. Values apply immediately and persist in realsteel_settings.cfg.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace rex {
class Runtime;
namespace ui {
class Window;
class WindowedAppContext;
}  // namespace ui
}  // namespace rex

namespace rs::vk {
class Renderer;
}

namespace realsteel::overlay {

// Applies realsteel_settings.cfg (values given on the command line win). Call
// before the window and the runtime are created.
void LoadSavedSettings();

// UI thread, once the window and the runtime exist.
void Init(rex::ui::Window* window, rex::ui::WindowedAppContext* app_context,
          rex::Runtime* runtime);
// UI thread, before the window goes away.
void Shutdown();

// Guest thread: pad 0 as the game just read it (hook on the game's
// XInputGetState). Returns true when the game must not see it (settings page
// open, or the buttons that closed it still held).
bool OnGuestPad(uint16_t buttons, int16_t thumb_lx, int16_t thumb_ly);

// Any thread: saves the settings and closes the game window.
void QuitGame();

// The game's ACHIEVEMENTS button (any thread): shows the achievements page;
// on_closed runs on the UI thread once the player closes it. Returns false
// when the overlay is not available (on_closed is then not called).
bool ShowAchievements(std::function<void()> on_closed);

// Guest thread: the "Vibration" setting.
bool VibrationEnabled();

// The settings as pages of the game's own OPTIONS panel (2026-10-12; see
// realsteel_native_settings.cpp). Page 0 is the game's own options; pages
// 1..PageCount() are ours, six rows each, built from the settings table.
// Guest thread.
namespace native {
struct RowView {
  std::string label;   // upper case, as the game's labels
  std::string value;   // text in the value column
  std::string help;    // one line for the help bar
  uint32_t count = 1;  // spinner values (1: left / right act through Press)
  uint32_t index = 0;  // current spinner value
  bool wrap = false;
  bool active = false;  // false: an empty row
};
int PageCount();
std::string PageTitle(int page);
// The game's OPTIONS panel, from its draw hook every frame: the page shown
// (0 = the game's own options) and whether it lists achievements. While it
// is up the overlay draws tabs above it.
void PanelShown(int page, bool achievements);
// A tab (or LB / RB) clicked / tapped on that strip: the page to open, or -1.
int TakePageRequest();
RowView Row(int page, int row);
// The spinner of |row| moved to |index|.
void Change(int page, int row, uint32_t index);
// Left (-1) / right (+1) on a row without values: edit a text, rebind a
// key, run an action.
void Press(int page, int row, int direction);
// RETURN TO DEFAULT on one of our pages.
void ResetPage(int page);
// The pad as the game last read it (buttons, left stick x).
uint16_t PadButtons();
int PadStickX();
// True while our text entry box is open (the game must not see the pad).
bool EditingText();
// The achievements as read-only pages of the same panel (main menu >
// ACHIEVEMENTS opens it in this mode).
int AchievementPageCount();
std::string AchievementPageTitle(int page);
RowView AchievementRow(int page, int row);
}  // namespace native

// Renderer thread, once per presented frame: hands the renderer the latest
// recorded overlay and schedules the next UI-thread tick.
void Pump(rs::vk::Renderer& renderer, double fps);

}  // namespace realsteel::overlay
