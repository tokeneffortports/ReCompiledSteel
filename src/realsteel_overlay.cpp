#include "realsteel_overlay.h"

#include "realsteel_keyboard.h"
#include "realsteel_icon.h"
#include "realsteel_native_present.h"
#include "realsteel_netplay.h"
#include "realsteel_online.h"
#include "realsteel_touch.h"
#include "realsteel_version.h"

#include "vk_backend.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>
#include <rex/ui/windowed_app_context.h>

REXCVAR_DEFINE_BOOL(realsteel_mute_background, true, "Real Steel",
                    "Mute the game while its window is not focused");
REXCVAR_DEFINE_BOOL(realsteel_show_fps, false, "Real Steel",
                    "Show the native renderer's frame rate in the corner");
REXCVAR_DEFINE_BOOL(realsteel_vibration, true, "Real Steel",
                    "Controller vibration");
REXCVAR_DEFINE_BOOL(realsteel_settings_open, false, "Real Steel",
                    "Open the settings page at start-up (testing)");

namespace realsteel::overlay {
namespace {

constexpr const char* kSettingsFile = "realsteel_settings.cfg";

// ---------------------------------------------------------------------------
// The settings table. One row per setting; the page, the defaults button and
// the settings file are all driven from it.
// ---------------------------------------------------------------------------

enum class Kind { kToggle, kChoice, kSlider, kText, kKey, kInfo, kAction };

struct Choice {
  const char* label;
  const char* value;
};

struct Setting {
  const char* section;
  const char* label;
  const char* cvar;
  Kind kind;
  const char* default_value;
  const char* help;
  std::vector<Choice> choices;      // kChoice
  float slider_min = 0, slider_max = 1;  // kSlider
  void (*on_change)() = nullptr;    // UI thread, after the cvar was set
  std::string (*info)() = nullptr;  // kInfo: the text to show
};

void ApplyFullscreen();
void RequestQuit();

// Cheats: the game's cash is a 32-bit word at a fixed address in its data
// (found 2026-10-11 by scanning for the amount the garage shows; the game
// saves it with the profile).
constexpr uint32_t kCashAddress = 0x824B06A8u;
void AddCash() {
  unsigned char* base = realsteel::native_present::GuestBase();
  if (base == nullptr) return;
  unsigned char* p = base + kCashAddress;
  uint32_t cash = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  cash = std::min<uint32_t>(cash + 100000u, 9999999u);
  p[0] = uint8_t(cash >> 24);
  p[1] = uint8_t(cash >> 16);
  p[2] = uint8_t(cash >> 8);
  p[3] = uint8_t(cash);
  REXLOG_INFO("[cheats] cash set to {}", cash);
}
void SaveSettings();

const std::vector<Setting>& Settings() {
  static const std::vector<Setting> settings = {
      // ------------------------------------------------------------ Display
      {"Display", "Fullscreen", "fullscreen", Kind::kToggle, "false",
       "Borderless fullscreen on the current monitor.", {}, 0, 1,
       &ApplyFullscreen},
      {"Display", "Aspect ratio", "realsteel_native_aspect", Kind::kChoice,
       "auto",
       "Shape of the picture. Other shapes than 16:9 show more of the scene "
       "instead of stretching it; 16:9 keeps the original framing with bars.",
       {{"Match screen", "auto"},
        {"16:9 (original)", "16:9"},
        {"16:10 (Steam Deck)", "16:10"},
        {"4:3 (Retroid Pocket Nova)", "4:3"},
        {"21:9 (ultrawide)", "21:9"}}},
      {"Display", "Bars on 2D screens", "realsteel_native_aspect_mask_2d", Kind::kToggle,
       "false",
       "Shapes other than 16:9: title, loading and other screens without 3D "
       "keep the original 16:9 picture with black bars instead of a widened "
       "backdrop."},
      {"Display", "V-sync", "realsteel_native_vsync", Kind::kChoice, "auto",
       "Off shows each frame as soon as it is ready; on waits for the "
       "display. Automatic: off on Steam Deck / Linux, on under Windows.",
       {{"Automatic", "auto"}, {"On", "on"}, {"Off", "off"}}},
      {"Display", "Gamma", "realsteel_native_gamma", Kind::kSlider, "1.28",
       "Lower is brighter. 1.28 is the original look.", {}, 0.80f, 1.80f},
      {"Display", "Sharpening", "realsteel_native_sharpen", Kind::kSlider, "0",
       "Sharpens the final picture. 0 is off; useful when the internal "
       "resolution is below the screen's.", {}, 0.0f, 1.0f},
      {"Display", "HDR", "realsteel_native_hdr", Kind::kToggle, "false",
       "High dynamic range output: highlights (sparks, lights, chrome) glow "
       "brighter than ordinary white. Needs an HDR display with HDR switched "
       "on in the system (Windows: Settings > Display > Use HDR); otherwise "
       "the picture stays as it is."},
      {"Display", "HDR peak brightness", "realsteel_native_hdr_peak", Kind::kSlider, "600",
       "How bright the brightest highlights get, in nits. Set it to your "
       "display's peak (often 400-1000).", {}, 400.0f, 2000.0f},
      {"Display", "HDR paper white", "realsteel_native_hdr_paper", Kind::kSlider, "200",
       "How bright ordinary white is, in nits: menus, text and most of the "
       "picture. Raise it if HDR looks dim.", {}, 80.0f, 400.0f},
      {"Display", "Show frame rate", "realsteel_show_fps", Kind::kToggle, "false",
       "Frame rate counter in the top left corner."},
      // ----------------------------------------------------------- Graphics
      {"Graphics", "Internal resolution", "realsteel_native_resolution",
       Kind::kChoice, "1280x720",
       "Resolution the game is rendered at before it is scaled to the screen "
       "(the height follows the aspect ratio). 720p is the native resolution "
       "of the Steam Deck and Retroid Pocket Nova panels. Below 720p is for "
       "slower devices: faster, but softer than the original.",
       {{"360p (fastest)", "640x360"},
        {"480p (faster)", "854x480"},
        {"540p (faster)", "960x540"},
        {"720p (original; Nova and Deck native)", "1280x720"},
        {"900p (1.25x)", "1600x900"},
        {"1080p (1.5x)", "1920x1080"},
        {"1440p (2x)", "2560x1440"},
        {"2160p / 4K (3x)", "3840x2160"},
        {"2880p (4x)", "5120x2880"}}},
      {"Graphics", "Anti-aliasing", "realsteel_native_aa", Kind::kChoice,
       "msaa",
       "MSAA is the game's own edge smoothing; FXAA is much cheaper, good on "
       "handhelds. MSAA + FXAA also catches the edges MSAA misses (fences, "
       "cut-out textures); a higher internal resolution smooths edges too.",
       {{"MSAA (game default)", "msaa"},
        {"MSAA + FXAA", "msaa_fxaa"},
        {"FXAA (fast)", "fxaa"},
        {"Off", "off"}}},
      {"Graphics", "Texture filtering", "realsteel_native_anisotropy",
       Kind::kChoice, "1",
       "Anisotropic filtering keeps floors and other slanted surfaces sharp "
       "into the distance.",
       {{"Original (trilinear)", "1"},
        {"Anisotropic 2x", "2"},
        {"Anisotropic 4x", "4"},
        {"Anisotropic 8x", "8"},
        {"Anisotropic 16x", "16"}}},
      {"Graphics", "Depth of field", "realsteel_native_dof", Kind::kToggle,
       "false", "The game's background blur."},
      // -------------------------------------------------------------- Audio
      {"Audio", "Mute in background", "realsteel_mute_background", Kind::kToggle, "true",
       "Silence the game while another window has the focus."},
      {"Audio", "Mute", "audio_mute", Kind::kToggle, "false",
       "Silences all game audio."},
      // ----------------------------------------------------------- Controls
      {"Controls", "Vibration", "realsteel_vibration", Kind::kToggle, "true",
       "Controller rumble."},
      {"Controls", "Touch controls", "realsteel_touch_controls", Kind::kChoice,
#ifdef __ANDROID__
       "auto",
#else
       "off",
#endif
       "On-screen buttons for touch screens. Automatic shows them only when no "
       "controller is connected. The ... button at the top opens this page.",
       {{"Automatic", "auto"}, {"On", "on"}, {"Off", "off"}}},
      {"Controls", "Keyboard controls", "realsteel_keyboard", Kind::kToggle, "true",
       "The keys below act as pad 0 (with or without a controller plugged "
       "in). Click a key to change it, then press the new key."},
      // --------------------------------------------------------------- Game
      {"Game", "Online play", "realsteel_online", Kind::kToggle, "true",
       "Makes the MULTIPLAYER menu work: matches are found through the lobby "
       "server, on your local network and at the join address. Restart the "
       "game after changing this."},
      {"Game", "Your name", "realsteel_online_name", Kind::kText, "",
       "The name other players see (up to 15 letters). Empty: PLAYER "
       "and a number."},
      {"Game", "Lobby server", "realsteel_lobby_server", Kind::kText, "",
       "A lobby server lists matches over the internet, so nobody has to "
       "type addresses: host or host:port, from whoever runs one. Empty: "
       "none."},
      {"Game", "Lobby status", "", Kind::kInfo, "", "What the lobby server last said.",
       {}, 0, 1, nullptr, [] { return realsteel::netplay::LobbyStatus(); }},
      {"Game", "Local network", "realsteel_net_lan", Kind::kToggle, "true",
       "Also look for matches on your local network."},
      {"Game", "Join address", "realsteel_net_peers", Kind::kText, "",
       "Internet play: the host's public IP address, with the port if it is "
       "not 28700 (1.2.3.4:28700). Several hosts: separate with commas. "
       "Empty: your local network only."},
      {"Game", "Match password", "realsteel_net_password", Kind::kText, "",
       "A host's match is only seen by players who entered the same "
       "password. Empty: open to everyone who can reach you."},
      {"Game", "Your UDP port", "", Kind::kInfo, "",
       "To host over the internet, forward this UDP port on your router to "
       "this machine and give joiners your public IP address (and the port).",
       {}, 0, 1, nullptr, [] { return std::to_string(realsteel::netplay::ServicePort()); }},
      {"Game", "Language", "user_language", Kind::kChoice, "1",
       "The game ships in English and Japanese. Restart the game after "
       "changing this.",
       {{"English", "1"}, {"Japanese", "2"}}},
      // ------------------------------------------------------------- Cheats
      {"Cheats", "Cash", "", Kind::kAction, "+ R100,000",
       "Adds R100,000 to your cash. The amount shows in the garage and is saved "
       "with your game data.", {}, 0, 1, &AddCash},
      {"Cheats", "Game speed", "realsteel_guest_fps_cap", Kind::kChoice, "60",
       "The game runs at this fraction of normal speed (it follows the frame "
       "rate: 60 fps is normal). Slow motion for knockouts, fast forward for "
       "careers.",
       {{"50% (slow motion)", "30"}, {"75%", "45"}, {"100% (normal)", "60"},
        {"150%", "90"}, {"200%", "120"}, {"Unlimited", "0"}}},
  };
  static const std::vector<Setting> all = [] {
    std::vector<Setting> out = settings;
    for (const realsteel::keyboard::Action& a : realsteel::keyboard::Actions()) {
      out.push_back({"Controls", a.label, a.cvar, Kind::kKey, a.default_key, a.help, {}, 0, 1,
                     &realsteel::keyboard::ReloadMapping});
    }
    return out;
  }();
  return all;
}

// Tabs of the page, in order of first appearance in the table.
const std::vector<const char*>& Sections() {
  static const std::vector<const char*> sections = [] {
    std::vector<const char*> out;
    for (const Setting& s : Settings()) {
      bool seen = false;
      for (const char* name : out) seen = seen || std::strcmp(name, s.section) == 0;
      if (!seen) out.push_back(s.section);
    }
    return out;
  }();
  return sections;
}

bool AsBool(const std::string& v) { return v == "true" || v == "1"; }

// Cvar values come back as text ("1", "1.28", "60.000000", "true"); compare
// numerically where both sides are numbers.
bool SameValue(const std::string& a, const char* b) {
  char* ea = nullptr;
  char* eb = nullptr;
  const double da = std::strtod(a.c_str(), &ea);
  const double db = std::strtod(b, &eb);
  if (ea != a.c_str() && *ea == 0 && eb != b && *eb == 0) return da == db;
  if (a == b) return true;
  return (a == "true" || a == "false" || !std::strcmp(b, "true") ||
          !std::strcmp(b, "false")) &&
         AsBool(a) == AsBool(b);
}

// Kept in the settings file without a row on the page.
constexpr const char* kStoredOnly[] = {
    "realsteel_online_xuid",  // this player's online identity, made on first run
};
// Read from older settings files only, to carry them over (LoadSavedSettings).
constexpr const char* kLegacy[] = {
    "realsteel_native_msaa",  // + realsteel_native_fxaa: realsteel_native_aa since 2026-10-12
    "realsteel_native_fxaa",
};

void SaveSettings() {
  FILE* f = std::fopen(kSettingsFile, "w");
  if (f == nullptr) return;
  std::fputs("# Real Steel settings (written by the in-game settings page)\n", f);
  for (const Setting& s : Settings()) {
    if (s.kind == Kind::kInfo || s.kind == Kind::kAction || s.cvar[0] == 0) continue;
    std::fprintf(f, "%s=%s\n", s.cvar, rex::cvar::GetFlagByName(s.cvar).c_str());
  }
  for (const char* name : kStoredOnly) {
    std::fprintf(f, "%s=%s\n", name, rex::cvar::GetFlagByName(name).c_str());
  }
  std::fclose(f);
}


// ---------------------------------------------------------------------------
// Recording drawer: UI thread -> renderer thread.
// ---------------------------------------------------------------------------

std::mutex g_mutex;  // guards the three below
std::vector<rs::vk::OverlayTextureOp> g_texture_ops;
std::shared_ptr<const rs::vk::OverlayFrame> g_frame;
bool g_frame_dirty = false;

std::atomic<uint32_t> g_next_texture_id{1};

void Publish(std::shared_ptr<const rs::vk::OverlayFrame> frame) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_frame = std::move(frame);
  g_frame_dirty = true;
}

class RecordedTexture : public rex::ui::ImmediateTexture {
 public:
  RecordedTexture(uint32_t width, uint32_t height, uint32_t id)
      : ImmediateTexture(width, height), id(id) {}
  ~RecordedTexture() override {
    rs::vk::OverlayTextureOp op;
    op.id = id;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_texture_ops.push_back(std::move(op));
  }
  uint32_t id;
};

class RecordingDrawer : public rex::ui::ImmediateDrawer {
 public:
  RecordingDrawer() {
    const uint8_t white[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    white_ = CreateTexture(1, 1, rex::ui::ImmediateTextureFilter::kNearest, false,
                           white);
  }

  std::unique_ptr<rex::ui::ImmediateTexture> CreateTexture(
      uint32_t width, uint32_t height, rex::ui::ImmediateTextureFilter /*filter*/,
      bool /*is_repeated*/, const uint8_t* data) override {
    if (width == 0 || height == 0 || data == nullptr) return nullptr;
    rs::vk::OverlayTextureOp op;
    op.id = g_next_texture_id.fetch_add(1);
    op.width = width;
    op.height = height;
    op.rgba.assign(data, data + size_t(width) * height * 4);
    const uint32_t id = op.id;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      g_texture_ops.push_back(std::move(op));
    }
    return std::make_unique<RecordedTexture>(width, height, id);
  }

