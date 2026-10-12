// Maps captured shader microcode VAs to compiled SPIR-V from the
// XenosRecomp shader cache.
//
// Chain: draw.vs_object/ps_object (guest VA, recorded per draw)
//   -> rs_shaders/containers/manifest.txt  (VA -> container FNV hash,
//      written by src/realsteel_shader_dump.cpp)
//   -> rs_shaders/containers/{v,p}s_<FNV>.xpu (container bytes)
//   -> XXH3_64bits(file) == ShaderCacheEntry::hash (rs_shaders/shader_cache.cpp)
//   -> SMOL-V decode -> SPIR-V, plus the vertex-fetch table for input state.
//
// Stride variants: the 360 runtime patches a VS's vfetch block per bound
// vertex declaration, and the skinning family is drawn with two
// declarations (stride 32 rigid parts, stride 44 two-bone parts) through
// ONE patched program. FindVariantByVa re-patches the fetch block for the
// other stride itself (the C++ form of tools/shaders/
// make_stride_variants.py) and translates the result through the on-disk
// template; a pre-made vs_<FNV>_s<stride>.xpu file, when present, wins.

#pragma once

#include "shader_cache.h"

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace rs::vk {

struct ShaderInfo {
  uint64_t cache_hash = 0;
  uint64_t container_fnv = 0;                // rs_shaders/hlsl/<vs|ps>_<this>.hlsl
  std::vector<uint32_t> spirv;               // decoded, ready for Vulkan
  uint32_t spec_constants_mask = 0;
  std::vector<ShaderVertexFetch> fetches;    // vertex shaders only
  bool is_vertex = false;
  // VS emits the fetched position unchanged (post-process quads with pixel
  // coordinates). The title draws these with the viewport transform disabled
  // (D3DRS_VIEWPORTENABLE = FALSE inside its own D3D calls, invisible to the
  // command stream), so the backend maps clip space 1:1 to pixels for them.
  bool passthrough_position = false;
  // Template vertex shaders (2026-10-02; see xenos_vfetch_scan.h): the
  // SPIR-V was translated from the on-disk container and takes each fetch's
  // destination swizzle at run time; |fetches| come from the live, patched
  // vfetch block. |zero_inputs| are the shader inputs the bound declaration
  // does not provide (location, 1 = integer-typed input).
  bool is_template = false;
  uint32_t fetch_swizzles[16] = {};
  struct ZeroInput {
    uint32_t location;
    bool integer;
  };
  std::vector<ZeroInput> zero_inputs;
};

class ShaderLibrary {
 public:
  // Decompresses the SPIR-V cache and loads the manifest. Returns false with
  // a printed reason on failure.
  bool Init(const std::string& shaders_dir);

  // Resolves a captured microcode VA to a decoded shader, or nullptr.
  // Decoded shaders are cached; missing VAs are reported once each.
  const ShaderInfo* FindByVa(uint32_t va);

  // Resolves the re-patched variant of vertex shader |va| for a stream of
  // |stride_bytes| (containers/vs_<FNV>_s<stride_bytes>.xpu), or nullptr
  // (silently) when no such variant exists.
  const ShaderInfo* FindVariantByVa(uint32_t va, uint32_t stride_bytes);

  // v15 captures: the draw names its shaders by container FNV.
  const ShaderInfo* FindByContainer(uint64_t fnv, bool is_vs);

  // In-process path: the game's own (VA -> container) table replaces the
  // manifest file entirely (addresses are reused across sessions). A miss
  // is not cached, so a shader that loads later resolves on a later frame.
  using LiveLookup = bool (*)(uint32_t va, uint64_t* container_hash,
                              bool* is_vs);
  void SetLiveLookup(LiveLookup fn) { live_lookup_ = fn; }
  // In-process path: container bytes straight from the game's memory, so
  // nothing has to exist in rs_shaders/ at run time.
  using LiveContainer = bool (*)(uint64_t container_fnv, std::vector<uint8_t>* bytes);
  void SetLiveContainerSource(LiveContainer fn) { live_container_ = fn; }

  // Testing: resolve vertex shaders through the on-disk templates even when
  // the exact runtime-patched container is in the cache (--vktemplates).
  void SetPreferTemplates(bool on) { prefer_templates_ = on; }
  // Prints the template key of a container file (tooling).
  static bool PrintTemplateKey(const std::string& path);

 private:
  LiveLookup live_lookup_ = nullptr;
  LiveContainer live_container_ = nullptr;
  bool prefer_templates_ = false;
  // The container's bytes: from the game (live) or the file |name| in dir_;
  // for a stride variant without a file, re-patched from the base container.
  bool LoadContainer(const std::string& name, uint64_t fnv, uint32_t stride_bytes,
                     std::vector<uint8_t>& bytes);
  static bool SynthesizeStrideVariant(const std::vector<uint8_t>& base, uint32_t stride_bytes,
                                      std::vector<uint8_t>& out);
  bool BuildFromTemplate(const std::vector<uint8_t>& bytes, uint64_t patched_hash,
                         ShaderInfo& info, const char** why);
  bool DecodeEntry(size_t entry_index, ShaderInfo& info, const char** why);
  const ShaderInfo* Resolve(uint32_t va, uint32_t stride_bytes,
                            uint64_t forced_fnv = 0, bool forced_is_vs = false);

  std::string dir_;
  std::vector<uint8_t> spirv_blob_;                 // decompressed cache
  std::unordered_map<uint64_t, size_t> entries_;    // cache hash -> index
  std::unordered_map<uint32_t, uint64_t> manifest_; // va -> container FNV
  std::unordered_map<uint32_t, bool> manifest_is_vs_;
  std::unordered_map<std::string, uint64_t> file_to_cache_; // container file -> XXH3
  std::unordered_map<uint64_t, ShaderInfo> by_key_;  // va | stride<<32
  std::unordered_map<uint64_t, bool> missing_reported_;
};

}  // namespace rs::vk
