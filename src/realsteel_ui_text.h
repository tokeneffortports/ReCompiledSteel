#pragma once
// The game's own UI text by string id (2026-10-12): every label, value, help
// line and title the game draws comes from its string manager
// (sub_82083638: id -> text). Menus look labels and titles up once when a
// screen opens; ON/OFF values every frame. Overrides set here are returned instead, so
// our settings can appear in the game's own menus.

#include <cstdint>
#include <string>

namespace realsteel::ui_text {

// Text for |id| from now on (any thread). Empty |text| is a valid override.
void Set(uint32_t id, const std::string& text);
// Back to the game's own text.
void Clear(uint32_t id);
// Ids [first, first + count) always come from our slots (the game's own text
// copied in when there is no override), so a screen that looked them up
// before any override existed still follows later Set calls.
void Pin(uint32_t first, uint32_t count);

}  // namespace realsteel::ui_text
