// Villen — the `snake` engine: the authoritative simulation (DESIGN-snake §3/§4).
//
// Ported from the upstream `snake2` game logic (`snake2/snake2.cpp`'s
// `SnakeGameLogic`), with the two kids-friendly rule changes the design calls for
// (DESIGN-snake §3): edges wrap rather than kill, and collisions are lenient. The
// upstream stepped snakes one at a time with a "movementPaused" flag; Villen steps
// every snake simultaneously so a shared multiplayer tick is fair and order-
// independent — heads move together, then collisions resolve.
//
// Everything here is deterministic in (World, inputs): the only randomness is the
// embedded splitmix64 RNG, whose state lives in the World, so a fixed seed + a
// fixed input script reproduce the exact grid (DESIGN-snake §3 tests).
#include "villen/snake/world.hpp"

#include <algorithm>
#include <numeric>

#include "villen/snake/pathfinding.hpp"

namespace villen::snake {

World World::create(const Rules& rules) {
    World w;
    w.rules_ = rules;
    w.rng_ = rules.seed;
    w.tick_ = 0;
    w.maintainFood();
    return w;
}

std::uint64_t World::nextRand() {
    // splitmix64 — a tiny, well-distributed deterministic generator. No <random>
    // engine: this must produce identical sequences across compilers/platforms so
    // the replay tests (and a Deck vs CI run) agree bit-for-bit.
    rng_ += 0x9E3779B97F4A7C15ull;
    std::uint64_t z = rng_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

ix2 World::wrapCell(ix2 c) const {
    if (!rules_.wrap) {
        return c;
    }
    const int W = rules_.w;
    const int H = rules_.h;
    int x = ((c.x % W) + W) % W;
    int y = ((c.y % H) + H) % H;
    return {x, y};
}

bool World::occupied(ix2 c) const {
    for (const Snake& s : snakes_) {
        for (const ix2& seg : s.body) {
            if (seg == c) {
                return true;
            }
        }
    }
    return false;
}

bool World::findFreeCell(ix2& out) {
    const int W = rules_.w;
    const int H = rules_.h;
    // Mark every occupied cell (snake bodies + existing food), then pick uniformly
    // among the empties. O(W*H) but the arena is tiny; correctness over cleverness.
    std::vector<char> blocked(static_cast<std::size_t>(W) * H, 0);
    auto mark = [&](ix2 c) {
        if (c.x >= 0 && c.x < W && c.y >= 0 && c.y < H) {
            blocked[static_cast<std::size_t>(c.y) * W + c.x] = 1;
        }
    };
    for (const Snake& s : snakes_) {
        for (const ix2& seg : s.body) {
            mark(seg);
        }
    }
    for (const ix2& f : food_) {
        mark(f);
    }
    std::vector<ix2> empties;
    empties.reserve(blocked.size());
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            if (!blocked[static_cast<std::size_t>(y) * W + x]) {
                empties.push_back({x, y});
            }
        }
    }
    if (empties.empty()) {
        return false;
    }
    out = empties[nextRand() % empties.size()];
    return true;
}

void World::placeSnakeAt(Snake& s, ix2 head) {
    s.body.clear();
    s.alive = true;
    const int W = rules_.w;
    const int H = rules_.h;
    // Lay the body trailing behind the head, opposite the travel direction, so the
    // snake starts mid-stride (matches the upstream's three-segment seed). Fold each
    // cell into the grid unconditionally: a spawn is placement, not gameplay
    // movement, so it must land on valid cells even when the wrap *rule* is off.
    ix2 back = delta(s.dir);
    for (int i = 0; i < rules_.startLen; ++i) {
        int x = ((head.x - back.x * i) % W + W) % W;
        int y = ((head.y - back.y * i) % H + H) % H;
        s.body.push_back({x, y});
    }
}

bool World::spawnSnake(Snake& s) {
    ix2 head;
    if (!findFreeCell(head)) {
        return false;  // board full: the caller decides (fail an add, keep a respawn)
    }
    // Give a (re)spawn a fresh, varied heading so two of them don't line up.
    s.dir = static_cast<Dir>(nextRand() % 4);
    placeSnakeAt(s, head);
    return true;
}

Snake* World::add(int id, bool ai, NavType nav) {
    if (byId(id)) {
        return nullptr;
    }
    Snake s;
    s.id = id;
    s.ai = ai;
    s.nav = nav;
    s.score = 0;
    if (!spawnSnake(s)) {
        return nullptr;  // no room — honour the world.hpp contract, never overlap-spawn
    }
    snakes_.push_back(std::move(s));
    return &snakes_.back();
}

