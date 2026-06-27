#include "snake_engine.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <string>

#include "room.hpp"
#include "villen/snake/pathfinding.hpp"
#include "villen/snake/types.hpp"

#ifdef VILLEN_ADMIN_UI
#include "imgui.h"
#endif

using json = nlohmann::json;

namespace villen {
namespace {

// The seat roster — the operator player cap (DESIGN-snake §6). Eight comfortably
// fit the default 32x20 arena. Player snake id == seat index, so "p1" is id 0.
constexpr int kSeats = 8;

// Read a string field from untrusted client JSON without throwing — a wrong type
// or missing key yields "" rather than a json::type_error that would crash the
// single-threaded server (the DoS discipline shared with chess/filter/envelope).
std::string strField(const json& j, const char* key) {
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// A friendly, high-contrast colour per snake (DESIGN-snake §8 "big, friendly").
// Players cycle a bright palette by id; AI snakes are a muted grey so they read as
// "not a kid".
const char* colorFor(int id, bool ai) {
    if (ai) {
        return "#9aa3ad";
    }
    static const std::array<const char*, kSeats> kPalette = {
        "#3399dd", "#e0533b", "#3fb950", "#d2a106", "#a371f7", "#ec6cb9", "#2dd4bf", "#f0883e",
    };
    return kPalette[static_cast<std::size_t>(id) % kPalette.size()];
}

const char* collisionsName(snake::Collisions c) {
    switch (c) {
        case snake::Collisions::Off:
            return "off";
        case snake::Collisions::Respawn:
            return "respawn";
    }
    return "respawn";
}

}  // namespace

SnakeEngine::SnakeEngine() {
    snake::Rules rules;  // defaults: 32x20, wrap on, lenient respawn (DESIGN-snake §3)
    world_ = snake::World::create(rules);
    pendingW_ = rules.w;
    pendingH_ = rules.h;
}

SeatRoster SnakeEngine::seats() const {
    SeatRoster r;
    for (int i = 0; i < kSeats; ++i) {
        r.names.push_back("p" + std::to_string(i + 1));
    }
    return r;
}

std::string SnakeEngine::configMsg() const {
    json msg = {
        {"type", "config"},
        {"w", world_.w()},
        {"h", world_.h()},
        {"tickMs", tickMs_},
        {"wrap", world_.rules().wrap},
        {"collisions", collisionsName(world_.rules().collisions)},
    };
    return msg.dump();
}

std::string SnakeEngine::youMsg(SeatId seat) const {
    // The joiner's own snake id, so the client can highlight "you". -1 = spectator.
    json msg = {{"type", "you"}, {"id", seat == kNoSeat ? -1 : static_cast<int>(seat)}};
    if (seat != kNoSeat) {
        msg["color"] = colorFor(seat, false);
    }
    return msg.dump();
}

std::string SnakeEngine::stateMsg() const {
    // The full authoritative snapshot — the only thing clients render from
    // (DESIGN-snake §5, the chess §6.2 rationale: full state is idempotent and
    // simple; deltas are a large-arena optimisation, §11).
    json snakes = json::array();
    for (const snake::Snake& s : world_.snakes()) {
        json cells = json::array();
        for (const snake::ix2& c : s.body) {
            cells.push_back({c.x, c.y});
        }
        std::string name = s.ai ? "AI" : ("p" + std::to_string(s.id + 1));
        snakes.push_back({
            {"id", s.id},
            {"cells", std::move(cells)},
            {"dir", snake::dirName(s.dir)},
            {"alive", s.alive},
            {"score", s.score},
            {"ai", s.ai},
            {"color", colorFor(s.id, s.ai)},
            {"name", std::move(name)},
        });
    }
    json food = json::array();
    for (const snake::ix2& f : world_.food()) {
        food.push_back({f.x, f.y});
    }
    json msg = {
        {"type", "state"}, {"tick", world_.tick()},       {"w", world_.w()},
        {"h", world_.h()}, {"snakes", std::move(snakes)}, {"food", std::move(food)},
    };
    return msg.dump();
}

void SnakeEngine::broadcastState(Room& room) {
    room.broadcast(stateMsg());
}

void SnakeEngine::pushConfig() {
    if (room_) {
        room_->broadcast(configMsg());
    }
}

void SnakeEngine::onJoin(Room& room, ConnId id, SeatId seat) {
    room.send(id, configMsg());
    if (seat != kNoSeat) {
        snake::Snake* s = world_.byId(seat);
        if (!s) {
            s = world_.add(seat, false);  // a fresh snake for this seat
        }
        if (s) {
            pendingInputs_.erase(seat);
            room.send(id, youMsg(seat));
            broadcastState(room);  // everyone sees the new snake appear
            return;
        }
        // add() can refuse on a full board (world.hpp contract): fall through and
        // treat this connection as a spectator until the arena has room again.
    }
    // A spectator (no seat, or a seat we couldn't place): watches, owns no snake (§12).
    room.send(id, youMsg(kNoSeat));
    room.send(id, stateMsg());
}

void SnakeEngine::onLeave(Room& room, ConnId, SeatId seat) {
    if (seat != kNoSeat) {
        world_.remove(seat);  // the dropped player's snake leaves the arena
        pendingInputs_.erase(seat);
        broadcastState(room);
    }
    // A spectator leaving changes nothing in the world.
}

void SnakeEngine::onMessage(Room&, ConnId, SeatId seat, std::string_view text) {
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        return;  // malformed -> ignore (the world keeps ticking regardless)
    }
    if (strField(j, "type") != "input") {
        return;  // unknown control message
    }
    // Authority: a direction only steers the snake derived from the connection's
    // seat — never a client-claimed id, and never from an unseated spectator
    // (DESIGN-snake §5).
    if (seat == kNoSeat) {
        return;
    }
    snake::Dir d;
    if (!snake::dirFromString(strField(j, "dir"), d)) {
        return;
    }
    snake::Snake* s = world_.byId(seat);
    if (!s) {
        return;
    }
    // Buffer only the latest *legal* direction (DESIGN-snake §4): drop a reversal
    // into the neck here so it can't overwrite a good queued turn with one step()
    // would only discard.
    if (s->len() <= 1 || !snake::opposite(d, s->dir)) {
        pendingInputs_[seat] = d;
    }
}

void SnakeEngine::onTick(Room& room, std::uint64_t nowMs) {
    // First call only seeds the clock — there is no "time since last iteration" yet.
    if (!clockInit_) {
        lastNowMs_ = nowMs;
        clockInit_ = true;
        return;
    }
    std::uint64_t dt = nowMs - lastNowMs_;
    lastNowMs_ = nowMs;
    if (paused_) {
        return;  // hold the world; don't let the accumulator build behind the pause
    }

    accMs_ += dt;
    // Clamp after a stall (the Deck can suspend/resume for far longer than a tick):
    // bound a long pause to one brief skip instead of a burst of catch-up steps —
    // the spiral of death (DESIGN-snake §4).
    if (accMs_ > catchUpCapMs()) {
        accMs_ = catchUpCapMs();
    }

    bool stepped = false;
    while (accMs_ >= tickMs_) {
        world_.step(pendingInputs_);
        pendingInputs_.clear();  // the latest direction was consumed; snake.dir holds
        accMs_ -= tickMs_;
        stepped = true;
    }
    if (stepped) {
        broadcastState(room);  // one snapshot per onTick; clients render the latest
    }
}

std::string SnakeEngine::statusLine() const {
    int players = world_.aliveCount() - world_.aiCount();
    std::string s = "snake - " + std::to_string(players) + " player(s), " +
                    std::to_string(world_.aiCount()) + " AI, " +
                    std::to_string(world_.food().size()) + " food";
    if (tickMs_ > 0) {
        s += " @ " + std::to_string(1000 / tickMs_) + " Hz";
    }
    if (paused_) {
        s += " [paused]";
    }
    return s;
}

void SnakeEngine::addAi(snake::NavType nav) {
    // Only advance the id on a successful add, so ids don't skip if the board is full.
    if (world_.add(nextAiId_, true, nav)) {
        ++nextAiId_;
    }
}

void SnakeEngine::removeLastAi() {
    int target = -1;
    for (const snake::Snake& s : world_.snakes()) {
        if (s.ai && s.id > target) {
            target = s.id;
        }
    }
    if (target >= 0) {
        world_.remove(target);
    }
}

void SnakeEngine::reset() {
    world_.reset();
    pendingInputs_.clear();
    accMs_ = 0;
    if (room_) {
        broadcastState(*room_);
    }
}

void SnakeEngine::drawAdmin() {
#ifdef VILLEN_ADMIN_UI
    // Only the engine's own body — the shell draws the chrome + join QR
    // (admin-shell §8). ASCII-only (default ImGui font). The operator runs the
    // playground: start/pause/reset, size, speed, wrap, leniency, AI (DESIGN-snake §8).
    ImGui::TextUnformatted(statusLine().c_str());
    ImGui::Spacing();

    if (ImGui::Button(paused_ ? "Resume" : "Pause")) {
        paused_ = !paused_;
    }
    ImGui::SameLine();
    if (ImGui::Button("New game")) {
        reset();
    }

    ImGui::SeparatorText("Arena");
    int tick = static_cast<int>(tickMs_);
    if (ImGui::SliderInt("tick ms", &tick, 50, 250)) {
        tickMs_ = static_cast<std::uint64_t>(tick < 1 ? 1 : tick);
        pushConfig();  // clients show the new rate; sim period changes immediately
    }

    bool wrap = world_.rules().wrap;
    if (ImGui::Checkbox("wrap edges", &wrap)) {
        world_.rules().wrap = wrap;
        pushConfig();
    }

    static const char* kCollide[] = {"off", "respawn"};
    int collide = static_cast<int>(world_.rules().collisions);
    if (ImGui::Combo("collisions", &collide, kCollide, IM_ARRAYSIZE(kCollide))) {
        world_.rules().collisions = static_cast<snake::Collisions>(collide);
        pushConfig();
    }

    int food = world_.rules().targetFood;
    if (ImGui::SliderInt("food", &food, 1, 12)) {
        world_.rules().targetFood = food;  // topped up to the new target next tick
    }

    ImGui::SliderInt("width", &pendingW_, 8, 64);
    ImGui::SliderInt("height", &pendingH_, 8, 48);
    ImGui::SameLine();
    if (ImGui::Button("Apply size")) {
        world_.resize(pendingW_, pendingH_);  // re-places snakes; food re-seeded
        pushConfig();
    }

    ImGui::SeparatorText("AI snakes");
    static const char* kNav[] = {"A*", "greedy", "random"};
    int nav = static_cast<int>(pendingNav_);
    if (ImGui::Combo("difficulty", &nav, kNav, IM_ARRAYSIZE(kNav))) {
        pendingNav_ = static_cast<snake::NavType>(nav);
    }
    if (ImGui::Button("+ AI")) {
        addAi(pendingNav_);
    }
    ImGui::SameLine();
    if (ImGui::Button("- AI")) {
        removeLastAi();
    }

    ImGui::SeparatorText("Snakes");
    if (ImGui::BeginTable("snakes", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        for (const char* c : {"who", "score", "len", "dir"}) {
            ImGui::TableSetupColumn(c);
        }
        ImGui::TableHeadersRow();
        for (const snake::Snake& s : world_.snakes()) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%s%d", s.ai ? "AI#" : "p", s.ai ? s.id : s.id + 1);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("%d", s.score);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%d", s.len());
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(snake::dirName(s.dir));
        }
        ImGui::EndTable();
    }
#endif
}

}  // namespace villen
