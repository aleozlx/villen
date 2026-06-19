// Villen host — the player wire contract (DESIGN §6), JSON over WebSocket.
//
// This is the only place that knows the on-the-wire message shapes. The engine
// and session layers speak in `chess::Move` / `chess::Position`; this translates
// to and from the JSON the browser client uses. Keeping all network-format
// concerns here is the §9.5 discipline — nothing else reaches into wire details.
#pragma once

#include <array>
#include <optional>
#include <string>
#include <string_view>

#include "villen/chess/move.hpp"
#include "villen/chess/position.hpp"

namespace villen::proto {

// Per-seat connection status, indexed [white, black]. e.g. "connected"/"open".
using SeatStatus = std::array<std::string, 2>;

// ---- Server -> client -------------------------------------------------------

// Full authoritative state (the only thing clients render from, §6.2). Includes
// legalMoves for the side to move as a pure UX convenience.
std::string state(const std::string& session, const chess::Position& pos,
                  const SeatStatus& seats);

// Rejection of an illegal/unauthorized proposal.
std::string reject(const std::string& reason,
                   const std::optional<chess::Move>& move = std::nullopt);

// Seat occupancy changed (player joined/left) without a board change.
std::string sessionUpdate(const SeatStatus& seats);

// ---- Client -> server -------------------------------------------------------

struct Incoming {
    bool ok = false;       // false if the JSON was malformed/unrecognized
    std::string type;      // "join" | "proposeMove" | "ping" | ...
    std::string session;   // target session (defaults to "default")
    std::string seat;      // "white" | "black" | "" (server may assign)
    std::optional<chess::Move> move;  // present for proposeMove
};

Incoming parse(std::string_view json);

}  // namespace villen::proto
