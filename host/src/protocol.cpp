#include "protocol.hpp"

#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace villen::chess;

namespace villen::proto {
namespace {

const char* promoToString(PieceType pt) {
    switch (pt) {
        case PieceType::Queen: return "queen";
        case PieceType::Rook: return "rook";
        case PieceType::Bishop: return "bishop";
        case PieceType::Knight: return "knight";
        default: return nullptr;
    }
}

PieceType promoFromString(const std::string& s) {
    if (s == "queen" || s == "q") return PieceType::Queen;
    if (s == "rook" || s == "r") return PieceType::Rook;
    if (s == "bishop" || s == "b") return PieceType::Bishop;
    if (s == "knight" || s == "n") return PieceType::Knight;
    return PieceType::None;
}

}  // namespace

std::string state(const std::string& session, const Position& pos,
                  const SeatStatus& seats) {
    json legal = json::array();
    for (const Move& m : pos.legalMoves()) {
        json entry = {{"from", squareToString(m.from)},
                      {"to", squareToString(m.to)}};
        if (const char* p = promoToString(m.promotion)) entry["promotion"] = p;
        legal.push_back(std::move(entry));
    }

    json msg = {
        {"type", "state"},
        {"session", session},
        {"position", pos.toFen()},
        {"turn", pos.sideToMove() == Color::White ? "white" : "black"},
        {"legalMoves", std::move(legal)},
        {"status", wireStatusName(pos.status())},
        {"check", pos.inCheck()},
        {"seats", {{"white", seats[0]}, {"black", seats[1]}}},
    };
    return msg.dump();
}

std::string reject(const std::string& reason,
                   const std::optional<Move>& move) {
    json msg = {{"type", "reject"}, {"reason", reason}};
    if (move) {
        json m = {{"from", squareToString(move->from)},
                  {"to", squareToString(move->to)}};
        if (const char* p = promoToString(move->promotion)) m["promotion"] = p;
        msg["move"] = std::move(m);
    }
    return msg.dump();
}

std::string sessionUpdate(const SeatStatus& seats) {
    json msg = {{"type", "sessionUpdate"},
                {"seats", {{"white", seats[0]}, {"black", seats[1]}}}};
    return msg.dump();
}

std::string joined(const std::string& session, const std::string& seat) {
    json msg = {{"type", "joined"}, {"session", session}, {"seat", seat}};
    return msg.dump();
}

Incoming parse(std::string_view text) {
    Incoming in;
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object() || !j.contains("type")) return in;

    in.type = j.value("type", "");
    in.session = j.value("session", "default");
    in.seat = j.value("seat", "");

    if (in.type == "proposeMove" && j.contains("move") && j["move"].is_object()) {
        const auto& mj = j["move"];
        auto from = squareFromString(mj.value("from", ""));
        auto to = squareFromString(mj.value("to", ""));
        if (!from || !to) return in;  // ok stays false: malformed move
        Move m;
        m.from = *from;
        m.to = *to;
        if (mj.contains("promotion") && mj["promotion"].is_string())
            m.promotion = promoFromString(mj["promotion"].get<std::string>());
        in.move = m;
    }

    in.ok = true;
    return in;
}

}  // namespace villen::proto
