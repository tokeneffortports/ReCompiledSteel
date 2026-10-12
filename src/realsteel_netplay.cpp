// realsteel - the network under the game's Xbox Live code (2026-10-05).
//
// On the console the game talks to its peer over one UDP ("VDP") socket, to
// addresses it gets by converting the XNADDR in a session's info, and finds
// sessions through Live matchmaking. Here:
//   * every running game owns one real UDP socket (the "service socket",
//     port --realsteel_net_port, or the next free one). Everything goes
//     through it: session discovery, and the game's own packets, tunnelled.
//     One port to forward, and several games can run on one machine.
//   * every game picks a random 10.x.y.z "guest address". XNADDRs carry it,
//     the game's socket calls use it, and a peer table maps it to the real
//     endpoint it was last heard from.
//   * sessions are found by asking: a broadcast on the LAN, the same machine,
//     and any hosts listed in --realsteel_net_peers (ip[:port],...).
//
// Packets: "RSNP", type u8, sender guest address (4 bytes), then
//   1 DISCOVER  password hash u64, mods signature u64 (the host answers
//               only when both match its own; 0 = no password / no mods),
//               mod names (u8 length + "A, B, C")
//   2 ADVERT    session id[8] key[16] game port u16 open u8 filled u8
//               qos size u16, qos data, host name (u8 length + UTF-8),
//               mods signature u64, mod names (u8 length + "A, B, C")
//   3 DATA      source port u16, destination port u16, payload
// Multi-byte fields are big-endian.
//
// Lobby (--realsteel_lobby_server host[:port], 2026-10-09): a public server
// (tools/lobby/realsteel_lobby.py) that hosts register with and joiners ask
// for the list, so players find each other without typing addresses. It
// sees each game's public address (the service socket's, so the same NAT
// mapping the peers use) and, when a joiner asks, tells every host the
// joiner's address so both sides send first (UDP hole punching).
// Packets: "RSLB", version u8 (1), type u8, then
//   1 REGISTER    guest address u32, flags u8 (1 = password), filled u8,
//                 open u8, mods hash u64, name (u8 length + UTF-8)
//   2 UNREGISTER  guest address u32
//   3 LIST        guest address u32
//   4 LISTING     (server) count u8, then count x: ip u32, port u16, flags
//                 u8, filled u8, open u8, mods hash u64, age s u16, name
//   5 INTRO       (server, to a host) joiner ip u32, port u16
//   6 STATS       reporter xuid u64, reporter name, player xuid u64, view
//                 count u8, per view: id u32, property count u8, per
//                 property: id u32, type u8, value i64. Both games of a
//                 ranked match report both players (arbitration); the
//                 server counts a result once.
//   7 READ        request u32, view u32, by-player u8, first rank u32, rows
//                 u16, xuid u64, column count u8, column ids u16...
//   8 ROWS        (server) request u32, view u32, total rows u32, row count
//                 u8, per row: xuid u64, rank u32, rating i64, name, column
//                 count u8, per column: id u16, type u8, value i64
//   9 HELLO       xuid u64, name: who this game is (every 30 s), so the
//                 boards show each player's own name
// and the RSNP packet 4 PING (no payload) opens the host's NAT towards a
// joiner.

#include "realsteel_netplay.h"

#include "realsteel_online.h"

#include "generated/default_final/realsteel_init.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

REXCVAR_DEFINE_INT32(realsteel_net_port, 28700, "Real Steel",
                     "UDP port of the online service socket (the next free "
                     "one is used when it is taken)");
REXCVAR_DEFINE_STRING(realsteel_net_peers, "", "Real Steel",
                      "Hosts to ask for sessions besides the local network: "
                      "ip[:port],ip[:port],...");
REXCVAR_DEFINE_STRING(realsteel_lobby_server, "", "Real Steel",
                      "Lobby server that lists matches over the internet: "
                      "host[:port] (port 28800 if left out); empty = none");
REXCVAR_DEFINE_BOOL(realsteel_net_lan, true, "Real Steel",
                    "Look for matches on the local network too");
REXCVAR_DEFINE_STRING(realsteel_online_name, "", "Real Steel",
                      "Your name in online play (empty: the computer's user name)");
REXCVAR_DEFINE_STRING(realsteel_net_password, "", "Real Steel",
                      "Match password: a host only shows its session to "
                      "players who entered the same one (empty = open)");

