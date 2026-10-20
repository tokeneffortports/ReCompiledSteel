// Real Steel in-process native renderer (2026-09-06).
//
// The pinned ReXGlue SDK has no guest-output handoff, so the native path
// presents in its own window: every completed FrameCapture is serialized (the
// same bytes a disk capture holds), parsed with the replayer's loader and
// rendered by the replayer's persistent Vulkan backend, with textures read
// straight from guest memory. Enabled with --realsteel_native_present=true;
// the xenos plugin keeps running alongside during bring-up.
#pragma once

namespace realsteel::native_present {
// Any thread: the next presented frame is written to screenshots/ as PNG.
void RequestScreenshot();
// The guest memory base once the game runs (nullptr before).
unsigned char* GuestBase();
}  // namespace realsteel::native_present

#include <cstddef>
#include <cstdint>
#include <vector>

namespace rs::capture {
struct FrameData;
}

namespace realsteel::native_present {

bool Enabled();
// True when the worker has consumed the last frame and could take a new
// one: the capture code only pays for per-draw snapshots, texture hashing
// and serialization on frames that will actually be rendered.
bool WorkerIdle();
void OnFrameBytes(uint8_t* base, std::vector<uint8_t>& bytes);
// Same, without the byte form (2026-10-12): the frame's structures as the
// renderer takes them. Swapped, not copied - |frame| comes back holding an
// earlier frame's storage (capacity kept), for the caller to refill.
void OnFrameData(uint8_t* base, rs::capture::FrameData& frame);
// True when the worker wants the byte form (frame dumps to disk).
bool WantsFrameBytes();

// Profiling (2026-09-15): guest-thread cost of the frame-end hook while the
// native path is active — reported with the worker's own timings every 120
// rendered frames in rex_latest.log.
// The game's own window (rex::ui::Window native handle). When set before the
// first frame, the renderer presents into it instead of opening a second
// window - used when the GPU plugin is off and nothing else draws there.
void SetHostWindow(void* native_handle);
// The same for Linux: X11 (display, window id) or Wayland (display, surface)
// handles of the game's window; unused pairs are null / 0.
// Android: the activity's ANativeWindow (from SDL).
void SetHostWindowAndroid(void* native_window);
// Android lifecycle (any thread): the app is in the background or not, and
// the window SDL currently has (SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER).
void SetAndroidBackground(bool background);
// Render thread: while true, the frame finished is kept back; false hands
// it to the renderer (see the frame cap in realsteel_hooks.cpp).
void HoldFrames(bool hold);
// The picture's shape in use (width / height; 0 = the game's own 16:9).
float CurrentPictureAspect();
// The game's main camera (vertical fov in radians, near, far) as last set
// up, for the renderer's own effects (ambient occlusion).
void NoteCamera(float fov_y, float near_z, float far_z);
// Where the game's central 16:9 band (the 1280x720 space that menus and
// panels keep) lies in a window of |win_w| x |win_h| pixels: x, y, w, h.
void BandRect(float win_w, float win_h, float rect[4]);
void SetAndroidWindowSource(void* (*current_window)());
void SetHostWindowLinux(void* x11_display, unsigned long x11_window,
                        void* wayland_display, void* wayland_surface);
// Pixel size of the game's window (UI thread, on creation and every resize).
void SetHostWindowSize(uint32_t width, uint32_t height);

void NoteGuestTiming(uint64_t frame_end_ns, uint64_t serialize_ns,
                     uint64_t draw_hooks_ns);

}  // namespace realsteel::native_present
