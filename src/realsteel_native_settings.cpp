// realsteel - our settings inside the game's own OPTIONS panel (2026-10-12).
//
// HELP AND OPTIONS > GAME OPTIONS opens the game's panel: a title, six rows
// of "LABEL  < value >", a help line in the bottom bar. Its first page is the
// game's own options as before; LB / RB page through ours (the settings
// table of realsteel_overlay.cpp, six rows a page). Texts come from the
// game's string manager, which realsteel_ui_text.cpp can override: labels
// and title are fetched once when the panel opens, but the panel keeps the
// pointer it got - into our override slots - so rewriting a slot changes
// the text on screen. Values are the rows' own text buffers.
//
// The panel (vtable 0x82009EF8, constructor sub_82169170), offsets from its
// base B:
//   +56   cursor row
//   +72   spinner value, +76 value count, +80 wraps, +84 1
//   +616  six row records of 80 bytes: char text[64]; +64 text id (-1:
//         the text); +68 value; +72 "0 shows OFF"; +76 row active
//   +1096 the game's options (76 bytes; the rows' bytes at +1143.. +1171)
// Methods hooked here: sub_82168F60 draw (every frame), sub_821680F8 sets the
// spinner for the cursor row, sub_82168458 fills the rows from the options,
// sub_82168B00 (on B+28) a value changed, sub_8216A758 applies the options.
// Not hooked, called: sub_82168568 arrows, sub_821681D8 help line.
// On our pages the game's options never change: a value change is taken
// from the spinner and the options block is put back; RETURN TO DEFAULT
// shows up as a changed block and resets our page instead.

#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>

#include <rex/logging.h>

#include "realsteel_overlay.h"
#include "realsteel_ui_text.h"

#include "generated/default_final/realsteel_init.h"

namespace {

constexpr uint32_t kCursor = 56;
constexpr uint32_t kSpinValue = 72, kSpinCount = 76, kSpinWrap = 80, kSpinSet = 84;
constexpr uint32_t kRowBase = 616, kRowSize = 80, kRows = 6;
constexpr uint32_t kOptions = 1096, kOptionsSize = 76;
constexpr uint32_t kLabelIds = 20050, kHelpIds = 20057, kTitleId = 20097;
constexpr uint32_t kLegendId = 25122;  // "RETURN TO DEFAULT" in the bottom bar
constexpr uint32_t kStringManager = 0x8247C878;
constexpr uint32_t kXGlyphId = 25003;  // the X button glyph

namespace native = realsteel::overlay::native;

uint32_t g_panel = 0;  // guest address of the open panel
int g_page = 0;        // 0: the game's own options
std::map<uint32_t, std::string> g_original;  // the game's texts, at open
uint8_t g_saved_options[kOptionsSize];
uint16_t g_prev_buttons = 0;
int g_prev_stick = 0;
std::chrono::steady_clock::time_point g_last_draw{};
// Main menu > ACHIEVEMENTS: the panel shows the achievements instead
// (opened through HELP AND OPTIONS > GAME OPTIONS, see the hooks below).
bool g_achievements_mode = false;
bool g_achievements_pending = false;
uint32_t g_main_menu = 0;

int Pages() { return g_achievements_mode ? native::AchievementPageCount() : native::PageCount(); }
std::string Title(int page) {
  return g_achievements_mode ? native::AchievementPageTitle(page) : native::PageTitle(page);
}
native::RowView RowOf(int page, int row) {
  return g_achievements_mode ? native::AchievementRow(page, row) : native::Row(page, row);
}
int g_spinner_row = -1;  // the row the spinner was last set up for

// Calls a guest function with r3 / r4 and gives the caller's registers back.
void CallGuest(PPCContext& ctx, uint8_t* base, void (*fn)(PPCContext&, uint8_t*), uint32_t r3,
               uint32_t r4 = 0) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  fn(ctx, base);
  ctx = saved;
}

