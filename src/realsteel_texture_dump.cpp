// realsteel - texture capture (capture format v3).
//
// The guest D3D device shadows Xenos texture fetch constants at
// device+1152+slot*24 (docs/d3d_surface.md). At draw time we read slots 0..19,
// report each slot's texture base address for the frame capture, and dump
// each unique texture once to rs_textures/ as:
//   RSTX header: u32 magic 'RSTX', u32 version=1, u32 fetch[6], u32 data_bytes
//   followed by raw guest texture bytes (tiled/endian exactly as in memory).
// Field layout per Xenia's xe_gpu_texture_fetch_t:
//   dword0: type:2 ... pitch:9@22 (pixels>>5), tiled:1@31
//   dword1: format:6@0, endian:2@6, base_address:20@12 (>>12)
//   dword2 (2D): width-1:13@0, height-1:13@13
//   dword5: dimension:2@9, mip_address:20@12
// Mip chains are not captured (base level only).

#include "realsteel_texture_dump.h"

#include "generated/default_final/realsteel_init.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_BOOL(realsteel_texture_dump, false, "Real Steel",
                    "Development: dump unique bound textures to rs_textures/ "
                    "during play (needed for F10 frame captures)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace realsteel::texture_dump {
namespace {

constexpr uint32_t kFetchShadowBase = 1152;
constexpr uint32_t kFetchStride = 24;
constexpr uint64_t kMaxTextureBytes = 16 * 1024 * 1024;
constexpr size_t kMaxDumps = 4096;

std::mutex g_mutex;
std::unordered_set<uint64_t> g_seen;          // fetch-constant keys (first bind)
std::unordered_set<uint64_t> g_seen_content;  // content hashes written (armed)
struct FrameHash {
  uint64_t frame;
  uint64_t hash;
};
std::unordered_map<uint32_t, FrameHash> g_frame_hash;  // phys -> this frame's hash
std::unordered_map<uint32_t, FrameHash> g_sample_hash; // live path: phys -> (frame+1, stamp)
std::atomic<bool> g_disk_armed{false};
struct FetchWords {
  uint32_t w[6];
};
std::unordered_map<uint32_t, FetchWords> g_fetch_by_phys;  // last bound fetch constant

uint64_t Fnv1a64(const uint8_t* data, size_t size) {
  uint64_t hash = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < size; ++i) {
    hash = (hash ^ data[i]) * 0x100000001B3ull;
  }
  return hash;
}

uint32_t PhysToGuestVa(uint32_t phys) {
  return 0xA0000000u | (phys & 0x1FFFFFFFu);
}

// Bits per pixel for the formats we expect; 0 = unknown (skip, log).
uint32_t FormatBitsPerPixel(uint32_t format) {
  switch (format) {
    case 2:   return 8;    // k_8
    case 3:   return 16;   // k_1_5_5_5
    case 4:   return 16;   // k_5_6_5
    case 5:   return 16;   // k_6_5_5
    case 6:   return 32;   // k_8_8_8_8
    case 7:   return 32;   // k_2_10_10_10
    case 10:  return 16;   // k_8_8
    case 15:  return 16;   // k_4_4_4_4
    case 18:  return 4;    // k_DXT1
    case 19:  return 8;    // k_DXT2_3
    case 20:  return 8;    // k_DXT4_5
    case 26:  return 32;   // k_24_8
    case 22:  return 32;   // k_24_8 (depth)
    case 23:  return 32;   // k_24_8_FLOAT (depth)
    case 36:  return 32;   // k_32_FLOAT (depth / shadow maps)
    case 50:  return 16;   // k_16_FLOAT
    case 54:  return 32;   // k_32_FLOAT
    default:  return 0;
  }
}