namespace realsteel::netplay {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kNoSocket = INVALID_SOCKET;
void CloseNative(NativeSocket s) { closesocket(s); }
#else
using NativeSocket = int;
constexpr NativeSocket kNoSocket = -1;
void CloseNative(NativeSocket s) { close(s); }
#endif

constexpr uint8_t kMagic[4] = {'R', 'S', 'N', 'P'};
enum : uint8_t { kDiscover = 1, kAdvert = 2, kData = 3, kPing = 4 };
constexpr uint8_t kLobbyMagic[4] = {'R', 'S', 'L', 'B'};
enum : uint8_t { kRegister = 1, kUnregister = 2, kList = 3, kListing = 4, kIntro = 5, kStats = 6,
                 kRead = 7, kRows = 8, kHello = 9 };
constexpr uint16_t kLobbyPort = 28800;
constexpr uint32_t kDiscoveryPorts = 4;  // base port .. base port + 3

struct Endpoint {
  uint32_t ip = 0;    // host byte order
  uint16_t port = 0;
};

struct Advert {
  uint8_t session_id[8] = {};
  uint8_t key[16] = {};
  uint32_t guest_ip = 0;
  uint16_t game_port = 0;
  uint8_t open_slots = 0, filled_slots = 0;
  std::vector<uint8_t> qos;
  std::string name;
  bool has_mods = false;   // the advert carried the host's mod signature
  uint64_t mods = 0;
  std::string mod_names;   // "A, B, C" (empty: none, or an older build)
  std::chrono::steady_clock::time_point seen;
};

struct Datagram {
  uint32_t from_ip = 0;      // guest address of the sender
  uint16_t from_port = 0;
  uint16_t to_port = 0;
  std::vector<uint8_t> payload;
};

std::mutex g_mutex;  // everything below
NativeSocket g_socket = kNoSocket;
uint16_t g_real_port = 0;
uint32_t g_self_ip = 0;                      // guest address, host byte order
std::map<uint32_t, Endpoint> g_peers;        // guest address -> real endpoint
std::map<uint32_t, Advert> g_adverts;        // by host guest address
std::deque<Datagram> g_inbox;
bool g_started = false;

// Lobby: resolved server endpoint (0 = none / not resolved yet) and what it
// last told us.
Endpoint g_lobby;
std::string g_lobby_status = "not set";
size_t g_lobby_listed = 0;
size_t g_hidden_mods = 0;   // sessions not shown: the host's mods differ from ours
std::string g_mods_notice;  // what the player can do about it (panel text)
std::chrono::steady_clock::time_point g_mods_notice_time;
std::chrono::steady_clock::time_point g_lobby_heard;
bool g_registered = false;
// Leaderboard requests waiting for the server's ROWS.
std::condition_variable g_rows_cv;
std::map<uint32_t, LeaderboardPage> g_rows;   // by request id, once answered
uint32_t g_next_request = 1;
std::string g_mods_signature;               // robot names, for the mods hash

// Hosting state (set by the session hooks).
bool g_hosting = false;
uint8_t g_host_session[8] = {};
uint8_t g_host_key[16] = {};
uint16_t g_game_port = 0;                    // what the game bound

void Put16(std::vector<uint8_t>& out, uint16_t v) {
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}
void Put32(std::vector<uint8_t>& out, uint32_t v) {
  Put16(out, uint16_t(v >> 16));
  Put16(out, uint16_t(v));
}
uint16_t Get16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
uint32_t Get32(const uint8_t* p) { return (uint32_t(Get16(p)) << 16) | Get16(p + 2); }

// FNV-1a of the password; not secret, just a lock on the door.
uint64_t PasswordHash() {
  const std::string text = REXCVAR_GET(realsteel_net_password);
  if (text.empty()) return 0;
  uint64_t h = 1469598103934665603ull;
  for (const char c : text) h = (h ^ uint8_t(c)) * 1099511628211ull;
  return h != 0 ? h : 1;
}

std::vector<uint8_t> LobbyHeader(uint8_t type) {
  std::vector<uint8_t> out(kLobbyMagic, kLobbyMagic + 4);
  out.push_back(1);
  out.push_back(type);
  return out;
}

std::vector<uint8_t> Header(uint8_t type) {
  std::vector<uint8_t> out(kMagic, kMagic + 4);
  out.push_back(type);
  Put32(out, g_self_ip);
  return out;
}

void SendTo(const Endpoint& to, const std::vector<uint8_t>& packet) {
  if (g_socket == kNoSocket || to.ip == 0 || to.port == 0) return;
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(to.port);
  addr.sin_addr.s_addr = htonl(to.ip);
  sendto(g_socket, reinterpret_cast<const char*>(packet.data()), int(packet.size()), 0,
         reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
}

uint64_t ModsHash() {  // g_mutex held
  uint64_t h = 1469598103934665603ull;
  for (const char c : g_mods_signature) h = (h ^ uint8_t(c)) * 1099511628211ull;
  return g_mods_signature.empty() ? 0 : h;
}

// Our mod robots as "A, B, C" (the signature holds one name per line).
std::string ModsNames() {  // g_mutex held
  std::string out;
  std::string line;
  for (const char c : g_mods_signature) {
    if (c == '\n') {
      if (!line.empty()) out += (out.empty() ? "" : ", ") + line;
      line.clear();
    } else {
      line.push_back(c);
    }
  }
  if (!line.empty()) out += (out.empty() ? "" : ", ") + line;
  if (out.size() > 200) out = out.substr(0, 197) + "...";
  return out;
}

std::vector<std::string> SplitNames(const std::string& names) {
  std::vector<std::string> out;
  size_t at = 0;
  while (at <= names.size()) {
    const size_t comma = names.find(", ", at);
    const std::string part = names.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
    if (!part.empty()) out.push_back(part);
    if (comma == std::string::npos) break;
    at = comma + 2;
  }
  return out;
}

// "GUNDAM, COMET" -> what the other side has that we lack and the reverse,
// as one sentence for the panel. |who| names the other side.
std::string ModsDifference(const std::string& theirs, const std::string& who) {  // g_mutex held
  const std::vector<std::string> mine = SplitNames(ModsNames()), other = SplitNames(theirs);
  auto missing = [](const std::vector<std::string>& a, const std::vector<std::string>& b) {
    std::string out;
    for (const std::string& x : a) {
      if (std::find(b.begin(), b.end(), x) == b.end()) out += (out.empty() ? "" : ", ") + x;
    }
    return out;
  };
  const std::string need = missing(other, mine), extra = missing(mine, other);
  std::string text;
  if (!need.empty()) text += who + " has mods you don't: " + need + ". ";
  if (!extra.empty()) text += "You have mods " + who + " doesn't: " + extra + ". ";
  if (text.empty()) text = "Different mod versions on each side. ";
  return text + "Install the same mod folders on both sides to play together.";
}

void SetModsNotice(const std::string& text) {  // g_mutex held
  g_mods_notice = text;
  g_mods_notice_time = std::chrono::steady_clock::now();
}

void AppendNames(std::vector<uint8_t>& out, const std::string& names) {
  out.push_back(uint8_t(std::min<size_t>(names.size(), 255)));
  out.insert(out.end(), names.begin(), names.begin() + std::min<size_t>(names.size(), 255));
}

std::string PlayerName() {
  std::string name = REXCVAR_GET(realsteel_online_name);
  if (name.empty()) {
    // Not the computer's user name (often a real name): PLAYER plus a few
    // digits of this player's online identity, so two unnamed players
    // still tell each other apart.
    char text[24];
    std::snprintf(text, sizeof(text), "PLAYER%04u", unsigned(realsteel::online::Xuid() % 10000u));
    name = text;
  }
  if (name.size() > 15) name.resize(15);
  return name;
}

std::vector<uint8_t> BuildAdvert() {  // g_mutex held
  std::vector<uint8_t> qos;
  realsteel::online::HostQosData(&qos);
  std::vector<uint8_t> out = Header(kAdvert);
  out.insert(out.end(), g_host_session, g_host_session + 8);
  out.insert(out.end(), g_host_key, g_host_key + 16);
  Put16(out, g_game_port);
  out.push_back(1);  // open slots
  out.push_back(1);  // filled slots
  Put16(out, uint16_t(qos.size()));
  out.insert(out.end(), qos.begin(), qos.end());
  const std::string name = PlayerName();
  out.push_back(uint8_t(name.size()));
  out.insert(out.end(), name.begin(), name.end());
  const uint64_t mods = ModsHash();  // after the name (2026-10-10)
  Put32(out, uint32_t(mods >> 32));
  Put32(out, uint32_t(mods));
  AppendNames(out, ModsNames());
  return out;
}

void HandleLobby(const uint8_t* p, int n, const Endpoint& from) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_lobby.ip == 0 || from.ip != g_lobby.ip || from.port != g_lobby.port || p[4] != 1) return;
  g_lobby_heard = std::chrono::steady_clock::now();
  const uint8_t type = p[5];
  if (type == kListing && n >= 7) {
    const int count = p[6];
    int at = 7;
    size_t listed = 0;
    std::vector<uint8_t> discover = Header(kDiscover);
    const uint64_t hash = PasswordHash();
    Put32(discover, uint32_t(hash >> 32));
    Put32(discover, uint32_t(hash));
    const uint64_t mods = ModsHash();
    Put32(discover, uint32_t(mods >> 32));
    Put32(discover, uint32_t(mods));
    AppendNames(discover, ModsNames());
    // entry: ip u32, port u16, flags u8, filled u8, open u8, mods u64,
    // age u16 (19 bytes), then the name (u8 length + bytes)
    for (int i = 0; i < count && at + 20 <= n; ++i) {
      const Endpoint host{Get32(p + at), Get16(p + at + 4)};
      const int name_len = p[at + 19];
      at += 20 + name_len;
      if (at > n) break;
      ++listed;
      SendTo(host, discover);  // the session answers with an ADVERT
    }
    g_lobby_listed = listed;
    {
      static std::atomic<int> logs{0};
      if (logs.fetch_add(1) < 10) REXLOG_INFO("[netplay] lobby listed {} match(es)", listed);
    }
    g_lobby_status = "connected, " + std::to_string(listed) + (listed == 1 ? " match listed" : " matches listed");
  } else if (type == kIntro && n >= 12) {
    // a joiner is about to knock: send first so our NAT lets it in
    SendTo({Get32(p + 6), Get16(p + 10)}, Header(kPing));
  } else if (type == kRegister) {
    if (g_lobby_status.rfind("connected", 0) != 0) g_lobby_status = "connected";
  } else if (type == kRows && n >= 19) {
    LeaderboardPage page;
    const uint32_t request = Get32(p + 6);
    page.view = Get32(p + 10);
    page.total = Get32(p + 14);
    const int count = p[18];
    int at = 19;
    for (int i = 0; i < count && at + 21 <= n; ++i) {
      LeaderboardRow row;
      row.xuid = (uint64_t(Get32(p + at)) << 32) | Get32(p + at + 4);
      row.rank = Get32(p + at + 8);
      row.rating = int64_t((uint64_t(Get32(p + at + 12)) << 32) | Get32(p + at + 16));
      const int name_len = p[at + 20];
      at += 21;
      if (at + name_len + 1 > n) break;
      row.name.assign(reinterpret_cast<const char*>(p + at), name_len);
      at += name_len;
      const int columns = p[at++];
      for (int c = 0; c < columns && at + 11 <= n; ++c) {
        LeaderboardColumn column;
        column.id = Get16(p + at);
        column.type = p[at + 2];
        column.value = int64_t((uint64_t(Get32(p + at + 3)) << 32) | Get32(p + at + 7));
        row.columns.push_back(column);
        at += 11;
      }
      page.rows.push_back(std::move(row));
    }
    g_rows[request] = std::move(page);
    g_rows_cv.notify_all();
  }
}

