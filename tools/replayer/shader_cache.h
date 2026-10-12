// Layout of the XenosRecomp-generated shader cache (rs_shaders/shader_cache.cpp).
// The .cpp is emitted by our patched XenosRecomp (cache mode) and compiled
// into rs_replay; this header must match its emission code in
// xenosrecompgit/XenosRecomp/main.cpp.

#pragma once

#include <cstddef>
#include <cstdint>

struct ShaderCacheEntry {
  uint64_t hash;         // XXH3_64bits of the container bytes
  uint32_t dxilOffset;   // into the decompressed DXIL blob
  uint32_t dxilSize;
  uint32_t spirvOffset;  // into the decompressed SPIR-V blob (SMOL-V encoded)
  uint32_t spirvSize;
  uint32_t specConstantsMask;
};

// Real Steel extension: per-vertex-fetch info so the native renderer can
// build vertex input state. Flat array, rows carry the owning shader's hash.
struct ShaderVertexFetch {
  uint64_t hash;
  uint32_t usage;       // DeclUsage (D3D): 0=Position 1=BlendWeight
                        // 2=BlendIndices 3=Normal 5=TexCoord 6=Tangent
                        // 7=Binormal 10=Color; 0xFF = unknown
  uint32_t usageIndex;
  uint32_t offset;      // dwords into the stream
  uint32_t stride;      // dwords
  uint32_t format;      // Xenos vertex format (xenos VertexFormat enum)
  uint32_t flags;       // bit0 mini-fetch, bit1 signed, bit2 integer,
                        // bit3 formatCompAll
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;
extern ShaderVertexFetch g_shaderVertexFetches[];
extern const size_t g_shaderVertexFetchCount;
extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;
extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
