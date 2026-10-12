#include "realsteel_gamma_dump.h"

#include "generated/default_final/realsteel_init.h"

#include <cstring>
#include <mutex>

#include <rex/logging.h>

namespace realsteel::gamma_dump {
namespace {

std::mutex g_mutex;
Snapshot g_latest;

// The builder compares the kernel's (type, level) against these statics and
// refreshes them (sub_82260D80: lwzx off 0x82464860, lfsx off 0x824F1164,
// indexed by the direction flag).
constexpr uint32_t kTypeTable = 0x82464860u;
constexpr uint32_t kLevelTable = 0x824F1164u;

bool GuestRange(uint32_t va, uint32_t bytes) {
  return va >= 0x80000000u && va + bytes <= 0xC0000000u && va + bytes > va;
}

void Record(uint8_t* base, uint32_t which, uint32_t dest, uint32_t temp,
            uint32_t flag) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Snapshot& s = g_latest;
  s.valid = 1;
  s.which = which;
  s.flag = flag;
  s.dest_va = dest;
  s.temp_va = temp;
  for (uint32_t i = 0; i < 2; ++i) {
    s.type[i] = REX_LOAD_U32(kTypeTable + i * 4);
    s.level_bits[i] = REX_LOAD_U32(kLevelTable + i * 4);
  }
  if (GuestRange(dest - 512, kDestBytes)) {
    std::memcpy(s.dest, base + (dest - 512), kDestBytes);
  } else {
    std::memset(s.dest, 0, kDestBytes);
  }
  if (GuestRange(temp, kTempBytes)) {
    std::memcpy(s.temp, base + temp, kTempBytes);
  } else {
    std::memset(s.temp, 0, kTempBytes);
  }
  float level0, level1;
  std::memcpy(&level0, &s.level_bits[0], 4);
  std::memcpy(&level1, &s.level_bits[1], 4);
  REXLOG_INFO(
      "[gamma-dump] ramp built via {} dest={:#010x} temp={:#010x} flag={} "
      "display gamma type=({}, {}) level=({}, {}) dest[0..2]={:02x}{:02x} "
      "{:02x}{:02x} {:02x}{:02x} dest[510..511]={:02x}{:02x}",
      which == 0 ? "sub_82260F08" : "sub_82260FA0", dest, temp, flag,
      s.type[0], s.type[1], level0, level1, s.dest[512], s.dest[513],
      s.dest[514], s.dest[515], s.dest[516], s.dest[517], s.dest[1022],
      s.dest[1023]);
}

}  // namespace

bool Latest(Snapshot* out) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_latest.valid == 0) return false;
  *out = g_latest;
  return true;
}

}  // namespace realsteel::gamma_dump

extern "C" REX_FUNC(sub_82260F08) {
  const uint32_t dest = ctx.r3.u32;
  const uint32_t temp = ctx.r4.u32;
  const uint32_t flag = ctx.r5.u32;
  __imp__sub_82260F08(ctx, base);
  realsteel::gamma_dump::Record(base, 0, dest, temp, flag);
}

extern "C" REX_FUNC(sub_82260FA0) {
  const uint32_t dest = ctx.r3.u32;
  const uint32_t temp = ctx.r4.u32;
  const uint32_t flag = ctx.r5.u32;
  __imp__sub_82260FA0(ctx, base);
  realsteel::gamma_dump::Record(base, 1, dest, temp, flag);
}
