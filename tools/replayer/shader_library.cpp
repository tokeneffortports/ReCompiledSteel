#include "shader_library.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <zstd.h>

#include <smolv.h>

#include "xenos_vfetch_scan.h"

namespace rs::vk {
namespace {

// Template keys of the two pass-through post-process vertex shaders
// (archive vs_1975D0381EBB3666 / vs_ABF37D364027A0C2 and vs_264650574755E987;
// printed by rs_replay --templatekey).
constexpr uint64_t kPassthroughTemplateA = 0x93E84F2CFDE0DDFCull;
constexpr uint64_t kPassthroughTemplateB = 0x6DF3D8F10A2E3F39ull;

bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return false;
  const std::streamsize size = f.tellg();
  if (size <= 0) return false;
  out.resize(static_cast<size_t>(size));
  f.seekg(0);
  f.read(reinterpret_cast<char*>(out.data()), size);
  return f.good();
}

}  // namespace

namespace {
// Shader input locations by D3D usage (must match the backend's LocationFor
// and the recompiler's usage -> location table).
int TemplateLocation(uint32_t usage, uint32_t usage_index) {
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
}  // namespace

bool ShaderLibrary::PrintTemplateKey(const std::string& path) {
  std::vector<uint8_t> bytes;
  if (!ReadFileBytes(path, bytes)) return false;
  bool is_vs = false;
  std::vector<rs::vfetch::ScannedFetch> fetches;
  if (!rs::vfetch::Scan(bytes.data(), bytes.size(), &is_vs, fetches)) return false;
  std::printf("%s: %s, %zu fetches, %s, template key %016llX, xxh3 %016llX\n",
              path.c_str(), is_vs ? "vertex" : "pixel", fetches.size(),
              rs::vfetch::IsUnpatched(fetches) ? "on-disk form" : "runtime-patched",
              static_cast<unsigned long long>(
                  rs::vfetch::TemplateKey(bytes.data(), bytes.size(), fetches)),
              static_cast<unsigned long long>(XXH3_64bits(bytes.data(), bytes.size())));
  return true;
}

// The highest SPIR-V version among the shaders decoded so far, logged when
// it rises (a Vulkan 1.1 device accepts up to SPIR-V 1.3).
static void NoteSpirvVersion(const std::vector<uint32_t>& spirv) {
  static uint32_t highest = 0;
  if (spirv.size() < 2 || spirv[1] <= highest) return;
  highest = spirv[1];
  std::printf("[vk] shaders use SPIR-V %u.%u\n", (highest >> 16) & 0xff, (highest >> 8) & 0xff);
}

bool ShaderLibrary::DecodeEntry(size_t entry_index, ShaderInfo& info,
                                const char** why) {
  const ShaderCacheEntry& entry = g_shaderCacheEntries[entry_index];
  if (entry.spirvOffset + entry.spirvSize > spirv_blob_.size()) {
    *why = "cache entry out of range";
    return false;
  }
  info.spec_constants_mask = entry.specConstantsMask;
  const uint8_t* smol = spirv_blob_.data() + entry.spirvOffset;
  const size_t spirv_bytes = smolv::GetDecodedBufferSize(smol, entry.spirvSize);
  if (spirv_bytes == 0 || (spirv_bytes % 4) != 0) {
    *why = "smol-v header invalid";
    return false;
  }
  info.spirv.resize(spirv_bytes / 4);
  if (!smolv::Decode(smol, entry.spirvSize, info.spirv.data(), spirv_bytes)) {
    *why = "smol-v decode failed";
    return false;
  }
  NoteSpirvVersion(info.spirv);
  return true;
}

// Pairs a runtime-patched vertex shader container with the cache entry of
// its on-disk form and derives the vertex inputs from the live vfetch block.
bool ShaderLibrary::BuildFromTemplate(const std::vector<uint8_t>& bytes,
                                      uint64_t patched_hash, ShaderInfo& info,
                                      const char** why) {
  bool is_vs = false;
  std::vector<rs::vfetch::ScannedFetch> live;
  if (!rs::vfetch::Scan(bytes.data(), bytes.size(), &is_vs, live) || !is_vs ||
      live.empty()) {
    *why = "container not in shader cache (not a vertex shader with fetches)";
    return false;
  }
  const uint64_t key = rs::vfetch::TemplateKey(bytes.data(), bytes.size(), live);
  const auto entry_it = entries_.find(key);
  if (entry_it == entries_.end()) {
    *why = "container not in shader cache (no on-disk template either)";
    return false;
  }
  if (!DecodeEntry(entry_it->second, info, why)) return false;

  struct TemplateFetch {
    uint32_t usage, usage_index, ordinal, match_key;
    bool matched = false, has_data = false;
  };
  std::vector<TemplateFetch> tpl;
  for (size_t i = 0; i < g_shaderVertexFetchCount; ++i) {
    const ShaderVertexFetch& f = g_shaderVertexFetches[i];
    if (f.hash != key || (f.flags & 0x80000000u) == 0) continue;
    TemplateFetch t;
    t.usage = f.usage;
    t.usage_index = f.usageIndex;
    t.ordinal = (f.flags >> 16) & 0xF;
    t.match_key = (((f.flags >> 8) & 0x3F) << 4) | ((f.flags >> 4) & 0xF);
    tpl.push_back(t);
  }
  if (tpl.empty()) {
    *why = "cache entry is not a vertex template";
    return false;
  }
  for (uint32_t& s : info.fetch_swizzles) s = 0xFFF;  // keep every lane
  uint32_t last_stride = 0;
  bool full_pushed = false;  // a non-mini row carries the stride for the backend
  for (const rs::vfetch::ScannedFetch& lf : live) {
    if (!lf.mini()) {
      last_stride = lf.stride();
      full_pushed = false;
    }
    TemplateFetch* match = nullptr;
    for (TemplateFetch& t : tpl) {
      if (!t.matched && t.match_key == lf.match_key()) {
        match = &t;
        break;
      }
    }
    if (match == nullptr) {
      *why = "live vfetch block does not match the on-disk template";
      return false;
    }
    match->matched = true;
    info.fetch_swizzles[match->ordinal] = lf.swizzle();
    if (!lf.reads_data()) continue;  // constant lanes only: element absent
    match->has_data = true;
    ShaderVertexFetch f{};
    f.hash = patched_hash;
    f.usage = match->usage;
    f.usageIndex = match->usage_index;
    f.offset = uint32_t(lf.offset());
    f.stride = lf.mini() ? last_stride : lf.stride();
    f.format = lf.format();
    f.flags = (lf.mini() ? 1u : 0u) | lf.flags();
    if (!full_pushed) {
      // The block's full fetch may be a constant-only one (element absent in
      // the declaration) that produces no row: the first data fetch after it
      // stands in as the stride carrier.
      f.flags &= ~1u;
      full_pushed = true;
    }
    info.fetches.push_back(f);
  }
  for (const TemplateFetch& t : tpl) {
    if (t.has_data) continue;
    const int location = TemplateLocation(t.usage, t.usage_index);
    if (location < 0) continue;
    bool dup = false;
    for (const ShaderVertexFetch& f : info.fetches) {
      dup |= TemplateLocation(f.usage, f.usageIndex) == location;
    }
    for (const ShaderInfo::ZeroInput& z : info.zero_inputs) {
      dup |= int(z.location) == location;
    }
    if (dup) continue;
    // Integer-typed inputs in the translated shader: BlendIndices, Normal,
    // Tangent, Binormal (the recompiler's USAGE_TYPES).
    const bool integer = t.usage == 2 || t.usage == 3 || t.usage == 6 || t.usage == 7;
    info.zero_inputs.push_back({uint32_t(location), integer});
  }
  info.is_template = true;
  // Pass-through position VSs (blur/downsample quads, bloom composite).
  info.passthrough_position = key == kPassthroughTemplateA ||
                              key == kPassthroughTemplateB;
  return true;
}

bool ShaderLibrary::Init(const std::string& shaders_dir) {
  dir_ = shaders_dir;

  // Decompress the SMOL-V cache blob once.
  spirv_blob_.resize(g_spirvCacheDecompressedSize);
  const size_t got =
      ZSTD_decompress(spirv_blob_.data(), spirv_blob_.size(),
                      g_compressedSpirvCache, g_spirvCacheCompressedSize);
  if (ZSTD_isError(got) || got != g_spirvCacheDecompressedSize) {
    std::printf("[vk] shader cache decompress failed\n");
    return false;
  }
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
    entries_[g_shaderCacheEntries[i].hash] = i;
  }

