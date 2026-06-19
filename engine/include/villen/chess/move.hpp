// Villen chess engine — the move value.
//
// A Move is plain data: where a piece came from, where it goes, and (only for a
// pawn reaching the back rank) what it promotes to. Crucially it carries *no*
// trace of how it was produced — a mouse click and a gamepad confirm yield byte-
// identical Moves (DESIGN-villen.md §6.2, §7). The engine never asks who made it.
#pragma once

#include <string>

#include "types.hpp"

namespace villen::chess {

struct Move {
    Square from{NO_SQUARE};
    Square to{NO_SQUARE};
    PieceType promotion{PieceType::None};  // None unless a pawn promotion.
};

inline bool operator==(const Move& a, const Move& b) {
    return a.from == b.from && a.to == b.to && a.promotion == b.promotion;
}
inline bool operator!=(const Move& a, const Move& b) { return !(a == b); }

// "e2e4" / "e7e8q" style long-algebraic. Useful for tests and logging; the wire
// protocol uses structured JSON instead (DESIGN-villen.md §6).
std::string moveToString(const Move& m);

}  // namespace villen::chess
