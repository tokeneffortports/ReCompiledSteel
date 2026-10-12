// realsteel - online play through the game's own Xbox Live code (2026-10-05).
//
// The game ships complete Live multiplayer: sessions through the XGI app
// (XMsgStartIORequest), UDP peer to peer through the NetDll socket calls,
// XNet address translation, QoS probes and voice. This file makes that code
// reachable and gives it something to talk to. Work in progress:
//   step 1 (here): the profile reads as signed in to Live with every
//   privilege, so "Xbox LIVE", "Leaderboards" and "Download content" stop
//   being greyed out on the main menu.
//
// Everything is behind --realsteel_online=true.

#include "realsteel_online.h"

#include "realsteel_netplay.h"

#include "generated/default_final/realsteel_init.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <cstdlib>
#include <map>
#include <mutex>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xevent.h>

REXCVAR_DEFINE_BOOL(realsteel_online, true, "Real Steel",
                    "Online play: the MULTIPLAYER menu (the game's Xbox Live features over our own network)");
REXCVAR_DEFINE_STRING(realsteel_online_xuid, "", "Real Steel",
                      "Online identity (16 hex digits). Empty: a random one is "
                      "made and kept in realsteel_settings.cfg. Two players must differ");

namespace realsteel::online {
bool Enabled() { return REXCVAR_GET(realsteel_online); }

// The game asks for the sign-in state every frame while it is in its
// online menus (and rarely elsewhere): how often it asked lately tells
// whether the player is in them.
namespace {
std::atomic<uint64_t> g_signin_polls{0};
std::atomic<uint64_t> g_calls[8];
}
void NoteSigninPoll() { g_signin_polls.fetch_add(1, std::memory_order_relaxed); }
void NoteCall(int which) { g_calls[which & 7].fetch_add(1, std::memory_order_relaxed); }
uint64_t Calls(int which) { return g_calls[which & 7].load(std::memory_order_relaxed); }
uint64_t SigninPolls() { return g_signin_polls.load(std::memory_order_relaxed); }
}  // namespace realsteel::online

namespace {

bool Online() { return realsteel::online::Enabled(); }

// The runtime gives every profile the same XUID; two players with the same
// one cannot meet. Online, the game sees a per-player one (Live-style, top
// word 0x0009).
uint64_t OnlineXuid() {
  static const uint64_t xuid = [] {
    const std::string text = REXCVAR_GET(realsteel_online_xuid);
    uint64_t value = text.empty() ? 0 : std::strtoull(text.c_str(), nullptr, 16);
    if (value == 0) {
      std::string seed;
      for (const char* name : {"COMPUTERNAME", "HOSTNAME", "USERNAME", "USER", "HOME"}) {
        if (const char* v = std::getenv(name)) seed += v;
      }
      value = 1469598103934665603ull;
      for (const char c : seed) value = (value ^ uint8_t(c)) * 1099511628211ull;
    }
    return (value & 0x0000FFFFFFFFFFFFull) | 0x0009000000000000ull;
  }();
  return xuid;
}

void StoreXuid(uint8_t* p) {
  const uint64_t xuid = OnlineXuid();
  for (int i = 0; i < 8; ++i) p[i] = uint8_t(xuid >> (56 - 8 * i));
}

// XSESSION_INFO of the session this game created (guest pointer): XNKID
// sessionID[8], XNADDR hostAddress[36], XNKEY keyExchangeKey[16].
std::atomic<uint32_t> g_created_session_info{0};

void LogOnce(std::atomic<uint32_t>& counter, uint32_t limit, const char* what,
             uint32_t a, uint32_t b, uint32_t result) {
  if (counter.fetch_add(1) < limit) {
    REXLOG_INFO("[online] {} ({:#x}, {:#x}) -> {:#x}", what, a, b, result);
  }
}

}  // namespace

namespace realsteel::online {
uint64_t Xuid() { return OnlineXuid(); }
}  // namespace realsteel::online

// XUserGetSigninState(user r3): 0 not signed in, 1 signed in locally, 2 signed
// in to Live. A local sign-in is reported as a Live one.
extern "C" REX_FUNC(sub_82237590) {
  realsteel::online::NoteCall(0);
  const uint32_t user = ctx.r3.u32;
  __imp__sub_82237590(ctx, base);
  static std::atomic<uint32_t> logs{0};
  const uint32_t state = ctx.r3.u32;
  if (Online() && state == 1) ctx.r3.u64 = 2;
  if (Online()) {
    static std::once_flag once;
    std::call_once(once, [] { realsteel::netplay::Prepare(); });
  }
  LogOnce(logs, 8, "XUserGetSigninState", user, state, ctx.r3.u32);
  realsteel::online::NoteSigninPoll();
}

// XUserCheckPrivilege(user r3, privilege r4, BOOL* result r5): the game's
// wrapper refuses outright for a locally signed-in profile. Granted.
extern "C" REX_FUNC(sub_822375A0) {
  realsteel::online::NoteCall(1);
  realsteel::online::NoteSigninPoll();
  const uint32_t user = ctx.r3.u32;
  const uint32_t privilege = ctx.r4.u32;
  const uint32_t result = ctx.r5.u32;
  __imp__sub_822375A0(ctx, base);
  static std::atomic<uint32_t> logs{0};
  const uint32_t status = ctx.r3.u32;
  if (Online() && result != 0) {
    REX_STORE_U32(result, 1u);
    ctx.r3.u64 = 0;
  }
  LogOnce(logs, 16, "XUserCheckPrivilege", user, privilege, status);
}