// The game's own text for |id| (the lookup without our overrides).
std::string OriginalText(PPCContext& ctx, uint8_t* base, uint32_t id) {
  const PPCContext saved = ctx;
  ctx.r3.u64 = REX_LOAD_U32(kStringManager);
  ctx.r4.u64 = id;
  __imp__sub_82083638(ctx, base);
  const uint32_t text = ctx.r3.u32;
  ctx = saved;
  return text != 0 ? std::string(reinterpret_cast<const char*>(base + text)) : std::string();
}

void ApplyTexts(int page) {
  for (uint32_t r = 0; r < kRows; ++r) {
    if (page == 0) {
      realsteel::ui_text::Set(kLabelIds + r, g_original[kLabelIds + r]);
      realsteel::ui_text::Set(kHelpIds + r, g_original[kHelpIds + r]);
    } else {
      const native::RowView v = RowOf(page, int(r));
      realsteel::ui_text::Set(kLabelIds + r, v.label);
      realsteel::ui_text::Set(kHelpIds + r, v.help);
    }
  }
  realsteel::ui_text::Set(kTitleId, page == 0 ? g_original[kTitleId] : Title(page));
}

void FillRows(uint8_t* base, int page) {
  for (uint32_t r = 0; r < kRows; ++r) {
    const uint32_t row = g_panel + kRowBase + r * kRowSize;
    const native::RowView v = RowOf(page, int(r));
    std::memset(base + row, 0, 64);
    std::memcpy(base + row, v.value.data(), std::min<size_t>(v.value.size(), 63));
    REX_STORE_U32(row + 64, 0xFFFFFFFFu);
    REX_STORE_U32(row + 68, v.index);
    REX_STORE_U32(row + 72, 0u);
    REX_STORE_U32(row + 76, v.active ? 1u : 0u);
  }
}

void SetSpinner(uint8_t* base, int page) {
  const int row = int(REX_LOAD_U32(g_panel + kCursor));
  g_spinner_row = row;
  const native::RowView v = RowOf(page, row);
  REX_STORE_U32(g_panel + kSpinValue, v.index);
  REX_STORE_U32(g_panel + kSpinCount, std::max<uint32_t>(1, v.count));
  REX_STORE_U32(g_panel + kSpinWrap, v.wrap ? 1u : 0u);
  REX_STORE_U32(g_panel + kSpinSet, 1u);
}

void SwitchPage(PPCContext& ctx, uint8_t* base, int page) {
  if (g_page == 0 && page != 0) std::memcpy(g_saved_options, base + g_panel + kOptions, kOptionsSize);
  g_page = page;
  ApplyTexts(page);
  if (page != 0) {
    FillRows(base, page);
    SetSpinner(base, page);
  } else {
    for (uint32_t r = 0; r < kRows; ++r) {
      REX_STORE_U32(g_panel + kRowBase + r * kRowSize + 72, 1u);
      REX_STORE_U32(g_panel + kRowBase + r * kRowSize + 76, 1u);
    }
    CallGuest(ctx, base, __imp__sub_82168458, g_panel);
    CallGuest(ctx, base, __imp__sub_821680F8, g_panel);
  }
  CallGuest(ctx, base, sub_82168568, g_panel);
  CallGuest(ctx, base, sub_821681D8, g_panel, 1);
  REXLOG_INFO("[settings] options panel page {}: {}", page, page == 0 ? std::string("game") : Title(page));
}

bool OurPage(uint32_t panel) { return panel == g_panel && g_panel != 0 && g_page != 0; }

}  // namespace

