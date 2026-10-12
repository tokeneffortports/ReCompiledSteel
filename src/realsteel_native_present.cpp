#include "realsteel_native_present.h"

#include "realsteel_overlay.h"
#include "realsteel_shader_dump.h"
#include "realsteel_texture_dump.h"

#include "capture_format.h"
#include "texture_decode.h"
#include "vk_backend.h"

#ifdef __ANDROID__
#include <android/native_window.h>
#endif

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <atomic>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <deque>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(realsteel_native_present, true, "Real Steel",
                    "Render every frame with the in-process native Vulkan "
                    "renderer (false: development only, with the GPU plugin)");
REXCVAR_DEFINE_INT32(realsteel_native_dump_every, 0, "Real Steel",
                     "Every N rendered frames write the native renderer's "
                     "output (rs_native/native_<n>.png) and the frame bytes it "
                     "rendered (rs_native/native_<n>.bin) for offline comparison");

// 2026-10-12, for trailers: a file named record.trigger (first line: the
// number of frames, default 600; second line: a name, {out} below) sends
// that many rendered frames, raw RGBA, to this command's standard input,
// e.g. wsl -e ffmpeg -f rawvideo -pix_fmt rgba -s {w}x{h} -r 60 -i - out.mp4.
// The game waits for the renderer before handing over each frame, so a slow
// encoder slows the game down instead of dropping frames: the video is
// smooth at 60 fps whatever the speed it was recorded at.
REXCVAR_DEFINE_BOOL(realsteel_capture_full, false, "Real Steel",
                    "Development: test pictures (dump.trigger) and recordings at "
                    "the internal resolution, tone curve applied");
REXCVAR_DEFINE_STRING(realsteel_record_pipe, "", "Real Steel",
                      "Development: command that receives recorded frames "
                      "(raw RGBA on stdin; {w} {h} {out} are filled in)");

REXCVAR_DEFINE_DOUBLE(realsteel_native_gamma, 1.28, "Real Steel",
                      "Output tone curve of the native window (RGB^gamma). "
                      "1.28 matches the xenos presenter; 1.0 = raw output");

REXCVAR_DEFINE_INT32(realsteel_native_scale, 1, "Real Steel",
                     "Internal resolution multiplier of the native renderer "
                     "(1 = 1280x720, 2 = 2560x1440, 3 = 3840x2160)");
REXCVAR_DEFINE_STRING(realsteel_native_resolution, "1280x720", "Real Steel",
                      "Internal resolution of the native renderer as WxH. Need "
                      "not be 16:9 (1280x800 = Steam Deck panel, 1280x960 = "
                      "Retroid Pocket Nova panel: use with stretch). At the "
                      "default, realsteel_native_scale applies");
REXCVAR_DEFINE_STRING(realsteel_native_aspect, "auto", "Real Steel",
                      "Shape of the picture: auto (the window's own shape), "
                      "16:9 (original), 16:10, 4:3 or any W:H. The scene is "
                      "re-proportioned, not stretched: narrower shapes show "
                      "more above and below");
REXCVAR_DEFINE_BOOL(realsteel_native_aspect_mask_2d, false, "Real Steel",
                    "Picture shapes other than 16:9: show screens without 3D "
                    "(title, loading) as their 16:9 band only");
REXCVAR_DEFINE_STRING(realsteel_native_vsync, "auto", "Real Steel",
                      "Present with vertical sync: on, off, or auto (on under "
                      "Windows; off under Linux, where the compositor / "
                      "gamescope already prevents tearing)");
REXCVAR_DEFINE_BOOL(realsteel_native_msaa, true, "Real Steel",
                    "Use the game's own multisample anti-aliasing");
REXCVAR_DEFINE_INT32(realsteel_native_anisotropy, 8, "Real Steel",
                     "Anisotropic texture filtering level (1 = off, 2-16)");
REXCVAR_DEFINE_DOUBLE(realsteel_native_sharpen, 0.0, "Real Steel",
                      "Sharpening of the presented picture, 0 (off) to 1");
REXCVAR_DEFINE_BOOL(realsteel_native_fxaa, false, "Real Steel",
                    "FXAA edge smoothing on the presented picture");
// 2026-10-12: one setting for both (the settings page's Anti-aliasing row);
// empty falls back to realsteel_native_msaa / realsteel_native_fxaa, which
// older settings files and command lines use.
REXCVAR_DEFINE_STRING(realsteel_native_aa, "", "Real Steel",
                      "Anti-aliasing: msaa (the game's own), msaa_fxaa, fxaa or off");
REXCVAR_DEFINE_BOOL(realsteel_native_hdr, false, "Real Steel",
                    "HDR output (HDR10 or scRGB) where the display and system "
                    "have HDR switched on; the picture is expanded from SDR");
