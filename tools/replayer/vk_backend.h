// The Vulkan renderer that draws the game from its recorded frames.
//
// Renders a captured frame with the real recompiled shaders (SPIR-V cache),
// captured geometry (v4 IB/VB + inline payload snapshots) and dumped
// textures, into an offscreen image written out as a BMP. No window, no
// swapchain — the offline twin of the future in-process renderer.

#pragma once

#include "capture_format.h"
#include "texture_decode.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace rs::vk {

// Where the backend gets texture content when not reading rs_textures/ dumps:
// the in-process path implements this over guest memory (decode with
// rs::capture::DecodeGuestTexture, cache by content hash).
struct TextureSource {
  virtual ~TextureSource() = default;
  // |hash| is the draw's content hash for |phys| (0 = unknown). Fill |out|
  // (width 0 = unavailable → white fallback).
  virtual bool Fetch(uint32_t phys, uint64_t hash,
                     rs::capture::DecodedTexture& out) = 0;
};

struct RenderOptions {
  TextureSource* texture_source = nullptr;  // null: use textures_dir dumps
  // In-process path: resolves this session's microcode addresses (see
  // ShaderLibrary::SetLiveLookup). Null: manifest file only.
  bool (*shader_lookup)(uint32_t va, uint64_t* container_hash, bool* is_vs) = nullptr;
  // In-process: container bytes from the game's memory (no files needed).
  bool (*container_source)(uint64_t container_hash, std::vector<uint8_t>* bytes) = nullptr;
  std::string output_bmp;
  // Output tone curve applied to the final image only (RGB = RGB^gamma).
  // 2026-09-06: the xenos plugin's presenter applies the title's display
  // gamma ramp (game builds sRGB / Rec.709 / pow(1/level) from
  // VdGetCurrentDisplayGamma, composed with the brightness LUT); measured
  // against the author's screenshot the net curve is ~pow(1.28). Not applied by
  // default; --vkgamma 1.28 matches the screenshots.
  float output_gamma = 1.0f;
  bool no_gamma_ramp = false;      // --vknoramp: ignore a captured (v14) display ramp
  // --vknodof: zero the depth-of-field blend of the scene composite (pixel
  // shader CB9A5EB75CB8B59E, g_f4DOFParam at c20). 2026-10-02: the game asks
  // for up to 63% blurred background beyond the focus depth and our depth
  // texture delivers it; the xenos plugin shows no such blur (its depth
  // fetch evidently lands in focus), so matching xenos means switching it
  // off. Which one the console showed is not established.
  bool no_dof = false;
  // --vktemplates: resolve vertex shaders through the on-disk templates even
  // when the exact runtime-patched translation exists (equivalence testing).
  bool prefer_vs_templates = false;
  // Anisotropic texture filtering (1 = off, up to 16); clamped to what the
  // device supports.
  uint32_t anisotropy = 1;
  // Sharpening of the presented picture, 0 (off) to 1.
  float sharpen = 0.0f;
  // FXAA in the present pass (source texels), 2026-10-12.
  bool fxaa = false;
  // HDR output (2026-10-12): an HDR10 (PQ) or scRGB swapchain where the
  // surface offers one; the SDR picture is expanded in the present pass
  // (paper white for everyday content, highlights up to the peak).
  bool hdr = false;
  float hdr_peak_nits = 600.0f;
  float hdr_paper_nits = 200.0f;
  bool no_msaa = false;            // --vknomsaa: render single-sampled even when the capture says 2x
  // --vkwindow: present each frame into a window (bilinear blit of the
  // 1280x720 output) instead of / in addition to the BMP. Win32 only.
  bool window = false;
  // Never activate the window (in-process path: the game's own window keeps
  // keyboard/controller focus while the native one is only looked at).
  bool window_background = false;
  // Present into this existing native window (HWND on Windows) instead of
  // creating one; its owner keeps the message loop, focus and fullscreen
  // handling (2026-10-02: the game's own window once the GPU plugin is gone).
  void* external_window = nullptr;
  // The same on Linux (2026-10-02, Steam Deck): the host window as X11
  // (display + window id) or Wayland (display + surface) handles; whichever
  // pair is set is used.
  void* x11_display = nullptr;
  unsigned long x11_window = 0;
  void* wayland_display = nullptr;
  void* wayland_surface = nullptr;
  // Android (2026-10-11): the activity's ANativeWindow, from SDL.
  void* android_window = nullptr;
  // Pixel size of the host window right now. Needed where the surface has no
  // size of its own (Wayland) and to notice resizes there.
  void (*window_size)(uint32_t* width, uint32_t* height) = nullptr;
  bool HasHostWindow() const {
    return external_window != nullptr || x11_display != nullptr ||
           wayland_display != nullptr || android_window != nullptr;
  }
  // Internal resolution multiplier (1-4): every render target, viewport and
  // copy is scaled, guest-space bookkeeping stays in 1280x720 units; the
  // present pass filters down to the window. 2 = 2560x1440.
  uint32_t resolution_scale = 1;
  // Exact internal resolution of the 1280x720 guest frame (0 = use
  // resolution_scale). Need not be 16:9: 1280x800 / 1280x960 render the
  // picture at a handheld panel's own pixel grid for a stretched present.
  uint32_t internal_width = 0;
  uint32_t internal_height = 0;
  // Window presentation: false keeps 16:9 with black bars, true fills the
  // window whatever its shape.
  bool present_stretch = false;
  // Wait for the display's vertical blank when presenting (FIFO). Off picks
  // MAILBOX, else IMMEDIATE, where the surface offers them: presenting never
  // blocks, so a frame that misses a refresh is not held for a whole extra
  // one (2026-10-03, Steam Deck at 90 Hz: 13 ms frames were shown every
  // 22 ms = 45 fps). Pace with the guest frame cap instead.
  bool vsync = true;
  // Shape of the displayed picture (width / height); 0 = the guest's 16:9.
  // Other values re-proportion the scene inside the guest's 16:9 frame (see
  // the draw loop) and present it at this shape: 1.6 fills a 1280x800 panel,
  // 1.333 a 4:3 one, without distortion.
  float picture_aspect = 0.0f;
  // Readbacks (Render's |rgba|) at the picture's shape, as presented (the
  // 16:9 band mask included), instead of 1280x720: 720 high for narrower
  // shapes, 1280 wide for wider. LastReadbackSize says what was produced.
  bool readback_picture_shape = false;
  // Passes render the whole attachment (as before 2026-10-12) instead of the
  // part viewports reach - for comparing output.
  bool full_render_area = false;
  // Picture shapes other than 16:9: screens without 3D show only their 16:9
  // band (black elsewhere), as before 2026-10-12, instead of the whole
  // picture with backdrops widened (Layout2D in the backend).
  bool aspect_mask_2d = false;
  // Live path: no per-frame stdout chatter (resolve lists etc.); the print
  // log is unbuffered and every line is a write.
  bool quiet = false;
  uint32_t window_width = 1280;
  uint32_t window_height = 720;
  std::string shaders_dir = "rs_shaders";
  // Vulkan pipeline cache file (empty = none): pipelines built once are kept
  // across runs, so a robot or screen seen before needs no driver compile
  // (each took ~70 ms on first use; drivers like Adreno's keep no cache).
  std::string pipeline_cache_path;
  std::string textures_dir = "rs_textures";
  bool validation = false;
  // Empirically (2026-08-29, frame 4600 A/B): Real Steel's clip space maps
  // correctly WITHOUT a viewport Y flip. --vkflip re-enables the D3D-style
  // negative-height viewport for experiments.
  bool flip_y = false;
  bool no_depth_resolves = false;  // experiment: leave depth-format resolves white
  int zero_slot = -1;
  int white_slot = -1;             // experiment: bind white to this sampler slot for every draw              // experiment: bind black to this sampler slot for every draw
  bool use_cull = false;           // --vkcull: apply captured cull mode (winding unverified)
  float alpha_ref = 0.5f;          // --vkalpharef X: alpha-test reference (v14 captures carry the real one)
  bool alpha_ref_override = false; // true when --vkalpharef was given: wins over the captured value
  // Per-draw/per-pass event trace to stdout (--vktrace).
  bool trace = false;
  // Suppress draws whose command index exceeds this (--vklimit N): bisects
  // the frame to find overdraw culprits. State replay is unaffected.
  uint32_t max_draw_cmd = 0xFFFFFFFFu;
  // Suppress draws before this command index (--vkfrom N).
  uint32_t min_draw_cmd = 0;
  // Dump the color attachment after every pass into this directory
  // (--vkdumppasses DIR): the per-pass debugger.
  std::string dump_passes_dir;
  // Experiments: reverse bool-constant bit order within each dword
  // (--vkboolrev); disable depth test/write entirely (--vknodepth).
  bool bool_reverse = false;
  // 2026-09-06: the recompiler's 16-bit texcoord lane swap (tfetchTexcoord
  // .yx for FMT_16_16*) is WRONG for Real Steel: its streams are 8IN32 and
  // the half-float pair is already (u, v) after the host dword swap. With the
  // swap on, every 16-bit-UV mesh sampled a transposed atlas (Metro's paint
  // landed on grey, the Robot lost its blue lights). Off by default;
  // --vkuvswap re-enables it for comparison.
  bool uv_lane_swap = false;