void Receive() {
  std::vector<uint8_t> buf(65536);
  for (;;) {
    sockaddr_in from{};
#ifdef _WIN32
    int from_len = sizeof(from);
#else
    socklen_t from_len = sizeof(from);
#endif
    const int n = int(recvfrom(g_socket, reinterpret_cast<char*>(buf.data()),
                               int(buf.size()), 0, reinterpret_cast<sockaddr*>(&from),
                               &from_len));
    if (n >= 6 && std::memcmp(buf.data(), kLobbyMagic, 4) == 0) {
      HandleLobby(buf.data(), n, {ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)});
      continue;
    }
    if (n < 9 || std::memcmp(buf.data(), kMagic, 4) != 0) {
      if (n < 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    const uint8_t type = buf[4];
    const uint32_t sender = Get32(buf.data() + 5);
    const Endpoint real{ntohl(from.sin_addr.s_addr), ntohs(from.sin_port)};
    const uint8_t* p = buf.data() + 9;
    const int left = n - 9;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (sender == g_self_ip || sender == 0) continue;  // our own broadcast
    g_peers[sender] = real;
    if (type == kDiscover) {
      const uint64_t offered = left >= 8 ? (uint64_t(Get32(p)) << 32) | Get32(p + 4) : 0;
      // Mods (2026-10-10): a joiner without our robots could not load our
      // parts, so it does not get an answer (an older build sends no
      // signature and is answered as before).
      const uint64_t offered_mods = left >= 16 ? (uint64_t(Get32(p + 8)) << 32) | Get32(p + 12) : ModsHash();
      if (g_hosting && offered == PasswordHash()) {
        if (offered_mods == ModsHash()) {
          SendTo(real, BuildAdvert());
        } else {
          std::string theirs;
          if (left >= 17 && 17 + int(p[16]) <= left) theirs.assign(reinterpret_cast<const char*>(p + 17), p[16]);
          SetModsNotice("A player searched but can't see your match. " + ModsDifference(theirs, "That player"));
          static std::atomic<int> logs{0};
          if (logs.fetch_add(1) < 10) REXLOG_INFO("[netplay] not answering a search: {}", g_mods_notice);
        }
      }
    } else if (type == kAdvert && left >= 30) {
      Advert& a = g_adverts[sender];
      std::memcpy(a.session_id, p, 8);
      std::memcpy(a.key, p + 8, 16);
      a.guest_ip = sender;
      a.game_port = Get16(p + 24);
      a.open_slots = p[26];
      a.filled_slots = p[27];
      const uint16_t qos = Get16(p + 28);
      a.qos.assign(p + 30, p + 30 + (30 + qos <= left ? qos : 0));
      a.name.clear();
      a.has_mods = false;
      if (30 + qos < left) {
        const int name_len = p[30 + qos];
        if (31 + qos + name_len <= left) a.name.assign(reinterpret_cast<const char*>(p + 31 + qos), name_len);
        a.mod_names.clear();
        if (31 + qos + name_len + 8 <= left) {
          const uint8_t* m = p + 31 + qos + name_len;
          a.mods = (uint64_t(Get32(m)) << 32) | Get32(m + 4);
          a.has_mods = true;
          const int names_at = 31 + qos + name_len + 8;
          if (names_at < left && names_at + 1 + int(p[names_at]) <= left) {
            a.mod_names.assign(reinterpret_cast<const char*>(p + names_at + 1), p[names_at]);
          }
        }
      }
      a.seen = std::chrono::steady_clock::now();
    } else if (type == kData && left >= 4) {
      Datagram d;
      d.from_ip = sender;
      d.from_port = Get16(p);
      d.to_port = Get16(p + 2);
      d.payload.assign(p + 4, p + left);
      if (g_inbox.size() < 4096) g_inbox.push_back(std::move(d));
    }
  }
}

void LobbyLoop();

void Start() {  // g_mutex held
  if (g_started) return;
  g_started = true;
#ifdef _WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  std::random_device rd;
  do {
    g_self_ip = (10u << 24) | (rd() & 0x00FFFFFFu);
  } while ((g_self_ip & 0xFF) == 0 || (g_self_ip & 0xFF) == 0xFF);
  const NativeSocket s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kNoSocket) {
    REXLOG_INFO("[netplay] cannot create a UDP socket");
    return;
  }
  const int yes = 1;
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&yes), sizeof(yes));
  const uint16_t base_port = uint16_t(REXCVAR_GET(realsteel_net_port));
  for (uint16_t i = 0; i < 8 && g_real_port == 0; ++i) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(base_port + i));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) {
      g_real_port = uint16_t(base_port + i);
    }
  }
  if (g_real_port == 0) {
    REXLOG_INFO("[netplay] no free UDP port from {} up", base_port);
    CloseNative(s);
    return;
  }
  g_socket = s;
  std::thread(Receive).detach();
  std::thread(LobbyLoop).detach();
  REXLOG_INFO("[netplay] service socket on UDP {}; guest address {}.{}.{}.{}", g_real_port,
              g_self_ip >> 24, (g_self_ip >> 16) & 0xFF, (g_self_ip >> 8) & 0xFF,
              g_self_ip & 0xFF);
}

