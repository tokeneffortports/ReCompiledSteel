#include "vk_backend.h"
#include "present_spv.h"

#include "shader_library.h"
#include "texture_decode.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <volk.h>
#ifdef _WIN32
#include <windows.h>  // volk only forward-declares HWND/HINSTANCE
#endif

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <atomic>
#include <thread>
#include <unordered_set>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

namespace rs::vk {
namespace {

using rs::capture::CommandRecord;
using rs::capture::DecodedTexture;
using rs::capture::DrawKind;
using rs::capture::DrawRecord;
using rs::capture::FrameData;
using rs::capture::kInvalidPayloadOffset;
using rs::capture::LoadBE32;

constexpr uint32_t kWidth = 1280;
constexpr uint32_t kHeight = 720;
// Attachments are allocated taller than the frame: the title's 1024x1024
// shadow-map pass renders into the same EDRAM-backed surfaces, and a
// 720-row attachment cropped its bottom 304 rows (2026-09-06).
constexpr uint32_t kAttachW = 1280;
constexpr uint32_t kAttachH = 1024;
constexpr uint32_t kMaxTextures = 1024;
constexpr uint32_t kMaxCubeTextures = 128;  // cube heap (set 2); set 1 (3D) same size
// Upload buffers (2026-10-15): 512 MB per frame slot had been reserved -
// 1 GB of a phone's RAM - for frames that use about 17 MB (startup, menus,
// fights on the PC). They start at 128 MB and double, up to the old size,
// when a frame used more than half (see RenderFrame).
constexpr VkDeviceSize kArenaStartBytes = 128ull * 1024 * 1024;
constexpr VkDeviceSize kArenaMaxBytes = 512ull * 1024 * 1024;
// Shader constants (VS bank, PS bank, shared block per draw: ~9 KB) live in
// their own storage buffer, read by the shaders at 32-bit offsets
// (XenosRecomp's shader_common.h as patched, set 4). Until 2026-10-11 they were read through
// 64-bit buffer device addresses, which need shaderInt64 - absent on Adreno.
constexpr VkDeviceSize kConstBytes = 32ull * 1024 * 1024;

// SharedConstants layout v2 (shader_common.h of the XenosRecomp fork): 20 descriptor
// index slots per section, bools, misc.
constexpr uint32_t kSharedBytes = 448;  // +16 fetch swizzles at 384 (templates)
constexpr uint32_t kSharedViewportScale = 368;
constexpr uint32_t kShared2DBase = 0;
constexpr uint32_t kSharedCubeBase = 160;
constexpr uint32_t kSharedSamplerBase = 240;
constexpr uint32_t kSharedSwappedTexcoords = 352;

#define VK_CHECK(expr)                                              \
  do {                                                              \
    const VkResult vk_check_result = (expr);                        \
    if (vk_check_result != VK_SUCCESS) {                            \
      std::printf("[vk] %s failed: %d (line %d)\n", #expr,          \
                  int(vk_check_result), __LINE__);                  \
      std::fflush(stdout);                                          \
      return false;                                                 \
    }                                                               \
  } while (0)

// D3D DeclUsage -> the fixed vk::location table the recompiler emits
// (USAGE_LOCATIONS in shader_recompiler.cpp).
int LocationFor(uint32_t usage, uint32_t usage_index) {
  struct Row { uint32_t usage, index, location; };
  static constexpr Row rows[] = {
      {0, 0, 0},  {3, 0, 1},  {6, 0, 2},  {7, 0, 3},  {5, 0, 4},
      {5, 1, 5},  {5, 2, 6},  {5, 3, 7},  {10, 0, 8}, {2, 0, 9},
      {1, 0, 10}, {10, 1, 11}, {5, 4, 12}, {5, 5, 13}, {5, 6, 14},
      {5, 7, 15}, {0, 1, 15},
  };
  for (const Row& r : rows) {
    if (r.usage == usage && r.index == usage_index) return int(r.location);
  }
  return -1;
}

// Xenos vertex format -> VkFormat (+ dword count for bounds checks).
// |flags|: bit1 signedRfMode, bit2 integer (numFormatAll), bit3 signed
VkFormat FormatFor(uint32_t xenos_format, uint32_t flags, uint32_t* dwords) {
// (formatCompAll) — straight from the vfetch instruction. Float formats
// ignore the number-format bits, as on Xenos.
  const bool is_signed = (flags & 8) != 0;
  const bool is_int = (flags & 4) != 0;
  *dwords = 1;
  switch (xenos_format) {
    case 6:  // k_8_8_8_8
      if (is_int) return is_signed ? VK_FORMAT_R8G8B8A8_SINT : VK_FORMAT_R8G8B8A8_UINT;
      return is_signed ? VK_FORMAT_R8G8B8A8_SNORM : VK_FORMAT_R8G8B8A8_UNORM;
    case 7:  // k_2_10_10_10
      return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 16:  // k_10_11_11
    case 17:  // k_11_11_10 (decoded in-shader via tfetchR11G11B10)
      return VK_FORMAT_R32_UINT;
    case 25:  // k_16_16
      if (is_int) return is_signed ? VK_FORMAT_R16G16_SINT : VK_FORMAT_R16G16_UINT;
      return is_signed ? VK_FORMAT_R16G16_SNORM : VK_FORMAT_R16G16_UNORM;
    case 26:  // k_16_16_16_16
      *dwords = 2;
      if (is_int) return is_signed ? VK_FORMAT_R16G16B16A16_SINT : VK_FORMAT_R16G16B16A16_UINT;
      return is_signed ? VK_FORMAT_R16G16B16A16_SNORM : VK_FORMAT_R16G16B16A16_UNORM;
    case 31:  // k_16_16_FLOAT
      return VK_FORMAT_R16G16_SFLOAT;
    case 32:  // k_16_16_16_16_FLOAT
      *dwords = 2;
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 33: return is_signed ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT;
    case 34: *dwords = 2; return is_signed ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT;
    case 36: return VK_FORMAT_R32_SFLOAT;
    case 37: *dwords = 2; return VK_FORMAT_R32G32_SFLOAT;
    case 57: *dwords = 3; return VK_FORMAT_R32G32B32_SFLOAT;
    case 38: *dwords = 4; return VK_FORMAT_R32G32B32A32_SFLOAT;
    default:
      return VK_FORMAT_UNDEFINED;
  }
}

bool Is16BitTexcoordFormat(uint32_t xenos_format) {
  return xenos_format == 25 || xenos_format == 26 || xenos_format == 31 ||
         xenos_format == 32;
}

bool AllocImage(VkDevice device, VkPhysicalDevice phys, uint32_t width,
                uint32_t height, VkFormat format, VkImageUsageFlags usage,
                VkImage* image, VkDeviceMemory* memory, uint32_t layers = 1,
                uint32_t mip_levels = 1,
                VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);

struct PipelineKey {
  // Shader content (ShaderInfo::cache_hash), never the guest VA: the live
  // path sees one address hold different shaders over a session.
  uint64_t vs_hash, ps_hash;
  uint32_t topology;
  uint32_t stride;
  uint32_t blend;  // 1 = src-alpha/inv-src-alpha, 0 = opaque (pre-v12 guess)
  uint32_t depth;  // 1 = test+write LE (pre-v12 guess), 2 = always
  // v12+ captured state (rs_valid = 1): Xenos encodings straight from the
  // device shadow — see DrawRecord::rs.
  uint32_t rs_valid = 0;
  uint32_t blend_ctl = 0;     // RB_BLENDCONTROL (0 = blending off)
  uint32_t depth_ctl = 0;     // RB_DEPTHCONTROL bits 1 (test), 2 (write), 4-6 (func)
  uint32_t alpha_test = 0;    // 1 = enabled (func GE/GT)
  uint32_t cull = 0;          // PA_SU_SC_MODE_CNTL bits 0-2
  uint32_t samples = 1;       // rasterization samples (v14: RB_SURFACE_INFO msaa)
  bool operator<(const PipelineKey& o) const {
    return std::memcmp(this, &o, sizeof(o)) < 0;
  }
};

// Picture shapes other than 16:9 and the title's 2D. Menus, HUD, fades and
// flashes are inline quads whose position is in guest pixels; the vertex
// shader maps them to clip space with c0 = (2/W, 2/H, 0, 0) (VS 405BE2A6
// textured, 7C3B2686 flat colour). Pixel2DBox finds such a draw's pixel
// bounds from its vertices (positions as 32-bit floats, guest big-endian).
bool Pixel2DBox(const ShaderInfo& vs, const uint8_t* vertices, uint32_t bytes,
                uint32_t stride, uint32_t count, const float* vs_consts, uint32_t vp_w,
                uint32_t vp_h, float box[4]) {
  if (vertices == nullptr || stride == 0 || count == 0 || vp_w == 0 || vp_h == 0) {
    return false;
  }
  const float* c0 = vs_consts;
  if (std::fabs(c0[0] * float(vp_w) - 2.0f) > 0.01f ||
      std::fabs(c0[1] * float(vp_h) - 2.0f) > 0.01f || c0[2] != 0.0f || c0[3] != 0.0f) {
    return false;
  }
  const ShaderVertexFetch* pos = nullptr;
  for (const ShaderVertexFetch& f : vs.fetches) {
    if (f.usage == 0 && f.usageIndex == 0 && (f.flags & 1) == 0) pos = &f;
  }
  // Xenos k_32_32_FLOAT, k_32_32_32_32_FLOAT, k_32_32_32_FLOAT.
  if (pos == nullptr || (pos->format != 37 && pos->format != 38 && pos->format != 57)) {
    return false;
  }
  box[0] = box[1] = 1e30f;
  box[2] = box[3] = -1e30f;
  for (uint32_t v = 0; v < count && (v + 1) * stride <= bytes; ++v) {
    const uint8_t* p = vertices + v * stride + pos->offset * 4;
    float x, y;
    const uint32_t ux = LoadBE32(p), uy = LoadBE32(p + 4);
    std::memcpy(&x, &ux, 4);
    std::memcpy(&y, &uy, 4);
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    box[0] = std::min(box[0], x);
    box[1] = std::min(box[1], y);
    box[2] = std::max(box[2], x);
    box[3] = std::max(box[3], y);
  }
  return box[0] <= box[2];
}

// Where a 2D draw with pixel box |box| (x0, y0, x1, y1 in a W x H target)
// goes in a picture of relative shape |rel| (picture aspect / (16/9)), as
// clip-space scale and offset (vps, see the draw loop):
// - covers the whole screen (backdrops, fades, flashes): scaled evenly until
//   it covers the whole picture (the ends of the long side are cut) - unless
//   it draws a copy of the screen (|screen_copy|: blurs, wipes), which must
//   fill the picture exactly, as the picture is what the copy holds;
// - spans the full width in a wider picture, or the full height in a taller
//   one (bars, side panels): stretched along that side;
// - a full-width bar in the bottom sixth (the menus' help bar), and what is
//   drawn in the bottom sixth after it in this frame (the bar's other layers,
//   prompts, help text): moved down with the bottom of the picture. Dialogs
//   draw a black underlay first and the metal bar, starting higher, after
//   it. |bar_top| is where anchoring starts once a bar was seen (start each
//   frame at a large value). Without a bar, elements near the bottom stay
//   put: a loading box and its text must move together.
// - on screens without 3D (|backdrops|): a quad that bleeds past an edge of
//   the screen is a backdrop tile (the title's backdrop is several): scaled
//   evenly about the centre like a whole-screen quad, so the tiles still
//   meet and together cover the picture.
// - anything else stays in the central 16:9 band, as the 3D's squeeze.
void Layout2D(const float box[4], float w, float h, float rel, bool backdrops,
              bool screen_copy, float& bar_top, float vps[4]) {
  const float slack = 2.0f;  // the title's quads carry -0.5 pixel offsets
  const bool full_w = box[0] <= slack && box[2] >= w - slack;
  const bool full_h = box[1] <= slack && box[3] >= h - slack;
  if (full_w && full_h) {
    vps[0] = screen_copy ? 1.0f : rel < 1.0f ? 1.0f / rel : 1.0f;
    vps[1] = screen_copy ? 1.0f : rel > 1.0f ? rel : 1.0f;
    return;
  }
  if (rel > 1.0f && full_w) vps[0] = 1.0f;
  if (rel < 1.0f && full_h) vps[1] = 1.0f;
  if (rel < 1.0f && full_w && !full_h && box[1] >= h * 0.82f) {
    bar_top = h * 0.82f;
  }
  if (rel < 1.0f && !full_h && box[1] >= bar_top - 0.5f) {
    vps[3] = -(1.0f - rel);  // clip +y is the top
    return;
  }
  const float bleed = 8.0f;
  if (backdrops && (box[0] < -bleed || box[1] < -bleed || box[2] > w + bleed ||
                    box[3] > h + bleed)) {
    vps[0] = rel < 1.0f ? 1.0f / rel : 1.0f;
    vps[1] = rel > 1.0f ? rel : 1.0f;
  }
}

class Backend {
 public:
  bool Run(const FrameData& frame, const RenderOptions& options);
  bool Init(const RenderOptions& options);
  bool RenderFrame(const FrameData& frame, std::vector<uint8_t>& rgba,
                   bool want_rgba = true);
  ~Backend();

 private:
  bool InitVulkan(bool validation);
  bool CreateTargets();
  bool CreateDescriptors();
  uint32_t UploadTexture(uint32_t phys, uint64_t hash = 0);  // heap index; 0 = white fallback
  // Uploads a decoded 2D (set 0) or 6-layer cube (set 2) image; returns its
  // index in that heap.
  uint32_t UploadDecoded(uint32_t phys, const DecodedTexture& tex);
  void EnsureCubeFallback();               // cube heap index 0 = white
  VkPipeline GetPipeline(const PipelineKey& key, const ShaderInfo* vs,
                         const ShaderInfo* ps);
  VkDeviceSize Alloc(VkDeviceSize bytes, VkDeviceSize align);

 public:
  bool WriteBmp(const std::string& path, const std::vector<uint8_t>& rgba);
  const RenderOptions& options() const { return options_; }

 private:
  RenderOptions options_;
  bool initialized_ = false;
  // Window presentation (--vkwindow).
  bool CreateWindowSurface();
  bool CreateSwapchain();
  void DestroySwapchain();
  void PumpMessages();
  bool window_open_ = false;
#ifdef _WIN32
  HWND hwnd_ = nullptr;
#endif
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  std::vector<VkImage> sc_images_;
  // Present pass (2026-10-02): the output is copied into |present_src_| and
  // drawn to the swapchain image through a tiny shader that scales it and
  // applies the output tone curve (a plain blit could not).
  std::vector<VkImageView> sc_views_;
  VkImage present_src_ = VK_NULL_HANDLE;
  VkDeviceMemory present_src_mem_ = VK_NULL_HANDLE;
  VkImageView present_src_view_ = VK_NULL_HANDLE;
  VkSampler present_sampler_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout present_dsl_ = VK_NULL_HANDLE;
  VkDescriptorPool present_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet present_set_ = VK_NULL_HANDLE;
  VkPipelineLayout present_layout_ = VK_NULL_HANDLE;
  VkPipeline present_pipeline_ = VK_NULL_HANDLE;
  // Ambient occlusion pass (RunAmbientOcclusion).
  bool EnsureAoResources(VkFormat color_format);
  VkDescriptorSetLayout ao_dsl_ = VK_NULL_HANDLE;
  VkDescriptorPool ao_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet ao_set_ = VK_NULL_HANDLE;
  VkPipelineLayout ao_layout_ = VK_NULL_HANDLE;
  VkPipeline ao_pipeline_ = VK_NULL_HANDLE;
  VkPipeline ao_blur_pipeline_ = VK_NULL_HANDLE;
  VkImage ao_image_ = VK_NULL_HANDLE;  // the occlusion, R8
  VkDeviceMemory ao_image_mem_ = VK_NULL_HANDLE;
  VkImageView ao_image_view_ = VK_NULL_HANDLE;
  uint32_t ao_image_w_ = 0, ao_image_h_ = 0;
  VkFormat ao_pipeline_format_ = VK_FORMAT_UNDEFINED;
  VkImageView ao_bound_view_ = VK_NULL_HANDLE;
  float camera_fov_y_ = 0.64f, camera_near_ = 1.0f, camera_far_ = 30000.0f;
  VkFormat present_pipeline_format_ = VK_FORMAT_UNDEFINED;
  bool present_src_written_ = false;  // present_set_ points at present_src_
  bool EnsurePresentResources();
  // UI overlay (see OverlayFrame in the header).
  struct OverlayTexture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
  };
  std::unordered_map<uint32_t, OverlayTexture> overlay_textures_;
  std::vector<OverlayTextureOp> overlay_ops_;
  std::shared_ptr<const OverlayFrame> overlay_;
  VkDescriptorPool overlay_pool_ = VK_NULL_HANDLE;
  VkPipelineLayout overlay_layout_ = VK_NULL_HANDLE;
  VkPipeline overlay_pipeline_ = VK_NULL_HANDLE;
  VkFormat overlay_pipeline_format_ = VK_FORMAT_UNDEFINED;
  bool EnsureOverlayPipeline();
  void ProcessOverlayOps();
  void DestroyOverlayTexture(OverlayTexture& t);
  void DrawOverlay();  // inside the present pass
  // SetLiveOptions; applied at the next frame start.
  uint32_t pending_w_ = kWidth, pending_h_ = kHeight;
  bool frame_is_scene_ = false;  // this frame runs the 3D scene composite
  uint32_t shot_w_ = 1280, shot_h_ = 720;  // last readback's size
  bool draw_list_once_ = false;            // log the next frame's draws
  // GPU pass timing (ProfileGpu): two timestamps per pass, per frame slot.
  struct GpuPassInfo {
    uint32_t w, h, draws, gap_index;
  };
  static constexpr uint32_t kGpuQueries = 1024;
  VkQueryPool gpu_pool_[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
  uint32_t gpu_used_[2] = {0, 0};
  std::vector<GpuPassInfo> gpu_info_[2];
  uint32_t gpu_profile_frames_ = 0;    // frames still to record
  uint32_t gpu_profile_done_ = 0;      // frames accumulated
  double gpu_ms_per_tick_ = 0.0;
  std::map<uint32_t, std::pair<double, GpuPassInfo>> gpu_pass_sum_;  // pass index -> ms
  double gpu_frame_sum_ = 0.0, gpu_gap_sum_ = 0.0;
  void GpuStamp(VkPipelineStageFlagBits stage) {
    if (gpu_pool_[slot_] == VK_NULL_HANDLE || gpu_used_[slot_] >= kGpuQueries) return;
    vkCmdWriteTimestamp(cmd_, stage, gpu_pool_[slot_], gpu_used_[slot_]++);
  }
  void GpuCollect();
  float bottom_bar_top_ = 1e30f;           // Layout2D: this frame's help bar
  // Per-frame cost of first uses (logged for slow frames): textures fetched
  // and decoded from guest memory, uploads, pipelines built.
  uint32_t stat_tex_n_ = 0, stat_pipe_n_ = 0;
  // Pipeline cache (RenderOptions::pipeline_cache_path).
  VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
  uint32_t pipelines_unsaved_ = 0;
  std::chrono::steady_clock::time_point last_pipeline_built_{};
  void CreatePipelineCache();
  void SavePipelineCache();
  uint64_t stat_tex_fetch_ns_ = 0, stat_tex_up_ns_ = 0, stat_pipe_ns_ = 0;
  // Textures new to the frame, decoded in parallel before the pre-pass
  // (PrefetchTextures); UploadTexture takes them from here.
  struct Prefetched {
    bool ok = false;
    DecodedTexture tex;
  };
  std::unordered_map<uint64_t, Prefetched> prefetched_;
  void PrefetchTextures(const FrameData& frame);
  // Device-local staging for depth -> R32F copies (2026-10-02: staging them in
  // the host-visible arena sent every copy across the bus and back - 13 ms a
  // frame at 2x, 2/3 of the GPU time). Sized for the largest surface at rs_.
  VkBuffer depth_stage_ = VK_NULL_HANDLE;
  VkDeviceMemory depth_stage_mem_ = VK_NULL_HANDLE;
  bool EnsureDepthStage();
  void DestroyDepthStage() {
    if (depth_stage_) vkDestroyBuffer(device_, depth_stage_, nullptr);
    if (depth_stage_mem_) vkFreeMemory(device_, depth_stage_mem_, nullptr);
    depth_stage_ = VK_NULL_HANDLE;
    depth_stage_mem_ = VK_NULL_HANDLE;
  }
  // 1280x720 target the scaled output is filtered into for readbacks.
  VkImage readback_img_ = VK_NULL_HANDLE;
  VkDeviceMemory readback_img_mem_ = VK_NULL_HANDLE;
  // Full-size captures (2026-10-12, trailers and screenshots): the frame at
  // the internal resolution with the output tone curve, instead of the
  // 1280x720 test readback.
  bool capture_full_ = false;
  bool capture_used_ = false;  // this frame's readback went to capture_buf_
  VkBuffer capture_buf_ = VK_NULL_HANDLE;
  VkDeviceMemory capture_mem_ = VK_NULL_HANDLE;
  VkDeviceSize capture_bytes_ = 0;
  bool EnsureCaptureBuffer(VkDeviceSize bytes) {
    if (capture_buf_ != VK_NULL_HANDLE && capture_bytes_ >= bytes) return true;
    if (capture_buf_ != VK_NULL_HANDLE) {
      vkDeviceWaitIdle(device_);
      vkDestroyBuffer(device_, capture_buf_, nullptr);
      vkFreeMemory(device_, capture_mem_, nullptr);
      capture_buf_ = VK_NULL_HANDLE;
    }
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(device_, &bci, nullptr, &capture_buf_) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, capture_buf_, &req);
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(phys_, &mem_props);
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
      if ((req.memoryTypeBits & (1u << i)) &&
          (mem_props.memoryTypes[i].propertyFlags & want) == want) {
        type = i;
        break;
      }
    }
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (type == UINT32_MAX || vkAllocateMemory(device_, &mai, nullptr, &capture_mem_) != VK_SUCCESS) {
      vkDestroyBuffer(device_, capture_buf_, nullptr);
      capture_buf_ = VK_NULL_HANDLE;
      return false;
    }
    vkBindBufferMemory(device_, capture_buf_, capture_mem_, 0);
    capture_bytes_ = bytes;
    return true;
  }
  bool EnsureReadbackImage() {
    if (readback_img_ != VK_NULL_HANDLE) return true;
    return AllocImage(device_, phys_, kWidth, kHeight, VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      &readback_img_, &readback_img_mem_);
  }
  VkFormat sc_format_ = VK_FORMAT_B8G8R8A8_UNORM;
  uint32_t hdr_mode_ = 0;            // 0 SDR, 1 HDR10 (PQ), 2 scRGB
  bool surface_lost_ = false;         // VK_ERROR_SURFACE_LOST_KHR seen
  bool have_colorspace_ext_ = false;  // VK_EXT_swapchain_colorspace
  bool have_hdr_metadata_ = false;    // VK_EXT_hdr_metadata
  VkExtent2D sc_extent_ = {0, 0};
  VkSemaphore acquire_sem_ = VK_NULL_HANDLE;
  VkSemaphore render_sem_ = VK_NULL_HANDLE;
  bool swapchain_dirty_ = false;
  bool sc_extent_from_owner_ = false;  // surface has no size of its own
  // Android pre-rotation (2026-10-15): sc_extent_ stays the window's own
  // (landscape) size for every layout decision; the swapchain images are
  // sc_image_extent_, turned sc_quarter_turns_ x 90 degrees clockwise to the
  // panel's native orientation, and everything drawn into them is turned the
  // same way (MapRect, the rotation in the present and overlay shaders).
  VkExtent2D sc_image_extent_ = {0, 0};
  uint32_t sc_quarter_turns_ = 0;
  VkSurfaceTransformFlagBitsKHR sc_transform_ = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
  VkRect2D MapRect(float x, float y, float w, float h) const {
    const float W = float(sc_extent_.width), H = float(sc_extent_.height);
    float nx = x, ny = y, nw = w, nh = h;
    switch (sc_quarter_turns_) {
      case 1: nx = H - (y + h); ny = x; nw = h; nh = w; break;
      case 2: nx = W - (x + w); ny = H - (y + h); break;
      case 3: nx = y; ny = W - (x + w); nw = h; nh = w; break;
      default: break;
    }
    return {{int32_t(nx + 0.5f), int32_t(ny + 0.5f)}, {uint32_t(nw + 0.5f), uint32_t(nh + 0.5f)}};
  }
  VkViewport MapViewport(float x, float y, float w, float h) const {
    const float W = float(sc_extent_.width), H = float(sc_extent_.height);
    switch (sc_quarter_turns_) {
      case 1: return {H - (y + h), x, h, w, 0.0f, 1.0f};
      case 2: return {W - (x + w), H - (y + h), w, h, 0.0f, 1.0f};
      case 3: return {y, W - (x + w), h, w, 0.0f, 1.0f};
      default: return {x, y, w, h, 0.0f, 1.0f};
    }
  }
  void QuarterTurn(float rot[2]) const {
    static const float kCos[4] = {1.0f, 0.0f, -1.0f, 0.0f};
    static const float kSin[4] = {0.0f, 1.0f, 0.0f, -1.0f};
    rot[0] = kCos[sc_quarter_turns_ & 3];
    rot[1] = kSin[sc_quarter_turns_ & 3];
  }
  bool texture_reload_ = false;  // SetLowEnd changed the texture size setting
  // Measurement only: "rs_exp_skip" lists shader ids (the first 8 hex digits
  // drawlist.trigger prints); draws using one are not drawn.
  std::unordered_set<uint32_t> exp_skip_;
#if defined(__ANDROID__)
  // On a display that is turned from its natural orientation (a phone held
  // in landscape) Android gives currentExtent at the window's size and
  // currentTransform as the turn. We present unrotated (identity) at that
  // size and the compositor turns it; using currentTransform as preTransform
  // would ask us to draw the picture turned ourselves. SUBOPTIMAL (transform
  // != identity) is then expected rather than a reason to rebuild, so size
  // changes are polled instead.
  static VkExtent2D WindowExtent(const VkSurfaceCapabilitiesKHR& caps) {
    return caps.currentExtent;
  }
  static constexpr bool kRebuildOnSuboptimal = false;
#else
  static constexpr bool kRebuildOnSuboptimal = true;
#endif

 public:
  bool WindowOpen() const { return window_open_; }
  // Resolution scale (RenderOptions::resolution_scale). RtEntry / ResolveDst
  // sizes and all viewport maths stay in guest pixels; Vulkan extents are
  // multiplied by this.
  // Internal resolution: the size the guest's 1280x720 frame is rendered at
  // (any size - 2560x1440, or a panel's own 1280x800). RtEntry / ResolveDst
  // sizes and all viewport maths stay in guest pixels; SW / SH convert a
  // guest x / y to Vulkan pixels.
  uint32_t out_w_ = kWidth, out_h_ = kHeight;
  uint32_t SW(uint32_t x) const { return (x * out_w_ + kWidth / 2) / kWidth; }
  uint32_t SH(uint32_t y) const { return (y * out_h_ + kHeight / 2) / kHeight; }
  bool Unscaled() const { return out_w_ == kWidth && out_h_ == kHeight; }
  static void ClampInternal(uint32_t& w, uint32_t& h) {
    w = std::min(5120u, std::max(320u, w));
    h = std::min(3840u, std::max(180u, h));
  }
  void SetDumpPassesDir(const std::string& dir) { options_.dump_passes_dir = dir; }
  void SetPostFx(bool fxaa, bool hdr, float peak, float paper) {
    options_.fxaa = fxaa;
    options_.hdr_peak_nits = std::min(10000.0f, std::max(100.0f, peak));
    options_.hdr_paper_nits = std::min(options_.hdr_peak_nits, std::max(80.0f, paper));
    if (hdr != options_.hdr) {
      options_.hdr = hdr;
      swapchain_dirty_ = true;
    }
  }
  bool HdrActive() const { return hdr_mode_ != 0; }
  void SetFullCapture(bool on) { capture_full_ = on; }
  void ReplaceAndroidWindow(void* native_window) {
    if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
    DestroySwapchain();
    if (surface_ != VK_NULL_HANDLE) {
      vkDestroySurfaceKHR(instance_, surface_, nullptr);
      surface_ = VK_NULL_HANDLE;
    }
    surface_lost_ = false;
    options_.android_window = native_window;
    if (native_window != nullptr && CreateWindowSurface()) {
      swapchain_dirty_ = true;
      present_pipeline_format_ = VK_FORMAT_UNDEFINED;  // rebuilt for the new format
      overlay_pipeline_format_ = VK_FORMAT_UNDEFINED;
    }
    std::printf("[vk] Android window %s\n", native_window != nullptr ? "replaced" : "dropped (background)");
    std::fflush(stdout);
  }
  bool SurfaceLost() const { return surface_lost_; }
  void SetPrerotate(bool on) {
    if (on != options_.prerotate) {
      options_.prerotate = on;
      swapchain_dirty_ = true;
    }
  }
  void ReloadTextures() { texture_reload_ = true; }  // e.g. a texture pack switched on / off
  void SetAmbientOcclusion(float strength) { options_.ao_strength = std::max(0.0f, std::min(1.0f, strength)); }
  void SetCamera(float fov_y, float near_z, float far_z) {
    if (fov_y > 0.05f && fov_y < 3.0f && near_z > 0.0f && far_z > near_z) {
      camera_fov_y_ = fov_y;
      camera_near_ = near_z;
      camera_far_ = far_z;
    }
  }
  void SetLowEnd(bool crowd, bool half_textures) {
    options_.no_crowd = !crowd;
    if (half_textures != options_.half_textures) {
      options_.half_textures = half_textures;
      texture_reload_ = true;  // the heap is rebuilt at the next frame
    }
  }
  void SetQuality(bool msaa, uint32_t anisotropy, float sharpen) {
    options_.no_msaa = !msaa;
    options_.anisotropy = std::min(16u, std::max(1u, anisotropy));
    options_.sharpen = sharpen;
  }
  void SetVsync(bool vsync) {
    if (vsync != options_.vsync) {
      options_.vsync = vsync;
      swapchain_dirty_ = true;
    }
  }
  void SetPictureAspect(float aspect) { options_.picture_aspect = aspect; }
  void SetAspectMask2D(bool mask) { options_.aspect_mask_2d = mask; }
  float WindowAspect() const {
    return sc_extent_.height != 0 ? float(sc_extent_.width) / float(sc_extent_.height)
                                  : 0.0f;
  }
  void DrawListOnce() { draw_list_once_ = true; }
  void ProfileGpu(uint32_t frames) {
    gpu_profile_frames_ = frames;
    gpu_profile_done_ = 0;
    gpu_pass_sum_.clear();
    gpu_frame_sum_ = gpu_gap_sum_ = 0.0;
  }
  void LastReadbackSize(uint32_t& w, uint32_t& h) const {
    w = shot_w_;
    h = shot_h_;
  }
  void SetLiveOptions(uint32_t width, uint32_t height, bool stretch, float gamma,
                      bool no_dof) {
    ClampInternal(width, height);
    pending_w_ = width;
    pending_h_ = height;
    options_.present_stretch = stretch;
    options_.output_gamma = gamma;
    options_.no_dof = no_dof;
  }
  void QueueOverlayTexture(OverlayTextureOp op) { overlay_ops_.push_back(std::move(op)); }
  void SetOverlay(std::shared_ptr<const OverlayFrame> frame) { overlay_ = std::move(frame); }

 private:
  ShaderLibrary shaders_;
  std::unordered_map<uint32_t, std::string> texture_paths_;
  std::unordered_map<uint32_t, std::string> texture_uploaded_path_;  // phys -> dump uploaded
  std::unordered_map<uint64_t, uint32_t> texture_index_by_key_;  // (phys<<32)^hash -> heap (source mode)
  std::unordered_map<uint64_t, std::string> texture_paths_by_hash_;  // (phys<<32)^hash
  bool gamma_ramp_valid_ = false;   // v14 type-204 record seen
  float gamma_ramp_[3][256] = {};   // per-channel display ramp, 0..1
  std::unordered_map<uint32_t, uint32_t> texture_indices_;  // 2D heap
  std::unordered_map<uint32_t, uint32_t> cube_indices_;     // cube heap
  uint32_t cube_count_ = 0;

