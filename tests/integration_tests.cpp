// Host integration tests: the real WsServer + Host + ChessEngine, exercised end
// to end over a loopback WebSocket by a C++ stand-in for the browser player.
//
// These promote the throwaway smoke/auth/reconnect/DoS scripts used while the
// IEngine/Room framework was built (PR #15) into committed coverage. Unlike the
// engine unit tests (pure rules, no I/O), these cover the network edge and the
// membership authority that live in the host: seat assignment, move authority,
// the disconnect-hold + token-free reconnect lifecycle, and that hostile JSON on
// the wire can't crash the single-threaded server (the DoS Gemini caught).
//
// Each TEST_CASE spins up its own server on an OS-assigned ephemeral port, driven
// by a background poll thread — the host's real single loop, just moved off the
// test thread so the test can act as a client over a real socket. A fresh server
// per case means each test starts from a pristine game with an empty roster, with
// no seat-hold state bleeding between cases.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "engines/chess/chess_engine.hpp"
#include "host.hpp"
#include "ws_server.hpp"
#include "ws_test_client.hpp"

using json = nlohmann::json;
using villen::test::WsClient;

namespace {

// A self-contained chess server: WsServer + Host(chess), pumped by a background
// thread running the host's poll+tick loop. Construction binds an ephemeral port
// and starts chess; destruction stops the loop and joins before anything it
// touches is torn down.
struct ChessServer {
    villen::net::WsServer ws;
    std::unique_ptr<villen::Host> host;
    std::atomic<bool> running{true};
    std::thread loop;

    ChessServer() {
        std::vector<std::unique_ptr<villen::IEngineFactory>> engines;
        engines.push_back(std::make_unique<villen::ChessFactory>());
        host = std::make_unique<villen::Host>(ws, std::move(engines));
        REQUIRE(ws.listen(0));   // 0 -> OS picks a free port; port() reports it
        host->startEngine(0);    // chess is the only registered engine
        loop = std::thread([this] {
            using namespace std::chrono;
            while (running.load(std::memory_order_relaxed)) {
                ws.poll(10);
                const auto now = duration_cast<milliseconds>(
                                     steady_clock::now().time_since_epoch())
                                     .count();
                host->tick(static_cast<std::uint64_t>(now));
            }
        });
    }

    ~ChessServer() {
        running.store(false, std::memory_order_relaxed);
        if (loop.joinable()) loop.join();
    }