REXCVAR_DEFINE_DOUBLE(realsteel_native_hdr_peak, 600.0, "Real Steel",
                      "HDR: brightness of the brightest highlights, in nits");
REXCVAR_DEFINE_DOUBLE(realsteel_native_hdr_paper, 200.0, "Real Steel",
                      "HDR: brightness of ordinary white (menus, the arena), in nits");
REXCVAR_DEFINE_BOOL(realsteel_native_headless, false, "Real Steel",
                    "Render without presenting to a window (unattended tests)");
REXCVAR_DEFINE_BOOL(realsteel_native_stretch, false, "Real Steel",
                    "Fill the window instead of keeping 16:9 with black bars");
REXCVAR_DEFINE_BOOL(realsteel_native_dof, false, "Real Steel",
                    "Apply the game's depth-of-field blend in the native "
                    "renderer (off matches the xenos plugin's picture)");

namespace realsteel::native_present {
namespace {

// Textures for the live path come straight from guest memory; the backend
// caches uploads by (id, content hash) so this runs once per new content.
class GuestTextureSource : public rs::vk::TextureSource {
 public:
  uint8_t* base = nullptr;
  bool Fetch(uint32_t phys, uint64_t /*hash*/,
             rs::capture::DecodedTexture& out) override {
    uint32_t fetch[6];
    std::vector<uint8_t> data;
    if (base == nullptr ||
        !realsteel::texture_dump::ReadTextureBytes(base, phys, fetch, data)) {
      return false;
    }
    char label[32];
    std::snprintf(label, sizeof(label), "guest %08X", phys);
    const size_t raw_bytes = data.size();
    const bool ok =
        rs::capture::DecodeGuestTexture(label, fetch, std::move(data), out);
    // Diagnostics (rs_native_print.log): what the live path decoded, with an
    // RGBA checksum + alpha range to compare against the offline dumps.
    uint64_t sum = 1469598103934665603ull;
    uint32_t amin = 255, amax = 0;
    for (size_t i = 0; ok && i < out.rgba.size(); i += 4) {
      uint32_t px;
      std::memcpy(&px, out.rgba.data() + i, 4);
      sum = (sum ^ px) * 1099511628211ull;
      const uint32_t a = out.rgba[i + 3];
      amin = a < amin ? a : amin;
      amax = a > amax ? a : amax;
    }
    std::printf("[live-tex] %08X fetch=%08X %08X %08X %08X %08X %08X raw=%zu "
                "-> %s %ux%u layers=%u rgba=%zu sum=%016llX alpha=%u..%u\n",
                phys, fetch[0], fetch[1], fetch[2], fetch[3], fetch[4],
                fetch[5], raw_bytes, ok ? "ok" : "FAIL", out.width, out.height,
                out.layers, out.rgba.size(),
                static_cast<unsigned long long>(sum), amin, amax);
    return ok;
  }
};

// Internal resolution from the cvars (WxH wins; the integer scale is the
// command-line shorthand).
void InternalResolution(uint32_t& w, uint32_t& h) {
  const uint32_t scale = uint32_t(std::clamp(REXCVAR_GET(realsteel_native_scale), 1, 4));
  w = 1280 * scale;
  h = 720 * scale;
  unsigned pw = 0, ph = 0;
  const std::string text = REXCVAR_GET(realsteel_native_resolution);
  if (std::sscanf(text.c_str(), "%ux%u", &pw, &ph) == 2 && pw != 0 && ph != 0 &&
      !(pw == 1280 && ph == 720)) {
    w = pw;
    h = ph;
  }
}

struct AaMode {
  bool msaa, fxaa;
};
AaMode AntiAliasing() {
  const std::string aa = REXCVAR_GET(realsteel_native_aa);
  if (aa == "msaa") return {true, false};
  if (aa == "msaa_fxaa") return {true, true};
  if (aa == "fxaa") return {false, true};
  if (aa == "off") return {false, false};
  return {REXCVAR_GET(realsteel_native_msaa), REXCVAR_GET(realsteel_native_fxaa)};
}

bool WantVsync() {
  const std::string text = REXCVAR_GET(realsteel_native_vsync);
  if (text == "on") return true;
  if (text == "off") return false;
#ifdef _WIN32
  return true;
#else
  return false;
#endif
}

// Picture shape from realsteel_native_aspect: 0 = the guest's own 16:9.
float PictureAspect(float window_aspect) {
  const std::string text = REXCVAR_GET(realsteel_native_aspect);
  float aspect = 0.0f;
  unsigned aw = 0, ah = 0;
  if (text == "auto") {
    aspect = window_aspect;
  } else if (std::sscanf(text.c_str(), "%u:%u", &aw, &ah) == 2 && aw != 0 && ah != 0) {
    aspect = float(aw) / float(ah);
  }
  if (aspect <= 0.0f) return 0.0f;
  aspect = std::clamp(aspect, 1.25f, 2.40f);
  const float rel = aspect / (16.0f / 9.0f);
  return rel > 0.99f && rel < 1.01f ? 0.0f : aspect;
}

// The resolution setting names a 16:9 size; for another picture shape the
// internal frame takes that shape at the same width (narrower shapes) or
// height (wider ones), so "720p" on a 1280x800 panel renders 1280x800.
void ShapeInternalResolution(float aspect, uint32_t& w, uint32_t& h) {
  if (aspect <= 0.0f) return;
  if (aspect < 16.0f / 9.0f) {
    h = (uint32_t(float(w) / aspect + 0.5f) + 1u) & ~1u;
  } else {
    w = (uint32_t(float(h) * aspect + 0.5f) + 1u) & ~1u;
  }
}

// Handoff: the guest thread drops the newest serialized frame here (2026-09-06:
// the first cut rendered synchronously inside the frame-end hook, under the
// capture mutex — every draw hook on other threads stalled for the whole
// render and loading never progressed). The worker takes the latest frame
// and renders it; frames arriving while it is busy are replaced, never
// queued, so the game's own pacing is untouched.
std::mutex g_handoff_mutex;
std::condition_variable g_handoff_cv;
std::condition_variable g_consumed_cv;  // worker took the pending frame
// One handed-over frame: the byte form (disk-compatible, parsed by the
// worker) or the structures themselves (no serialize / parse).
struct Handoff {
  std::vector<uint8_t> bytes;
  rs::capture::FrameData data;
  bool has_data = false;
  bool empty() const { return bytes.empty() && !has_data; }
  void clear() {
    bytes.clear();
    has_data = false;
  }
  void swap(Handoff& o) {
    bytes.swap(o.bytes);
    std::swap(data, o.data);
    std::swap(has_data, o.has_data);
  }
};
Handoff g_pending;   // newest frame, or empty
uint64_t g_pending_seq = 0;
uint64_t g_dropped = 0;
uint8_t* g_base = nullptr;
std::atomic<void*> g_host_window{nullptr};
std::atomic<void*> g_host_x11_display{nullptr};
std::atomic<unsigned long> g_host_x11_window{0};
std::atomic<void*> g_host_wl_display{nullptr};
std::atomic<void*> g_host_wl_surface{nullptr};
std::atomic<void*> g_host_android_window{nullptr};
// Android suspend / resume (2026-10-12: the picture stayed black after the
// app came back - the old surface was gone). See AndroidWindowReady.
std::atomic<bool> g_android_background{false};
std::atomic<float> g_picture_aspect{0.0f};  // for the guest-side culling hook
std::atomic<void* (*)()> g_android_window_source{nullptr};

#ifdef __ANDROID__
// True when the renderer has a window to present to. The ANativeWindow in
// use is held with our own reference: SDL releases its one when the surface
// is destroyed but leaves the stale pointer in its window property, and
// while we hold the old object a new window can never take its address -
// so a different pointer in the property means a new, live window.
bool AndroidWindowReady(rs::vk::Renderer& renderer) {
  static ANativeWindow* held = nullptr;
  static bool has_surface = true;
  if (held == nullptr && has_surface) {
    held = static_cast<ANativeWindow*>(g_host_android_window.load());
    if (held != nullptr) ANativeWindow_acquire(held);
  }
  if (g_android_background.load() || renderer.SurfaceLost()) {
    if (has_surface) {
      renderer.ReplaceAndroidWindow(nullptr);
      has_surface = false;
      REXLOG_INFO("[native-present] app in the background: window dropped");
    }
    return false;
  }
  if (has_surface) return true;
  auto* source = g_android_window_source.load();
  ANativeWindow* current = source != nullptr ? static_cast<ANativeWindow*>(source()) : nullptr;
  if (current == nullptr || current == held) return false;  // not created yet
  ANativeWindow_acquire(current);
  if (held != nullptr) ANativeWindow_release(held);
  held = current;
  renderer.ReplaceAndroidWindow(current);
  has_surface = true;
  REXLOG_INFO("[native-present] app back in the foreground: new window {}", static_cast<void*>(current));
  return true;
}
#endif
std::atomic<uint32_t> g_host_width{0}, g_host_height{0};
void HostWindowSize(uint32_t* width, uint32_t* height) {
  *width = g_host_width.load();
  *height = g_host_height.load();
}
std::atomic<bool> g_failed{false};
std::atomic<bool> g_worker_started{false};
std::atomic<uint64_t> g_guest_frame_end_ns{0};
std::atomic<uint64_t> g_guest_serialize_ns{0};
std::atomic<uint64_t> g_guest_draw_hooks_ns{0};
std::atomic<uint64_t> g_guest_timed_frames{0};

std::atomic<bool> g_screenshot_requested{false};

void WorkerMain() {
  // The backend reports through printf (shader misses, skipped draws, Vulkan
  // errors); the game has no console, so send stdout to a file.
  // (setvbuf with a zero-size line buffer is an invalid parameter on the
  // MSVC runtime and terminated the process at the first frame — 23:22 run.)
  if (std::freopen("rs_native_print.log", "w", stdout) != nullptr) {
    // Fully buffered (64 KB) and flushed with every 120-frame report; an
    // unbuffered log made every backend line a write syscall.
    std::setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
  }
  GuestTextureSource source;
  source.base = g_base;
  rs::vk::RenderOptions options;
  options.window = true;
  options.window_background = true;  // the game window keeps controller focus
  options.external_window = g_host_window.load();
  options.x11_display = g_host_x11_display.load();
  options.x11_window = g_host_x11_window.load();
  options.wayland_display = g_host_wl_display.load();
  options.wayland_surface = g_host_wl_surface.load();
  options.android_window = g_host_android_window.load();
  options.window_size = &HostWindowSize;
  if (options.HasHostWindow()) {
    REXLOG_INFO("[native-present] presenting into the game window");
  }
  if (REXCVAR_GET(realsteel_native_headless)) {
    // Unattended test runs: render without a swapchain; frame dumps
    // (realsteel_native_dump_every) still work.
    options.window = false;
    REXLOG_INFO("[native-present] headless: rendering without a window");
  }
  options.quiet = true;
  InternalResolution(options.internal_width, options.internal_height);
  options.picture_aspect = PictureAspect(0.0f);
  options.vsync = WantVsync();
  options.no_msaa = !AntiAliasing().msaa;
  options.anisotropy = uint32_t(std::clamp(REXCVAR_GET(realsteel_native_anisotropy), 1, 16));
  options.sharpen = static_cast<float>(REXCVAR_GET(realsteel_native_sharpen));
  ShapeInternalResolution(options.picture_aspect, options.internal_width,
                          options.internal_height);
  options.present_stretch = REXCVAR_GET(realsteel_native_stretch);
  options.readback_picture_shape = true;  // screenshots as presented
  options.aspect_mask_2d = REXCVAR_GET(realsteel_native_aspect_mask_2d);
  options.no_dof = !REXCVAR_GET(realsteel_native_dof);
  options.output_gamma = static_cast<float>(REXCVAR_GET(realsteel_native_gamma));
  options.texture_source = &source;
  options.shader_lookup = &realsteel::shader_dump::LookupUcode;
  options.container_source = &realsteel::shader_dump::ContainerBytes;
  options.shaders_dir = "rs_shaders";
  options.pipeline_cache_path = "rs_pipeline_cache.bin";
  options.textures_dir = "rs_textures";
  options.use_cull = true;
  auto renderer = std::make_unique<rs::vk::Renderer>();
  if (!renderer->Init(options)) {
    std::fflush(stdout);  // the backend's reason is in rs_native_print.log
    REXLOG_INFO("[native-present] renderer init FAILED — disabled (reason: rs_native_print.log)");
    g_failed.store(true);
    return;
  }
#ifdef _WIN32
  {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode)) {
      REXLOG_INFO("[pacing] display {}x{} at {} Hz", mode.dmPelsWidth, mode.dmPelsHeight,
                  mode.dmDisplayFrequency);
    }
  }
#endif
  REXLOG_INFO("[native-present] renderer up on its own thread; presenting in "
              "the native window");
  const auto t0 = std::chrono::steady_clock::now();
  auto t_report = t0;
  auto t_last_frame = t0;
  double fps_smooth = 60.0;
  uint64_t frames = 0;
  uint64_t parse_ns = 0, render_ns = 0, upload_ns = 0;
  Handoff in;
  std::vector<uint8_t>& bytes = in.bytes;
  uint64_t last_seq = 0;
  while (!g_failed.load()) {
    {
      std::unique_lock<std::mutex> lock(g_handoff_mutex);
      g_handoff_cv.wait_for(lock, std::chrono::milliseconds(50),
                            [&] { return g_pending_seq != last_seq; });
      if (g_pending_seq == last_seq) {
        // Nothing new: keep the window responsive (message pump lives in
        // Render, so re-render the last frame — cheap enough at menu rates).
        if (in.empty()) continue;
      } else {
        in.swap(g_pending);
        g_pending.clear();
        last_seq = g_pending_seq;
        g_consumed_cv.notify_all();
      }
    }
    static rs::capture::FrameData parsed;
    std::string error;
    const auto t_parse = std::chrono::steady_clock::now();
    if (!in.has_data) {
      parsed = rs::capture::FrameData();
      if (!rs::capture::LoadFrameFromMemory(bytes.data(), bytes.size(), "live",
                                            parsed, error)) {
        REXLOG_INFO("[native-present] frame parse failed: {}", error);
        continue;
      }
    }
    rs::capture::FrameData& frame = in.has_data ? in.data : parsed;
    const auto t_render = std::chrono::steady_clock::now();
    parse_ns += uint64_t((t_render - t_parse).count());
    std::vector<uint8_t> rgba;
    const int32_t dump_every = REXCVAR_GET(realsteel_native_dump_every);
    // Testing: a file named dump.trigger asks for one picture of the current
    // frame as rs_native/live.png (the file is removed when it is taken).
    // Headless runs render nothing between pictures (software Vulkan in WSL
    // dropped the game to 9 fps in 3D menus, and inputs timed by the clock
    // went astray): a request renders 4 frames in a row so frame-to-frame
    // state (feedback surfaces) has settled, and takes the last.
    static const bool headless = REXCVAR_GET(realsteel_native_headless);
    static int shot_countdown = 0;
    static int64_t record_left = 0;   // frames still to record (record.trigger)
    static std::string record_out;
    static FILE* record_pipe = nullptr;
    static uint32_t record_w = 0, record_h = 0;
    static bool shot_to_screenshots = false;  // F12 (else the test trigger)
    bool shot_this = false;
    if (shot_countdown == 0 && g_screenshot_requested.exchange(false)) {
      shot_countdown = 1;
      shot_to_screenshots = true;
    }
    if (shot_countdown == 0 && (frames & 7) == 0) {
      std::error_code shot_ec;
      if (std::filesystem::exists("gpuprof.trigger", shot_ec)) {
        std::filesystem::remove("gpuprof.trigger", shot_ec);
        renderer->ProfileGpu(120);  // results in rs_native_print.log
      }
      if (std::filesystem::exists("drawlist.trigger", shot_ec)) {
        std::filesystem::remove("drawlist.trigger", shot_ec);
        renderer->DrawListOnce();  // goes with the next picture
      }
      if (record_left == 0 && std::filesystem::exists("record.trigger", shot_ec)) {
        std::ifstream f("record.trigger");
        int64_t count = 600;
        std::string out = "clip";
        f >> count >> out;
        f.close();
        std::filesystem::remove("record.trigger", shot_ec);
        record_left = std::max<int64_t>(1, count);
        record_out = out;
        REXLOG_INFO("[native-present] recording {} frames ({})", record_left, record_out);
      }
      if (std::filesystem::exists("dump.trigger", shot_ec)) {
        std::filesystem::remove("dump.trigger", shot_ec);
        shot_countdown = headless ? 4 : 1;
        shot_to_screenshots = false;
      }
    }
    const bool render_for_shot = shot_countdown > 0;
    if (shot_countdown > 0 && --shot_countdown == 0) shot_this = true;
    const bool dump_this = dump_every > 0 && (frames + 1) % uint64_t(dump_every) == 0;
    if (dump_this) {
      char dir[96];
      std::snprintf(dir, sizeof(dir), "rs_native/passes_%06llu",
                    static_cast<unsigned long long>(frame.index));
      renderer->SetDumpPassesDir(dir);
    }
    // Settings page: these may change at any time (realsteel_overlay.cpp).
    uint32_t internal_w = 0, internal_h = 0;
    InternalResolution(internal_w, internal_h);
    const float aspect = PictureAspect(renderer->WindowAspect());
    ShapeInternalResolution(aspect, internal_w, internal_h);
    renderer->SetPictureAspect(aspect);
    g_picture_aspect.store(aspect);
    renderer->SetAspectMask2D(REXCVAR_GET(realsteel_native_aspect_mask_2d));
    renderer->SetFullCapture(REXCVAR_GET(realsteel_capture_full));
    renderer->SetVsync(WantVsync());
    renderer->SetQuality(
        AntiAliasing().msaa,
        uint32_t(std::clamp(REXCVAR_GET(realsteel_native_anisotropy), 1, 16)),
        static_cast<float>(REXCVAR_GET(realsteel_native_sharpen)));
    renderer->SetPostFx(AntiAliasing().fxaa, REXCVAR_GET(realsteel_native_hdr),
                        static_cast<float>(REXCVAR_GET(realsteel_native_hdr_peak)),
                        static_cast<float>(REXCVAR_GET(realsteel_native_hdr_paper)));
    renderer->SetLiveOptions(
        internal_w, internal_h, REXCVAR_GET(realsteel_native_stretch),
        static_cast<float>(REXCVAR_GET(realsteel_native_gamma)),
        !REXCVAR_GET(realsteel_native_dof));
    {
      // Frames over the last ~half second (2026-10-12: a running average of
      // 1/frame time reads high whenever frame times vary - 61-62 at a true
      // 60 on the Nova).
      static std::deque<std::chrono::steady_clock::time_point> stamps;
      const auto now = std::chrono::steady_clock::now();
      t_last_frame = now;
      stamps.push_back(now);
      while (stamps.size() > 2 && now - stamps.front() > std::chrono::milliseconds(500)) stamps.pop_front();
      const double span = std::chrono::duration<double>(now - stamps.front()).count();
      if (stamps.size() > 2 && span > 0.0) fps_smooth = double(stamps.size() - 1) / span;
    }
    realsteel::overlay::Pump(*renderer, fps_smooth);
    // A file named render_all turns the skipping off (screens that draw
    // parts of themselves once and keep them - lobby, versus - and the
    // surfaces a fight carries from frame to frame need every frame drawn).
    static bool render_all = false;
    if ((frames % 30) == 0) {
      std::error_code all_ec;
      render_all = std::filesystem::exists("render_all", all_ec);
    }
    if (headless && !render_all && !render_for_shot && !dump_this && record_left == 0) {
      ++frames;  // consumed, not drawn
      continue;
    }
#ifdef __ANDROID__
    if (!AndroidWindowReady(*renderer)) {
      ++frames;  // in the background: the game runs on, nothing is drawn
      continue;
    }
#endif
    const bool rendered =
        renderer->Render(frame, rgba, /*want_rgba=*/dump_this || shot_this || record_left > 0);
    if (record_left > 0 && rendered && !rgba.empty()) {
      uint32_t w = 1280, h = 720;
      renderer->LastReadbackSize(w, h);
      if (record_pipe == nullptr) {
        std::string cmd = REXCVAR_GET(realsteel_record_pipe);
        auto fill = [&cmd](const std::string& key, const std::string& value) {
          for (size_t at = cmd.find(key); at != std::string::npos; at = cmd.find(key, at + value.size())) {
            cmd.replace(at, key.size(), value);
          }
        };
        fill("{w}", std::to_string(w));
        fill("{h}", std::to_string(h));
        fill("{out}", record_out);
#ifdef _WIN32
        record_pipe = cmd.empty() ? nullptr : _popen(cmd.c_str(), "wb");
#else
        record_pipe = cmd.empty() ? nullptr : popen(cmd.c_str(), "w");
#endif
        record_w = w;
        record_h = h;
        if (record_pipe == nullptr) {
          REXLOG_INFO("[native-present] recording: no realsteel_record_pipe command, or it failed");
          record_left = 0;
        }
      }
      if (record_pipe != nullptr && (w != record_w || h != record_h)) {
        record_left = 1;  // the size changed: end the clip here
      } else if (record_pipe != nullptr) {
        std::fwrite(rgba.data(), 1, size_t(w) * h * 4, record_pipe);
      }
      if (--record_left == 0 && record_pipe != nullptr) {
#ifdef _WIN32
        _pclose(record_pipe);
#else
        pclose(record_pipe);
#endif
        record_pipe = nullptr;
        REXLOG_INFO("[native-present] recording finished ({})", record_out);
      }
    }
    {
      // Present pacing: with FIFO v-sync, Render returns once the frame is
      // queued behind the previous one, so the gaps between returns follow
      // the display. A gap near twice the median is a frame the display had
      // to show twice (or a late frame).
      static std::vector<float> gaps;
      static std::chrono::steady_clock::time_point last_present{};
      static std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();
      const auto now = std::chrono::steady_clock::now();
      if (rendered && last_present.time_since_epoch().count() != 0) {
        gaps.push_back(float(std::chrono::duration<double, std::milli>(now - last_present).count()));
      }
      if (rendered) last_present = now;
      if (now - window_start >= std::chrono::seconds(5) && gaps.size() > 10) {
        std::vector<float> v = gaps;
        std::sort(v.begin(), v.end());
        const float median = v[v.size() / 2];
        size_t doubled = 0;
        for (float g : v) {
          if (g > median * 1.5f) ++doubled;
        }
        REXLOG_INFO("[pacing] present gaps ms min {:.2f} p50 {:.2f} p99 {:.2f} max {:.2f}, "
                    "{} long of {} (v-sync {})",
                    v.front(), median, v[std::min(v.size() - 1, v.size() * 99 / 100)], v.back(),
                    doubled, v.size(), WantVsync() ? "on" : "off");
        gaps.clear();
        window_start = now;
      }
    }
    if (rendered && shot_this && !rgba.empty()) {
      uint32_t shot_w = 1280, shot_h = 720;
      renderer->LastReadbackSize(shot_w, shot_h);
      std::error_code shot_ec;
      if (shot_to_screenshots) {
        std::filesystem::create_directories("screenshots", shot_ec);
        const std::time_t now = std::time(nullptr);
        char name[96];
        std::strftime(name, sizeof(name), "screenshots/realsteel_%Y%m%d_%H%M%S.png", std::localtime(&now));
        rs::vk::WriteRgbaPngFile(name, rgba, shot_w, shot_h);
        REXLOG_INFO("[native-present] screenshot {}", name);
      } else {
        std::filesystem::create_directories("rs_native", shot_ec);
        rs::vk::WriteRgbaPngFile("rs_native/live_tmp.png", rgba, shot_w, shot_h);
        std::filesystem::rename("rs_native/live_tmp.png", "rs_native/live.png", shot_ec);
      }
    }
    render_ns += uint64_t((std::chrono::steady_clock::now() - t_render).count());
    if (dump_this) renderer->SetDumpPassesDir(std::string());
    if (!rendered) {
      if (!renderer->WindowOpen()) {
        REXLOG_INFO("[native-present] window closed — disabled");
        g_failed.store(true);
      }
      continue;
    }
    ++frames;
    if (dump_every > 0 && frames % uint64_t(dump_every) == 0) {
      std::error_code ec;
      std::filesystem::create_directories("rs_native", ec);
      char name[96];
      std::snprintf(name, sizeof(name), "rs_native/native_%06llu.png",
                    static_cast<unsigned long long>(frame.index));
      uint32_t shot_w = 1280, shot_h = 720;
      renderer->LastReadbackSize(shot_w, shot_h);
      rs::vk::WriteRgbaPngFile(name, rgba, shot_w, shot_h);
      std::snprintf(name, sizeof(name), "rs_native/native_%06llu.bin",
                    static_cast<unsigned long long>(frame.index));
      if (FILE* f = std::fopen(name, "wb")) {
        std::fwrite(bytes.data(), 1, bytes.size(), f);
        std::fclose(f);
      }
      REXLOG_INFO("[native-present] dumped frame {} to rs_native/", frame.index);
    }
    if (frames % 120 == 0) {
      const auto now = std::chrono::steady_clock::now();
      const double s = std::chrono::duration<double>(now - t0).count();
      const double window = std::chrono::duration<double>(now - t_report).count();
      t_report = now;
      uint64_t dropped;
      {
        std::lock_guard<std::mutex> lock(g_handoff_mutex);
        dropped = g_dropped;
      }
      const uint64_t gframes = g_guest_timed_frames.exchange(0);
      const uint64_t g_end = g_guest_frame_end_ns.exchange(0);
      const uint64_t g_ser = g_guest_serialize_ns.exchange(0);
      const uint64_t g_hooks = g_guest_draw_hooks_ns.exchange(0);
      REXLOG_INFO("[native-present] {} frames rendered, {:.1f} fps overall, "
                  "{:.1f} fps last 120 | worker: parse {:.1f} ms, render "
                  "{:.1f} ms | guest frame-end hook {:.1f} ms (serialize "
                  "{:.1f} ms), draw hooks {:.1f} ms, over {} guest frames | "
                  "{} skipped, {} draws, {} bytes this frame",
                  frames, frames / s, 120.0 / window, parse_ns / 120.0e6,
                  render_ns / 120.0e6,
                  gframes ? g_end / (1e6 * gframes) : 0.0,
                  gframes ? g_ser / (1e6 * gframes) : 0.0,
                  gframes ? g_hooks / (1e6 * gframes) : 0.0, gframes, dropped,
                  frame.draws.size(), in.has_data ? frame.payload_blob.size() + frame.command_blob.size() : bytes.size());
      parse_ns = render_ns = 0;
      std::fflush(stdout);
    }
  }
}

}  // namespace