// XUserGetSigninInfo(user r3, flags r4, XUSER_SIGNIN_INFO* r5): xuid u64,
// dwInfoFlags u32, UserSigninState u32, guest number, sponsor, name[16].
extern "C" REX_FUNC(sub_82238918) {
  realsteel::online::NoteCall(3);
  realsteel::online::NoteSigninPoll();
  const uint32_t user = ctx.r3.u32;
  const uint32_t flags = ctx.r4.u32;
  const uint32_t info = ctx.r5.u32;
  __imp__sub_82238918(ctx, base);
  static std::atomic<uint32_t> logs{0};
  const uint32_t status = ctx.r3.u32;
  if (Online() && status == 0 && info != 0 && user == 0) StoreXuid(base + info);
  if (Online() && status == 0 && info != 0 && REX_LOAD_U32(info + 12) == 1) {
    REX_STORE_U32(info + 8, REX_LOAD_U32(info + 8) | 1u);  // LIVE_ENABLED
    REX_STORE_U32(info + 12, 2u);
  }
  // szUserName[16] at +24: the gamertag other players see
  if (Online() && status == 0 && info != 0 && user == 0) {
    const std::string name = realsteel::netplay::OnlineName();
    std::memset(base + info + 24, 0, 16);
    std::memcpy(base + info + 24, name.data(), name.size() < 15 ? name.size() : 15);
  }
  LogOnce(logs, 8, "XUserGetSigninInfo", user, flags, status);
}

// XUserGetName(user r3, char* buffer r4, size r5): the gamertag the game
// shows and puts into its session data. Online, the player's chosen name.
extern "C" REX_FUNC(sub_82237588) {
  const uint32_t user = ctx.r3.u32, buffer = ctx.r4.u32, size = ctx.r5.u32;
  __imp__sub_82237588(ctx, base);
  if (Online() && user == 0 && buffer != 0 && size > 1) {
    const std::string name = realsteel::netplay::OnlineName();
    const uint32_t n = std::min<uint32_t>(uint32_t(name.size()), size - 1);
    std::memcpy(base + buffer, name.data(), n);
    base[buffer + n] = 0;
    ctx.r3.u64 = 0;
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 6) {
    REXLOG_INFO("[online] XUserGetName(user {}, size {}) -> {:#x} '{}'", user, size, ctx.r3.u32,
                buffer != 0 ? reinterpret_cast<const char*>(base + buffer) : "");
  }
}

// XUserGetXUID(user r3, XUID* r4).
extern "C" REX_FUNC(sub_822377C8) {
  realsteel::online::NoteCall(4);
  realsteel::online::NoteSigninPoll();
  const uint32_t user = ctx.r3.u32, out = ctx.r4.u32;
  __imp__sub_822377C8(ctx, base);
  if (Online() && ctx.r3.u32 == 0 && user == 0 && out != 0) StoreXuid(base + out);
}

// XUserMuteListQuery(user r3, XUID r4, BOOL* on list r5): asked for the
// remote player several times a frame; the runtime has no answer. Nobody is
// muted.
extern "C" REX_FUNC(sub_823329B0) {
  realsteel::online::NoteCall(5);
  if (!Online()) {
    __imp__sub_823329B0(ctx, base);
    return;
  }
  if (ctx.r5.u32 != 0) REX_STORE_U32(ctx.r5.u32, 0u);
  ctx.r3.u64 = 0;
}

// XNetGetEthernetLinkStatus(): bit 0 active, 1 100 Mbps, 2 full duplex.
extern "C" REX_FUNC(sub_822B7BE0) {
  realsteel::online::NoteCall(2);
  __imp__sub_822B7BE0(ctx, base);
  static std::atomic<uint32_t> logs{0};
  const uint32_t status = ctx.r3.u32;
  if (Online() && status == 0) ctx.r3.u64 = 0x7;
  LogOnce(logs, 4, "XNetGetEthernetLinkStatus", 0, status, ctx.r3.u32);
}