// Returns the content hash (0 when nothing was read). |armed|: hash the
// live content (cached per frame) and write it if that content is new;
// otherwise dump once per fetch constant as before.
// Bytes of guest memory a texture occupies (base level; cube = 6 faces at
// a 4 KB-aligned stride), 0 when unsupported. Tiled surfaces are laid out in
// 32x32 macro-tiles of *elements* — texels for linear formats, 4x4 blocks
// for DXT — so tiled DXT pads to 32 blocks (128 texels) per axis. Linear
// rows use the fetch pitch. Over-reading padding inside the physical mirror
// is safe; the clamp keeps the read inside the 512MB physical range.
uint64_t TextureDataBytes(const uint32_t fetch[6], uint32_t phys, bool log) {
  const uint32_t dimension = (fetch[5] >> 9) & 0x3;
  const uint32_t format = fetch[1] & 0x3F;
  const uint32_t tiled = fetch[0] >> 31;
  const uint32_t pitch_px = ((fetch[0] >> 22) & 0x1FF) << 5;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1;
  const uint32_t height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  // dimension: 0 = 1D, 1 = 2D, 2 = 3D, 3 = cube. 3D textures are skipped.
  const bool cube = dimension == 3;
  if (dimension != 1 && !cube) {
    if (log) {
      REXLOG_INFO("[texture-dump] skipping non-2D texture: dim={} fmt={} at {:#010x}",
                  dimension, format, phys);
    }
    return 0;
  }
  const uint32_t bpp = FormatBitsPerPixel(format);
  if (bpp == 0) {
    if (log) {
      REXLOG_INFO("[texture-dump] unknown format {} ({}x{}) at {:#010x} — skipped",
                  format, width, height, phys);
    }
    return 0;
  }
  const uint32_t row_px = pitch_px != 0 ? pitch_px : ((width + 31u) & ~31u);
  const uint32_t padded_h = (height + 31u) & ~31u;
  const bool block_compressed = format == 18 || format == 19 || format == 20;
  uint64_t data_bytes;
  if (tiled && block_compressed) {
    const uint32_t block_bytes = format == 18 ? 8u : 16u;
    const uint32_t pw_blocks =
        (((row_px > width ? row_px : width) + 3u) / 4u + 31u) & ~31u;
    const uint32_t ph_blocks = ((height + 3u) / 4u + 31u) & ~31u;
    data_bytes = uint64_t(pw_blocks) * ph_blocks * block_bytes;
  } else {
    data_bytes = uint64_t(row_px) * padded_h * bpp / 8;
  }
  if (cube) {
    data_bytes = ((data_bytes + 4095ull) & ~4095ull) * 6;
  }
  // Never read past the end of the 512MB physical space.
  const uint64_t phys_remaining = 0x20000000ull - (phys & 0x1FFFFFFFu);
  if (data_bytes > phys_remaining) {
    data_bytes = phys_remaining;
  }
  if (data_bytes == 0 || data_bytes > kMaxTextureBytes) {
    if (log) {
      REXLOG_INFO("[texture-dump] implausible size {}B at {:#010x} — skipped",
                  data_bytes, phys);
    }
    return 0;
  }
  return data_bytes;
}

