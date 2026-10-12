// realsteel - performance fixes for the runtime underneath the game
// (2026-10-11).
//
// 1. XMPGetPlaybackController. Every frame the game's logic thread asks the
//    Xbox music player who controls background music (XMsgInProcessCall,
//    app 0xFA, message 0x0007001B, wrapper sub_823349B0: client r3,
//    controller* r4, locked* r5). The runtime's handler, inherited from
//    Xenia, sleeps 10 ms whenever the call comes from a thread other than
//    the title's main thread (a workaround for a game that polls it in a
//    tight loop). Real Steel's logic thread is such a thread, so each frame
//    lost 10 ms there (about 15 ms on Windows' default timer): stack samples
//    put 61 % of that thread in the call, and the game could not run faster
//    than ~67 fps uncapped. The answer only changes when the game itself
//    sets the controller (message 0x0007001A, wrapper sub_82334908), so the
//    first answer is cached and every later query is answered from it; a
//    set clears the cache and the next query asks the runtime once more.
//
// 2. Timer resolution (Windows). Sleeps default to a 15.6 ms tick; the frame
//    limiter, the GPU service's vblank thread and the game's own waits all
//    sleep in small steps. 1 ms resolution is requested for the life of the
//    process, as games commonly do.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "generated/default_final/realsteel_init.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

REXCVAR_DEFINE_BOOL(realsteel_fast_xmp, true, "Real Steel",
                    "Answer the per-frame music-controller query from a cache "
                    "(the runtime's handler sleeps 10 ms per call)");

namespace {

std::mutex g_xmp_mutex;
bool g_xmp_cached = false;
uint32_t g_xmp_controller = 0;
uint32_t g_xmp_locked = 0;
std::atomic<uint32_t> g_xmp_runtime_calls{0};
std::atomic<uint32_t> g_xmp_cached_calls{0};

#ifdef _WIN32
struct TimerResolution {
  TimerResolution() { timeBeginPeriod(1); }
  ~TimerResolution() { timeEndPeriod(1); }
};
TimerResolution g_timer_resolution;
#endif

}  // namespace

// XMPGetPlaybackController(client r3, controller* r4, locked* r5).
extern "C" REX_FUNC(sub_823349B0) {
  const uint32_t controller_ptr = ctx.r4.u32;
  const uint32_t locked_ptr = ctx.r5.u32;
  if (!REXCVAR_GET(realsteel_fast_xmp) || controller_ptr == 0 || locked_ptr == 0) {
    __imp__sub_823349B0(ctx, base);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_xmp_mutex);
    if (g_xmp_cached) {
      REX_STORE_U32(controller_ptr, g_xmp_controller);
      REX_STORE_U32(locked_ptr, g_xmp_locked);
      ctx.r3.u64 = 0;
      const uint32_t n = g_xmp_cached_calls.fetch_add(1);
      if (n == 0 || n == 100000) {
        REXLOG_INFO("[perf] music controller answered from the cache ({} so far, {} asked the runtime)",
                    n + 1, g_xmp_runtime_calls.load());
      }
      return;
    }
  }
  __imp__sub_823349B0(ctx, base);
  g_xmp_runtime_calls.fetch_add(1);
  if (ctx.r3.u32 != 0) return;  // failed: ask again next time
  std::lock_guard<std::mutex> lock(g_xmp_mutex);
  g_xmp_controller = REX_LOAD_U32(controller_ptr);
  g_xmp_locked = REX_LOAD_U32(locked_ptr);
  g_xmp_cached = true;
  REXLOG_INFO("[perf] music controller {} (locked {}) cached", g_xmp_controller, g_xmp_locked);
}

// XMPSetPlaybackController: the cached answer is stale from here on.
extern "C" REX_FUNC(sub_82334908) {
  {
    std::lock_guard<std::mutex> lock(g_xmp_mutex);
    g_xmp_cached = false;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 8) REXLOG_INFO("[perf] music controller set by the game");
  __imp__sub_82334908(ctx, base);
}