// ---------------------------------------------------------------------------
// Quality-of-service probes. On the console these measure the link to Live
// and to session hosts, and carry a small blob of host data to searchers.
// XNQOS (guest, big-endian): count u32, pending u32, then
// XNQOSINFO[count] of 24 bytes: flags u8, reserved u8, probes sent u16,
// probes received u16, data size u16, data pointer u32, rtt min u16, rtt
// median u16, up bits/s u32, down bits/s u32.
// ---------------------------------------------------------------------------
namespace {

std::mutex g_qos_mutex;
std::vector<uint8_t> g_qos_listen_data;  // what the host answers probes with
bool g_qos_listening = false;

// A finished, healthy XNQOS result: one entry per blob (empty = no data).
uint32_t AllocQosResult(uint8_t* base, const std::vector<std::vector<uint8_t>>& blobs) {
  const uint32_t count = uint32_t(blobs.size());
  uint32_t total = 8 + 24 * count;
  for (const auto& blob : blobs) total += (uint32_t(blob.size()) + 3u) & ~3u;
  const uint32_t va = rex::system::kernel_memory()->SystemHeapAlloc(total);
  if (va == 0) return 0;
  std::memset(base + va, 0, total);
  REX_STORE_U32(va + 0, count);
  REX_STORE_U32(va + 4, 0u);  // none pending
  uint32_t data_va = va + 8 + 24 * count;
  for (uint32_t i = 0; i < count; ++i) {
    const std::vector<uint8_t>& blob = blobs[i];
    const uint32_t entry = va + 8 + 24 * i;
    uint8_t* info = base + entry;
    info[0] = 0x01 | 0x02 | (!blob.empty() ? 0x08 : 0x00);  // complete, contacted, data
    info[3] = 4;  // probes sent
    info[5] = 4;  // probes received
    info[6] = uint8_t(blob.size() >> 8);
    info[7] = uint8_t(blob.size());
    if (!blob.empty()) {
      REX_STORE_U32(entry + 8, data_va);
      std::memcpy(base + data_va, blob.data(), blob.size());
      data_va += (uint32_t(blob.size()) + 3u) & ~3u;
    }
    info[13] = 20;  // rtt min, ms
    info[15] = 25;  // rtt median, ms
    REX_STORE_U32(entry + 16, 4000000u);  // up, bits/s
    REX_STORE_U32(entry + 20, 4000000u);  // down
  }
  return va;
}

}  // namespace

bool realsteel::online::HostQosData(std::vector<uint8_t>* data) {
  std::lock_guard<std::mutex> lock(g_qos_mutex);
  *data = g_qos_listen_data;
  return g_qos_listening;
}

// XSessionCreate(flags r3, user r4, public slots r5, private slots r6, nonce*
// r7, XSESSION_INFO* r8, overlapped r9, handle* r10).
extern "C" REX_FUNC(sub_82416E38) {
  const uint32_t flags = ctx.r3.u32, info = ctx.r8.u32;
  __imp__sub_82416E38(ctx, base);
  if (Online()) g_created_session_info.store(info);
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 16) {
    REXLOG_INFO("[online] XSessionCreate(flags {:#x}, info {:#010x}) -> {:#x}", flags, info,
                ctx.r3.u32);
  }
}

// XNetQosServiceLookup(flags r3, event r4, XNQOS** r5): the link to the Live
// service itself. Always healthy.
extern "C" REX_FUNC(sub_822B7BA8) {
  const uint32_t out = ctx.r5.u32;
  if (!Online() || out == 0) {
    __imp__sub_822B7BA8(ctx, base);
    return;
  }
  const uint32_t result = AllocQosResult(base, {{}});
  REX_STORE_U32(out, result);
  // the game may wait on the event before it reads the result
  if (const uint32_t event = ctx.r4.u32) {
    auto object = rex::system::kernel_state()->object_table()->LookupObject<rex::system::XEvent>(event);
    if (object) object->Set(0, false);
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 4) {
    REXLOG_INFO("[online] XNetQosServiceLookup -> result at {:#010x}", result);
  }
  ctx.r3.u64 = result != 0 ? 0 : 0x2733;  // WSAEWOULDBLOCK-ish failure
}

// XNetQosListen(XNKID* r3, data r4, bytes r5, bits/s r6, flags r7): the host
// publishes (flag 0x4 SET_DATA) / stops publishing its probe answer.
extern "C" REX_FUNC(sub_822B7B28) {
  const uint32_t xnkid = ctx.r3.u32, data = ctx.r4.u32, bytes = ctx.r5.u32;
  const uint32_t flags = ctx.r7.u32;
  if (!Online()) {
    __imp__sub_822B7B28(ctx, base);
    return;
  }
  // The session exists by now: make its info ours (host address = this
  // game) and start answering searches. Only on the first call after the
  // create: the info block is the game's own memory and is not ours to
  // touch later (2026-10-05: patching it again on a later call from another
  // thread overwrote whatever lived there by then and crashed the host).
  if (const uint32_t info = g_created_session_info.exchange(0)) {
    uint8_t* p = base + info;
    bool empty = true;
    for (int i = 0; i < 8; ++i) empty = empty && p[i] == 0;
    if (empty) {
      for (int i = 0; i < 8; ++i) p[i] = uint8_t(0x52 + i * 37 + (info >> (i % 3)));
    }
    realsteel::netplay::WriteSelfXnAddr(p + 8);
    realsteel::netplay::SetHosting(true, p, p + 44);
  }
  if (flags & 0x12) realsteel::netplay::SetHosting(false, nullptr, nullptr);  // disable / release
  {
    std::lock_guard<std::mutex> lock(g_qos_mutex);
    if (flags & 0x04) {  // XNET_QOS_LISTEN_SET_DATA
      g_qos_listen_data.assign(base + data, base + data + (data != 0 ? bytes : 0));
    }
    if (flags & 0x01) g_qos_listening = true;    // ENABLE
    if (flags & 0x02) g_qos_listening = false;   // DISABLE
    if (flags & 0x10) {                          // RELEASE
      g_qos_listening = false;
      g_qos_listen_data.clear();
    }
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 16) {
    REXLOG_INFO("[online] XNetQosListen(session {:08x}{:08x}, {} bytes, flags {:#x})",
                xnkid != 0 ? REX_LOAD_U32(xnkid) : 0u,
                xnkid != 0 ? REX_LOAD_U32(xnkid + 4) : 0u, bytes, flags);
  }
  ctx.r3.u64 = 0;
}