  void Begin(rex::ui::UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override {
    rex::ui::ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width,
                                    coordinate_space_height);
    frame_ = std::make_shared<rs::vk::OverlayFrame>();
    frame_->width = coordinate_space_width > 0.0f
                        ? coordinate_space_width
                        : float(ui_draw_context.render_target_width());
    frame_->height = coordinate_space_height > 0.0f
                         ? coordinate_space_height
                         : float(ui_draw_context.render_target_height());
  }

  void BeginDrawBatch(const rex::ui::ImmediateDrawBatch& batch) override {
    if (frame_ == nullptr) return;
    static_assert(sizeof(rex::ui::ImmediateVertex) == sizeof(rs::vk::OverlayVertex));
    batch_vertex_base_ = frame_->vertices.size();
    batch_index_base_ = frame_->indices.size();
    batch_indexed_ = batch.indices != nullptr;
    const size_t vertex_count = size_t(batch.vertex_count > 0 ? batch.vertex_count : 0);
    frame_->vertices.resize(batch_vertex_base_ + vertex_count);
    if (vertex_count != 0) {
      std::memcpy(frame_->vertices.data() + batch_vertex_base_, batch.vertices,
                  vertex_count * sizeof(rs::vk::OverlayVertex));
    }
    if (batch_indexed_ && batch.index_count > 0) {
      frame_->indices.insert(frame_->indices.end(), batch.indices,
                             batch.indices + batch.index_count);
    }
  }

  void Draw(const rex::ui::ImmediateDraw& draw) override {
    if (frame_ == nullptr || !batch_indexed_ || draw.count <= 0 ||
        draw.primitive_type != rex::ui::ImmediatePrimitiveType::kTriangles) {
      return;
    }
    rs::vk::OverlayDraw d;
    d.texture = draw.texture != nullptr
                    ? static_cast<RecordedTexture*>(draw.texture)->id
                    : static_cast<RecordedTexture*>(white_.get())->id;
    d.index_offset = uint32_t(batch_index_base_) + uint32_t(draw.index_offset);
    d.index_count = uint32_t(draw.count);
    d.base_vertex = int32_t(batch_vertex_base_) + draw.base_vertex;
    if (draw.scissor) {
      d.left = draw.scissor_left;
      d.top = draw.scissor_top;
      d.right = draw.scissor_right;
      d.bottom = draw.scissor_bottom;
    } else {
      d.right = frame_->width;
      d.bottom = frame_->height;
    }
    frame_->draws.push_back(d);
  }

  void EndDrawBatch() override {}

  void End() override {
    Publish(std::move(frame_));
    frame_.reset();
    rex::ui::ImmediateDrawer::End();
  }

