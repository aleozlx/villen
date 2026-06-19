#include "villen/chess/types.hpp"

#include <cctype>

namespace villen::chess {

std::string squareToString(Square s) {
    if (s < 0 || s >= 64) return "-";
    std::string out;
    out.push_back(static_cast<char>('a' + fileOf(s)));
    out.push_back(static_cast<char>('1' + rankOf(s)));
    return out;
}

std::optional<Square> squareFromString(const std::string& s) {
    if (s.size() != 2) return std::nullopt;
    int file = s[0] - 'a';
    int rank = s[1] - '1';
    if (!onBoard(file, rank)) return std::nullopt;
    return makeSquare(file, rank);
}

char pieceToChar(Piece p) {
    char c;
    switch (p.type) {
        case PieceType::Pawn: c = 'p'; break;
        case PieceType::Knight: c = 'n'; break;
        case PieceType::Bishop: c = 'b'; break;
        case PieceType::Rook: c = 'r'; break;
        case PieceType::Queen: c = 'q'; break;
        case PieceType::King: c = 'k'; break;
        default: return '.';
    }
    return p.color == Color::White ? static_cast<char>(std::toupper(c)) : c;
}

std::optional<Piece> pieceFromChar(char c) {
    Color color = std::isupper(static_cast<unsigned char>(c)) ? Color::White : Color::Black;
    PieceType type;
    switch (std::tolower(static_cast<unsigned char>(c))) {
        case 'p': type = PieceType::Pawn; break;
        case 'n': type = PieceType::Knight; break;
        case 'b': type = PieceType::Bishop; break;
        case 'r': type = PieceType::Rook; break;
        case 'q': type = PieceType::Queen; break;
        case 'k': type = PieceType::King; break;
        default: return std::nullopt;
    }
    return Piece{color, type};
}

}  // namespace villen::chess