// Resolves the lobby address now and then, and keeps the host's entry
// alive (every 3 s; the server forgets entries after 15 s).
void LobbyLoop() {
  std::string resolved_for;
  int hello_countdown = 0;  // HELLO now and every 10 rounds (30 s)
  auto last_resolve = std::chrono::steady_clock::now() - std::chrono::hours(1);
  for (;;) {
    std::string server = REXCVAR_GET(realsteel_lobby_server);
    const auto now = std::chrono::steady_clock::now();
    if (server != resolved_for || now - last_resolve > std::chrono::minutes(5)) {
      resolved_for = server;
      last_resolve = now;
      Endpoint found;
      std::string status = "not set";
      if (!server.empty()) {
        uint16_t port = kLobbyPort;
        const size_t colon = server.rfind(':');
        if (colon != std::string::npos) {
          port = uint16_t(std::atoi(server.c_str() + colon + 1));
          server.resize(colon);
        }
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* result = nullptr;
        if (getaddrinfo(server.c_str(), nullptr, &hints, &result) == 0 && result != nullptr) {
          found = {ntohl(reinterpret_cast<sockaddr_in*>(result->ai_addr)->sin_addr.s_addr), port};
          freeaddrinfo(result);
          status = "waiting for the server";
        } else {
          status = "cannot find " + server;
        }
      }
      std::lock_guard<std::mutex> lock(g_mutex);
      g_lobby = found;
      hello_countdown = 0;
      g_lobby_status = status;
      REXLOG_INFO("[netplay] lobby server: {}", status);
    }
    {
      std::lock_guard<std::mutex> lock(g_mutex);
      if (g_lobby.ip != 0 && g_lobby_heard.time_since_epoch().count() != 0 &&
          std::chrono::steady_clock::now() - g_lobby_heard > std::chrono::seconds(20) &&
          g_lobby_status.rfind("connected", 0) == 0) {
        g_lobby_status = "no answer from the server";
      }
      if (g_lobby.ip != 0 && --hello_countdown <= 0) {
        hello_countdown = 10;  // 30 s
        std::vector<uint8_t> out = LobbyHeader(kHello);
        const uint64_t xuid = realsteel::online::Xuid();
        Put32(out, uint32_t(xuid >> 32));
        Put32(out, uint32_t(xuid));
        const std::string name = PlayerName();
        out.push_back(uint8_t(name.size()));
        out.insert(out.end(), name.begin(), name.end());
        SendTo(g_lobby, out);
      }
      if (g_lobby.ip != 0 && g_hosting) {
        std::vector<uint8_t> out = LobbyHeader(kRegister);
        Put32(out, g_self_ip);
        out.push_back(PasswordHash() != 0 ? 1 : 0);
        out.push_back(1);  // filled slots
        out.push_back(1);  // open slots
        const uint64_t mods = ModsHash();
        Put32(out, uint32_t(mods >> 32));
        Put32(out, uint32_t(mods));
        const std::string name = PlayerName();
        out.push_back(uint8_t(name.size()));
        out.insert(out.end(), name.begin(), name.end());
        SendTo(g_lobby, out);
        g_registered = true;
      } else if (g_lobby.ip != 0 && g_registered) {
        std::vector<uint8_t> out = LobbyHeader(kUnregister);
        Put32(out, g_self_ip);
        SendTo(g_lobby, out);
        g_registered = false;
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(3));
  }
}

void EnsureStarted() {
  std::lock_guard<std::mutex> lock(g_mutex);
  Start();
}

// Asks everyone we can reach for their sessions.
void Discover() {
  std::lock_guard<std::mutex> lock(g_mutex);
  Start();
  std::vector<uint8_t> packet = Header(kDiscover);
  const uint64_t hash = PasswordHash();
  Put32(packet, uint32_t(hash >> 32));
  Put32(packet, uint32_t(hash));
  const uint64_t mods = ModsHash();
  Put32(packet, uint32_t(mods >> 32));
  Put32(packet, uint32_t(mods));
  AppendNames(packet, ModsNames());
  const uint16_t base_port = uint16_t(REXCVAR_GET(realsteel_net_port));
  for (uint16_t i = 0; i < kDiscoveryPorts && REXCVAR_GET(realsteel_net_lan); ++i) {
    const uint16_t port = uint16_t(base_port + i);
    SendTo({0xFFFFFFFFu, port}, packet);                      // the LAN
    if (port != g_real_port) SendTo({0x7F000001u, port}, packet);  // this machine
  }
  {
    static std::atomic<int> logs{0};
    if (logs.fetch_add(1) < 10) REXLOG_INFO("[netplay] discover: lan {}, lobby {}", REXCVAR_GET(realsteel_net_lan), g_lobby.ip != 0);
  }
  if (g_lobby.ip != 0) {  // the lobby answers with the list; see HandleLobby
    std::vector<uint8_t> list = LobbyHeader(kList);
    Put32(list, g_self_ip);
    SendTo(g_lobby, list);
  }
  // Configured hosts: ip[:port],...
  const std::string peers = REXCVAR_GET(realsteel_net_peers);
  size_t at = 0;
  while (at < peers.size()) {
    size_t end = peers.find(',', at);
    if (end == std::string::npos) end = peers.size();
    std::string item = peers.substr(at, end - at);
    at = end + 1;
    uint16_t port = base_port;
    const size_t colon = item.find(':');
    if (colon != std::string::npos) {
      port = uint16_t(std::atoi(item.c_str() + colon + 1));
      item.resize(colon);
    }
    in_addr ip{};
    if (!item.empty() && inet_pton(AF_INET, item.c_str(), &ip) == 1) {
      SendTo({ntohl(ip.s_addr), port}, packet);
    }
  }
}

// XNADDR (36 bytes): ina[4], inaOnline[4], wPortOnline u16, abEnet[6],
// abOnline[20]. All derived from the guest address.
void WriteXnAddr(uint8_t* out, uint32_t guest_ip, uint16_t port) {
  std::memset(out, 0, 36);
  for (int i = 0; i < 4; ++i) out[i] = out[4 + i] = uint8_t(guest_ip >> (24 - 8 * i));
  out[8] = uint8_t(port >> 8);
  out[9] = uint8_t(port);
  out[10] = 0x00;
  out[11] = 0x22;
  for (int i = 0; i < 4; ++i) out[12 + i] = out[i];
  for (int i = 0; i < 20; ++i) out[16 + i] = out[i % 4];
}

// The game's sockets: small fake handles (it only ever opens one or two).
struct GuestSocket {
  bool open = false;
  uint16_t port = 0;
};
constexpr uint32_t kHandleBase = 0x5350;
GuestSocket g_guest_sockets[8];

GuestSocket* FindSocket(uint32_t handle) {
  const uint32_t i = handle - kHandleBase;
  return i < 8 && g_guest_sockets[i].open ? &g_guest_sockets[i] : nullptr;
}

std::atomic<uint32_t> g_logs{0};
bool LogSome() { return g_logs.fetch_add(1) < 200; }

}  // namespace