// XNetQosLookup(count r3, XNADDR** r4, XNKID** r5, XNKEY** r6, cina r7, aina
// r8, service ids r9, probes r10, [stack] bits/s, flags, event, XNQOS** out):
// probing session hosts. Answered at once from what the hosts advertised.
extern "C" REX_FUNC(sub_822B7B48) {
  const uint32_t count = ctx.r3.u32, xnaddrs = ctx.r4.u32;
  const uint32_t out = REX_LOAD_U32(ctx.r1.u32 + 108);
  if (!Online() || out == 0) {
    __imp__sub_822B7B48(ctx, base);
    return;
  }
  std::vector<std::vector<uint8_t>> blobs(count != 0 && count <= 64 ? count : 1);
  for (uint32_t i = 0; i < count && i < 64 && xnaddrs != 0; ++i) {
    const uint32_t xnaddr = REX_LOAD_U32(xnaddrs + i * 4);
    if (xnaddr != 0) realsteel::netplay::QosDataFor(REX_LOAD_U32(xnaddr), &blobs[i]);
  }
  const uint32_t result = AllocQosResult(base, blobs);
  REX_STORE_U32(out, result);
  // The game waits on its event for the probes to finish.
  const uint32_t event = REX_LOAD_U32(ctx.r1.u32 + 100);
  if (event != 0) {
    auto object = rex::system::kernel_state()->object_table()->LookupObject<rex::system::XEvent>(
        event);
    if (object) object->Set(0, false);
  }
  REXLOG_INFO("[online] XNetQosLookup({} host(s), event {:#x}) -> result at {:#010x}, first "
              "blob {} bytes",
              count, event, result, blobs[0].size());
  ctx.r3.u64 = result != 0 ? 0 : 0x2733;
}

// Guest memory for one search result's host-name property (24 bytes) and its
// text (64 bytes); allocated once, reused by every search.
uint32_t HostNamePool(uint32_t index) {
  constexpr uint32_t kSlots = 64, kSlotBytes = 24 + 64;
  static uint32_t pool = 0;
  static std::once_flag once;
  std::call_once(once, [] { pool = rex::system::kernel_memory()->SystemHeapAlloc(kSlots * kSlotBytes); });
  return pool != 0 && index < kSlots ? pool + index * kSlotBytes : 0;
}

// XSessionSearchEx(procedure r3, user r4, max results r5, users r6, properties
// r7, contexts r8, property* r9, context* r10, [stack] size*, results*,
// overlapped*). Results: count u32, array pointer u32, then entries of 92
// bytes: XSESSION_INFO (id[8], host XNADDR[36], key[16]), open public, open
// private, filled public, filled private, property count, context count,
// property pointer, context pointer.
extern "C" REX_FUNC(sub_82417638) {
  const uint32_t max_results = ctx.r5.u32;
  const uint32_t size_ptr = REX_LOAD_U32(ctx.r1.u32 + 84);
  const uint32_t results = REX_LOAD_U32(ctx.r1.u32 + 92);
  const uint32_t overlapped = REX_LOAD_U32(ctx.r1.u32 + 100);
  const uint32_t needed = 8 + max_results * 92;
  if (!Online() || results == 0 || size_ptr == 0 || REX_LOAD_U32(size_ptr) < needed) {
    __imp__sub_82417638(ctx, base);  // also answers the buffer-size query
    return;
  }
  const std::vector<realsteel::netplay::FoundSession> found =
      realsteel::netplay::FindSessions(400);
  const uint32_t count = std::min<uint32_t>(max_results, uint32_t(found.size()));
  std::memset(base + results, 0, needed);
  REX_STORE_U32(results + 0, count);
  REX_STORE_U32(results + 4, results + 8);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = results + 8 + i * 92;
    std::memcpy(base + entry, found[i].session_id, 8);
    std::memcpy(base + entry + 8, found[i].xnaddr, 36);
    std::memcpy(base + entry + 44, found[i].key, 16);
    REX_STORE_U32(entry + 60, uint32_t(found[i].open_slots));
    REX_STORE_U32(entry + 68, uint32_t(found[i].filled_slots));
    // one property: X_PROPERTY_GAMER_HOSTNAME (0x40008109), the name the
    // session list shows ("Undefind Name" without it). XUSER_PROPERTY: id
    // u32, XUSER_DATA at +8 (type u8 = 4 UNICODE, cbData u32 at +16,
    // pwszData at +20); the UTF-16BE text lives in a pool of ours.
    const uint32_t prop = HostNamePool(i);
    if (prop != 0 && !found[i].host_name.empty()) {
      const uint32_t text = prop + 24;
      std::memset(base + prop, 0, 24 + 64);
      const std::string& name = found[i].host_name;
      const uint32_t chars = std::min<uint32_t>(uint32_t(name.size()), 31);
      for (uint32_t c = 0; c < chars; ++c) {
        base[text + c * 2] = 0;
        base[text + c * 2 + 1] = uint8_t(name[c]);
      }
      REX_STORE_U32(prop + 0, 0x40008109u);
      base[prop + 8] = 4;
      REX_STORE_U32(prop + 16, (chars + 1) * 2);
      REX_STORE_U32(prop + 20, text);
      REX_STORE_U32(entry + 76, 1u);    // property count
      REX_STORE_U32(entry + 84, prop);  // property pointer
    }
  }
  REXLOG_INFO("[online] XSessionSearchEx: {} session(s) found", count);
  if (overlapped != 0) {
    rex::system::kernel_state()->CompleteOverlappedImmediate(overlapped, 0);
    ctx.r3.u64 = 0x3E5;  // ERROR_IO_PENDING; already complete
  } else {
    ctx.r3.u64 = 0;
  }
}