bool Enabled() { return REXCVAR_GET(realsteel_native_present) && !g_failed.load(); }

void SetHostWindow(void* native_handle) { g_host_window.store(native_handle); }

void BandRect(float win_w, float win_h, float rect[4]) {
  // As the present pass places the picture (centred at its shape unless
  // stretched), then the 16:9 band in it that menus keep (Layout2D).
  const float aspect = g_picture_aspect.load() > 0.0f ? g_picture_aspect.load() : 16.0f / 9.0f;
  float px = 0.0f, py = 0.0f, pw = win_w, ph = win_h;
  if (!REXCVAR_GET(realsteel_native_stretch) && win_h > 0.0f) {
    if (pw / ph > aspect) {
      pw = ph * aspect;
      px = (win_w - pw) * 0.5f;
    } else {
      ph = pw / aspect;
      py = (win_h - ph) * 0.5f;
    }
  }
  const float rel = aspect / (16.0f / 9.0f);
  const float bw = rel > 1.0f ? pw / rel : pw;
  const float bh = rel < 1.0f ? ph * rel : ph;
  rect[0] = px + (pw - bw) * 0.5f;
  rect[1] = py + (ph - bh) * 0.5f;
  rect[2] = bw;
  rect[3] = bh;
}

void SetHostWindowAndroid(void* native_window) { g_host_android_window.store(native_window); }
void SetAndroidBackground(bool background) { g_android_background.store(background); }
float CurrentPictureAspect() { return g_picture_aspect.load(); }
void SetAndroidWindowSource(void* (*current_window)()) { g_android_window_source.store(current_window); }