  VkInstance instance_ = VK_NULL_HANDLE;
  VkPhysicalDevice phys_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  uint32_t queue_family_ = 0;
  VkQueue queue_ = VK_NULL_HANDLE;
  VkCommandPool pool_ = VK_NULL_HANDLE;
  VkCommandBuffer cmd_ = VK_NULL_HANDLE;
  // Two frames in the pipe (2026-10-03): frame N+1 is recorded while the GPU
  // still executes frame N, instead of waiting idle after every submit (on
  // the Steam Deck: record 7 ms + GPU 13 ms in series = 20 ms a frame, and a
  // GPU idle half the time that its governor then clocked down). Each slot
  // owns what a frame writes while the other may be in flight: command
  // buffer, upload arena, fence, swapchain semaphores. cmd_ / arena_* /
  // *_sem_ alias the current slot. Anything that destroys or rewrites
  // objects an in-flight frame may use calls WaitFlight() first.
  struct FrameSlot {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkBuffer arena = VK_NULL_HANDLE;
    VkDeviceMemory arena_mem = VK_NULL_HANDLE;
    uint8_t* arena_ptr = nullptr;
    VkDeviceAddress arena_addr = 0;
    VkBuffer cbuf = VK_NULL_HANDLE;
    VkDeviceMemory cbuf_mem = VK_NULL_HANDLE;
    uint8_t* cbuf_ptr = nullptr;
    VkDescriptorSet cset = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkSemaphore acquire_sem = VK_NULL_HANDLE;
    VkSemaphore render_sem = VK_NULL_HANDLE;
    bool in_flight = false;
  };
  FrameSlot slots_[2];
  uint32_t slot_ = 0;
  void UseSlot(uint32_t i) {
    slot_ = i;
    cmd_ = slots_[i].cmd;
    arena_ = slots_[i].arena;
    arena_mem_ = slots_[i].arena_mem;
    arena_ptr_ = slots_[i].arena_ptr;
    arena_addr_ = slots_[i].arena_addr;
    cbuf_ptr_ = slots_[i].cbuf_ptr;
    cset_ = slots_[i].cset;
    acquire_sem_ = slots_[i].acquire_sem;
    render_sem_ = slots_[i].render_sem;
  }
  // Blocks until no submitted frame is executing.
  void WaitFlight() {
    for (FrameSlot& slot : slots_) {
      if (!slot.in_flight) continue;
      const auto t0 = std::chrono::steady_clock::now();
      vkWaitForFences(device_, 1, &slot.fence, VK_TRUE, UINT64_MAX);
      if (recording_) {  // a stall in the middle of recording a frame
        stall_ns_ += uint64_t((std::chrono::steady_clock::now() - t0).count());
        ++stall_count_;
      }
      slot.in_flight = false;
    }
  }
  bool recording_ = false;  // between the start of a frame's record and its submit
  uint64_t stall_ns_ = 0, stall_count_ = 0;

  VkBuffer arena_ = VK_NULL_HANDLE;
  VkDeviceMemory arena_mem_ = VK_NULL_HANDLE;
  uint8_t* arena_ptr_ = nullptr;
  VkDeviceAddress arena_addr_ = 0;
  VkDeviceSize arena_used_ = 0;
  VkDeviceSize arena_bytes_ = kArenaStartBytes;  // each slot's upload buffer
  bool arena_grow_ = false;  // a frame ran out: grow before the next one
  bool CreateArena(FrameSlot& slot);
  void DestroyArena(FrameSlot& slot);
  VkDeviceSize arena_peak_ = 0, arena_peak_ever_ = 0;  // per log window / since start
  uint8_t* cbuf_ptr_ = nullptr;          // the current slot's constants buffer
  VkDeviceSize cbuf_used_ = 0;
  VkDescriptorSet cset_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout const_layout_ = VK_NULL_HANDLE;
  VkDeviceSize AllocConst(VkDeviceSize bytes);

  VkImage color_ = VK_NULL_HANDLE;
  VkDeviceMemory color_mem_ = VK_NULL_HANDLE;
  VkImageView color_view_ = VK_NULL_HANDLE;
  VkImage depth_ = VK_NULL_HANDLE;
  VkDeviceMemory depth_mem_ = VK_NULL_HANDLE;
  VkImageView depth_view_ = VK_NULL_HANDLE;
  VkBuffer readback_ = VK_NULL_HANDLE;
  VkDeviceMemory readback_mem_ = VK_NULL_HANDLE;

  VkDescriptorSetLayout set_layouts_[4] = {};
  VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet sets_[4] = {};
  VkSampler sampler_ = VK_NULL_HANDLE;
  VkSampler extra_samplers_[8] = {};  // sampler set entries 1-8 (CreateTextureSampler)
  float max_anisotropy_ = 1.0f;       // device limit; 1 = unsupported
  uint32_t sampler_anisotropy_ = 1;   // what sampler_ was created with
  bool CreateTextureSampler();
  uint32_t texture_count_ = 0;
  std::vector<VkImage> images_;
  std::vector<VkDeviceMemory> image_mems_;
  std::vector<VkImageView> image_views_;

  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  std::map<PipelineKey, VkPipeline> pipelines_;
  // Keyed by ShaderInfo::cache_hash, not guest VA: one VA can resolve to
  // several recompiled variants (stride re-patches, see ShaderLibrary).
  std::unordered_map<uint64_t, VkShaderModule> modules_;

  // Render-target pool for multi-pass replay: one color+depth pair per
  // attachment size, reused across passes of that size.
  struct RtEntry {
    VkImage color = VK_NULL_HANDLE;
    VkDeviceMemory color_mem = VK_NULL_HANDLE;
    VkImageView color_view = VK_NULL_HANDLE;
    VkImage depth = VK_NULL_HANDLE;
    VkDeviceMemory depth_mem = VK_NULL_HANDLE;
    VkImageView depth_view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool depth_ready = false;
    bool cleared_once = false;
    // 2026-10-12: the part of the surface viewports have reached (guest
    // pixels, grow-only). Passes render only that area: the main surface is
    // 1280x1024 like EDRAM but the frame uses 1280x720 of it, and on a tiled
    // GPU (Nova, Adreno) every pass loaded / stored the unused 40 % too.
    uint32_t used_w = 0, used_h = 0;
    // 2026-09-06: multisampled attachments (v14 captures: RB_SURFACE_INFO
    // msaa_samples = 1 → 2x on the scene surfaces). The MS images are the
    // real attachments and persist across passes like EDRAM; every pass
    // resolves into |color| / |depth| (average / sample zero), which is what
    // snapshots, resolves, depth copies and the readback read.
    VkImage ms_color = VK_NULL_HANDLE;
    VkDeviceMemory ms_color_mem = VK_NULL_HANDLE;
    VkImageView ms_color_view = VK_NULL_HANDLE;
    VkImage ms_depth = VK_NULL_HANDLE;
    VkDeviceMemory ms_depth_mem = VK_NULL_HANDLE;
    VkImageView ms_depth_view = VK_NULL_HANDLE;
    uint32_t ms_samples = 0;   // 0 = not created
    bool ms_ready = false;     // layouts transitioned once
    // 2026-09-15: a surface used at one count and then another (menus vs
    // fights on the main surface) had its MS set refused forever, and every
    // draw then split the pass (2x asked, 1x delivered, 2x asked...). The
    // set is recreated at the new count at the next frame start (the old one
    // may still be referenced by this frame's command buffer); meanwhile the
    // frame renders 1x without splitting.
    uint32_t ms_pending_samples = 0;
    uint64_t ms_failed_frame = 0;
    // 2026-09-15: a surface's sample count is fixed for a frame by the first
    // pass that draws into it. The versus screen draws its background at 1x
    // and then the robot at 4x on the same surface: switching to the MS
    // attachments mid-frame loaded never-written MS colour/depth (black, and
    // a depth that rejected every robot pixel). Xenos keeps one EDRAM
    // surface; we keep one attachment set per frame instead of unresolving.
    uint32_t frame_samples = 1;
    uint64_t frame_samples_frame = 0;
    // 2026-09-06: sized to the surface's largest viewport (rounded up to
    // 256), not always full-frame — a live session touches dozens of small
    // surfaces (twenty 64x64 menu passes alone) and full-size MSAA pairs
    // for each exhausted GPU memory, which surfaced as a device loss.
    uint32_t width = 0, height = 0;
    uint64_t last_used = 0;    // frame counter; unused entries are evicted
  };
  bool EnsureMultisample(RtEntry* rt, uint32_t samples);
  void DestroyMultisample(RtEntry& rt);
  // Live sessions upload every new texture content for hours; the heap has
  // fixed slots and no per-slot eviction, so when it nears full everything
  // (textures, cube maps, resolve targets) is dropped and re-uploaded on
  // demand — one hitch and one lost frame of glow feedback.
  void ResetTextureHeap();
  void UploadFallbacks();
  void DestroyRt(RtEntry& rt);
  uint64_t frame_counter_ = 0;
  // Keyed by the guest RT surface pointer (SetRenderTargetOrDS payload +16):
  // distinct EDRAM surfaces get distinct attachments, all full-frame sized
  // (viewports bound the used region, mirroring EDRAM allocation).
  std::unordered_map<uint64_t, RtEntry> rt_pool_;
  // Sampled texture ids with no dump file; if also not a resolve target,
  // they are GPU-written outside the command stream (front buffer) and get
  // a main-attachment snapshot substituted.
  std::unordered_map<uint32_t, bool> missing_textures_;
  // Sampled textures whose dump is frame-sized (1280x720): the scene colour
  // texture the title resolves at swap time (outside the command stream) and
  // reads back for bloom/blur. Substituted with the main-attachment
  // snapshot at first use unless an in-frame resolve produces the id.
  std::unordered_map<uint32_t, bool> frame_sized_textures_;
  // Resolve-destination images, keyed by phys; layout tracked for re-resolve.
  struct ResolveDst {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    uint32_t heap_index = 0;
    uint32_t width = 0, height = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    // D32_SFLOAT for a direct depth copy (see copy_depth_to), else colour.
    VkFormat format = VK_FORMAT_UNDEFINED;
    // Last depth copy into this image: (frame, pass index, source) - a
    // repeat with nothing rendered in between is skipped.
    uint64_t depth_frame = 0;
    uint32_t depth_pass = 0;
    const void* depth_src = nullptr;
  };
  // Depth copies go image -> image into a sampled D32 texture when the device
  // can filter one (0 = not probed, 1 = yes, 2 = no: staged R32F copy).
  int depth_direct_ = 0;
  std::unordered_map<uint32_t, ResolveDst> resolve_dsts_;
  // 2026-09-15: the title reuses a resolve address for surfaces of different
  // sizes across screens (versus screen vs fight). A destination image of a
  // stale size sampled with the new size showed as a moire of old content
  // that the glow chain turned into a white-out of the versus screen. A
  // resized destination is retired here (destroyed at the next frame start,
  // after the queue went idle) and recreated at the new size, keeping its
  // heap index.
  struct RetiredImage {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
  };
  std::vector<RetiredImage> retired_images_;
  void RetireResolveDst(ResolveDst& dst) {
    retired_images_.push_back({dst.image, dst.memory, dst.view});
    dst.image = VK_NULL_HANDLE;
    dst.memory = VK_NULL_HANDLE;
    dst.view = VK_NULL_HANDLE;
    dst.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    dst.width = dst.height = 0;
    dst.format = VK_FORMAT_UNDEFINED;
    dst.depth_src = nullptr;
  }
  void DestroyRetired() {
    for (const RetiredImage& r : retired_images_) {
      if (r.view) vkDestroyImageView(device_, r.view, nullptr);
      if (r.image) vkDestroyImage(device_, r.image, nullptr);
      if (r.memory) vkFreeMemory(device_, r.memory, nullptr);
    }
    retired_images_.clear();
  }

  void RunAmbientOcclusion(RtEntry* rt);  // (defined with EnsureAoResources)
  RtEntry* GetRt(uint64_t key, uint32_t need_w = kAttachW,
                 uint32_t need_h = kAttachH);
};

bool Backend::EnsureMultisample(RtEntry* rt, uint32_t samples) {
  if (rt->ms_samples == samples && rt->ms_color != VK_NULL_HANDLE) return true;
  if (rt->ms_color != VK_NULL_HANDLE) {
    rt->ms_pending_samples = samples;  // recreated next frame (see RenderFrame)
    return false;
  }
  const VkSampleCountFlagBits bits =
      samples == 4 ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_2_BIT;
  if (!AllocImage(device_, phys_, SW(rt->width), SH(rt->height), VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &rt->ms_color,
                  &rt->ms_color_mem, 1, 1, bits) ||
      !AllocImage(device_, phys_, SW(rt->width), SH(rt->height), VK_FORMAT_D32_SFLOAT,
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &rt->ms_depth,
                  &rt->ms_depth_mem, 1, 1, bits)) {
    return false;
  }
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.image = rt->ms_color;
  vci.format = VK_FORMAT_R8G8B8A8_UNORM;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &vci, nullptr, &rt->ms_color_view) != VK_SUCCESS) {
    return false;
  }
  vci.image = rt->ms_depth;
  vci.format = VK_FORMAT_D32_SFLOAT;
  vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  if (vkCreateImageView(device_, &vci, nullptr, &rt->ms_depth_view) != VK_SUCCESS) {
    return false;
  }
  rt->ms_samples = samples;
  return true;
}

void Backend::DestroyMultisample(RtEntry& rt) {
  if (rt.ms_color_view) vkDestroyImageView(device_, rt.ms_color_view, nullptr);
  if (rt.ms_depth_view) vkDestroyImageView(device_, rt.ms_depth_view, nullptr);
  if (rt.ms_color) vkDestroyImage(device_, rt.ms_color, nullptr);
  if (rt.ms_depth) vkDestroyImage(device_, rt.ms_depth, nullptr);
  if (rt.ms_color_mem) vkFreeMemory(device_, rt.ms_color_mem, nullptr);
  if (rt.ms_depth_mem) vkFreeMemory(device_, rt.ms_depth_mem, nullptr);
  rt.ms_color_view = rt.ms_depth_view = VK_NULL_HANDLE;
  rt.ms_color = rt.ms_depth = VK_NULL_HANDLE;
  rt.ms_color_mem = rt.ms_depth_mem = VK_NULL_HANDLE;
  rt.ms_samples = 0;
  rt.ms_ready = false;
  rt.ms_pending_samples = 0;
}

void Backend::DestroyRt(RtEntry& rt) {
  if (rt.color_view) vkDestroyImageView(device_, rt.color_view, nullptr);
  if (rt.depth_view) vkDestroyImageView(device_, rt.depth_view, nullptr);
  if (rt.ms_color_view) vkDestroyImageView(device_, rt.ms_color_view, nullptr);
  if (rt.ms_depth_view) vkDestroyImageView(device_, rt.ms_depth_view, nullptr);
  if (rt.color) vkDestroyImage(device_, rt.color, nullptr);
  if (rt.depth) vkDestroyImage(device_, rt.depth, nullptr);
  if (rt.ms_color) vkDestroyImage(device_, rt.ms_color, nullptr);
  if (rt.ms_depth) vkDestroyImage(device_, rt.ms_depth, nullptr);
  if (rt.color_mem) vkFreeMemory(device_, rt.color_mem, nullptr);
  if (rt.depth_mem) vkFreeMemory(device_, rt.depth_mem, nullptr);
  if (rt.ms_color_mem) vkFreeMemory(device_, rt.ms_color_mem, nullptr);
  if (rt.ms_depth_mem) vkFreeMemory(device_, rt.ms_depth_mem, nullptr);
  rt = RtEntry{};
}

Backend::RtEntry* Backend::GetRt(uint64_t key, uint32_t need_w, uint32_t need_h) {
  // Key 0 (the main/presented surface) is always full size; others get the
  // largest extent seen for them, rounded up to 256.
  uint32_t w = kAttachW, h = kAttachH;
  if (key != 0) {
    w = std::min(kAttachW, std::max(256u, (need_w + 255u) & ~255u));
    h = std::min(kAttachH, std::max(256u, (need_h + 255u) & ~255u));
  }
  auto it = rt_pool_.find(key);
  if (it != rt_pool_.end()) {
    if (it->second.width >= w && it->second.height >= h) {
      it->second.last_used = frame_counter_;
      return &it->second;
    }
    // Grow: a new size means a new use of the surface; content is dropped.
    w = std::max(w, it->second.width);
    h = std::max(h, it->second.height);
    // The old images may already be referenced by this frame's command
    // buffer (2026-10-02: a pass opened for a clear-only resolve at a stale
    // small viewport, then the surface grew -> device loss / crash): retire
    // them until the next frame start instead of destroying them now.
    {
      RtEntry& o = it->second;
      retired_images_.push_back({o.color, o.color_mem, o.color_view});
      retired_images_.push_back({o.depth, o.depth_mem, o.depth_view});
      retired_images_.push_back({o.ms_color, o.ms_color_mem, o.ms_color_view});
      retired_images_.push_back({o.ms_depth, o.ms_depth_mem, o.ms_depth_view});
      o = RtEntry{};
    }
    rt_pool_.erase(it);
  }
  RtEntry entry;
  entry.width = w;
  entry.height = h;
  entry.last_used = frame_counter_;
  if (!AllocImage(device_, phys_, SW(w), SH(h), VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                  &entry.color, &entry.color_mem) ||
      !AllocImage(device_, phys_, SW(w), SH(h), VK_FORMAT_D32_SFLOAT,
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  &entry.depth, &entry.depth_mem)) {
    return nullptr;
  }
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.image = entry.color;
  vci.format = VK_FORMAT_R8G8B8A8_UNORM;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &vci, nullptr, &entry.color_view) !=
      VK_SUCCESS) {
    return nullptr;
  }
  vci.image = entry.depth;
  vci.format = VK_FORMAT_D32_SFLOAT;
  vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  if (vkCreateImageView(device_, &vci, nullptr, &entry.depth_view) !=
      VK_SUCCESS) {
    return nullptr;
  }
  auto inserted = rt_pool_.emplace(key, entry);
  return &inserted.first->second;
}

VkDeviceSize Backend::AllocConst(VkDeviceSize bytes) {
  cbuf_used_ = (cbuf_used_ + 15) & ~VkDeviceSize(15);
  const VkDeviceSize at = cbuf_used_;
  cbuf_used_ += bytes;
  if (cbuf_used_ > kConstBytes) {
    std::printf("[vk] constants buffer exhausted\n");
    return VkDeviceSize(-1);
  }
  return at;
}

// One frame slot's upload buffer (arena_bytes_): host-visible, mapped, for
// constants, geometry and texture staging.
bool Backend::CreateArena(FrameSlot& slot) {
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = arena_bytes_;
  bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
              VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
              VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
              VK_BUFFER_USAGE_TRANSFER_DST_BIT |  // per-pass dumps
              VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
  VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &arena_));

  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, arena_, &req);
  VkPhysicalDeviceMemoryProperties mem_props;
  vkGetPhysicalDeviceMemoryProperties(phys_, &mem_props);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if ((req.memoryTypeBits & (1u << i)) &&
        (mem_props.memoryTypes[i].propertyFlags & want) == want) {
      type = i;
      break;
    }
  }
  if (type == UINT32_MAX) {
    std::printf("[vk] no host-visible memory type\n");
    return false;
  }
  VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
  flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.pNext = &flags;
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &arena_mem_));
  VK_CHECK(vkBindBufferMemory(device_, arena_, arena_mem_, 0));
  VK_CHECK(vkMapMemory(device_, arena_mem_, 0, VK_WHOLE_SIZE, 0,
                       reinterpret_cast<void**>(&arena_ptr_)));
  VkBufferDeviceAddressInfo bda{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
  bda.buffer = arena_;
  arena_addr_ = vkGetBufferDeviceAddress(device_, &bda);
  slot.arena = arena_;
  slot.arena_mem = arena_mem_;
  slot.arena_ptr = arena_ptr_;
  slot.arena_addr = arena_addr_;
  return true;
}

void Backend::DestroyArena(FrameSlot& slot) {
  if (slot.arena_mem != VK_NULL_HANDLE) {
    vkUnmapMemory(device_, slot.arena_mem);
    vkFreeMemory(device_, slot.arena_mem, nullptr);
  }
  if (slot.arena != VK_NULL_HANDLE) vkDestroyBuffer(device_, slot.arena, nullptr);
  slot.arena = VK_NULL_HANDLE;
  slot.arena_mem = VK_NULL_HANDLE;
  slot.arena_ptr = nullptr;
  slot.arena_addr = 0;
}

VkDeviceSize Backend::Alloc(VkDeviceSize bytes, VkDeviceSize align) {
  arena_used_ = (arena_used_ + align - 1) & ~(align - 1);
  const VkDeviceSize at = arena_used_;
  arena_used_ += bytes;
  if (arena_used_ > arena_bytes_) {
    if (!arena_grow_) {
      std::printf("[vk] upload buffer full (%.0f MB) - it grows before the next frame\n",
                  double(arena_bytes_) / 1048576.0);
    }
    arena_grow_ = true;
    return VkDeviceSize(-1);
  }
  return at;
}

// Android, a player-chosen GPU driver (src/realsteel_gpu_driver.cpp): its
// vkGetInstanceProcAddr replaces the system loader's. Null = system driver.
PFN_vkGetInstanceProcAddr g_custom_loader = nullptr;
std::string g_driver_text;
std::string g_init_error;  // why the device could not be set up, for the player
std::atomic<VkPhysicalDevice> g_budget_phys{VK_NULL_HANDLE};  // VK_EXT_memory_budget enabled

// The renderer's own GPU memory, for the memory display where the driver
// has no budget extension (the Retroid Pocket Nova's): every allocation
// goes through these wrappers of volk's entry points.
PFN_vkAllocateMemory g_real_allocate = nullptr;
PFN_vkFreeMemory g_real_free = nullptr;
std::mutex g_memory_mutex;
std::unordered_map<VkDeviceMemory, VkDeviceSize> g_memory_sizes;
std::atomic<uint64_t> g_memory_total{0};

VKAPI_ATTR VkResult VKAPI_CALL TrackedAllocateMemory(VkDevice device, const VkMemoryAllocateInfo* info,
                                                     const VkAllocationCallbacks* callbacks,
                                                     VkDeviceMemory* memory) {
  const VkResult r = g_real_allocate(device, info, callbacks, memory);
  if (r == VK_SUCCESS && memory != nullptr && *memory != VK_NULL_HANDLE) {
    std::lock_guard<std::mutex> lock(g_memory_mutex);
    g_memory_sizes[*memory] = info->allocationSize;
    g_memory_total.fetch_add(info->allocationSize);
  }
  return r;
}

VKAPI_ATTR void VKAPI_CALL TrackedFreeMemory(VkDevice device, VkDeviceMemory memory,
                                             const VkAllocationCallbacks* callbacks) {
  if (memory != VK_NULL_HANDLE) {
    std::lock_guard<std::mutex> lock(g_memory_mutex);
    const auto it = g_memory_sizes.find(memory);
    if (it != g_memory_sizes.end()) {
      g_memory_total.fetch_sub(it->second);
      g_memory_sizes.erase(it);
    }
  }
  g_real_free(device, memory, callbacks);
}

bool Backend::InitVulkan(bool validation) {
  if (FILE* f = std::fopen("rs_exp_skip", "rb")) {
    char word[32];
    while (std::fscanf(f, "%31s", word) == 1) {
      exp_skip_.insert(uint32_t(std::strtoul(word, nullptr, 16)));
    }
    std::fclose(f);
    std::printf("[vk] EXPERIMENT: skipping draws of %zu shader id(s)\n", exp_skip_.size());
  }
  if (g_custom_loader != nullptr) {
    volkInitializeCustom(g_custom_loader);
    std::printf("[vk] Vulkan loader: CUSTOM GPU driver (chosen in the launcher)\n");
  } else {
    VK_CHECK(volkInitialize());
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "rs_replay";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  const char* inst_exts[] = {VK_KHR_SURFACE_EXTENSION_NAME,
#ifdef _WIN32
                             VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#elif defined(__ANDROID__)
                             "VK_KHR_android_surface"
#else
                             options_.wayland_display != nullptr
                                 ? "VK_KHR_wayland_surface"
                                 : "VK_KHR_xlib_surface"
#endif
  };
  std::vector<const char*> inst_ext_list(inst_exts, inst_exts + sizeof(inst_exts) / sizeof(inst_exts[0]));
  if (options_.window) {
    // HDR colour spaces for the swapchain, where the loader offers them.
    uint32_t count = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> available(count);
    vkEnumerateInstanceExtensionProperties(nullptr, &count, available.data());
    for (const VkExtensionProperties& e : available) {
      if (std::strcmp(e.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0) {
        inst_ext_list.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        have_colorspace_ext_ = true;
      }
    }
    ici.enabledExtensionCount = uint32_t(inst_ext_list.size());
    ici.ppEnabledExtensionNames = inst_ext_list.data();
  }
  const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
  if (validation) {
    // The validation layer ships with the LunarG SDK / vkconfig, not with
    // GPU drivers — enable it only when actually present.
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);
    std::vector<VkLayerProperties> available(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, available.data());
    bool found = false;
    for (const VkLayerProperties& p : available) {
      if (std::strcmp(p.layerName, layers[0]) == 0) {
        found = true;
        break;
      }
    }
    if (found) {
      ici.enabledLayerCount = 1;
      ici.ppEnabledLayerNames = layers;
    } else {
      std::printf("[vk] validation layer not installed — continuing without\n");
    }
  }
  VK_CHECK(vkCreateInstance(&ici, nullptr, &instance_));
  volkLoadInstance(instance_);

  uint32_t count = 0;
  vkEnumeratePhysicalDevices(instance_, &count, nullptr);
  if (count == 0) {
    std::printf("[vk] no Vulkan devices\n");
    return false;
  }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance_, &count, devices.data());
  // Discrete, then integrated (the Steam Deck's only GPU), then anything
  // else; a software rasteriser (lavapipe is often enumerated too) last.
  phys_ = devices[0];
  int best_rank = -1;
  for (VkPhysicalDevice d : devices) {
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(d, &props);
    const int rank =
        props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3
        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2
        : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU ? 0 : 1;
    if (rank > best_rank) {
      best_rank = rank;
      phys_ = d;
    }
  }
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(phys_, &props);
  std::printf("[vk] using %s\n", props.deviceName);
  {
    // Which driver is running (bug reports): name, build, version.
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(phys_, &p2);
    // A custom driver that fails to load (a missing companion library, say)
    // is quietly replaced by the system driver inside libadrenotools: only
    // call it custom when its file is really mapped into the process
    // (2026-10-16, a Qualcomm driver package on an Adreno 725).
    const char* custom_note = "";
    if (g_custom_loader != nullptr) {
      custom_note = " - CUSTOM DRIVER";
#ifdef __ANDROID__
      if (const char* chosen = std::getenv("REALSTEEL_GPU_DRIVER"); chosen != nullptr && *chosen != 0) {
        const std::string file = std::filesystem::path(chosen).filename().string();
        bool mapped = false;
        if (FILE* maps = std::fopen("/proc/self/maps", "r")) {
          char line[1024];
          while (!mapped && std::fgets(line, sizeof(line), maps) != nullptr) mapped = std::strstr(line, file.c_str()) != nullptr;
          std::fclose(maps);
        }
        if (!mapped) {
          custom_note = " - the CUSTOM DRIVER FAILED TO LOAD, this is the system driver (see hook_impl in the logcat)";
        }
      }
#endif
    }
    char text[640];
    std::snprintf(text, sizeof(text),
                  "%s, driver %s [%s] (id %d, version 0x%08x, Vulkan %u.%u.%u)%s",
                  props.deviceName, drv.driverName, drv.driverInfo, int(drv.driverID),
                  props.driverVersion, VK_API_VERSION_MAJOR(props.apiVersion),
                  VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion),
                  custom_note);
    // (Qualcomm's driverInfo is several lines: keep the log line whole.)
    for (char* c = text; *c != 0; ++c) {
      if (*c == '\n' || *c == '\r') *c = ' ';
    }
    g_driver_text = text;
    std::printf("[vk] GPU %s\n", text);
    std::fflush(stdout);
  }

  uint32_t family_count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(phys_, &family_count, nullptr);
  std::vector<VkQueueFamilyProperties> families(family_count);
  vkGetPhysicalDeviceQueueFamilyProperties(phys_, &family_count,
                                           families.data());
  queue_family_ = 0;
  for (uint32_t i = 0; i < family_count; ++i) {
    if (!(families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
#ifdef _WIN32
    if (options_.window &&
        !vkGetPhysicalDeviceWin32PresentationSupportKHR(phys_, i)) {
      continue;
    }
#endif
    queue_family_ = i;
    break;
  }

  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueFamilyIndex = queue_family_;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  // Vulkan 1.3 is the target. A device that reports less (2026-10-13: a
  // phone with an Adreno 725 whose driver says 1.1) gets the same
  // features through their extensions where the driver offers them; what
  // stays missing is named and the renderer stops before vkCreateDevice
  // (asking a 1.1 driver for 1.3 structures killed the process).
  const uint32_t api = props.apiVersion;
  // Tests the path on a 1.3 device: the variable, or a file "rs_vk_compat"
  // in the working folder (RealSteel/ on Android).
  bool force_compat = std::getenv("REALSTEEL_VK_COMPAT") != nullptr;
  if (FILE* f = std::fopen("rs_vk_compat", "rb")) {
    std::fclose(f);
    force_compat = true;
  }
  const bool core12 = api >= VK_API_VERSION_1_2 && !force_compat;
  const bool core13 = api >= VK_API_VERSION_1_3 && !force_compat;
  std::vector<VkExtensionProperties> available;
  {
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &count, nullptr);
    available.resize(count);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &count, available.data());
  }
  auto has_ext = [&](const char* name) {
    for (const VkExtensionProperties& e : available) {
      if (std::strcmp(e.extensionName, name) == 0) return true;
    }
    return false;
  };
  std::vector<const char*> dev_exts;
  if (options_.window) dev_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  std::string missing;
  auto need_ext = [&](const char* name) {
    if (has_ext(name)) {
      dev_exts.push_back(name);
    } else {
      missing += missing.empty() ? name : std::string(", ") + name;
    }
  };
  if (!core12) {
    need_ext(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);       // (for depth_stencil_resolve)
    need_ext(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);     // (for dynamic_rendering)
    need_ext(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
    need_ext(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    need_ext(VK_KHR_SEPARATE_DEPTH_STENCIL_LAYOUTS_EXTENSION_NAME);
  }
  if (!core13) {
    need_ext(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    need_ext(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
  }
  if (!core13) {
    std::printf("[vk] device reports Vulkan %u.%u%s: using extensions for the 1.3 features\n",
                VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api),
                force_compat ? " (REALSTEEL_VK_COMPAT)" : "");
  }

  // What we ask for: the 1.2 / 1.3 feature blocks where they are core, the
  // extensions' own feature structures otherwise. The "s" twins receive
  // what the device offers.
  VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceDynamicRenderingFeatures fdr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
  VkPhysicalDeviceSynchronization2Features fs2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
  VkPhysicalDeviceBufferDeviceAddressFeatures fbda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
  VkPhysicalDeviceDescriptorIndexingFeatures fdi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
  VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures fsd{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES};
  VkPhysicalDeviceVulkan13Features s13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features s12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  VkPhysicalDeviceDynamicRenderingFeatures sdr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
  VkPhysicalDeviceSynchronization2Features ss2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES};
  VkPhysicalDeviceBufferDeviceAddressFeatures sbda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES};
  VkPhysicalDeviceDescriptorIndexingFeatures sdi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES};
  VkPhysicalDeviceSeparateDepthStencilLayoutsFeatures ssd{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SEPARATE_DEPTH_STENCIL_LAYOUTS_FEATURES};
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  VkPhysicalDeviceFeatures2 s2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  auto chain = [](void* head, std::initializer_list<void*> rest) {
    auto* at = static_cast<VkBaseOutStructure*>(head);
    for (void* next : rest) {
      at->pNext = static_cast<VkBaseOutStructure*>(next);
      at = at->pNext;
    }
    at->pNext = nullptr;
  };
  if (core13) {
    chain(&f2, {&f12, &f13});
    chain(&s2, {&s12, &s13});
  } else if (core12) {
    chain(&f2, {&f12, &fdr, &fs2});
    chain(&s2, {&s12, &sdr, &ss2});
  } else if (missing.empty()) {
    chain(&f2, {&fbda, &fdi, &fsd, &fdr, &fs2});
    chain(&s2, {&sbda, &sdi, &ssd, &sdr, &ss2});
  } else {
    chain(&f2, {});
    chain(&s2, {});
  }
  // The same switches whichever structure carries them.
  VkBool32& want_dynamic_rendering = core13 ? f13.dynamicRendering : fdr.dynamicRendering;
  VkBool32& want_sync2 = core13 ? f13.synchronization2 : fs2.synchronization2;
  VkBool32& want_bda = core12 ? f12.bufferDeviceAddress : fbda.bufferDeviceAddress;
  VkBool32& want_rda = core12 ? f12.runtimeDescriptorArray : fdi.runtimeDescriptorArray;
  VkBool32& want_pb = core12 ? f12.descriptorBindingPartiallyBound : fdi.descriptorBindingPartiallyBound;
  VkBool32& want_vdc = core12 ? f12.descriptorBindingVariableDescriptorCount
                              : fdi.descriptorBindingVariableDescriptorCount;
  VkBool32& want_uab = core12 ? f12.descriptorBindingSampledImageUpdateAfterBind
                              : fdi.descriptorBindingSampledImageUpdateAfterBind;
  VkBool32& want_uuwp = core12 ? f12.descriptorBindingUpdateUnusedWhilePending
                               : fdi.descriptorBindingUpdateUnusedWhilePending;
  VkBool32& want_nui = core12 ? f12.shaderSampledImageArrayNonUniformIndexing
                              : fdi.shaderSampledImageArrayNonUniformIndexing;
  VkBool32& want_sdsl = core12 ? f12.separateDepthStencilLayouts : fsd.separateDepthStencilLayouts;
  want_dynamic_rendering = VK_TRUE;
  want_sync2 = VK_TRUE;
  want_bda = VK_TRUE;
  want_rda = VK_TRUE;
  want_pb = VK_TRUE;
  want_vdc = VK_TRUE;
  // Snapshots/resolves write heap descriptors while the sets are bound in
  // the recording command buffer; that needs update-after-bind heaps.
  want_uab = VK_TRUE;
  want_uuwp = VK_TRUE;
  want_nui = VK_TRUE;
  want_sdsl = VK_TRUE;  // depth-only image layouts
  {
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(phys_, &supported);
    VkPhysicalDeviceProperties dev_props{};
    vkGetPhysicalDeviceProperties(phys_, &dev_props);
    max_anisotropy_ = supported.samplerAnisotropy
                          ? dev_props.limits.maxSamplerAnisotropy
                          : 1.0f;
  }
  f2.features.samplerAnisotropy = max_anisotropy_ > 1.0f ? VK_TRUE : VK_FALSE;
  f2.features.shaderInt64 = VK_TRUE;  // 64-bit push-constant addresses
  f2.features.robustBufferAccess = VK_TRUE;  // OOB reads return zero
  f2.features.logicOp = VK_TRUE;  // --vkflatps rasterization proof
  if (missing.empty()) {
    // What the device offers (2026-10-11: the Adreno 740 driver refused the
    // device with VK_ERROR_FEATURE_NOT_PRESENT). Optional features are only
    // asked for when present; every missing one is reported.
    vkGetPhysicalDeviceFeatures2(phys_, &s2);
    auto have = [&](VkBool32 VkPhysicalDeviceVulkan12Features::*core,
                    VkBool32 VkPhysicalDeviceDescriptorIndexingFeatures::*ext) {
      return core12 ? s12.*core : sdi.*ext;
    };
    auto check = [&](const char* name, VkBool32& wanted, VkBool32 got, bool optional) {
      if (wanted && !got) {
        std::printf("[vk] device lacks %s%s\n", name, optional ? " (optional: not used)" : "");
        if (optional) {
          wanted = VK_FALSE;
        } else {
          missing += missing.empty() ? name : std::string(", ") + name;
        }
      }
    };
    check("logicOp", f2.features.logicOp, s2.features.logicOp, true);
    check("robustBufferAccess", f2.features.robustBufferAccess, s2.features.robustBufferAccess, true);
    check("shaderInt64", f2.features.shaderInt64, s2.features.shaderInt64, true);
    check("separateDepthStencilLayouts", want_sdsl,
          core12 ? s12.separateDepthStencilLayouts : ssd.separateDepthStencilLayouts, true);
    check("bufferDeviceAddress", want_bda, core12 ? s12.bufferDeviceAddress : sbda.bufferDeviceAddress, false);
    check("runtimeDescriptorArray", want_rda,
          have(&VkPhysicalDeviceVulkan12Features::runtimeDescriptorArray,
               &VkPhysicalDeviceDescriptorIndexingFeatures::runtimeDescriptorArray), false);
    check("descriptorBindingPartiallyBound", want_pb,
          have(&VkPhysicalDeviceVulkan12Features::descriptorBindingPartiallyBound,
               &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingPartiallyBound), false);
    check("descriptorBindingVariableDescriptorCount", want_vdc,
          have(&VkPhysicalDeviceVulkan12Features::descriptorBindingVariableDescriptorCount,
               &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingVariableDescriptorCount), false);
    check("descriptorBindingSampledImageUpdateAfterBind", want_uab,
          have(&VkPhysicalDeviceVulkan12Features::descriptorBindingSampledImageUpdateAfterBind,
               &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingSampledImageUpdateAfterBind),
          false);
    check("descriptorBindingUpdateUnusedWhilePending", want_uuwp,
          have(&VkPhysicalDeviceVulkan12Features::descriptorBindingUpdateUnusedWhilePending,
               &VkPhysicalDeviceDescriptorIndexingFeatures::descriptorBindingUpdateUnusedWhilePending),
          false);
    check("shaderSampledImageArrayNonUniformIndexing", want_nui,
          have(&VkPhysicalDeviceVulkan12Features::shaderSampledImageArrayNonUniformIndexing,
               &VkPhysicalDeviceDescriptorIndexingFeatures::shaderSampledImageArrayNonUniformIndexing),
          false);
    check("dynamicRendering", want_dynamic_rendering,
          core13 ? s13.dynamicRendering : sdr.dynamicRendering, false);
    check("synchronization2", want_sync2, core13 ? s13.synchronization2 : ss2.synchronization2, false);
  }
  if (!missing.empty()) {
    char why[1024];
    std::snprintf(why, sizeof(why),
                  "This device's graphics driver (Vulkan %u.%u) lacks what the renderer needs: %s",
                  VK_API_VERSION_MAJOR(api), VK_API_VERSION_MINOR(api), missing.c_str());
    g_init_error = why;
    std::printf("[vk] %s\n", why);
    std::fflush(stdout);
    return false;
  }
  std::fflush(stdout);

  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.pNext = &f2;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  // Memory use display (Settings > Display > Show memory use): the driver's
  // own figures where it offers them.
  if (has_ext(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME)) {
    dev_exts.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    g_budget_phys = phys_;
  }
  if (options_.window && has_ext(VK_EXT_HDR_METADATA_EXTENSION_NAME)) {
    dev_exts.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    have_hdr_metadata_ = true;
  }
  dci.enabledExtensionCount = uint32_t(dev_exts.size());
  dci.ppEnabledExtensionNames = dev_exts.empty() ? nullptr : dev_exts.data();
  VK_CHECK(vkCreateDevice(phys_, &dci, nullptr, &device_));
  volkLoadDevice(device_);
  if (vkAllocateMemory != TrackedAllocateMemory) {
    g_real_allocate = vkAllocateMemory;
    g_real_free = vkFreeMemory;
    vkAllocateMemory = TrackedAllocateMemory;
    vkFreeMemory = TrackedFreeMemory;
  }
  if (!core13) {
    // The extensions' entry points under the core names the renderer calls.
    vkCmdBeginRendering = vkCmdBeginRenderingKHR;
    vkCmdEndRendering = vkCmdEndRenderingKHR;
    vkCmdPipelineBarrier2 = vkCmdPipelineBarrier2KHR;
  }
  if (!core12) vkGetBufferDeviceAddress = vkGetBufferDeviceAddressKHR;
  if (vkCmdBeginRendering == nullptr || vkCmdEndRendering == nullptr ||
      vkCmdPipelineBarrier2 == nullptr || vkGetBufferDeviceAddress == nullptr) {
    g_init_error = "This device's graphics driver did not provide the rendering functions it listed";
    std::printf("[vk] %s\n", g_init_error.c_str());
    std::fflush(stdout);
    return false;
  }
  vkGetDeviceQueue(device_, queue_family_, 0, &queue_);

  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = queue_family_;
  VK_CHECK(vkCreateCommandPool(device_, &pci, nullptr, &pool_));
  VkCommandBufferAllocateInfo cai{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool_;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  for (FrameSlot& slot : slots_) {
    VK_CHECK(vkAllocateCommandBuffers(device_, &cai, &slot.cmd));
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(device_, &fci, nullptr, &slot.fence));
  }

  // The arena: one host-visible buffer with a device address, used for
  // constants, geometry and texture staging. One per frame slot.
  if (const char* mb = std::getenv("REALSTEEL_ARENA_MB")) {  // tests the growth path
    arena_bytes_ = std::max<VkDeviceSize>(1, std::strtoull(mb, nullptr, 10)) * 1024 * 1024;
  }
  for (FrameSlot& slot : slots_) {
  if (!CreateArena(slot)) return false;
  VkPhysicalDeviceMemoryProperties mem_props;
  vkGetPhysicalDeviceMemoryProperties(phys_, &mem_props);

  VkBufferCreateInfo cci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  cci.size = kConstBytes;
  cci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  VK_CHECK(vkCreateBuffer(device_, &cci, nullptr, &slot.cbuf));
  VkMemoryRequirements creq;
  vkGetBufferMemoryRequirements(device_, slot.cbuf, &creq);
  uint32_t ctype = UINT32_MAX;
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if ((creq.memoryTypeBits & (1u << i)) &&
        (mem_props.memoryTypes[i].propertyFlags & want) == want) {
      ctype = i;
      break;
    }
  }
  if (ctype == UINT32_MAX) {
    std::printf("[vk] no host-visible memory type for the constants buffer\n");
    return false;
  }
  VkMemoryAllocateInfo cmai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  cmai.allocationSize = creq.size;
  cmai.memoryTypeIndex = ctype;
  VK_CHECK(vkAllocateMemory(device_, &cmai, nullptr, &slot.cbuf_mem));
  VK_CHECK(vkBindBufferMemory(device_, slot.cbuf, slot.cbuf_mem, 0));
  VK_CHECK(vkMapMemory(device_, slot.cbuf_mem, 0, VK_WHOLE_SIZE, 0,
                       reinterpret_cast<void**>(&slot.cbuf_ptr)));
  }
  UseSlot(0);
  return true;
}