// XNetQosRelease(XNQOS* r3).
extern "C" REX_FUNC(sub_822B7BC0) {
  if (!Online()) {
    __imp__sub_822B7BC0(ctx, base);
    return;
  }
  if (ctx.r3.u32 != 0) rex::system::kernel_memory()->SystemHeapFree(ctx.r3.u32);
  ctx.r3.u64 = 0;
}

// ---------------------------------------------------------------------------
// Call tracing of the game's network and session API wrappers (first 48 calls
// of each; select / recvfrom / sendto are per-frame, so only their first few).
// ---------------------------------------------------------------------------
#define RS_TRACE(fn, name, limit)                                             \
  extern "C" REX_FUNC(fn) {                                                   \
    const uint32_t a = ctx.r3.u32, b = ctx.r4.u32, c = ctx.r5.u32,            \
                   d = ctx.r6.u32;                                            \
    __imp__##fn(ctx, base);                                                   \
    static std::atomic<uint32_t> logs{0};                                     \
    if (logs.fetch_add(1) < (limit)) {                                        \
      REXLOG_INFO("[online] {}({:#x}, {:#x}, {:#x}, {:#x}) -> {:#x}", name, a,\
                  b, c, d, ctx.r3.u32);                                       \
    }                                                                         \
  }

RS_TRACE(sub_822B7A08, "XNetStartup", 48)
RS_TRACE(sub_822B7AD8, "XNetCleanup", 48)
RS_TRACE(sub_822B7B10, "XNetInAddrToString", 48)
RS_TRACE(sub_82333490, "WSAStartup", 48)
RS_TRACE(sub_82416FE8, "XSession@82416FE8", 48)
RS_TRACE(sub_824170A8, "XSession@824170A8", 48)
RS_TRACE(sub_82417158, "XSession@82417158", 48)
RS_TRACE(sub_82417208, "XSession@82417208", 48)
RS_TRACE(sub_824172B8, "XSession@824172B8", 48)
RS_TRACE(sub_82417360, "XSession@82417360", 48)
RS_TRACE(sub_82417408, "XSession@82417408", 48)
RS_TRACE(sub_824174E8, "XSession@824174E8", 48)
RS_TRACE(sub_82417588, "XSession@82417588", 48)
RS_TRACE(sub_82417720, "XSession@82417720", 48)
RS_TRACE(sub_824178C0, "XSession@824178C0", 48)
RS_TRACE(sub_82417998, "XSession@82417998", 48)
RS_TRACE(sub_82330A80, "XGI@82330A80", 24)
RS_TRACE(sub_82331740, "XGI@82331740", 24)
RS_TRACE(sub_823314E0, "XGI@823314E0", 24)
RS_TRACE(sub_82331A20, "XGI@82331A20", 24)
RS_TRACE(sub_82331CA8, "XGI@82331CA8", 24)
RS_TRACE(sub_82331F38, "XGI@82331F38", 24)
RS_TRACE(sub_82330D70, "XGI@82330D70", 24)

// XLiveBase string verify (user r3, flags r4, strings r5, result size r6,
// results r7, overlapped r8): the robot-name profanity check. With nobody to
// ask, the wrapper answered "not signed in" and the game renamed every robot
// to ROBOT when it went online (2026-10-11). Every name passes: an empty
// result set, completed at once.
extern "C" REX_FUNC(sub_823306A0) {
  if (!Online()) {
    __imp__sub_823306A0(ctx, base);
    return;
  }
  const uint32_t results = ctx.r7.u32, overlapped = ctx.r8.u32;
  if (ctx.r6.u32 >= 8 && results != 0) {
    REX_STORE_U32(results, 0);
    REX_STORE_U32(results + 4, 0);
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 8) REXLOG_INFO("[online] string verify: name accepted");
  if (overlapped != 0) {
    rex::system::kernel_state()->CompleteOverlappedImmediate(overlapped, 0);
    ctx.r3.u64 = 0x3E5;  // ERROR_IO_PENDING; already complete
  } else {
    ctx.r3.u64 = 0;
  }
}
RS_TRACE(sub_82331260, "XLiveBase@82331260", 24)
RS_TRACE(sub_82332880, "XLiveBase@82332880", 24)
RS_TRACE(sub_82330950, "XLiveBase@82330950", 24)
RS_TRACE(sub_82330FA8, "XLiveBase@82330FA8", 24)
// The friends enumerator (XLiveBase): the runtime has no friends service
// and fails it, which stops the leaderboard screen from opening. Online, the
// answer is "no friends" (ERROR_NO_MORE_FILES), which the game handles.
extern "C" REX_FUNC(sub_823307F0) {
  if (!Online()) {
    __imp__sub_823307F0(ctx, base);
    return;
  }
  if (ctx.r6.u32 != 0) REX_STORE_U32(ctx.r6.u32, 0u);
  if (ctx.r7.u32 != 0) REX_STORE_U32(ctx.r7.u32, 0u);
  ctx.r3.u64 = 18;
}
RS_TRACE(sub_82332C28, "XLiveBase@82332C28", 24)
RS_TRACE(sub_823313A0, "XLiveBase@823313A0", 24)
RS_TRACE(sub_82331128, "XLiveBase@82331128", 24)
RS_TRACE(sub_82332918, "XLiveBase@82332918", 24)
RS_TRACE(sub_82332E00, "XLiveBase@82332E00", 24)

