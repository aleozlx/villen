// Villen chess engine — core value types.
//
// Pure data + tiny inline helpers. No I/O, no graphics, no allocation beyond
// std::string for human-readable square names. This header is the vocabulary the
// rest of the engine speaks; keep it dependency-free so the engine stays a drop-in
// "game slot" (see DESIGN-villen.md §9.1).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace villen::chess {

enum class Color : std::uint8_t { White, Black };

inline Color opponent(Color c) {
    return c == Color::White ? Color::Black : Color::White;
}

enum class PieceType : std::uint8_t { None, Pawn, Knight, Bishop, Rook, Queen, King };

struct Piece {
    Color color{Color::White};
    PieceType type{PieceType::None};

    bool empty() const { return type == PieceType::None; }
};

inline bool operator==(const Piece& a, const Piece& b) {
    return a.color == b.color && a.type == b.type;
}
inline bool operator!=(const Piece& a, const Piece& b) { return !(a == b); }

// Square index 0..63. a1 = 0, h1 = 7, a8 = 56, h8 = 63.
// file = column (a..h -> 0..7), rank = row (1..8 -> 0..7).
using Square = int;
constexpr Square NO_SQUARE = -1;

inline int fileOf(Square s) { return s & 7; }
inline int rankOf(Square s) { return s >> 3; }
inline Square makeSquare(int file, int rank) { return rank * 8 + file; }
inline bool onBoard(int file, int rank) {
    return file >= 0 && file < 8 && rank >= 0 && rank < 8;
}

// "e4" <-> 28. Returns std::nullopt for malformed input.
std::string squareToString(Square s);
std::optional<Square> squareFromString(const std::string& s);

// FEN piece letter: white = uppercase, black = lowercase. '.' for empty.
char pieceToChar(Piece p);
// Inverse of pieceToChar for a single FEN letter. std::nullopt if not a piece.
std::optional<Piece> pieceFromChar(char c);

// Square color (for insufficient-material bishop checks). true == light square.
inline bool isLightSquare(Square s) { return ((fileOf(s) + rankOf(s)) & 1) != 0; }

}  // namespace villen::chess
