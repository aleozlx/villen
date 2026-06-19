#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <string>

#include "villen/chess/position.hpp"

using namespace villen::chess;

namespace {

Move mv(const std::string& from, const std::string& to,
        PieceType promo = PieceType::None) {
    return Move{*squareFromString(from), *squareFromString(to), promo};
}

// perft: count leaf nodes of the legal move tree to a given depth. This is the
// canonical correctness test for a move generator — it exercises captures,
// castling, en passant, promotion and check evasion all at once.
long long perft(const Position& pos, int depth) {
    if (depth == 0) return 1;
    long long nodes = 0;
    for (const Move& m : pos.legalMoves()) {
        Position next = pos;
        bool ok = next.apply(m);
        REQUIRE(ok);  // everything legalMoves() returns must apply cleanly
        nodes += perft(next, depth - 1);
    }
    return nodes;
}

}  // namespace

TEST_CASE("FEN round-trips") {
    const char* fens[] = {
        "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1",
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
        "8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
        "rnbqkbnr/pp1ppppp/8/2p5/4P3/8/PPPP1PPP/RNBQKBNR w KQkq c6 0 2",
    };
    for (const char* f : fens) {
        auto p = Position::fromFen(f);
        REQUIRE(p.has_value());
        CHECK(p->toFen() == f);
    }
}

TEST_CASE("malformed FEN is rejected") {
    CHECK_FALSE(Position::fromFen("not a fen").has_value());
    CHECK_FALSE(Position::fromFen("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP w - - 0 1")
                    .has_value());  // only 7 ranks
    CHECK_FALSE(Position::fromFen("8/8/8/8/8/8/8/8 x - - 0 1").has_value());
}

TEST_CASE("initial position has 20 legal moves") {
    Position p = Position::initial();
    CHECK(p.legalMoves().size() == 20);
    CHECK(p.status() == Status::Active);
    CHECK_FALSE(p.inCheck());
}

TEST_CASE("perft from the initial position") {
    Position p = Position::initial();
    CHECK(perft(p, 1) == 20);
    CHECK(perft(p, 2) == 400);
    CHECK(perft(p, 3) == 8902);
    CHECK(perft(p, 4) == 197281);
}

TEST_CASE("perft Kiwipete (castling, en passant, pins)") {
    auto p = Position::fromFen(
        "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1");
    REQUIRE(p.has_value());
    CHECK(perft(*p, 1) == 48);
    CHECK(perft(*p, 2) == 2039);
    CHECK(perft(*p, 3) == 97862);
}

TEST_CASE("perft endgame position 3") {
    auto p = Position::fromFen("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1");
    REQUIRE(p.has_value());
    CHECK(perft(*p, 1) == 14);
    CHECK(perft(*p, 2) == 191);
    CHECK(perft(*p, 3) == 2812);
    CHECK(perft(*p, 4) == 43238);
}

TEST_CASE("illegal and out-of-turn moves are rejected") {
    Position p = Position::initial();
    CHECK_FALSE(p.apply(mv("e2", "e5")));   // pawn can't jump three
    CHECK_FALSE(p.apply(mv("e7", "e5")));   // not Black's turn
    CHECK_FALSE(p.apply(mv("e1", "e2")));   // king blocked by own pawn
    CHECK(p.toFen() == "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1");
    CHECK(p.apply(mv("e2", "e4")));         // a legal move does apply
    CHECK(p.sideToMove() == Color::Black);
}

TEST_CASE("Scholar's mate is detected as checkmate") {
    Position p = Position::initial();
    REQUIRE(p.apply(mv("e2", "e4")));
    REQUIRE(p.apply(mv("e7", "e5")));
    REQUIRE(p.apply(mv("f1", "c4")));
    REQUIRE(p.apply(mv("b8", "c6")));
    REQUIRE(p.apply(mv("d1", "h5")));
    REQUIRE(p.apply(mv("g8", "f6")));
    REQUIRE(p.apply(mv("h5", "f7")));  // Qxf7#
    CHECK(p.inCheck());
    CHECK(p.legalMoves().empty());
    CHECK(p.status() == Status::Checkmate);
}

TEST_CASE("classic stalemate is detected") {
    // Black to move, not in check, no legal move.
    auto p = Position::fromFen("7k/5Q2/6K1/8/8/8/8/8 b - - 0 1");
    REQUIRE(p.has_value());
    CHECK_FALSE(p->inCheck());
    CHECK(p->legalMoves().empty());
    CHECK(p->status() == Status::Stalemate);
}

