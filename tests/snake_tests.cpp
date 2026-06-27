// Unit tests for the pure `snake` simulation (DESIGN-snake.md §3 "Determinism &
// tests", §10 acceptance #1). GPU-free, host-free, headless — the same always-on
// CI discipline as the chess and filter engine tests. The design calls for "a
// fixed input script over a fixed seed [asserting] the exact grid"; the
// determinism case below is exactly that, and the rest pin the kids-friendly rule
// changes (wrap, lenient collisions, always-food) and the A* AI mover.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <unordered_map>

#include "villen/snake/pathfinding.hpp"
#include "villen/snake/types.hpp"
#include "villen/snake/world.hpp"

using namespace villen::snake;

namespace {

// The 180° reverse of a direction (for the reversal-guard test).
Dir reverseOf(Dir d) {
    for (Dir k : {Dir::Up, Dir::Down, Dir::Left, Dir::Right}) {
        if (opposite(d, k)) {
            return k;
        }
    }
    return d;
}

// A clockwise 90° turn (screen-space, y-down). Applying it every tick walks the
// head around a 4-cell loop — a guaranteed self-overlap for a longer snake.
Dir turnCW(Dir d) {
    switch (d) {
        case Dir::Up:
            return Dir::Right;
        case Dir::Right:
            return Dir::Down;
        case Dir::Down:
            return Dir::Left;
        case Dir::Left:
            return Dir::Up;
    }
    return d;
}

std::unordered_map<int, Dir> input(int id, Dir d) {
    return {{id, d}};
}

// Whole-snake / whole-world equality, for the exact-replay determinism check.
bool sameSnake(const Snake& a, const Snake& b) {
    return a.id == b.id && a.ai == b.ai && a.dir == b.dir && a.alive == b.alive &&
           a.score == b.score && a.body == b.body;
}

bool sameWorld(const World& a, const World& b) {
    if (a.tick() != b.tick() || a.food() != b.food()) {
        return false;
    }
    if (a.snakes().size() != b.snakes().size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.snakes().size(); ++i) {
        if (!sameSnake(a.snakes()[i], b.snakes()[i])) {
            return false;
        }
    }
    return true;
}

bool inBounds(const World& w, ix2 c) {
    return c.x >= 0 && c.x < w.w() && c.y >= 0 && c.y < w.h();
}

}  // namespace

TEST_CASE("a snake moving off an edge wraps to the opposite side, never dying") {
    Rules r;
    r.w = 8;
    r.h = 8;
    r.targetFood = 0;  // no food, no growth — isolate the wrap
    r.startLen = 3;
    r.collisions = Collisions::Off;
    World w = World::create(r);
    Snake* s = w.add(0, false);
    REQUIRE(s != nullptr);

    // Drive in the snake's *current* heading (never a reversal): after one full lap
    // of the torus the head returns exactly to where it started.
    Dir d = s->dir;
    ix2 start = s->head();
    int lap = (d == Dir::Up || d == Dir::Down) ? r.h : r.w;
    for (int i = 0; i < lap; ++i) {
        w.step(input(0, d));
        for (const ix2& seg : w.byId(0)->body) {
            CHECK(inBounds(w, seg));  // wrap keeps every cell on the grid
        }
    }
    CHECK(w.byId(0)->head() == start);
    CHECK(w.byId(0)->alive);  // an edge is never game over (DESIGN-snake §3)
}

TEST_CASE("a snake cannot reverse into its own neck") {
    Rules r;
    r.w = 12;
    r.h = 12;
    r.targetFood = 0;
    r.collisions = Collisions::Off;
    World w = World::create(r);
    Snake* s = w.add(0, false);
    REQUIRE(s->len() > 1);

    Dir d = s->dir;
    ix2 before = s->head();
    w.step(input(0, reverseOf(d)));  // try to U-turn

    // The reversal is dropped: the snake keeps heading `d` and advances one cell.
    CHECK(w.byId(0)->dir == d);
    ix2 expect = w.wrapCell({before.x + delta(d).x, before.y + delta(d).y});
    CHECK(w.byId(0)->head() == expect);
}

TEST_CASE("the board is kept topped up to the target food count") {
    Rules r;
    r.w = 10;
    r.h = 10;
    r.targetFood = 4;
    r.collisions = Collisions::Off;
    World w = World::create(r);
    CHECK(static_cast<int>(w.food().size()) == 4);  // seeded at creation

    Snake* s = w.add(0, false);
    Dir d = s->dir;
    for (int i = 0; i < 40; ++i) {
        w.step(input(0, d));
        // Whatever gets eaten is immediately replaced (DESIGN-snake §3).
        CHECK(static_cast<int>(w.food().size()) == 4);
    }
}

