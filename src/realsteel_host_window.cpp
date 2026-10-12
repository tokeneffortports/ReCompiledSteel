#include "realsteel_host_window.h"

#include "realsteel_native_present.h"

#include <rex/logging.h>
#include <rex/ui/window.h>

#ifndef _WIN32
#include <SDL3/SDL.h>

#include <filesystem>
#include <string>
#include <system_error>
#endif

namespace realsteel::host_window {

void Attach(rex::ui::Window* window) {
  if (window == nullptr) return;
  native_present::SetHostWindowSize(window->GetActualPhysicalWidth(),
                                    window->GetActualPhysicalHeight());
#ifdef _WIN32
  native_present::SetHostWindow(window->GetNativeWindowHandle());
#else
  // The SDK does not expose its SDL_Window; the game has exactly one.
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* sdl_window = windows != nullptr && count > 0 ? windows[0] : nullptr;
  if (sdl_window == nullptr) {
    REXLOG_INFO("[host-window] no SDL window found: {}", SDL_GetError());
    if (windows != nullptr) SDL_free(windows);
    return;
  }
  const SDL_PropertiesID props = SDL_GetWindowProperties(sdl_window);
#ifdef __ANDROID__
  {
    void* native_window =
        SDL_GetPointerProperty(props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
    int aw = 0, ah = 0;
    if (SDL_GetWindowSizeInPixels(sdl_window, &aw, &ah) && aw > 0 && ah > 0) {
      native_present::SetHostWindowSize(uint32_t(aw), uint32_t(ah));
    }
    REXLOG_INFO("[host-window] Android native window {} ({}x{} px)", native_window, aw, ah);
    native_present::SetHostWindowAndroid(native_window);
    // Suspend / resume: the renderer drops its surface in the background and
    // takes the new window SDL gets when the app returns.
    native_present::SetAndroidWindowSource([]() -> void* {
      int n = 0;
      SDL_Window** list = SDL_GetWindows(&n);
      void* current = nullptr;
      if (list != nullptr && n > 0) {
        current = SDL_GetPointerProperty(SDL_GetWindowProperties(list[0]),
                                         SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, nullptr);
      }
      if (list != nullptr) SDL_free(list);
      return current;
    });
    SDL_AddEventWatch(
        [](void*, SDL_Event* event) -> bool {
          if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND || event->type == SDL_EVENT_DID_ENTER_BACKGROUND) {
            native_present::SetAndroidBackground(true);
          } else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND) {
            native_present::SetAndroidBackground(false);
          }
          return true;
        },
        nullptr);
    SDL_free(windows);
    return;
  }
#endif
  void* wl_display =
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
  void* wl_surface =
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
  void* x11_display =
      SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
  const unsigned long x11_window = static_cast<unsigned long>(
      SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  int w = 0, h = 0;
  if (SDL_GetWindowSizeInPixels(sdl_window, &w, &h) && w > 0 && h > 0) {
    native_present::SetHostWindowSize(uint32_t(w), uint32_t(h));
  }
  REXLOG_INFO("[host-window] SDL video driver {}: wayland {} x11 {} ({}x{} px)",
              SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?",
              wl_surface != nullptr ? "yes" : "no",
              x11_window != 0 ? "yes" : "no", w, h);
  if (wl_display != nullptr && wl_surface != nullptr) {
    native_present::SetHostWindowLinux(nullptr, 0, wl_display, wl_surface);
  } else {
    native_present::SetHostWindowLinux(x11_display, x11_window, nullptr, nullptr);
  }
  SDL_free(windows);
#endif
}

void RemoveStaleGuestMemoryFiles() {
#if !defined(_WIN32) && !defined(__ANDROID__)  // Android has no /dev/shm
  std::error_code ec;
  uint32_t files = 0;
  for (const auto& entry : std::filesystem::directory_iterator("/dev/shm", ec)) {
    const std::string name = entry.path().filename().string();
    if (name.rfind("xenia_memory_", 0) != 0) continue;
    // Unlinking is harmless to a process that still has the file mapped.
    std::error_code file_ec;
    if (std::filesystem::remove(entry.path(), file_ec)) ++files;
  }
  if (files != 0) {
    REXLOG_INFO("[host-window] removed {} guest memory file(s) left in /dev/shm by "
                "earlier runs", files);
  }
#endif
}

}  // namespace realsteel::host_window