 private:
  std::unique_ptr<rex::ui::ImmediateTexture> white_;
  std::shared_ptr<rs::vk::OverlayFrame> frame_;
  size_t batch_vertex_base_ = 0;
  size_t batch_index_base_ = 0;
  bool batch_indexed_ = false;
};

// ---------------------------------------------------------------------------
// State (UI thread unless noted).
// ---------------------------------------------------------------------------

rex::ui::Window* g_window = nullptr;
rex::ui::WindowedAppContext* g_app_context = nullptr;
std::unique_ptr<RecordingDrawer> g_drawer;
std::unique_ptr<rex::ui::ImGuiDrawer> g_imgui;
ImFont* g_font = nullptr;

std::atomic<bool> g_ready{false};       // any thread
std::atomic<bool> g_tick_pending{false};
std::atomic<bool> g_page_open{false};   // read by the guest's pad hook
std::atomic<bool> g_text_entry_open{false};  // a native-page text box is up
void PollTextEntry();  // UI thread: closes a finished text box
void TickTabStrip();   // UI thread: tabs over the game's OPTIONS panel
void DropTextEntry();
std::atomic<float> g_fps{0.0f};
// Pad 0 as the game last read it (guest thread; see OnGuestPad). The SDK's
// InputSystem is not thread-safe, so the overlay never polls it itself
// (2026-10-02: doing so from the UI thread crashed in RefreshDevices).
std::atomic<uint32_t> g_pad_buttons{0};
std::atomic<int32_t> g_pad_lx{0}, g_pad_ly{0};
// After the page closes the game stays blind until every button is released,
// so the press that closed it does not reach the game.
std::atomic<bool> g_block_until_release{false};

bool g_want_close = false;
bool g_published_any = false;
std::string g_capture_cvar;   // kKey row waiting for a key (UI thread)
uint16_t g_prev_buttons = 0;

float UiScale(const ImGuiIO& io) {
  const float s = io.DisplaySize.y / 720.0f;
  return s < 1.0f ? 1.0f : s;
}

void ApplyFullscreen() {
  if (g_window == nullptr || g_app_context == nullptr) return;
  const bool want = AsBool(rex::cvar::GetFlagByName("fullscreen"));
  // Resizing from inside a draw must be deferred (see rex/ui/ui_drawer.h).
  g_app_context->CallInUIThreadDeferred([want] {
    if (g_ready.load() && g_window != nullptr && g_window->IsFullscreen() != want) {
      g_window->SetFullscreen(want);
    }
  });
}

void RequestQuit() {
  if (g_window == nullptr || g_app_context == nullptr) return;
  SaveSettings();
  g_app_context->CallInUIThreadDeferred([] {
    if (g_ready.load() && g_window != nullptr) g_window->RequestClose();
  });
}

void SetValue(const Setting& s, const std::string& value) {
  if (!rex::cvar::SetFlagByName(s.cvar, value)) {
    REXLOG_INFO("[settings] {} = {} was rejected", s.cvar, value);
    return;
  }
  if (s.on_change != nullptr) s.on_change();
  SaveSettings();
}

class SettingsPage : public rex::ui::ImGuiDialog {
 public:
  explicit SettingsPage(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const float ui = UiScale(io);
    ImGui::PushFont(g_font, 19.0f * ui);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f * ui, 18.0f * ui));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * ui, 9.0f * ui));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * ui, 5.0f * ui));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * ui);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f * ui);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.06f, 0.08f, 0.94f));

    // Dim the game behind the page.
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize,
                                                  IM_COL32(0, 0, 0, 110));
    // Wide enough for every tab label in one row (seven tabs since the
    // Cheats tab; fixed 108-unit tabs ran off the page), within the screen.
    const auto& tab_names = Sections();
    float widest_tab = 0.0f;
    for (const char* name : tab_names) widest_tab = std::max(widest_tab, ImGui::CalcTextSize(name).x);
    const float tab_spacing = 10.0f * ui;
    const float tabs_width = float(tab_names.size()) * (widest_tab + 24.0f * ui) +
                             float(tab_names.size() - 1) * tab_spacing;
    const float width = std::min(std::max(640.0f * ui, tabs_width + 2.0f * 22.0f * ui),
                                 io.DisplaySize.x - 32.0f * ui);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 0.0f), ImGuiCond_Always);
    if (first_frame_) ImGui::SetNextWindowFocus();
    if (ImGui::Begin("##realsteel_settings", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::PushFont(g_font, 26.0f * ui);
      ImGui::TextUnformatted("SETTINGS");
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.5f, 0.62f, 0.68f, 1.0f), "  ReCompiled Steel %s", realsteel::kVersion);

      // Tabs: click, or LB / RB on the controller, Q / E on the keyboard.
      const auto& sections = Sections();
      const bool popup_open_now =
          ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
      if (!popup_open_now && !ImGui::IsAnyItemActive()) {
        const int count = int(sections.size());
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) ||
            ImGui::IsKeyPressed(ImGuiKey_Q, false)) {
          tab_ = (tab_ + count - 1) % count;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) ||
            ImGui::IsKeyPressed(ImGuiKey_E, false)) {
          tab_ = (tab_ + 1) % count;
        }
      }
      ImGui::Dummy(ImVec2(0.0f, 2.0f * ui));
      const float tab_width =
          (ImGui::GetContentRegionAvail().x - float(sections.size() - 1) * ImGui::GetStyle().ItemSpacing.x) /
          float(sections.size());
      for (int i = 0; i < int(sections.size()); ++i) {
        if (i != 0) ImGui::SameLine();
        const bool current = i == tab_;
        ImGui::PushStyleColor(ImGuiCol_Button,
                              current ? ImVec4(0.16f, 0.45f, 0.62f, 1.0f)
                                      : ImVec4(0.14f, 0.16f, 0.20f, 1.0f));
        // Tabs are reached with LB / RB, not by walking through them.
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        if (ImGui::Button(sections[i], ImVec2(tab_width, 0.0f))) tab_ = i;
        ImGui::PopItemFlag();
        ImGui::PopStyleColor();
      }
      ImGui::Separator();

      const float control_x = 250.0f * ui;
      const char* hovered_help = nullptr;
      int rows = 0, most_rows = 0;
      for (const char* name : sections) {
        int n = 0;
        for (const Setting& s : Settings()) n += std::strcmp(s.section, name) == 0;
        most_rows = n > most_rows ? n : most_rows;
      }
      const int visible_rows = most_rows > 11 ? 11 : most_rows;
      ImGui::BeginChild("##rows", ImVec2(0.0f, ImGui::GetFrameHeightWithSpacing() * visible_rows + 4.0f * ui),
                        ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
      for (const Setting& s : Settings()) {
        if (std::strcmp(s.section, sections[tab_]) != 0) continue;
        ++rows;
        ImGui::PushID(s.cvar[0] != 0 ? s.cvar : s.label);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(s.label);
        ImGui::SameLine(control_x);
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (rows == 1 && focus_first_row_) {
          ImGui::SetKeyboardFocusHere();
          focus_first_row_ = false;
        }
        const std::string current = s.cvar[0] != 0 ? rex::cvar::GetFlagByName(s.cvar) : std::string();
        switch (s.kind) {
          case Kind::kToggle: {
            bool on = AsBool(current);
            if (ImGui::Checkbox("##v", &on)) SetValue(s, on ? "true" : "false");
            break;
          }
          case Kind::kChoice: {
            const char* preview = current.c_str();
            for (const Choice& c : s.choices) {
              if (SameValue(current, c.value)) preview = c.label;
            }
            if (ImGui::BeginCombo("##v", preview)) {
              for (const Choice& c : s.choices) {
                const bool selected = SameValue(current, c.value);
                if (ImGui::Selectable(c.label, selected) && !selected) {
                  SetValue(s, c.value);
                }
                if (selected) ImGui::SetItemDefaultFocus();
              }
              ImGui::EndCombo();
            }
            break;
          }
          case Kind::kText: {
            if (text_cvar_ != s.cvar) {
              text_buffer_ = current;
              text_buffer_.resize(256, '\0');
            }
            ImGuiInputTextFlags flags = ImGuiInputTextFlags_AutoSelectAll;
            if (std::strstr(s.cvar, "password") != nullptr) flags |= ImGuiInputTextFlags_Password;
            ImGui::InputText("##v", text_buffer_.data(), text_buffer_.size(), flags);
            if (ImGui::IsItemActive()) {
              text_cvar_ = s.cvar;
            } else if (text_cvar_ == s.cvar) {
              text_cvar_.clear();
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
              SetValue(s, std::string(text_buffer_.c_str()));
              text_cvar_.clear();
            }
            break;
          }
          case Kind::kKey: {
            const bool capturing = g_capture_cvar == s.cvar;
            std::string label = capturing ? "press a key  (Esc: keep)" : realsteel::keyboard::KeyName(realsteel::keyboard::KeyCode(current));
            if (!capturing && realsteel::keyboard::KeyCode(current) == 0) label = "(none)";
            if (ImGui::Button((label + "##v").c_str(), ImVec2(-FLT_MIN, 0.0f))) g_capture_cvar = s.cvar;
            break;
          }
          case Kind::kInfo: {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(s.info != nullptr ? s.info().c_str() : "");
            break;
          }
          case Kind::kAction: {
            if (ImGui::Button(s.default_value) && s.on_change != nullptr) s.on_change();
            break;
          }
          case Kind::kSlider: {
            float v = float(std::strtod(current.c_str(), nullptr));
            // Wide ranges (HDR nits) in whole numbers, small ones to 2 places.
            const char* format = s.slider_max > 50.0f ? "%.0f" : "%.2f";
            if (ImGui::SliderFloat("##v", &v, s.slider_min, s.slider_max, format,
                                   ImGuiSliderFlags_AlwaysClamp)) {
              char text[32];
              std::snprintf(text, sizeof(text), format, v);
              SetValue(s, text);
            }
            break;
          }
        }
        if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) hovered_help = s.help;
        ImGui::PopID();
      }
      // Every tab is as tall as the fullest one, so the page keeps its size.
      for (; rows < visible_rows; ++rows) {
        ImGui::Dummy(ImVec2(0.0f, ImGui::GetFrameHeight()));
      }
      ImGui::EndChild();
      if (last_tab_ != tab_) {
        last_tab_ = tab_;
        focus_first_row_ = true;  // next frame: the new tab's first control
        quit_armed_ = false;
      }

      ImGui::Dummy(ImVec2(0.0f, 2.0f * ui));
      ImGui::Separator();
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.74f, 0.80f, 1.0f));
      ImGui::PushTextWrapPos(0.0f);
      // Fixed two-line slot so the page does not jump as the focus moves.
      const float help_h = ImGui::GetTextLineHeightWithSpacing() * 2.0f;
      const float help_y = ImGui::GetCursorPosY();
      ImGui::TextUnformatted(hovered_help != nullptr ? hovered_help : " ");
      ImGui::SetCursorPosY(help_y + help_h);
      ImGui::PopTextWrapPos();
      ImGui::PopStyleColor();

      if (ImGui::Button("Reset this tab")) {
        for (const Setting& s : Settings()) {
          if (std::strcmp(s.section, sections[tab_]) == 0 && s.kind != Kind::kInfo && s.kind != Kind::kAction) SetValue(s, s.default_value);
        }
      }
      ImGui::SameLine();
      if (ImGui::Button("Close")) g_want_close = true;
      ImGui::SameLine();
      // Two presses, so a stray click cannot end the game.
      if (ImGui::Button(quit_armed_ ? "Press again to quit" : "Quit game")) {
        if (quit_armed_) {
          RequestQuit();
        } else {
          quit_armed_ = true;
        }
      } else if (quit_armed_ && !ImGui::IsItemHovered() && !ImGui::IsItemFocused()) {
        quit_armed_ = false;
      }
      ImGui::TextDisabled("LB / RB or Q / E: tabs      B or Esc: close      F1 or L3 + R3: open / close");

      const bool popup_open =
          ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId);
      if (!first_frame_ && !popup_open && !ImGui::IsAnyItemActive() &&
          (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
           ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        g_want_close = true;
      }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(5);
    ImGui::PopFont();
    first_frame_ = false;
  }

 private:
  bool first_frame_ = true;
  std::string text_cvar_;      // kText row being edited
  std::string text_buffer_;
  int tab_ = 0;
  int last_tab_ = 0;
  bool focus_first_row_ = false;
  bool quit_armed_ = false;
};

