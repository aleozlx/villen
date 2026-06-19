// Villen host — authoritative session state (DESIGN §2, §9).
//
// GameServer owns the one in-memory source of truth for the game and is the sole
// authority: every proposed move is validated against the engine before it
// changes anything (§3.2). It speaks to remote players only through the WsServer
// seam; the in-process admin UI (step 7) will read and mutate this same object
// directly, on the same thread, with no socket between them (§2, §5).
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

    // Admin action: start a fresh game (used by the ImGui panel later).
    void reset();

private:
    net::WsServer& ws_;
    std::string session_ = "default";
    chess::Position position_ = chess::Position::initial();

    // Step 2 has no seat ownership yet — both report open. Step 5 makes this real.
    proto::SeatStatus seatStatus() const { return {"open", "open"}; }

    void sendState(ConnId id);
    void broadcastState();
};

}  // namespace villen