bool AllocImage(VkDevice device, VkPhysicalDevice phys, uint32_t width,
                uint32_t height, VkFormat format, VkImageUsageFlags usage,
                VkImage* image, VkDeviceMemory* memory, uint32_t layers,
                uint32_t mip_levels, VkSampleCountFlagBits samples) {
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.flags = layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  ici.format = format;
  ici.extent = {width, height, 1};
  ici.mipLevels = mip_levels;
  ici.arrayLayers = layers;
  ici.samples = samples;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage;
  VK_CHECK(vkCreateImage(device, &ici, nullptr, image));
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(device, *image, &req);
  VkPhysicalDeviceMemoryProperties mem_props;
  vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) &&
        (mem_props.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      type = i;
      break;
    }
  }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  VK_CHECK(vkAllocateMemory(device, &mai, nullptr, memory));
  VK_CHECK(vkBindImageMemory(device, *image, *memory, 0));
  return true;
}

bool Backend::CreateTargets() {
  if (!AllocImage(device_, phys_, kWidth, kHeight, VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                  &color_, &color_mem_)) {
    return false;
  }
  if (!AllocImage(device_, phys_, kWidth, kHeight, VK_FORMAT_D32_SFLOAT,
                  VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, &depth_,
                  &depth_mem_)) {
    return false;
  }
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.image = color_;
  vci.format = VK_FORMAT_R8G8B8A8_UNORM;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &color_view_));
  vci.image = depth_;
  vci.format = VK_FORMAT_D32_SFLOAT;
  vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &depth_view_));

  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = VkDeviceSize(kWidth) * kHeight * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VK_CHECK(vkCreateBuffer(device_, &bci, nullptr, &readback_));
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, readback_, &req);
  VkPhysicalDeviceMemoryProperties mem_props;
  vkGetPhysicalDeviceMemoryProperties(phys_, &mem_props);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    if ((req.memoryTypeBits & (1u << i)) &&
        (mem_props.memoryTypes[i].propertyFlags & want) == want) {
      type = i;
      break;
    }
  }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  VK_CHECK(vkAllocateMemory(device_, &mai, nullptr, &readback_mem_));
  VK_CHECK(vkBindBufferMemory(device_, readback_, readback_mem_, 0));
  return true;
}

bool Backend::CreateDescriptors() {
  // Sets 0..2: sampled image heaps (2D/3D/Cube), set 3: samplers — matching
  // register(t0/s0, spaceN) in the fork's shader_common.h.
  for (int set = 0; set < 4; ++set) {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = set == 3 ? VK_DESCRIPTOR_TYPE_SAMPLER
                                      : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    binding.descriptorCount =
        set == 0 ? kMaxTextures : (set == 3 ? 16u : kMaxCubeTextures);
    binding.stageFlags = VK_SHADER_STAGE_ALL;
    VkDescriptorBindingFlags flag = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT |
                                    VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                    VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo flags{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    flags.bindingCount = 1;
    flags.pBindingFlags = &flag;
    VkDescriptorSetLayoutCreateInfo lci{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.pNext = &flags;
    lci.bindingCount = 1;
    lci.pBindings = &binding;
    lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &lci, nullptr,
                                         &set_layouts_[set]));
  }

  {
    // Set 4: the shader constants (the fork's shader_common.h, g_ConstArena).
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 1;
    lci.pBindings = &binding;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &lci, nullptr, &const_layout_));
  }

  VkDescriptorPoolSize sizes[3] = {
      {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxTextures + 2 * kMaxCubeTextures},
      {VK_DESCRIPTOR_TYPE_SAMPLER, 16},
      {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
  };
  VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pci.maxSets = 4 + 2;
  pci.poolSizeCount = 3;
  pci.pPoolSizes = sizes;
  pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
  VK_CHECK(vkCreateDescriptorPool(device_, &pci, nullptr, &desc_pool_));

  VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  dai.descriptorPool = desc_pool_;
  dai.descriptorSetCount = 4;
  dai.pSetLayouts = set_layouts_;
  VK_CHECK(vkAllocateDescriptorSets(device_, &dai, sets_));
  for (FrameSlot& slot : slots_) {
    VkDescriptorSetAllocateInfo cai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    cai.descriptorPool = desc_pool_;
    cai.descriptorSetCount = 1;
    cai.pSetLayouts = &const_layout_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &cai, &slot.cset));
    VkDescriptorBufferInfo info{slot.cbuf, 0, kConstBytes};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = slot.cset;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &info;
    vkUpdateDescriptorSets(device_, 1, &w, 0, nullptr);
  }
  cset_ = slots_[slot_].cset;

  if (!CreateTextureSampler()) return false;

  VkPushConstantRange range{};
  range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  range.offset = 0;
  range.size = 12;  // 3 x uint32 offsets into set 4: VS, PS constants, shared block.
  const VkDescriptorSetLayout layouts[5] = {set_layouts_[0], set_layouts_[1], set_layouts_[2],
                                            set_layouts_[3], const_layout_};
  VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  plci.setLayoutCount = 5;
  plci.pSetLayouts = layouts;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &range;
  VK_CHECK(vkCreatePipelineLayout(device_, &plci, nullptr, &pipeline_layout_));
  return true;
}

// 2026-10-15: a frame that brings in many new textures (a fight starting, a
// new menu) decoded them one by one on this thread - the 20-40 ms hitches
// on the Retroid Pocket Nova. They are decoded side by side on a few short-
// lived threads now; uploads stay on this thread, in the same order.
void Backend::PrefetchTextures(const FrameData& frame) {
  std::vector<std::pair<uint32_t, uint64_t>> todo;
  std::unordered_set<uint64_t> queued;
  for (const DrawRecord& d : frame.draws) {
    for (uint32_t s = 0; s < rs::capture::kTextureSlots; ++s) {
      const uint32_t phys = d.textures[s];
      if (phys == 0) continue;
      const uint64_t key = (uint64_t(phys) << 32) ^ d.texture_hash[s];
      if (texture_index_by_key_.count(key) != 0 || !queued.insert(key).second) continue;
      todo.emplace_back(phys, d.texture_hash[s]);
    }
  }
  if (todo.size() < 2) return;  // one texture: no gain from a thread
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<Prefetched> results(todo.size());
  std::atomic<size_t> next{0};
  auto work = [&]() {
    for (size_t i = next.fetch_add(1); i < todo.size(); i = next.fetch_add(1)) {
      results[i].ok = options_.texture_source->Fetch(todo[i].first, todo[i].second, results[i].tex);
    }
  };
  const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
  const size_t helpers = std::min<size_t>(todo.size() - 1, std::min(4u, cores > 2 ? cores - 2 : 1u));
  std::vector<std::thread> pool;
  pool.reserve(helpers);
  for (size_t i = 0; i < helpers; ++i) pool.emplace_back(work);
  work();
  for (std::thread& t : pool) t.join();
  for (size_t i = 0; i < todo.size(); ++i) {
    prefetched_[(uint64_t(todo[i].first) << 32) ^ todo[i].second] = std::move(results[i]);
  }
  stat_tex_fetch_ns_ += uint64_t((std::chrono::steady_clock::now() - t0).count());
}

// Uploads a decoded RGBA8 texture into the 2D heap; returns its index.
// Index 0 is a 1x1 white fallback created on first use.
uint32_t Backend::UploadTexture(uint32_t phys, uint64_t hash) {
  if (options_.texture_source != nullptr && phys != 0) {
    // Source mode: content identity is (phys, hash); a changed hash means
    // the guest streamed new data into the same address.
    const uint64_t key = (uint64_t(phys) << 32) ^ hash;
    // An id the renderer itself produces (resolve target, front/scene
    // snapshot, depth copy — all in resolve_dsts_) keeps that image: those
    // heap indices are assigned once at creation, and re-pointing the id at
    // the guest-memory upload every frame (2026-09-15) handed the DOF and
    // composite passes a white scene and a "far" depth from the second
    // frame on — fog colour and nothing else. (The file path never had this:
    // it returns early on a cached id without touching the map.)
    const auto rd = resolve_dsts_.find(phys);
    const bool overridden =
        rd != resolve_dsts_.end() && rd->second.image != VK_NULL_HANDLE;
    const auto kit = texture_index_by_key_.find(key);
    if (kit != texture_index_by_key_.end()) {
      texture_indices_[phys] = overridden ? rd->second.heap_index : kit->second;
      return texture_indices_[phys];
    }
    DecodedTexture tex;
    const auto t_fetch = std::chrono::steady_clock::now();
    bool fetched;
    if (const auto pf = prefetched_.find(key); pf != prefetched_.end()) {
      fetched = pf->second.ok;
      tex = std::move(pf->second.tex);
      prefetched_.erase(pf);
    } else {
      fetched = options_.texture_source->Fetch(phys, hash, tex);
    }
    const auto t_fetched = std::chrono::steady_clock::now();
    stat_tex_fetch_ns_ += uint64_t((t_fetched - t_fetch).count());
    ++stat_tex_n_;
    if (!fetched || tex.width == 0) {
      missing_textures_[phys] = true;
      tex.width = tex.height = 1;
      tex.layers = 1;
      tex.rgba.assign(4, 0xFF);
    } else if (tex.width == kWidth && tex.height == kHeight) {
      frame_sized_textures_[phys] = true;
    }
    const uint32_t index = UploadDecoded(phys, tex);
    stat_tex_up_ns_ += uint64_t((std::chrono::steady_clock::now() - t_fetched).count());
    if (index == 0 && arena_grow_) {
      // The upload buffer ran out (it grows before the next frame): white
      // for this frame, uploaded again next frame instead of kept white.
      return texture_indices_[phys] = 0;
    }
    texture_index_by_key_[key] = index;
    if (overridden) texture_indices_[phys] = rd->second.heap_index;
    return texture_indices_[phys];
  }
  const auto path = texture_paths_.find(phys);
  const std::string wanted = path == texture_paths_.end() ? std::string() : path->second;
  const auto up = texture_uploaded_path_.find(phys);
  const bool same_content = up == texture_uploaded_path_.end() || up->second == wanted;
  const auto it = texture_indices_.find(phys);
  if (it != texture_indices_.end() && same_content) return it->second;
  const auto cit = cube_indices_.find(phys);
  if (cit != cube_indices_.end() && same_content) return cit->second;
  texture_uploaded_path_[phys] = wanted;
  DecodedTexture tex;
  if (phys != 0) {
    if (path != texture_paths_.end()) {
      rs::capture::LoadXtx(path->second, tex);
    }
    if (tex.width == 0) {
      std::printf("[vk] texture %08X %s — white fallback\n", phys,
                  path == texture_paths_.end() ? "not dumped"
                                               : "failed to decode");
      missing_textures_[phys] = true;
    }
  }
  if (tex.width == 0) {
    tex.width = tex.height = 1;
    tex.layers = 1;
    tex.rgba.assign(4, 0xFF);
  } else if (tex.width == kWidth && tex.height == kHeight && phys != 0) {
    frame_sized_textures_[phys] = true;
  }
  return UploadDecoded(phys, tex);
}

void Backend::UploadFallbacks() {
  UploadTexture(0);  // heap index 0 = white fallback (missing dumps)
  {
    // Heap index 1 = black: what an unbound sampler slot reads (id 0). v9
    // captures only recorded slots 0-7, so their slots 8-19 land here too.
    DecodedTexture black;
    black.width = black.height = 1;
    black.layers = 1;
    black.rgba.assign(4, 0);
    black.rgba[3] = 0xFF;
    UploadDecoded(0xFFFFFFFFu, black);
  }
  EnsureCubeFallback();
}

void Backend::ResetTextureHeap() {
  vkDeviceWaitIdle(device_);
  std::printf("[vk] texture heap reset (%u 2D / %u cube / %zu resolve targets)\n",
              texture_count_, cube_count_, resolve_dsts_.size());
  for (VkImageView v : image_views_) vkDestroyImageView(device_, v, nullptr);
  for (VkImage i : images_) vkDestroyImage(device_, i, nullptr);
  for (VkDeviceMemory m : image_mems_) vkFreeMemory(device_, m, nullptr);
  image_views_.clear();
  images_.clear();
  image_mems_.clear();
  for (auto& [phys, dst] : resolve_dsts_) {
    if (dst.view) vkDestroyImageView(device_, dst.view, nullptr);
    if (dst.image) vkDestroyImage(device_, dst.image, nullptr);
    if (dst.memory) vkFreeMemory(device_, dst.memory, nullptr);
  }
  resolve_dsts_.clear();
  DestroyRetired();
  texture_indices_.clear();
  cube_indices_.clear();
  texture_index_by_key_.clear();
  texture_uploaded_path_.clear();
  missing_textures_.clear();
  frame_sized_textures_.clear();
  texture_count_ = 0;
  cube_count_ = 0;
}

void Backend::EnsureCubeFallback() {
  if (cube_count_ != 0) return;
  DecodedTexture white;
  white.width = white.height = 1;
  white.layers = 6;
  white.rgba.assign(24, 0xFF);
  UploadDecoded(0, white);  // cube heap index 0
}