// The on-screen touch controls (realsteel_touch.cpp), drawn under any page.
class TouchControls : public rex::ui::ImGuiDialog {
 public:
  explicit TouchControls(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    realsteel::touch::Draw(ImGui::GetBackgroundDrawList(), g_font, io.DisplaySize.x,
                           io.DisplaySize.y);
  }
};
std::unique_ptr<TouchControls> g_touch_controls;

class FpsCounter : public rex::ui::ImGuiDialog {
 public:
  explicit FpsCounter(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const float ui = UiScale(io);
    ImGui::PushFont(g_font, 17.0f * ui);
    ImGui::SetNextWindowPos(ImVec2(10.0f * ui, 8.0f * ui), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.45f);
    if (ImGui::Begin("##realsteel_fps", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("%.0f fps", double(g_fps.load()));
    }
    ImGui::End();
    ImGui::PopFont();
  }
};

std::unique_ptr<SettingsPage> g_page;

// "Achievement unlocked" pop-up (2026-10-12). The runtime's own toast is
// drawn by its overlay, which never reaches the native renderer, so unlocks
// went unannounced. The UI tick compares the unlocked set every half second
// and queues new ones here; each shows for five seconds, top centre.
class AchievementToast : public rex::ui::ImGuiDialog {
 public:
  explicit AchievementToast(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
  void Push(std::string label, uint32_t gamerscore) {
    queue_.push_back({std::move(label), gamerscore});
  }
  bool Idle() const { return queue_.empty(); }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (queue_.empty()) return;
    const auto now = std::chrono::steady_clock::now();
    if (shown_since_.time_since_epoch().count() == 0) shown_since_ = now;
    const float age = std::chrono::duration<float>(now - shown_since_).count();
    if (age > 5.0f) {
      queue_.erase(queue_.begin());
      shown_since_ = {};
      return;
    }
    const float ui = UiScale(io);
    const float alpha = std::max(0.0f, std::min(1.0f, std::min(age * 4.0f, (5.0f - age) * 2.0f)));
    ImGui::PushFont(g_font, 19.0f * ui);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18.0f * ui, 10.0f * ui));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * ui);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f * ui);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.05f, 0.07f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.8f, 0.85f, 0.9f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, 24.0f * ui), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    if (ImGui::Begin("##realsteel_toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.9f, 1.0f), "ACHIEVEMENT UNLOCKED");
      ImGui::PushFont(g_font, 24.0f * ui);
      ImGui::TextUnformatted(queue_.front().label.c_str());
      ImGui::PopFont();
      ImGui::TextDisabled("%u G", queue_.front().gamerscore);
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(4);
    ImGui::PopFont();
  }

 private:
  struct Item {
    std::string label;
    uint32_t gamerscore;
  };
  std::vector<Item> queue_;
  std::chrono::steady_clock::time_point shown_since_{};
};
std::unique_ptr<AchievementToast> g_toast;

// New unlocks since the last look (the first look only takes stock).
void PollAchievements() {
  static std::chrono::steady_clock::time_point last{};
  static std::set<uint32_t> known;
  static bool primed = false;
  const auto now = std::chrono::steady_clock::now();
  if (now - last < std::chrono::milliseconds(500)) return;
  last = now;
  auto* ks = rex::system::kernel_state();
  if (ks == nullptr) return;
  auto list = ks->achievements().ListAchievements();
  if (list.empty()) list = ks->loaded_achievements();
  if (list.empty()) return;
  for (const auto& a : list) {
    if (!ks->achievements().IsUnlocked(a.id) || known.count(a.id) != 0) continue;
    known.insert(a.id);
    if (!primed) continue;
    REXLOG_INFO("[achievements] unlocked: {} ({} G)", a.label, a.gamerscore);
    if (g_toast == nullptr) g_toast = std::make_unique<AchievementToast>(g_imgui.get());
    g_toast->Push(a.label, a.gamerscore);
  }
  primed = true;
}

// ACHIEVEMENTS from the main menu: the list the console's own screen showed.
class AchievementsPage : public rex::ui::ImGuiDialog {
 public:
  explicit AchievementsPage(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {
    if (auto* ks = rex::system::kernel_state()) {
      list_ = ks->achievements().ListAchievements();
      if (list_.empty()) list_ = ks->loaded_achievements();
      for (const auto& a : list_) {
        const bool unlocked = ks->achievements().IsUnlocked(a.id);
        unlocked_.push_back(unlocked);
        total_score_ += a.gamerscore;
        if (unlocked) {
          ++unlocked_count_;
          score_ += a.gamerscore;
        }
      }
    }
  }
  bool want_close() const { return want_close_; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const float ui = UiScale(io);
    ImGui::PushFont(g_font, 19.0f * ui);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0f * ui, 18.0f * ui));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f * ui, 7.0f * ui));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 6.0f * ui);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.05f, 0.06f, 0.08f, 0.95f));
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 110));
    const float width = std::min(760.0f * ui, io.DisplaySize.x - 32.0f * ui);
    const float height = std::min(560.0f * ui, io.DisplaySize.y - 32.0f * ui);
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    if (first_frame_) ImGui::SetNextWindowFocus();
    if (ImGui::Begin("##realsteel_achievements", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
      ImGui::PushFont(g_font, 26.0f * ui);
      ImGui::TextUnformatted("ACHIEVEMENTS");
      ImGui::PopFont();
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(0.5f, 0.62f, 0.68f, 1.0f), "  %d / %d unlocked, %u / %u G",
                         unlocked_count_, int(list_.size()), score_, total_score_);
      ImGui::Separator();
      ImGui::BeginChild("##list", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_None,
                        ImGuiWindowFlags_NoBackground);
      if (list_.empty()) ImGui::TextDisabled("No achievement data was loaded.");
      for (size_t i = 0; i < list_.size(); ++i) {
        const auto& a = list_[i];
        const bool unlocked = unlocked_[i];
        const ImVec4 title = unlocked ? ImVec4(0.35f, 0.88f, 0.9f, 1.0f) : ImVec4(0.62f, 0.64f, 0.68f, 1.0f);
        ImGui::PushID(int(i));
        // Selectable rows so the stick / D-pad scrolls the list.
        ImGui::Selectable("##row", false, ImGuiSelectableFlags_None,
                          ImVec2(0.0f, ImGui::GetTextLineHeight() * 2.0f + 4.0f * ui));
        ImGui::SameLine(4.0f * ui);
        ImGui::BeginGroup();
        ImGui::TextColored(title, "%s%s", unlocked ? "" : "(locked)  ", a.label.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%u G", a.gamerscore);
        const std::string& text =
            unlocked || a.unachieved_description.empty() ? a.description : a.unachieved_description;
        ImGui::TextDisabled("%s", text.c_str());
        ImGui::EndGroup();
        ImGui::PopID();
      }
      ImGui::EndChild();
      // A button as well as B / Esc: on a touch screen the on-screen pad is
      // hidden while a page is open, so a tap is the only way out.
      if (ImGui::Button("CLOSE")) want_close_ = true;
      ImGui::SameLine();
      ImGui::TextDisabled("(B or Esc)");
      if (!first_frame_ && (ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                            ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))) {
        want_close_ = true;
      }
    }
    ImGui::End();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    first_frame_ = false;
  }

 private:
  std::vector<rex::system::AchievementInfo> list_;
  std::vector<bool> unlocked_;
  int unlocked_count_ = 0;
  uint32_t score_ = 0, total_score_ = 0;
  bool first_frame_ = true;
  bool want_close_ = false;
};
std::unique_ptr<AchievementsPage> g_achievements;
std::function<void()> g_achievements_closed;

void CloseAchievements() {
  if (g_achievements == nullptr) return;
  REXLOG_INFO("[achievements] page closed");
  g_achievements.reset();
  g_page_open.store(g_page != nullptr);
  g_block_until_release.store(true);
  if (g_achievements_closed) {
    auto done = std::move(g_achievements_closed);
    g_achievements_closed = nullptr;
    done();
  }
}
std::unique_ptr<FpsCounter> g_fps_counter;

// ---------------------------------------------------------------------------
// The MULTIPLAYER panel: shown beside the game's own online menus (the game
// polls the sign-in state every frame while it is in them), in the game's
// look. Join address and match password are typed with the keyboard or
// mouse; the game's menu keeps the controller.
// ---------------------------------------------------------------------------
const Setting* FindSetting(const char* cvar) {
  for (const Setting& s : Settings())
    if (std::strcmp(s.cvar, cvar) == 0) return &s;
  return nullptr;
}

