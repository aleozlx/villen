#include "chess_engine.hpp"

#include <nlohmann/json.hpp>
#include <optional>

#include "room.hpp"
#include "villen/chess/move.hpp"

#ifdef VILLEN_ADMIN_UI
#include "imgui.h"
#endif

using json = nlohmann::json;

namespace villen {
namespace {

// chess declares the roster {"white","black"}, so these indices are fixed.
constexpr SeatId kWhite = 0;
constexpr SeatId kBlack = 1;

const char* promoToString(chess::PieceType pt) {
    switch (pt) {
        case chess::PieceType::Queen: return "queen";
        case chess::PieceType::Rook: return "rook";
        case chess::PieceType::Bishop: return "bishop";
        case chess::PieceType::Knight: return "knight";
        default: return nullptr;
    }
}

chess::PieceType promoFromString(const std::string& s) {
    if (s == "queen" || s == "q") return chess::PieceType::Queen;
    if (s == "rook" || s == "r") return chess::PieceType::Rook;
    if (s == "bishop" || s == "b") return chess::PieceType::Bishop;
    if (s == "knight" || s == "n") return chess::PieceType::Knight;
    return chess::PieceType::None;
}

json moveJson(const chess::Move& m) {
    json mj = {{"from", chess::squareToString(m.from)},
               {"to", chess::squareToString(m.to)}};
    if (const char* p = promoToString(m.promotion)) mj["promotion"] = p;
    return mj;
}

// The full authoritative snapshot — the only thing clients render from (DESIGN
// §6.2). Seats come from Room (the membership authority), keyed by roster name so
// the wire shape stays {"white":..,"black":..}.
std::string stateMsg(const chess::Position& pos, const Room& room) {
    json legal = json::array();
    for (const chess::Move& m : pos.legalMoves()) legal.push_back(moveJson(m));

    json seats = json::object();
    for (const std::string& name : room.roster().names)
        seats[name] = room.seatStatus(room.seatIndex(name));

    json msg = {
        {"type", "state"},
        {"position", pos.toFen()},
        {"turn", pos.sideToMove() == chess::Color::White ? "white" : "black"},
        {"legalMoves", std::move(legal)},
        {"status", chess::wireStatusName(pos.status())},
        {"check", pos.inCheck()},
        {"seats", std::move(seats)},
    };
    return msg.dump();
}

std::string rejectMsg(const std::string& reason,
                      const std::optional<chess::Move>& move = std::nullopt) {
    json msg = {{"type", "reject"}, {"reason", reason}};
    if (move) msg["move"] = moveJson(*move);
    return msg.dump();
}

}  // namespace

SeatRoster ChessEngine::seats() const { return {{"white", "black"}}; }

void ChessEngine::onJoin(Room& room, ConnId id, SeatId) {
    // A new (or re-confirming) member gets the current authoritative state.
    room.send(id, stateMsg(position_, room));
}

void ChessEngine::onLeave(Room&, ConnId, SeatId) {
    // Membership is Room's to track; chess keeps no per-connection state.
}

void ChessEngine::onMessage(Room& room, ConnId conn, SeatId, std::string_view text) {
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("type")) {
        room.send(conn, rejectMsg("bad_message"));
        return;
    }
    if (j.value("type", "") != "proposeMove") {
        room.send(conn, rejectMsg("unknown_type"));
        return;
    }

    // Parse the proposed move out of the opaque payload.
    if (!j.contains("move") || !j["move"].is_object()) {
        room.send(conn, rejectMsg("bad_message"));
        return;
    }
    const auto& mj = j["move"];
    auto from = chess::squareFromString(mj.value("from", ""));
    auto to = chess::squareFromString(mj.value("to", ""));
    if (!from || !to) {
        room.send(conn, rejectMsg("bad_message"));
        return;
    }
    chess::Move move;
    move.from = *from;
    move.to = *to;
    if (mj.contains("promotion") && mj["promotion"].is_string())
        move.promotion = promoFromString(mj["promotion"].get<std::string>());

    // Authority is derived from the connection and the side to move — never from
    // a client-supplied seat (DESIGN §9.3). The acting seat is the side to move;
    // you may move it only if you hold that seat, or it is fully open (the
    // lone-player case).
    SeatId turnSeat = position_.sideToMove() == chess::Color::White ? kWhite : kBlack;
    ConnId owner = room.connOfSeat(turnSeat);
    const char* status = room.seatStatus(turnSeat);

    if (owner != 0 && owner != conn) {
        room.send(conn, rejectMsg("not_your_turn", move));
        return;
    }
    // A held (disconnected) seat is reserved: nobody — not even the opponent — may
    // move for an offline player until it's reclaimed or admin-freed (DESIGN §13).
    if (owner == 0 && std::string(status) == "disconnected") {
        room.send(conn, rejectMsg("seat_disconnected", move));
        return;
    }

    // The engine is the single authority on legality.
    if (position_.apply(move))
        broadcastState(room);
    else
        room.send(conn, rejectMsg("illegal_move", move));
}

void ChessEngine::broadcastState(Room& room) {
    room.broadcast(stateMsg(position_, room));
}

std::string ChessEngine::statusLine() const {
    const char* turn =
        position_.sideToMove() == chess::Color::White ? "white" : "black";
    switch (position_.status()) {
        case chess::Status::Checkmate: return std::string("checkmate — ") + turn + " is mated";
        case chess::Status::Stalemate: return "stalemate — draw";
        case chess::Status::Draw: return "draw";
        default: return std::string(turn) + " to move";
    }
}

void ChessEngine::reset() {
    position_ = chess::Position::initial();
    if (room_) broadcastState(*room_);
}

void ChessEngine::drawAdmin() {
#ifdef VILLEN_ADMIN_UI
    // Only the engine's own body — the shell draws the chrome, roster, and join
    // QR (admin-shell §8). Default ImGui font is ASCII-only, so keep text ASCII.
    ImGui::TextUnformatted(statusLine().c_str());
    ImGui::Spacing();
    if (ImGui::Button("New game")) reset();
#endif
}

}  // namespace villen