// Live path (2026-09-15): the renderer keys uploads by (id, hash), and
// without the full dumper the hash was 0 — a texture streamed into a reused
// address kept its first content. Size plus 256 samples spread over the
// data is enough to notice a change and cheap enough for every bind.
uint64_t SampledHash(const uint8_t* base, const uint32_t fetch[6],
                     uint32_t phys) {
  const uint64_t bytes = TextureDataBytes(fetch, phys, /*log=*/false);
  if (bytes == 0) return 0;
  const uint8_t* p = base + PhysToGuestVa(phys);
  uint64_t h = 1469598103934665603ull ^ bytes;
  const uint64_t step = bytes < 256 * 8 ? 8 : bytes / 256;
  for (uint64_t off = 0; off + 8 <= bytes; off += step) {
    uint64_t v;
    std::memcpy(&v, p + off, 8);
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

uint64_t DumpTexture(uint8_t* base, const uint32_t fetch[6], uint32_t phys,
                     bool armed, uint64_t frame) {
  const uint64_t key = Fnv1a64(reinterpret_cast<const uint8_t*>(fetch), 24);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (armed) {
      const auto cached = g_frame_hash.find(phys);
      if (cached != g_frame_hash.end() && cached->second.frame == frame) {
        return cached->second.hash;
      }
    } else if (g_seen.size() >= kMaxDumps || !g_seen.insert(key).second) {
      return 0;
    }
  }

  const uint32_t dimension = (fetch[5] >> 9) & 0x3;
  const uint32_t format = fetch[1] & 0x3F;
  const uint32_t endian = (fetch[1] >> 6) & 0x3;
  const uint32_t tiled = fetch[0] >> 31;
  const uint32_t pitch_px = ((fetch[0] >> 22) & 0x1FF) << 5;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1;
  const uint32_t height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const bool cube = dimension == 3;
  const uint64_t data_bytes = TextureDataBytes(fetch, phys, /*log=*/true);
  if (data_bytes == 0) {
    return 0;
  }
  const uint32_t row_px = pitch_px != 0 ? pitch_px : ((width + 31u) & ~31u);
  const uint32_t va = PhysToGuestVa(phys);
  const uint8_t* bytes = base + va;
  const uint64_t content_hash = Fnv1a64(bytes, static_cast<size_t>(data_bytes));

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (armed) {
      g_frame_hash[phys] = {frame, content_hash};
    }
    if (!g_seen_content.insert(content_hash).second) {
      return content_hash;  // this exact content is already on disk
    }
  }
  std::error_code ec;
  std::filesystem::create_directories("rs_textures", ec);
  char name[112];
  std::snprintf(name, sizeof(name), "rs_textures/tex_%08X_%016llX.xtx", phys,
                static_cast<unsigned long long>(content_hash));
  std::ofstream file(name, std::ios::binary | std::ios::trunc);
  bool written = false;
  if (file) {
    const uint32_t magic = 0x52535458u;  // 'RSTX'
    const uint32_t version = 1;
    const uint32_t size32 = static_cast<uint32_t>(data_bytes);
    file.write(reinterpret_cast<const char*>(&magic), 4);
    file.write(reinterpret_cast<const char*>(&version), 4);
    file.write(reinterpret_cast<const char*>(fetch), 24);
    file.write(reinterpret_cast<const char*>(&size32), 4);
    file.write(reinterpret_cast<const char*>(bytes),
               static_cast<std::streamsize>(data_bytes));
    written = file.good();
  }

  REXLOG_INFO(
      "[texture-dump] fmt={} {}x{} dim={} pitch={} tiled={} endian={} "
      "phys={:#010x} bytes={} hash={:016X} written={}",
      format, width, height, dimension, row_px, tiled, endian, phys,
      data_bytes, content_hash, written);
  return content_hash;
}

}  // namespace

namespace {
// 2026-10-15: most of a draw's ~20 texture slots are what the previous draw
// bound. Each slot remembers its fetch words and results for the frame, so an
// unchanged slot costs six loads and a compare instead of two locked map
// lookups (0.8 ms of draw hooks a frame on a PC, several times that on a
// phone). The draw hooks run on the one guest render thread.
struct SlotMemo {
  uint64_t frame = ~0ull;
  bool armed = false;
  bool has_hash_out = false;
  uint32_t fetch[6] = {};
  uint32_t id = 0, fmt = 0;
  uint64_t hash = 0;
};
thread_local SlotMemo t_slot_memo[kSlotCount];
}  // namespace