  // Manifest: "<v|p> <va:%08X> <fnv:%016llX>" per line; later lines win so a
  // rerun's fresh mappings shadow stale ones. Optional: in-process the
  // game's own table (SetLiveLookup) replaces it.
  const std::string manifest_path = dir_ + "/containers/manifest.txt";
  std::ifstream manifest(manifest_path);
  size_t lines = 0;
  if (!manifest) {
    std::printf("[vk] no %s (not needed in-process)\n", manifest_path.c_str());
  } else {
    char type = 0;
    unsigned va = 0;
    unsigned long long fnv = 0;
    std::string line;
    while (std::getline(manifest, line)) {
      if (std::sscanf(line.c_str(), " %c %8X %16llX", &type, &va, &fnv) == 3) {
        manifest_[va] = fnv;
        manifest_is_vs_[va] = type == 'v';
        ++lines;
      }
    }
  }
  std::printf("[vk] shader library: %zu cache entries, %zu manifest lines\n",
              g_shaderCacheEntryCount, lines);
  return true;
}

bool ShaderLibrary::LoadContainer(const std::string& name, uint64_t fnv,
                                  uint32_t stride_bytes, std::vector<uint8_t>& bytes) {
  if (stride_bytes == 0 && live_container_ != nullptr && live_container_(fnv, &bytes)) {
    return true;
  }
  if (ReadFileBytes(dir_ + name, bytes)) return true;
  if (stride_bytes == 0) return false;
  // No pre-made variant file: re-patch the base container's fetch block.
  char base_name[80];
  std::snprintf(base_name, sizeof(base_name), "/containers/vs_%016llX.xpu",
                static_cast<unsigned long long>(fnv));
  std::vector<uint8_t> base;
  return LoadContainer(base_name, fnv, 0, base) &&
         SynthesizeStrideVariant(base, stride_bytes, bytes);
}

