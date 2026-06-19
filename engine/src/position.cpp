#include "villen/chess/position.hpp"

#include <cstdlib>
#include <sstream>

namespace villen::chess {

namespace {
// Movement vectors as {file-delta, rank-delta}.
constexpr int kKnight[8][2] = {{1, 2},  {2, 1},   {2, -1}, {1, -2},
                               {-1, -2}, {-2, -1}, {-2, 1}, {-1, 2}};
constexpr int kKing[8][2] = {{1, 0}, {1, 1}, {0, 1}, {-1, 1},
                             {-1, 0}, {-1, -1}, {0, -1}, {1, -1}};
constexpr int kBishop[4][2] = {{1, 1}, {1, -1}, {-1, 1}, {-1, -1}};
constexpr int kRook[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
constexpr int kQueen[8][2] = {{1, 1},  {1, -1}, {-1, 1}, {-1, -1},
                              {1, 0}, {-1, 0}, {0, 1}, {0, -1}};
}  // namespace

const char* statusName(Status s) {
    switch (s) {
        case Status::Active: return "active";
        case Status::Check: return "check";
        case Status::Checkmate: return "checkmate";
        case Status::Stalemate: return "stalemate";
        case Status::Draw: return "draw";
    }
    return "active";
}

const char* wireStatusName(Status s) {
    // The player protocol has no "check" status — a king in check is still
    // active play (DESIGN-villen.md §6.2).
    return s == Status::Check ? "active" : statusName(s);
}

Position Position::initial() {
    return *Position::fromFen(
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
}

std::optional<Position> Position::fromFen(const std::string& fen) {
    std::istringstream in(fen);
    std::string placement, side, castling, ep;
    if (!(in >> placement >> side >> castling >> ep)) return std::nullopt;

    Position p;
    p.squares_.fill(Piece{});

    // Placement: rank 8 first, descending to rank 1; files a..h within a rank.
    int rank = 7, file = 0;
    for (char c : placement) {
        if (c == '/') {
            if (file != 8) return std::nullopt;
            --rank;
            file = 0;
        } else if (c >= '1' && c <= '8') {
            file += c - '0';
        } else {
            auto piece = pieceFromChar(c);
            if (!piece || !onBoard(file, rank)) return std::nullopt;
            p.squares_[makeSquare(file, rank)] = *piece;
            ++file;
        }
    }
    if (rank != 0 || file != 8) return std::nullopt;

    if (side == "w") p.side_ = Color::White;
    else if (side == "b") p.side_ = Color::Black;
    else return std::nullopt;

    p.castling_ = CastlingRights{};
    if (castling != "-") {
        for (char c : castling) {
            switch (c) {
                case 'K': p.castling_.whiteKing = true; break;
                case 'Q': p.castling_.whiteQueen = true; break;
                case 'k': p.castling_.blackKing = true; break;
                case 'q': p.castling_.blackQueen = true; break;
                default: return std::nullopt;
            }
        }
    }

    if (ep == "-") {
        p.epTarget_ = NO_SQUARE;
    } else {
        auto sq = squareFromString(ep);
        if (!sq) return std::nullopt;
        p.epTarget_ = *sq;
    }

    // Move counters are optional; default sensibly if absent.
    int halfmove = 0, fullmove = 1;
    in >> halfmove;
    in >> fullmove;
    p.halfmove_ = halfmove;
    p.fullmove_ = fullmove;
    return p;
}

std::string Position::toFen() const {
    std::string out;
    for (int rank = 7; rank >= 0; --rank) {
        int empties = 0;
        for (int file = 0; file < 8; ++file) {
            Piece pc = squares_[makeSquare(file, rank)];
            if (pc.empty()) {
                ++empties;
            } else {
                if (empties) out += std::to_string(empties);
                empties = 0;
                out.push_back(pieceToChar(pc));
            }
        }
        if (empties) out += std::to_string(empties);
        if (rank) out.push_back('/');
    }

    out.push_back(' ');
    out.push_back(side_ == Color::White ? 'w' : 'b');

    out.push_back(' ');
    std::string c;
    if (castling_.whiteKing) c += 'K';
    if (castling_.whiteQueen) c += 'Q';
    if (castling_.blackKing) c += 'k';
    if (castling_.blackQueen) c += 'q';
    out += c.empty() ? "-" : c;

    out.push_back(' ');
    out += epTarget_ == NO_SQUARE ? "-" : squareToString(epTarget_);

    out.push_back(' ');
    out += std::to_string(halfmove_);
    out.push_back(' ');
    out += std::to_string(fullmove_);
    return out;
}

Square Position::kingSquare(Color c) const {
    for (Square s = 0; s < 64; ++s) {
        if (squares_[s].type == PieceType::King && squares_[s].color == c) return s;
    }
    return NO_SQUARE;
}

bool Position::isSquareAttacked(Square s, Color by) const {
    int f = fileOf(s), r = rankOf(s);

    // Pawn attacks: a `by` pawn sits one rank "behind" s (toward its own side)
    // on an adjacent file.
    int pawnRank = by == Color::White ? r - 1 : r + 1;
    for (int df : {-1, 1}) {
        if (onBoard(f + df, pawnRank)) {
            Piece p = squares_[makeSquare(f + df, pawnRank)];
            if (p.type == PieceType::Pawn && p.color == by) return true;
        }
    }

    // Knight attacks.
    for (auto& d : kKnight) {
        if (onBoard(f + d[0], r + d[1])) {
            Piece p = squares_[makeSquare(f + d[0], r + d[1])];
            if (p.type == PieceType::Knight && p.color == by) return true;
        }
    }

    // King attacks (adjacent).
    for (auto& d : kKing) {
        if (onBoard(f + d[0], r + d[1])) {
            Piece p = squares_[makeSquare(f + d[0], r + d[1])];
            if (p.type == PieceType::King && p.color == by) return true;
        }
    }

    // Sliding attacks: bishops/queens on diagonals, rooks/queens on ranks/files.
    for (auto& d : kBishop) {
        int nf = f + d[0], nr = r + d[1];
        while (onBoard(nf, nr)) {
            Piece p = squares_[makeSquare(nf, nr)];
            if (!p.empty()) {
                if (p.color == by &&
                    (p.type == PieceType::Bishop || p.type == PieceType::Queen))
                    return true;
                break;
            }
            nf += d[0];
            nr += d[1];
        }
    }
    for (auto& d : kRook) {
        int nf = f + d[0], nr = r + d[1];
        while (onBoard(nf, nr)) {
            Piece p = squares_[makeSquare(nf, nr)];
            if (!p.empty()) {
                if (p.color == by &&
                    (p.type == PieceType::Rook || p.type == PieceType::Queen))
                    return true;
                break;
            }
            nf += d[0];
            nr += d[1];
        }
    }
    return false;
}

bool Position::inCheck(Color c) const {
    Square ks = kingSquare(c);
    if (ks == NO_SQUARE) return false;
    return isSquareAttacked(ks, opponent(c));
}

void Position::addStepMoves(Square from, const int (*dirs)[2], int n,
                            std::vector<Move>& out) const {
    Color us = squares_[from].color;
    int f = fileOf(from), r = rankOf(from);
    for (int i = 0; i < n; ++i) {
        int nf = f + dirs[i][0], nr = r + dirs[i][1];
        if (!onBoard(nf, nr)) continue;
        Square to = makeSquare(nf, nr);
        if (squares_[to].empty() || squares_[to].color != us)
            out.push_back(Move{from, to, PieceType::None});
    }
}

void Position::addSliderMoves(Square from, const int (*dirs)[2], int n,
                              std::vector<Move>& out) const {
    Color us = squares_[from].color;
    int f = fileOf(from), r = rankOf(from);
    for (int i = 0; i < n; ++i) {
        int nf = f + dirs[i][0], nr = r + dirs[i][1];
        while (onBoard(nf, nr)) {
            Square to = makeSquare(nf, nr);
            if (squares_[to].empty()) {
                out.push_back(Move{from, to, PieceType::None});
            } else {
                if (squares_[to].color != us)
                    out.push_back(Move{from, to, PieceType::None});
                break;
            }
            nf += dirs[i][0];
            nr += dirs[i][1];
        }
    }
}

void Position::addPawnMoves(Square from, std::vector<Move>& out) const {
    Color us = squares_[from].color;
    int f = fileOf(from), r = rankOf(from);
    int dir = us == Color::White ? 1 : -1;
    int startRank = us == Color::White ? 1 : 6;
    int promoRank = us == Color::White ? 7 : 0;

    auto emit = [&](Square to) {
        if (rankOf(to) == promoRank) {
            for (PieceType pt : {PieceType::Queen, PieceType::Rook,
                                 PieceType::Bishop, PieceType::Knight})
                out.push_back(Move{from, to, pt});
        } else {
            out.push_back(Move{from, to, PieceType::None});
        }
    };

    // Single and double pushes (only onto empty squares).
    int oneR = r + dir;
    if (onBoard(f, oneR)) {
        Square one = makeSquare(f, oneR);
        if (squares_[one].empty()) {
            emit(one);
            int twoR = r + 2 * dir;
            if (r == startRank && onBoard(f, twoR)) {
                Square two = makeSquare(f, twoR);
                if (squares_[two].empty())
                    out.push_back(Move{from, two, PieceType::None});
            }
        }
    }

    // Captures, including en passant.
    for (int df : {-1, 1}) {
        int nf = f + df, nr = r + dir;
        if (!onBoard(nf, nr)) continue;
        Square to = makeSquare(nf, nr);
        if (!squares_[to].empty() && squares_[to].color != us) {
            emit(to);
        } else if (to == epTarget_) {
            out.push_back(Move{from, to, PieceType::None});
        }
    }
}

void Position::addCastlingMoves(std::vector<Move>& out) const {
    Color us = side_;
    Color them = opponent(us);
    int r = us == Color::White ? 0 : 7;
    Square kingFrom = makeSquare(4, r);
    if (squares_[kingFrom].type != PieceType::King ||
        squares_[kingFrom].color != us)
        return;
    if (isSquareAttacked(kingFrom, them)) return;  // cannot castle out of check

    bool kingSide = us == Color::White ? castling_.whiteKing : castling_.blackKing;
    bool queenSide =
        us == Color::White ? castling_.whiteQueen : castling_.blackQueen;

    if (kingSide) {
        Square f1 = makeSquare(5, r), g1 = makeSquare(6, r);
        if (squares_[f1].empty() && squares_[g1].empty() &&
            !isSquareAttacked(f1, them) && !isSquareAttacked(g1, them))
            out.push_back(Move{kingFrom, g1, PieceType::None});
    }
    if (queenSide) {
        Square d1 = makeSquare(3, r), c1 = makeSquare(2, r), b1 = makeSquare(1, r);
        if (squares_[d1].empty() && squares_[c1].empty() && squares_[b1].empty() &&
            !isSquareAttacked(d1, them) && !isSquareAttacked(c1, them))
            out.push_back(Move{kingFrom, c1, PieceType::None});
    }
}

void Position::generatePseudoLegal(std::vector<Move>& out) const {
    for (Square s = 0; s < 64; ++s) {
        Piece p = squares_[s];
        if (p.empty() || p.color != side_) continue;
        switch (p.type) {
            case PieceType::Pawn: addPawnMoves(s, out); break;
            case PieceType::Knight: addStepMoves(s, kKnight, 8, out); break;
            case PieceType::King: addStepMoves(s, kKing, 8, out); break;
            case PieceType::Bishop: addSliderMoves(s, kBishop, 4, out); break;
            case PieceType::Rook: addSliderMoves(s, kRook, 4, out); break;
            case PieceType::Queen: addSliderMoves(s, kQueen, 8, out); break;
            default: break;
        }
    }
    addCastlingMoves(out);
}

void Position::makeMoveRaw(const Move& m) {
    Piece p = squares_[m.from];
    Color us = p.color;
    bool isPawn = p.type == PieceType::Pawn;
    bool isCapture = !squares_[m.to].empty();
    Square newEp = NO_SQUARE;

    // En passant: a pawn moving diagonally onto the (empty) ep target captures
    // the pawn that sits beside it.
    if (isPawn && m.to == epTarget_ && squares_[m.to].empty()) {
        Square captured = makeSquare(fileOf(m.to), rankOf(m.from));
        squares_[captured] = Piece{};
        isCapture = true;
    }

    squares_[m.to] = p;
    squares_[m.from] = Piece{};

    if (m.promotion != PieceType::None) squares_[m.to] = Piece{us, m.promotion};

    // Castling: relocate the rook to match the king's two-square hop.
    if (p.type == PieceType::King) {
        int delta = fileOf(m.to) - fileOf(m.from);
        if (delta == 2) {
            Square rf = makeSquare(7, rankOf(m.from)),
                   rt = makeSquare(5, rankOf(m.from));
            squares_[rt] = squares_[rf];
            squares_[rf] = Piece{};
        } else if (delta == -2) {
            Square rf = makeSquare(0, rankOf(m.from)),
                   rt = makeSquare(3, rankOf(m.from));
            squares_[rt] = squares_[rf];
            squares_[rf] = Piece{};
        }
    }

    if (isPawn && std::abs(rankOf(m.to) - rankOf(m.from)) == 2)
        newEp = makeSquare(fileOf(m.from), (rankOf(m.from) + rankOf(m.to)) / 2);

    // Castling rights: lost when the king moves, or when a rook leaves / is
    // captured on its home square.
    if (p.type == PieceType::King) {
        if (us == Color::White) {
            castling_.whiteKing = castling_.whiteQueen = false;
        } else {
            castling_.blackKing = castling_.blackQueen = false;
        }
    }
    auto clearRookRight = [&](Square sq) {
        if (sq == makeSquare(0, 0)) castling_.whiteQueen = false;
        if (sq == makeSquare(7, 0)) castling_.whiteKing = false;
        if (sq == makeSquare(0, 7)) castling_.blackQueen = false;
        if (sq == makeSquare(7, 7)) castling_.blackKing = false;
    };
    clearRookRight(m.from);
    clearRookRight(m.to);

    halfmove_ = (isPawn || isCapture) ? 0 : halfmove_ + 1;
    if (us == Color::Black) ++fullmove_;
    epTarget_ = newEp;
    side_ = opponent(us);
}

std::vector<Move> Position::legalMoves() const {
    std::vector<Move> pseudo;
    pseudo.reserve(64);
    generatePseudoLegal(pseudo);

    std::vector<Move> legal;
    legal.reserve(pseudo.size());
    for (const Move& m : pseudo) {
        Color mover = squares_[m.from].color;
        Position next = *this;
        next.makeMoveRaw(m);
        if (!next.inCheck(mover)) legal.push_back(m);
    }
    return legal;
}

bool Position::isLegal(const Move& m) const {
    for (const Move& legal : legalMoves())
        if (legal == m) return true;
    return false;
}

bool Position::apply(const Move& m) {
    if (!isLegal(m)) return false;
    makeMoveRaw(m);
    return true;
}

bool Position::hasInsufficientMaterial() const {
    // Detects the classic dead positions: K vs K, K+minor vs K, and
    // K+bishop(s) vs K+bishop(s) with every bishop on one square color.
    int knights = 0;
    bool sawLightBishop = false, sawDarkBishop = false;
    for (Square s = 0; s < 64; ++s) {
        Piece p = squares_[s];
        if (p.empty()) continue;
        switch (p.type) {
            case PieceType::King: break;
            case PieceType::Knight: ++knights; break;
            case PieceType::Bishop:
                (isLightSquare(s) ? sawLightBishop : sawDarkBishop) = true;
                break;
            default:
                return false;  // any pawn/rook/queen can mate
        }
    }
    if (knights == 0 && !sawLightBishop && !sawDarkBishop) return true;  // K vs K
    if (knights == 1 && !sawLightBishop && !sawDarkBishop) return true;  // lone N
    if (knights == 0 && (sawLightBishop != sawDarkBishop)) return true;  // 1-color B
    return false;
}

Status Position::status() const {
    if (legalMoves().empty())
        return inCheck() ? Status::Checkmate : Status::Stalemate;
    if (halfmove_ >= 100) return Status::Draw;        // 50-move rule
    if (hasInsufficientMaterial()) return Status::Draw;
    return inCheck() ? Status::Check : Status::Active;
}

}  // namespace villen::chess