TEST_CASE("en passant capture is legal and removes the pawn") {
    auto p = Position::fromFen(
        "rnbqkbnr/ppp1pppp/8/3pP3/8/8/PPPP1PPP/RNBQKBNR w KQkq d6 0 3");
    REQUIRE(p.has_value());
    REQUIRE(p->isLegal(mv("e5", "d6")));
    REQUIRE(p->apply(mv("e5", "d6")));
    CHECK(p->at(*squareFromString("d5")).empty());          // captured pawn gone
    CHECK(p->at(*squareFromString("d6")).type == PieceType::Pawn);
}

TEST_CASE("castling moves the rook too") {
    auto p = Position::fromFen("r3k2r/8/8/8/8/8/8/R3K2R w KQkq - 0 1");
    REQUIRE(p.has_value());
    REQUIRE(p->isLegal(mv("e1", "g1")));  // kingside
    Position ks = *p;
    REQUIRE(ks.apply(mv("e1", "g1")));
    CHECK(ks.at(*squareFromString("g1")).type == PieceType::King);
    CHECK(ks.at(*squareFromString("f1")).type == PieceType::Rook);

    REQUIRE(p->isLegal(mv("e1", "c1")));  // queenside
    Position qs = *p;
    REQUIRE(qs.apply(mv("e1", "c1")));
    CHECK(qs.at(*squareFromString("c1")).type == PieceType::King);
    CHECK(qs.at(*squareFromString("d1")).type == PieceType::Rook);
}

TEST_CASE("cannot castle through or out of check") {
    // Black rook on e8 attacks e1: white king is in check, cannot castle.
    auto p = Position::fromFen("4r3/8/8/8/8/8/8/R3K2R w KQ - 0 1");
    REQUIRE(p.has_value());
    CHECK_FALSE(p->isLegal(mv("e1", "g1")));
    CHECK_FALSE(p->isLegal(mv("e1", "c1")));

    // Rook on f8 attacks f1, a kingside transit square.
    auto q = Position::fromFen("5r2/8/8/8/8/8/8/R3K2R w KQ - 0 1");
    REQUIRE(q.has_value());
    CHECK_FALSE(q->isLegal(mv("e1", "g1")));
    CHECK(q->isLegal(mv("e1", "c1")));  // queenside still fine
}

TEST_CASE("promotion offers all four pieces") {
    auto p = Position::fromFen("8/P7/8/8/8/8/8/k6K w - - 0 1");
    REQUIRE(p.has_value());
    int promos = 0;
    for (const Move& m : p->legalMoves())
        if (m.from == *squareFromString("a7") && m.to == *squareFromString("a8"))
            ++promos;
    CHECK(promos == 4);

    REQUIRE(p->apply(mv("a7", "a8", PieceType::Queen)));
    CHECK(p->at(*squareFromString("a8")).type == PieceType::Queen);
}

TEST_CASE("a pinned piece cannot expose its king") {
    // White rook on e2 is pinned by the black rook on e8 against the e1 king.
    auto p = Position::fromFen("4r3/8/8/8/8/8/4R3/4K3 w - - 0 1");
    REQUIRE(p.has_value());
    CHECK_FALSE(p->isLegal(mv("e2", "d2")));  // sideways would expose the king
    CHECK(p->isLegal(mv("e2", "e3")));        // sliding along the pin is fine
    CHECK(p->isLegal(mv("e2", "e8")));        // capturing the pinner is fine

    // A bishop pinned along a file has no legal move at all.
    auto q = Position::fromFen("4r3/8/8/8/8/8/4B3/4K3 w - - 0 1");
    REQUIRE(q.has_value());
    for (const Move& m : q->legalMoves())
        CHECK(m.from != *squareFromString("e2"));
}

TEST_CASE("insufficient material is a draw") {
    CHECK(Position::fromFen("8/8/8/4k3/8/8/4K3/8 w - - 0 1")->status() ==
          Status::Draw);  // K vs K
    CHECK(Position::fromFen("8/8/8/4k3/8/8/4K3/6N1 w - - 0 1")->status() ==
          Status::Draw);  // K+N vs K
    CHECK(Position::fromFen("8/8/8/4k3/8/8/4K3/6B1 w - - 0 1")->status() ==
          Status::Draw);  // K+B vs K
    // A single pawn is still enough material to play on.
    CHECK(Position::fromFen("8/8/8/4k3/8/8/4KP2/8 w - - 0 1")->status() ==
          Status::Active);
}

TEST_CASE("fifty-move rule yields a draw") {
    auto p = Position::fromFen("4k3/8/8/8/8/8/8/R3K3 w - - 100 80");
    REQUIRE(p.has_value());
    CHECK(p->status() == Status::Draw);
}