// make_stride_variants.py in C++: the skinning vertex shaders are dumped as
// patched for the stride-32 (rigid) declaration, whose BlendWeight element is
// absent - the runtime turned the weight fetch into a constant-lane fetch
// (three lanes select 0/1, stride 8, offset 0). The stride-44 (two-bone,
// crowd) declaration has float3 weights at dword 4, indices at 7, normal at
// 8, colour at 9, UV at 10. The compiled program reads the weights
// reversed: the three constant lanes, in ascending order, receive Z, Y, X
// (verified against the bone math of C9F870BF and 8793597E).
bool ShaderLibrary::SynthesizeStrideVariant(const std::vector<uint8_t>& base,
                                            uint32_t stride_bytes,
                                            std::vector<uint8_t>& out) {
  if (stride_bytes != 44) return false;
  bool is_vertex = false;
  std::vector<rs::vfetch::ScannedFetch> fetches;
  if (!rs::vfetch::Scan(base.data(), base.size(), &is_vertex, fetches) || !is_vertex ||
      fetches.empty()) {
    return false;
  }
  const rs::vfetch::ScannedFetch& w = fetches[0];
  const uint32_t raw_offset = (w.d2 >> 8) & 0x7FFFFF;
  if (w.mini() || w.stride() != 8 || raw_offset != 0) return false;
  uint32_t constant_lanes = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t lane = (w.swizzle() >> (3 * c)) & 7;
    if (lane == 4 || lane == 5) {
      ++constant_lanes;
    } else if (lane != 7) {
      return false;
    }
  }
  if (constant_lanes != 3) return false;

  auto set_bits = [](uint32_t v, uint32_t lo, uint32_t n, uint32_t x) {
    const uint32_t mask = ((n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1u)) << lo);
    return (v & ~mask) | ((x << lo) & mask);
  };
  auto put_be32 = [](uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
  };
  out = base;
  // Weight fetch: lanes read Z, Y, X of the stream's float3 at dword 4.
  uint32_t swizzle = 0;
  uint32_t sources[3] = {2, 1, 0};
  uint32_t next = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t lane = (w.swizzle() >> (3 * c)) & 7;
    const uint32_t sel = (lane == 4 || lane == 5) ? sources[next++] : 7;
    swizzle |= sel << (3 * c);
  }
  uint32_t d1 = set_bits(w.d1, 0, 12, swizzle);
  d1 = set_bits(d1, 12, 1, 1);   // formatCompAll
  d1 = set_bits(d1, 13, 1, 1);   // numFormatAll
  d1 = set_bits(d1, 16, 6, 57);  // k_32_32_32_FLOAT
  const uint32_t d2 = set_bits(set_bits(w.d2, 0, 8, 11), 8, 23, 4);
  put_be32(out.data() + w.byte_offset + 4, d1);
  put_be32(out.data() + w.byte_offset + 8, d2);
  // The mini-fetches that follow move with the layout.
  for (size_t i = 1; i < fetches.size(); ++i) {
    const rs::vfetch::ScannedFetch& f = fetches[i];
    const uint32_t off = (f.d2 >> 8) & 0x7FFFFF;
    uint32_t moved = 0;
    switch (off) {
      case 0: moved = 0; break;
      case 4: moved = 7; break;
      case 5: moved = 8; break;
      case 6: moved = 9; break;
      case 7: moved = 10; break;
      default: return false;
    }
    if (!f.mini()) return false;
    put_be32(out.data() + f.byte_offset + 8, set_bits(set_bits(f.d2, 0, 8, 11), 8, 23, moved));
  }
  return true;
}

