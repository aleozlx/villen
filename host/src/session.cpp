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

    auto take = [&](ConnId& seat, const char* name) {
        seat = id;
        ws_.send(id, proto::joined(session_, name));
        sendState(id);
        broadcastSessionUpdate();
    };

    if (requested == "white" && whiteConn_ == 0) { take(whiteConn_, "white"); return; }
    if (requested == "black" && blackConn_ == 0) { take(blackConn_, "black"); return; }
    if (requested.empty()) {  // auto-assign: white first, then black
        if (whiteConn_ == 0) { take(whiteConn_, "white"); return; }
        if (blackConn_ == 0) { take(blackConn_, "black"); return; }
    }

    // Requested seat taken, or the table is full: join as a spectator.
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
    // own that seat or it is open.
    bool whiteToMove = position_.sideToMove() == chess::Color::White;
    ConnId owner = whiteToMove ? whiteConn_ : blackConn_;
    if (owner != 0 && owner != id) {
        ws_.send(id, proto::reject("not_your_turn", in.move));
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
    bool freed = false;
    if (whiteConn_ == id) { whiteConn_ = 0; freed = true; }
    if (blackConn_ == id) { blackConn_ = 0; freed = true; }
    if (freed) broadcastSessionUpdate();
}

void GameServer::reset() {
    position_ = chess::Position::initial();
    broadcastState();
}

std::string GameServer::seatOf(ConnId id) const {
    if (id != 0 && id == whiteConn_) return "white";
    if (id != 0 && id == blackConn_) return "black";
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