// --- used by the session hooks (realsteel_online.cpp) -----------------------

uint32_t SelfGuestIp() {
  EnsureStarted();
  return g_self_ip;
}

void SetHosting(bool hosting, const uint8_t* session_id, const uint8_t* key) {
  EnsureStarted();
  std::lock_guard<std::mutex> lock(g_mutex);
  g_hosting = hosting;
  if (session_id != nullptr) std::memcpy(g_host_session, session_id, 8);
  if (key != nullptr) std::memcpy(g_host_key, key, 16);
}

void WriteSelfXnAddr(uint8_t* out) {
  EnsureStarted();
  std::lock_guard<std::mutex> lock(g_mutex);
  WriteXnAddr(out, g_self_ip, g_game_port != 0 ? g_game_port : 1000);
}

void Prepare() { EnsureStarted(); }

std::vector<FoundSession> FindSessions(uint32_t wait_ms) {
  // Asked three times over the wait: the lobby may still be resolving, and
  // through a lobby the host's first answer can be lost while the routers
  // on both sides open up (hole punching).
  for (int round = 0; round < 3; ++round) {
    Discover();
    std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms / 3 + 1));
  }
  std::vector<FoundSession> out;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = std::chrono::steady_clock::now();
  size_t hidden = 0;
  for (const auto& [ip, a] : g_adverts) {
    if (now - a.seen > std::chrono::seconds(3)) continue;
    if (a.has_mods && a.mods != ModsHash()) {
      ++hidden;  // its robots are not ours: the game could not load them
      const std::string who = a.name.empty() ? std::string("The host") : a.name;
      SetModsNotice(who + "'s match is hidden. " + ModsDifference(a.mod_names, who));
      continue;
    }
    FoundSession f{};
    std::memcpy(f.session_id, a.session_id, 8);
    std::memcpy(f.key, a.key, 16);
    WriteXnAddr(f.xnaddr, a.guest_ip, a.game_port != 0 ? a.game_port : 1000);
    f.open_slots = a.open_slots;
    f.filled_slots = a.filled_slots;
    f.qos = a.qos;
    f.host_name = a.name;
    out.push_back(std::move(f));
  }
  if (hidden != g_hidden_mods) {
    REXLOG_INFO("[netplay] {} session(s) hidden: the host has different mods installed", hidden);
  }
  g_hidden_mods = hidden;
  return out;
}

