// Villen chess engine — the authoritative position.
//
// `Position` is the whole engine surface: construct one, ask it for `legalMoves`,
// `apply` a move, ask its `status`. It is self-contained and serializable to FEN
// (DESIGN-villen.md §9.2), which is what buys save/load, spectating and
// reconnection later for free. No networking, no rendering, no device code lives
// here — this is the pure "engine slot" the design is built around (§9.1).
#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "move.hpp"
#include "types.hpp"

namespace villen::chess {

// Outcome of a position from the side-to-move's perspective.
//
// `Check` is an engine convenience; the player wire protocol collapses it into
// "active" (DESIGN-villen.md §6.2 lists active|checkmate|stalemate|draw).
enum class Status { Active, Check, Checkmate, Stalemate, Draw };

const char* statusName(Status s);     // engine name: "active"/"check"/...
const char* wireStatusName(Status s); // protocol name: Check -> "active"

struct CastlingRights {
    bool whiteKing = false;
    bool whiteQueen = false;
    bool blackKing = false;
    bool blackQueen = false;
};

class Position {
public:
    // Standard chess starting position.
    static Position initial();

    // Parse Forsyth–Edwards Notation. std::nullopt on malformed input.
    static std::optional<Position> fromFen(const std::string& fen);

    // Serialize to FEN. Round-trips with fromFen.
    std::string toFen() const;

    Piece at(Square s) const { return squares_[s]; }
    const std::array<Piece, 64>& squares() const { return squares_; }
    Color sideToMove() const { return side_; }
    CastlingRights castling() const { return castling_; }
    Square enPassantTarget() const { return epTarget_; }
    int halfmoveClock() const { return halfmove_; }
    int fullmoveNumber() const { return fullmove_; }

    // All fully-legal moves for the side to move (king-safety enforced).
    std::vector<Move> legalMoves() const;

    // True if `m` is among legalMoves(). The single authority gate (§6 rules).
    bool isLegal(const Move& m) const;

    // Apply a move. Returns false (and leaves the position untouched) if the move
    // is not legal — the server relies on this to reject illegal proposals.
    bool apply(const Move& m);

    bool inCheck() const { return inCheck(side_); }
    bool inCheck(Color c) const;

    // Is `s` attacked by any piece of color `by`? (Ignores castling.)
    bool isSquareAttacked(Square s, Color by) const;

    // Computed terminal/ongoing status for the side to move.
    Status status() const;

private:
    std::array<Piece, 64> squares_{};
    Color side_{Color::White};
    CastlingRights castling_{};
    Square epTarget_{NO_SQUARE};
    int halfmove_{0};
    int fullmove_{1};

    Square kingSquare(Color c) const;

    void generatePseudoLegal(std::vector<Move>& out) const;
    void addSliderMoves(Square from, const int (*dirs)[2], int n,
                        std::vector<Move>& out) const;
    void addStepMoves(Square from, const int (*dirs)[2], int n,
                      std::vector<Move>& out) const;
    void addPawnMoves(Square from, std::vector<Move>& out) const;
    void addCastlingMoves(std::vector<Move>& out) const;

    // Mutate the board assuming `m` is pseudo-legal. Used by apply() (after the
    // legality gate) and by the legal-move filter (on a throwaway copy).
    void makeMoveRaw(const Move& m);

    bool hasInsufficientMaterial() const;
};

}  // namespace villen::chess