  // Force every bool constant TRUE (--vkboolall): path-selection probe.
  bool bool_all = false;
  // Replace every pixel shader with a flat magenta one (--vkflatps):
  // proves rasterization independent of pixel-shader math.
  bool flat_ps = false;
  bool no_depth = false;
  // Bind the white fallback for every sampler slot (--vkwhite): isolates
  // texture-sourced darkening from constant math.
  bool all_white = false;
  // Depth compare ALWAYS with writes on (--vkdepthalways): distinguishes
  // depth-failing geometry from geometry that never rasterizes.
  bool depth_always = false;
};

// Renders |frame| and writes options.output_bmp. Returns false after printing
// the reason. Draws whose shaders cannot be resolved are skipped and counted.
bool RenderFrameVulkan(const rs::capture::FrameData& frame,
                       const RenderOptions& options);

// UI overlay drawn over the presented picture (2026-10-02: the in-game
// settings page). The owner records textured, vertex-coloured triangles in a
// coordinate space of |width| x |height| covering the whole window (Dear
// ImGui draw lists) and hands the finished frame over; the backend draws the
// latest one inside the present pass, after the game picture.
struct OverlayVertex {
  float x, y;
  float u, v;
  uint32_t color;  // RGBA8, R in the low byte
};
struct OverlayDraw {
  uint32_t texture = 0;       // id of an OverlayTextureOp
  uint32_t index_offset = 0;
  uint32_t index_count = 0;
  int32_t base_vertex = 0;
  // Scissor in overlay coordinates.
  float left = 0, top = 0, right = 0, bottom = 0;
};
struct OverlayFrame {
  float width = 0, height = 0;
  std::vector<OverlayVertex> vertices;
  std::vector<uint16_t> indices;
  std::vector<OverlayDraw> draws;
};
// Creates / replaces texture |id| from RGBA8 pixels; empty |rgba| destroys it.
struct OverlayTextureOp {
  uint32_t id = 0;
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> rgba;
};