    std::uint16_t port() const { return ws.port(); }
};

// --- helpers over a drained batch of messages -------------------------------

std::vector<json> parseAll(const std::vector<std::string>& raw) {
    std::vector<json> out;
    for (const auto& s : raw) {
        json j = json::parse(s, nullptr, /*allow_exceptions=*/false);
        if (!j.is_discarded()) out.push_back(std::move(j));
    }
    return out;
}

// The string "type" of a message, or "" if absent or not a string. Type-checked
// via find()/is_string() rather than value()'d so a wrong-typed "type" key yields
// "" instead of throwing — the exception-free JSON discipline the rest of the
// codebase follows (envelope.cpp, chess_engine.cpp's strField).
std::string msgType(const json& m) {
    if (!m.is_object()) return {};
    auto it = m.find("type");
    return (it != m.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

// The first message of `type`, returned BY VALUE (a null json if none). By value
// on purpose: callers routinely inline `firstOfType(parseAll(c->drain()), ...)`,
// and a pointer would dangle into that temporary vector the moment the statement
// ends.
json firstOfType(const std::vector<json>& msgs, const char* type) {
    for (const auto& m : msgs)
        if (msgType(m) == type) return m;
    return json{};  // null
}

bool hasType(const std::vector<json>& msgs, const char* type) {
    for (const auto& m : msgs)
        if (msgType(m) == type) return true;
    return false;
}

// Connect a client and consume the initial engine-announce, leaving it ready to
// join. REQUIRE the connection so a bind/handshake failure aborts the case.
std::unique_ptr<WsClient> connect(ChessServer& s) {
    auto c = std::make_unique<WsClient>();
    REQUIRE(c->connect(s.port()));
    auto hello = parseAll(c->drain());
    REQUIRE(hasType(hello, "engine"));
    return c;
}

// The seat a `joined` reply assigned, or "" if the batch had no joined message.
std::string joinedSeat(const std::vector<json>& msgs) {
    json j = firstOfType(msgs, "joined");
    return j.is_null() ? std::string{} : j.value("seat", std::string{});
}

// The first field of a FEN ("position"), i.e. the piece placement.
std::string placement(const json& state) {
    std::string fen = state.value("position", std::string{});
    return fen.substr(0, fen.find(' '));
}

constexpr const char* kProposeE2E4 =
    R"({"type":"proposeMove","move":{"from":"e2","to":"e4"}})";

}  // namespace

TEST_CASE("connect announces chess, then auto-assigns the first open seat") {
    ChessServer s;
    auto a = connect(s);

    a->sendText(R"({"type":"join","seat":""})");
    auto m = parseAll(a->drain());

    CHECK(joinedSeat(m) == "white");
    json st = firstOfType(m, "state");
    REQUIRE_FALSE(st.is_null());
    // The authoritative snapshot the client renders from: initial position,
    // white to move, and the roster reflecting this connection as seated.
    CHECK(placement(st) == "rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR");
    CHECK(st.value("turn", std::string{}) == "white");
    CHECK(st["seats"].value("white", std::string{}) == "connected");
}

TEST_CASE("move authority is derived from seat and side to move") {
    ChessServer s;
    auto a = connect(s);
    auto b = connect(s);

    a->sendText(R"({"type":"join","seat":""})");
    CHECK(joinedSeat(parseAll(a->drain())) == "white");
    b->sendText(R"({"type":"join","seat":""})");
    CHECK(joinedSeat(parseAll(b->drain())) == "black");

    SUBCASE("black cannot move while it is white's turn") {
        b->sendText(R"({"type":"proposeMove","move":{"from":"e7","to":"e5"}})");
        json rej = firstOfType(parseAll(b->drain()), "reject");
        REQUIRE_FALSE(rej.is_null());
        CHECK(rej.value("reason", std::string{}) == "not_your_turn");
    }

    SUBCASE("white's legal move is applied and broadcast as new state") {
        a->sendText(kProposeE2E4);
        json st = firstOfType(parseAll(a->drain()), "state");
        REQUIRE_FALSE(st.is_null());
        CHECK(st.value("turn", std::string{}) == "black");
        CHECK(placement(st).find("4P3") != std::string::npos);  // pawn now on e4
    }

    SUBCASE("an illegal move from the side to move is rejected, not applied") {
        // Advance to black's turn first, so this exercises the *legality* check
        // rather than the authority check (which would fire first if black tried
        // to move on white's turn — that path is covered above).
        a->sendText(kProposeE2E4);
        a->drain();
        b->sendText(R"({"type":"proposeMove","move":{"from":"e7","to":"e8"}})");
        json rej = firstOfType(parseAll(b->drain()), "reject");
        REQUIRE_FALSE(rej.is_null());
        CHECK(rej.value("reason", std::string{}) == "illegal_move");
    }
}

TEST_CASE("ping is answered with pong (the generic envelope)") {
    ChessServer s;
    auto a = connect(s);
    a->sendText(R"({"type":"ping"})");
    CHECK(hasType(parseAll(a->drain()), "pong"));
}

TEST_CASE("an unseated spectator can never move, even an open side") {
    ChessServer s;
    auto spec = connect(s);

    // 1) A connection that never joined must not move an open seat.
    spec->sendText(kProposeE2E4);
    json rej = firstOfType(parseAll(spec->drain()), "reject");
    REQUIRE_FALSE(rej.is_null());
    CHECK(rej.value("reason", std::string{}) == "not_seated");

    // 2) A *seated* lone player may legitimately drive both sides (the documented
    //    single-client case) — this is the allowance the spectator is denied.
    auto a = connect(s);
    a->sendText(R"({"type":"join","seat":""})");
    CHECK(joinedSeat(parseAll(a->drain())) == "white");

    a->sendText(kProposeE2E4);
    json st1 = firstOfType(parseAll(a->drain()), "state");
    REQUIRE_FALSE(st1.is_null());
    CHECK(st1.value("turn", std::string{}) == "black");

    a->sendText(R"({"type":"proposeMove","move":{"from":"e7","to":"e5"}})");
    json st2 = firstOfType(parseAll(a->drain()), "state");
    REQUIRE_FALSE(st2.is_null());
    CHECK(st2.value("turn", std::string{}) == "white");
    CHECK(placement(st2).find("4p3") != std::string::npos);  // black pawn on e5

    // 3) The still-unseated spectator stays blocked while a seated player plays.
    spec->sendText(R"({"type":"proposeMove","move":{"from":"d2","to":"d4"}})");
    json rej2 = firstOfType(parseAll(spec->drain()), "reject");
    REQUIRE_FALSE(rej2.is_null());
    CHECK(rej2.value("reason", std::string{}) == "not_seated");
}

TEST_CASE("a dropped seat is held and reclaimed by name (token-free reconnect)") {
    ChessServer s;
    auto a = connect(s);
    a->sendText(R"({"type":"join","seat":""})");
    CHECK(joinedSeat(parseAll(a->drain())) == "white");

    auto c = connect(s);
    c->sendText(R"({"type":"join","seat":""})");
    CHECK(joinedSeat(parseAll(c->drain())) == "black");

    // A drops: white is held, and the still-connected C is told white went
    // disconnected (not open) so the seat can't be seized.
    a->disconnect();
    {
        json su = firstOfType(parseAll(c->drain()), "sessionUpdate");
        REQUIRE_FALSE(su.is_null());
        CHECK(su["seats"].value("white", std::string{}) == "disconnected");
    }

    // A walk-up auto-join must NOT seize the held white seat -> spectator.
    {
        auto d = connect(s);
        d->sendText(R"({"type":"join","seat":""})");
        CHECK(joinedSeat(parseAll(d->drain())) == "spectator");
    }  // d disconnects here; it held no seat, so nothing is broadcast

    // The original player reclaims white by name (DESIGN §13: the seat name is
    // public routing, not a credential — no token needed).
    auto a2 = connect(s);
    a2->sendText(R"({"type":"join","seat":"white"})");
    CHECK(joinedSeat(parseAll(a2->drain())) == "white");
    {
        json su = firstOfType(parseAll(c->drain()), "sessionUpdate");
        REQUIRE_FALSE(su.is_null());
        CHECK(su["seats"].value("white", std::string{}) == "connected");
    }
}

TEST_CASE("hostile JSON on the wire cannot crash the single-threaded server") {
    ChessServer s;

    // Type-confused payloads that would throw json::type_error on an unguarded
    // value()/get<>() read — fatal on a single loop (this is the DoS Gemini
    // caught on PR #15). Each is sent on its own connection; the server must stay
    // up (a thrown exception on the poll thread would terminate the process).
    const char* hostile[] = {
        R"({"type":123})",
        R"({"type":"proposeMove","move":{"from":42,"to":["x"]}})",
        R"({"type":["join"],"seat":{"x":1}})",
        R"({"type":"join","seat":99})",
        R"({"type":"proposeMove","move":"notobject"})",
        "not json at all",
        R"({"session":5,"type":"ping"})",
        "[]",
        "12345",
    };
    for (const char* payload : hostile) {
        WsClient c;
        REQUIRE(c.connect(s.port()));
        c.drain();          // announce
        c.sendText(payload);
        c.drain();          // whatever the server replies (or nothing) — no crash
        c.disconnect();
    }

    // After the barrage, a normal client must still get a clean announce + join.
    auto good = connect(s);
    good->sendText(R"({"type":"join","seat":""})");
    auto m = parseAll(good->drain());
    CHECK(hasType(m, "joined"));
    CHECK(hasType(m, "state"));
}
