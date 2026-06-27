// Villen — the `snake` engine: the pure, server-authoritative world (DESIGN-snake
// §3). No SDL, no GL, no sockets, no wall clock: `step()` is a deterministic
// function of `(World, inputs)`, so CI can replay a fixed input script over a
// fixed seed and assert the exact grid (the same oracle discipline as chess
// legality and the filter reference, DESIGN-snake §3 "Determinism & tests").
//
// This is the port target: the upstream `snake2` simulation (grid + snakes + food
// + tick logic) lifted out of its SDL/OpenGL app shell (DESIGN-snake §2). Two
// rules change for a kids-friendly arena (DESIGN-snake §3): edges WRAP instead of
// killing, and collisions are lenient (respawn the shorter snake small, or off).
#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "villen/snake/types.hpp"

namespace villen::snake {

// What happens when a snake's head runs into a body (its own or another's). Data,
// so the operator can soften it live (DESIGN-snake §3/§8). Edges never kill — that
// is the `wrap` rule, separate from this.
enum class Collisions {
    Off,      // free play: snakes pass through each other and themselves (youngest)
    Respawn,  // forgiving default: the shorter snake respawns small, nobody "loses"
};

// The tunable rule set (DESIGN-snake §3 "kids-friendly rules, as data"). The
// operator edits these from the admin console (§8); the pure engine just obeys.
struct Rules {
    int w = 32;
    int h = 20;
    bool wrap = true;  // edges wrap; hitting one is not death
    Collisions collisions = Collisions::Respawn;
    int targetFood = 3;  // maintain this many foods so the board never feels empty
    int startLen = 3;    // length a freshly spawned / respawned snake starts at
    int maxLen = 64;     // growth is bounded so one expert can't fill the arena
    std::uint64_t seed = 0x9E3779B97F4A7C15ull;  // deterministic RNG seed
};

// One snake. `body[0]` is the head; the tail is `body.back()`. AI snakes carry a
// `nav` algorithm and are stepped by the engine itself (DESIGN-snake §7); player
// snakes take their direction from the per-tick input map.
struct Snake {
    int id = 0;
    bool ai = false;
    NavType nav = NavType::AStar;
    std::vector<ix2> body;
    Dir dir = Dir::Right;
    bool alive = true;
    int score = 0;

    int len() const { return static_cast<int>(body.size()); }
    const ix2& head() const { return body.front(); }
};

class World {
 public:
    // Build an empty arena (no snakes) seeded from the rules. Snakes join later via
    // add(); food is placed up to rules.targetFood immediately.
    static World create(const Rules&);

    // Advance the world exactly one authoritative tick (DESIGN-snake §4). For each
    // alive snake: pick its direction (AI computes its own; a player snake takes
    // its id's entry from `inputs`, ignored if it would reverse into the neck),
    // move one cell with wrap, eat, then resolve collisions per the rule set.
    // Deterministic in (World, inputs): the only randomness is the embedded RNG,
    // which is part of the World's state.
    void step(const std::unordered_map<int, Dir>& inputs);

    // --- membership (the host adapter drives these on join/leave/admin) ---------
    // Spawn a snake of `startLen` at a free cell. Returns nullptr if `id` already
    // exists or the board has no room. Player ids are the seat index; AI ids are
    // an engine-assigned range disjoint from seats (DESIGN-snake §6/§7).
    Snake* add(int id, bool ai, NavType nav = NavType::AStar);
    void remove(int id);
    Snake* byId(int id);
    const Snake* byId(int id) const;

    // --- admin-tunable state (DESIGN-snake §8) ---------------------------------
    // Resize the grid: snakes are re-placed small in the new bounds and food is
    // re-seeded (old coordinates may be out of range). Cheap rule tweaks (wrap,
    // collisions, targetFood, speed) don't need this — mutate rules() directly.
    void resize(int w, int h);
    // New game: respawn every snake small, zero scores, re-seed food, reset tick.
    void reset();

    // --- queries / accessors ----------------------------------------------------
    int w() const { return rules_.w; }
    int h() const { return rules_.h; }
    unsigned tick() const { return tick_; }
    const Rules& rules() const { return rules_; }
    Rules& rules() { return rules_; }  // mutable for live, non-resizing admin tweaks
    const std::vector<Snake>& snakes() const { return snakes_; }
    const std::vector<ix2>& food() const { return food_; }
    int aliveCount() const;
    int aiCount() const;

    // True if any snake segment occupies `c` (used by AI pathfinding, §7).
    bool occupied(ix2 c) const;
    // Map a cell back into the grid when wrap is on; clamp is never needed because
    // movement is one cell at a time, but out-of-range inputs are folded safely.
    ix2 wrapCell(ix2 c) const;

 private:
    Rules rules_;
    std::vector<Snake> snakes_;
    std::vector<ix2> food_;
    unsigned tick_ = 0;
    std::uint64_t rng_ = 0;

    std::uint64_t nextRand();             // splitmix64 step — deterministic
    bool findFreeCell(ix2& out);          // a uniformly-random empty cell, or false
    void placeSnakeAt(Snake&, ix2 head);  // lay a startLen body from a head cell
    bool spawnSnake(Snake&);              // place at a free cell; false if board full
    void maintainFood();                  // top up to rules_.targetFood
    void resolveCollisions();             // the lenient Respawn pass (§3)
};

}  // namespace villen::snake