void SetHostWindowLinux(void* x11_display, unsigned long x11_window,
                        void* wayland_display, void* wayland_surface) {
  g_host_x11_display.store(x11_display);
  g_host_x11_window.store(x11_window);
  g_host_wl_display.store(wayland_display);
  g_host_wl_surface.store(wayland_surface);
}

void SetHostWindowSize(uint32_t width, uint32_t height) {
  g_host_width.store(width);
  g_host_height.store(height);
}

void NoteGuestTiming(uint64_t frame_end_ns, uint64_t serialize_ns,
                     uint64_t draw_hooks_ns) {
  g_guest_frame_end_ns.fetch_add(frame_end_ns);
  g_guest_serialize_ns.fetch_add(serialize_ns);
  g_guest_draw_hooks_ns.fetch_add(draw_hooks_ns);
  g_guest_timed_frames.fetch_add(1);
}

bool WantsFrameBytes() { return REXCVAR_GET(realsteel_native_dump_every) > 0; }

bool WorkerIdle() {
  if (!g_worker_started.load()) return true;  // first frame starts the worker
  std::lock_guard<std::mutex> lock(g_handoff_mutex);
  return g_pending.empty();
}

namespace {
// Frame pacing (2026-10-12): the frame cap now waits after the render loop's
// present step, outside its lock (robot select, realsteel_hooks.cpp). The
// frame finished inside that step is held until the wait is over, so the
// renderer still gets frames at the cap's even pace - handed over at once,
// each arrived with the guest's work jitter and the fps counter on the Nova
// (v-sync off) read 59-62 instead of 60.
thread_local bool t_hold = false;
thread_local Handoff t_held;
thread_local uint8_t* t_held_base = nullptr;
void Publish(uint8_t* base, Handoff& frame);

void Offer(uint8_t* base, Handoff& frame) {
  if (t_hold) {
    if (!t_held.empty()) Publish(t_held_base, t_held);  // never two held
    t_held.swap(frame);  // the caller gets the held slot's old storage
    frame.clear();
    t_held_base = base;
    return;
  }
  Publish(base, frame);
}
}  // namespace

