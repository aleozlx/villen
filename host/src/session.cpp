#include "session.hpp"

#include <cstdio>

namespace villen {

void GameServer::onOpen(ConnId id) {
    // A new player connection gets the current authoritative state immediately.
    sendState(id);
}

void GameServer::onMessage(ConnId id, std::string_view text) {
    proto::Incoming in = proto::parse(text);
    if (!in.ok) {
        ws_.send(id, proto::reject("bad_message"));
        return;
    }

    if (in.type == "join") {
        // Seat claiming arrives in step 5; for now just (re)send state.
        sendState(id);
    } else if (in.type == "proposeMove") {
        if (!in.move) {
            ws_.send(id, proto::reject("bad_message"));
            return;
        }
        // The engine is the single authority: apply() rejects illegal and
        // out-of-turn moves (those aren't in legalMoves), leaving state intact.
        if (position_.apply(*in.move)) {
            broadcastState();
        } else {
            ws_.send(id, proto::reject("illegal_move", in.move));
        }
    } else if (in.type == "ping") {
        ws_.send(id, R"({"type":"pong"})");
    } else {
        ws_.send(id, proto::reject("unknown_type"));
    }
}

void GameServer::onClose(ConnId /*id*/) {
    // Seat release + sessionUpdate broadcast arrive in step 5.
}

void GameServer::reset() {
    position_ = chess::Position::initial();
    broadcastState();
}

void GameServer::sendState(ConnId id) {
    ws_.send(id, proto::state(session_, position_, seatStatus()));
}

void GameServer::broadcastState() {
    ws_.broadcast(proto::state(session_, position_, seatStatus()));
}

}  // namespace villen
