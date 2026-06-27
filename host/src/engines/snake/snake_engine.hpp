// Villen — SnakeEngine: a real-time, server-authoritative arena on the IEngine
// contract (DESIGN-snake.md).
//
// The fourth engine, and the first that moves on *its own clock*: chess advances
// only on a move, filter only on a frame, chat only on a prompt — all reactive.
// Snake's world ticks ~8-12 times a second whether or not anyone presses anything
// (DESIGN-snake §4). That is the one thing the §5 single loop had never been asked
// to carry, and it carries it the cleanest possible way: the simulation is cheap,
// so it runs *directly* in the loop (no worker thread, no queue — unlike chat),
// and the tick is just the loop noticing wall-time crossed a fixed-timestep
// boundary in onTick().
//
// Like chess (and unlike filter/chat) snake is a *shared world*, so it BROADCASTS
// one authoritative state to everyone each tick (DESIGN-snake §5). Authority is
// the carried-over invariant: the client sends intent (a direction), never a
// position — there is no client-trusted state to forge (§5). The deterministic
// simulation it drives is the pure villen::snake library (engine/snake/, §3); the
// A* AI snakes are pure too (§7), just another input source feeding step().
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

#include "engine.hpp"
#include "villen/snake/world.hpp"

namespace villen {

class SnakeEngine : public IEngine {
 public:
    SnakeEngine();

    // N symmetric seats, no turns: each join allocates a snake (DESIGN-snake §6).
    // The roster size is the operator player cap; the grid comfortably fits these.
    SeatRoster seats() const override;

    void onJoin(Room&, ConnId, SeatId) override;
    void onLeave(Room&, ConnId, SeatId) override;
    void onMessage(Room&, ConnId, SeatId, std::string_view) override;  // {input,dir}
    void onTick(Room&, std::uint64_t nowMs) override;  // the authoritative clock (§4)
    std::string statusLine() const override;
    void drawAdmin() override;
    void reset() override;  // admin "new game"

 private:
    snake::World world_;

    // Per-player latest queued direction (DESIGN-snake §4 "input buffering"): a
    // player may send several directions between ticks; we keep only the latest
    // legal one, applied at the next step(). Keyed by snake id (== seat index).
    std::unordered_map<int, snake::Dir> pendingInputs_;

    // Fixed-timestep accumulator (DESIGN-snake §4). tickMs_ is the sim period
    // (~8-12 Hz); the admin window still renders at ~60 Hz and the browser
    // animates between the discrete states it receives — sim rate != render rate.
    std::uint64_t tickMs_ = 100;   // 10 Hz default
    std::uint64_t accMs_ = 0;      // wall time owed to the simulation
    std::uint64_t lastNowMs_ = 0;  // previous onTick clock
    bool clockInit_ = false;       // first onTick just seeds lastNowMs_
    bool paused_ = false;          // admin pause: hold the world (§8)

    // AI snakes get ids in a range disjoint from the seat indices, so a player and
    // an AI never collide on an id (DESIGN-snake §6/§7).
    static constexpr int kAiIdBase = 100;
    int nextAiId_ = kAiIdBase;
    snake::NavType pendingNav_ = snake::NavType::AStar;  // admin "add AI" selector

    // Admin grid-size editor: edited live, applied (a resize) on a button so a
    // slider drag doesn't re-seed the board every frame.
    int pendingW_ = 32;
    int pendingH_ = 20;

    std::uint64_t catchUpCapMs() const { return tickMs_ * 3; }  // clamp (§4)

    void broadcastState(Room&);
    std::string stateMsg() const;
    std::string configMsg() const;
    std::string youMsg(SeatId) const;
    void pushConfig();  // re-broadcast config after an admin change (via room_)
    void addAi(snake::NavType);
    void removeLastAi();
};

class SnakeFactory : public IEngineFactory {
 public:
    std::unique_ptr<IEngine> create() override { return std::make_unique<SnakeEngine>(); }
    const char* name() const override { return "snake"; }
    // The snake view lives in client/snake/; when snake is active the host serves
    // that subdir as the static root (admin-shell §5, the per-engine client model).
    const char* clientDir() const override { return "snake"; }
};

}  // namespace villen
