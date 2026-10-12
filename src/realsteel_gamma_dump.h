// Real Steel: capture the display gamma ramp the title builds.
//
// sub_82260D80 builds a 256-entry 16-bit ramp from VdGetCurrentDisplayGamma
// (type 1 sRGB, 2 Rec.709, 3 pow(1/level)) and a direction flag;
// sub_82260F08 / sub_82260FA0 compose it with the brightness LUT into the
// three per-channel tables D3D hands to the display. The xenos plugin applies
// that ramp in its presenter, so the offline replayer needs the same data.
// The hooks keep the latest tables; the frame capture writes them as a
// synthetic type-204 record (2026-09-06).
#pragma once

#include <cstdint>

namespace realsteel::gamma_dump {

constexpr uint32_t kDestBytes = 1536;  // [dest-512, dest+1024): three 256xu16 tables
constexpr uint32_t kTempBytes = 512;   // the builder's own 256xu16 output

struct Snapshot {
  uint32_t valid = 0;      // 0 = never captured
  uint32_t which = 0;      // 0 = sub_82260F08, 1 = sub_82260FA0
  uint32_t flag = 0;       // the r5 argument (direction flag to the builder)
  uint32_t dest_va = 0;
  uint32_t temp_va = 0;
  uint32_t type[2] = {};   // VdGetCurrentDisplayGamma type, as the title stored it
  uint32_t level_bits[2] = {};  // ... and level (float bits)
  uint8_t dest[kDestBytes] = {};
  uint8_t temp[kTempBytes] = {};
};

// Copies the latest snapshot; returns false when none was captured yet.
bool Latest(Snapshot* out);

}  // namespace realsteel::gamma_dump
