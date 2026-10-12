// Locates the vertex-fetch instructions of an Xbox 360 shader container and
// derives the "template key" that pairs a runtime-patched vertex shader with
// its on-disk form (2026-10-02).
//
// The game's D3D rewrites a vertex shader's vfetch block for every vertex
// declaration it is bound with (format, stride, offset, constant lanes and
// format-driven swizzles; with a declaration lacking an element the block is
// even reordered). Everything else — metadata and the remaining microcode —
// is identical to the container in pac/_startup.pac. Zeroing the fetch
// instructions and appending their sorted (destination register, keep-lane
// mask) pairs therefore gives the same bytes for both, and the XXH3 of that
// is the key.
//
// Header-only; shared by the replayer/game (ShaderLibrary) and the
// XenosRecomp fork (cache generation). Needs xxhash.h on the include path.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include <xxhash.h>

namespace rs::vfetch {

struct ScannedFetch {
  uint32_t byte_offset;  // of the 12-byte instruction inside the container
  uint32_t address;      // instruction index (the vertex-element table's key)
  uint32_t d0, d1, d2;   // host-order dwords

  uint32_t dst() const { return (d0 >> 12) & 0x3F; }
  uint32_t swizzle() const { return d1 & 0xFFF; }
  uint32_t format() const { return (d1 >> 16) & 0x3F; }
  bool mini() const { return ((d1 >> 30) & 1) != 0; }
  uint32_t stride() const { return d2 & 0xFF; }
  int32_t offset() const { return int32_t(d2 << 1) >> 9; }  // 23-bit signed
  uint32_t flags() const {  // ShaderVertexFetch::flags bits 1-3
    return (((d1 >> 14) & 1) ? 2u : 0u) | (((d1 >> 13) & 1) ? 4u : 0u) |
           (((d1 >> 12) & 1) ? 8u : 0u);
  }
  // Lanes the fetch leaves untouched (destination select 7), bit per lane.
  uint32_t keep_mask() const {
    uint32_t m = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (((swizzle() >> (3 * i)) & 7) == 7) m |= 1u << i;
    }
    return m;
  }
  // True when at least one lane reads vertex data (select 0-3).
  bool reads_data() const {
    for (uint32_t i = 0; i < 4; ++i) {
      if (((swizzle() >> (3 * i)) & 7) < 4) return true;
    }
    return false;
  }
  uint32_t match_key() const { return (dst() << 4) | keep_mask(); }
};

inline uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Returns false when |data| is not a well-formed container. |fetches| come
// out in execution (address) order of the clauses.
inline bool Scan(const uint8_t* data, size_t size, bool* is_vertex,
                 std::vector<ScannedFetch>& fetches) {
  fetches.clear();
  if (size < 36) return false;
  const uint32_t flags = Be32(data);
  const uint32_t virtual_size = Be32(data + 4);
  const uint32_t physical_size = Be32(data + 8);
  const uint32_t shader_off = Be32(data + 24);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u || Be32(data + 28) != 0 ||
      Be32(data + 32) != 0) {
    return false;
  }
  if (uint64_t(virtual_size) + physical_size > size ||
      uint64_t(shader_off) + 8 > virtual_size) {
    return false;
  }
  if (is_vertex != nullptr) *is_vertex = (flags & 1u) != 0;
  const uint32_t physical_offset = Be32(data + shader_off);
  const uint32_t code_size = Be32(data + shader_off + 4);
  const uint64_t code_off = uint64_t(virtual_size) + physical_offset;
  if (code_off + code_size > size) return false;

  struct Clause { uint32_t address, count, sequence; };
  std::vector<Clause> clauses;
  uint64_t instr_size = code_size;
  for (uint64_t addr = 0; addr + 12 <= instr_size; addr += 12) {
    const uint8_t* p = data + code_off + addr;
    const uint64_t dw0 = Be32(p), dw1 = Be32(p + 4), dw2 = Be32(p + 8);
    const uint64_t cf[2] = {
        dw0 | ((dw1 & 0xFFFF) << 32),
        ((dw1 >> 16) | ((dw2 & 0xFFFF) << 16)) | ((dw2 >> 16) << 32)};
    for (uint64_t c : cf) {
      const uint32_t op = uint32_t((c >> 44) & 0xF);
      // Exec, ExecEnd, CondExec(End), CondExecPred(End),
      // CondExecPredClean(End).
      if ((op >= 1 && op <= 6) || op == 13 || op == 14) {
        const uint32_t a = uint32_t(c & 0xFFF);
        clauses.push_back({a, uint32_t((c >> 12) & 7), uint32_t((c >> 16) & 0xFFF)});
        if (a != 0) instr_size = std::min<uint64_t>(instr_size, uint64_t(a) * 12);
      }
    }
  }
  for (const Clause& cl : clauses) {
    for (uint32_t i = 0; i < cl.count; ++i) {
      if (((cl.sequence >> (2 * i)) & 1) == 0) continue;  // ALU instruction
      const uint64_t off = code_off + uint64_t(cl.address + i) * 12;
      if (off + 12 > size) return false;
      const uint32_t d0 = Be32(data + off);
      if ((d0 & 0x1F) != 0) continue;  // not a vertex fetch
      fetches.push_back({uint32_t(off), cl.address + i, d0, Be32(data + off + 4),
                         Be32(data + off + 8)});
    }
  }
  return true;
}

// On-disk vertex shaders carry blank fetches (format and stride zero).
inline bool IsUnpatched(const std::vector<ScannedFetch>& fetches) {
  if (fetches.empty()) return false;
  for (const ScannedFetch& f : fetches) {
    if (f.format() != 0 || f.stride() != 0) return false;
  }
  return true;
}

inline uint64_t TemplateKey(const uint8_t* data, size_t size,
                            const std::vector<ScannedFetch>& fetches) {
  std::vector<uint8_t> norm(data, data + size);
  std::vector<uint8_t> keys;
  for (const ScannedFetch& f : fetches) {
    std::memset(norm.data() + f.byte_offset, 0, 12);
    keys.push_back(uint8_t(f.match_key() >> 8));
    keys.push_back(uint8_t(f.match_key()));
  }
  // Sort the (dst, keep) pairs: the runtime may reorder the block.
  std::vector<uint16_t> sorted;
  for (size_t i = 0; i + 1 < keys.size(); i += 2) {
    sorted.push_back(uint16_t((keys[i] << 8) | keys[i + 1]));
  }
  std::sort(sorted.begin(), sorted.end());
  for (uint16_t k : sorted) {
    norm.push_back(uint8_t(k >> 8));
    norm.push_back(uint8_t(k));
  }
  return XXH3_64bits(norm.data(), norm.size());
}

}  // namespace rs::vfetch