void World::remove(int id) {
    snakes_.erase(
        std::remove_if(snakes_.begin(), snakes_.end(), [id](const Snake& s) { return s.id == id; }),
        snakes_.end());
}

Snake* World::byId(int id) {
    for (Snake& s : snakes_) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

const Snake* World::byId(int id) const {
    for (const Snake& s : snakes_) {
        if (s.id == id) {
            return &s;
        }
    }
    return nullptr;
}

void World::maintainFood() {
    // Always something to eat (DESIGN-snake §3): top up to the target count, but
    // never spin forever if the board is full.
    while (static_cast<int>(food_.size()) < rules_.targetFood) {
        ix2 c;
        if (!findFreeCell(c)) {
            break;
        }
        food_.push_back(c);
    }
}

void World::step(const std::unordered_map<int, Dir>& inputs) {
    ++tick_;
    const int W = rules_.w;
    const int H = rules_.h;
    const std::size_t n = snakes_.size();

    // 1) Resolve each alive snake's direction. AI snakes choose their own
    //    (DESIGN-snake §7); a player snake takes its id's queued input, rejected
    //    if it would reverse into its own neck (the input buffer kept only the
    //    latest, §4). A length-1 snake has no neck, so any turn is allowed.
    for (Snake& s : snakes_) {
        if (!s.alive) {
            continue;
        }
        Dir want = s.dir;
        if (s.ai) {
            want = aiDirection(*this, s);
        } else {
            auto it = inputs.find(s.id);
            if (it != inputs.end()) {
                want = it->second;
            }
        }
        if (s.len() <= 1 || !opposite(want, s.dir)) {
            s.dir = want;
        }
    }

    // 2) Compute each snake's new head. With wrap on, an edge folds to the far
    //    side; with wrap off, stepping off the edge simply blocks the move (the
    //    snake holds position this tick — the upstream's gentle "paused" behaviour)
    //    rather than dying.
    std::vector<ix2> newHead(n);
    std::vector<char> moved(n, 0);
    for (std::size_t i = 0; i < n; ++i) {
        Snake& s = snakes_[i];
        if (!s.alive) {
            continue;
        }
        ix2 d = delta(s.dir);
        ix2 raw = {s.head().x + d.x, s.head().y + d.y};
        if (rules_.wrap) {
            newHead[i] = wrapCell(raw);
            moved[i] = 1;
        } else if (raw.x < 0 || raw.x >= W || raw.y < 0 || raw.y >= H) {
            moved[i] = 0;  // blocked by the wall — hold position
        } else {
            newHead[i] = raw;
            moved[i] = 1;
        }
    }

    // Deterministic processing order: ascending snake id, so when two snakes reach
    // the same food the lower id eats it — independent of join order in the vector.
    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return snakes_[a].id < snakes_[b].id; });

    // 3) Eat resolution: each food is taken by at most one snake (lowest id wins).
    std::vector<char> consumed(food_.size(), 0);
    std::vector<char> ate(n, 0);
    for (std::size_t idx : order) {
        Snake& s = snakes_[idx];
        if (!s.alive || !moved[idx]) {
            continue;
        }
        for (std::size_t f = 0; f < food_.size(); ++f) {
            if (!consumed[f] && food_[f] == newHead[idx]) {
                consumed[f] = 1;
                ate[idx] = 1;
                ++s.score;
                break;
            }
        }
    }

    // 4) Apply movement: push the new head, drop the tail unless the snake ate
    //    (and isn't already at the length cap, so growth stays bounded, §3).
    for (std::size_t i = 0; i < n; ++i) {
        Snake& s = snakes_[i];
        if (!s.alive || !moved[i]) {
            continue;
        }
        s.body.insert(s.body.begin(), newHead[i]);
        if (ate[i] && s.len() <= rules_.maxLen) {
            // grew: keep the tail
        } else {
            s.body.pop_back();
        }
    }

    // Compact out the eaten food, then refill.
    if (!consumed.empty()) {
        std::vector<ix2> remaining;
        remaining.reserve(food_.size());
        for (std::size_t f = 0; f < food_.size(); ++f) {
            if (!consumed[f]) {
                remaining.push_back(food_[f]);
            }
        }
        food_ = std::move(remaining);
    }

    // 5) Lenient collisions (DESIGN-snake §3). In Off mode snakes pass through each
    //    other and themselves; otherwise the loser respawns small.
    if (rules_.collisions == Collisions::Respawn) {
        resolveCollisions();
    }

    // 6) Keep the board fed.
    maintainFood();
}

