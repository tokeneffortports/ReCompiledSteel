// realsteel - online play through the game's own Xbox Live code (2026-10-05).
// See realsteel_online.cpp (sign-in, privileges, QoS) and
// realsteel_netplay.cpp (addresses, sockets, session discovery).

#pragma once

#include <cstdint>
#include <vector>

namespace realsteel::online {

// --realsteel_online=true
bool Enabled();

// Count of the game's sign-in state queries so far (any thread); the
// overlay watches its rate to know when the online menus are open.
void NoteSigninPoll();
uint64_t SigninPolls();
void NoteCall(int which);     // per-hook counters (diagnostics)
uint64_t Calls(int which);

// The host's probe-answer blob (XNetQosListen) and whether it is accepting
// probes; empty when not hosting.
bool HostQosData(std::vector<uint8_t>* data);

// This player's online identity (XUID), as the game sees it.
uint64_t Xuid();

}  // namespace realsteel::online
