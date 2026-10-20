// realsteel - frame-rate modes (realsteel_hooks.cpp).
#pragma once

namespace realsteel::framerate {

// The display's refresh rate in Hz (0 = unknown, e.g. headless).
int DisplayRefreshHz();

// The frame rate the game runs at now: the realsteel_frame_rate setting,
// held at 60 during online play and kept at or below the display's refresh.
int Current();

}  // namespace realsteel::framerate