void HoldFrames(bool hold) {
  t_hold = hold;
  if (!hold && !t_held.empty()) Publish(t_held_base, t_held);
}

void OnFrameBytes(uint8_t* base, std::vector<uint8_t>& bytes) {
  if (!Enabled()) return;
  thread_local Handoff h;
  h.clear();
  h.bytes.swap(bytes);
  Offer(base, h);
  bytes.swap(h.bytes);
  bytes.clear();
}

void OnFrameData(uint8_t* base, rs::capture::FrameData& frame) {
  if (!Enabled()) return;
  thread_local Handoff h;
  h.clear();
  std::swap(h.data, frame);
  h.has_data = true;
  Offer(base, h);
  std::swap(h.data, frame);  // storage of an earlier frame, to refill
}

namespace {
void Publish(uint8_t* base, Handoff& bytes) {
  if (!g_worker_started.exchange(true)) {
    g_base = base;
    std::thread(WorkerMain).detach();
  }
  std::unique_lock<std::mutex> lock(g_handoff_mutex);
  // Every frame must reach the renderer (2026-09-15: a screen can render
  // something once and keep it alive through front-buffer feedback — the
  // versus screen's robot — so a skipped frame is lost for good). If the
  // worker is still busy with the previous frame, wait for it (bounded, so a
  // stuck renderer cannot hang the game); this also paces the guest to the
  // renderer.
  if (!g_pending.empty()) {
    g_consumed_cv.wait_for(lock, std::chrono::milliseconds(200),
                           [] { return g_pending.empty() || g_failed.load(); });
    if (!g_pending.empty()) ++g_dropped;  // gave up: replace
  }
  // Swap, not copy: a fight frame is 5-8 MB. The caller gets the buffer the
  // worker finished with back (capacity kept on both sides).
  g_pending.swap(bytes);
  bytes.clear();
  ++g_pending_seq;
  g_handoff_cv.notify_one();
}
}  // namespace

unsigned char* GuestBase() { return g_base; }

void RequestScreenshot() {
  g_screenshot_requested.store(true);
  REXLOG_INFO("[native-present] screenshot requested");
}

}  // namespace realsteel::native_present