void World::resolveCollisions() {
    const int W = rules_.w;
    const std::size_t n = snakes_.size();
    auto key = [&](ix2 c) { return c.y * W + c.x; };

    std::vector<std::size_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(),
              [&](std::size_t a, std::size_t b) { return snakes_[a].id < snakes_[b].id; });

    // Cell -> the (lowest-id) snake whose *body* (a non-head segment) sits there,
    // and cell -> the ids of every snake whose *head* sits there (for head-to-head).
    std::unordered_map<int, int> bodyOwner;
    std::unordered_map<int, std::vector<int>> headsAt;
    for (std::size_t idx : order) {
        const Snake& s = snakes_[idx];
        if (!s.alive) {
            continue;
        }
        headsAt[key(s.head())].push_back(s.id);
        for (std::size_t i = 1; i < s.body.size(); ++i) {
            int k = key(s.body[i]);
            if (bodyOwner.find(k) == bodyOwner.end()) {
                bodyOwner[k] = s.id;
            }
        }
    }

    std::vector<int> toRespawn;
    for (std::size_t idx : order) {
        Snake& s = snakes_[idx];
        if (!s.alive) {
            continue;
        }
        bool hit = false;

        // Self-collision: the head landed on one of the snake's own segments.
        for (std::size_t i = 1; i < s.body.size(); ++i) {
            if (s.body[i] == s.head()) {
                hit = true;
                break;
            }
        }

        // Head-to-head: the shorter snake respawns; a clean tie respawns both, so
        // nobody is left stuck (DESIGN-snake §3 "nobody spectates their own death").
        if (!hit) {
            const std::vector<int>& hv = headsAt[key(s.head())];
            if (hv.size() > 1) {
                int maxLen = 0;
                int countAtMax = 0;
                for (int id : hv) {
                    int l = byId(id)->len();
                    if (l > maxLen) {
                        maxLen = l;
                        countAtMax = 1;
                    } else if (l == maxLen) {
                        ++countAtMax;
                    }
                }
                if (s.len() < maxLen || (s.len() == maxLen && countAtMax > 1)) {
                    hit = true;
                }
            }
        }

        // Head-into-body: running into another snake. The shorter (or equal) snake
        // respawns; a clearly longer snake ploughs through (big kids win, §3).
        if (!hit) {
            auto it = bodyOwner.find(key(s.head()));
            if (it != bodyOwner.end() && it->second != s.id) {
                int otherLen = byId(it->second)->len();
                if (s.len() <= otherLen) {
                    hit = true;
                }
            }
        }

        if (hit) {
            toRespawn.push_back(s.id);
        }
    }

    for (int id : toRespawn) {
        Snake* s = byId(id);
        if (s) {
            spawnSnake(*s);  // small again, score kept — forgiving, not punishing
        }
    }
}

void World::resize(int w, int h) {
    rules_.w = std::max(4, w);
    rules_.h = std::max(4, h);
    // Old coordinates may now be out of range, so re-place every snake small and
    // re-seed the food in the new bounds.
    food_.clear();
    for (Snake& s : snakes_) {
        s.body.clear();  // force a re-place: old coords may be out of the new bounds
        if (!spawnSnake(s)) {
            placeSnakeAt(s, {rules_.w / 2, rules_.h / 2});  // full board: center, not stale
        }
    }
    maintainFood();
}

void World::reset() {
    rng_ = rules_.seed;  // a "new game" is reproducible
    tick_ = 0;
    food_.clear();
    for (Snake& s : snakes_) {
        s.score = 0;
        s.body.clear();  // force a re-place into the bounds, never keep stale coords
        if (!spawnSnake(s)) {
            placeSnakeAt(s, {rules_.w / 2, rules_.h / 2});  // full board: center fallback
        }
    }
    maintainFood();
}

int World::aliveCount() const {
    int c = 0;
    for (const Snake& s : snakes_) {
        if (s.alive) {
            ++c;
        }
    }
    return c;
}

int World::aiCount() const {
    int c = 0;
    for (const Snake& s : snakes_) {
        if (s.ai) {
            ++c;
        }
    }
    return c;
}

}  // namespace villen::snake