void SendStats(uint64_t xuid, const std::vector<StatsView>& views) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_lobby.ip == 0 || views.empty()) return;
  std::vector<uint8_t> out = LobbyHeader(kStats);
  const uint64_t self = realsteel::online::Xuid();
  Put32(out, uint32_t(self >> 32));
  Put32(out, uint32_t(self));
  const std::string name = PlayerName();
  out.push_back(uint8_t(name.size()));
  out.insert(out.end(), name.begin(), name.end());
  Put32(out, uint32_t(xuid >> 32));
  Put32(out, uint32_t(xuid));
  out.push_back(uint8_t(std::min<size_t>(views.size(), 8)));
  for (size_t v = 0; v < views.size() && v < 8; ++v) {
    Put32(out, views[v].id);
    const size_t count = std::min<size_t>(views[v].properties.size(), 32);
    out.push_back(uint8_t(count));
    for (size_t i = 0; i < count; ++i) {
      const StatsProperty& prop = views[v].properties[i];
      Put32(out, prop.id);
      out.push_back(prop.type);
      Put32(out, uint32_t(uint64_t(prop.value) >> 32));
      Put32(out, uint32_t(uint64_t(prop.value)));
    }
  }
  SendTo(g_lobby, out);
  REXLOG_INFO("[netplay] match results sent to the lobby server ({} view(s))", views.size());
}

bool ReadLeaderboard(uint32_t view, bool by_player, uint32_t first_rank, uint32_t rows, uint64_t xuid,
                     const std::vector<uint16_t>& columns, LeaderboardPage* page) {
  std::unique_lock<std::mutex> lock(g_mutex);
  if (g_lobby.ip == 0) return false;
  const uint32_t request = g_next_request++;
  std::vector<uint8_t> out = LobbyHeader(kRead);
  Put32(out, request);
  Put32(out, view);
  out.push_back(by_player ? 1 : 0);
  Put32(out, first_rank);
  Put16(out, uint16_t(std::min<uint32_t>(rows, 100)));
  Put32(out, uint32_t(xuid >> 32));
  Put32(out, uint32_t(xuid));
  out.push_back(uint8_t(std::min<size_t>(columns.size(), 32)));
  for (size_t i = 0; i < columns.size() && i < 32; ++i) Put16(out, columns[i]);
  for (int attempt = 0; attempt < 3; ++attempt) {
    SendTo(g_lobby, out);
    if (g_rows_cv.wait_for(lock, std::chrono::milliseconds(700), [&] { return g_rows.count(request) != 0; })) {
      *page = std::move(g_rows[request]);
      g_rows.erase(request);
      return true;
    }
  }
  return false;
}

