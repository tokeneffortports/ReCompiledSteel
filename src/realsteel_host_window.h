// realsteel - hands the game's window to the native renderer (2026-10-02).
//
// With the built-in GPU service nothing draws into the SDK window, so the
// native renderer presents there. Windows passes the HWND; on Linux (Steam
// Deck) the SDK window is SDL's, and the renderer needs its X11 or Wayland
// handles.

#pragma once

namespace rex::ui {
class Window;
}

namespace realsteel::host_window {

// UI thread, once the window is open.
void Attach(rex::ui::Window* window);

// Linux: the runtime backs guest memory with /dev/shm/xenia_memory_* and its
// hard exit never unlinks it, so every run left its whole memory image in
// RAM-backed tmpfs until reboot (2026-10-03, Steam Deck: start-up went from
// 1 s to minutes as RAM filled). Removes the files of earlier runs; call
// before the runtime creates this run's. No-op elsewhere.
void RemoveStaleGuestMemoryFiles();

}  // namespace realsteel::host_window