void CaptureBoundTextures(uint8_t* base, uint32_t device,
                          uint32_t out_ids[kSlotCount],
                          uint32_t out_fmt[kSlotCount],
                          uint64_t out_hash[kSlotCount], bool armed,
                          uint64_t frame) {
  const bool dump = REXCVAR_GET(realsteel_texture_dump);
  const bool disk_armed = armed && g_disk_armed.load();
  for (uint32_t s = 0; s < kSlotCount; ++s) {
    out_ids[s] = 0;
    out_fmt[s] = 0;
    if (out_hash != nullptr) out_hash[s] = 0;
    if (device == 0) {
      continue;
    }
    const uint32_t addr = device + kFetchShadowBase + s * kFetchStride;
    uint32_t fetch[6];
    for (uint32_t i = 0; i < 6; ++i) {
      fetch[i] = REX_LOAD_U32(addr + i * 4);
    }
    SlotMemo& memo = t_slot_memo[s];
    const bool want_hash = out_hash != nullptr;
    if (!dump && memo.frame == frame && memo.armed == armed &&
        memo.has_hash_out == want_hash &&
        std::memcmp(memo.fetch, fetch, sizeof(fetch)) == 0) {
      out_ids[s] = memo.id;
      out_fmt[s] = memo.fmt;
      if (want_hash) out_hash[s] = memo.hash;
      continue;
    }
    memo.frame = frame;
    memo.armed = armed;
    memo.has_hash_out = want_hash;
    std::memcpy(memo.fetch, fetch, sizeof(fetch));
    memo.id = 0;
    memo.fmt = 0;
    memo.hash = 0;
    // FetchConstantType: 2 = texture, 0 = "invalid texture". Real Steel
    // binds its render-target textures (blur/bloom targets, 2026-09-05
    // capture: dw0=0x8a004800 dw1=0x112bd086) and some DXT1 textures with
    // type 0, like the titles behind xenia's gpu_allow_invalid_fetch_constants
    // — accept both when a base address is present; 1/3 are vertex fetches.
    const uint32_t type = fetch[0] & 0x3;
    if (type != 2 && !(type == 0 && ((fetch[1] >> 12) << 12) != 0)) {
      static std::atomic<uint32_t> odd_logs{0};
      if ((fetch[0] | fetch[1]) != 0 && odd_logs.fetch_add(1) < 16) {
        REXLOG_INFO("[texture-dump] slot {} fetch type {} dw0={:#010x} "
                    "dw1={:#010x} dw5={:#010x} — not a texture?",
                    s, fetch[0] & 0x3, fetch[0], fetch[1], fetch[5]);
      }
      continue;
    }
    const uint32_t phys = (fetch[1] >> 12) << 12;
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      FetchWords& fw = g_fetch_by_phys[phys];
      for (uint32_t i = 0; i < 6; ++i) fw.w[i] = fetch[i];
    }
    if (phys == 0) {
      continue;
    }
    out_ids[s] = phys;
    out_fmt[s] = (fetch[1] & 0x3Fu) | (((fetch[5] >> 9) & 0x3u) << 8) |
                 (((fetch[1] >> 6) & 0x3u) << 12);
    memo.id = out_ids[s];
    memo.fmt = out_fmt[s];
    if (dump) {
      const uint64_t h = DumpTexture(base, fetch, phys, disk_armed, frame);
      if (out_hash != nullptr && disk_armed) out_hash[s] = h;
    }
    if (out_hash != nullptr && armed && !disk_armed) {
      // Once per texture per frame: the stamp costs 256 strided reads, and
      // a fight binds ~20 slots x 500 draws (2026-09-15: 44 ms of draw
      // hooks per frame, the whole performance problem).
      std::lock_guard<std::mutex> lock(g_mutex);
      FrameHash& fh = g_sample_hash[phys];
      if (fh.frame != frame + 1) {
        fh.frame = frame + 1;  // +1: frame 0 must not match a fresh entry
        fh.hash = SampledHash(base, fetch, phys);
      }
      out_hash[s] = fh.hash;
    }
    if (want_hash) memo.hash = out_hash[s];
  }
}

void SetDiskCaptureArmed(bool armed) { g_disk_armed.store(armed); }

bool LookupFetch(uint32_t phys, uint32_t out_fetch[6]) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_fetch_by_phys.find(phys);
  if (it == g_fetch_by_phys.end()) return false;
  for (uint32_t i = 0; i < 6; ++i) out_fetch[i] = it->second.w[i];
  return true;
}

bool ReadTextureBytes(uint8_t* base, uint32_t phys, uint32_t out_fetch[6],
                      std::vector<uint8_t>& out) {
  if (!LookupFetch(phys, out_fetch)) return false;
  const uint64_t bytes = TextureDataBytes(out_fetch, phys, /*log=*/false);
  if (bytes == 0) return false;
  const uint32_t va = PhysToGuestVa(phys);
  out.resize(static_cast<size_t>(bytes));
  std::memcpy(out.data(), base + va, static_cast<size_t>(bytes));
  return true;
}

}  // namespace realsteel::texture_dump