// ---------------------------------------------------------------------------
// Leaderboards (2026-10-09), kept by the lobby server. The game's schema
// (from its XLAST): views 1 (total) and 3
// (weekly), columns 1..5 = ranking points, matches, wins, losses, KOs
// (properties 0x10000002..6), rating 65534 = property 0x20000001, all
// summed. XSessionWriteStats goes to the server; the stats enumerators
// read pages back from it.
// ---------------------------------------------------------------------------
namespace {

struct StatsEnumerator {
  uint32_t view = 0;
  bool by_player = false;
  uint32_t first_rank = 1, rows = 10;
  uint64_t xuid = 0;
  std::vector<uint16_t> columns;
  bool done = false;
};
std::mutex g_enum_mutex;
std::map<uint32_t, StatsEnumerator> g_enumerators;
uint32_t g_next_enum_handle = 0x7E5A0001;

uint32_t StatsBufferSize(const StatsEnumerator& e) {
  return 8 + 16 + e.rows * 48 + e.rows * uint32_t(e.columns.size()) * 24;
}

// XUserCreateStatsEnumeratorBy*(title, pivot, rows, spec count, specs,
// buffer size*, handle*): one spec (view + column ids, 136 bytes each).
void CreateStatsEnumerator(PPCContext& ctx, uint8_t* base, bool by_player, uint64_t pivot) {
  const uint32_t rows = ctx.r5.u32, spec_count = ctx.r6.u32, specs = ctx.r7.u32;
  const uint32_t size_out = ctx.r8.u32, handle_out = ctx.r9.u32;
  StatsEnumerator e;
  e.by_player = by_player;
  if (by_player) e.xuid = pivot; else e.first_rank = uint32_t(pivot);
  e.rows = std::max<uint32_t>(1, std::min<uint32_t>(rows, 100));
  if (spec_count > 0 && specs != 0) {
    e.view = REX_LOAD_U32(specs);
    const uint32_t n = std::min<uint32_t>(REX_LOAD_U32(specs + 4), 64);
    for (uint32_t i = 0; i < n; ++i) e.columns.push_back(REX_LOAD_U16(specs + 8 + i * 2));
  }
  std::lock_guard<std::mutex> lock(g_enum_mutex);
  const uint32_t handle = g_next_enum_handle++;
  if (size_out != 0) REX_STORE_U32(size_out, StatsBufferSize(e));
  if (handle_out != 0) REX_STORE_U32(handle_out, handle);
  REXLOG_INFO("[online] leaderboard view {} {} {} rows -> handle {:#x}", e.view,
              by_player ? "around the player" : "from rank " + std::to_string(e.first_rank), e.rows, handle);
  g_enumerators[handle] = std::move(e);
  ctx.r3.u64 = 0;
}

// Fills XUSER_STATS_READ_RESULTS { views 1, pViews } -> XUSER_STATS_VIEW
// { id, total rows, rows, pRows } -> XUSER_STATS_ROW (48 bytes: xuid, rank,
// rating i64 at +16, gamertag[16] at +24, columns, pColumns) ->
// XUSER_STATS_COLUMN (24: id u16, XUSER_DATA at +8: type, value at +16).
uint32_t FillStats(uint8_t* base, uint32_t buffer, uint32_t size, const StatsEnumerator& e,
                   const realsteel::netplay::LeaderboardPage& page) {
  const uint32_t rows = std::min<uint32_t>(uint32_t(page.rows.size()), e.rows);
  const uint32_t cols = uint32_t(e.columns.size());
  if (size < 8 + 16 + rows * 48 + rows * cols * 24) return 0;
  std::memset(base + buffer, 0, 8 + 16 + rows * 48 + rows * cols * 24);
  const uint32_t view = buffer + 8, row_base = view + 16, column_base = row_base + rows * 48;
  REX_STORE_U32(buffer, 1u);
  REX_STORE_U32(buffer + 4, view);
  REX_STORE_U32(view, e.view);
  REX_STORE_U32(view + 4, page.total);
  REX_STORE_U32(view + 8, rows);
  REX_STORE_U32(view + 12, rows ? row_base : 0u);
  for (uint32_t r = 0; r < rows; ++r) {
    const auto& src = page.rows[r];
    const uint32_t row = row_base + r * 48;
    REX_STORE_U32(row, uint32_t(src.xuid >> 32));
    REX_STORE_U32(row + 4, uint32_t(src.xuid));
    REX_STORE_U32(row + 8, src.rank);
    REX_STORE_U32(row + 16, uint32_t(uint64_t(src.rating) >> 32));
    REX_STORE_U32(row + 20, uint32_t(uint64_t(src.rating)));
    std::memcpy(base + row + 24, src.name.data(), std::min<size_t>(src.name.size(), 15));
    REX_STORE_U32(row + 40, cols);
    REX_STORE_U32(row + 44, cols ? column_base + r * cols * 24 : 0u);
    for (uint32_t c = 0; c < cols; ++c) {
      const uint32_t column = column_base + (r * cols + c) * 24;
      REX_STORE_U16(column, e.columns[c]);
      for (const auto& value : src.columns) {
        if (value.id != e.columns[c]) continue;
        base[column + 8] = value.type;
        if (value.type == 2 || value.type == 3) {
          REX_STORE_U32(column + 16, uint32_t(uint64_t(value.value) >> 32));
          REX_STORE_U32(column + 20, uint32_t(uint64_t(value.value)));
        } else {
          REX_STORE_U32(column + 16, uint32_t(value.value));
        }
      }
      if (base[column + 8] == 0) base[column + 8] = 1;  // LONG 0
    }
  }
  return 1;
}

}  // namespace