uint32_t Backend::UploadDecoded(uint32_t phys, const DecodedTexture& full) {
  // Half-size textures (low-end setting): 2x2 box filter, 2D textures of
  // 256 px and up only (small ones and cube maps as they are).
  DecodedTexture half;
  const bool halve = options_.half_textures && full.layers == 1 && full.width >= 256 &&
                     full.height >= 256 && full.rgba.size() >= size_t(full.width) * full.height * 4;
  if (halve) {
    half.width = full.width / 2;
    half.height = full.height / 2;
    half.layers = 1;
    half.rgba.resize(size_t(half.width) * half.height * 4);
    const size_t row = size_t(full.width) * 4;
    for (uint32_t y = 0; y < half.height; ++y) {
      const uint8_t* a = full.rgba.data() + size_t(y) * 2 * row;
      const uint8_t* b = a + row;
      uint8_t* out = half.rgba.data() + size_t(y) * half.width * 4;
      for (uint32_t x = 0; x < half.width; ++x, a += 8, b += 8, out += 4) {
        for (int c = 0; c < 4; ++c) {
          out[c] = uint8_t((a[c] + a[c + 4] + b[c] + b[c + 4] + 2) >> 2);
        }
      }
    }
  }
  const DecodedTexture& tex = halve ? half : full;
  const bool cube = tex.layers == 6;
  if (cube ? cube_count_ >= kMaxCubeTextures : texture_count_ >= kMaxTextures) {
    static bool warned = false;
    if (!warned) {
      std::printf("[vk] texture heap full (%u 2D / %u cube) — white fallback from here\n",
                  texture_count_, cube_count_);
      warned = true;
    }
    return 0;
  }

  // Full mip chain, generated below with blits: the title samples with
  // trilinear minification (crowd at distance, the robots' reflection cube
  // map through fast-varying reflection vectors); level-0-only sampling
  // aliases into noise and makes the reflections harsh.
  uint32_t mip_levels = 1;
  while ((tex.width >> mip_levels) > 0 || (tex.height >> mip_levels) > 0) {
    ++mip_levels;
  }
  VkImage image;
  VkDeviceMemory memory;
  if (!AllocImage(device_, phys_, tex.width, tex.height,
                  VK_FORMAT_R8G8B8A8_UNORM,
                  VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                  &image, &memory, tex.layers, mip_levels)) {
    return 0;
  }
  VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vci.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
  vci.image = image;
  vci.format = VK_FORMAT_R8G8B8A8_UNORM;
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mip_levels, 0,
                          tex.layers};
  VkImageView view;
  if (vkCreateImageView(device_, &vci, nullptr, &view) != VK_SUCCESS) return 0;

  // Stage pixels in the arena and record the copy into the setup command
  // buffer (already recording during texture upload phase). Layers are
  // packed back to back, which is what a tightly packed buffer copy expects.
  // 2026-10-18: textures leave a quarter of the arena to the frame's own
  // vertex / constant data - a burst of big texture-pack textures filled it
  // and the frame's draws then read outside it (device lost). A texture
  // that does not fit waits a frame (white for that frame; the arena grows).
  if (arena_used_ + tex.rgba.size() + 16 > arena_bytes_ - arena_bytes_ / 4) {
    if (!arena_grow_) {
      std::printf("[vk] texture uploads deferred to the next frame (upload buffer %.0f MB)\n",
                  double(arena_bytes_) / 1048576.0);
    }
    arena_grow_ = true;
    vkDestroyImageView(device_, view, nullptr);
    vkDestroyImage(device_, image, nullptr);
    vkFreeMemory(device_, memory, nullptr);
    return 0;
  }
  const VkDeviceSize at = Alloc(tex.rgba.size(), 16);
  if (at == VkDeviceSize(-1)) return 0;
  std::memcpy(arena_ptr_ + at, tex.rgba.data(), tex.rgba.size());

  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, tex.layers};
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 1;
  dep.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(cmd_, &dep);

  VkBufferImageCopy copy{};
  copy.bufferOffset = at;
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, tex.layers};
  copy.imageExtent = {tex.width, tex.height, 1};
  vkCmdCopyBufferToImage(cmd_, arena_, image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

  // Generate mips: level i-1 (TRANSFER_DST after its write) -> TRANSFER_SRC,
  // blit into level i, until the 1x1 level; then everything -> sampled.
  uint32_t mw = tex.width, mh = tex.height;
  for (uint32_t level = 1; level < mip_levels; ++level) {
    VkImageMemoryBarrier2 to_src{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    to_src.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
    to_src.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_src.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
    to_src.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.image = image;
    to_src.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 1, 0,
                               tex.layers};
    VkDependencyInfo sdep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    sdep.imageMemoryBarrierCount = 1;
    sdep.pImageMemoryBarriers = &to_src;
    vkCmdPipelineBarrier2(cmd_, &sdep);

    const uint32_t nw = std::max(1u, mw / 2), nh = std::max(1u, mh / 2);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, tex.layers};
    blit.srcOffsets[1] = {int32_t(mw), int32_t(mh), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, tex.layers};
    blit.dstOffsets[1] = {int32_t(nw), int32_t(nh), 1};
    vkCmdBlitImage(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_LINEAR);
    mw = nw;
    mh = nh;
  }
  {
    VkImageMemoryBarrier2 fin[2] = {};
    fin[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    fin[0].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
    fin[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    fin[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                          VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    fin[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    fin[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    fin[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    fin[0].image = image;
    fin[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0,
                               mip_levels > 1 ? mip_levels - 1 : 1, 0,
                               tex.layers};
    if (mip_levels == 1) fin[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    fin[1] = fin[0];
    fin[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    fin[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mip_levels - 1, 1, 0,
                               tex.layers};
    VkDependencyInfo fdep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    fdep.imageMemoryBarrierCount = mip_levels > 1 ? 2u : 1u;
    fdep.pImageMemoryBarriers = fin;
    vkCmdPipelineBarrier2(cmd_, &fdep);
  }

  const uint32_t index = cube ? cube_count_++ : texture_count_++;
  images_.push_back(image);
  image_mems_.push_back(memory);
  image_views_.push_back(view);

  VkDescriptorImageInfo info{};
  info.imageView = view;
  info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = cube ? sets_[2] : sets_[0];
  write.dstBinding = 0;
  write.dstArrayElement = index;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  write.pImageInfo = &info;
  // No wait for the frame in flight (2026-10-12): |index| is a slot never
  // used before (counters only grow until a heap reset, which idles the
  // device), and the binding is UPDATE_UNUSED_WHILE_PENDING. The wait made
  // every new texture - the WRB arena's animated boards, each frame - stall
  // recording until the GPU had finished the previous frame (Nova: ~5 ms of
  // GPU idle a frame).
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

  if (cube) {
    if (phys != 0) cube_indices_[phys] = index;
    std::printf("[vk] cube texture %08X %ux%u -> cube heap %u\n", phys, tex.width,
                tex.height, index);
  } else {
    texture_indices_[phys] = index;
  }
  return index;
}

VkPipeline Backend::GetPipeline(const PipelineKey& key, const ShaderInfo* vs,
                                const ShaderInfo* ps) {
  const auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;
  const auto t_pipe = std::chrono::steady_clock::now();
  struct PipeTimer {
    Backend* b;
    std::chrono::steady_clock::time_point t0;
    ~PipeTimer() {
      b->stat_pipe_ns_ += uint64_t((std::chrono::steady_clock::now() - t0).count());
      ++b->stat_pipe_n_;
    }
  } pipe_timer{this, t_pipe};

  auto module_for = [&](const ShaderInfo* info) -> VkShaderModule {
    const auto cached = modules_.find(info->cache_hash);
    if (cached != modules_.end()) return cached->second;
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = info->spirv.size() * 4;
    mci.pCode = info->spirv.data();
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &mci, nullptr, &module) != VK_SUCCESS) {
      std::printf("[vk] shader module creation failed for %016llX\n",
                  static_cast<unsigned long long>(info->cache_hash));
    }
    modules_[info->cache_hash] = module;
    return module;
  };

  VkShaderModule vs_module = module_for(vs);
  VkShaderModule ps_module = module_for(ps);
  if (vs_module == VK_NULL_HANDLE || ps_module == VK_NULL_HANDLE) {
    pipelines_[key] = VK_NULL_HANDLE;
    return VK_NULL_HANDLE;
  }

  // Spec constant 0: enable R11G11B10 normal decode when the VS fetches
  // packed normals; alpha test handling comes later.
  uint32_t spec_value = 0;
  for (const ShaderVertexFetch& f : vs->fetches) {
    if ((f.usage == 3 || f.usage == 6 || f.usage == 7) &&
        (f.format == 16 || f.format == 17)) {
      spec_value |= 1;  // SPEC_CONSTANT_R11G11B10_NORMAL
    }
  }
  if (key.rs_valid && key.alpha_test) {
    spec_value |= 2;  // SPEC_CONSTANT_ALPHA_TEST: clip(alpha - g_AlphaThreshold)
  }
  VkSpecializationMapEntry spec_entry{0, 0, 4};
  VkSpecializationInfo spec{1, &spec_entry, 4, &spec_value};

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs_module;
  stages[0].pName = "main";
  stages[0].pSpecializationInfo = &spec;
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = ps_module;
  stages[1].pName = "main";
  stages[1].pSpecializationInfo = &spec;

  // Vertex input from the VS's fetch table (full fetches only; minis carry
  // no independent offsets and are rare — logged if seen).
  std::vector<VkVertexInputAttributeDescription> attrs;
  for (const ShaderVertexFetch& f : vs->fetches) {
    const int location = LocationFor(f.usage, f.usageIndex);
    uint32_t dwords = 0;
    // Offset/format/number-format bits come straight from the runtime-
    // patched vfetch instructions (mini-fetches included; see the
    // recompiler's VertexFetchRecord). The robot family's fetch block is
    // a constant-zero BlendWeight full fetch followed by the real float4
    // position mini-fetch at offset 0; every skinned VS fetches
    // BlendIndices as unsigned integer u8x4, matching the uint4 input.
    const uint32_t attr_offset_dwords = f.offset;
    VkFormat format = FormatFor(f.format, f.flags, &dwords);
    if (location < 0 || format == VK_FORMAT_UNDEFINED) {
      std::printf("[vk] unmapped vertex element usage=%u idx=%u fmt=%u\n",
                  f.usage, f.usageIndex, f.format);
      continue;
    }
    VkVertexInputAttributeDescription attr{};
    attr.location = uint32_t(location);
    attr.binding = 0;
    attr.format = format;
    attr.offset = attr_offset_dwords * 4;
    attrs.push_back(attr);
  }
  {
    static std::unordered_map<uint64_t, bool> logged;
    if (logged.emplace(vs->cache_hash, true).second) {
      std::printf("[vk] pipeline vs=%llX stride=%u prim=%u %zu attrs:\n",
                  (unsigned long long)vs->cache_hash, key.stride, key.topology,
                  attrs.size());
      for (const auto& a : attrs) {
        std::printf("      loc=%u fmt=%d off=%u\n", a.location, int(a.format),
                    a.offset);
      }
    }
  }
  // Template shaders: inputs the bound declaration lacks read zeros from
  // binding 1 (a 16-byte zero block, stride 0).
  for (const ShaderInfo::ZeroInput& z : vs->zero_inputs) {
    VkVertexInputAttributeDescription attr{};
    attr.location = z.location;
    attr.binding = 1;
    attr.format = z.integer ? VK_FORMAT_R32G32B32A32_UINT
                            : VK_FORMAT_R32G32B32A32_SFLOAT;
    attr.offset = 0;
    attrs.push_back(attr);
  }
  VkVertexInputBindingDescription bindings[2] = {};
  bindings[0].binding = 0;
  bindings[0].stride = key.stride;
  bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  bindings[1].binding = 1;
  bindings[1].stride = 0;
  bindings[1].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
  VkPipelineVertexInputStateCreateInfo vin{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vin.vertexBindingDescriptionCount =
      attrs.empty() ? 0 : (vs->zero_inputs.empty() ? 1 : 2);
  vin.pVertexBindingDescriptions = bindings;
  vin.vertexAttributeDescriptionCount = uint32_t(attrs.size());
  vin.pVertexAttributeDescriptions = attrs.data();

  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VkPrimitiveTopology(key.topology);
  // Xenos strips separate sub-strips with 0xFFFF cut indices.
  ia.primitiveRestartEnable =
      ia.topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ? VK_TRUE : VK_FALSE;

  // Viewport/scissor are dynamic: multi-pass replay renders at each pass's
  // attachment size (set at pass begin).
  VkPipelineViewportStateCreateInfo vp{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                      VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dynamic_states;

  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
  if (key.rs_valid && options_.use_cull) {
    // PA_SU_SC_MODE_CNTL: bit0 cull_front, bit1 cull_back, bit2 face (1 =
    // CW is front). Winding vs the backend's no-flip convention is unverified,
    // hence opt-in (--vkcull).
    VkCullModeFlags cm = 0;
    if (key.cull & 1u) cm |= VK_CULL_MODE_FRONT_BIT;
    if (key.cull & 2u) cm |= VK_CULL_MODE_BACK_BIT;
    rs.cullMode = cm;
    rs.frontFace = (key.cull & 4u) ? VK_FRONT_FACE_CLOCKWISE
                                   : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  }
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = key.samples == 4   ? VK_SAMPLE_COUNT_4_BIT
                            : key.samples == 2 ? VK_SAMPLE_COUNT_2_BIT
                                               : VK_SAMPLE_COUNT_1_BIT;

  VkPipelineDepthStencilStateCreateInfo ds{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  ds.depthTestEnable = key.depth ? VK_TRUE : VK_FALSE;
  ds.depthWriteEnable = key.depth ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = key.depth == 2 ? VK_COMPARE_OP_ALWAYS
                                     : VK_COMPARE_OP_LESS_OR_EQUAL;
  if (key.rs_valid && key.depth != 2) {
    // RB_DEPTHCONTROL: bit1 z_enable, bit2 z_write, bits 4-6 zfunc (Xenos
    // compare order == VkCompareOp order).
    ds.depthTestEnable = (key.depth_ctl & 0x2u) ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = (key.depth_ctl & 0x4u) ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VkCompareOp((key.depth_ctl >> 4) & 0x7u);
    if (!ds.depthTestEnable) ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
  }

  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = 0xF;
  if (key.blend) {
    blend.blendEnable = VK_TRUE;
    blend.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.colorBlendOp = VK_BLEND_OP_ADD;
    blend.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend.alphaBlendOp = VK_BLEND_OP_ADD;
  }
  if (key.rs_valid) {
    // RB_BLENDCONTROL: src 0-4, op 5-7, dst 8-12; alpha src 16-20, op 21-23,
    // dst 24-28. Xenos BlendFactor / BlendOp -> Vulkan.
    auto factor = [](uint32_t f) -> VkBlendFactor {
      switch (f) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 4: return VK_BLEND_FACTOR_SRC_COLOR;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_COLOR;
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 10: return VK_BLEND_FACTOR_DST_ALPHA;
        case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        default: return VK_BLEND_FACTOR_ONE;
      }
    };
    auto op = [](uint32_t o) -> VkBlendOp {
      switch (o) {
        case 1: return VK_BLEND_OP_SUBTRACT;
        case 2: return VK_BLEND_OP_MIN;
        case 3: return VK_BLEND_OP_MAX;
        case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
        default: return VK_BLEND_OP_ADD;
      }
    };
    blend.blendEnable = key.blend_ctl != 0 ? VK_TRUE : VK_FALSE;
    if (blend.blendEnable) {
      const uint32_t b = key.blend_ctl;
      blend.srcColorBlendFactor = factor(b & 0x1Fu);
      blend.colorBlendOp = op((b >> 5) & 0x7u);
      blend.dstColorBlendFactor = factor((b >> 8) & 0x1Fu);
      blend.srcAlphaBlendFactor = factor((b >> 16) & 0x1Fu);
      blend.alphaBlendOp = op((b >> 21) & 0x7u);
      blend.dstAlphaBlendFactor = factor((b >> 24) & 0x1Fu);
    }
  }
  VkPipelineColorBlendStateCreateInfo cb{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;
  if (options_.flat_ps) {
    // Invert the pixel-shader output: a black-outputting PS becomes white
    // wherever geometry actually rasterizes. Proves rasterization.
    blend.blendEnable = VK_FALSE;
    cb.logicOpEnable = VK_TRUE;
    cb.logicOp = VK_LOGIC_OP_COPY_INVERTED;
  }

  VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
  VkPipelineRenderingCreateInfo rendering{
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &color_format;
  rendering.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;

  VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pci.pNext = &rendering;
  pci.stageCount = 2;
  pci.pStages = stages;
  pci.pVertexInputState = &vin;
  pci.pInputAssemblyState = &ia;
  pci.pViewportState = &vp;
  pci.pRasterizationState = &rs;
  pci.pMultisampleState = &ms;
  pci.pDepthStencilState = &ds;
  pci.pColorBlendState = &cb;
  pci.pDynamicState = &dyn;
  pci.layout = pipeline_layout_;

  VkPipeline pipeline = VK_NULL_HANDLE;
  ++pipelines_unsaved_;
  last_pipeline_built_ = std::chrono::steady_clock::now();
  if (vkCreateGraphicsPipelines(device_, pipeline_cache_, 1, &pci, nullptr,
                                &pipeline) != VK_SUCCESS) {
    std::printf("[vk] pipeline creation failed (vs=%016llX ps=%016llX)\n",
                static_cast<unsigned long long>(key.vs_hash),
                static_cast<unsigned long long>(key.ps_hash));
    pipeline = VK_NULL_HANDLE;
  }
  pipelines_[key] = pipeline;
  return pipeline;
}

// Minimal RGBA -> PNG (stored deflate), alpha dropped.
static void PngBE32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(uint8_t(x >> 24));
  v.push_back(uint8_t(x >> 16));
  v.push_back(uint8_t(x >> 8));
  v.push_back(uint8_t(x));
}

uint32_t PngCrc(const uint8_t* a, size_t an, const uint8_t* b, size_t bn) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < an; ++i) c = table[(c ^ a[i]) & 0xFF] ^ (c >> 8);
  for (size_t i = 0; i < bn; ++i) c = table[(c ^ b[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

void PngChunk(std::vector<uint8_t>& png, const char* type,
              const std::vector<uint8_t>& data) {
  PngBE32(png, uint32_t(data.size()));
  const size_t at = png.size();
  png.insert(png.end(), type, type + 4);
  png.insert(png.end(), data.begin(), data.end());
  PngBE32(png, PngCrc(png.data() + at, 4, data.data(), data.size()));
}

bool WriteRgbaPng(const std::string& path, const std::vector<uint8_t>& rgba,
                  uint32_t width, uint32_t height) {
  std::vector<uint8_t> raw;
  raw.reserve(size_t(height) * (1 + size_t(width) * 3));
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* px = &rgba[(size_t(y) * width + x) * 4];
      raw.push_back(px[0]);
      raw.push_back(px[1]);
      raw.push_back(px[2]);
    }
  }
  std::vector<uint8_t> z;
  z.push_back(0x78);
  z.push_back(0x01);
  size_t pos = 0;
  while (pos < raw.size()) {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n == raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back(uint8_t(n & 0xFF));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n & 0xFF));
    z.push_back(uint8_t((~n >> 8) & 0xFF));
    z.insert(z.end(), raw.begin() + pos, raw.begin() + pos + n);
    pos += n;
  }
  uint32_t a = 1, b = 0;
  for (uint8_t byte : raw) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  PngBE32(z, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  PngBE32(ihdr, width);
  PngBE32(ihdr, height);
  ihdr.push_back(8);
  ihdr.push_back(2);
  ihdr.push_back(0);
  ihdr.push_back(0);
  ihdr.push_back(0);
  PngChunk(png, "IHDR", ihdr);
  PngChunk(png, "IDAT", z);
  PngChunk(png, "IEND", {});
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fwrite(png.data(), 1, png.size(), f);
  std::fclose(f);
  return true;
}

bool Backend::WriteBmp(const std::string& path,
                       const std::vector<uint8_t>& rgba) {
  const uint32_t row_bytes = (kWidth * 3 + 3) & ~3u;
  const uint32_t image_bytes = row_bytes * kHeight;
  uint8_t header[54] = {};
  header[0] = 'B';
  header[1] = 'M';
  const uint32_t file_size = 54 + image_bytes;
  std::memcpy(header + 2, &file_size, 4);
  const uint32_t data_offset = 54, dib_size = 40;
  std::memcpy(header + 10, &data_offset, 4);
  std::memcpy(header + 14, &dib_size, 4);
  const int32_t w = kWidth, h = kHeight;
  std::memcpy(header + 18, &w, 4);
  std::memcpy(header + 22, &h, 4);
  const uint16_t planes = 1, bpp = 24;
  std::memcpy(header + 26, &planes, 2);
  std::memcpy(header + 28, &bpp, 2);
  std::memcpy(header + 34, &image_bytes, 4);
  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) return false;
  std::fwrite(header, 1, sizeof(header), f);
  std::vector<uint8_t> row(row_bytes, 0);
  for (int y = int(kHeight) - 1; y >= 0; --y) {
    for (uint32_t x = 0; x < kWidth; ++x) {
      const uint8_t* px = &rgba[(size_t(y) * kWidth + x) * 4];
      row[x * 3 + 0] = px[2];
      row[x * 3 + 1] = px[1];
      row[x * 3 + 2] = px[0];
    }
    std::fwrite(row.data(), 1, row.size(), f);
  }
  std::fclose(f);
  return true;
}

bool Backend::Init(const RenderOptions& options) {
  options_ = options;
  if (!shaders_.Init(options.shaders_dir)) return false;
  {
    const uint32_t scale = std::min(4u, std::max(1u, options.resolution_scale));
    out_w_ = options.internal_width != 0 ? options.internal_width : kWidth * scale;
    out_h_ = options.internal_height != 0 ? options.internal_height : kHeight * scale;
    ClampInternal(out_w_, out_h_);
    pending_w_ = out_w_;
    pending_h_ = out_h_;
  }
  if (options.shader_lookup != nullptr) shaders_.SetLiveLookup(options.shader_lookup);
  if (options.container_source != nullptr) shaders_.SetLiveContainerSource(options.container_source);
  shaders_.SetPreferTemplates(options.prefer_vs_templates);
  {
    std::error_code ec;
    for (const auto& entry :
         std::filesystem::directory_iterator(options.textures_dir, ec)) {
      unsigned phys = 0;
      unsigned long long hash = 0;
      if (std::sscanf(entry.path().filename().string().c_str(), "tex_%8X_%16llX",
                      &phys, &hash) == 2) {
        texture_paths_[phys] = entry.path().string();
        texture_paths_by_hash_[(uint64_t(phys) << 32) ^ hash] = entry.path().string();
      }
    }
  }
  if (!InitVulkan(options.validation)) return false;
  CreatePipelineCache();
  if (!CreateTargets()) return false;
  if (!CreateDescriptors()) return false;
  if (options_.window) {
    if (!CreateWindowSurface() || !CreateSwapchain()) return false;
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (FrameSlot& slot : slots_) {
      VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &slot.acquire_sem));
      VK_CHECK(vkCreateSemaphore(device_, &sci, nullptr, &slot.render_sem));
    }
    UseSlot(slot_);
  }

  // Fallback textures live at fixed heap indices; upload them once.
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  VK_CHECK(vkBeginCommandBuffer(cmd_, &begin));
  UploadFallbacks();
  VK_CHECK(vkEndCommandBuffer(cmd_));
  {
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    VK_CHECK(vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(queue_));
  }
  arena_used_ = 0;
  cbuf_used_ = 0;
  initialized_ = true;
  return true;
}

bool Backend::Run(const FrameData& frame, const RenderOptions& options) {
  if (!initialized_ && !Init(options)) return false;
  std::vector<uint8_t> rgba;
  if (!RenderFrame(frame, rgba)) return false;
  const bool ok = WriteBmp(options.output_bmp, rgba);
  std::printf("[vk] frame %llu -> %s (%s)\n",
              static_cast<unsigned long long>(frame.index),
              options.output_bmp.c_str(), ok ? "ok" : "WRITE FAILED");
  return ok;
}

bool Backend::RenderFrame(const FrameData& frame, std::vector<uint8_t>& rgba,
                          bool want_rgba) {
  if (!initialized_) return false;
  if (!options_.dump_passes_dir.empty()) want_rgba = true;
  // Profiling (2026-09-15): CPU recording vs GPU (submit -> idle), averaged
  // over 120 frames into the print log.
  const auto t_frame0 = std::chrono::steady_clock::now();
  recording_ = true;
  stat_tex_n_ = stat_pipe_n_ = 0;
  stat_tex_fetch_ns_ = stat_tex_up_ns_ = stat_pipe_ns_ = 0;
  const RenderOptions& options = options_;
  // Per-frame state: the previous frame's GPU work is complete (we wait idle
  // after every submit), so the upload arena and command buffer are free.
  // Attachments, resolve targets, textures, pipelines persist — resolve
  // targets deliberately keep last frame's content (temporal feedback).
  arena_peak_ = std::max(arena_peak_, arena_used_);  // what the last frame used
  arena_peak_ever_ = std::max(arena_peak_ever_, arena_used_);
  if ((arena_grow_ || arena_peak_ever_ > arena_bytes_ / 2) && arena_bytes_ < kArenaMaxBytes) {
    WaitFlight();
    vkDeviceWaitIdle(device_);
    arena_bytes_ = std::min(kArenaMaxBytes, arena_bytes_ * 2);
    for (FrameSlot& slot : slots_) {
      DestroyArena(slot);
      if (!CreateArena(slot)) return false;
    }
    UseSlot(slot_);
    std::printf("[vk] upload buffers grown to %.0f MB each\n", double(arena_bytes_) / 1048576.0);
  }
  arena_grow_ = false;
  arena_used_ = 0;
  cbuf_used_ = 0;
  PumpMessages();
  if (options_.window && !window_open_) return false;
  if (options_.window && sc_extent_from_owner_ && options_.window_size != nullptr) {
    // No OUT_OF_DATE on such surfaces: follow the owner's size ourselves.
    uint32_t w = 0, h = 0;
    options_.window_size(&w, &h);
    if (w != 0 && h != 0 && (w != sc_extent_.width || h != sc_extent_.height)) {
      swapchain_dirty_ = true;
    }
  }
#if defined(__ANDROID__)
  if (options_.window && swapchain_ != VK_NULL_HANDLE && !swapchain_dirty_) {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps) == VK_SUCCESS) {
      const VkExtent2D e = WindowExtent(caps);
      if (e.width != sc_extent_.width || e.height != sc_extent_.height ||
          (sc_quarter_turns_ != 0 || options_.prerotate) && caps.currentTransform != sc_transform_) {
        swapchain_dirty_ = true;
      }
    }
  }
#endif
  if (options_.window && swapchain_dirty_) {
    vkDeviceWaitIdle(device_);
    DestroySwapchain();
    if (!CreateSwapchain()) return false;
  }
  // The other slot: its last frame (two frames ago) finished before the
  // previous frame was submitted.
  UseSlot(slot_ ^ 1u);
  if (slots_[slot_].in_flight) WaitFlight();
  GpuCollect();
  VK_CHECK(vkResetCommandBuffer(cmd_, 0));
  ++frame_counter_;
  // Headroom for one frame's worth of new content (a fight binds ~200
  // textures) plus its resolve targets.
  if (options_.anisotropy != sampler_anisotropy_) CreateTextureSampler();
  // Resolution scale changed (settings page): every scaled image goes; the
  // frame rebuilds what it uses.
  const bool rescale = pending_w_ != out_w_ || pending_h_ != out_h_;
  if (rescale) {
    vkDeviceWaitIdle(device_);
    for (auto& [key, rt] : rt_pool_) DestroyRt(rt);
    rt_pool_.clear();
    if (present_src_view_) vkDestroyImageView(device_, present_src_view_, nullptr);
    if (present_src_) vkDestroyImage(device_, present_src_, nullptr);
    if (present_src_mem_) vkFreeMemory(device_, present_src_mem_, nullptr);
    present_src_view_ = VK_NULL_HANDLE;
    present_src_ = VK_NULL_HANDLE;
    present_src_mem_ = VK_NULL_HANDLE;
    DestroyDepthStage();
    std::printf("[vk] internal resolution %ux%u -> %ux%u\n", out_w_, out_h_,
                pending_w_, pending_h_);
    out_w_ = pending_w_;
    out_h_ = pending_h_;
  }
  const bool heap_reset = rescale || texture_reload_ || texture_count_ + 320 > kMaxTextures ||
                          cube_count_ + 16 > kMaxCubeTextures;
  texture_reload_ = false;
  if (heap_reset) ResetTextureHeap();
  if (!retired_images_.empty()) WaitFlight();  // the frame in flight may use them
  DestroyRetired();
  for (auto& [key, rt] : rt_pool_) {
    if (rt.ms_pending_samples != 0 && rt.ms_color != VK_NULL_HANDLE) {
      WaitFlight();
      std::printf("[vk] rt %08llX: multisample set %ux -> %ux (recreated)\n",
                  static_cast<unsigned long long>(key), rt.ms_samples,
                  rt.ms_pending_samples);
      DestroyMultisample(rt);
    }
  }
  for (auto it = rt_pool_.begin(); it != rt_pool_.end();) {
    if (it->first != 0 && it->second.last_used + 120 < frame_counter_) {
      DestroyRt(it->second);
      it = rt_pool_.erase(it);
    } else {
      // 2026-09-15: EDRAM persists across frames — a surface is cleared only
      // by the title (versus screen: the main surface is never cleared, and the
      // alpha the previous frame left decides what the front-buffer quads cover).
      // it->second.cleared_once = false;
      ++it;
    }
  }
  if (!options_.dump_passes_dir.empty()) {
    // Per-pass debugger: the persistent state this frame starts from.
    for (const auto& [key, rt] : rt_pool_) {
      std::printf("[vk] rt-pool %08llX %ux%u ms=%u last_used=%llu\n",
                  static_cast<unsigned long long>(key), rt.width, rt.height,
                  rt.ms_samples, static_cast<unsigned long long>(rt.last_used));
    }
    std::printf("[vk] textures=%u cube=%u resolve_dsts=%zu frame_counter=%llu\n",
                texture_count_, cube_count_, resolve_dsts_.size(),
                static_cast<unsigned long long>(frame_counter_));
  }
  VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  VK_CHECK(vkBeginCommandBuffer(cmd_, &begin));
  if (gpu_profile_frames_ > 0) {
    if (gpu_pool_[slot_] == VK_NULL_HANDLE) {
      VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qi.queryCount = kGpuQueries;
      vkCreateQueryPool(device_, &qi, nullptr, &gpu_pool_[slot_]);
      VkPhysicalDeviceProperties props{};
      vkGetPhysicalDeviceProperties(phys_, &props);
      gpu_ms_per_tick_ = double(props.limits.timestampPeriod) / 1e6;
    }
    --gpu_profile_frames_;
    vkCmdResetQueryPool(cmd_, gpu_pool_[slot_], 0, kGpuQueries);
    gpu_used_[slot_] = 0;
    gpu_info_[slot_].clear();
    GpuStamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
  }
  if (heap_reset) UploadFallbacks();
  // Picture shapes other than 16:9: frames that render the 3D scene (they
  // run a scene composite) show more of it outside the 16:9 band. Screens
  // without 3D are composed for 16:9 - backdrops spill into the extra strips
  // (the versus screen, 2026-10-02); since 2026-10-12 their backdrop
  // tiles are widened to cover the picture instead (Layout2D), and
  // aspect_mask_2d brings back the old black strips.
  frame_is_scene_ = false;
  bottom_bar_top_ = 1e30f;
  for (const DrawRecord& d : frame.draws) {
    uint64_t ps_id = d.ps_container;
    if (ps_id == 0) {  // captures before v15 name shaders by address
      const ShaderInfo* ps = shaders_.FindByVa(d.ps_object);
      ps_id = ps != nullptr ? ps->container_fnv : 0;
    }
    // Fights' composite, and the menus' (A451BBFF: main menu and match
    // modes over the arena, 2026-10-12).
    if (ps_id == 0xCB9A5EB75CB8B59Eull || ps_id == 0xA451BBFF044CB389ull) {
      frame_is_scene_ = true;
      break;
    }
  }
  if (!options_.quiet && options_.picture_aspect > 0.0f) {
    std::printf("[vk] frame %llu (v%u): scene composite %s\n",
                static_cast<unsigned long long>(frame.index), frame.version,
                frame_is_scene_ ? "present" : "absent");
  }
  if (!overlay_ops_.empty() && options_.window && swapchain_ != VK_NULL_HANDLE &&
      EnsurePresentResources()) {
    WaitFlight();  // replaced textures are destroyed on the spot
    ProcessOverlayOps();
  }
  // v13: the frame names the exact content of each texture it sampled;
  // prefer that dump over whichever file for the phys came last in the
  // directory (textures stream in after their first bind).
  for (const DrawRecord& d : frame.draws) {
    for (uint32_t t = 0; t < rs::capture::kTextureSlots; ++t) {
      if (d.textures[t] == 0 || d.texture_hash[t] == 0) continue;
      const auto it = texture_paths_by_hash_.find(
          (uint64_t(d.textures[t]) << 32) ^ d.texture_hash[t]);
      if (it != texture_paths_by_hash_.end()) {
        texture_paths_[d.textures[t]] = it->second;
      }
    }
  }

  // Pre-pass: constants are replayed on the CPU, so all draw resources are
  // known before recording; upload each draw's textures first (copies must
  // happen outside dynamic rendering).
  if (options_.texture_source != nullptr) PrefetchTextures(frame);
  for (const DrawRecord& d : frame.draws) {
    for (uint32_t s = 0; s < rs::capture::kTextureSlots; ++s) {
      if (d.textures[s] != 0) UploadTexture(d.textures[s], d.texture_hash[s]);
    }
  }
  prefetched_.clear();

  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline_layout_, 0, 4, sets_, 0, nullptr);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline_layout_, 4, 1, &cset_, 0, nullptr);

  if (!options_.quiet) std::printf("[vk] %zu resolve records\n", frame.resolves.size());
  for (size_t i = 0; !options_.quiet && i < frame.resolves.size() && i < 40; ++i) {
    const rs::capture::ResolveRecord& r = frame.resolves[i];
    std::printf("[vk]   resolve[%zu] cmd=%u obj=%08X phys=%08X %ux%u fmt=%u\n",
                i, r.command_index, r.tex_object, r.phys,
                (r.fetch[2] & 0x1FFF) + 1, ((r.fetch[2] >> 13) & 0x1FFF) + 1,
                r.fetch[1] & 0x3F);
  }

  // ---- Multi-pass state -----------------------------------------------
  // Passes are delimited by SetRenderTargetOrDS (39); each pass's
  // attachment size comes from the next Resolve's destination dims
  // (fallback: full frame). Clear (52) becomes the next pass's loadOp,
  // Resolve (53) becomes a copy into the destination texture.
  size_t next_resolve = 0;  // index into frame.resolves, sorted by cmd index
  bool rendering_active = false;
  bool pass_resumable = false;  // ended by resolve/snapshot, not SetRT
  uint32_t pass_samples = 1;    // requested for the next pass (v14 msaa)
  uint32_t active_samples = 1;  // what the open pass renders with
  bool pending_clear = false;
  uint32_t pending_clear_flags = 0x3F;
  VkClearColorValue pending_clear_color{{0.0f, 0.0f, 0.0f, 1.0f}};
  // Faithful EDRAM model: ONE shared full-frame attachment for every pass;
  // the game's viewport (type 33) bounds each pass's render area, clears
  // clear the viewport rect, resolves copy a rect out. (Sizing passes by
  // their resolve dims was wrong — Xenos resolve cannot scale.)
  uint32_t vp_x = 0, vp_y = 0, vp_w = kWidth, vp_h = kHeight;
  uint64_t cur_key = 0;  // guest RT surface ptr; 0 = boot default
  RtEntry* final_rt = GetRt(0);
  bool ao_done = false;  // ambient occlusion ran this frame
  RtEntry* cur_rt = final_rt;
  RtEntry* last_draw_rt = final_rt;  // presented surface = last drawn into
  if (final_rt == nullptr) return false;

  uint32_t pass_area_w = 0, pass_area_h = 0;  // the open pass's render area (guest px)
  auto set_viewport = [&]() {
    const float fx = float(out_w_) / float(kWidth);
    const float fy = float(out_h_) / float(kHeight);
    VkViewport viewport{float(vp_x) * fx, float(vp_y) * fy, float(vp_w) * fx,
                        float(vp_h) * fy, 0.0f, 1.0f};
    if (options_.flip_y) {
      viewport.y = float(vp_y + vp_h) * fy;
      viewport.height = -float(vp_h) * fy;
    }
    // Viewports larger than the attachment are legal (1024x1024 shadow pass
    // on a 720p-sized attachment); the scissor clamps to the attachment -
    // and to the pass's render area (used_w / used_h; a viewport that grows
    // past it mid-pass is clipped this frame and covered from the next).
    uint32_t aw = cur_rt ? cur_rt->width : kAttachW;
    uint32_t ah = cur_rt ? cur_rt->height : kAttachH;
    if (cur_rt != nullptr && !options_.full_render_area && cur_rt->used_w != 0) {
      const uint32_t grow_w = std::min(cur_rt->width, (vp_x + vp_w + 31u) & ~31u);
      const uint32_t grow_h = std::min(cur_rt->height, (vp_y + vp_h + 31u) & ~31u);
      if (rendering_active && pass_area_w != 0) {
        cur_rt->used_w = std::max(cur_rt->used_w, grow_w);  // from the next pass on
        cur_rt->used_h = std::max(cur_rt->used_h, grow_h);
        aw = pass_area_w;
        ah = pass_area_h;
      }
    }
    const uint32_t sx = std::min(vp_x, aw);
    const uint32_t sy = std::min(vp_y, ah);
    // Edges are scaled (not sizes) so adjacent regions stay adjacent at
    // non-integer scales.
    VkRect2D scissor{{int32_t(SW(sx)), int32_t(SH(sy))},
                     {SW(sx + std::min(vp_w, aw - sx)) - SW(sx),
                      SH(sy + std::min(vp_h, ah - sy)) - SH(sy)}};
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
  };
  // Xbox 360 clear flags: 0x0F = color targets 0-3, 0x10 = Z, 0x20 = stencil.
  auto clear_viewport_rect = [&](uint32_t flags,
                                 const VkClearColorValue& color) {
    VkClearAttachment clears[2] = {};
    uint32_t count = 0;
    if (flags & 0x0Fu) {
      clears[count].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      clears[count].colorAttachment = 0;
      clears[count].clearValue.color = color;
      ++count;
    }
    if (flags & 0x30u) {
      clears[count].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      clears[count].clearValue.depthStencil = {1.0f, 0};
      ++count;
    }
    if (count == 0) return;
    // Within the pass's render area (a clear rect must be).
    const uint32_t x1 = pass_area_w != 0 ? std::min(vp_x + vp_w, pass_area_w) : vp_x + vp_w;
    const uint32_t y1 = pass_area_h != 0 ? std::min(vp_y + vp_h, pass_area_h) : vp_y + vp_h;
    if (x1 <= vp_x || y1 <= vp_y) return;
    VkClearRect rect{{{int32_t(SW(vp_x)), int32_t(SH(vp_y))},
                      {SW(x1) - SW(vp_x), SH(y1) - SH(vp_y)}}, 0, 1};
    vkCmdClearAttachments(cmd_, count, clears, 1, &rect);
  };

  auto begin_pass = [&]() {
    // EDRAM aliasing approximation: every full-frame-viewport pass renders
    // to the same physical region on hardware (scene, then UI on top of it)
    // even when bound through different D3D surface objects. Alias those to
    // one "main" attachment; small-viewport passes (shadows, bloom, motion
    // blur) get their own per-surface attachments.
    const bool full_frame = vp_w >= 1200 && vp_h >= 600 && vp_h <= kHeight;
    cur_rt = GetRt(full_frame ? 0 : cur_key, vp_x + vp_w, vp_y + vp_h);
    if (cur_rt == nullptr) return;
    // Color to attachment layout.
    if (cur_rt->layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
      VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                              VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
      barrier.oldLayout = cur_rt->layout;
      barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      barrier.image = cur_rt->color;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &barrier;
      vkCmdPipelineBarrier2(cmd_, &dep);
      cur_rt->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (!cur_rt->depth_ready) {
      VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT;
      barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
      barrier.image = cur_rt->depth;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
      VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      dep.imageMemoryBarrierCount = 1;
      dep.pImageMemoryBarriers = &barrier;
      vkCmdPipelineBarrier2(cmd_, &dep);
      cur_rt->depth_ready = true;
    }

    // Multisampling (v14): the pass renders into the MS images and resolves
    // into the single-sample ones at its end.
    if (cur_rt->frame_samples_frame == frame_counter_) {
      pass_samples = cur_rt->frame_samples;  // fixed by this frame's first pass
    } else {
      cur_rt->frame_samples_frame = frame_counter_;
      cur_rt->frame_samples = pass_samples;
    }
    bool use_ms = false;
    if (pass_samples > 1) {
      if (EnsureMultisample(cur_rt, pass_samples)) {
        use_ms = true;
        VkImageMemoryBarrier2 b[2] = {};
        for (int i = 0; i < 2; ++i) {
          b[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
          b[i].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
          b[i].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        }
        b[0].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b[0].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
        b[0].oldLayout = cur_rt->ms_ready ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                          : VK_IMAGE_LAYOUT_UNDEFINED;
        b[0].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b[0].image = cur_rt->ms_color;
        b[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b[1].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b[1].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
        b[1].oldLayout = cur_rt->ms_ready ? VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
                                          : VK_IMAGE_LAYOUT_UNDEFINED;
        b[1].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        b[1].image = cur_rt->ms_depth;
        b[1].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 2;
        dep.pImageMemoryBarriers = b;
        vkCmdPipelineBarrier2(cmd_, &dep);
        cur_rt->ms_ready = true;
      } else {
        static bool warned = false;
        if (!warned) {
          std::printf("[vk] multisampled attachments unavailable — rendering 1x\n");
          warned = true;
        }
        cur_rt->ms_failed_frame = frame_counter_;
        pass_samples = 1;
      }
    }
    active_samples = pass_samples;

    VkRenderingAttachmentInfo color_att{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color_att.imageView = use_ms ? cur_rt->ms_color_view : cur_rt->color_view;
    color_att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color_att.loadOp = cur_rt->cleared_once ? VK_ATTACHMENT_LOAD_OP_LOAD
                                            : VK_ATTACHMENT_LOAD_OP_CLEAR;
    if (options_.trace) {
      std::printf("[trace] begin pass key=%08llX rt=%p %ux%u main=%d load=%s "
                  "samples=%u resumable=%d vp=%ux%u\n",
                  static_cast<unsigned long long>(cur_key),
                  static_cast<void*>(cur_rt), cur_rt->width, cur_rt->height,
                  cur_rt == GetRt(0) ? 1 : 0,
                  cur_rt->cleared_once ? "LOAD" : "CLEAR", pass_samples,
                  pass_resumable ? 1 : 0, vp_w, vp_h);
    }
    color_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color_att.clearValue.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    if (use_ms) {
      color_att.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
      color_att.resolveImageView = cur_rt->color_view;
      color_att.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkRenderingAttachmentInfo depth_att{
        VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depth_att.imageView = use_ms ? cur_rt->ms_depth_view : cur_rt->depth_view;
    depth_att.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    if (use_ms) {
      depth_att.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
      depth_att.resolveImageView = cur_rt->depth_view;
      depth_att.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    }
    // Depth: every SetRT gets fresh EDRAM depth on hardware; only mid-pass
    // resumes (after a resolve or snapshot copy) keep it.
    depth_att.loadOp = (cur_rt->cleared_once && pass_resumable)
                           ? VK_ATTACHMENT_LOAD_OP_LOAD
                           : VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_att.clearValue.depthStencil = {1.0f, 0};
    VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
    {
      const uint32_t need_w = std::min(cur_rt->width, (vp_x + vp_w + 31u) & ~31u);
      const uint32_t need_h = std::min(cur_rt->height, (vp_y + vp_h + 31u) & ~31u);
      cur_rt->used_w = std::max(cur_rt->used_w, need_w);
      cur_rt->used_h = std::max(cur_rt->used_h, need_h);
      pass_area_w = options_.full_render_area ? cur_rt->width : cur_rt->used_w;
      pass_area_h = options_.full_render_area ? cur_rt->height : cur_rt->used_h;
      rendering.renderArea = {{0, 0}, {SW(pass_area_w), SH(pass_area_h)}};
    }
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &color_att;
    rendering.pDepthAttachment = &depth_att;
    if (gpu_used_[slot_] != 0) GpuStamp(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);
    vkCmdBeginRendering(cmd_, &rendering);
    rendering_active = true;
    cur_rt->cleared_once = true;
    set_viewport();
    if (pending_clear) {
      clear_viewport_rect(pending_clear_flags, pending_clear_color);
      pending_clear = false;
    }
  };
  // Per-pass dump bookkeeping (--vkdumppasses).
  struct PassDump {
    VkDeviceSize offset;
    uint64_t key;
    uint32_t index;
    uint32_t draws;
    uint32_t vp_w, vp_h;
  };
  std::vector<PassDump> pass_dumps;
  uint32_t pass_index = 0;
  uint32_t draws_in_pass = 0;

  auto end_pass = [&]() {
    if (!rendering_active) return;
    vkCmdEndRendering(cmd_);
    rendering_active = false;
    if (gpu_used_[slot_] != 0) {
      GpuStamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
      if (cur_rt != nullptr) {
        gpu_info_[slot_].push_back({SW(cur_rt->width), SH(cur_rt->height), draws_in_pass, 0});
      }
    }
    ++pass_index;
    if (!options_.dump_passes_dir.empty() && cur_rt != nullptr && Unscaled() &&
        draws_in_pass != 0 && arena_used_ < arena_bytes_ / 2) {
      // (arena guard: a frame with hundreds of passes must not starve the
      // draws that follow of constant/vertex space)
      const VkDeviceSize at = Alloc(VkDeviceSize(kWidth) * kHeight * 4, 16);
      if (at != VkDeviceSize(-1)) {
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barrier.oldLayout = cur_rt->layout;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.image = cur_rt->color;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd_, &dep);
        cur_rt->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkBufferImageCopy copy{};
        copy.bufferOffset = at;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {std::min(kWidth, cur_rt->width),
                            std::min(kHeight, cur_rt->height), 1};
        vkCmdCopyImageToBuffer(cmd_, cur_rt->color,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, arena_,
                               1, &copy);
        pass_dumps.push_back({at, cur_rt == GetRt(0) ? 0 : cur_key,
                              pass_index, draws_in_pass, vp_w, vp_h});
      }
    }
    draws_in_pass = 0;
  };

  // Draws sampling the front buffer see the previously presented frame
  // (written by the Swap-time resolve, outside the command stream). We
  // approximate with a snapshot of the main attachment at first use, per id.
  if (!options_.quiet) std::printf("[vk] front_buffer_phys=%08X\n", frame.front_buffer_phys);
  std::unordered_map<uint32_t, bool> frame_resolve_targets;
  for (const rs::capture::ResolveRecord& r : frame.resolves) {
    if (r.phys != 0) frame_resolve_targets[r.phys] = true;
  }
  std::unordered_map<uint32_t, bool> front_done;
  std::unordered_map<uint32_t, bool> front_done_direct;
  // Copies |rt|'s depth attachment (D32) into an R32_SFLOAT sampled image
  // for texture id |phys| — used for depth-format resolves (shadow maps) and
  // for orphan depth ids (the DOF composite's full-screen depth). Goes
  // through the arena: D32 -> buffer -> R32 image (no direct depth->colour
  // image copies in Vulkan).
  // --vktrace: value statistics of every depth copy, printed after the frame.
  struct DepthDbg { VkDeviceSize at; uint32_t w, h, phys; };
  std::vector<DepthDbg> depth_dbg;
  auto copy_depth_to = [&](RtEntry* rt, uint32_t phys, uint32_t w,
                           uint32_t h) {
    if (rt == nullptr || rt->depth == VK_NULL_HANDLE) return;
    end_pass();
    pass_resumable = true;
    w = std::min(w, rt->width);
    h = std::min(h, rt->height);
    if (depth_direct_ == 0) {
      VkFormatProperties fp{};
      vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_D32_SFLOAT, &fp);
      const VkFormatFeatureFlags need =
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
      depth_direct_ = (fp.optimalTilingFeatures & need) == need ? 1 : 2;
    }
    // --vktrace reads the staged depth back for its statistics.
    const bool direct = depth_direct_ == 1 && !options_.trace;
    const VkFormat dst_format = direct ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R32_SFLOAT;
    const VkImageAspectFlags dst_aspect =
        direct ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    ResolveDst& dst = resolve_dsts_[phys];
    if (dst.image != VK_NULL_HANDLE &&
        (dst.width != w || dst.height != h || dst.format != dst_format)) {
      RetireResolveDst(dst);  // address reused at another size / kind: recreate
    }
    if (dst.image != VK_NULL_HANDLE && dst.depth_frame == frame_counter_ &&
        dst.depth_pass == pass_index && dst.depth_src == rt) {
      return;  // same depth already copied, nothing rendered since
    }
    dst.depth_frame = frame_counter_;
    dst.depth_pass = pass_index;
    dst.depth_src = rt;
    if (dst.image == VK_NULL_HANDLE) {
      dst.format = dst_format;
      if (!AllocImage(device_, phys_, SW(w), SH(h), dst_format,
                      VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      &dst.image, &dst.memory)) {
        return;
      }
      VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vci.image = dst.image;
      vci.format = dst_format;
      vci.subresourceRange = {dst_aspect, 0, 1, 0, 1};
      if (vkCreateImageView(device_, &vci, nullptr, &dst.view) != VK_SUCCESS) {
        return;
      }
      dst.width = w;
      dst.height = h;
      if (dst.heap_index == 0) dst.heap_index = texture_count_ < kMaxTextures ? texture_count_++ : 0;
      VkDescriptorImageInfo info{};
      info.imageView = dst.view;
      info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = sets_[0];
      write.dstBinding = 0;
      write.dstArrayElement = dst.heap_index;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &info;
      WaitFlight();  // the set may be in use by the frame in flight
      vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
      texture_indices_[phys] = dst.heap_index;
    }
    // Staged in GPU memory (the copies of one command buffer run in order,
    // so every copy reuses offset 0); --vktrace keeps the arena path because
    // it reads the staged depth back for its statistics.
    const bool stage_in_arena = !direct && (options_.trace || !EnsureDepthStage());
    const VkBuffer stage = stage_in_arena ? arena_ : depth_stage_;
    const VkDeviceSize at =
        stage_in_arena ? Alloc(VkDeviceSize(SW(w)) * (SH(h)) * 4, 16) : 0;
    if (at == VkDeviceSize(-1)) return;
    if (options_.trace) depth_dbg.push_back({at, SW(w), SH(h), phys});

    VkImageMemoryBarrier2 b[2] = {};
    b[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b[0].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b[0].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    b[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b[0].image = rt->depth;
    b[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    b[1] = b[0];
    b[1].srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b[1].oldLayout = dst.layout;
    b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b[1].image = dst.image;
    b[1].subresourceRange = {dst_aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = b;
    vkCmdPipelineBarrier2(cmd_, &dep);

    if (direct) {
      VkImageCopy copy{};
      copy.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copy.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
      copy.extent = {SW(w), SH(h), 1};
      vkCmdCopyImage(cmd_, rt->depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    } else {
    VkBufferImageCopy to_buf{};
    to_buf.bufferOffset = at;
    to_buf.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    to_buf.imageExtent = {SW(w), SH(h), 1};
    vkCmdCopyImageToBuffer(cmd_, rt->depth,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage, 1,
                           &to_buf);

    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo mdep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    mdep.memoryBarrierCount = 1;
    mdep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd_, &mdep);

    VkBufferImageCopy to_img{};
    to_img.bufferOffset = at;
    to_img.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    to_img.imageExtent = {SW(w), SH(h), 1};
    vkCmdCopyBufferToImage(cmd_, stage, dst.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &to_img);
    }

    VkImageMemoryBarrier2 after[2] = {};
    after[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    after[0].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    after[0].srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    after[0].dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    after[0].dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                             VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    after[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    after[0].newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    after[0].image = rt->depth;
    after[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    after[1] = after[0];
    after[1].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    after[1].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    after[1].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    after[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    after[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    after[1].image = dst.image;
    after[1].subresourceRange = {dst_aspect, 0, 1, 0, 1};
    VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep2.imageMemoryBarrierCount = 2;
    dep2.pImageMemoryBarriers = after;
    vkCmdPipelineBarrier2(cmd_, &dep2);
    dst.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (options_.trace) {
      std::printf("[trace] depth copy %08X %ux%u -> 2D heap %u\n", phys, w, h,
                  dst.heap_index);
    }
  };

  auto take_front_snapshot = [&](uint32_t front_phys) {
    end_pass();
    pass_resumable = true;
    ResolveDst& dst = resolve_dsts_[front_phys];
    if (dst.image != VK_NULL_HANDLE && dst.format == VK_FORMAT_D32_SFLOAT) {
      RetireResolveDst(dst);  // address was a depth copy: recreate as colour
    }
    if (dst.image == VK_NULL_HANDLE) {
      if (!AllocImage(device_, phys_, SW(kWidth), SH(kHeight), VK_FORMAT_R8G8B8A8_UNORM,
                      VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                      &dst.image, &dst.memory)) {
        return;
      }
      VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vci.image = dst.image;
      vci.format = VK_FORMAT_R8G8B8A8_UNORM;
      vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      if (vkCreateImageView(device_, &vci, nullptr, &dst.view) != VK_SUCCESS) {
        return;
      }
      dst.width = kWidth;
      dst.height = kHeight;
      if (dst.heap_index == 0) dst.heap_index = texture_count_ < kMaxTextures ? texture_count_++ : 0;
      VkDescriptorImageInfo info{};
      info.imageView = dst.view;
      info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      write.dstSet = sets_[0];
      write.dstBinding = 0;
      write.dstArrayElement = dst.heap_index;
      write.descriptorCount = 1;
      write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      write.pImageInfo = &info;
      WaitFlight();  // the set may be in use by the frame in flight
      vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
      texture_indices_[front_phys] = dst.heap_index;
    }
    if (options_.trace) {
      std::printf("[trace] front snapshot %08X -> 2D heap %u (textures=%u)\n",
                  front_phys, dst.heap_index, texture_count_);
    }
    if (last_draw_rt->width < kWidth || last_draw_rt->height < kHeight) {
      last_draw_rt = final_rt;  // a small scratch surface cannot be the frame
    }
    VkImageMemoryBarrier2 barriers[2] = {};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barriers[0].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[0].oldLayout = last_draw_rt->layout;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].image = last_draw_rt->color;
    barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barriers[1] = barriers[0];
    barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = dst.layout;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].image = dst.image;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = barriers;
    vkCmdPipelineBarrier2(cmd_, &dep);
    last_draw_rt->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {SW(kWidth), SH(kHeight), 1};
    vkCmdCopyImage(cmd_, last_draw_rt->color,
                   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

    VkImageLayout snap_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    if (!options_.dump_passes_dir.empty() && Unscaled()) {
      // Debug: write the snapshot alongside the pass dumps (snap_<phys>).
      const VkDeviceSize at = Alloc(VkDeviceSize(kWidth) * kHeight * 4, 16);
      if (at != VkDeviceSize(-1)) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.image = dst.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo bd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        bd.imageMemoryBarrierCount = 1;
        bd.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cmd_, &bd);
        VkBufferImageCopy c{};
        c.bufferOffset = at;
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageExtent = {kWidth, kHeight, 1};
        vkCmdCopyImageToBuffer(cmd_, dst.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, arena_, 1,
                               &c);
        pass_dumps.push_back({at, uint64_t(front_phys), 900u + pass_index, 0,
                              kWidth, kHeight});
        snap_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      }
    }

    VkImageMemoryBarrier2 to_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT |
                            VK_ACCESS_2_TRANSFER_READ_BIT;
    to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    to_read.oldLayout = snap_layout;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.image = dst.image;
    to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep2.imageMemoryBarrierCount = 1;
    dep2.pImageMemoryBarriers = &to_read;
    vkCmdPipelineBarrier2(cmd_, &dep2);
    dst.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    front_done[front_phys] = true;
    std::printf("[vk] front-buffer snapshot substituted for %08X\n",
                front_phys);
  };

  // CPU-side constant shadows, replayed in command-stream order. Seeded from
  // the capture's end-of-frame device snapshot (v7) so constants set in
  // EARLIER frames — lighting, materials, fades — carry over.
  std::vector<uint8_t> vs_consts(256 * 16, 0);
  std::vector<uint8_t> ps_consts(256 * 16, 0);
  uint32_t bool_dwords[8] = {};
  auto seed_bank = [](const std::vector<uint8_t>& src,
                      std::vector<uint8_t>& dst) {
    if (src.size() != dst.size()) return;
    for (size_t i = 0; i + 4 <= src.size(); i += 4) {
      const uint32_t v = LoadBE32(src.data() + i);
      std::memcpy(&dst[i], &v, 4);
    }
  };
  seed_bank(frame.vs_const_bank, vs_consts);
  seed_bank(frame.ps_const_bank, ps_consts);
  if (frame.bool_bank.size() == 32) {
    for (size_t i = 0; i < 8; ++i) {
      bool_dwords[i] = LoadBE32(frame.bool_bank.data() + i * 4);
    }
  }
  auto apply_const_f = [&](const uint8_t* cmd, uint32_t byte_count,
                           std::vector<uint8_t>& bank) {
    if (byte_count < 48) return;
    const uint32_t start = LoadBE32(cmd + 16);
    const uint32_t count = LoadBE32(cmd + 20);
    for (uint32_t i = 0; i < count && start + i < 256; ++i) {
      if (32 + (i + 1) * 16 > byte_count) break;
      for (int k = 0; k < 4; ++k) {
        const uint32_t v = LoadBE32(cmd + 32 + i * 16 + k * 4);
        std::memcpy(&bank[(start + i) * 16 + k * 4], &v, 4);
      }
    }
  };

  size_t inline_cursor = 0, indexed_cursor = 0;
  uint32_t drawn = 0, skipped = 0;
  size_t stride_mismatch_logged = 0;
  size_t trace_ci = 0;

  // Data many draws share (2026-10-12, WRB arena: 8.8 MB of vertex data a
  // frame, 2.6 MB of it unique; 1199 constant banks of 3188): converted once
  // per frame - the capture dedupes payload ranges, so equal data has one
  // payload offset - and the arena copy is shared.
  std::unordered_map<uint64_t, VkDeviceSize> vb_cache;
  std::unordered_map<uint32_t, VkDeviceSize> bank_cache;
  auto emit_draw = [&](const DrawRecord& d) {
    float alpha_threshold = 0.0f;  // set from the v12 state below
    if (trace_ci > options_.max_draw_cmd || trace_ci < options_.min_draw_cmd) {
      return;
    }
    if (options_.trace) {
      std::printf("[trace] cmd=%zu %s prim=%u count=%u tex0=%08X vs=%08X "
                  "ps=%08X\n",
                  trace_ci, d.kind == DrawKind::kIndexed ? "indexed" : "inline",
                  d.primitive_type, d.count, d.textures[0], d.vs_object,
                  d.ps_object);
    }
    // v15: content identity recorded at the draw; older captures only have
    // the microcode address.
    const ShaderInfo* vs = d.vs_container != 0
                               ? shaders_.FindByContainer(d.vs_container, true)
                               : shaders_.FindByVa(d.vs_object);
    const ShaderInfo* ps = d.ps_container != 0
                               ? shaders_.FindByContainer(d.ps_container, false)
                               : shaders_.FindByVa(d.ps_object);
    // Safety net: a recorded identity that cannot be resolved, or whose
    // layout does not fit the draw's stream, must not cost the draw — fall
    // back to the address path (with its stride variants) as before v15.
    if (d.vs_container != 0) {
      bool fits = vs != nullptr;
      if (fits) {
        uint32_t s = 0;
        for (const ShaderVertexFetch& f : vs->fetches) {
          if ((f.flags & 1) == 0 && f.stride != 0) {
            s = f.stride * 4;
            break;
          }
        }
        const uint32_t stream =
            d.kind == DrawKind::kIndexed ? d.vb_stride[0] : d.arg6;
        fits = s == 0 || stream == 0 || s == stream;
      }
      if (!fits) vs = shaders_.FindByVa(d.vs_object);
    }
    if (d.ps_container != 0 && ps == nullptr) ps = shaders_.FindByVa(d.ps_object);
    if (vs == nullptr || ps == nullptr) {
      ++skipped;
      return;
    }
    if (!exp_skip_.empty() &&
        (exp_skip_.count(uint32_t(vs->container_fnv >> 32)) != 0 ||
         exp_skip_.count(uint32_t(ps->container_fnv >> 32)) != 0)) {
      return;  // measurement switch (rs_exp_skip)
    }
    // Crowd off (2026-10-15): the audience in the stands is drawn with these
    // two vertex shaders (Underworld arena; about 12% of the Retroid Pocket
    // Nova's GPU time in the arena scene).
    if (options_.no_crowd) {
      const uint32_t vs_id = uint32_t(vs->container_fnv >> 32);
      if (vs_id == 0xF32A48B5u || vs_id == 0xD5D3FAC3u) return;
    }
    // The title's direct post-process draws (bloom/DOF) read sampler slots
    // 8-19, which captures before v10 did not record: skip them there rather
    // than composite from black inputs (the scene stays as rendered).
    if (frame.version < 10 && d.dispatch_caller != 0x821B1EF4u &&
        d.dispatch_caller != 0x821B1E94u) {
      ++skipped;
      return;
    }

    const bool indexed = d.kind == DrawKind::kIndexed;
    // The authoritative stride is the vfetch microcode's, not the stream's:
    // Xenos steps vertices by the instruction stride. The dumped container
    // holds the microcode as patched for ONE vertex declaration; a draw whose
    // stream stride differs is bound to another declaration (robot VS
    // C9F870BF: stride-32 rigid parts vs stride-44 two-bone parts, 2026-09-02)
    // and needs a re-patched shader variant — skipped until that exists.
    uint32_t stride = 0;
    for (const ShaderVertexFetch& f : vs->fetches) {
      if ((f.flags & 1) == 0 && f.stride != 0) {
        stride = f.stride * 4;
        break;
      }
    }
    if (stride == 0) {
      stride = indexed ? d.vb_stride[0] : d.arg6;
    }
    if (stride == 0) {
      ++skipped;
      return;
    }
    {
      const uint32_t stream_stride = indexed ? d.vb_stride[0] : d.arg6;
      const ShaderInfo* variant =
          (stream_stride != 0 && stream_stride != stride)
              ? shaders_.FindVariantByVa(d.vs_object, stream_stride)
              : nullptr;
      if (variant != nullptr) {
        // Re-patched fetch block for this declaration (see
        // tools/shaders/make_stride_variants.py); the pipeline key carries
        // the stride, so it gets its own pipeline.
        vs = variant;
        stride = stream_stride;
      } else if (stream_stride != 0 && stream_stride != stride) {
        if (stride_mismatch_logged < 8) {
          std::printf("[vk] draw %zu: stream stride %u != shader stride %u "
                      "(vs=%08X) — skipped, no layout variant\n",
                      trace_ci, stream_stride, stride, d.vs_object);
        }
        ++stride_mismatch_logged;
        ++skipped;
        return;
      }
    }

    // Geometry into the arena, byte-swapped from guest big-endian (8IN32).
    const uint8_t* vertex_src = nullptr;
    uint32_t vertex_bytes = 0;
    if (indexed) {
      if (d.vb_payload_offset[0] == kInvalidPayloadOffset ||
          d.vb_payload_offset[0] + d.vb_bytes[0] > frame.payload_blob.size()) {
        ++skipped;
        return;
      }
      vertex_src = frame.payload_blob.data() + d.vb_payload_offset[0];
      vertex_bytes = d.vb_bytes[0];
    } else {
      if (d.payload_offset == kInvalidPayloadOffset ||
          d.payload_offset + d.payload_bytes > frame.payload_blob.size()) {
        ++skipped;
        return;
      }
      vertex_src = frame.payload_blob.data() + d.payload_offset;
      vertex_bytes = d.payload_bytes;
    }
    const uint64_t vb_key =
        (uint64_t(indexed ? d.vb_payload_offset[0] : d.payload_offset) << 32) | vertex_bytes;
    VkDeviceSize vb_at;
    if (const auto cached = vb_cache.find(vb_key); cached != vb_cache.end()) {
      vb_at = cached->second;
    } else {
      vb_at = Alloc(vertex_bytes, 16);
      if (vb_at == VkDeviceSize(-1)) { ++skipped; return; }
      rs::capture::CopySwap32(arena_ptr_ + vb_at, vertex_src, vertex_bytes);
      vb_cache[vb_key] = vb_at;
    }

    // Index data. Quadlists are expanded to triangle lists.
    VkDeviceSize ib_at = 0;
    uint32_t index_count = 0;
    uint32_t topology = uint32_t(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    if (indexed) {
      if (d.ib_payload_offset == kInvalidPayloadOffset ||
          d.ib_payload_offset + d.ib_bytes > frame.payload_blob.size() ||
          d.Uses32BitIndices()) {
        ++skipped;
        return;
      }
      const uint8_t* src = frame.payload_blob.data() + d.ib_payload_offset;
      index_count = d.count;
      ib_at = Alloc(size_t(index_count) * 2, 16);
      if (ib_at == VkDeviceSize(-1)) { ++skipped; return; }
      rs::capture::CopySwap16(arena_ptr_ + ib_at, src, size_t(index_count) * 2);
      switch (d.primitive_type) {
        case 4: topology = uint32_t(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST); break;
        case 6: topology = uint32_t(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP); break;
        default: ++skipped; return;
      }
    } else {
      // Inline draws: prim 13 = quadlist -> synth indices; strips as-is.
      const uint32_t vertex_count = d.count;
      if (d.primitive_type == 13) {
        const uint32_t quads = vertex_count / 4;
        index_count = quads * 6;
        ib_at = Alloc(size_t(index_count) * 2, 16);
        if (ib_at == VkDeviceSize(-1)) { ++skipped; return; }
        uint16_t* dst = reinterpret_cast<uint16_t*>(arena_ptr_ + ib_at);
        for (uint32_t q = 0; q < quads; ++q) {
          const uint16_t b = uint16_t(q * 4);
          *dst++ = b; *dst++ = uint16_t(b + 1); *dst++ = uint16_t(b + 2);
          *dst++ = b; *dst++ = uint16_t(b + 2); *dst++ = uint16_t(b + 3);
        }
      } else if (d.primitive_type == 6) {
        topology = uint32_t(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP);
      } else if (d.primitive_type != 4) {
        ++skipped;
        return;
      }
    }

    // Front-buffer sampling: snapshot the main attachment at first use.
    // Matches the recorded front-buffer id, plus any sampled id that has
    // neither a dump file nor a resolve in this frame (GPU-written outside
    // the command stream).
    // Snapshots model the title's swap-time resolves (previous frame's
    // colour/depth). Early users (shadow passes) get whatever is there at
    // first use; the direct post-process draws (bloom/DOF) run after the
    // scene and need it re-taken once so they see the finished frame.
    const bool direct_draw = d.dispatch_caller != 0x821B1EF4u &&
                             d.dispatch_caller != 0x821B1E94u;
    for (uint32_t s = 0; s < rs::capture::kTextureSlots; ++s) {
      const uint32_t id = d.textures[s];
      if (id == 0) continue;
      if (front_done.count(id) != 0 &&
          (!direct_draw || front_done_direct.count(id) != 0)) {
        continue;
      }
      if (direct_draw) front_done_direct[id] = true;
      const bool is_front = id == frame.front_buffer_phys;
      const bool is_orphan = missing_textures_.count(id) != 0 &&
                             frame_resolve_targets.count(id) == 0;
      const bool is_scene_copy = frame_sized_textures_.count(id) != 0 &&
                                 frame_resolve_targets.count(id) == 0;
      const bool is_depth = rs::capture::DrawRecord::IsDepthFormat(
                                d.texture_fmt[s]) &&
                            frame_resolve_targets.count(id) == 0;
      if (is_depth) {
        // v11 captures: the slot holds a depth texture the title produced
        // outside the stream (swap-time depth resolve) — snapshot the MAIN
        // attachment's depth (the scene), not the last-drawn target: by the
        // time the composite runs, that is the 320x180 blur target, whose
        // depth is 1.0 everywhere (= "far": full fog + full DOF blur).
        copy_depth_to(final_rt, id, kWidth, kHeight);
        front_done[id] = true;
      } else if (is_scene_copy) {
        RtEntry* keep = last_draw_rt;
        last_draw_rt = final_rt;
        take_front_snapshot(id);
        last_draw_rt = keep;
      } else if (is_front || is_orphan) {
        take_front_snapshot(id);
      }
    }

    // Constant blocks. v8 captures carry the device's actual banks per draw
    // (ground truth, catching every write path); stream replay is the
    // fallback for older captures.
    auto write_bank = [&](uint32_t snapshot_offset,
                          const std::vector<uint8_t>& fallback,
                          VkDeviceSize dst) {
      if (snapshot_offset != kInvalidPayloadOffset &&
          size_t(snapshot_offset) + 4096 <= frame.payload_blob.size()) {
        rs::capture::CopySwap32(cbuf_ptr_ + dst, frame.payload_blob.data() + snapshot_offset, 4096);
      } else {
        std::memcpy(cbuf_ptr_ + dst, fallback.data(), fallback.size());
      }
    };
    // Banks that get patched below (DOF off, radial blurs) are this draw's own.
    const uint64_t ps_id = ps->container_fnv;
    const bool ps_patched =
        (options_.no_dof && ps_id == 0xCB9A5EB75CB8B59Eull) ||
        (options_.picture_aspect > 0.0f &&
         (ps_id == 0x41CBD44E4D2341D2ull || ps_id == 0x69285499B02D3760ull ||
          ps_id == 0x848059FFF6A5306Cull || ps_id == 0xCDAF988C95DBABE2ull));
    auto bank_at = [&](uint32_t snapshot, const std::vector<uint8_t>& fallback, bool shareable) {
      shareable = shareable && snapshot != kInvalidPayloadOffset;
      if (shareable) {
        const auto cached = bank_cache.find(snapshot);
        if (cached != bank_cache.end()) return cached->second;
      }
      const VkDeviceSize at = AllocConst(std::max<size_t>(fallback.size(), 4096));
      if (at == VkDeviceSize(-1)) return at;
      write_bank(snapshot, fallback, at);
      if (shareable) bank_cache[snapshot] = at;
      return at;
    };
    const VkDeviceSize vs_at = bank_at(d.vs_bank_offset, vs_consts, true);
    const VkDeviceSize ps_at = bank_at(d.ps_bank_offset, ps_consts, !ps_patched);
    const VkDeviceSize sh_at = AllocConst(kSharedBytes);
    if (vs_at == VkDeviceSize(-1) || ps_at == VkDeviceSize(-1) || sh_at == VkDeviceSize(-1)) {
      ++skipped;
      return;
    }
    if (options_.picture_aspect > 0.0f &&
        (ps->container_fnv == 0x41CBD44E4D2341D2ull || ps->container_fnv == 0x69285499B02D3760ull ||
         ps->container_fnv == 0x848059FFF6A5306Cull || ps->container_fnv == 0xCDAF988C95DBABE2ull)) {
      // The title's radial / zoom blurs (2026-10-12): centre (c3.x, c4.x) and
      // blur lengths (c8-c11 .x per axis, c12.xy) are in texture coordinates
      // of the 16:9 frame. The source now holds the whole picture, where a
      // 16:9 position sits squeezed towards the centre - move them with it.
      const float rel = options_.picture_aspect / (16.0f / 9.0f);
      const float sx = rel > 1.0f ? 1.0f / rel : 1.0f;
      const float sy = rel < 1.0f ? rel : 1.0f;
      float* c = reinterpret_cast<float*>(cbuf_ptr_ + ps_at);
      c[3 * 4] = 0.5f + (c[3 * 4] - 0.5f) * sx;   // g_fCenterPosX
      c[4 * 4] = 0.5f + (c[4 * 4] - 0.5f) * sy;   // g_fCenterPosY
      c[8 * 4] *= sx;                               // g_fBlurRateMinLengthX
      c[9 * 4] *= sx;                               // g_fBlurRateMaxLengthX
      c[10 * 4] *= sy;                              // g_fBlurRateMinLengthY
      c[11 * 4] *= sy;                              // g_fBlurRateMaxLengthY
      c[12 * 4] *= sx;                              // g_f2BlurRateLength.x
      c[12 * 4 + 1] *= sy;                          //                    .y
      static int logged = 0;
      if (logged++ < 4) {
        std::printf("[vk] radial blur ps=%016llX vs=%016llX pt=%d %s vp=%ux%u centre %.3f %.3f\n",
                    static_cast<unsigned long long>(ps->container_fnv),
                    static_cast<unsigned long long>(vs->container_fnv),
                    vs->passthrough_position ? 1 : 0, indexed ? "idx" : "inl", d.vp_w, d.vp_h,
                    c[3 * 4], c[4 * 4]);
      }
    }
    if (options_.no_dof && ps->container_fnv == 0xCB9A5EB75CB8B59Eull) {
      // g_f4DOFParam = c20: .x near slope, .w far maximum -> no blend.
      const float zero = 0.0f;
      std::memcpy(cbuf_ptr_ + ps_at + 320, &zero, 4);
      std::memcpy(cbuf_ptr_ + ps_at + 332, &zero, 4);
    }
    std::memset(cbuf_ptr_ + sh_at, 0, kSharedBytes);
    const bool dispatcher_draw_dbg = d.dispatch_caller == 0x821B1EF4u ||
                                     d.dispatch_caller == 0x821B1E94u;
    uint32_t* shared = reinterpret_cast<uint32_t*>(cbuf_ptr_ + sh_at);
    for (uint32_t s = 0; s < rs::capture::kTextureSlots; ++s) {
      const auto tex = texture_indices_.find(d.textures[s]);
      // Unbound slot (id 0) samples black — except slots 8-19 of pre-v10
      // captures, which were simply not recorded (keep the white fallback
      // there: crowd/world shaders multiply by lightmaps in those slots).
      const bool unrecorded =
          frame.version < 10 && s >= rs::capture::kTextureSlotsV9;
      shared[kShared2DBase / 4 + s] =
          options_.all_white ? 0
          : int(s) == options_.zero_slot ? 1
          : int(s) == options_.white_slot ? 0
          : d.textures[s] == 0 ? (unrecorded ? 0u : 1u)
          : tex != texture_indices_.end() ? tex->second : 0;
      if (options_.trace && !dispatcher_draw_dbg && d.textures[s] != 0) {
        std::printf("[trace]   direct draw slot %u id=%08X -> 2D heap %u\n", s,
                    d.textures[s],
                    tex != texture_indices_.end() ? tex->second : 0u);
      }
      const auto cube = cube_indices_.find(d.textures[s]);
      shared[kSharedCubeBase / 4 + s] =
          (!options_.all_white && cube != cube_indices_.end()) ? cube->second
                                                               : 0;
      {
        // Clamp modes (texture_fmt bits 16-21, Xenos ClampMode): 0 repeat,
        // 1 mirrored repeat, 2-7 clamp variants (approximated as clamp to
        // edge). 2026-10-18: everything was repeat - the dialog buttons'
        // nine-slice pieces sampled their opposite edge at the seams (the
        // 1-px line 7 px above their bottom).
        const auto cls = [](uint32_t m) { return m == 0 ? 0u : m == 1 ? 1u : 2u; };
        const uint32_t cx = (d.texture_fmt[s] >> 16) & 0x7u;
        const uint32_t cy = (d.texture_fmt[s] >> 19) & 0x7u;
        shared[kSharedSamplerBase / 4 + s] = cls(cx) * 3u + cls(cy);
      }
    }
    // Bool constants (offset 320 in SharedConstants layout v2).
    if (d.bool_bank_offset != kInvalidPayloadOffset &&
        size_t(d.bool_bank_offset) + 32 <= frame.payload_blob.size()) {
      const uint8_t* src = frame.payload_blob.data() + d.bool_bank_offset;
      for (size_t i = 0; i < 8; ++i) {
        shared[320 / 4 + i] = LoadBE32(src + i * 4);
      }
    } else {
      std::memcpy(shared + 320 / 4, bool_dwords, sizeof(bool_dwords));
    }
    if (options_.bool_all) {
      for (size_t i = 0; i < 8; ++i) shared[320 / 4 + i] = 0xFFFFFFFFu;
    }
    if (options_.bool_reverse) {
      for (size_t i = 0; i < 8; ++i) {
        uint32_t v = shared[320 / 4 + i];
        v = ((v & 0xAAAAAAAAu) >> 1) | ((v & 0x55555555u) << 1);
        v = ((v & 0xCCCCCCCCu) >> 2) | ((v & 0x33333333u) << 2);
        v = ((v & 0xF0F0F0F0u) >> 4) | ((v & 0x0F0F0F0Fu) << 4);
        v = ((v & 0xFF00FF00u) >> 8) | ((v & 0x00FF00FFu) << 8);
        v = (v >> 16) | (v << 16);
        shared[320 / 4 + i] = v;
      }
    }
    // Vertex-texture slots point at the fallback for now (not captured yet).
    // g_SwappedTexcoords: see Options::uv_lane_swap — off for this title.
    uint32_t swapped = 0;
    if (options_.uv_lane_swap) {
      for (const ShaderVertexFetch& f : vs->fetches) {
        if (f.usage == 5 && Is16BitTexcoordFormat(f.format)) {
          swapped |= 1u << f.usageIndex;
        }
      }
    }
    shared[kSharedSwappedTexcoords / 4] = swapped;
    if (vs->is_template) {
      std::memcpy(shared + 384 / 4, vs->fetch_swizzles, 64);  // g_FetchSwizzle
    }
    std::memcpy(shared + 364 / 4, &alpha_threshold, 4);  // g_AlphaThreshold
    // Viewport transform disabled (360 D3DRS_VIEWPORTENABLE = FALSE): the VS
    // outputs pixel coordinates with w = 1. An identity viewport cannot work
    // (Vulkan clips to |xy| <= w before the viewport transform), so the
    // recompiled VS maps pixels to NDC itself when g_ViewportScale is set.
    // The title's quads already carry the -0.5 pixel-centre offset.
    const uint32_t draw_vp_w = d.vp_w != 0 ? d.vp_w : vp_w;
    const uint32_t draw_vp_h = d.vp_h != 0 ? d.vp_h : vp_h;
    if (vs->passthrough_position && draw_vp_w != 0 && draw_vp_h != 0) {
      float* vps = reinterpret_cast<float*>(shared + kSharedViewportScale / 4);
      vps[0] = 2.0f / float(draw_vp_w);
      // Pixel y = 0 is the top of the target. The backend's default (no
      // viewport flip) puts the title's clip-space +y at the top, so pixel
      // rows map to NDC top-down as 1 - 2y/H; a flipped viewport inverts it.
      vps[1] = (options_.flip_y ? 2.0f : -2.0f) / float(draw_vp_h);
      vps[2] = -1.0f;
      vps[3] = options_.flip_y ? -1.0f : 1.0f;
    } else if (options_.picture_aspect > 0.0f && draw_vp_w * 9 == draw_vp_h * 16) {
      // Picture aspect other than 16:9 (2026-10-02): the guest still renders
      // its 16:9 frame, which the present pass stretches to the target
      // shape; pre-squeezing clip space by the inverse keeps proportions.
      // Narrower than 16:9 squeezes y (more is visible above and below),
      // wider squeezes x. Pixel-coordinate quads (post-processing, above)
      // and non-16:9 surfaces (shadow maps, probes) are left alone.
      const float rel = options_.picture_aspect / (16.0f / 9.0f);
      float* vps = reinterpret_cast<float*>(shared + kSharedViewportScale / 4);
      vps[0] = rel > 1.0f ? 1.0f / rel : 1.0f;
      vps[1] = rel < 1.0f ? rel : 1.0f;
      vps[2] = 0.0f;
      vps[3] = 0.0f;
      // The title's 2D (2026-10-12): menus, HUD, fades and flashes are inline
      // quads in guest pixels, mapped to clip space by c0 = (2/W, 2/H).
      // Their pixel box decides where they go in the wider/taller picture
      // (Layout2D); everything else stays in the central 16:9 band. A frame
      // shown as its 16:9 band only (aspect_mask_2d) keeps its own layout.
      float box[4];
      const bool masked = options_.aspect_mask_2d && !frame_is_scene_;
      if (!indexed && !masked && Pixel2DBox(*vs, vertex_src, vertex_bytes, stride, d.count,
                                 reinterpret_cast<const float*>(cbuf_ptr_ + vs_at),
                                 draw_vp_w, draw_vp_h, box)) {
        bool screen_copy = false;
        for (uint32_t s = 0; s < rs::capture::kTextureSlots; ++s) {
          const uint32_t id = d.textures[s];
          screen_copy |= id != 0 && (id == frame.front_buffer_phys ||
                                     frame_sized_textures_.count(id) != 0 ||
                                     frame_resolve_targets.count(id) != 0);
        }
        Layout2D(box, float(draw_vp_w), float(draw_vp_h), rel, !frame_is_scene_,
                 screen_copy, bottom_bar_top_, vps);
      }
    }
    if (draw_list_once_) {
      // One frame's draw list (DrawListOnce): what each draw is and, for
      // inline (2D) draws, the first vertices' raw position floats.
      std::printf("[drawlist] %zu %s caller=%08X vs=%016llX ps=%016llX pt=%d vp=%ux%u "
                  "prim=%u n=%u tex0=%08X stride=%u",
                  trace_ci, indexed ? "idx" : "inl", d.dispatch_caller,
                  static_cast<unsigned long long>(vs->container_fnv),
                  static_cast<unsigned long long>(ps->container_fnv),
                  vs->passthrough_position ? 1 : 0, draw_vp_w, draw_vp_h,
                  d.primitive_type, d.count, d.textures[0], stride);
      if (!indexed) {
        uint32_t pos_off = 0;
        for (const ShaderVertexFetch& f : vs->fetches) {
          if (f.usage == 0 && f.usageIndex == 0) pos_off = f.offset * 4;
        }
        for (uint32_t v = 0; v < 4 && (v + 1) * stride <= vertex_bytes; ++v) {
          float p[3];
          for (int k = 0; k < 3; ++k) {
            const uint32_t u = LoadBE32(vertex_src + v * stride + pos_off + k * 4);
            std::memcpy(&p[k], &u, 4);
          }
          std::printf(" (%.1f %.1f %.2f)", p[0], p[1], p[2]);
        }
      }
      // Sampled ids: R = resolved this frame, F = front buffer / frame-sized
      // copy (screen contents).
      for (uint32_t s = 0; s < rs::capture::kTextureSlots; ++s) {
        const uint32_t id = d.textures[s];
        if (id == 0) continue;
        std::printf(" t%u=%08X%s%s", s, id, frame_resolve_targets.count(id) ? "R" : "",
                    (id == frame.front_buffer_phys || frame_sized_textures_.count(id)) ? "F" : "");
      }
      const float* c = reinterpret_cast<const float*>(cbuf_ptr_ + vs_at);
      std::printf(" c0=%.4g %.4g %.4g %.4g c1=%.4g %.4g %.4g %.4g c3=%.4g %.4g %.4g %.4g\n",
                  c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[12], c[13], c[14], c[15]);
    }

    PipelineKey key{};
    key.vs_hash = vs->cache_hash;
    key.ps_hash = ps->cache_hash;
    key.topology = topology;
    key.stride = stride;
    // Blend state is not captured yet: dispatcher inline draws are the 2D/UI
    // path (type-65 blend block: src-alpha/inv-src-alpha), 3D indexed draws
    // are opaque, and the title's direct post-process quads (UserCallback
    // draws) set their state inside the callback — opaque, else the scene
    // snapshot's zero alpha blends the blur/bloom chain away (2026-09-05).
    const bool dispatcher_draw = d.dispatch_caller == 0x821B1EF4u ||
                                 d.dispatch_caller == 0x821B1E94u;
    // v14: RB_SURFACE_INFO msaa_samples (bits 16-17: 0 = 1x, 1 = 2x, 2 = 4x).
    uint32_t draw_samples = 1;
    if (frame.version >= 14 && !options_.no_msaa) {
      const uint32_t m = (d.rs2[2] >> 16) & 3u;
      draw_samples = m == 1 ? 2u : m == 2 ? 4u : 1u;
    }
    if (rendering_active && cur_rt != nullptr &&
        cur_rt->ms_failed_frame == frame_counter_) {
      draw_samples = 1;  // no MS set on this surface this frame: stay 1x
    }
    if (rendering_active && cur_rt != nullptr &&
        cur_rt->frame_samples_frame == frame_counter_) {
      draw_samples = cur_rt->frame_samples;  // see RtEntry::frame_samples
    }
    if (rendering_active && draw_samples != active_samples) {
      end_pass();
      pass_resumable = true;
    }
    pass_samples = draw_samples;
    key.samples = draw_samples;
    key.blend = (indexed || !dispatcher_draw) ? 0 : 1;
    key.depth = (indexed && !options_.no_depth)
                    ? (options_.depth_always ? 2u : 1u)
                    : 0u;
    if (frame.version >= 12 && !options_.no_depth) {
      // v12: the device's render-state shadow at draw time (the effect
      // system sets these outside the command stream). rs[2] =
      // RB_DEPTHCONTROL, rs[4] = RB_COLORCONTROL (bit3 alpha test, bits 0-2
      // func), rs[9] = RB_BLENDCONTROL, rs[10] bit31 = ALPHABLENDENABLE,
      // rs[7] = PA_SU_SC_MODE_CNTL.
      key.rs_valid = 1;
      key.depth_ctl = d.rs[2] & 0x7Fu;
      key.blend_ctl = (d.rs[10] & 0x80000000u) ? d.rs[9] : 0u;
      const uint32_t alpha_func = d.rs[4] & 0x7u;
      key.alpha_test = ((d.rs[4] & 0x8u) != 0 &&
                        (alpha_func == 4 || alpha_func == 6)) ? 1u : 0u;
      key.cull = d.rs[7] & 0x7u;
      if (key.depth != 2) key.depth = 1;  // real test/write bits come from depth_ctl
      float ref = options_.alpha_ref;
      if (frame.version >= 14 && !options_.alpha_ref_override) {
        // v14: RB_ALPHA_REF shadow (dev+10500) is a float in [0, 1].
        float captured;
        std::memcpy(&captured, &d.rs2[35], 4);
        if (captured >= 0.0f && captured <= 1.0f) ref = captured;
      }
      alpha_threshold = key.alpha_test ? ref : 0.0f;
    }
    VkPipeline pipeline = GetPipeline(key, vs, ps);
    if (pipeline == VK_NULL_HANDLE) {
      ++skipped;
      return;
    }

    // v9 captures carry the draw's true viewport; adopt it before the pass
    // begins so attachment aliasing sees the real render area.
    if (d.vp_w != 0 && d.vp_h != 0 &&
        (d.vp_x != vp_x || d.vp_y != vp_y || d.vp_w != vp_w ||
         d.vp_h != vp_h)) {
      vp_x = d.vp_x;
      vp_y = d.vp_y;
      vp_w = d.vp_w;
      vp_h = d.vp_h;
      if (rendering_active) set_viewport();
    }
    // A full-frame draw belongs on the main attachment even when the pass
    // opened on a small-viewport draw (the UI pass often starts with one):
    // split the pass and continue on main.
    const bool want_main = vp_w >= 1200 && vp_h >= 600 && vp_h <= kHeight;
    if (rendering_active && want_main && cur_rt != GetRt(0)) {
      end_pass();
      pass_resumable = true;  // keep depth across the split
    }
    if (!rendering_active) {
      begin_pass();
      if (!rendering_active) {
        ++skipped;
        return;
      }
    }
    if (key.samples != active_samples) {
      // Multisampled attachments were unavailable for this surface: rebuild
      // the pipeline for the count the pass really renders with (a 2x
      // pipeline into 1x attachments faulted the device — 23:25 run).
      key.samples = active_samples;
      pipeline = GetPipeline(key, vs, ps);
      if (pipeline == VK_NULL_HANDLE) {
        ++skipped;
        return;
      }
    }
    last_draw_rt = cur_rt;
    ++draws_in_pass;
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const uint32_t push[3] = {uint32_t(vs_at), uint32_t(ps_at), uint32_t(sh_at)};
    vkCmdPushConstants(cmd_, pipeline_layout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(push), push);
    const VkDeviceSize vb_offset = vb_at;
    vkCmdBindVertexBuffers(cmd_, 0, 1, &arena_, &vb_offset);
    if (!vs->zero_inputs.empty()) {
      const VkDeviceSize zero_at = Alloc(16, 16);
      if (zero_at != VkDeviceSize(-1)) {
        std::memset(arena_ptr_ + zero_at, 0, 16);
        vkCmdBindVertexBuffers(cmd_, 1, 1, &arena_, &zero_at);
      }
    }
    if (index_count != 0) {
      vkCmdBindIndexBuffer(cmd_, arena_, ib_at, VK_INDEX_TYPE_UINT16);
      vkCmdDrawIndexed(cmd_, index_count, 1, 0, 0, 0);
    } else {
      vkCmdDraw(cmd_, d.count, 1, 0, 0);
    }
    ++drawn;
  };

  // Resolve records (v5+) are executed at their command index whatever the
  // command type: dispatcher Resolve commands (53), and — 2026-09-15 — the
  // D3D-level resolves the game issues from inside other commands (the
  // sub-renderer's end-flush resolves the versus screen's robot surface into
  // the front buffer; hooked at sub_8224E748 in the game).
  auto do_resolve = [&](const rs::capture::ResolveRecord& r) {
    if (r.tex_object == 0 || cur_rt == nullptr) return;
    // 2026-10-02: a Clear followed by no draws and a Resolve (the glow
    // chain's model pass on screens without models: menu intro) must still
    // clear — the clear was only applied when a draw opened the pass, so the
    // resolve copied whatever an earlier screen left in the surface (a white
    // logo screen -> a white-out through the glow composite).
    if (pending_clear && !rendering_active) begin_pass();
    end_pass();
    pass_resumable = true;
    // Ambient occlusion (option): once a frame, on the 3D scene, before the
    // game's first copy of it.
    if (options_.ao_strength > 0.0f && frame_is_scene_ && !ao_done && cur_rt == final_rt) {
      ao_done = true;
      RunAmbientOcclusion(cur_rt);
    }
        const uint32_t dst_w = (r.fetch[2] & 0x1FFF) + 1;
        const uint32_t dst_h = ((r.fetch[2] >> 13) & 0x1FFF) + 1;
        // Shadow maps (small targets / float format): our passes lack the
        // game's tiny viewports, so the copies would be garbage that puts
        // the whole world in shadow. Leave them at the white fallback
        // (= unoccluded) until shadow-pass viewports are recovered.
        if (rs::capture::DrawRecord::IsDepthFormat(r.fetch[1])) {
          // Depth resolve (shadow maps, k_32_FLOAT): copy the pass's depth.
          if (!options_.no_depth_resolves) {
            copy_depth_to(cur_rt, r.phys, dst_w, dst_h);
          }
          return;
        }
        ResolveDst& dst = resolve_dsts_[r.phys];
        if (dst.image != VK_NULL_HANDLE &&
            (dst.width != dst_w || dst.height != dst_h ||
             dst.format == VK_FORMAT_D32_SFLOAT)) {
          RetireResolveDst(dst);  // address reused at another size / as colour
        }
        if (dst.image == VK_NULL_HANDLE) {
          if (!AllocImage(device_, phys_, SW(dst_w), SH(dst_h),
                          VK_FORMAT_R8G8B8A8_UNORM,
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          &dst.image, &dst.memory)) {
            return;
          }
          VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
          vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
          vci.image = dst.image;
          vci.format = VK_FORMAT_R8G8B8A8_UNORM;
          vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          if (vkCreateImageView(device_, &vci, nullptr, &dst.view) !=
              VK_SUCCESS) {
            return;
          }
          dst.width = dst_w;
          dst.height = dst_h;
          if (dst.heap_index == 0) dst.heap_index = texture_count_ < kMaxTextures ? texture_count_++ : 0;
          VkDescriptorImageInfo info{};
          info.imageView = dst.view;
          info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
          VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
          write.dstSet = sets_[0];
          write.dstBinding = 0;
          write.dstArrayElement = dst.heap_index;
          write.descriptorCount = 1;
          write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
          write.pImageInfo = &info;
          WaitFlight();  // the set may be in use by the frame in flight
          vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
          texture_indices_[r.phys] = dst.heap_index;  // resolve overrides file
        }
        // Transition src RT -> TRANSFER_SRC, dst -> TRANSFER_DST.
        VkImageMemoryBarrier2 barriers[2] = {};
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
        barriers[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barriers[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barriers[0].dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
        barriers[0].oldLayout = cur_rt->layout;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].image = cur_rt->color;
        barriers[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        barriers[1] = barriers[0];
        barriers[1].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        barriers[1].srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barriers[1].oldLayout = dst.layout;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].image = dst.image;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 2;
        dep.pImageMemoryBarriers = barriers;
        vkCmdPipelineBarrier2(cmd_, &dep);
        cur_rt->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

        VkImageCopy copy{};
        copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.extent = {SW(std::min(cur_rt->width, dst.width)),
                       SH(std::min(cur_rt->height, dst.height)), 1};
        vkCmdCopyImage(cmd_, cur_rt->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &copy);

        VkImageMemoryBarrier2 to_read{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_read.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_read.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                               VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        to_read.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_read.image = dst.image;
        to_read.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep2.imageMemoryBarrierCount = 1;
        dep2.pImageMemoryBarriers = &to_read;
        vkCmdPipelineBarrier2(cmd_, &dep2);
        dst.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  };

  for (size_t ci = 0; ci < frame.commands.size(); ++ci) {
    const CommandRecord& c = frame.commands[ci];
    if (c.blob_offset + c.byte_count > frame.command_blob.size()) continue;
    const uint8_t* cmd = frame.command_blob.data() + c.blob_offset;
    trace_ci = ci;
    if (options_.trace &&
        (c.type == 39 || c.type == 52 || c.type == 53 || c.type == 65)) {
      std::printf("[trace] cmd=%zu type=%u\n", ci, c.type);
    }
    while (next_resolve < frame.resolves.size() &&
           frame.resolves[next_resolve].command_index < ci) {
      ++next_resolve;
    }
    switch (c.type) {
      // Types 19/26 also route through the hooked setters (duplicate,
      // adjacent, identical applications — harmless); kept for captures
      // made before the hook existed.
      case 19: apply_const_f(cmd, c.byte_count, vs_consts); break;
      case 26: apply_const_f(cmd, c.byte_count, ps_consts); break;
      case 204: {  // v14 synthetic: the title's display gamma ramp
        // Header: valid, which, flag, dest_va, temp_va, type[2], level[2];
        // then [dest-512, dest+1024) raw guest bytes (three 256 x u16 BE
        // tables, value in bits 6-15) and the builder's 512-byte temp table.
        if (c.byte_count < 36 + 1536 + 512) break;
        const uint32_t valid = LoadBE32(cmd + 0);
        if (valid == 0) break;
        float ramp[3][256];
        bool usable = true;
        for (uint32_t ch = 0; ch < 3; ++ch) {
          for (uint32_t i = 0; i < 256; ++i) {
            const uint8_t* e = cmd + 36 + ch * 512 + i * 2;
            const uint32_t v = (uint32_t(e[0]) << 8) | e[1];
            ramp[ch][i] = float(v >> 6) / 1023.0f;
          }
          // A table that never rises is unset (the first v14 capture had the
          // dest tables all zero: the title never composed a ramp — its
          // builder output was the identity). Ignore such records.
          if (ramp[ch][255] <= ramp[ch][0]) usable = false;
        }
        if (usable) {
          gamma_ramp_valid_ = true;
          std::memcpy(gamma_ramp_, ramp, sizeof(ramp));
        }
        if (options_.trace) {
          float level;
          const uint32_t level_bits = LoadBE32(cmd + 28);
          std::memcpy(&level, &level_bits, 4);
          std::printf("[trace] gamma ramp: via=%u flag=%u type=%u level=%.4f "
                      "ch0[0,64,128,255]=%.3f %.3f %.3f %.3f ch1[128]=%.3f "
                      "ch2[128]=%.3f\n",
                      LoadBE32(cmd + 4), LoadBE32(cmd + 8), LoadBE32(cmd + 20),
                      level, gamma_ramp_[0][0], gamma_ramp_[0][64],
                      gamma_ramp_[0][128], gamma_ramp_[0][255],
                      gamma_ramp_[1][128], gamma_ramp_[2][128]);
        }
        break;
      }
      case 201:
      case 202: {
        if (c.byte_count < 8) break;
        std::vector<uint8_t>& bank = c.type == 201 ? vs_consts : ps_consts;
        const uint32_t start = LoadBE32(cmd + 0);
        const uint32_t count = LoadBE32(cmd + 4);
        for (uint32_t i = 0; i < count && start + i < 256; ++i) {
          if (8 + (i + 1) * 16 > c.byte_count) break;
          for (int k = 0; k < 4; ++k) {
            const uint32_t v = LoadBE32(cmd + 8 + i * 16 + k * 4);
            std::memcpy(&bank[(start + i) * 16 + k * 4], &v, 4);
          }
        }
        break;
      }
      // Single-component constant writes (inline dispatcher handlers that
      // bypass the setters): VS 20-23 = .x/.y/.z/.w, PS 27-30 likewise.
      case 20: case 21: case 22: case 23:
      case 27: case 28: case 29: case 30: {
        if (c.byte_count < 32) break;
        const bool is_ps = c.type >= 27;
        std::vector<uint8_t>& bank = is_ps ? ps_consts : vs_consts;
        const uint32_t comp = (c.type - (is_ps ? 27 : 20));
        const uint32_t reg = LoadBE32(cmd + 16);
        if (reg >= 256) break;
        const uint32_t v = LoadBE32(cmd + 20);
        std::memcpy(&bank[reg * 16 + comp * 4], &v, 4);
        break;
      }
      case 39: {  // SetRenderTargetOrDS: pass boundary + surface identity.
        end_pass();
        pass_resumable = false;
        // D3D semantics: SetRenderTarget resets the viewport to the full
        // surface (fallback — v9 per-draw viewports override this anyway).
        vp_x = 0;
        vp_y = 0;
        vp_w = kWidth;
        vp_h = kHeight;
        if (c.byte_count >= 20) {
          cur_key = LoadBE32(cmd + 16);
          if (options_.trace) {
            std::printf("[trace] cmd=%zu setrt key=%08llX\n", ci,
                        static_cast<unsigned long long>(cur_key));
          }
        }
        break;
      }
      case 200: {  // Synthetic viewport record: dev+13024 holds FLOATS
                   // {x, y, w, h, minZ, maxZ, pad} (observed 2026-09-01).
        if (c.byte_count >= 28) {
          auto be_float_u32 = [&](uint32_t off) -> uint32_t {
            const uint32_t bits = LoadBE32(cmd + off);
            float f;
            std::memcpy(&f, &bits, 4);
            if (!(f >= 0.0f && f <= 8192.0f)) return 0xFFFFFFFFu;
            return uint32_t(f + 0.5f);
          };
          const uint32_t x = be_float_u32(0);
          const uint32_t y = be_float_u32(4);
          const uint32_t w = be_float_u32(8);
          const uint32_t h = be_float_u32(12);
          static int svp_logged = 0;
          if (options_.trace && svp_logged < 12) {
            std::printf("[trace] cmd=%zu vp200: %08X %08X %08X %08X %08X "
                        "%08X %08X\n",
                        ci, LoadBE32(cmd + 0), LoadBE32(cmd + 4),
                        LoadBE32(cmd + 8), LoadBE32(cmd + 12),
                        LoadBE32(cmd + 16), LoadBE32(cmd + 20),
                        LoadBE32(cmd + 24));
            ++svp_logged;
          }
          if (w >= 1 && h >= 1 && w <= 4096 && h <= 4096 && x <= 4096 &&
              y <= 4096) {
            vp_x = x;
            vp_y = y;
            vp_w = w;
            vp_h = h;
            if (rendering_active) set_viewport();
          }
        }
        break;
      }
      case 33: {  // SetViewport: bounds the pass's render area.
        if (c.byte_count >= 32) {
          const uint32_t x = LoadBE32(cmd + 16);
          const uint32_t y = LoadBE32(cmd + 20);
          const uint32_t w = LoadBE32(cmd + 24);
          const uint32_t h = LoadBE32(cmd + 28);
          static int vp_logged = 0;
          if (vp_logged < 6) {
            std::printf("[vk] viewport cmd: %u %u %u %u\n", x, y, w, h);
            ++vp_logged;
          }
          if (w >= 1 && h >= 1 && w <= kAttachW && h <= kAttachH &&
              x + w <= kAttachW && y + h <= kAttachH) {
            vp_x = x;
            vp_y = y;
            vp_w = w;
            vp_h = h;
            if (rendering_active) set_viewport();
          }
        }
        break;
      }
      case 52: {  // Clear: clears the viewport rect per the 360 flag bits.
        uint32_t flags = 0x3F, argb = 0;
        if (c.byte_count >= 32) {
          flags = LoadBE32(cmd + 24);
          argb = LoadBE32(cmd + 28);
        }
        if (options_.trace) {
          std::printf("[trace] cmd=%zu clear flags=%02X argb=%08X\n", ci,
                      flags, argb);
        }
        const VkClearColorValue color{{((argb >> 16) & 0xFF) / 255.0f,
                                       ((argb >> 8) & 0xFF) / 255.0f,
                                       (argb & 0xFF) / 255.0f,
                                       ((argb >> 24) & 0xFF) / 255.0f}};
        if (rendering_active) {
          clear_viewport_rect(flags, color);
        } else {
          pending_clear = true;
          pending_clear_flags = flags;
          pending_clear_color = color;
        }
        break;
      }
      case 53: {  // Resolve: ends the pass; the record runs below (do_resolve)
        end_pass();
        pass_resumable = true;
        break;
      }
      // Draw pairing: the k-th dispatcher command pairs with the k-th draw
      // record MADE BY THE DISPATCHER. Draw records from direct D3D calls
      // (outside the 67-type stream) would skew a naive count-based pairing;
      // they are recognized by dispatch_caller and emitted in passing.
      case 15: {
        while (indexed_cursor < frame.draws.size()) {
          const DrawRecord& d = frame.draws[indexed_cursor];
          if (d.kind == DrawKind::kIndexed) {
            if (d.dispatch_caller == 0x821B1EF4u) break;
            emit_draw(d);  // direct indexed draw
          }
          ++indexed_cursor;
        }
        if (indexed_cursor < frame.draws.size()) {
          emit_draw(frame.draws[indexed_cursor++]);
        }
        break;
      }
      // Direct D3D draws happen inside the title's UserCallback commands
      // (post-process quads: fn 821DA330 draws the blur passes, 821DA350 the
      // bloom composite). Emit one direct draw per callback so it lands
      // before the render-target switch / clear that follows, instead of
      // being flushed "in passing" at the next dispatcher draw.
      case 56: {
        size_t best = frame.draws.size();
        for (size_t c : {indexed_cursor, inline_cursor}) {
          const DrawKind want =
              c == indexed_cursor ? DrawKind::kIndexed : DrawKind::kInline;
          for (size_t i = c; i < frame.draws.size(); ++i) {
            const DrawRecord& d = frame.draws[i];
            if (d.kind != want) continue;
            const bool dispatcher = d.dispatch_caller == 0x821B1EF4u ||
                                    d.dispatch_caller == 0x821B1E94u;
            if (!dispatcher && i < best) best = i;
            break;
          }
        }
        if (best < frame.draws.size()) {
          emit_draw(frame.draws[best]);
          if (frame.draws[best].kind == DrawKind::kIndexed) {
            indexed_cursor = best + 1;
          } else {
            inline_cursor = best + 1;
          }
        }
        break;
      }
      case 18: {
        while (inline_cursor < frame.draws.size()) {
          const DrawRecord& d = frame.draws[inline_cursor];
          if (d.kind == DrawKind::kInline) {
            if (d.dispatch_caller == 0x821B1E94u) break;
            emit_draw(d);  // direct inline draw
          }
          ++inline_cursor;
        }
        if (inline_cursor < frame.draws.size()) {
          emit_draw(frame.draws[inline_cursor++]);
        }
        break;
      }
      default:
        break;
    }
    while (next_resolve < frame.resolves.size() &&
           frame.resolves[next_resolve].command_index == ci) {
      do_resolve(frame.resolves[next_resolve]);
      ++next_resolve;
    }
  }

  end_pass();
  if (draw_list_once_) {
    std::printf("[drawlist] end of frame %llu (scene composite %s)\n",
                static_cast<unsigned long long>(frame.index), frame_is_scene_ ? "yes" : "no");
    std::fflush(stdout);
    draw_list_once_ = false;
  }

  // Copy out the full-frame render target and write the BMP.
  {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout = last_draw_rt->layout;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.image = last_draw_rt->color;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd_, &dep);
    last_draw_rt->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kWidth, kHeight, 1};
    shot_w_ = kWidth;
    shot_h_ = kHeight;
    if (options_.readback_picture_shape && options_.picture_aspect > 0.0f &&
        options_.dump_passes_dir.empty() && !Unscaled()) {
      // The picture's own shape, within 1280x720 (the readback buffer's size).
      if (options_.picture_aspect < float(kWidth) / float(kHeight)) {
        shot_w_ = (uint32_t(float(kHeight) * options_.picture_aspect + 0.5f)) & ~1u;
      } else {
        shot_h_ = (uint32_t(float(kWidth) / options_.picture_aspect + 0.5f)) & ~1u;
      }
      copy.imageExtent = {shot_w_, shot_h_, 1};
    }
    if (last_draw_rt->width < kWidth || last_draw_rt->height < kHeight) {
      last_draw_rt = final_rt;  // a small scratch surface cannot be the frame
    }
    capture_used_ = false;
    if (capture_full_ && want_rgba && options_.dump_passes_dir.empty() &&
        EnsureCaptureBuffer(VkDeviceSize(SW(kWidth)) * SH(kHeight) * 4)) {
      // The whole frame as rendered (picture-shaped at any internal size).
      VkBufferImageCopy full{};
      full.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      full.imageExtent = {SW(kWidth), SH(kHeight), 1};
      vkCmdCopyImageToBuffer(cmd_, last_draw_rt->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             capture_buf_, 1, &full);
      capture_used_ = true;
      shot_w_ = SW(kWidth);
      shot_h_ = SH(kHeight);
    } else if (Unscaled()) {
      vkCmdCopyImageToBuffer(cmd_, last_draw_rt->color,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1,
                             &copy);
    } else if (want_rgba && EnsureReadbackImage()) {
      // Scaled rendering: filter down to 1280x720 first.
      VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
      b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.image = readback_img_;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
      d2.imageMemoryBarrierCount = 1;
      d2.pImageMemoryBarriers = &b;
      vkCmdPipelineBarrier2(cmd_, &d2);
      VkImageBlit blit{};
      blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.srcOffsets[1] = {int32_t(SW(kWidth)), int32_t(SH(kHeight)), 1};
      blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      blit.dstOffsets[1] = {int32_t(shot_w_), int32_t(shot_h_), 1};
      vkCmdBlitImage(cmd_, last_draw_rt->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     readback_img_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                     VK_FILTER_LINEAR);
      b.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
      b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      b.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
      b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      vkCmdPipelineBarrier2(cmd_, &d2);
      vkCmdCopyImageToBuffer(cmd_, readback_img_,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback_, 1,
                             &copy);
    }
  }
  // Window presentation: blit the presented surface (already in
  // TRANSFER_SRC after the readback copy) into the acquired swapchain image.
  uint32_t present_index = UINT32_MAX;
  if (options_.window && swapchain_ != VK_NULL_HANDLE) {
    const VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                               acquire_sem_, VK_NULL_HANDLE,
                                               &present_index);
    if (acq == VK_ERROR_SURFACE_LOST_KHR) surface_lost_ = true;
    if (acq == VK_ERROR_OUT_OF_DATE_KHR || (acq == VK_SUBOPTIMAL_KHR && kRebuildOnSuboptimal)) {
      swapchain_dirty_ = true;
    }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) present_index = UINT32_MAX;
  }
  if (present_index != UINT32_MAX && !EnsurePresentResources()) {
    present_index = UINT32_MAX;  // (acquired image is simply not presented)
  }
  if (present_index != UINT32_MAX) {
    VkImage sc = sc_images_[present_index];
    // Output (TRANSFER_SRC after the readback copy) -> present source.
    VkImageMemoryBarrier2 b0{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b0.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b0.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b0.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b0.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b0.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b0.image = present_src_;
    b0.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b0;
    vkCmdPipelineBarrier2(cmd_, &dep);
    VkImageCopy copy{};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.extent = {SW(kWidth), SH(kHeight), 1};
    vkCmdCopyImage(cmd_, last_draw_rt->color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   present_src_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    VkImageMemoryBarrier2 b1[2] = {};
    b1[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b1[0].srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b1[0].srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b1[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b1[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    b1[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b1[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b1[0].image = present_src_;
    b1[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b1[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b1[1].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b1[1].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    b1[1].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    b1[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b1[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b1[1].image = sc;
    b1[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dep.imageMemoryBarrierCount = 2;
    dep.pImageMemoryBarriers = b1;
    vkCmdPipelineBarrier2(cmd_, &dep);

    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    att.imageView = sc_views_[present_index];
    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;  // black bars
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, sc_image_extent_};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
    vkCmdBeginRendering(cmd_, &ri);
    // 16:9 picture centred in the window unless stretching was asked for.
    float pw = float(sc_extent_.width), ph = float(sc_extent_.height);
    float px = 0.0f, py = 0.0f;
    if (!options_.present_stretch) {
      const float want = options_.picture_aspect > 0.0f
                             ? options_.picture_aspect
                             : float(kWidth) / float(kHeight);
      if (pw / ph > want) {
        const float w2 = ph * want;
        px = (pw - w2) * 0.5f;
        pw = w2;
      } else {
        const float h2 = pw / want;
        py = (ph - h2) * 0.5f;
        ph = h2;
      }
    }
    const VkViewport pvp = MapViewport(px, py, pw, ph);
    VkRect2D psc = MapRect(0.0f, 0.0f, float(sc_extent_.width), float(sc_extent_.height));
    if (!options_.present_stretch && options_.picture_aspect > 0.0f &&
        options_.aspect_mask_2d && !frame_is_scene_) {
      // 16:9 band of the picture rectangle; the rest stays black.
      const float rel = options_.picture_aspect / (16.0f / 9.0f);
      const float bw = rel > 1.0f ? pw / rel : pw;
      const float bh = rel < 1.0f ? ph * rel : ph;
      psc = MapRect(px + (pw - bw) * 0.5f, py + (ph - bh) * 0.5f, bw, bh);
    }
    vkCmdSetViewport(cmd_, 0, 1, &pvp);
    vkCmdSetScissor(cmd_, 0, 1, &psc);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            present_layout_, 0, 1, &present_set_, 0, nullptr);
    const float gamma = options_.output_gamma > 0.0f ? options_.output_gamma : 1.0f;
    struct {
      float gamma, sharpen, texel_x, texel_y;
      uint32_t mode;
      float paper, peak;
      uint32_t fxaa;
    } present_pc{gamma,
                 std::min(1.0f, std::max(0.0f, options_.sharpen)),
                 1.0f / float(SW(kWidth)),
                 1.0f / float(SH(kHeight)),
                 hdr_mode_,
                 options_.hdr_paper_nits,
                 options_.hdr_peak_nits,
                 options_.fxaa ? 1u : 0u};
    vkCmdPushConstants(cmd_, present_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32,
                       &present_pc);
    float present_rot[2];
    QuarterTurn(present_rot);
    vkCmdPushConstants(cmd_, present_layout_, VK_SHADER_STAGE_VERTEX_BIT, 32, 8, present_rot);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    DrawOverlay();
    vkCmdEndRendering(cmd_);

    VkImageMemoryBarrier2 to_present{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    to_present.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    to_present.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    to_present.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    to_present.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    to_present.image = sc;
    to_present.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &to_present;
    vkCmdPipelineBarrier2(cmd_, &dep);
  }
  if (gpu_used_[slot_] != 0) GpuStamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
  VK_CHECK(vkEndCommandBuffer(cmd_));

  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &cmd_;
  const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  if (present_index != UINT32_MAX) {
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquire_sem_;
    submit.pWaitDstStageMask = &wait_stage;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_sem_;
  }
  // Record is done; now the previous frame must be (one frame in flight at a
  // time: frames share render targets and persistent images, and the GPU
  // executes them in order anyway).
  const auto t_submit = std::chrono::steady_clock::now();
  recording_ = false;
  WaitFlight();
  const auto t_waited = std::chrono::steady_clock::now();
  VK_CHECK(vkResetFences(device_, 1, &slots_[slot_].fence));
  VK_CHECK(vkQueueSubmit(queue_, 1, &submit, slots_[slot_].fence));
  slots_[slot_].in_flight = true;
  if (present_index != UINT32_MAX) {
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &render_sem_;
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &present_index;
    const VkResult pr = vkQueuePresentKHR(queue_, &pi);
    if (pr == VK_ERROR_SURFACE_LOST_KHR) surface_lost_ = true;
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || (pr == VK_SUBOPTIMAL_KHR && kRebuildOnSuboptimal)) {
      swapchain_dirty_ = true;
    }
  }
  // Readbacks, pass dumps and --vktrace read what this frame wrote.
  if (want_rgba || options_.trace) WaitFlight();
  {
    const auto t_done = std::chrono::steady_clock::now();
    static uint64_t rec_ns = 0, wait_ns = 0, tail_ns = 0, n = 0;
    rec_ns += uint64_t((t_submit - t_frame0).count());
    wait_ns += uint64_t((t_waited - t_submit).count());
    tail_ns += uint64_t((t_done - t_waited).count());
    // New pipelines reach the cache file once a burst of them is over (a
    // new screen builds several over a few frames).
    if (pipelines_unsaved_ > 0 &&
        t_done - last_pipeline_built_ > std::chrono::seconds(3)) {
      SavePipelineCache();
    }
    const uint64_t first_use_ns = stat_tex_fetch_ns_ + stat_tex_up_ns_ + stat_pipe_ns_;
    if (first_use_ns > 15000000ull) {
      // A frame held up by first uses (new robot, new screen).
      std::printf("[vk] slow frame %llu: %.1f ms total; %u textures (fetch+decode %.1f ms, "
                  "upload %.1f ms), %u pipelines (%.1f ms)\n",
                  static_cast<unsigned long long>(frame.index),
                  double((t_done - t_frame0).count()) / 1e6, stat_tex_n_,
                  double(stat_tex_fetch_ns_) / 1e6, double(stat_tex_up_ns_) / 1e6,
                  stat_pipe_n_, double(stat_pipe_ns_) / 1e6);
      std::fflush(stdout);
    }
    if (++n % 120 == 0) {
      std::printf("[vk] timing (120 frames): record %.1f ms (%.1f ms of it stalled on the GPU, "
                  "%.1f stalls), wait for previous frame %.1f ms, submit+present %.1f ms, "
                  "upload buffer peak %.1f MB (since start %.1f MB)\n",
                  rec_ns / 120.0e6, stall_ns_ / 120.0e6, stall_count_ / 120.0, wait_ns / 120.0e6,
                  tail_ns / 120.0e6, double(arena_peak_) / 1048576.0,
                  double(arena_peak_ever_) / 1048576.0);
      arena_peak_ = 0;
      rec_ns = wait_ns = tail_ns = 0;
      stall_ns_ = stall_count_ = 0;
    }
  }

  for (const DepthDbg& dd : depth_dbg) {
    const float* v = reinterpret_cast<const float*>(arena_ptr_ + dd.at);
    float mn = 2.0f, mx = -1.0f;
    for (size_t i = 0; i < size_t(dd.w) * dd.h; ++i) {
      mn = std::min(mn, v[i]);
      mx = std::max(mx, v[i]);
    }
    std::printf("[trace] depth %08X %ux%u min=%.6f max=%.6f grid:", dd.phys,
                dd.w, dd.h, mn, mx);
    for (uint32_t gy = 1; gy < 4; ++gy) {
      for (uint32_t gx = 1; gx < 8; ++gx) {
        std::printf(" %.5f", v[size_t(dd.h * gy / 4) * dd.w + dd.w * gx / 8]);
      }
      std::printf(" |");
    }
    std::printf("%c", 10);
  }
  if (!want_rgba) {
    uint32_t indexed = 0;
    for (const DrawRecord& d : frame.draws) indexed += d.kind == DrawKind::kIndexed;
    std::printf("[vk] frame %llu: drawn=%u skipped=%u indexed=%u resolves=%zu\n",
                static_cast<unsigned long long>(frame.index), drawn, skipped,
                indexed, frame.resolves.size());
    return true;
  }
  void* readback_ptr = nullptr;
  VkDeviceMemory mem = capture_used_ ? capture_mem_ : readback_mem_;
  VK_CHECK(vkMapMemory(device_, mem, 0, VK_WHOLE_SIZE, 0, &readback_ptr));
  rgba.resize(size_t(shot_w_) * shot_h_ * 4);
  std::memcpy(rgba.data(), readback_ptr, rgba.size());
  vkUnmapMemory(device_, mem);
  if (capture_used_ && options_.output_gamma > 0.0f && options_.output_gamma != 1.0f) {
    // The present pass's tone curve, so the capture looks as it does on screen.
    uint8_t lut[256];
    for (int i = 0; i < 256; ++i) {
      lut[i] = uint8_t(std::lround(255.0 * std::pow(i / 255.0, double(options_.output_gamma))));
    }
    for (size_t i = 0; i < rgba.size(); i += 4) {
      rgba[i] = lut[rgba[i]];
      rgba[i + 1] = lut[rgba[i + 1]];
      rgba[i + 2] = lut[rgba[i + 2]];
    }
  }
  if ((shot_w_ != kWidth || shot_h_ != kHeight) && !options_.present_stretch &&
      options_.aspect_mask_2d && !frame_is_scene_) {
    // As presented: only the 16:9 band of a non-scene frame is shown.
    const float rel = options_.picture_aspect / (16.0f / 9.0f);
    const uint32_t bw = rel > 1.0f ? uint32_t(float(shot_w_) / rel + 0.5f) : shot_w_;
    const uint32_t bh = rel < 1.0f ? uint32_t(float(shot_h_) * rel + 0.5f) : shot_h_;
    const uint32_t x0 = (shot_w_ - bw) / 2, y0 = (shot_h_ - bh) / 2;
    for (uint32_t y = 0; y < shot_h_; ++y) {
      for (uint32_t x = 0; x < shot_w_; ++x) {
        if (x >= x0 && x < x0 + bw && y >= y0 && y < y0 + bh) continue;
        uint8_t* p = &rgba[(size_t(y) * shot_w_ + x) * 4];
        p[0] = p[1] = p[2] = 0;
      }
    }
  }

  if (!options_.dump_passes_dir.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(options_.dump_passes_dir, ec);
    for (const PassDump& dump : pass_dumps) {
      std::vector<uint8_t> pass_rgba(size_t(kWidth) * kHeight * 4);
      std::memcpy(pass_rgba.data(), arena_ptr_ + dump.offset,
                  pass_rgba.size());
      char name[160];
      std::snprintf(name, sizeof(name), "%s/pass_%03u_key%08llX_d%u.png",
                    options_.dump_passes_dir.c_str(), dump.index,
                    static_cast<unsigned long long>(dump.key), dump.draws);
      WriteRgbaPng(name, pass_rgba, kWidth, kHeight);
      std::printf("[vk] pass %03u key=%08llX draws=%u vp=%ux%u -> %s\n",
                  dump.index, static_cast<unsigned long long>(dump.key),
                  dump.draws, dump.vp_w, dump.vp_h, name);
    }
  }

  if (!options_.dump_passes_dir.empty()) {
    WriteRgbaPng(options_.dump_passes_dir + "/final.png", rgba, kWidth,
                 kHeight);
  }
  if (gamma_ramp_valid_ && options_.output_gamma == 1.0f &&
      !options_.no_gamma_ramp) {
    // v14: the title's own display ramp (what the xenos plugin's presenter
    // applies). Tables are 10-bit; assume R, G, B order (dest-512, dest,
    // dest+512) until a capture says otherwise.
    uint8_t lut[3][256];
    for (uint32_t ch = 0; ch < 3; ++ch) {
      for (int i = 0; i < 256; ++i) {
        lut[ch][i] = static_cast<uint8_t>(
            std::lround(255.0f * std::min(1.0f, std::max(0.0f, gamma_ramp_[ch][i]))));
      }
    }
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      rgba[i] = lut[0][rgba[i]];
      rgba[i + 1] = lut[1][rgba[i + 1]];
      rgba[i + 2] = lut[2][rgba[i + 2]];
    }
  } else if (options_.output_gamma > 0.0f && options_.output_gamma != 1.0f) {
    uint8_t lut[256];
    for (int i = 0; i < 256; ++i) {
      lut[i] = static_cast<uint8_t>(std::lround(
          255.0 * std::pow(i / 255.0, double(options_.output_gamma))));
    }
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
      rgba[i] = lut[rgba[i]];
      rgba[i + 1] = lut[rgba[i + 1]];
      rgba[i + 2] = lut[rgba[i + 2]];
    }
  }
  std::printf("[vk] frame %llu: drawn=%u skipped=%u\n",
              static_cast<unsigned long long>(frame.index), drawn, skipped);
  return true;
}

// Reads the finished frame's timestamps of this slot: [0] frame start, then a
// begin / end pair per pass, last the frame end (after the present pass).
void Backend::GpuCollect() {
  const uint32_t n = gpu_used_[slot_];
  gpu_used_[slot_] = 0;
  if (n < 2 || gpu_pool_[slot_] == VK_NULL_HANDLE) return;
  std::vector<uint64_t> t(n);
  if (vkGetQueryPoolResults(device_, gpu_pool_[slot_], 0, n, n * sizeof(uint64_t), t.data(),
                            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
    return;
  }
  const auto ms = [&](uint32_t a, uint32_t b) { return double(t[b] - t[a]) * gpu_ms_per_tick_; };
  const std::vector<GpuPassInfo>& info = gpu_info_[slot_];
  const uint32_t passes = std::min<uint32_t>(uint32_t(info.size()), (n - 2) / 2);
  double inside = 0.0;
  for (uint32_t p = 0; p < passes; ++p) {
    const double d = ms(1 + p * 2, 2 + p * 2);
    inside += d;
    auto& sum = gpu_pass_sum_[p];
    sum.first += d;
    sum.second = info[p];
  }
  const double frame = ms(0, n - 1);
  gpu_frame_sum_ += frame;
  gpu_gap_sum_ += frame - inside;
  // Report once the run is over (a frame still in the other slot afterwards
  // is collected but not reported on its own).
  if (++gpu_profile_done_ < 60 || gpu_profile_frames_ != 0) return;
  const double frames = double(gpu_profile_done_);
  std::vector<std::pair<double, uint32_t>> order;
  for (const auto& [p, sum] : gpu_pass_sum_) order.push_back({sum.first / frames, p});
  std::sort(order.rbegin(), order.rend());
  std::printf("[gpu] %u frames: %.2f ms a frame on the GPU, %.2f ms of it outside passes "
              "(resolves, copies, present), %zu passes\n",
              gpu_profile_done_, gpu_frame_sum_ / frames, gpu_gap_sum_ / frames, gpu_pass_sum_.size());
  for (size_t i = 0; i < order.size() && i < 14; ++i) {
    const GpuPassInfo& pi = gpu_pass_sum_[order[i].second].second;
    std::printf("[gpu]   pass %3u  %6.2f ms  %ux%u  %u draws\n", order[i].second, order[i].first,
                pi.w, pi.h, pi.draws);
  }
  std::fflush(stdout);
  gpu_profile_done_ = 0;
}

void Backend::CreatePipelineCache() {
  if (options_.pipeline_cache_path.empty()) return;
  std::vector<uint8_t> data;
  if (FILE* f = std::fopen(options_.pipeline_cache_path.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size > 32) {
      data.resize(size_t(size));
      if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
    }
    std::fclose(f);
  }
  if (!data.empty()) {
    // Header (VkPipelineCacheHeaderVersionOne): length, version, vendor,
    // device, UUID. Data from another GPU or driver is dropped here rather
    // than trusted to the driver's own check.
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(phys_, &props);
    uint32_t header[4];
    std::memcpy(header, data.data(), 16);
    if (header[0] < 32 || header[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE ||
        header[2] != props.vendorID || header[3] != props.deviceID ||
        std::memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) != 0) {
      std::printf("[vk] pipeline cache %s is from another GPU or driver - starting afresh\n",
                  options_.pipeline_cache_path.c_str());
      data.clear();
    }
  }
  VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  ci.initialDataSize = data.size();
  ci.pInitialData = data.empty() ? nullptr : data.data();
  if (vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
    ci.initialDataSize = 0;
    ci.pInitialData = nullptr;
    if (vkCreatePipelineCache(device_, &ci, nullptr, &pipeline_cache_) != VK_SUCCESS) {
      pipeline_cache_ = VK_NULL_HANDLE;
    }
  }
  std::printf("[vk] pipeline cache: %zu bytes loaded from %s\n", data.size(),
              options_.pipeline_cache_path.c_str());
}

void Backend::SavePipelineCache() {
  pipelines_unsaved_ = 0;
  if (pipeline_cache_ == VK_NULL_HANDLE || options_.pipeline_cache_path.empty()) return;
  size_t size = 0;
  if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || size == 0) {
    return;
  }
  std::vector<uint8_t> data(size);
  if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, data.data()) != VK_SUCCESS) return;
  const std::string tmp = options_.pipeline_cache_path + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "wb");
  if (f == nullptr) return;
  const bool ok = std::fwrite(data.data(), 1, size, f) == size;
  std::fclose(f);
  std::error_code ec;
  if (ok) std::filesystem::rename(tmp, options_.pipeline_cache_path, ec);
  if (!ok || ec) std::filesystem::remove(tmp, ec);
}

Backend::~Backend() {
  if (device_ != VK_NULL_HANDLE) {
    vkDeviceWaitIdle(device_);
    if (pipelines_unsaved_ > 0) SavePipelineCache();
    if (pipeline_cache_) vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
    for (auto& [key, pipeline] : pipelines_) {
      if (pipeline) vkDestroyPipeline(device_, pipeline, nullptr);
    }
    for (auto& [hash, module] : modules_) {
      if (module) vkDestroyShaderModule(device_, module, nullptr);
    }
    for (VkImageView v : image_views_) vkDestroyImageView(device_, v, nullptr);
    for (VkImage i : images_) vkDestroyImage(device_, i, nullptr);
    for (VkDeviceMemory m : image_mems_) vkFreeMemory(device_, m, nullptr);
    for (auto& [key, rt] : rt_pool_) DestroyRt(rt);
    DestroyRetired();
    for (auto& [id, t] : overlay_textures_) DestroyOverlayTexture(t);
    DestroyDepthStage();
    if (overlay_pipeline_) vkDestroyPipeline(device_, overlay_pipeline_, nullptr);
    if (overlay_layout_) vkDestroyPipelineLayout(device_, overlay_layout_, nullptr);
    if (overlay_pool_) vkDestroyDescriptorPool(device_, overlay_pool_, nullptr);
    if (present_pipeline_) vkDestroyPipeline(device_, present_pipeline_, nullptr);
    if (present_layout_) vkDestroyPipelineLayout(device_, present_layout_, nullptr);
    if (present_pool_) vkDestroyDescriptorPool(device_, present_pool_, nullptr);
    if (present_dsl_) vkDestroyDescriptorSetLayout(device_, present_dsl_, nullptr);
    if (present_sampler_) vkDestroySampler(device_, present_sampler_, nullptr);
    if (present_src_view_) vkDestroyImageView(device_, present_src_view_, nullptr);
    if (present_src_) vkDestroyImage(device_, present_src_, nullptr);
    if (present_src_mem_) vkFreeMemory(device_, present_src_mem_, nullptr);
    if (readback_img_) vkDestroyImage(device_, readback_img_, nullptr);
    if (readback_img_mem_) vkFreeMemory(device_, readback_img_mem_, nullptr);
    for (VkImageView v : sc_views_) {
      if (v) vkDestroyImageView(device_, v, nullptr);
    }
    sc_views_.clear();
    for (auto& [phys, dst] : resolve_dsts_) {
      if (dst.view) vkDestroyImageView(device_, dst.view, nullptr);
      if (dst.image) vkDestroyImage(device_, dst.image, nullptr);
      if (dst.memory) vkFreeMemory(device_, dst.memory, nullptr);
    }
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
    for (VkSampler& x : extra_samplers_) {
      if (x) vkDestroySampler(device_, x, nullptr);
      x = VK_NULL_HANDLE;
    }
    if (desc_pool_) vkDestroyDescriptorPool(device_, desc_pool_, nullptr);
    for (VkDescriptorSetLayout l : set_layouts_) {
      if (l) vkDestroyDescriptorSetLayout(device_, l, nullptr);
    }
    if (readback_) vkDestroyBuffer(device_, readback_, nullptr);
    if (readback_mem_) vkFreeMemory(device_, readback_mem_, nullptr);
    if (capture_buf_) vkDestroyBuffer(device_, capture_buf_, nullptr);
    if (capture_mem_) vkFreeMemory(device_, capture_mem_, nullptr);
    if (color_view_) vkDestroyImageView(device_, color_view_, nullptr);
    if (depth_view_) vkDestroyImageView(device_, depth_view_, nullptr);
    if (color_) vkDestroyImage(device_, color_, nullptr);
    if (depth_) vkDestroyImage(device_, depth_, nullptr);
    if (color_mem_) vkFreeMemory(device_, color_mem_, nullptr);
    if (depth_mem_) vkFreeMemory(device_, depth_mem_, nullptr);
    for (FrameSlot& slot : slots_) {
      if (slot.arena) vkDestroyBuffer(device_, slot.arena, nullptr);
      if (slot.arena_mem) vkFreeMemory(device_, slot.arena_mem, nullptr);
      if (slot.fence) vkDestroyFence(device_, slot.fence, nullptr);
      if (slot.acquire_sem) vkDestroySemaphore(device_, slot.acquire_sem, nullptr);
      if (slot.render_sem) vkDestroySemaphore(device_, slot.render_sem, nullptr);
    }
    if (pool_) vkDestroyCommandPool(device_, pool_, nullptr);
    vkDestroyDevice(device_, nullptr);
  }
  if (instance_ != VK_NULL_HANDLE) vkDestroyInstance(instance_, nullptr);
}

}  // namespace

#ifdef _WIN32
namespace {
bool g_window_closed = false;
LRESULT CALLBACK ReplayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_CLOSE:
      g_window_closed = true;
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_KEYDOWN:
      if (wp == VK_ESCAPE) {
        g_window_closed = true;
        DestroyWindow(hwnd);
      }
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wp, lp);
  }
}
}  // namespace

bool Backend::CreateWindowSurface() {
  if (options_.external_window != nullptr) {
    VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    sci.hinstance = GetModuleHandleW(nullptr);
    sci.hwnd = static_cast<HWND>(options_.external_window);
    VK_CHECK(vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_));
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(phys_, queue_family_, surface_, &supported);
    if (!supported) {
      std::printf("[vk] queue family cannot present to the host window\n");
      return false;
    }
    window_open_ = true;  // lifetime and messages belong to the owner
    return true;
  }
  WNDCLASSW wc{};
  wc.lpfnWndProc = ReplayWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"rs_replay_window";
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));  // IDC_ARROW
  RegisterClassW(&wc);
  RECT r{0, 0, LONG(options_.window_width), LONG(options_.window_height)};
  AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
  hwnd_ = CreateWindowExW(options_.window_background ? WS_EX_NOACTIVATE : 0,
                          wc.lpszClassName, L"Real Steel — native replay",
                          WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                          r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                          wc.hInstance, nullptr);
  if (hwnd_ == nullptr) {
    std::printf("[vk] CreateWindowEx failed\n");
    return false;
  }
  ShowWindow(hwnd_, options_.window_background ? SW_SHOWNOACTIVATE : SW_SHOW);
  VkWin32SurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
  sci.hinstance = wc.hInstance;
  sci.hwnd = hwnd_;
  VK_CHECK(vkCreateWin32SurfaceKHR(instance_, &sci, nullptr, &surface_));
  VkBool32 supported = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(phys_, queue_family_, surface_, &supported);
  if (!supported) {
    std::printf("[vk] queue family cannot present to the window\n");
    return false;
  }
  window_open_ = true;
  g_window_closed = false;
  return true;
}

void Backend::PumpMessages() {
  if (hwnd_ == nullptr) return;  // also the external-window case
  MSG msg;
  while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (g_window_closed) window_open_ = false;
}
#else
// Linux: only the host-window case (the game's SDL window). The surface is
// created through vkGetInstanceProcAddr with locally declared create-info
// structs, so the build needs neither the X11 nor the Wayland headers.
bool Backend::CreateWindowSurface() {
  struct XlibSurfaceCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    void* dpy;
    unsigned long window;
  };
  struct WaylandSurfaceCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    void* display;
    void* surface;
  };
  struct AndroidSurfaceCreateInfo {
    VkStructureType sType;
    const void* pNext;
    VkFlags flags;
    void* window;
  };
  VkResult r = VK_ERROR_EXTENSION_NOT_PRESENT;
  if (options_.android_window != nullptr) {
    using Fn = VkResult(VKAPI_PTR*)(VkInstance, const AndroidSurfaceCreateInfo*,
                                    const VkAllocationCallbacks*, VkSurfaceKHR*);
    const Fn fn = reinterpret_cast<Fn>(
        vkGetInstanceProcAddr(instance_, "vkCreateAndroidSurfaceKHR"));
    AndroidSurfaceCreateInfo ci{};
    ci.sType = static_cast<VkStructureType>(1000008000);
    ci.window = options_.android_window;
    if (fn != nullptr) r = fn(instance_, &ci, nullptr, &surface_);
  } else if (options_.wayland_display != nullptr && options_.wayland_surface != nullptr) {
    using Fn = VkResult(VKAPI_PTR*)(VkInstance, const WaylandSurfaceCreateInfo*,
                                    const VkAllocationCallbacks*, VkSurfaceKHR*);
    const Fn fn = reinterpret_cast<Fn>(
        vkGetInstanceProcAddr(instance_, "vkCreateWaylandSurfaceKHR"));
    WaylandSurfaceCreateInfo ci{};
    ci.sType = static_cast<VkStructureType>(1000006000);
    ci.display = options_.wayland_display;
    ci.surface = options_.wayland_surface;
    if (fn != nullptr) r = fn(instance_, &ci, nullptr, &surface_);
  } else if (options_.x11_display != nullptr && options_.x11_window != 0) {
    using Fn = VkResult(VKAPI_PTR*)(VkInstance, const XlibSurfaceCreateInfo*,
                                    const VkAllocationCallbacks*, VkSurfaceKHR*);
    const Fn fn = reinterpret_cast<Fn>(
        vkGetInstanceProcAddr(instance_, "vkCreateXlibSurfaceKHR"));
    XlibSurfaceCreateInfo ci{};
    ci.sType = static_cast<VkStructureType>(1000004000);
    ci.dpy = options_.x11_display;
    ci.window = options_.x11_window;
    if (fn != nullptr) r = fn(instance_, &ci, nullptr, &surface_);
  } else {
    std::printf("[vk] no host window handles: a window of our own is Win32 only\n");
    return false;
  }
  if (r != VK_SUCCESS) {
    std::printf("[vk] host window surface creation failed: %d\n", int(r));
    return false;
  }
  VkBool32 supported = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(phys_, queue_family_, surface_, &supported);
  if (!supported) {
    std::printf("[vk] queue family cannot present to the host window\n");
    return false;
  }
  window_open_ = true;  // lifetime and events belong to the owner
  return true;
}
void Backend::PumpMessages() {}
#endif

bool Backend::CreateSwapchain() {
  VkSurfaceCapabilitiesKHR caps;
  VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps));
  uint32_t fmt_count = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fmt_count, nullptr);
  std::vector<VkSurfaceFormatKHR> fmts(fmt_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &fmt_count, fmts.data());
  sc_format_ = fmts.empty() ? VK_FORMAT_B8G8R8A8_UNORM : fmts[0].format;
  VkColorSpaceKHR cs = fmts.empty() ? VK_COLOR_SPACE_SRGB_NONLINEAR_KHR : fmts[0].colorSpace;
  for (const VkSurfaceFormatKHR& f : fmts) {
    if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
      sc_format_ = f.format;
      cs = f.colorSpace;
      break;
    }
  }
  // HDR: HDR10 (10-bit, BT.2020 + PQ) first - what TVs, Windows HDR and the
  // Steam Deck OLED's compositor take - then scRGB (16-bit float, linear).
  hdr_mode_ = 0;
  {
    static bool listed = false;
    if (!listed && have_colorspace_ext_) {
      listed = true;
      std::printf("[vk] surface formats (format/colour space):");
      for (const VkSurfaceFormatKHR& f : fmts) std::printf(" %d/%d", int(f.format), int(f.colorSpace));
      std::printf("\n");
    }
  }
  if (options_.hdr && have_colorspace_ext_) {
    for (const VkSurfaceFormatKHR& f : fmts) {
      if (f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT &&
          (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
           f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32)) {
        sc_format_ = f.format;
        cs = f.colorSpace;
        hdr_mode_ = 1;
        break;
      }
    }
    for (const VkSurfaceFormatKHR& f : fmts) {
      if (hdr_mode_ != 0) break;
      if (f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT &&
          f.format == VK_FORMAT_R16G16B16A16_SFLOAT) {
        sc_format_ = f.format;
        cs = f.colorSpace;
        hdr_mode_ = 2;
      }
    }
  }
  if (options_.hdr) {
    std::printf("[vk] HDR %s\n", hdr_mode_ == 1   ? "on: HDR10 (PQ)"
                                  : hdr_mode_ == 2 ? "on: scRGB"
                                                   : "asked for, but the display offers no HDR format - SDR");
  }
  sc_extent_ = caps.currentExtent;
#if defined(__ANDROID__)
  sc_extent_ = WindowExtent(caps);
#endif
  sc_extent_from_owner_ = sc_extent_.width == 0xFFFFFFFFu;
  if (sc_extent_from_owner_) {
    // Wayland: the surface takes the size of what is presented to it.
    sc_extent_ = {options_.window_width, options_.window_height};
    if (options_.window_size != nullptr) {
      uint32_t w = 0, h = 0;
      options_.window_size(&w, &h);
      if (w != 0 && h != 0) sc_extent_ = {w, h};
    }
    sc_extent_.width = std::min(caps.maxImageExtent.width,
                                std::max(caps.minImageExtent.width, sc_extent_.width));
    sc_extent_.height = std::min(caps.maxImageExtent.height,
                                 std::max(caps.minImageExtent.height, sc_extent_.height));
  }
  if (sc_extent_.width == 0 || sc_extent_.height == 0) {
    swapchain_dirty_ = true;  // minimised; try again next frame
    return true;
  }
  VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  sci.surface = surface_;
  sci.minImageCount = std::max(2u, caps.minImageCount);
  if (caps.maxImageCount != 0) sci.minImageCount = std::min(sci.minImageCount, caps.maxImageCount);
  sci.imageFormat = sc_format_;
  sci.imageColorSpace = cs;
  sc_image_extent_ = sc_extent_;
  sc_quarter_turns_ = 0;
  sc_transform_ = caps.currentTransform;
#if defined(__ANDROID__)
  // Pre-rotation: when the display is turned from the panel's own
  // orientation (a phone held sideways; a handheld with a portrait phone
  // panel mounted sideways, as on some handhelds), draw the picture
  // turned and tell the compositor so (Android's recommended path).
  if (options_.prerotate && (caps.supportedTransforms & caps.currentTransform) != 0) {
    switch (caps.currentTransform) {
      case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: sc_quarter_turns_ = 1; break;
      case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: sc_quarter_turns_ = 2; break;
      case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: sc_quarter_turns_ = 3; break;
      default: break;
    }
    if (sc_quarter_turns_ & 1) {
      sc_image_extent_ = {sc_extent_.height, sc_extent_.width};
      if (sc_image_extent_.width > caps.maxImageExtent.width ||
          sc_image_extent_.height > caps.maxImageExtent.height) {
        sc_quarter_turns_ = 0;  // (never expected) the compositor turns it
        sc_image_extent_ = sc_extent_;
      }
    }
  }
#endif
  sci.imageExtent = sc_image_extent_;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
#if defined(__ANDROID__)
  sci.preTransform = sc_quarter_turns_ != 0 ? caps.currentTransform
                                            : VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
#endif
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) {
    for (uint32_t bit = 1; bit <= 8; bit <<= 1) {
      if (caps.supportedCompositeAlpha & bit) {
        sci.compositeAlpha = static_cast<VkCompositeAlphaFlagBitsKHR>(bit);
        break;
      }
    }
  }
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  if (!options_.vsync) {
    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &mode_count,
                                              modes.data());
    bool mailbox = false, immediate = false;
    for (VkPresentModeKHR m : modes) {
      mailbox = mailbox || m == VK_PRESENT_MODE_MAILBOX_KHR;
      immediate = immediate || m == VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
    if (mailbox) {
      sci.presentMode = VK_PRESENT_MODE_MAILBOX_KHR;
    } else if (immediate) {
      sci.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    }
  }
  std::printf("[vk] present mode %d (vsync %s)\n", int(sci.presentMode),
              options_.vsync ? "on" : "off");
  sci.clipped = VK_TRUE;
  VK_CHECK(vkCreateSwapchainKHR(device_, &sci, nullptr, &swapchain_));
  if (hdr_mode_ != 0 && have_hdr_metadata_ && vkSetHdrMetadataEXT != nullptr) {
    // Tells the display / compositor what to expect (BT.2020 primaries,
    // D65, our peak), so its own tone mapping leaves the range alone.
    VkHdrMetadataEXT md{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
    md.displayPrimaryRed = {0.708f, 0.292f};
    md.displayPrimaryGreen = {0.170f, 0.797f};
    md.displayPrimaryBlue = {0.131f, 0.046f};
    md.whitePoint = {0.3127f, 0.3290f};
    md.maxLuminance = options_.hdr_peak_nits;
    md.minLuminance = 0.0f;
    md.maxContentLightLevel = options_.hdr_peak_nits;
    md.maxFrameAverageLightLevel = options_.hdr_paper_nits;
    vkSetHdrMetadataEXT(device_, 1, &swapchain_, &md);
  }
  uint32_t n = 0;
  vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
  sc_images_.resize(n);
  vkGetSwapchainImagesKHR(device_, swapchain_, &n, sc_images_.data());
  sc_views_.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.image = sc_images_[i];
    vci.format = sc_format_;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &sc_views_[i]));
  }
  swapchain_dirty_ = false;
  std::printf("[vk] swapchain %ux%u (images %ux%u), %u images, display transform 0x%x, "
              "pre-rotated %u x 90\n", sc_extent_.width, sc_extent_.height,
              sc_image_extent_.width, sc_image_extent_.height, n,
              unsigned(caps.currentTransform), sc_quarter_turns_);
  std::fflush(stdout);  // (startup lines reach the file even if a crash follows)
  return true;
}

