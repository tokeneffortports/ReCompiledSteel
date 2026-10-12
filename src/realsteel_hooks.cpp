// realsteel - mid-asm hook bodies.
//
// RealSteelSwapHook fires on the guest thread at the VdSwap call site (once per
// presented frame). It now measures the raw guest rate by default. The previous
// unconditional 60 Hz limiter was useful as an experiment, but it obscured
// renderer profiling and could only delay frames; it could never repair a
// gameplay frame that already took longer than 16.67 ms.

#include "realsteel_overlay.h"
#include "realsteel_native_present.h"
#include <cstring>
#include <chrono>
#include <cstdint>
#include <thread>
#include <algorithm>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <time.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

#include "generated/default_final/realsteel_init.h"

REXCVAR_DEFINE_DOUBLE(realsteel_guest_fps_cap, 60.0, "Real Steel",
                      "Guest frame cap (0 = uncapped: the game then runs "
                      "faster than normal speed; profiling uses 0)")
    .range(0.0, 240.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(realsteel_cap_outside_lock, true, "Real Steel",
                    "Wait for the frame cap with the render thread's device "
                    "lock released, so loading threads can use the wait "
                    "(robot select loads 2 s -> fraction; see sub_821A9BD0)");

namespace {
using clock = std::chrono::steady_clock;

clock::time_point g_next_swap;
bool g_primed = false;

// Diagnostics.
clock::time_point g_win_start;
uint64_t g_win_swaps = 0;
uint64_t g_total_swaps = 0;
// Frame pacing: swap-to-swap intervals and the time the guest thread spent
// working (not waiting in the limiter) in the current window.
clock::time_point g_last_swap;
clock::time_point g_frame_start;  // when the limiter released the last frame
std::vector<float> g_intervals_ms;
double g_busy_ms = 0.0;
double g_thread_cpu_start_ms = -1.0;

double ThreadCpuMs() {
#ifdef _WIN32
  FILETIME created, exited, kernel, user;
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return 0.0;
  const auto to100ns = [](const FILETIME& t) {
    return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  };
  return double(to100ns(kernel) + to100ns(user)) / 10000.0;
#else
  timespec ts{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return double(ts.tv_sec) * 1000.0 + double(ts.tv_nsec) / 1e6;
#endif
}
}  // namespace

namespace {
// Waits for |deadline|: one sleep for the bulk, a spin for the last 1 ms
// (2026-10-12, after the Geometry Wars 2 recomp's limiter). On Windows a
// high-resolution waitable timer (Windows 10 1803+) does the sleep in one
// go; the old loop slept in 1 ms steps and spun the last 2 ms. With a
// 0.5 ms spin the timer's late wakes spread intervals to 16.1-17.2 ms.
// Elsewhere sleep_until is precise enough (nanosleep).
void WaitUntil(clock::time_point deadline) {
  const auto spin = std::chrono::microseconds(1000);
  const auto wake = deadline - spin;
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
  thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  const auto remaining = wake - clock::now();
  if (remaining > clock::duration::zero()) {
    LARGE_INTEGER due;
    due.QuadPart = -std::max<long long>(
        1, std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count() / 100);
    if (timer != nullptr && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
    } else {
      // No high-resolution timer (older Windows): 1 ms sleeps, as before.
      while (clock::now() < wake) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }
#else
  if (wake > clock::now()) std::this_thread::sleep_until(wake);
#endif
  while (clock::now() < deadline) std::this_thread::yield();
}

// Set while the render loop's present step (sub_821A9BD0) runs: the frame
// cap's wait is then done after it returns, outside the device lock.
thread_local bool t_defer_pace = false;
thread_local bool t_pace_pending = false;
void SwapPace();
}  // namespace

void RealSteelSwapHook() {
  if (t_defer_pace) {
    t_pace_pending = true;
    return;
  }
  SwapPace();
}

// The render thread's loop (sub_821B2638) holds a lock (critical section at
// obj+168 plus the D3D device, taken by sub_821A9228, released by
// sub_821A9270) for its whole iteration and lets go only for a moment between
// drawing and the next present step. Loading threads share it: while a robot
// loads on the select screen, the loader works in short slices and yields
// (sub_821A9678), then waits for the render thread's next gap - a whole
// frame, most of it the frame cap's sleep inside VdSwap. New robots took about
// 2 s to appear (81 yields a second, 1 s of them blocked). The frame is
// already handed over when VdSwap runs, so the cap's wait can as well happen
// just after the present step, with the lock released: same pacing and
// latency, and the loader gets the wait.
extern "C" REX_FUNC(sub_821A9BD0) {
  const uint32_t obj = ctx.r3.u32;
  const bool from_loop = ctx.lr == 0x821B2948u;  // the render loop's call
  if (!from_loop || !REXCVAR_GET(realsteel_cap_outside_lock)) {
    __imp__sub_821A9BD0(ctx, base);
    return;
  }
  t_defer_pace = true;
  t_pace_pending = false;
  realsteel::native_present::HoldFrames(true);
  __imp__sub_821A9BD0(ctx, base);
  t_defer_pace = false;
  if (!t_pace_pending) {
    realsteel::native_present::HoldFrames(false);
    return;
  }
  t_pace_pending = false;
  const uint64_t saved_r3 = ctx.r3.u64, saved_lr = ctx.lr;
  ctx.r3.u64 = obj;
  sub_821A9270(ctx, base);  // release the device and the lock
  SwapPace();
  realsteel::native_present::HoldFrames(false);  // the frame goes now, on the beat
  ctx.r3.u64 = obj;
  sub_821A9228(ctx, base);  // take them back
  ctx.r3.u64 = saved_r3;
  ctx.lr = saved_lr;
}

namespace {
void SwapPace() {
  if (g_total_swaps < 4) {
    REXLOG_INFO("[tickrate] VdSwap call site reached (swap {})", g_total_swaps);
  }
  clock::time_point now = clock::now();
  const double cap = REXCVAR_GET(realsteel_guest_fps_cap);
  if (g_frame_start.time_since_epoch().count() != 0) {
    g_busy_ms += std::chrono::duration<double, std::milli>(now - g_frame_start).count();
  }
  if (g_thread_cpu_start_ms < 0.0) g_thread_cpu_start_ms = ThreadCpuMs();

  if (g_win_start.time_since_epoch().count() == 0) {
    g_win_start = now;
  }

  if (cap >= 1.0) {
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(1.0 / cap));
    const auto resync_slack = period * 2;
    if (!g_primed) {
      g_primed = true;
      g_next_swap = now + period;
    } else if (now > g_next_swap + resync_slack) {
      // Genuine stall — don't try to claw back the lost time in a burst.
      g_next_swap = now + period;
    } else {
      WaitUntil(g_next_swap);
      g_next_swap += period;
    }
  } else {
    // Re-prime if a cap is enabled later; no pacing at all in the default
    // profiling configuration.
    g_primed = false;
  }

  const clock::time_point released = clock::now();
  if (g_last_swap.time_since_epoch().count() != 0) {
    g_intervals_ms.push_back(
        float(std::chrono::duration<double, std::milli>(released - g_last_swap).count()));
  }
  g_last_swap = released;
  g_frame_start = released;
  ++g_win_swaps;
  ++g_total_swaps;
  double elapsed_s = std::chrono::duration<double>(released - g_win_start).count();
  if (elapsed_s >= 2.0) {
    REXLOG_INFO("[tickrate] guest swap rate: {:.2f} Hz over {:.1f}s "
                "(cap {:.1f}, total {})",
                double(g_win_swaps) / elapsed_s, elapsed_s, cap,
                g_total_swaps);
    if (!g_intervals_ms.empty()) {
      std::vector<float> v = g_intervals_ms;
      std::sort(v.begin(), v.end());
      const auto pct = [&](double q) { return v[std::min(v.size() - 1, size_t(q * double(v.size())))]; };
      size_t late = 0;
      const double period_ms = cap >= 1.0 ? 1000.0 / cap : 0.0;
      for (float x : v) {
        if (period_ms > 0.0 && x > period_ms * 1.25) ++late;
      }
      const double cpu_now = ThreadCpuMs();
      const double frames = double(g_win_swaps);
      REXLOG_INFO("[pacing] interval ms min {:.2f} p50 {:.2f} p99 {:.2f} max {:.2f}, {} late "
                  "of {} | guest work {:.2f} ms/frame (thread cpu {:.2f} ms/frame)",
                  v.front(), pct(0.5), pct(0.99), v.back(), late, v.size(),
                  g_busy_ms / frames, (cpu_now - g_thread_cpu_start_ms) / frames);
      g_thread_cpu_start_ms = cpu_now;
    }
    g_intervals_ms.clear();
    g_busy_ms = 0.0;
    g_win_start = released;
    g_win_swaps = 0;
  }
}
}  // namespace