// The panel's constructor (this r3). Texts are looked up while it builds the
// panel: overrides set first make the panel point at our slots.
extern "C" REX_FUNC(sub_82169170) {
  const uint32_t panel = ctx.r3.u32;
  for (uint32_t r = 0; r < kRows; ++r) {
    g_original[kLabelIds + r] = OriginalText(ctx, base, kLabelIds + r);
    g_original[kHelpIds + r] = OriginalText(ctx, base, kHelpIds + r);
  }
  g_original[kTitleId] = OriginalText(ctx, base, kTitleId);
  g_original[kLegendId] = OriginalText(ctx, base, kLegendId);
  g_achievements_mode = g_achievements_pending;
  g_achievements_pending = false;
  // Nothing to reset among achievements: no X glyph in the bottom bar (it
  // comes back when the panel closes, see sub_82160B58).
  if (g_achievements_mode) realsteel::ui_text::Set(kXGlyphId, "");
  g_page = 0;
  ApplyTexts(0);
  // The bottom bar: "X RETURN TO DEFAULT" plus the LB / RB glyphs.
  realsteel::ui_text::Set(kLegendId, g_achievements_mode
                                         ? std::string("\xee\x80\x8b\xee\x80\x8e PAGES")
                                         : g_original[kLegendId] + "     \xee\x80\x8b\xee\x80\x8e PAGES");
  __imp__sub_82169170(ctx, base);
  g_panel = panel;
  if (g_achievements_mode) SwitchPage(ctx, base, 1);
  g_prev_buttons = 0xFFFF;  // the press that opened the panel is not an edge
  g_prev_stick = 0;
  REXLOG_INFO("[settings] options panel opened ({}, {} pages)",
              g_achievements_mode ? "achievements" : "settings", Pages());
}

// Every frame while the panel is up: page buttons, left / right on rows
// without values, RETURN TO DEFAULT, live values.
extern "C" REX_FUNC(sub_82168F60) {
  const uint32_t panel = ctx.r3.u32;
  if (panel == g_panel && g_panel != 0) {
    const auto now = std::chrono::steady_clock::now();
    if (now - g_last_draw > std::chrono::seconds(1)) g_prev_buttons = 0xFFFF;  // reopened
    g_last_draw = now;
    const uint16_t buttons = native::EditingText() ? 0 : native::PadButtons();
    const uint16_t pressed = uint16_t(buttons & ~g_prev_buttons);
    g_prev_buttons = buttons;
    if (g_achievements_mode) {
      const int pages = Pages();
      if (pressed & 0x0100) SwitchPage(ctx, base, (g_page + pages - 2) % pages + 1);  // LB
      if (pressed & 0x0200) SwitchPage(ctx, base, g_page % pages + 1);                // RB
    } else {
      const int pages = Pages() + 1;
      if (pressed & 0x0100) SwitchPage(ctx, base, (g_page + pages - 1) % pages);  // LB
      if (pressed & 0x0200) SwitchPage(ctx, base, (g_page + 1) % pages);          // RB
      // A tab clicked / tapped on the overlay's strip above the panel.
      const int request = native::TakePageRequest();
      if (request >= 0 && request < pages && request != g_page) SwitchPage(ctx, base, request);
    }
    native::PanelShown(g_page, g_achievements_mode);
    if (g_page != 0) {
      if (std::memcmp(g_saved_options, base + g_panel + kOptions, kOptionsSize) != 0) {
        // The game changed its options on our page: RETURN TO DEFAULT.
        std::memcpy(base + g_panel + kOptions, g_saved_options, kOptionsSize);
        native::ResetPage(g_page);
        SetSpinner(base, g_page);
        CallGuest(ctx, base, sub_82168568, g_panel);
      }
      if ((pressed & 0x4000) && !g_achievements_mode) {  // X: RETURN TO DEFAULT for this page
        native::ResetPage(g_page);
        SetSpinner(base, g_page);
        CallGuest(ctx, base, sub_82168568, g_panel);
        REXLOG_INFO("[settings] page {} back to defaults", native::PageTitle(g_page));
      }
      const int x = native::EditingText() ? 0 : native::PadStickX();
      const int stick = x > 20000 ? 1 : x < -20000 ? -1 : 0;
      int direction = (pressed & 0x0004) ? -1 : (pressed & 0x0008) ? 1 : 0;
      if (stick != 0 && stick != g_prev_stick) direction = stick;
      g_prev_stick = stick;
      const int row = int(REX_LOAD_U32(g_panel + kCursor));
      if (direction != 0 && !g_achievements_mode && RowOf(g_page, row).count <= 1) {
        native::Press(g_page, row, direction);
      }
      FillRows(base, g_page);
    }
  }
  __imp__sub_82168F60(ctx, base);
}

