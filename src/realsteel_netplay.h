// realsteel - the network under the game's Xbox Live code; see
// realsteel_netplay.cpp.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace realsteel::netplay {

// Starts the service socket (and the lobby connection) early.
void Prepare();
// This game's 10.x.y.z guest address (host byte order).
uint32_t SelfGuestIp();
// Whether to answer session searches, and with which session.
void SetHosting(bool hosting, const uint8_t* session_id, const uint8_t* key);
// XNADDR (36 bytes) of this game.
void WriteSelfXnAddr(uint8_t* out);

struct FoundSession {
  uint8_t session_id[8];
  uint8_t key[16];
  uint8_t xnaddr[36];
  uint8_t open_slots, filled_slots;
  std::vector<uint8_t> qos;  // the host's probe-answer blob
  std::string host_name;
};
// Asks the LAN, this machine and the configured hosts, waits |wait_ms| and
// returns the sessions that answered.
std::vector<FoundSession> FindSessions(uint32_t wait_ms);
// The lobby server's state for the settings page ("connected, 3 matches
// listed", "cannot find ...", "not set").
std::string LobbyStatus();
// Why a match was hidden (or who could not see ours) and what to do about
// it; empty when nothing happened in the last minutes.
std::string ModsNotice();
// Names of the installed mod robots; players with different sets are told
// apart in the lobby by a hash of it.
void SetModsSignature(const std::string& signature);
// The name shown to other players.
std::string OnlineName();
// Leaderboards, kept by the lobby server (2026-10-09).
struct StatsProperty {
  uint32_t id = 0;
  uint8_t type = 0;      // XUSER_DATA type (1 LONG, 2 LONGLONG, 3 DOUBLE)
  int64_t value = 0;
};
struct StatsView {
  uint32_t id = 0;
  std::vector<StatsProperty> properties;
};
// The game's XSessionWriteStats for the local player: sent to the server,
// which adds them up per leaderboard.
void SendStats(uint64_t xuid, const std::vector<StatsView>& views);

struct LeaderboardColumn {
  uint16_t id = 0;
  uint8_t type = 0;
  int64_t value = 0;
};
struct LeaderboardRow {
  uint64_t xuid = 0;
  uint32_t rank = 0;
  int64_t rating = 0;
  std::string name;
  std::vector<LeaderboardColumn> columns;
};
struct LeaderboardPage {
  uint32_t view = 0, total = 0;
  std::vector<LeaderboardRow> rows;
};
// Asks the server for a page of a leaderboard (by rank, or around a
// player); waits up to about two seconds. False without a server / answer.
bool ReadLeaderboard(uint32_t view, bool by_player, uint32_t first_rank, uint32_t rows, uint64_t xuid,
                     const std::vector<uint16_t>& columns, LeaderboardPage* page);

// The UDP port this game listens on (the one to forward for internet play).
uint16_t ServicePort();
// The probe-answer blob last advertised by the host at |guest_ip|.
bool QosDataFor(uint32_t guest_ip, std::vector<uint8_t>* qos);

}  // namespace realsteel::netplay
