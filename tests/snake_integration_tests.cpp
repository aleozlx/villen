// Host integration tests for the snake engine: the real WsServer + Host +
// SnakeEngine driven end to end over a loopback WebSocket. These cover what the
// pure-engine unit tests (snake_tests.cpp) can't reach — the network edge and the
// authoritative *clock*: that the world advances on the server's fixed timestep
// with no input at all (DESIGN-snake §4, acceptance #3), that seats map to snakes,
// that a steering intent is applied server-side (§5), and that several browsers
// share one arena (§6).
//
// Like integration_tests.cpp, each case spins up its own server on an ephemeral
// port with a background poll+tick thread (the host's real single loop, moved off
// the test thread). Snake broadcasts continuously, so the tests use the client's
// time-bounded collect() rather than drain()'s idle-gap heuristic.
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

#include "engines/snake/snake_engine.hpp"
#include "host.hpp"
#include "ws_server.hpp"
#include "ws_test_client.hpp"

using json = nlohmann::json;
using villen::test::WsClient;

namespace {

struct SnakeServer {
    villen::net::WsServer ws;
    std::unique_ptr<villen::Host> host;
    std::atomic<bool> running{true};
    std::thread loop;

    SnakeServer() {
        std::vector<std::unique_ptr<villen::IEngineFactory>> engines;
        engines.push_back(std::make_unique<villen::SnakeFactory>());
        host = std::make_unique<villen::Host>(ws, std::move(engines), "client");
        REQUIRE(ws.listen(0));
        host->startEngine(0);
        loop = std::thread([this] {
            using namespace std::chrono;
            while (running.load(std::memory_order_relaxed)) {
                ws.poll(5);
                const auto now = duration_cast<milliseconds>(
                                     steady_clock::now().time_since_epoch())
                                     .count();
                host->tick(static_cast<std::uint64_t>(now));
            }
        });
    }

    ~SnakeServer() {
        running.store(false, std::memory_order_relaxed);
        if (loop.joinable()) loop.join();
    }

    std::uint16_t port() const { return ws.port(); }
};

std::vector<json> parseAll(const std::vector<std::string>& raw) {
    std::vector<json> out;
    for (const auto& s : raw) {
        json j = json::parse(s, nullptr, /*allow_exceptions=*/false);
        if (!j.is_discarded()) out.push_back(std::move(j));
    }
    return out;
}

std::string msgType(const json& m) {
    if (!m.is_object()) return {};
    auto it = m.find("type");
    return (it != m.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

bool hasType(const std::vector<json>& msgs, const char* type) {
    for (const auto& m : msgs)
        if (msgType(m) == type) return true;
    return false;
}

json firstOfType(const std::vector<json>& msgs, const char* type) {
    for (const auto& m : msgs)
        if (msgType(m) == type) return m;
    return json{};
}

json lastOfType(const std::vector<json>& msgs, const char* type) {
    json found{};
    for (const auto& m : msgs)
        if (msgType(m) == type) found = m;
    return found;
}

// The snake object with the given id from a `state` message, or null.
json snakeWithId(const json& state, int id) {
    if (!state.is_object() || !state.contains("snakes")) return json{};
    for (const auto& s : state["snakes"])
        if (s.is_object() && s.value("id", -999) == id) return s;
    return json{};
}

}  // namespace

TEST_CASE("connect announces snake and the world ticks with no input at all") {
    SnakeServer s;
    WsClient c;
    REQUIRE(c.connect(s.port()));

    // Even before joining, the shared arena broadcasts its state on the server
    // clock. Over a ~400ms window at 10 Hz we should see several `state` frames.
    auto msgs = parseAll(c.collect(400));
    CHECK(hasType(msgs, "engine"));

    std::vector<unsigned> ticks;
    for (const auto& m : msgs)
        if (msgType(m) == "state") ticks.push_back(m.value("tick", 0u));
    REQUIRE(ticks.size() >= 2);
    // The authoritative clock: tick strictly advances with zero player input
    // (DESIGN-snake §4) — the property no prior engine had.
    CHECK(ticks.back() > ticks.front());
}

TEST_CASE("joining allocates a snake and tells the client which one is theirs") {
    SnakeServer s;
    WsClient c;
    REQUIRE(c.connect(s.port()));
    c.collect(50);  // settle the announce

    c.sendText(R"({"type":"join","seat":""})");
    auto msgs = parseAll(c.collect(300));

    json joined = firstOfType(msgs, "joined");
    REQUIRE_FALSE(joined.is_null());
    CHECK(joined.value("seat", std::string{}) == "p1");  // first open seat

    json you = firstOfType(msgs, "you");
    REQUIRE_FALSE(you.is_null());
    CHECK(you.value("id", -1) == 0);  // seat index 0 == snake id 0

    json state = lastOfType(msgs, "state");
    REQUIRE_FALSE(state.is_null());
    CHECK_FALSE(snakeWithId(state, 0).is_null());  // the joiner's snake exists
}

TEST_CASE("a steering intent is applied server-side, never a position") {
    SnakeServer s;
    WsClient c;
    REQUIRE(c.connect(s.port()));
    c.collect(50);
    c.sendText(R"({"type":"join","seat":""})");

    json state = lastOfType(parseAll(c.collect(250)), "state");
    REQUIRE_FALSE(state.is_null());
    std::string dir = snakeWithId(state, 0).value("dir", std::string{});
    REQUIRE_FALSE(dir.empty());

    // Pick a direction perpendicular to the current heading so it is never a
    // reversal into the neck (which the engine legitimately drops). The server,
    // not the client, owns the snake's position; the client sends only intent.
    bool horiz = (dir == "left" || dir == "right");
    const char* want = horiz ? "up" : "left";
    c.sendText(std::string(R"({"type":"input","dir":")") + want + "\"}");

    json after = lastOfType(parseAll(c.collect(400)), "state");
    REQUIRE_FALSE(after.is_null());
    CHECK(snakeWithId(after, 0).value("dir", std::string{}) == want);
}

TEST_CASE("two clients share one arena") {
    SnakeServer s;
    WsClient a;
    WsClient b;
    REQUIRE(a.connect(s.port()));
    REQUIRE(b.connect(s.port()));
    a.collect(50);
    b.collect(50);

    a.sendText(R"({"type":"join","seat":""})");
    b.sendText(R"({"type":"join","seat":""})");

    // From A's view, the broadcast world holds both snakes (ids 0 and 1) — one
    // shared, server-authoritative arena (DESIGN-snake §5/§6).
    json state = lastOfType(parseAll(a.collect(400)), "state");
    REQUIRE_FALSE(state.is_null());
    CHECK_FALSE(snakeWithId(state, 0).is_null());
    CHECK_FALSE(snakeWithId(state, 1).is_null());
}

TEST_CASE("hostile JSON cannot crash the single-threaded snake server") {
    SnakeServer s;
    const char* hostile[] = {
        R"({"type":123})",
        R"({"type":"input","dir":42})",
        R"({"type":"input","dir":["up"]})",
        R"({"type":"input"})",
        "not json at all",
        "[]",
        "12345",
    };
    for (const char* payload : hostile) {
        WsClient c;
        REQUIRE(c.connect(s.port()));
        c.collect(30);
        c.sendText(payload);
        c.collect(30);
        c.disconnect();
    }
    // After the barrage a normal client still joins and sees its snake.
    WsClient good;
    REQUIRE(good.connect(s.port()));
    good.collect(30);
    good.sendText(R"({"type":"join","seat":""})");
    auto msgs = parseAll(good.collect(300));
    CHECK(hasType(msgs, "joined"));
    CHECK(hasType(msgs, "state"));
}