// Persistent renderer (2026-09-06): one Vulkan device, shader/texture/
// pipeline caches and EDRAM-model attachments kept across frames — the shape
// the in-process path needs, and what makes consecutive captures carry their
// temporal state (the glow chain's previous-frame feedback) across frames.
class Renderer {
 public:
  Renderer();
  ~Renderer();
  Renderer(const Renderer&) = delete;
  Renderer& operator=(const Renderer&) = delete;
  bool Init(const RenderOptions& options);
  // Renders one frame into |rgba| (1280x720 RGBA8, output curve applied).
  // |want_rgba| false: skip the readback + output curve (live frames that
  // are only presented to the window).
  bool Render(const rs::capture::FrameData& frame, std::vector<uint8_t>& rgba,
              bool want_rgba = true);
  // Convenience: Render + BMP write to |path|.
  bool RenderToBmp(const rs::capture::FrameData& frame, const std::string& path);
  // Window mode: false once the user closed the window.
  bool WindowOpen() const;
  // Per-pass debugger for the next frames: dump every pass's colour
  // attachment into |dir| (empty: off). Same output as --vkdumppasses.
  void SetDumpPassesDir(const std::string& dir);
  // Settings that may change while running; call between frames from the
  // thread that calls Render. A new internal resolution drops every render
  // target, texture and resolve target (rebuilt on demand at the next frame).
  void SetLiveOptions(uint32_t internal_width, uint32_t internal_height,
                      bool present_stretch, float output_gamma, bool no_dof);
  // Picture shape (RenderOptions::picture_aspect), and the window's own
  // shape for an "automatic" setting (0 until the swapchain exists).
  void SetVsync(bool vsync);
  // Picture quality settings that may change while running (same thread as
  // Render): the game's multisampling on/off, anisotropic filtering level
  // (1 = off) and present sharpening (0-1).
  void SetQuality(bool msaa, uint32_t anisotropy, float sharpen);
  // FXAA and HDR (same thread as Render). Turning HDR on or off rebuilds the
  // swapchain at the next frame; HdrActive says what the display took.
  // Readbacks (screenshots, recordings) at the internal resolution with the
  // output tone curve, instead of the 1280x720 test picture.
  void SetFullCapture(bool on);
  void SetPostFx(bool fxaa, bool hdr, float peak_nits, float paper_nits);
  bool HdrActive() const;
  void SetPictureAspect(float aspect);
  void SetAspectMask2D(bool mask);  // RenderOptions::aspect_mask_2d
  float WindowAspect() const;
  // Android: the app went to the background (null) or came back with a new
  // ANativeWindow - the surface and swapchain are rebuilt on it. Same thread
  // as Render; Render must not be called while there is no window.
  void ReplaceAndroidWindow(void* native_window);
  // True once presenting reported the surface lost (Android, app paused).
  bool SurfaceLost() const;
  // Size of the last |rgba| Render produced (1280x720 unless
  // RenderOptions::readback_picture_shape gave it the picture's shape).
  void LastReadbackSize(uint32_t& width, uint32_t& height) const;
  // Debugging: log the next rendered frame's draw list to stdout.
  void DrawListOnce();
  // Profiling: GPU time of every render pass over the next |frames| frames,
  // printed (heaviest first) once they are done.
  void ProfileGpu(uint32_t frames);
  // UI overlay (same thread as Render): texture ops apply at the next frame
  // start, the frame is drawn with every present until replaced (null: none).
  void QueueOverlayTexture(OverlayTextureOp op);
  void SetOverlay(std::shared_ptr<const OverlayFrame> frame);

 private:
  void* backend_;  // rs::vk::(anonymous)::Backend
};

// Use a player-chosen driver's vkGetInstanceProcAddr instead of the system
// Vulkan loader (Android custom GPU driver; call before Init). Null = system.
void SetVulkanLoader(void* get_instance_proc_addr);
// "<GPU>, driver <name> [<info>] (id, version, Vulkan x.y.z)" once Init ran.
const std::string& DriverDescription();

// RGBA8 -> PNG helper (also used by the texture dumper in main.cpp).
bool WriteRgbaPngFile(const std::string& path,
                      const std::vector<uint8_t>& rgba, uint32_t width,
                      uint32_t height);

}  // namespace rs::vk