const ShaderInfo* ShaderLibrary::FindByVa(uint32_t va) {
  return Resolve(va, 0);
}

const ShaderInfo* ShaderLibrary::FindVariantByVa(uint32_t va,
                                                 uint32_t stride_bytes) {
  if (stride_bytes == 0) return nullptr;
  return Resolve(va, stride_bytes);
}

const ShaderInfo* ShaderLibrary::FindByContainer(uint64_t fnv, bool is_vs) {
  if (fnv == 0) return nullptr;
  return Resolve(0, 0, fnv, is_vs);
}

const ShaderInfo* ShaderLibrary::Resolve(uint32_t va, uint32_t stride_bytes,
                                         uint64_t forced_fnv, bool forced_is_vs) {
  if (va == 0 && forced_fnv == 0) return nullptr;
  // Address -> container. In-process sessions: the game's own table is the
  // only truth — microcode addresses are heap allocations, and the manifest
  // file (read at Init, every earlier session's lines) maps reused addresses
  // to other sessions' shaders (2026-09-15: every fight draw resolved to a
  // stale VS through the file-first lookup and drew nothing).
  uint64_t fnv = 0;
  bool is_vs = false;
  bool known = false;
  if (forced_fnv != 0) {
    fnv = forced_fnv;
    is_vs = forced_is_vs;
    known = true;
  } else if (live_lookup_ != nullptr) {
    if (!live_lookup_(va, &fnv, &is_vs)) {
      return nullptr;  // not loaded yet; retry next time (no negative cache)
    }
    known = true;
  } else {
    const auto m = manifest_.find(va);
    if (m != manifest_.end()) {
      fnv = m->second;
      is_vs = manifest_is_vs_[va];
      known = true;
    }
  }
  // Decoded shaders are cached by content (+ stride variant), so a reused
  // address always lands on the shader currently loaded there.
  const uint64_t key =
      known ? (fnv ^ (uint64_t(stride_bytes) * 0x9E3779B97F4A7C15ull))
            : (uint64_t(va) | (uint64_t(stride_bytes) << 32));
  auto cached = by_key_.find(key);
  if (cached != by_key_.end()) {
    return cached->second.spirv.empty() ? nullptr : &cached->second;
  }

  // Variant misses are expected for every VS outside the skinning family,
  // so only base lookups are reported.
  auto report_missing = [&](const char* why) -> const ShaderInfo* {
    const bool expected_miss =
        stride_bytes != 0 && std::strcmp(why, "container file missing") == 0;
    if (!expected_miss && missing_reported_.emplace(key, true).second) {
      std::printf("[vk] shader va=%08X unresolved: %s\n", va, why);
    }
    by_key_.emplace(key, ShaderInfo{});  // negative-cache
    return nullptr;
  };

  if (!known) {
    return report_missing("not in manifest (stale capture or old game build)");
  }
  if (stride_bytes != 0 && !is_vs) return report_missing("not a VS");

  char name[80];
  if (stride_bytes == 0) {
    std::snprintf(name, sizeof(name), "/containers/%cs_%016llX.xpu",
                  is_vs ? 'v' : 'p', static_cast<unsigned long long>(fnv));
  } else {
    std::snprintf(name, sizeof(name), "/containers/vs_%016llX_s%u.xpu",
                  static_cast<unsigned long long>(fnv), stride_bytes);
  }
  uint64_t cache_hash = 0;
  std::vector<uint8_t> bytes;  // the container, when loaded by this call
  const auto known_file = file_to_cache_.find(name);
  if (known_file != file_to_cache_.end()) {
    cache_hash = known_file->second;
  } else {
    if (!LoadContainer(name, fnv, stride_bytes, bytes)) {
      return report_missing("container file missing");
    }
    cache_hash = XXH3_64bits(bytes.data(), bytes.size());
    file_to_cache_[name] = cache_hash;
  }

  const auto entry_it = entries_.find(cache_hash);
  if (is_vs && (entry_it == entries_.end() || prefer_templates_)) {
    // 2026-10-02: no translation of this exact runtime-patched container —
    // use the on-disk template of the shader with the live fetch block
    // (2026-10-10: stride variants come through here as well).
    ShaderInfo tinfo;
    const char* why = "container file missing";
    if ((!bytes.empty() || LoadContainer(name, fnv, stride_bytes, bytes)) &&
        BuildFromTemplate(bytes, cache_hash, tinfo, &why)) {
      tinfo.cache_hash = cache_hash;
      tinfo.container_fnv = fnv;
      tinfo.is_vertex = true;
      auto inserted = by_key_.emplace(key, std::move(tinfo));
      return &inserted.first->second;
    }
    if (entry_it == entries_.end()) return report_missing(why);
  }
  if (entry_it == entries_.end()) {
    return report_missing("container not in shader cache (regenerate cache)");
  }
  const ShaderCacheEntry& entry = g_shaderCacheEntries[entry_it->second];
  if (entry.spirvOffset + entry.spirvSize > spirv_blob_.size()) {
    return report_missing("cache entry out of range");
  }

  ShaderInfo info;
  info.cache_hash = cache_hash;
  info.container_fnv = fnv;
  info.spec_constants_mask = entry.specConstantsMask;
  info.is_vertex = is_vs;
  // Real Steel's two pass-through post-process VSs (blur/downsample quads and
  // the bloom composite): oPos = iPosition, no PosScale.
  info.passthrough_position =
      is_vs && (fnv == 0x48B00D49116FA623ull || fnv == 0x91B0AEA83143E712ull);

  // SMOL-V -> SPIR-V.
  const uint8_t* smol = spirv_blob_.data() + entry.spirvOffset;
  const size_t spirv_bytes = smolv::GetDecodedBufferSize(smol, entry.spirvSize);
  if (spirv_bytes == 0 || (spirv_bytes % 4) != 0) {
    return report_missing("smol-v header invalid");
  }
  info.spirv.resize(spirv_bytes / 4);
  if (!smolv::Decode(smol, entry.spirvSize, info.spirv.data(), spirv_bytes)) {
    return report_missing("smol-v decode failed");
  }
  NoteSpirvVersion(info.spirv);

  for (size_t i = 0; i < g_shaderVertexFetchCount; ++i) {
    if (g_shaderVertexFetches[i].hash == cache_hash &&
        (g_shaderVertexFetches[i].flags & 0x80000000u) == 0) {
      info.fetches.push_back(g_shaderVertexFetches[i]);
    }
  }

  auto inserted = by_key_.emplace(key, std::move(info));
  return &inserted.first->second;
}

}  // namespace rs::vk