// XUserCreateStatsEnumeratorByRank(title r3, first rank r4, rows r5, ...).
extern "C" REX_FUNC(sub_82237708) {
  if (!Online()) {
    __imp__sub_82237708(ctx, base);
    return;
  }
  CreateStatsEnumerator(ctx, base, false, ctx.r4.u32);
}

// XUserCreateStatsEnumeratorByXuid(title r3, xuid r4 (64-bit), rows r5, ...).
extern "C" REX_FUNC(sub_82237768) {
  if (!Online()) {
    __imp__sub_82237768(ctx, base);
    return;
  }
  CreateStatsEnumerator(ctx, base, true, ctx.r4.u64);
}

// XEnumerate(handle r3, buffer r4, size r5, items* r6, overlapped r7): our
// stats enumerators answer from the server; everything else goes on.
extern "C" REX_FUNC(sub_82237F38) {
  const uint32_t handle = ctx.r3.u32, buffer = ctx.r4.u32, size = ctx.r5.u32, items = ctx.r6.u32,
                 overlapped = ctx.r7.u32;
  StatsEnumerator e;
  {
    std::lock_guard<std::mutex> lock(g_enum_mutex);
    auto it = g_enumerators.find(handle);
    if (it == g_enumerators.end()) {
      __imp__sub_82237F38(ctx, base);
      return;
    }
    e = it->second;
    it->second.done = true;
  }
  auto fetch = [base, buffer, size, e](uint32_t& extended, uint32_t& length) -> rex::X_RESULT {
    extended = 0;
    length = 0;
    if (e.done) return 18;  // ERROR_NO_MORE_FILES
    realsteel::netplay::LeaderboardPage page;
    if (!realsteel::netplay::ReadLeaderboard(e.view, e.by_player, e.first_rank, e.rows, e.xuid, e.columns, &page)) {
      page.view = e.view;  // no server or no answer: an empty board
    }
    length = FillStats(base, buffer, size, e, page);
    REXLOG_INFO("[online] leaderboard view {}: {} row(s) of {}", e.view, page.rows.size(), page.total);
    return length != 0 ? 0 : 18;
  };
  if (overlapped != 0) {
    rex::system::kernel_state()->CompleteOverlappedDeferredEx(fetch, overlapped);
    ctx.r3.u64 = 0x3E5;  // ERROR_IO_PENDING
    return;
  }
  uint32_t extended = 0, length = 0;
  const rex::X_RESULT result = fetch(extended, length);
  if (items != 0) REX_STORE_U32(items, length);
  ctx.r3.u64 = result;
}