// Ambient occlusion (2026-10-18, off by default): see ao_shaders.hlsl. Two
// passes once a frame, just before the game first copies the 3D scene
// (RenderFrame's do_resolve): the occlusion from the main surface's depth
// into ao_image_, then a depth-aware blur of it multiplied into the colour.
bool Backend::EnsureAoResources(VkFormat color_format) {
  if (ao_dsl_ == VK_NULL_HANDLE) {
    VkDescriptorSetLayoutBinding b[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
      b[i].binding = i;  // 0 depth, 1 occlusion (blur pass)
      b[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
      b[i].descriptorCount = 1;
      b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 2;
    dli.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dli, nullptr, &ao_dsl_));
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpi, nullptr, &ao_pool_));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = ao_pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &ao_dsl_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &dai, &ao_set_));
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &ao_dsl_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(device_, &pli, nullptr, &ao_layout_));
  }
  // The occlusion image: one channel, the size of the frame area.
  const uint32_t w = SW(kWidth), h = SH(kHeight);
  if (ao_image_ != VK_NULL_HANDLE && (ao_image_w_ != w || ao_image_h_ != h)) {
    WaitFlight();
    vkDestroyImageView(device_, ao_image_view_, nullptr);
    vkDestroyImage(device_, ao_image_, nullptr);
    vkFreeMemory(device_, ao_image_mem_, nullptr);
    ao_image_view_ = VK_NULL_HANDLE;
    ao_image_ = VK_NULL_HANDLE;
    ao_image_mem_ = VK_NULL_HANDLE;
    ao_bound_view_ = VK_NULL_HANDLE;
  }
  if (ao_image_ == VK_NULL_HANDLE) {
    if (!AllocImage(device_, phys_, w, h, VK_FORMAT_R8_UNORM,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &ao_image_,
                    &ao_image_mem_)) {
      ao_image_ = VK_NULL_HANDLE;
      return false;
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.image = ao_image_;
    vci.format = VK_FORMAT_R8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &ao_image_view_));
    ao_image_w_ = w;
    ao_image_h_ = h;
    ao_bound_view_ = VK_NULL_HANDLE;  // binding 1 needs writing
  }
  if (ao_pipeline_ != VK_NULL_HANDLE && ao_pipeline_format_ == color_format) return true;
  if (ao_pipeline_ != VK_NULL_HANDLE) {
    WaitFlight();
    vkDestroyPipeline(device_, ao_pipeline_, nullptr);
    vkDestroyPipeline(device_, ao_blur_pipeline_, nullptr);
    ao_pipeline_ = ao_blur_pipeline_ = VK_NULL_HANDLE;
  }
  VkShaderModule mods[3] = {};
  VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  mci.codeSize = sizeof(kAoVSSpv);
  mci.pCode = kAoVSSpv;
  VK_CHECK(vkCreateShaderModule(device_, &mci, nullptr, &mods[0]));
  mci.codeSize = sizeof(kAoPSSpv);
  mci.pCode = kAoPSSpv;
  VK_CHECK(vkCreateShaderModule(device_, &mci, nullptr, &mods[1]));
  mci.codeSize = sizeof(kAoBlurPSSpv);
  mci.pCode = kAoBlurPSSpv;
  VK_CHECK(vkCreateShaderModule(device_, &mci, nullptr, &mods[2]));
  VkPipelineShaderStageCreateInfo st[2] = {};
  st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  st[0].module = mods[0];
  st[0].pName = "vs_main";
  st[1] = st[0];
  st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState att{};
  att.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &att;
  const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  VkFormat format = VK_FORMAT_R8_UNORM;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = 1;
  ri.pColorAttachmentFormats = &format;
  VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pci.pNext = &ri;
  pci.stageCount = 2;
  pci.pStages = st;
  pci.pVertexInputState = &vi;
  pci.pInputAssemblyState = &ia;
  pci.pViewportState = &vp;
  pci.pRasterizationState = &rs;
  pci.pMultisampleState = &ms;
  pci.pColorBlendState = &cb;
  pci.pDynamicState = &ds;
  pci.layout = ao_layout_;
  // Pass 1: occlusion into ao_image_, written as is.
  st[1].module = mods[1];
  st[1].pName = "ps_main";
  VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &ao_pipeline_);
  // Pass 2: colour x blurred factor; alpha kept (the title's composite reads it).
  if (r == VK_SUCCESS) {
    st[1].module = mods[2];
    st[1].pName = "ps_blur";
    format = color_format;
    att.blendEnable = VK_TRUE;
    att.srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR;
    att.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
    att.colorBlendOp = VK_BLEND_OP_ADD;
    att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    att.alphaBlendOp = VK_BLEND_OP_ADD;
    r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci, nullptr, &ao_blur_pipeline_);
  }
  for (VkShaderModule m : mods) vkDestroyShaderModule(device_, m, nullptr);
  if (r != VK_SUCCESS) {
    std::printf("[vk] ambient occlusion pipeline creation failed: %d\n", int(r));
    if (ao_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, ao_pipeline_, nullptr);
    ao_pipeline_ = ao_blur_pipeline_ = VK_NULL_HANDLE;
    return false;
  }
  ao_pipeline_format_ = color_format;
  return true;
}