std::string ModsNotice() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_mods_notice.empty() ||
      std::chrono::steady_clock::now() - g_mods_notice_time > std::chrono::minutes(3)) {
    return std::string();
  }
  return g_mods_notice;
}

std::string LobbyStatus() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_hidden_mods != 0) {
    return g_lobby_status + " (" + std::to_string(g_hidden_mods) + " hidden: different mods)";
  }
  return g_lobby_status;
}

void SetModsSignature(const std::string& signature) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_mods_signature = signature;
}

std::string OnlineName() { return PlayerName(); }

uint16_t ServicePort() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_real_port != 0 ? g_real_port : uint16_t(REXCVAR_GET(realsteel_net_port));
}

bool QosDataFor(uint32_t guest_ip, std::vector<uint8_t>* qos) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto it = g_adverts.find(guest_ip);
  if (it == g_adverts.end()) return false;
  *qos = it->second.qos;
  return true;
}

}  // namespace realsteel::netplay

using namespace realsteel::netplay;

// XNetGetTitleXnAddr(XNADDR* r3): our own address. Flags: static address,
// gateway, DNS, online.
extern "C" REX_FUNC(sub_822B7BD0) {
  const uint32_t out = ctx.r3.u32;
  if (!realsteel::online::Enabled() || out == 0) {
    __imp__sub_822B7BD0(ctx, base);
    return;
  }
  WriteSelfXnAddr(base + out);
  ctx.r3.u64 = 0x04 | 0x20 | 0x40 | 0x80;
}

// XNetXnAddrToInAddr(XNADDR* r3, XNKID* r4, IN_ADDR* r5): the guest address
// travels in the XNADDR itself.
extern "C" REX_FUNC(sub_822B7AE0) {
  const uint32_t xnaddr = ctx.r3.u32, out = ctx.r5.u32;
  if (!realsteel::online::Enabled() || xnaddr == 0 || out == 0) {
    __imp__sub_822B7AE0(ctx, base);
    return;
  }
  std::memcpy(base + out, base + xnaddr, 4);
  if (LogSome()) {
    const uint8_t* p = base + xnaddr;
    REXLOG_INFO("[netplay] XnAddrToInAddr {}.{}.{}.{}", p[0], p[1], p[2], p[3]);
  }
  ctx.r3.u64 = 0;
}

// XNetInAddrToXnAddr(IN_ADDR r3, XNADDR* r4, XNKID* r5).
extern "C" REX_FUNC(sub_822B7AF8) {
  const uint32_t ina = ctx.r3.u32, out = ctx.r4.u32, xnkid = ctx.r5.u32;
  if (!realsteel::online::Enabled() || out == 0) {
    __imp__sub_822B7AF8(ctx, base);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    WriteXnAddr(base + out, ina, g_game_port != 0 ? g_game_port : 1000);
    if (xnkid != 0) std::memcpy(base + xnkid, g_host_session, 8);
  }
  if (LogSome()) REXLOG_INFO("[netplay] InAddrToXnAddr {:#010x}", ina);
  ctx.r3.u64 = 0;
}

// socket(af r3, type r4, protocol r5).
extern "C" REX_FUNC(sub_82333558) {
  if (!realsteel::online::Enabled()) {
    __imp__sub_82333558(ctx, base);
    return;
  }
  EnsureStarted();
  std::lock_guard<std::mutex> lock(g_mutex);
  uint32_t handle = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < 8; ++i) {
    if (!g_guest_sockets[i].open) {
      g_guest_sockets[i] = {true, 0};
      handle = kHandleBase + i;
      break;
    }
  }
  if (LogSome()) {
    REXLOG_INFO("[netplay] socket(af {}, type {}, protocol {}) -> {:#x}", ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32, handle);
  }
  ctx.r3.u64 = handle;
}