class OnlinePanel : public rex::ui::ImGuiDialog {
 public:
  explicit OnlinePanel(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    const float ui = UiScale(io);
    const ImU32 cyan = IM_COL32(120, 230, 240, 255), dim = IM_COL32(170, 190, 200, 255);
    const float w = 450.0f * ui, h = 330.0f * ui;
    const ImVec2 pos(io.DisplaySize.x - w - 40.0f * ui, io.DisplaySize.y * 0.5f - h * 0.5f + 30.0f * ui);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(w, h), ImGuiCond_Always);
    ImGui::PushFont(g_font, 17.0f * ui);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * ui, 12.0f * ui));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f * ui, 7.0f * ui));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(7.0f * ui, 4.0f * ui));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 2.0f * ui);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.03f, 0.10f, 0.13f, 0.95f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0.05f, 0.16f, 0.20f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0.05f, 0.20f, 0.25f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.95f, 0.98f, 1.0f));
    if (ImGui::Begin("##realsteel_online", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
      // the game's panel look: dark steel plate, cyan header rule
      ImDrawList* dl = ImGui::GetWindowDrawList();
      const ImVec2 a = pos, b = ImVec2(pos.x + w, pos.y + h);
      dl->AddRectFilledMultiColor(a, b, IM_COL32(14, 22, 28, 235), IM_COL32(10, 16, 22, 235),
                                  IM_COL32(4, 8, 12, 245), IM_COL32(6, 12, 16, 245));
      dl->AddRect(a, b, IM_COL32(60, 90, 100, 255), 0.0f, 0, 1.5f * ui);
      dl->AddRectFilled(a, ImVec2(b.x, a.y + 30.0f * ui), IM_COL32(8, 30, 40, 255));
      dl->AddLine(ImVec2(a.x, a.y + 30.0f * ui), ImVec2(b.x, a.y + 30.0f * ui), cyan, 1.5f * ui);
      ImGui::PushFont(g_font, 21.0f * ui);
      ImGui::PushStyleColor(ImGuiCol_Text, cyan);
      ImGui::SetCursorPosY(5.0f * ui);
      ImGui::TextUnformatted("MULTIPLAYER");
      ImGui::PopStyleColor();
      ImGui::PopFont();
      ImGui::SetCursorPosY(42.0f * ui);
      const float label_w = 150.0f * ui;
      Row("YOUR NAME", "realsteel_online_name", label_w, false);
      Row("LOBBY SERVER", "realsteel_lobby_server", label_w, false);
      ImGui::TextUnformatted("LOBBY");
      ImGui::SameLine(label_w);
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.47f, 0.90f, 0.94f, 1.0f));
      ImGui::TextUnformatted(realsteel::netplay::LobbyStatus().c_str());
      ImGui::PopStyleColor();
      Row("JOIN ADDRESS", "realsteel_net_peers", label_w, false);
      Row("PASSWORD", "realsteel_net_password", label_w, true);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted("YOUR PORT");
      ImGui::SameLine(label_w);
      ImGui::PushStyleColor(ImGuiCol_Text, cyan);
      ImGui::Text("UDP %u", unsigned(realsteel::netplay::ServicePort()));
      ImGui::PopStyleColor();
      ImGui::PushStyleColor(ImGuiCol_Text, dim);
      ImGui::PushTextWrapPos(0.0f);
      ImGui::TextUnformatted(
          "With a lobby server: HOST MATCH or FIND MATCHES, nothing else to do. Without one: "
          "the host forwards the port and joiners type the host's address. Same password on "
          "both sides.");
      ImGui::PopTextWrapPos();
      ImGui::PopStyleColor();
      const std::string notice = realsteel::netplay::ModsNotice();
      if (!notice.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.35f, 1.0f));
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(notice.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
      }
      realsteel::keyboard::SetTyping(ImGui::IsAnyItemActive());
    }
    ImGui::End();
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(6);
    ImGui::PopFont();
  }

  void Row(const char* label, const char* cvar, float label_w, bool password) {
    const Setting* setting = FindSetting(cvar);
    if (setting == nullptr) return;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(label_w);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(cvar);
    std::string& buffer = buffers_[cvar];
    if (active_ != cvar) {
      buffer = rex::cvar::GetFlagByName(cvar);
      buffer.resize(256, '\0');
    }
    ImGui::InputText("##v", buffer.data(), buffer.size(),
                     ImGuiInputTextFlags_AutoSelectAll | (password ? ImGuiInputTextFlags_Password : 0));
    if (ImGui::IsItemActive()) active_ = cvar;
    else if (active_ == cvar) active_.clear();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
      SetValue(*setting, std::string(buffer.c_str()));
      active_.clear();
    }
    ImGui::PopID();
  }

  std::string active_;
  std::map<std::string, std::string> buffers_;
};
std::unique_ptr<OnlinePanel> g_online_panel;

// True while the game is on its MULTIPLAYER menu: there it checks the
// online privilege several hundred times a second (nowhere else does).
// Hysteresis so a stray check at the title screen does not flash the panel.
bool OnlineMenusOpen() {
  static uint64_t last_polls = 0;
  static auto last_time = std::chrono::steady_clock::now();
  static double rate = 0.0;    // polls per second, smoothed
  static bool open = false;
  const uint64_t polls = realsteel::online::SigninPolls();
  const auto now = std::chrono::steady_clock::now();
  const double dt = std::chrono::duration<double>(now - last_time).count();
  if (dt >= 0.1) {
    const double sample = double(polls - last_polls) / dt;
    rate = rate * 0.5 + sample * 0.5;
    last_polls = polls;
    last_time = now;
  }
  if (!realsteel::online::Enabled()) return false;
  if (!open && rate > 200.0) open = true;
  if (open && rate < 40.0) open = false;
  return open;
}

void SetPageOpen(bool open) {
  if (g_imgui == nullptr || open == (g_page != nullptr)) return;
  if (open) {
    g_page = std::make_unique<SettingsPage>(g_imgui.get());
  } else {
    g_page.reset();
    SaveSettings();
    g_block_until_release.store(true);
  }
  g_want_close = false;
  g_page_open.store(open);
}

// F1 toggles the page. Registered above the ImGui drawer so it also closes
// the page while ImGui has the keyboard.
class KeyListener : public rex::ui::WindowInputListener {
 public:
  void OnKeyDown(rex::ui::KeyEvent& e) override {
    const uint16_t vk = uint16_t(e.virtual_key());
    if (!g_capture_cvar.empty()) {        // a key row is being rebound
      if (vk != uint16_t(rex::ui::VirtualKey::kEscape) && !e.prev_state()) {
        rex::cvar::SetFlagByName(g_capture_cvar, realsteel::keyboard::KeyName(vk));
        realsteel::keyboard::ReloadMapping();
        SaveSettings();
      }
      g_capture_cvar.clear();
      e.set_handled(true);
      return;
    }
    realsteel::keyboard::OnKey(vk, true);
    if (vk == uint16_t(rex::ui::VirtualKey::kEscape) && g_achievements != nullptr && !e.prev_state()) {
      CloseAchievements();
      e.set_handled(true);
      return;
    }
    if (vk == uint16_t(rex::ui::VirtualKey::kEscape) && g_page != nullptr && !e.prev_state()) {
      SetPageOpen(false);
      e.set_handled(true);
      return;
    }
    if (e.virtual_key() == rex::ui::VirtualKey::kF12 && !e.prev_state()) {
      realsteel::native_present::RequestScreenshot();
      e.set_handled(true);
      return;
    }
    // F11: fullscreen / windowed at any time, kept for the next start.
    if (e.virtual_key() == rex::ui::VirtualKey::kF11 && !e.prev_state()) {
      const bool fullscreen = AsBool(rex::cvar::GetFlagByName("fullscreen"));
      rex::cvar::SetFlagByName("fullscreen", fullscreen ? "false" : "true");
      ApplyFullscreen();
      SaveSettings();
      e.set_handled(true);
      return;
    }
    if (e.virtual_key() != rex::ui::VirtualKey::kF1 || e.prev_state()) return;
    SetPageOpen(g_page == nullptr);
    e.set_handled(true);
  }
  void OnKeyUp(rex::ui::KeyEvent& e) override {
    realsteel::keyboard::OnKey(uint16_t(e.virtual_key()), false);
  }
};
KeyListener g_key_listener;

// "Mute in background": the game's own mute is left alone; ours is lifted
// again when the window comes back.
class FocusListener : public rex::ui::WindowListener {
 public:
  void OnLostFocus(rex::ui::UISetupEvent&) override {
    if (!REXCVAR_GET(realsteel_mute_background)) return;
    if (rex::cvar::GetFlagByName("audio_mute") == "true") return;  // the player's own mute
    rex::cvar::SetFlagByName("audio_mute", "true");
    muted_by_us_ = true;
  }
  void OnGotFocus(rex::ui::UISetupEvent&) override {
    if (!muted_by_us_) return;
    muted_by_us_ = false;
    rex::cvar::SetFlagByName("audio_mute", "false");
  }

 private:
  bool muted_by_us_ = false;
};
FocusListener g_focus_listener;

