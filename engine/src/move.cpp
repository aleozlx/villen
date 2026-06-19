#include "villen/chess/move.hpp"

namespace villen::chess {

std::string moveToString(const Move& m) {
    std::string out = squareToString(m.from) + squareToString(m.to);
    if (m.promotion != PieceType::None) {
        // Long-algebraic uses the lowercase piece letter for the promotion.
        out.push_back(pieceToChar(Piece{Color::Black, m.promotion}));
    }
    return out;
}

}  // namespace villen::chess
