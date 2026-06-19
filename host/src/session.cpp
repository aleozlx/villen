#include "session.hpp"

namespace villen {

void GameServer::onOpen(ConnId id) {
    // A new connection gets the current authoritative state immediately; it
    // claims a seat explicitly via a "join" message.
    sendState(id);
}

void GameServer::onMessage(ConnId id, std::string_view text) {
    proto::Incoming in = proto::parse(text);
    if (!in.ok) {
        ws_.send(id, proto::reject("bad_message"));
        return;
    }

    if (in.type == "join") {
        handleJoin(id, in.seat);
    } else if (in.type == "proposeMove") {
        handleProposeMove(id, in);
    } else if (in.type == "ping") {
        ws_.send(id, R"({"type":"pong"})");
    } else {
        ws_.send(id, proto::reject("unknown_type"));
    }
}

void GameServer::handleJoin(ConnId id, const std::string& requested) {
    // Already seated? Re-confirm without disturbing anything.
    std::string current = seatOf(id);
    if (!current.empty()) {
        ws_.send(id, proto::joined(session_, current));
        sendState(id);
        return;
    }

    auto take = [&](Seat& seat, const char* name) {
        seat.conn = id;
        seat.held = false;  // clears any "disconnected" hold on reclaim
        ws_.send(id, proto::joined(session_, name));
        sendState(id);
        broadcastSessionUpdate();
    };
    // An open seat is always takeable. A held (disconnected) seat is takeable
    // only by an *explicit* request for it by name — the token-free reconnect
    // path (DESIGN §13 #1): the seat name is public routing, not a credential, so
    // a player rejoins by re-asking for their seat. Auto-assign never reclaims a
    // held seat, so a fresh walk-up doesn't land on someone's reserved side.
    auto takeable = [](const Seat& s, bool byName) {
        return s.conn == 0 && (!s.held || byName);
    };

    if (requested == "white" && takeable(white_, true)) { take(white_, "white"); return; }
    if (requested == "black" && takeable(black_, true)) { take(black_, "black"); return; }
    if (requested.empty()) {  // auto-assign: first open seat, white before black
        if (takeable(white_, false)) { take(white_, "white"); return; }
        if (takeable(black_, false)) { take(black_, "black"); return; }
    }

    // Requested seat is live/held-by-someone, or the table is full: spectate.
    ws_.send(id, proto::joined(session_, "spectator"));
    sendState(id);
}

void GameServer::handleProposeMove(ConnId id, const proto::Incoming& in) {
    if (!in.move) {
        ws_.send(id, proto::reject("bad_message"));
        return;
    }

    // Authority is derived from the connection and the side to move — never from
    // the client-supplied seat (§9.3). You may move the side to move only if you
    // hold that seat, or it is fully open (the lone-player case, §7).
    bool whiteToMove = position_.sideToMove() == chess::Color::White;
    const Seat& seat = whiteToMove ? white_ : black_;
    if (seat.conn != 0 && seat.conn != id) {
        ws_.send(id, proto::reject("not_your_turn", in.move));
        return;
    }
    // A held (disconnected) seat is reserved: nobody — not even the opponent —
    // may move for an offline player until the seat is reclaimed or the admin
    // re-opens it (DESIGN §13 #1).
    if (seat.conn == 0 && seat.held) {
        ws_.send(id, proto::reject("seat_disconnected", in.move));
        return;
    }

    // The engine is the single authority on legality: apply() rejects illegal
    // and out-of-turn moves, leaving state untouched.
    if (position_.apply(*in.move)) {
        broadcastState();
    } else {
        ws_.send(id, proto::reject("illegal_move", in.move));
    }
}

void GameServer::onClose(ConnId id) {
    // Hold the seat across the disconnect instead of vacating it: the dropped
    // player keeps their side reserved (status "disconnected") so the opponent
    // can't seize it, until they reconnect or the admin re-opens it (§13 #1).
    bool changed = false;
    if (white_.conn == id) { white_.conn = 0; white_.held = true; changed = true; }
    if (black_.conn == id) { black_.conn = 0; black_.held = true; changed = true; }
    if (changed) broadcastSessionUpdate();
}

void GameServer::reset() {
    position_ = chess::Position::initial();
    broadcastState();
}

void GameServer::freeSeat(chess::Color c) {
    Seat& s = seatFor(c);
    s.conn = 0;
    s.held = false;
    broadcastSessionUpdate();
}

std::string GameServer::seatOf(ConnId id) const {
    if (id != 0 && id == white_.conn) return "white";
    if (id != 0 && id == black_.conn) return "black";
    return "";
}

void GameServer::sendState(ConnId id) {
    ws_.send(id, proto::state(session_, position_, seatStatus()));
}

void GameServer::broadcastState() {
    ws_.broadcast(proto::state(session_, position_, seatStatus()));
}

void GameServer::broadcastSessionUpdate() {
    ws_.broadcast(proto::sessionUpdate(seatStatus()));
}

}  // namespace villen