void Backend::RunAmbientOcclusion(RtEntry* rt) {
  if (rt == nullptr || rt->depth == VK_NULL_HANDLE || !rt->depth_ready) return;
  if (!EnsureAoResources(VK_FORMAT_R8G8B8A8_UNORM)) return;
  if (ao_bound_view_ != rt->depth_view) {
    WaitFlight();  // the set may be in use by the frame in flight
    VkDescriptorImageInfo img[2] = {};
    img[0].imageView = rt->depth_view;
    img[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    img[1].imageView = ao_image_view_;
    img[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = ao_set_;
    wr.dstBinding = 0;
    wr.descriptorCount = 2;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    wr.pImageInfo = img;
    vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);
    ao_bound_view_ = rt->depth_view;
  }
  // Depth -> sampled, occlusion image -> attachment.
  VkImageMemoryBarrier2 b[2] = {};
  b[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  b[0].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b[0].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  b[0].oldLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b[0].image = rt->depth;
  b[0].subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  b[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  b[1].srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b[1].srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  b[1].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  b[1].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;  // fully rewritten
  b[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b[1].image = ao_image_;
  b[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep.imageMemoryBarrierCount = 2;
  dep.pImageMemoryBarriers = b;
  vkCmdPipelineBarrier2(cmd_, &dep);

  const uint32_t w = SW(kWidth), h = SH(kHeight);
  const VkViewport viewport{0.0f, 0.0f, float(w), float(h), 0.0f, 1.0f};
  const VkRect2D scissor{{0, 0}, {w, h}};
  struct {
    float near_z, far_z, tan_y, tan_x, size_x, size_y, radius, strength;
  } push;
  push.near_z = camera_near_;
  push.far_z = camera_far_;
  push.tan_y = std::tan(camera_fov_y_ * 0.5f);
  // The game's 16:9 projection; other picture shapes stretch it.
  push.tan_x = push.tan_y * 16.0f / 9.0f;
  push.size_x = float(w);
  push.size_y = float(h);
  push.radius = 60.0f;
  push.strength = options_.ao_strength;

  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = ao_image_view_;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {w, h}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &color;
  vkCmdBeginRendering(cmd_, &ri);
  vkCmdSetViewport(cmd_, 0, 1, &viewport);
  vkCmdSetScissor(cmd_, 0, 1, &scissor);
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_pipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_layout_, 0, 1, &ao_set_, 0, nullptr);
  vkCmdPushConstants(cmd_, ao_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
  vkCmdDraw(cmd_, 3, 1, 0, 0);
  vkCmdEndRendering(cmd_);

  // Occlusion image -> sampled, main surface -> attachment.
  b[0] = {};
  b[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  b[0].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  b[0].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  b[0].dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  b[0].dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  b[0].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  b[0].image = ao_image_;
  b[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b[1] = {};
  b[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
  b[1].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  b[1].srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
  b[1].dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  b[1].dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  b[1].oldLayout = rt->layout;
  b[1].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  b[1].image = rt->color;
  b[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier2(cmd_, &dep);
  rt->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  color.imageView = rt->color_view;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  vkCmdBeginRendering(cmd_, &ri);
  vkCmdSetViewport(cmd_, 0, 1, &viewport);
  vkCmdSetScissor(cmd_, 0, 1, &scissor);
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_blur_pipeline_);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_layout_, 0, 1, &ao_set_, 0, nullptr);
  vkCmdPushConstants(cmd_, ao_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
  vkCmdDraw(cmd_, 3, 1, 0, 0);
  vkCmdEndRendering(cmd_);

  VkImageMemoryBarrier2 back{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  back.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  back.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  back.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                      VK_PIPELINE_STAGE_2_COPY_BIT;
  back.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT;
  back.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  back.newLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
  back.image = rt->depth;
  back.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
  VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dep2.imageMemoryBarrierCount = 1;
  dep2.pImageMemoryBarriers = &back;
  vkCmdPipelineBarrier2(cmd_, &dep2);
  // The game's draws rely on the global sets bound once at the frame's start
  // (texture heaps, samplers, constants): these passes replaced set 0 - bind
  // them again.
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 4, sets_, 0, nullptr);
  vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 4, 1, &cset_, 0, nullptr);
}

bool Backend::EnsurePresentResources() {
  if (present_src_ == VK_NULL_HANDLE) {
    if (!AllocImage(device_, phys_, SW(kWidth), SH(kHeight), VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &present_src_, &present_src_mem_)) {
      return false;
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.image = present_src_;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VK_CHECK(vkCreateImageView(device_, &vci, nullptr, &present_src_view_));
    present_src_written_ = false;
  }
  if (present_dsl_ == VK_NULL_HANDLE) {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &present_sampler_));
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0;
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    b[0].descriptorCount = 1;
    b[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    b[1].binding = 1;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    b[1].descriptorCount = 1;
    b[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 2;
    dli.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(device_, &dli, nullptr, &present_dsl_));
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1},
                                  {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    VK_CHECK(vkCreateDescriptorPool(device_, &dpi, nullptr, &present_pool_));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = present_pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &present_dsl_;
    VK_CHECK(vkAllocateDescriptorSets(device_, &dai, &present_set_));
    VkPushConstantRange ranges[2] = {};
    ranges[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ranges[0].size = 32;  // gamma, sharpen, source texel size, HDR mode/paper/peak, FXAA
    ranges[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    ranges[1].offset = 32;
    ranges[1].size = 8;   // pre-rotation (cos, sin)
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &present_dsl_;
    pli.pushConstantRangeCount = 2;
    pli.pPushConstantRanges = ranges;
    VK_CHECK(vkCreatePipelineLayout(device_, &pli, nullptr, &present_layout_));
  }
  if (!present_src_written_) {
    WaitFlight();
    VkDescriptorImageInfo img{};
    img.imageView = present_src_view_;
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo smp{};
    smp.sampler = present_sampler_;
    VkWriteDescriptorSet w[2] = {};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = present_set_;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &img;
    w[1] = w[0];
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo = &smp;
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    present_src_written_ = true;
  }
  if (present_pipeline_ != VK_NULL_HANDLE && present_pipeline_format_ == sc_format_) {
    return true;
  }
  if (present_pipeline_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, present_pipeline_, nullptr);
    present_pipeline_ = VK_NULL_HANDLE;
  }
  VkShaderModule mods[2] = {};
  VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  mci.codeSize = sizeof(kPresentVSSpv);
  mci.pCode = kPresentVSSpv;
  VK_CHECK(vkCreateShaderModule(device_, &mci, nullptr, &mods[0]));
  mci.codeSize = sizeof(kPresentPSSpv);
  mci.pCode = kPresentPSSpv;
  VK_CHECK(vkCreateShaderModule(device_, &mci, nullptr, &mods[1]));
  VkPipelineShaderStageCreateInfo st[2] = {};
  st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  st[0].module = mods[0];
  st[0].pName = "vs_main";
  st[1] = st[0];
  st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  st[1].module = mods[1];
  st[1].pName = "ps_main";
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState att{};
  att.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &att;
  const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = 1;
  ri.pColorAttachmentFormats = &sc_format_;
  VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pci.pNext = &ri;
  pci.stageCount = 2;
  pci.pStages = st;
  pci.pVertexInputState = &vi;
  pci.pInputAssemblyState = &ia;
  pci.pViewportState = &vp;
  pci.pRasterizationState = &rs;
  pci.pMultisampleState = &ms;
  pci.pColorBlendState = &cb;
  pci.pDynamicState = &ds;
  pci.layout = present_layout_;
  const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci,
                                               nullptr, &present_pipeline_);
  vkDestroyShaderModule(device_, mods[0], nullptr);
  vkDestroyShaderModule(device_, mods[1], nullptr);
  if (r != VK_SUCCESS) {
    std::printf("[vk] present pipeline creation failed: %d\n", int(r));
    present_pipeline_ = VK_NULL_HANDLE;
    return false;
  }
  present_pipeline_format_ = sc_format_;
  return true;
}

// The one sampler every game texture is read through; recreated when the
// anisotropy setting changes.
bool Backend::CreateTextureSampler() {
  WaitFlight();  // the old sampler / the set may be in use by the frame in flight
  const float aniso = std::min(max_anisotropy_,
                               float(std::max(1u, options_.anisotropy)));
  // Sampler i = U mode (i / 3) x V mode (i % 3): repeat, mirror, clamp - the
  // fetch constant's clamp_x / clamp_y (as in Pacific Rim's renderer).
  static const VkSamplerAddressMode kModes[3] = {VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                 VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
                                                 VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
  for (uint32_t i = 0; i < 9; ++i) {
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    sci.addressModeU = kModes[i / 3];
    sci.addressModeV = kModes[i % 3];
    sci.addressModeW = kModes[i % 3];
    sci.anisotropyEnable = aniso > 1.0f ? VK_TRUE : VK_FALSE;
    sci.maxAnisotropy = aniso;
    VkSampler sampler = VK_NULL_HANDLE;
    VK_CHECK(vkCreateSampler(device_, &sci, nullptr, &sampler));
    VkDescriptorImageInfo sampler_info{};
    sampler_info.sampler = sampler;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = sets_[3];
    write.dstBinding = 0;
    write.dstArrayElement = i;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    write.pImageInfo = &sampler_info;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    VkSampler& slot = i == 0 ? sampler_ : extra_samplers_[i - 1];
    if (slot != VK_NULL_HANDLE) vkDestroySampler(device_, slot, nullptr);
    slot = sampler;
  }
  sampler_anisotropy_ = options_.anisotropy;
  return true;
}

bool Backend::EnsureDepthStage() {
  if (depth_stage_ != VK_NULL_HANDLE) return true;
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = VkDeviceSize(SW(kAttachW)) * (SH(kAttachH)) * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  if (vkCreateBuffer(device_, &bci, nullptr, &depth_stage_) != VK_SUCCESS) {
    depth_stage_ = VK_NULL_HANDLE;
    return false;
  }
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(device_, depth_stage_, &req);
  VkPhysicalDeviceMemoryProperties mem_props;
  vkGetPhysicalDeviceMemoryProperties(phys_, &mem_props);
  uint32_t type = UINT32_MAX;
  for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
    if ((req.memoryTypeBits & (1u << i)) &&
        (mem_props.memoryTypes[i].propertyFlags &
         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
      type = i;
      break;
    }
  }
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type;
  if (type == UINT32_MAX ||
      vkAllocateMemory(device_, &mai, nullptr, &depth_stage_mem_) != VK_SUCCESS ||
      vkBindBufferMemory(device_, depth_stage_, depth_stage_mem_, 0) != VK_SUCCESS) {
    DestroyDepthStage();
    return false;
  }
  return true;
}

void Backend::DestroyOverlayTexture(OverlayTexture& t) {
  if (t.set) vkFreeDescriptorSets(device_, overlay_pool_, 1, &t.set);
  if (t.view) vkDestroyImageView(device_, t.view, nullptr);
  if (t.image) vkDestroyImage(device_, t.image, nullptr);
  if (t.memory) vkFreeMemory(device_, t.memory, nullptr);
  t = OverlayTexture{};
}

// Frame start, command buffer recording, queue idle (so replaced textures can
// be destroyed on the spot). Needs present_dsl_ / present_sampler_.
void Backend::ProcessOverlayOps() {
  if (overlay_pool_ == VK_NULL_HANDLE) {
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 64},
                                  {VK_DESCRIPTOR_TYPE_SAMPLER, 64}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    dpi.maxSets = 64;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    if (vkCreateDescriptorPool(device_, &dpi, nullptr, &overlay_pool_) != VK_SUCCESS) {
      overlay_pool_ = VK_NULL_HANDLE;
      overlay_ops_.clear();
      return;
    }
  }
  for (const OverlayTextureOp& op : overlay_ops_) {
    auto it = overlay_textures_.find(op.id);
    if (it != overlay_textures_.end()) {
      DestroyOverlayTexture(it->second);
      overlay_textures_.erase(it);
    }
    if (op.rgba.empty() || op.width == 0 || op.height == 0 ||
        op.rgba.size() < size_t(op.width) * op.height * 4) {
      continue;
    }
    OverlayTexture t;
    if (!AllocImage(device_, phys_, op.width, op.height, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    &t.image, &t.memory)) {
      continue;
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.image = t.image;
    vci.format = VK_FORMAT_R8G8B8A8_UNORM;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = overlay_pool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &present_dsl_;
    const size_t bytes = size_t(op.width) * op.height * 4;
    const VkDeviceSize at = Alloc(bytes, 16);
    if (at == VkDeviceSize(-1) ||
        vkCreateImageView(device_, &vci, nullptr, &t.view) != VK_SUCCESS ||
        vkAllocateDescriptorSets(device_, &dai, &t.set) != VK_SUCCESS) {
      t.set = VK_NULL_HANDLE;
      DestroyOverlayTexture(t);
      continue;
    }
    std::memcpy(arena_ptr_ + at, op.rgba.data(), bytes);
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.image = t.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cmd_, &dep);
    VkBufferImageCopy copy{};
    copy.bufferOffset = at;
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {op.width, op.height, 1};
    vkCmdCopyBufferToImage(cmd_, arena_, t.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier2(cmd_, &dep);
    VkDescriptorImageInfo img{};
    img.imageView = t.view;
    img.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo smp{};
    smp.sampler = present_sampler_;
    VkWriteDescriptorSet w[2] = {};
    w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w[0].dstSet = t.set;
    w[0].dstBinding = 0;
    w[0].descriptorCount = 1;
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w[0].pImageInfo = &img;
    w[1] = w[0];
    w[1].dstBinding = 1;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w[1].pImageInfo = &smp;
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    overlay_textures_[op.id] = t;
  }
  overlay_ops_.clear();
}

bool Backend::EnsureOverlayPipeline() {
  if (overlay_layout_ == VK_NULL_HANDLE) {
    VkPushConstantRange range{};
    range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    range.size = 32;  // transform, HDR mode + paper white
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &present_dsl_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    VK_CHECK(vkCreatePipelineLayout(device_, &pli, nullptr, &overlay_layout_));
  }
  if (overlay_pipeline_ != VK_NULL_HANDLE && overlay_pipeline_format_ == sc_format_) {
    return true;
  }
  if (overlay_pipeline_ != VK_NULL_HANDLE) {
    vkDestroyPipeline(device_, overlay_pipeline_, nullptr);
    overlay_pipeline_ = VK_NULL_HANDLE;
  }
  VkShaderModule mods[2] = {};
  VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  mci.codeSize = sizeof(kOverlayVSSpv);
  mci.pCode = kOverlayVSSpv;
  VK_CHECK(vkCreateShaderModule(device_, &mci, nullptr, &mods[0]));
  mci.codeSize = sizeof(kOverlayPSSpv);
  mci.pCode = kOverlayPSSpv;
  VK_CHECK(vkCreateShaderModule(device_, &mci, nullptr, &mods[1]));
  VkPipelineShaderStageCreateInfo st[2] = {};
  st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  st[0].module = mods[0];
  st[0].pName = "vs_main";
  st[1] = st[0];
  st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  st[1].module = mods[1];
  st[1].pName = "ps_main";
  VkVertexInputBindingDescription vb{0, sizeof(OverlayVertex), VK_VERTEX_INPUT_RATE_VERTEX};
  const VkVertexInputAttributeDescription va[3] = {
      {0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
      {1, 0, VK_FORMAT_R32G32_SFLOAT, 8},
      {2, 0, VK_FORMAT_R8G8B8A8_UNORM, 16}};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &vb;
  vi.vertexAttributeDescriptionCount = 3;
  vi.pVertexAttributeDescriptions = va;
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState att{};
  att.blendEnable = VK_TRUE;
  att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  att.colorBlendOp = VK_BLEND_OP_ADD;
  att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  att.alphaBlendOp = VK_BLEND_OP_ADD;
  att.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &att;
  const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;
  VkPipelineRenderingCreateInfo ri{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  ri.colorAttachmentCount = 1;
  ri.pColorAttachmentFormats = &sc_format_;
  VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pci.pNext = &ri;
  pci.stageCount = 2;
  pci.pStages = st;
  pci.pVertexInputState = &vi;
  pci.pInputAssemblyState = &ia;
  pci.pViewportState = &vp;
  pci.pRasterizationState = &rs;
  pci.pMultisampleState = &ms;
  pci.pColorBlendState = &cb;
  pci.pDynamicState = &ds;
  pci.layout = overlay_layout_;
  const VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &pci,
                                               nullptr, &overlay_pipeline_);
  vkDestroyShaderModule(device_, mods[0], nullptr);
  vkDestroyShaderModule(device_, mods[1], nullptr);
  if (r != VK_SUCCESS) {
    std::printf("[vk] overlay pipeline creation failed: %d\n", int(r));
    overlay_pipeline_ = VK_NULL_HANDLE;
    return false;
  }
  overlay_pipeline_format_ = sc_format_;
  return true;
}

void Backend::DrawOverlay() {
  if (overlay_ == nullptr || overlay_->draws.empty() || overlay_->width <= 0.0f ||
      overlay_->height <= 0.0f || overlay_->vertices.empty() ||
      overlay_->indices.empty() || !EnsureOverlayPipeline()) {
    return;
  }
  const OverlayFrame& f = *overlay_;
  const size_t vb_bytes = f.vertices.size() * sizeof(OverlayVertex);
  const size_t ib_bytes = f.indices.size() * sizeof(uint16_t);
  const VkDeviceSize vb_at = Alloc(vb_bytes, 16);
  const VkDeviceSize ib_at = Alloc(ib_bytes, 16);
  if (vb_at == VkDeviceSize(-1) || ib_at == VkDeviceSize(-1)) return;
  std::memcpy(arena_ptr_ + vb_at, f.vertices.data(), vb_bytes);
  std::memcpy(arena_ptr_ + ib_at, f.indices.data(), ib_bytes);
  const float sw = float(sc_extent_.width), sh = float(sc_extent_.height);
  const VkViewport vp = MapViewport(0.0f, 0.0f, sw, sh);
  vkCmdSetViewport(cmd_, 0, 1, &vp);
  vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, overlay_pipeline_);
  const float xform[4] = {2.0f / f.width, 2.0f / f.height, -1.0f, -1.0f};
  struct {
    float xform[4];
    uint32_t mode;
    float paper;
    float rot[2];
  } overlay_pc{{xform[0], xform[1], xform[2], xform[3]}, hdr_mode_, options_.hdr_paper_nits, {1, 0}};
  QuarterTurn(overlay_pc.rot);
  vkCmdPushConstants(cmd_, overlay_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, 32, &overlay_pc);
  vkCmdBindVertexBuffers(cmd_, 0, 1, &arena_, &vb_at);
  vkCmdBindIndexBuffer(cmd_, arena_, ib_at, VK_INDEX_TYPE_UINT16);
  const float kx = sw / f.width, ky = sh / f.height;
  for (const OverlayDraw& d : f.draws) {
    const auto it = overlay_textures_.find(d.texture);
    if (it == overlay_textures_.end() || it->second.set == VK_NULL_HANDLE) continue;
    if (uint64_t(d.index_offset) + d.index_count > f.indices.size()) continue;
    const float l = std::max(0.0f, d.left * kx), t = std::max(0.0f, d.top * ky);
    const float r = std::min(sw, d.right * kx), b = std::min(sh, d.bottom * ky);
    if (r <= l || b <= t) continue;
    const VkRect2D sc = MapRect(float(int32_t(l)), float(int32_t(t)),
                                float(uint32_t(r) - uint32_t(l)), float(uint32_t(b) - uint32_t(t)));
    if (sc.extent.width == 0 || sc.extent.height == 0) continue;
    vkCmdSetScissor(cmd_, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, overlay_layout_,
                            0, 1, &it->second.set, 0, nullptr);
    vkCmdDrawIndexed(cmd_, d.index_count, 1, d.index_offset, d.base_vertex, 0);
  }
}

void Backend::DestroySwapchain() {
  for (VkImageView v : sc_views_) {
    if (v) vkDestroyImageView(device_, v, nullptr);
  }
  sc_views_.clear();
  if (swapchain_ != VK_NULL_HANDLE) {
    vkDestroySwapchainKHR(device_, swapchain_, nullptr);
    swapchain_ = VK_NULL_HANDLE;
  }
  sc_images_.clear();
}

static Backend* AsBackend(void* p) { return static_cast<Backend*>(p); }
Renderer::Renderer() : backend_(new Backend()) {}
Renderer::~Renderer() { delete AsBackend(backend_); }
bool Renderer::Init(const RenderOptions& options) { return AsBackend(backend_)->Init(options); }
bool Renderer::Render(const FrameData& frame, std::vector<uint8_t>& rgba,
                      bool want_rgba) {
  return AsBackend(backend_)->RenderFrame(frame, rgba, want_rgba);
}
bool Renderer::WindowOpen() const { return AsBackend(backend_)->WindowOpen(); }
void Renderer::SetDumpPassesDir(const std::string& dir) {
  AsBackend(backend_)->SetDumpPassesDir(dir);
}
void Renderer::SetLiveOptions(uint32_t internal_width, uint32_t internal_height,
                              bool present_stretch, float output_gamma,
                              bool no_dof) {
  AsBackend(backend_)->SetLiveOptions(internal_width, internal_height,
                                      present_stretch, output_gamma, no_dof);
}
void Renderer::SetVsync(bool vsync) { AsBackend(backend_)->SetVsync(vsync); }
void Renderer::SetPrerotate(bool on) { AsBackend(backend_)->SetPrerotate(on); }
void Renderer::ReloadTextures() { AsBackend(backend_)->ReloadTextures(); }
void Renderer::SetAmbientOcclusion(float strength) { AsBackend(backend_)->SetAmbientOcclusion(strength); }
void Renderer::SetCamera(float fov_y, float near_z, float far_z) {
  AsBackend(backend_)->SetCamera(fov_y, near_z, far_z);
}
void Renderer::SetLowEnd(bool crowd, bool half_textures) {
  AsBackend(backend_)->SetLowEnd(crowd, half_textures);
}
void Renderer::SetQuality(bool msaa, uint32_t anisotropy, float sharpen) {
  AsBackend(backend_)->SetQuality(msaa, anisotropy, sharpen);
}
void Renderer::SetFullCapture(bool on) { AsBackend(backend_)->SetFullCapture(on); }
void Renderer::SetPostFx(bool fxaa, bool hdr, float peak_nits, float paper_nits) {
  AsBackend(backend_)->SetPostFx(fxaa, hdr, peak_nits, paper_nits);
}
bool Renderer::HdrActive() const { return AsBackend(backend_)->HdrActive(); }
void Renderer::ReplaceAndroidWindow(void* native_window) {
  AsBackend(backend_)->ReplaceAndroidWindow(native_window);
}
bool Renderer::SurfaceLost() const { return AsBackend(backend_)->SurfaceLost(); }
void Renderer::SetPictureAspect(float aspect) {
  AsBackend(backend_)->SetPictureAspect(aspect);
}
void Renderer::SetAspectMask2D(bool mask) { AsBackend(backend_)->SetAspectMask2D(mask); }
float Renderer::WindowAspect() const { return AsBackend(backend_)->WindowAspect(); }
void Renderer::DrawListOnce() { AsBackend(backend_)->DrawListOnce(); }

void SetVulkanLoader(void* get_instance_proc_addr) {
  g_custom_loader = reinterpret_cast<PFN_vkGetInstanceProcAddr>(get_instance_proc_addr);
}

const std::string& DriverDescription() { return g_driver_text; }
const std::string& InitError() { return g_init_error; }

bool GpuMemoryUse(uint64_t* used, uint64_t* budget) {
  const VkPhysicalDevice phys = g_budget_phys.load();
  if (phys == VK_NULL_HANDLE || vkGetPhysicalDeviceMemoryProperties2 == nullptr) {
    *used = g_memory_total.load();  // the renderer's own allocations
    *budget = 0;
    return *used != 0;
  }
  VkPhysicalDeviceMemoryBudgetPropertiesEXT mb{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
  VkPhysicalDeviceMemoryProperties2 mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
  mp.pNext = &mb;
  vkGetPhysicalDeviceMemoryProperties2(phys, &mp);
  uint64_t u = 0, b = 0;
  for (uint32_t i = 0; i < mp.memoryProperties.memoryHeapCount; ++i) {
    if ((mp.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0) continue;
    u += mb.heapUsage[i];
    b += mb.heapBudget[i];
  }
  *used = u;
  *budget = b;
  return b != 0;
}
void Renderer::ProfileGpu(uint32_t frames) { AsBackend(backend_)->ProfileGpu(frames); }
void Renderer::LastReadbackSize(uint32_t& width, uint32_t& height) const {
  AsBackend(backend_)->LastReadbackSize(width, height);
}
void Renderer::QueueOverlayTexture(OverlayTextureOp op) {
  AsBackend(backend_)->QueueOverlayTexture(std::move(op));
}
void Renderer::SetOverlay(std::shared_ptr<const OverlayFrame> frame) {
  AsBackend(backend_)->SetOverlay(std::move(frame));
}
bool Renderer::RenderToBmp(const FrameData& frame, const std::string& path) {
  std::vector<uint8_t> rgba;
  if (!AsBackend(backend_)->RenderFrame(frame, rgba)) return false;
  const bool ok = AsBackend(backend_)->WriteBmp(path, rgba);
  std::printf("[vk] frame %llu -> %s (%s)\n",
              static_cast<unsigned long long>(frame.index), path.c_str(),
              ok ? "ok" : "WRITE FAILED");
  return ok;
}

bool RenderFrameVulkan(const FrameData& frame, const RenderOptions& options) {
  Backend backend;
  return backend.Run(frame, options);
}

bool WriteRgbaPngFile(const std::string& path,
                      const std::vector<uint8_t>& rgba, uint32_t width,
                      uint32_t height) {
  return WriteRgbaPng(path, rgba, width, height);
}

}  // namespace rs::vk