// XUserReadStats(title r3, xuid count r4, xuids r5, spec count r6, specs r7,
// size* r8, results r9, overlapped r10) - the game's wrapper. The game reads
// its own row of the total board this way when it hosts (the session
// listing's ranking points and wins-losses-AKO) and to know its running
// totals, so the "10 / 50 / 100 ranked wins" achievements depend on it.
// The runtime answered with nothing (all zeros); now every requested player
// gets their row from the lobby server - a zero row if it has none.
extern "C" REX_FUNC(sub_82237630) {
  const uint32_t count = ctx.r4.u32, xuids = ctx.r5.u32, spec_count = ctx.r6.u32, specs = ctx.r7.u32;
  const uint32_t size_ptr = ctx.r8.u32, results = ctx.r9.u32, overlapped = ctx.r10.u32;
  if (!Online() || count == 0 || count > 16 || spec_count == 0 || spec_count > 4 || size_ptr == 0) {
    __imp__sub_82237630(ctx, base);
    return;
  }
  // Size as the game's own wrapper works it out (rows of 52, columns of 28:
  // more than the 48 / 24 we fill, so our layout always fits).
  uint32_t needed = 8;
  for (uint32_t s = 0; s < spec_count; ++s) {
    needed += 16 + count * 52 + REX_LOAD_U32(specs + s * 136 + 4) * count * 28;
  }
  if (REX_LOAD_U32(size_ptr) < needed || results == 0) {
    REX_STORE_U32(size_ptr, needed);
    ctx.r3.u64 = 122;  // ERROR_INSUFFICIENT_BUFFER
    return;
  }
  std::vector<uint64_t> players;
  for (uint32_t i = 0; i < count; ++i) {
    players.push_back((uint64_t(REX_LOAD_U32(xuids + i * 8)) << 32) | REX_LOAD_U32(xuids + i * 8 + 4));
  }
  std::vector<StatsEnumerator> views;
  for (uint32_t s = 0; s < spec_count; ++s) {
    StatsEnumerator e;
    e.view = REX_LOAD_U32(specs + s * 136);
    const uint32_t n = std::min<uint32_t>(REX_LOAD_U32(specs + s * 136 + 4), 64);
    for (uint32_t i = 0; i < n; ++i) e.columns.push_back(REX_LOAD_U16(specs + s * 136 + 8 + i * 2));
    e.by_player = true;
    e.rows = count;
    views.push_back(std::move(e));
  }
  const uint32_t size = REX_LOAD_U32(size_ptr);
  auto fetch = [base, results, size, players, views](uint32_t& extended, uint32_t& length) -> rex::X_RESULT {
    extended = 0;
    length = 0;
    // XUSER_STATS_READ_RESULTS { views, pViews } then each view's own block
    // (view header, rows, columns) as FillStats lays it out.
    std::memset(base + results, 0, std::min<uint32_t>(size, 8 + uint32_t(views.size()) * 16));
    REX_STORE_U32(results, uint32_t(views.size()));
    const uint32_t view_table = results + 8;
    REX_STORE_U32(results + 4, view_table);
    uint32_t cursor = view_table + uint32_t(views.size()) * 16;
    for (size_t v = 0; v < views.size(); ++v) {
      const StatsEnumerator& e = views[v];
      realsteel::netplay::LeaderboardPage page;
      page.view = e.view;
      for (uint64_t xuid : players) {
        realsteel::netplay::LeaderboardPage around;
        realsteel::netplay::LeaderboardRow row;
        row.xuid = xuid;
        if (realsteel::netplay::ReadLeaderboard(e.view, true, 1, 1, xuid, e.columns, &around)) {
          page.total = std::max(page.total, around.total);
          for (const auto& r : around.rows) {
            if (r.xuid == xuid) row = r;
          }
        }
        page.rows.push_back(std::move(row));
      }
      // FillStats writes a one-view result at |buffer|: put it at the
      // cursor and copy its view header into the shared table.
      const uint32_t block = cursor;
      const uint32_t block_size = 8 + 16 + uint32_t(page.rows.size()) * 48 +
                                  uint32_t(page.rows.size()) * uint32_t(e.columns.size()) * 24;
      if (block + block_size > results + size) return 122;
      if (!FillStats(base, block, block_size, e, page)) return 122;
      std::memcpy(base + view_table + v * 16, base + block + 8, 16);
      cursor = block + block_size;
      REXLOG_INFO("[online] stats read: view {} for {} player(s)", e.view, players.size());
    }
    length = 1;
    return 0;
  };
  if (overlapped != 0) {
    rex::system::kernel_state()->CompleteOverlappedDeferredEx(fetch, overlapped);
    ctx.r3.u64 = 0x3E5;  // ERROR_IO_PENDING
    return;
  }
  uint32_t extended = 0, length = 0;
  ctx.r3.u64 = fetch(extended, length);
}

// XSessionWriteStats(session r3, xuid r4 (64-bit), view count r5, views r6,
// overlapped r7): XSESSION_VIEW_PROPERTIES (id, property count, properties)
// with XUSER_PROPERTY entries of 24 bytes (id, XUSER_DATA at +8).
extern "C" REX_FUNC(sub_82417800) {
  const uint64_t xuid = ctx.r4.u64;
  const uint32_t count = ctx.r5.u32, views = ctx.r6.u32;
  if (Online() && views != 0) {
    std::vector<realsteel::netplay::StatsView> out;
    for (uint32_t v = 0; v < count && v < 8; ++v) {
      realsteel::netplay::StatsView view;
      view.id = REX_LOAD_U32(views + v * 12);
      const uint32_t props = REX_LOAD_U32(views + v * 12 + 4), list = REX_LOAD_U32(views + v * 12 + 8);
      for (uint32_t i = 0; i < props && i < 32 && list != 0; ++i) {
        const uint32_t prop = list + i * 24;
        realsteel::netplay::StatsProperty p;
        p.id = REX_LOAD_U32(prop);
        p.type = base[prop + 8];
        if (p.type == 2 || p.type == 3) {
          p.value = int64_t((uint64_t(REX_LOAD_U32(prop + 16)) << 32) | REX_LOAD_U32(prop + 20));
        } else if (p.type == 1) {
          p.value = int32_t(REX_LOAD_U32(prop + 16));
        } else {
          continue;  // strings / blobs are not kept
        }
        view.properties.push_back(p);
      }
      out.push_back(std::move(view));
    }
    realsteel::netplay::SendStats(xuid, out);
  }
  __imp__sub_82417800(ctx, base);
}