// Controller: L3 + R3 toggles; while the page is open the pad drives ImGui's
// navigation (and is hidden from the game by the input gate).
void PollController() {
  const uint16_t buttons = uint16_t(g_pad_buttons.load());
  const int lx = g_pad_lx.load(), ly = g_pad_ly.load();
  const uint16_t kCombo = rex::input::X_INPUT_GAMEPAD_LEFT_THUMB |
                          rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB;
  if ((buttons & kCombo) == kCombo && (g_prev_buttons & kCombo) != kCombo) {
    SetPageOpen(g_page == nullptr);
  }
  g_prev_buttons = buttons;
  if ((g_page == nullptr && g_achievements == nullptr) || g_imgui == nullptr) return;
  ImGuiIO& io = g_imgui->GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad |
                    ImGuiConfigFlags_NavEnableKeyboard;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  const int kStick = 16000;
  io.AddKeyEvent(ImGuiKey_GamepadDpadUp,
                 (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_UP) || ly > kStick);
  io.AddKeyEvent(ImGuiKey_GamepadDpadDown,
                 (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_DOWN) || ly < -kStick);
  io.AddKeyEvent(ImGuiKey_GamepadDpadLeft,
                 (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_LEFT) || lx < -kStick);
  io.AddKeyEvent(ImGuiKey_GamepadDpadRight,
                 (buttons & rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT) || lx > kStick);
  io.AddKeyEvent(ImGuiKey_GamepadL1,
                 (buttons & rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadR1,
                 (buttons & rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (buttons & rex::input::X_INPUT_GAMEPAD_A) != 0);
  io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (buttons & rex::input::X_INPUT_GAMEPAD_B) != 0);
}

void UiTick() {
  g_tick_pending.store(false);
  if (!g_ready.load() || g_imgui == nullptr || g_window == nullptr) return;
  PollController();
  static bool opened_at_start = false;
  if (!opened_at_start && REXCVAR_GET(realsteel_settings_open)) {
    opened_at_start = true;
    SetPageOpen(true);
  }
  const bool want_online = OnlineMenusOpen() && g_page == nullptr;
  if (want_online && g_online_panel == nullptr) {
    g_online_panel = std::make_unique<OnlinePanel>(g_imgui.get());
    REXLOG_INFO("[settings] multiplayer panel shown");
  } else if (!want_online && g_online_panel != nullptr) {
    REXLOG_INFO("[settings] multiplayer panel hidden");
    g_online_panel.reset();
    realsteel::keyboard::SetTyping(false);
  }
  {
    const float tw = float(g_window->GetActualPhysicalWidth());
    const float th = float(g_window->GetActualPhysicalHeight());
    realsteel::touch::Tick(tw, th, g_page != nullptr || g_achievements != nullptr);
    const bool want_touch = realsteel::touch::Visible();
    if (want_touch && g_touch_controls == nullptr) {
      g_touch_controls = std::make_unique<TouchControls>(g_imgui.get());
    } else if (!want_touch && g_touch_controls != nullptr) {
      g_touch_controls.reset();
    }
    if (realsteel::touch::TakeSettingsTap()) SetPageOpen(true);
  }
  PollAchievements();
  TickTabStrip();
  if (g_toast != nullptr && g_toast->Idle()) g_toast.reset();
  const bool want_fps = REXCVAR_GET(realsteel_show_fps);
  if (want_fps && g_fps_counter == nullptr) {
    g_fps_counter = std::make_unique<FpsCounter>(g_imgui.get());
  } else if (!want_fps && g_fps_counter != nullptr) {
    g_fps_counter.reset();
  }
  const uint32_t w = g_window->GetActualPhysicalWidth();
  const uint32_t h = g_window->GetActualPhysicalHeight();
  if (!g_imgui->HasDialogs() || w == 0 || h == 0) {
    if (g_published_any) {
      Publish(nullptr);
      g_published_any = false;
    }
    return;
  }
  rex::ui::AppUIDrawContext context(w, h);
  g_imgui->Draw(context);
  g_published_any = true;
  if (g_want_close) SetPageOpen(false);
  if (g_achievements != nullptr && g_achievements->want_close()) CloseAchievements();
  PollTextEntry();
}

void SetupFont(ImFontAtlas* atlas) {
  // A readable UI face where the system has one; the built-in ImGui font
  // otherwise (scaled, so a little soft).
  static const char* const kCandidates[] = {
      "C:/Windows/Fonts/segoeui.ttf",
      "/usr/share/fonts/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
      "/usr/share/fonts/TTF/DejaVuSans.ttf",
      "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
      // Android (2026-10-11: without these the pages used ImGui's pixel font).
      "/system/fonts/RobotoStatic-Regular.ttf",
      "/system/fonts/Roboto-Regular.ttf",
      "/system/fonts/DroidSans.ttf",
  };
  for (const char* path : kCandidates) {
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) continue;
    g_font = atlas->AddFontFromFileTTF(path, 30.0f);
    if (g_font != nullptr) return;
  }
}

}  // namespace

void LoadSettingsFile();

void LoadSavedSettings() {
  LoadSettingsFile();
  // Anti-aliasing was two settings (MSAA, FXAA) until 2026-10-12.
  if (rex::cvar::GetFlagByName("realsteel_native_aa").empty()) {
    const bool msaa = AsBool(rex::cvar::GetFlagByName("realsteel_native_msaa"));
    const bool fxaa = AsBool(rex::cvar::GetFlagByName("realsteel_native_fxaa"));
    rex::cvar::SetFlagByName("realsteel_native_aa", msaa ? (fxaa ? "msaa_fxaa" : "msaa")
                                                         : (fxaa ? "fxaa" : "off"));
    SaveSettings();
  }
  // Online identity: random on first run, then kept. (Deriving it from the
  // machine gave every Steam Deck the same one, and players with the same
  // identity cannot play each other.)
  if (rex::cvar::GetFlagByName("realsteel_online_xuid").empty()) {
    std::random_device random;
    const uint64_t value = (uint64_t(random()) << 32 | random()) & 0x0000FFFFFFFFFFFFull;
    char text[17];
    std::snprintf(text, sizeof(text), "%016llX",
                  static_cast<unsigned long long>(value | 0x0009000000000000ull));
    rex::cvar::SetFlagByName("realsteel_online_xuid", text);
    SaveSettings();
  }
}

void LoadSettingsFile() {
  FILE* f = std::fopen(kSettingsFile, "r");
  if (f == nullptr) return;
  char line[256];
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    std::string text(line);
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                             text.back() == ' ')) {
      text.pop_back();
    }
    const size_t eq = text.find('=');
    if (text.empty() || text[0] == '#' || eq == std::string::npos) continue;
    const std::string name = text.substr(0, eq);
    const std::string value = text.substr(eq + 1);
    // Only settings the page owns, and never over an explicit command line.
    bool known = false;
    for (const Setting& s : Settings()) known = known || (s.kind != Kind::kInfo && s.kind != Kind::kAction && name == s.cvar);
    for (const char* stored : kStoredOnly) known = known || name == stored;
    for (const char* legacy : kLegacy) known = known || name == legacy;
    if (!known) continue;
    if (rex::cvar::GetFlagSource(name) >= rex::cvar::Source::kEnvironment) continue;
    rex::cvar::SetFlagByName(name, value);
  }
  std::fclose(f);
}

void Init(rex::ui::Window* window, rex::ui::WindowedAppContext* app_context,
          rex::Runtime* runtime) {
  if (g_ready.load() || window == nullptr || app_context == nullptr) return;
  g_window = window;
  g_app_context = app_context;
  g_drawer = std::make_unique<RecordingDrawer>();
  g_imgui = std::make_unique<rex::ui::ImGuiDrawer>(window, 64, &SetupFont);
  g_imgui->SetPresenterAndImmediateDrawer(nullptr, g_drawer.get());
  window->AddInputListener(&g_key_listener, 128);
  window->AddListener(&g_focus_listener);
  window->SetTitle("ReCompiled Steel");
  window->SetIcon(kRealSteelIcon, kRealSteelIconSize);
  (void)runtime;
  g_ready.store(true);
  REXLOG_INFO("[settings] overlay ready (F1 or L3+R3 opens the settings page)");
}

void Shutdown() {
  if (!g_ready.exchange(false)) return;
  g_page_open.store(false);
  g_page.reset();
  g_online_panel.reset();
  g_fps_counter.reset();
  g_toast.reset();
  DropTextEntry();
  g_touch_controls.reset();
  g_achievements.reset();
  if (g_window != nullptr) g_window->RemoveInputListener(&g_key_listener);
  g_imgui.reset();
  g_drawer.reset();
  g_window = nullptr;
  Publish(nullptr);
}

bool VibrationEnabled() { return REXCVAR_GET(realsteel_vibration); }

void QuitGame() { RequestQuit(); }

bool ShowAchievements(std::function<void()> on_closed) {
  if (!g_ready.load() || g_app_context == nullptr) return false;
  return g_app_context->CallInUIThreadDeferred([done = std::move(on_closed)]() mutable {
    if (g_imgui == nullptr) {
      if (done) done();
      return;
    }
    if (g_achievements != nullptr) CloseAchievements();
    g_achievements_closed = std::move(done);
    g_achievements = std::make_unique<AchievementsPage>(g_imgui.get());
    g_page_open.store(true);  // the game does not see the pad meanwhile
  });
}