// closesocket(s r3).
extern "C" REX_FUNC(sub_82333570) {
  if (!realsteel::online::Enabled()) {
    __imp__sub_82333570(ctx, base);
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  if (GuestSocket* s = FindSocket(ctx.r3.u32)) *s = GuestSocket{};
  ctx.r3.u64 = 0;
}

// ioctlsocket(s r3, cmd r4, arg* r5): only FIONBIO matters, and our sockets
// never block anyway.
extern "C" REX_FUNC(sub_82333580) {
  if (!realsteel::online::Enabled()) {
    __imp__sub_82333580(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
}

// setsockopt(s r3, level r4, name r5, value* r6, size r7).
extern "C" REX_FUNC(sub_82333598) {
  if (!realsteel::online::Enabled()) {
    __imp__sub_82333598(ctx, base);
    return;
  }
  ctx.r3.u64 = 0;
}

// bind(s r3, sockaddr_in* r4, size r5): family u16, port u16, address u32.
extern "C" REX_FUNC(sub_823335B8) {
  if (!realsteel::online::Enabled()) {
    __imp__sub_823335B8(ctx, base);
    return;
  }
  const uint32_t addr = ctx.r4.u32;
  std::lock_guard<std::mutex> lock(g_mutex);
  GuestSocket* s = FindSocket(ctx.r3.u32);
  if (s == nullptr || addr == 0) {
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  s->port = Get16(base + addr + 2);
  g_game_port = s->port;
  if (LogSome()) REXLOG_INFO("[netplay] bind({:#x}) to port {}", ctx.r3.u32, s->port);
  ctx.r3.u64 = 0;
}

// select(nfds r3, readfds r4, writefds r5, exceptfds r6, timeout r7).
// fd_set: count u32, then up to 64 handles (u32).
extern "C" REX_FUNC(sub_823335D0) {
  if (!realsteel::online::Enabled()) {
    __imp__sub_823335D0(ctx, base);
    return;
  }
  const uint32_t readfds = ctx.r4.u32, writefds = ctx.r5.u32, exceptfds = ctx.r6.u32;
  uint32_t ready = 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (readfds != 0) {
    const uint32_t count = std::min(64u, REX_LOAD_U32(readfds));
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t handle = REX_LOAD_U32(readfds + 4 + i * 4);
      const GuestSocket* s = FindSocket(handle);
      bool has_data = false;
      for (const Datagram& d : g_inbox) {
        has_data = has_data || (s != nullptr && (s->port == 0 || d.to_port == s->port));
      }
      if (has_data) REX_STORE_U32(readfds + 4 + kept++ * 4, handle);
    }
    REX_STORE_U32(readfds, kept);
    ready += kept;
  }
  if (writefds != 0) ready += std::min(64u, REX_LOAD_U32(writefds));  // always writable
  if (exceptfds != 0) REX_STORE_U32(exceptfds, 0u);
  ctx.r3.u64 = ready;
}

// recvfrom(s r3, buffer r4, size r5, flags r6, from* r7, fromlen* r8).
extern "C" REX_FUNC(sub_823335F0) {
  if (!realsteel::online::Enabled()) {
    __imp__sub_823335F0(ctx, base);
    return;
  }
  const uint32_t buffer = ctx.r4.u32, size = ctx.r5.u32, from = ctx.r7.u32,
                 from_len = ctx.r8.u32;
  std::lock_guard<std::mutex> lock(g_mutex);
  const GuestSocket* s = FindSocket(ctx.r3.u32);
  if (s == nullptr || buffer == 0) {
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  for (auto it = g_inbox.begin(); it != g_inbox.end(); ++it) {
    if (s->port != 0 && it->to_port != s->port) continue;
    const uint32_t n = std::min<uint32_t>(size, uint32_t(it->payload.size()));
    std::memcpy(base + buffer, it->payload.data(), n);
    if (from != 0) {
      uint8_t* a = base + from;
      std::memset(a, 0, 16);
      a[0] = 0;
      a[1] = 2;  // AF_INET
      a[2] = uint8_t(it->from_port >> 8);
      a[3] = uint8_t(it->from_port);
      for (int i = 0; i < 4; ++i) a[4 + i] = uint8_t(it->from_ip >> (24 - 8 * i));
      if (from_len != 0) REX_STORE_U32(from_len, 16u);
    }
    static std::atomic<uint32_t> logs{0};
    if (logs.fetch_add(1) < 12) {
      REXLOG_INFO("[netplay] recvfrom: {} bytes from {:#010x}:{}", n, it->from_ip,
                  it->from_port);
    }
    g_inbox.erase(it);
    ctx.r3.u64 = n;
    return;
  }
  ctx.r3.u64 = 0xFFFFFFFFu;  // nothing waiting (would block)
}

// sendto(s r3, buffer r4, size r5, flags r6, to* r7, tolen r8).
extern "C" REX_FUNC(sub_82333610) {
  if (!realsteel::online::Enabled()) {
    __imp__sub_82333610(ctx, base);
    return;
  }
  const uint32_t buffer = ctx.r4.u32, size = ctx.r5.u32, to = ctx.r7.u32;
  std::lock_guard<std::mutex> lock(g_mutex);
  const GuestSocket* s = FindSocket(ctx.r3.u32);
  if (s == nullptr || buffer == 0 || to == 0 || size > 3000) {
    ctx.r3.u64 = 0xFFFFFFFFu;
    return;
  }
  const uint16_t to_port = Get16(base + to + 2);
  const uint32_t to_ip = Get32(base + to + 4);
  const auto peer = g_peers.find(to_ip);
  {
    // Traffic summary every 5 seconds.
    static uint64_t sent = 0, sent_bytes = 0, dropped = 0, biggest = 0;
    static auto last = std::chrono::steady_clock::now();
    ++sent;
    sent_bytes += size;
    biggest = std::max<uint64_t>(biggest, size);
    if (peer == g_peers.end()) ++dropped;
    const auto now = std::chrono::steady_clock::now();
    if (now - last > std::chrono::seconds(5)) {
      last = now;
      REXLOG_INFO("[netplay] 5 s: {} packets / {} bytes sent (largest {}, {} to unknown "
                  "addresses), {} waiting to be read",
                  sent, sent_bytes, biggest, dropped, g_inbox.size());
      sent = sent_bytes = dropped = biggest = 0;
    }
  }
  static std::atomic<uint32_t> logs{0};
  if (logs.fetch_add(1) < 12) {
    REXLOG_INFO("[netplay] sendto: {} bytes to {:#010x}:{} ({})", size, to_ip, to_port,
                peer != g_peers.end() ? "known peer" : "unknown address, dropped");
  }
  if (peer != g_peers.end()) {
    std::vector<uint8_t> packet = Header(kData);
    Put16(packet, s->port);
    Put16(packet, to_port);
    packet.insert(packet.end(), base + buffer, base + buffer + size);
    SendTo(peer->second, packet);
  }
  ctx.r3.u64 = size;
}
