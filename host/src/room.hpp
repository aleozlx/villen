// Villen — Room: the authoritative membership for one active engine (DESIGN §13,
// DESIGN-game-framework §3/§4).
//
// Room owns the seat lifecycle — open / connected / disconnected-held, token-free
// reconnect-by-name, admin Free — generalized from chess's two hardcoded seats to
// the N named seats an engine declares in its SeatRoster. It is the reusable
// crown jewel: a game gets rooms, seats, and serving without writing any of it.
//
// Room never interprets a gameplay payload. It parses only the join/seat envelope
// (envelope.hpp); every other message is relayed verbatim to IEngine::onMessage.
// Authority over seats lives here; authority over *moves* lives in the engine,
// which derives the acting seat from Room::seatOf — never from a client's claim
// (DESIGN §9.3).
//
// One process, one thread: Room is driven from the host's single poll loop and
// the in-process admin UI mutates it directly, with no locks (DESIGN §5).
#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "engine.hpp"
#include "ws_server.hpp"

namespace villen {

class Room {
 public:
    // Built by the host from the engine's declared roster, right after the engine
    // is constructed and before any connection is routed in.
    Room(net::WsServer& ws, IEngine& engine, SeatRoster roster);

    // --- driven by the host's ws callbacks (one thread, DESIGN §5) ---
    void onOpen(ConnId);
    void onMessage(ConnId, std::string_view);
    void onClose(ConnId);

    // --- engine-facing handle (passed as Room& to IEngine methods) ---
    void send(ConnId, std::string_view bytes, Delivery = Delivery::Reliable);
    void broadcast(std::string_view bytes, Delivery = Delivery::Reliable);
    SeatId seatOf(ConnId) const;          // kNoSeat if spectator/unknown
    ConnId connOfSeat(SeatId) const;      // 0 if the seat is empty
    SeatId seatIndex(std::string_view name) const;  // kNoSeat if no such seat
    const SeatRoster& roster() const { return roster_; }
    const std::string& seatName(SeatId) const;      // roster name; "" for kNoSeat
    const char* seatStatus(SeatId) const;           // "connected"|"disconnected"|"open"
    std::size_t connectionCount() const { return ws_.connectionCount(); }
    const std::string& session() const { return session_; }

    // --- admin-facing (DESIGN §13 #1): re-open a seat so someone can rejoin ---
    void freeSeat(SeatId);

 private:
    // A seat is owned by a connection but outlives it: when a seated player drops
    // we keep the seat reserved ("held") so the opponent can't seize it and the
    // player can reconnect into it by name.
    //   open         : conn == 0 && !held
    //   connected    : conn != 0
    //   disconnected : conn == 0 &&  held
    struct Seat {
        ConnId conn = 0;
        bool held = false;
    };

    net::WsServer& ws_;
    IEngine& engine_;
    SeatRoster roster_;
    std::vector<Seat> seats_;
    std::string session_ = "default";

    // Every connection the engine has seen onJoin for (seated or spectator), so
    // onClose can fire a matching onLeave for unseated members too. Per-connection
    // engines (chat, filter) keep state keyed by ConnId, not by seat, and need the
    // departure signal to release it (privacy, DESIGN-chat §11); seat-based engines
    // (chess) simply ignore a kNoSeat onLeave.
    std::unordered_set<ConnId> members_;

    void handleJoin(ConnId, const std::string& requested);
    void broadcastSeats();  // the sessionUpdate envelope
};

}  // namespace villen