// ---------------------------------------------------------------------------
// The settings as pages of the game's OPTIONS panel (2026-10-12). The panel
// hooks live in realsteel_native_settings.cpp; this side turns the settings
// table into pages of six rows and applies what the panel does to them.
// ---------------------------------------------------------------------------
namespace {

struct NativePage {
  std::string title;
  std::string section;  // upper case: the tab it belongs to
  std::vector<const Setting*> rows;
};

const std::vector<NativePage>& NativePages() {
  static const std::vector<NativePage> pages = [] {
    std::vector<NativePage> out;
    for (const char* section : Sections()) {
      std::vector<const Setting*> rows;
      for (const Setting& s : Settings()) {
#if defined(__ANDROID__)
        if (s.kind == Kind::kKey) continue;  // no keyboard to rebind
#endif
        if (std::strcmp(s.section, section) == 0) rows.push_back(&s);
      }
      if (rows.empty()) continue;
      const size_t count = (rows.size() + 5) / 6;
      for (size_t p = 0; p < count; ++p) {
        NativePage page;
        page.title = section;
        for (char& c : page.title) c = char(std::toupper(uint8_t(c)));
        page.section = page.title;
        if (count > 1) page.title += "  " + std::to_string(p + 1) + "/" + std::to_string(count);
        for (size_t r = p * 6; r < rows.size() && r < p * 6 + 6; ++r) page.rows.push_back(rows[r]);
        out.push_back(std::move(page));
      }
    }
    return out;
  }();
  return pages;
}

const Setting* NativeSetting(int page, int row) {
  const auto& pages = NativePages();
  if (page < 1 || page > int(pages.size()) || row < 0) return nullptr;
  const NativePage& p = pages[size_t(page - 1)];
  return row < int(p.rows.size()) ? p.rows[size_t(row)] : nullptr;
}

std::string Upper(std::string text) {
  for (char& c : text) c = char(std::toupper(uint8_t(c)));
  return text;
}

// Tabs over the game's OPTIONS panel (2026-10-12): the panel shows one page
// with nothing saying more follow, so a strip above its title names every
// tab, marks the current one and shows the LB / RB buttons that page. A
// click or tap on a tab opens it (realsteel_native_settings.cpp takes the
// request on the panel's next frame).
std::atomic<int> g_panel_page{0};
std::atomic<bool> g_panel_achievements{false};
std::atomic<int64_t> g_panel_seen_ns{0};
std::atomic<int> g_panel_request{-1};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct Tab {
  std::string name;
  int first_page;  // 0: the game's own options
  int last_page;
};

const std::vector<Tab>& Tabs() {
  static const std::vector<Tab> tabs = [] {
    std::vector<Tab> out{{"OPTIONS", 0, 0}};
    const auto& pages = NativePages();
    for (size_t i = 0; i < pages.size(); ++i) {
      const int page = int(i) + 1;
      if (out.back().name == pages[i].section) {
        out.back().last_page = page;
      } else {
        out.push_back({pages[i].section, page, page});
      }
    }
    return out;
  }();
  return tabs;
}

class TabStrip : public rex::ui::ImGuiDialog {
 public:
  explicit TabStrip(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
    float band[4];
    realsteel::native_present::BandRect(io.DisplaySize.x, io.DisplaySize.y, band);
    const float k = band[2] / 1280.0f;  // guest pixels -> window pixels
    // The panel's frame spans x 523..1117 with its title bar from y 88
    // (guest pixels); the strip sits just above it.
    const float x0 = band[0] + 523.0f * k, x1 = band[0] + 1117.0f * k;
    const float y0 = band[1] + 52.0f * k, y1 = band[1] + 84.0f * k;
    const int page = g_panel_page.load();
    const auto& tabs = Tabs();
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    // Text size: the largest up to 17 guest pixels that fits the tabs.
    const float badge_w = 36.0f * k, gap = 4.0f * k, pad = 8.0f * k;
    float size = 17.0f * k;
    auto total = [&](float sz) {
      float w = 2.0f * (badge_w + gap);
      for (const Tab& t : tabs) {
        w += g_font->CalcTextSizeA(sz, FLT_MAX, 0.0f, t.name.c_str()).x + 2.0f * pad + gap;
      }
      return w - gap;
    };
    while (size > 9.0f * k && total(size) > x1 - x0) size -= 0.5f * k;
    const float spare = std::max(0.0f, (x1 - x0) - total(size));
    const float extra = spare / float(tabs.size());  // tabs share the rest
    const ImU32 frame = IM_COL32(120, 128, 136, 200);
    const ImU32 fill = IM_COL32(6, 10, 14, 215);
    const ImU32 lit = IM_COL32(28, 104, 110, 240);
    const ImU32 lit_edge = IM_COL32(70, 215, 225, 255);
    const float round = 3.0f * k;
    const bool click = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
    const ImVec2 mouse = io.MousePos;
    auto hit = [&](float a, float b) {
      return click && mouse.x >= a && mouse.x < b && mouse.y >= y0 && mouse.y < y1;
    };
    // LB / RB badges, as the game draws its button glyphs.
    auto badge = [&](float x, const char* text) {
      const float by0 = y0 + 4.0f * k, by1 = y1 - 4.0f * k;
      dl->AddRectFilled(ImVec2(x, by0), ImVec2(x + badge_w, by1), IM_COL32(225, 228, 232, 255),
                        2.0f * k);
      const float ts = 15.0f * k;
      const ImVec2 sz = g_font->CalcTextSizeA(ts, FLT_MAX, 0.0f, text);
      const ImVec2 at(x + (badge_w - sz.x) * 0.5f, (by0 + by1 - sz.y) * 0.5f);
      for (float dx : {0.0f, 0.7f * k}) {  // twice, a hair apart: bolder
        dl->AddText(g_font, ts, ImVec2(at.x + dx, at.y), IM_COL32(10, 12, 14, 255), text);
      }
    };
    const int pages = int(NativePages().size()) + 1;
    float x = x0;
    badge(x, "LB");
    if (hit(x, x + badge_w)) g_panel_request.store((page + pages - 1) % pages);
    x += badge_w + gap;
    for (const Tab& t : tabs) {
      const float tw = g_font->CalcTextSizeA(size, FLT_MAX, 0.0f, t.name.c_str()).x;
      const float w = tw + 2.0f * pad + extra;
      const bool current = page >= t.first_page && page <= t.last_page;
      dl->AddRectFilled(ImVec2(x, y0), ImVec2(x + w, y1), current ? lit : fill, round);
      dl->AddRect(ImVec2(x, y0), ImVec2(x + w, y1), current ? lit_edge : frame, round, 0,
                  std::max(1.0f, (current ? 1.5f : 1.0f) * k));
      dl->AddText(g_font, size, ImVec2(x + (w - tw) * 0.5f, (y0 + y1 - size) * 0.5f),
                  current ? IM_COL32(255, 255, 255, 255) : IM_COL32(165, 172, 178, 255),
                  t.name.c_str());
      if (hit(x, x + w)) g_panel_request.store(t.first_page);
      x += w + gap;
    }
    badge(x, "RB");
    if (hit(x, x + badge_w)) g_panel_request.store((page + 1) % pages);
  }
};
std::unique_ptr<TabStrip> g_tab_strip;

void TickTabStrip() {
  // The panel's draw hook reports every frame; a gap means it has closed.
  const bool shown = NowNs() - g_panel_seen_ns.load() < 150000000 &&
                     !g_panel_achievements.load() && g_page == nullptr;
  if (shown && g_tab_strip == nullptr) {
    g_tab_strip = std::make_unique<TabStrip>(g_imgui.get());
  } else if (!shown && g_tab_strip != nullptr) {
    g_tab_strip.reset();
  }
}

// "4:3 (Retroid Pocket Nova)" -> "4:3": the value column is narrow.
std::string ShortChoice(const char* label) {
  std::string text = label;
  const size_t paren = text.find(" (");
  if (paren != std::string::npos) text.resize(paren);
  return Upper(text);
}

// The first sentence of a help text, for the game's one-line help bar.
std::string FirstSentence(const char* help) {
  std::string text = help != nullptr ? help : "";
  const size_t stop = text.find(". ");
  if (stop != std::string::npos) text.resize(stop + 1);
  if (text.size() > 96) text = text.substr(0, 93) + "...";
  return text;
}

// Sliders as spinner steps: about 50 at most, in round numbers.
float SliderStep(const Setting& s) {
  static const float kSteps[] = {0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f,
                                 10.0f, 20.0f, 50.0f, 100.0f};
  const float want = (s.slider_max - s.slider_min) / 50.0f;
  for (float step : kSteps) {
    if (step >= want) return step;
  }
  return 100.0f;
}

// Text rows (name, lobby server, join address, password): the panel has no
// text entry, so a small box opens over it.
class TextEntry : public rex::ui::ImGuiDialog {
 public:
  TextEntry(rex::ui::ImGuiDrawer* drawer, const Setting* setting)
      : ImGuiDialog(drawer), setting_(setting) {
    buffer_ = rex::cvar::GetFlagByName(setting->cvar);
    buffer_.resize(256, '\0');
  }
  bool done() const { return done_; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    const float ui = UiScale(io);
    ImGui::PushFont(g_font, 20.0f * ui);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f * ui, 16.0f * ui));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 4.0f * ui);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f * ui);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.03f, 0.05f, 0.07f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.2f, 0.8f, 0.85f, 0.9f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(620.0f * ui, io.DisplaySize.x - 32.0f * ui), 0.0f));
    if (ImGui::Begin("##realsteel_text_entry", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextColored(ImVec4(0.35f, 0.88f, 0.9f, 1.0f), "%s", Upper(setting_->label).c_str());
      ImGui::TextDisabled("%s", FirstSentence(setting_->help).c_str());
      ImGui::SetNextItemWidth(-FLT_MIN);
      if (first_) ImGui::SetKeyboardFocusHere();
      ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue;
      if (std::strstr(setting_->cvar, "password") != nullptr) flags |= ImGuiInputTextFlags_Password;
      const bool enter = ImGui::InputText("##text", buffer_.data(), buffer_.size(), flags);
      const bool ok = ImGui::Button("OK");
      ImGui::SameLine();
      const bool cancel = ImGui::Button("CANCEL") ||
                          (!first_ && ImGui::IsKeyPressed(ImGuiKey_Escape, false));
      if (enter || ok) {
        SetValue(*setting_, std::string(buffer_.c_str()));
        done_ = true;
      } else if (cancel) {
        done_ = true;
      }
    }
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(3);
    ImGui::PopFont();
    first_ = false;
  }

 private:
  const Setting* setting_;
  std::string buffer_;
  bool first_ = true;
  bool done_ = false;
};
std::unique_ptr<TextEntry> g_text_entry;

