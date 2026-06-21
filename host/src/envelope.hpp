// Villen — the join/seat envelope: the ONLY wire format Villen itself parses
// (DESIGN-game-framework §4). Everything past a seated `join` is an opaque engine
// payload Villen relays verbatim. This module knows seats and rooms; it knows no
// game (no chess move, no FEN) — those live in the engine's own protocol.
#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace villen::envelope {

// ---- Server -> client -------------------------------------------------------

// Acknowledge a join, naming the seat the conn now holds (a roster name, or
// "spectator") so the client can attribute its own actions.
std::string joined(const std::string& session, const std::string& seat);

// Seat occupancy changed (someone joined/left/was freed) without a game change.
// `seats` is roster-ordered (name, status) where status is
// "connected" | "disconnected" | "open".
std::string sessionUpdate(const std::vector<std::pair<std::string, std::string>>& seats);

// Announce the active engine to a client (admin-shell §5), so the page can load
// the matching view. An empty name means "launcher / no engine running" (null).
std::string engineAnnounce(std::string_view name);

// Envelope-level rejection (malformed join, etc.). Game-specific rejections
// (illegal move) are the engine's own protocol.
std::string reject(std::string_view reason);

// ---- Client -> server -------------------------------------------------------

struct Incoming {
    bool ok = false;      // false if the text wasn't a JSON object with a "type"
    std::string type;     // "join" | "ping" | ... (or an engine payload type)
    std::string session;  // target session (defaults to "default")
    std::string seat;     // requested seat name, "" = auto-assign
};

// Parse only the envelope fields. A non-envelope message still parses ok with its
// own `type`; the caller routes join/ping itself and relays everything else (and
// anything that fails to parse) to the engine untouched.
Incoming parse(std::string_view json);

}  // namespace villen::envelope