TEST_CASE("lenient collisions: Respawn recovers a self-bite, Off passes through") {
    auto runLoop = [](Collisions mode) {
        Rules r;
        r.w = 16;
        r.h = 16;
        r.targetFood = 0;
        r.startLen = 8;  // long enough that a tight 4-cell loop must self-overlap
        r.collisions = mode;
        r.seed = 777;
        World w = World::create(r);
        Snake* s = w.add(0, false);

        bool teleported = false;
        ix2 prev = s->head();
        for (int i = 0; i < 12; ++i) {
            Dir next = turnCW(w.byId(0)->dir);
            w.step(input(0, next));
            ix2 now = w.byId(0)->head();
            // A normal move (incl. a wrap) shifts the head exactly one cell; a jump
            // farther than that is a respawn placing the snake elsewhere.
            if (gridDistance(w, prev, now) > 1) {
                teleported = true;
            }
            prev = now;
            CHECK(w.byId(0)->alive);  // never dead — leniency, not death (§3)
        }
        return teleported;
    };

    CHECK(runLoop(Collisions::Respawn));    // the self-bite respawns it small
    CHECK_FALSE(runLoop(Collisions::Off));  // free play: it slides through itself
}

TEST_CASE("add() refuses (returns nullptr) once the board has no room") {
    Rules r;
    r.w = 4;
    r.h = 4;  // 16 cells
    r.targetFood = 0;
    r.startLen = 3;
    r.collisions = Collisions::Off;
    World w = World::create(r);

    int added = 0;
    bool refused = false;
    for (int i = 0; i < 50; ++i) {
        if (w.add(i, false)) {
            ++added;
        } else {
            refused = true;
            break;
        }
    }
    // The board fills and add() honours its contract rather than overlap-spawning.
    CHECK(added > 0);
    CHECK(refused);
    // Every placed snake is on valid, distinct-from-out-of-range cells.
    for (const Snake& s : w.snakes()) {
        for (const ix2& c : s.body) {
            CHECK(inBounds(w, c));
        }
    }
}

TEST_CASE("resizing to a board too small to fit every snake keeps all in bounds") {
    Rules r;
    r.w = 20;
    r.h = 20;
    r.targetFood = 0;
    r.startLen = 3;
    r.collisions = Collisions::Off;
    World w = World::create(r);
    for (int i = 0; i < 8; ++i) {
        w.add(i, false);  // 8 snakes * 3 = 24 cells, more than a 4x4 board holds
    }

    w.resize(4, 4);  // forces the free-cell search to fail for some snakes

    // Every segment must land inside the new bounds — a snake must never keep its
    // old, now-out-of-range coordinates (the resize re-place must always run).
    for (const Snake& s : w.snakes()) {
        for (const ix2& c : s.body) {
            CHECK(inBounds(w, c));
        }
    }
}

TEST_CASE("an A* AI snake navigates to food and scores on its own clock") {
    Rules r;
    r.w = 16;
    r.h = 16;
    r.targetFood = 1;
    r.startLen = 3;
    r.collisions = Collisions::Off;  // isolate navigation from respawns
    r.seed = 4242;
    World w = World::create(r);
    Snake* ai = w.add(0, true, NavType::AStar);
    REQUIRE(ai != nullptr);
    REQUIRE(ai->ai);

    // With no player input at all, the world advances on its own and the AI hunts
    // the food (DESIGN-snake §7) — the authoritative-clock property (§4).
    for (int i = 0; i < 200; ++i) {
        w.step({});
    }
    CHECK(w.byId(0)->score > 0);
    CHECK(w.tick() == 200u);
}

TEST_CASE("step() is deterministic: same seed + same script => identical grid") {
    auto build = []() {
        Rules r;
        r.seed = 0xC0FFEE;
        r.w = 16;
        r.h = 12;
        r.targetFood = 3;
        r.collisions = Collisions::Respawn;
        World w = World::create(r);
        w.add(0, false);                 // a player
        w.add(1, true, NavType::AStar);  // and an AI, exercising every code path
        return w;
    };

    World a = build();
    World b = build();
    REQUIRE(sameWorld(a, b));  // identical construction

    for (int t = 0; t < 80; ++t) {
        Dir d = std::vector<Dir>{Dir::Up, Dir::Right, Dir::Down, Dir::Left}[t % 4];
        a.step(input(0, d));
        b.step(input(0, d));
    }
    // The exact-grid oracle the design asks for: two independent runs of the same
    // script land on byte-identical state — snakes, food, scores, and tick.
    CHECK(sameWorld(a, b));
    CHECK(a.tick() == 80u);
}
