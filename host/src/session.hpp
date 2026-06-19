// Villen host — authoritative session + seat state (DESIGN §2, §9.3).
//
// GameServer owns the one in-memory source of truth and is the sole authority:
// every proposed move is validated against the engine, and against seat
// ownership, before it changes anything (§3.2). It speaks to remote players only
// through the WsServer seam; the in-process admin UI (step 7) will read and
// mutate this same object directly, on the same thread, with no socket between
// them (§2, §5).
//
// Moves are attributed to a *seat*, not a connection (§9.3): the server derives
// the acting seat from the connection and the side to move, never trusting the
// client's claimed seat — the same way it never trusts client-side legality.
// A connection may move the side to move iff it owns that seat OR the seat is
// open; this enforces two-player turn order while still letting a lone player
// drive both sides (the §7 single-client case) until someone claims the other.
//
// The MVP runs a single hardcoded session named "default" (DESIGN step 2/11).
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "protocol.hpp"
#include "villen/chess/position.hpp"
#include "ws_server.hpp"

namespace villen {

class GameServer {
public:
    using ConnId = net::WsServer::ConnId;

    explicit GameServer(net::WsServer& ws) : ws_(ws) {}

    // WsServer callbacks (wired in main).
    void onOpen(ConnId id);
    void onMessage(ConnId id, std::string_view text);
    void onClose(ConnId id);

    // Read access for the admin UI (step 7).
    const std::string& sessionName() const { return session_; }
    const chess::Position& position() const { return position_; }
    bool whiteSeated() const { return whiteConn_ != 0; }
    bool blackSeated() const { return blackConn_ != 0; }

    // Admin action: start a fresh game (used by the ImGui panel later).
    void reset();

private:
    net::WsServer& ws_;
    std::string session_ = "default";
    chess::Position position_ = chess::Position::initial();
    ConnId whiteConn_ = 0;  // 0 == seat open
    ConnId blackConn_ = 0;

    proto::SeatStatus seatStatus() const {
        return {whiteConn_ ? "connected" : "open",
                blackConn_ ? "connected" : "open"};
    }
    // The seat this connection occupies: "white", "black", or "" (spectator).
    std::string seatOf(ConnId id) const;
    void handleJoin(ConnId id, const std::string& requested);
    void handleProposeMove(ConnId id, const proto::Incoming& in);

    void sendState(ConnId id);
    void broadcastState();
    void broadcastSessionUpdate();
};

}  // namespace villen
