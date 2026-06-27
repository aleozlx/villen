// Villen — the `snake` engine: AI movers (DESIGN-snake §7).
//
// Ported from the upstream `algorithm/pathfinding.cpp`: the same A* and the same
// greedy/naive direction fallbacks, with two reshapes for Villen. (1) The
// occupancy callback + void* context becomes a plain occupancy grid built once
// from the World — O(1) lookups, no per-query World scan. (2) The grid is a torus
// (DESIGN-snake §3): neighbours and the heuristic wrap, so an AI chases food the
// short way round an edge. With wrap off, stepping off the grid is simply not a
// neighbour (a wall), mirroring the player rule.
#include "villen/snake/pathfinding.hpp"

#include <algorithm>
#include <climits>
#include <vector>

namespace villen::snake {
namespace {

constexpr Dir kDirs[4] = {Dir::Up, Dir::Down, Dir::Left, Dir::Right};

// All four cells one step from `c`, honouring wrap. Returns false for a cell that
// would leave the grid when wrap is off (a wall, not a neighbour).
bool stepCell(const World& w, ix2 c, Dir d, ix2& out) {
    ix2 raw = {c.x + delta(d).x, c.y + delta(d).y};
    if (w.rules().wrap) {
        out = w.wrapCell(raw);
        return true;
    }
    if (raw.x < 0 || raw.x >= w.w() || raw.y < 0 || raw.y >= w.h()) {
        return false;
    }
    out = raw;
    return true;
}

// A blocked-cell grid for one decision: every snake segment is an obstacle (as the
// upstream's tile grid had it). Indexed y*W + x.
std::vector<char> occupancyGrid(const World& w) {
    std::vector<char> blocked(static_cast<std::size_t>(w.w()) * w.h(), 0);
    for (const Snake& s : w.snakes()) {
        for (const ix2& seg : s.body) {
            if (seg.x >= 0 && seg.x < w.w() && seg.y >= 0 && seg.y < w.h()) {
                blocked[static_cast<std::size_t>(seg.y) * w.w() + seg.x] = 1;
            }
        }
    }
    return blocked;
}

bool isFree(const World& w, const std::vector<char>& blocked, ix2 c) {
    if (c.x < 0 || c.x >= w.w() || c.y < 0 || c.y >= w.h()) {
        return false;
    }
    return !blocked[static_cast<std::size_t>(c.y) * w.w() + c.x];
}

// The nearest food to `from` by toroidal distance, or false if the board is empty.
bool nearestFood(const World& w, ix2 from, ix2& out) {
    bool found = false;
    int best = INT_MAX;
    for (const ix2& f : w.food()) {
        int d = gridDistance(w, from, f);
        if (d < best) {
            best = d;
            out = f;
            found = true;
        }
    }
    return found;
}

// A* from head to goal on the (wrapping) grid; returns the first step's direction.
// Faithful to the upstream's simple min-f linear scan — fine on a 32x20 board, and
// it keeps the port obviously correct. `goal` (a food cell) is never blocked.
bool astarDir(const World& w, ix2 head, ix2 goal, const std::vector<char>& blocked, Dir& out) {
    const int W = w.w();
    const int H = w.h();
    const std::size_t N = static_cast<std::size_t>(W) * H;
    auto idx = [&](ix2 c) { return static_cast<std::size_t>(c.y) * W + c.x; };

    std::vector<int> g(N, INT_MAX);
    std::vector<int> cameDir(N, -1);  // direction taken to arrive at each cell
    std::vector<char> closed(N, 0);

    struct Node {
        ix2 c;
        int f;
        int h;
    };
    std::vector<Node> open;
    int h0 = gridDistance(w, head, goal);
    g[idx(head)] = 0;
    open.push_back({head, h0, h0});

    bool reached = false;
    while (!open.empty()) {
        // Lowest f (ties broken by lower h), as the upstream did.
        std::size_t bestI = 0;
        for (std::size_t i = 1; i < open.size(); ++i) {
            if (open[i].f < open[bestI].f ||
                (open[i].f == open[bestI].f && open[i].h < open[bestI].h)) {
                bestI = i;
            }
        }
        Node cur = open[bestI];
        open.erase(open.begin() + bestI);
        if (cur.c == goal) {
            reached = true;
            break;
        }
        if (closed[idx(cur.c)]) {
            continue;
        }
        closed[idx(cur.c)] = 1;

        for (Dir d : kDirs) {
            ix2 nb;
            if (!stepCell(w, cur.c, d, nb)) {
                continue;
            }
            if (closed[idx(nb)]) {
                continue;
            }
            if (!(nb == goal) && blocked[idx(nb)]) {
                continue;
            }
            int ng = g[idx(cur.c)] + 1;
            if (ng < g[idx(nb)]) {
                g[idx(nb)] = ng;
                cameDir[idx(nb)] = static_cast<int>(d);
                int hh = gridDistance(w, nb, goal);
                open.push_back({nb, ng + hh, hh});
            }
        }
    }

    if (!reached || cameDir[idx(goal)] < 0) {
        return false;
    }
    // Walk back from the goal to the head; the last direction stepped is the move
    // to make now.
    ix2 c = goal;
    Dir first = static_cast<Dir>(cameDir[idx(goal)]);
    while (!(c == head)) {
        int cd = cameDir[idx(c)];
        if (cd < 0) {
            return false;  // broken chain (shouldn't happen once reached)
        }
        first = static_cast<Dir>(cd);
        ix2 prev = w.wrapCell({c.x - delta(first).x, c.y - delta(first).y});
        c = prev;
    }
    out = first;
    return true;
}

// Greedy axis-first move toward goal (upstream calculateGreedyAxisPathDirection),
// wrap-aware: prefer the larger toroidal axis, never reverse into the neck, and
// fall back to any free move.
Dir greedyDir(const World& w, const Snake& self, ix2 goal, const std::vector<char>& blocked) {
    ix2 head = self.head();
    int dx = goal.x - head.x;
    int dy = goal.y - head.y;
    if (w.rules().wrap) {
        if (dx > w.w() / 2) {
            dx -= w.w();
        }
        if (dx < -w.w() / 2) {
            dx += w.w();
        }
        if (dy > w.h() / 2) {
            dy -= w.h();
        }
        if (dy < -w.h() / 2) {
            dy += w.h();
        }
    }

    std::vector<Dir> prefs;
    auto wantH = [&]() {
        if (dx > 0) {
            prefs.push_back(Dir::Right);
        } else if (dx < 0) {
            prefs.push_back(Dir::Left);
        }
    };
    auto wantV = [&]() {
        if (dy > 0) {
            prefs.push_back(Dir::Down);
        } else if (dy < 0) {
            prefs.push_back(Dir::Up);
        }
    };
    if (std::abs(dx) >= std::abs(dy)) {
        wantH();
        wantV();
    } else {
        wantV();
        wantH();
    }

    auto tryDir = [&](Dir d, bool allowReverse) -> bool {
        if (!allowReverse && self.len() > 1 && opposite(d, self.dir)) {
            return false;
        }
        ix2 nb;
        if (!stepCell(w, head, d, nb)) {
            return false;
        }
        return nb == goal || isFree(w, blocked, nb);
    };

    for (Dir d : prefs) {
        if (tryDir(d, false)) {
            return d;
        }
    }
    for (Dir d : kDirs) {  // any safe, non-reversing move
        if (tryDir(d, false)) {
            return d;
        }
    }
    for (Dir d : kDirs) {  // last resort: any free cell, even a reversal
        if (tryDir(d, true)) {
            return d;
        }
    }
    return self.dir;  // boxed in — keep going and let collision leniency sort it
}

// Random-but-safe: pick uniformly among the free, non-reversing moves, using a
// deterministic hash of (tick, id) so replays stay exact (no <random>, §3 tests).
Dir randomDir(const World& w, const Snake& self, const std::vector<char>& blocked) {
    std::vector<Dir> safe;
    for (Dir d : kDirs) {
        if (self.len() > 1 && opposite(d, self.dir)) {
            continue;
        }
        ix2 nb;
        if (stepCell(w, self.head(), d, nb) && isFree(w, blocked, nb)) {
            safe.push_back(d);
        }
    }
    if (safe.empty()) {
        return self.dir;
    }
    std::uint64_t z = static_cast<std::uint64_t>(w.tick()) * 0x9E3779B97F4A7C15ull +
                      static_cast<std::uint64_t>(self.id) * 0xD1B54A32D192ED03ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z ^= z >> 31;
    return safe[z % safe.size()];
}

}  // namespace

int gridDistance(const World& w, ix2 a, ix2 b) {
    int dx = std::abs(a.x - b.x);
    int dy = std::abs(a.y - b.y);
    if (w.rules().wrap) {
        dx = std::min(dx, w.w() - dx);
        dy = std::min(dy, w.h() - dy);
    }
    return dx + dy;
}

Dir aiDirection(const World& w, const Snake& self) {
    std::vector<char> blocked = occupancyGrid(w);

    ix2 goal;
    if (!nearestFood(w, self.head(), goal)) {
        // No food on the board: just keep moving somewhere safe.
        return greedyDir(w, self, self.head(), blocked);
    }

    switch (self.nav) {
        case NavType::AStar: {
            Dir d;
            if (astarDir(w, self.head(), goal, blocked, d)) {
                // Validate against the neck guard; A* won't suggest a reversal on a
                // body longer than 1, but stay defensive.
                if (self.len() <= 1 || !opposite(d, self.dir)) {
                    return d;
                }
            }
            return greedyDir(w, self, goal, blocked);  // A* failed -> greedy (upstream)
        }
        case NavType::Greedy:
            return greedyDir(w, self, goal, blocked);
        case NavType::Random:
            return randomDir(w, self, blocked);
    }
    return self.dir;
}

}  // namespace villen::snake