void PollTextEntry() {
  if (g_text_entry != nullptr && g_text_entry->done()) {
    g_text_entry.reset();
    g_text_entry_open.store(false);
    g_block_until_release.store(true);  // the confirming press stays with us
  }
}

void DropTextEntry() {
  g_text_entry.reset();
  g_text_entry_open.store(false);
}

}  // namespace

namespace native {

int PageCount() { return int(NativePages().size()); }

void PanelShown(int page, bool achievements) {
  g_panel_page.store(page);
  g_panel_achievements.store(achievements);
  g_panel_seen_ns.store(NowNs());
}

int TakePageRequest() { return g_panel_request.exchange(-1); }

std::string PageTitle(int page) {
  const auto& pages = NativePages();
  return page >= 1 && page <= int(pages.size()) ? pages[size_t(page - 1)].title : std::string();
}

RowView Row(int page, int row) {
  RowView view;
  const Setting* s = NativeSetting(page, row);
  if (s == nullptr) return view;
  view.active = true;
  view.label = Upper(s->label);
  view.help = FirstSentence(s->help);
  const std::string current = s->cvar[0] != 0 ? rex::cvar::GetFlagByName(s->cvar) : std::string();
  switch (s->kind) {
    case Kind::kToggle:
      view.count = 2;
      view.index = AsBool(current) ? 1 : 0;
      view.wrap = true;
      view.value = view.index ? "ON" : "OFF";
      break;
    case Kind::kChoice: {
      view.count = uint32_t(s->choices.size());
      view.value = Upper(current);
      for (size_t i = 0; i < s->choices.size(); ++i) {
        if (SameValue(current, s->choices[i].value)) {
          view.index = uint32_t(i);
          view.value = ShortChoice(s->choices[i].label);
        }
      }
      break;
    }
    case Kind::kSlider: {
      const float step = SliderStep(*s);
      const float v = float(std::strtod(current.c_str(), nullptr));
      view.count = uint32_t(std::lround((s->slider_max - s->slider_min) / step)) + 1;
      const long i = std::lround((v - s->slider_min) / step);
      view.index = uint32_t(std::max(0L, std::min(long(view.count) - 1, i)));
      char text[32];
      std::snprintf(text, sizeof(text), s->slider_max > 50.0f ? "%.0f" : "%.2f", v);
      view.value = text;
      break;
    }
    case Kind::kText:
      view.value = current.empty() ? "-" : std::strstr(s->cvar, "password") != nullptr
                                               ? std::string(std::min<size_t>(current.size(), 12), '*')
                                               : Upper(current);
      view.help = "Press right to edit.  " + view.help;
      break;
    case Kind::kKey: {
      const uint16_t code = realsteel::keyboard::KeyCode(current);
      view.value = code == 0 ? "(NONE)" : Upper(realsteel::keyboard::KeyName(code));
      if (g_capture_cvar == s->cvar) view.value = "PRESS A KEY";
      view.help = "Press right, then the new key (Esc keeps the old one).";
      break;
    }
    case Kind::kInfo:
      view.value = s->info != nullptr ? Upper(s->info()) : std::string();
      break;
    case Kind::kAction:
      view.value = Upper(s->default_value);
      view.help = "Press right: " + view.help;
      break;
  }
  if (std::strcmp(s->cvar, "realsteel_native_resolution") == 0) {
    // The size actually rendered: the height follows the picture's shape
    // (720p is 1280x960 on the Retroid Pocket Nova, 1280x800 on a Deck).
    unsigned w = 0, h = 0;
    const float aspect = realsteel::native_present::CurrentPictureAspect();
    if (std::sscanf(current.c_str(), "%ux%u", &w, &h) == 2 && w != 0 && h != 0) {
      if (aspect > 0.0f && aspect < 16.0f / 9.0f) {
        h = (unsigned(float(w) / aspect + 0.5f) + 1u) & ~1u;
      } else if (aspect > 16.0f / 9.0f) {
        w = (unsigned(float(h) * aspect + 0.5f) + 1u) & ~1u;
      }
      view.value += "  " + std::to_string(w) + "X" + std::to_string(h);
    }
  }
  if (view.value.size() > 24) view.value = view.value.substr(0, 22) + "..";
  return view;
}

void Change(int page, int row, uint32_t index) {
  const Setting* s = NativeSetting(page, row);
  if (s == nullptr || s->cvar[0] == 0) return;
  switch (s->kind) {
    case Kind::kToggle:
      SetValue(*s, index ? "true" : "false");
      break;
    case Kind::kChoice:
      if (index < s->choices.size()) SetValue(*s, s->choices[index].value);
      break;
    case Kind::kSlider: {
      const float v = std::min(s->slider_max, s->slider_min + float(index) * SliderStep(*s));
      char text[32];
      std::snprintf(text, sizeof(text), s->slider_max > 50.0f ? "%.0f" : "%.2f", v);
      SetValue(*s, text);
      break;
    }
    default:
      break;
  }
}

void Press(int page, int row, int direction) {
  const Setting* s = NativeSetting(page, row);
  if (s == nullptr || g_app_context == nullptr) return;
  if (s->kind == Kind::kAction && direction > 0) {
    if (s->on_change != nullptr) s->on_change();
  } else if (s->kind == Kind::kKey) {
    // A second press gives up waiting for the key.
    const std::string cvar = s->cvar;
    g_app_context->CallInUIThreadDeferred([cvar] {
      g_capture_cvar = g_capture_cvar == cvar ? std::string() : cvar;
    });
  } else if (s->kind == Kind::kText && direction > 0) {
    g_text_entry_open.store(true);
    g_app_context->CallInUIThreadDeferred([s] {
      if (g_imgui != nullptr && g_text_entry == nullptr) {
        g_text_entry = std::make_unique<TextEntry>(g_imgui.get(), s);
      }
    });
  }
}

void ResetPage(int page) {
  const auto& pages = NativePages();
  if (page < 1 || page > int(pages.size())) return;
  for (const Setting* s : pages[size_t(page - 1)].rows) {
    if (s->cvar[0] == 0 || s->kind == Kind::kInfo || s->kind == Kind::kAction || s->kind == Kind::kText) {
      continue;
    }
    SetValue(*s, s->default_value);
  }
}

uint16_t PadButtons() { return uint16_t(g_pad_buttons.load()); }
int PadStickX() { return g_pad_lx.load(); }
bool EditingText() { return g_text_entry_open.load(); }

namespace {
struct AchievementLine {
  std::string label, help;
  uint32_t gamerscore = 0;
  bool unlocked = false;
};
std::vector<AchievementLine> AchievementLines() {
  std::vector<AchievementLine> out;
  auto* ks = rex::system::kernel_state();
  if (ks == nullptr) return out;
  auto list = ks->achievements().ListAchievements();
  if (list.empty()) list = ks->loaded_achievements();
  for (const auto& a : list) {
    AchievementLine line;
    line.label = Upper(a.label);
    line.unlocked = ks->achievements().IsUnlocked(a.id);
    line.gamerscore = a.gamerscore;
    line.help = line.unlocked || a.unachieved_description.empty() ? a.description : a.unachieved_description;
    out.push_back(std::move(line));
  }
  return out;
}
}  // namespace

int AchievementPageCount() { return std::max(1, int((AchievementLines().size() + 5) / 6)); }

std::string AchievementPageTitle(int page) {
  const auto lines = AchievementLines();
  int unlocked = 0;
  uint32_t score = 0, total = 0;
  for (const auto& l : lines) {
    unlocked += l.unlocked;
    total += l.gamerscore;
    if (l.unlocked) score += l.gamerscore;
  }
  const int pages = AchievementPageCount();
  std::string title = "ACHIEVEMENTS";
  if (pages > 1) title += "  " + std::to_string(page) + "/" + std::to_string(pages);
  title += "   " + std::to_string(unlocked) + "/" + std::to_string(lines.size()) + "  " +
           std::to_string(score) + "/" + std::to_string(total) + "G";
  return title;
}

RowView AchievementRow(int page, int row) {
  RowView view;
  const auto lines = AchievementLines();
  const size_t i = size_t(page - 1) * 6 + size_t(row);
  if (page < 1 || row < 0 || row >= 6 || i >= lines.size()) return view;
  const AchievementLine& l = lines[i];
  view.active = true;
  view.label = l.label;
  view.value = (l.unlocked ? "UNLOCKED  " : "LOCKED  ") + std::to_string(l.gamerscore) + "G";
  view.help = l.help;
  if (view.help.size() > 96) view.help = view.help.substr(0, 93) + "...";
  return view;
}

}  // namespace native

bool OnGuestPad(uint16_t buttons, int16_t thumb_lx, int16_t thumb_ly) {
  g_pad_buttons.store(buttons);
  g_pad_lx.store(thumb_lx);
  g_pad_ly.store(thumb_ly);
  if (g_page_open.load() || g_text_entry_open.load()) return true;
  if (g_block_until_release.load()) {
    if (buttons != 0) return true;
    g_block_until_release.store(false);
  }
  return false;
}

void Pump(rs::vk::Renderer& renderer, double fps) {
  g_fps.store(float(fps));
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (rs::vk::OverlayTextureOp& op : g_texture_ops) {
      renderer.QueueOverlayTexture(std::move(op));
    }
    g_texture_ops.clear();
    if (g_frame_dirty) {
      renderer.SetOverlay(g_frame);
      g_frame_dirty = false;
    }
  }
  if (!g_ready.load() || g_app_context == nullptr) return;
  if (!g_tick_pending.exchange(true)) {
    if (!g_app_context->CallInUIThreadDeferred(&UiTick)) g_tick_pending.store(false);
  }
}

}  // namespace realsteel::overlay
