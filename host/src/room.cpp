#include "room.hpp"

#include "envelope.hpp"

namespace villen {

namespace {
const std::string kEmptyName;  // returned for kNoSeat
}

Room::Room(net::WsServer& ws, IEngine& engine, SeatRoster roster)
    : ws_(ws), engine_(engine), roster_(std::move(roster)) {
    seats_.resize(roster_.names.size());
}

// --- transport handle --------------------------------------------------------

void Room::send(ConnId id, std::string_view bytes, Delivery) {
    // Delivery is the future transport seam (framework §5.3); today's WebSocket is
    // always reliable, so the hint is accepted and ignored.
    ws_.send(id, bytes);
}

void Room::broadcast(std::string_view bytes, Delivery) { ws_.broadcast(bytes); }

void Room::sendBinary(ConnId id, std::string_view bytes, Delivery) {
    ws_.sendBinary(id, bytes);  // per-connection only, never broadcast (§10.1)
}

SeatId Room::seatOf(ConnId id) const {
    if (id == 0) return kNoSeat;
    for (SeatId s = 0; s < static_cast<SeatId>(seats_.size()); ++s)
        if (seats_[s].conn == id) return s;
    return kNoSeat;
}

ConnId Room::connOfSeat(SeatId s) const {
    if (s < 0 || s >= static_cast<SeatId>(seats_.size())) return 0;
    return seats_[s].conn;
}

SeatId Room::seatIndex(std::string_view name) const {
    for (SeatId s = 0; s < static_cast<SeatId>(roster_.names.size()); ++s)
        if (roster_.names[s] == name) return s;
    return kNoSeat;
}

const std::string& Room::seatName(SeatId s) const {
    if (s < 0 || s >= static_cast<SeatId>(roster_.names.size())) return kEmptyName;
    return roster_.names[s];
}

const char* Room::seatStatus(SeatId s) const {
    if (s < 0 || s >= static_cast<SeatId>(seats_.size())) return "open";
    const Seat& seat = seats_[s];
    return seat.conn ? "connected" : (seat.held ? "disconnected" : "open");
}

// --- lifecycle ---------------------------------------------------------------

void Room::onOpen(ConnId) {
    // Nothing to do until the connection claims a seat: the host has already
    // announced the active engine, and the client joins immediately, at which
    // point the engine pushes its state (onJoin). A pre-join connection is just
    // an open socket with no membership yet.
}

void Room::onMessage(ConnId id, std::string_view text) {
    envelope::Incoming in = envelope::parse(text);
    if (in.ok && in.type == "join") {
        handleJoin(id, in.seat);
        return;
    }
    if (in.ok && in.type == "ping") {
        ws_.send(id, R"({"type":"pong"})");
        return;
    }
    // Everything else — including text that didn't parse as an envelope — is an
    // opaque engine payload. Villen does not judge it; the engine parses and may
    // reject it. The acting seat is derived here, never trusted from the client.
    engine_.onMessage(*this, id, seatOf(id), text);
}

void Room::onBinary(ConnId id, std::string_view bytes) {
    // Binary frames are pure media — never an envelope — so they go straight to
    // the engine with the connection's derived seat (DESIGN-filter §5.1). A feed
    // need not be seated (filter has no seats, §7); seatOf is kNoSeat then.
    engine_.onBinary(*this, id, seatOf(id), bytes);
}

void Room::handleJoin(ConnId id, const std::string& requested) {
    // Already seated? Re-confirm and let the engine re-push state, disturbing
    // nothing else.
    SeatId current = seatOf(id);
    if (current != kNoSeat) {
        ws_.send(id, envelope::joined(session_, roster_.names[current]));
        engine_.onJoin(*this, id, current);
        return;
    }

    auto takeable = [&](SeatId s, bool byName) {
        // An open seat is always takeable. A held (disconnected) seat is takeable
        // only by an explicit by-name request — the token-free reconnect path
        // (DESIGN §13 #1): the seat name is public routing, not a credential.
        return seats_[s].conn == 0 && (!seats_[s].held || byName);
    };
    auto take = [&](SeatId s) {
        seats_[s].conn = id;
        seats_[s].held = false;  // clears any disconnected hold on reclaim
        ws_.send(id, envelope::joined(session_, roster_.names[s]));
        engine_.onJoin(*this, id, s);
        broadcastSeats();
    };

    SeatId req = seatIndex(requested);
    if (req != kNoSeat && takeable(req, true)) {
        take(req);
        return;
    }
    if (requested.empty()) {  // auto-assign: first open seat in roster order
        for (SeatId s = 0; s < static_cast<SeatId>(seats_.size()); ++s)
            if (takeable(s, false)) {
                take(s);
                return;
            }
    }

    // Requested seat is live/held-by-someone, or the table is full: spectate.
    ws_.send(id, envelope::joined(session_, "spectator"));
    engine_.onJoin(*this, id, kNoSeat);
}

void Room::onClose(ConnId id) {
    // Hold the seat across the disconnect rather than vacating it: the dropped
    // player keeps their side reserved (status "disconnected") so the opponent
    // can't seize it, until they reconnect or the admin re-opens it (DESIGN §13).
    bool changed = false;
    for (SeatId s = 0; s < static_cast<SeatId>(seats_.size()); ++s) {
        if (seats_[s].conn == id) {
            seats_[s].conn = 0;
            seats_[s].held = true;
            engine_.onLeave(*this, id, s);
            changed = true;
        }
    }
    if (changed) broadcastSeats();
}

void Room::freeSeat(SeatId s) {
    if (s < 0 || s >= static_cast<SeatId>(seats_.size())) return;
    // If a live player still holds this seat (admin override, not the usual
    // re-open of a disconnected seat), tell them they're now a spectator so their
    // client clears its remembered seat instead of desyncing. A held/disconnected
    // seat has no connection to notify.
    ConnId kicked = seats_[s].conn;
    seats_[s].conn = 0;
    seats_[s].held = false;
    if (kicked != 0) {
        // The kicked player is still connected, now a spectator. Notify the engine
        // it left the seat (so engine state stays in sync with membership), then
        // tell the client so it clears its remembered seat instead of desyncing.
        engine_.onLeave(*this, kicked, s);
        ws_.send(kicked, envelope::joined(session_, "spectator"));
    }
    broadcastSeats();
}

void Room::broadcastSeats() {
    std::vector<std::pair<std::string, std::string>> status;
    status.reserve(seats_.size());
    for (SeatId s = 0; s < static_cast<SeatId>(seats_.size()); ++s)
        status.emplace_back(roster_.names[s], seatStatus(s));
    ws_.broadcast(envelope::sessionUpdate(status));
}

}  // namespace villen