// The spinner for the cursor row.
extern "C" REX_FUNC(sub_821680F8) {
  if (OurPage(ctx.r3.u32)) {
    SetSpinner(base, g_page);
    return;
  }
  __imp__sub_821680F8(ctx, base);
}

// The rows' values from the options.
extern "C" REX_FUNC(sub_82168458) {
  if (OurPage(ctx.r3.u32)) {
    FillRows(base, g_page);
    return;
  }
  __imp__sub_82168458(ctx, base);
}

// A value changed (this = panel + 28): ours takes the spinner's value; the
// game's options block is put back as it was.
extern "C" REX_FUNC(sub_82168B00) {
  const uint32_t panel = ctx.r3.u32 - 28;
  if (!OurPage(panel)) {
    __imp__sub_82168B00(ctx, base);
    return;
  }
  // The spinner has moved by the time this runs (it also runs when the
  // cursor moves): a value that differs from the setting is a change.
  const int row = int(REX_LOAD_U32(panel + kCursor));
  const uint32_t value = REX_LOAD_U32(panel + kSpinValue);
  if (row == g_spinner_row && !g_achievements_mode) {
    const native::RowView v = RowOf(g_page, row);
    if (v.count > 1 && value != v.index) native::Change(g_page, row, value);
  }
  uint8_t options[kOptionsSize];
  std::memcpy(options, base + panel + kOptions, kOptionsSize);
  __imp__sub_82168B00(ctx, base);
  std::memcpy(base + panel + kOptions, options, kOptionsSize);
  FillRows(base, g_page);
  ApplyTexts(g_page);  // a change can move other rows (HDR off: no peak)
}

// Applying the options (volumes, brightness): nothing to apply on our pages.
extern "C" REX_FUNC(sub_8216A758) {
  if (OurPage(ctx.r3.u32)) return;
  __imp__sub_8216A758(ctx, base);
}

// Main menu: an item was chosen (this = menu + 28, cursor at menu + 56).
// ACHIEVEMENTS (4) asked the console's own achievements screen; it now
// goes the way of HELP AND OPTIONS (5), whose menu then opens its panel in
// achievements mode at once (sub_82161D88 below).
extern "C" REX_FUNC(sub_8215B7B0) {
  const uint32_t menu = ctx.r3.u32 - 28;
  if (REX_LOAD_U32(menu + 56) == 4) {
    REX_STORE_U32(menu + 56, 5u);
    g_achievements_pending = true;
    g_main_menu = menu;  // its cursor goes back to 4 once the help menu is up
    __imp__sub_8215B7B0(ctx, base);
    REXLOG_INFO("[achievements] main menu: opening the achievements panel");
    return;
  }
  __imp__sub_8215B7B0(ctx, base);
}

// HELP AND OPTIONS, every frame (vtable 0x820098F0 slot 21; this = menu).
// With achievements pending, it opens GAME OPTIONS (row 2) once the menu has
// settled for a few frames, as if the player had chosen it.
extern "C" REX_FUNC(sub_82160B58) {
  const uint32_t menu = ctx.r3.u32;
  __imp__sub_82160B58(ctx, base);
  if (g_achievements_mode && std::chrono::steady_clock::now() - g_last_draw > std::chrono::milliseconds(300)) {
    // The achievements panel has closed: back to settings for next time.
    g_achievements_mode = false;
    realsteel::ui_text::Clear(kXGlyphId);
  }
  static int frames = 0;
  if (!g_achievements_pending) {
    frames = 0;
    return;
  }
  if (g_main_menu != 0) REX_STORE_U32(g_main_menu + 56, 4u);
  g_main_menu = 0;
  if (++frames < 12) return;
  frames = 0;
  REX_STORE_U32(menu + 56, 2u);
  const PPCContext saved = ctx;
  ctx.r3.u64 = menu + 28;
  sub_82161EA0(ctx, base);
  ctx = saved;
}
