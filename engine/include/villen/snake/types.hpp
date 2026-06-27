// Villen — the `snake` engine: fundamental value types (DESIGN-snake §3).
//
// Tiny, dependency-free, header-only: the `ix2` integer vector is reused from the
// upstream Snake (`include/snake_types.h`) as the design intends — "ix2 / fx3 value
// types: reused as-is" (DESIGN-snake §2). `Dir` and its helpers replace the
// upstream's raw `Point(1,0)`-style direction literals with a named enum so the
// wire protocol and the reversal guard read clearly.
//
// Coordinates are screen-space: x grows right, y grows DOWN (row index), so the
// browser canvas renders a cell at (x*size, y*size) with no flip. `Up` therefore
// decreases y. This differs from the upstream's y-up convention, but the
// simulation is symmetric under that choice, and screen-space keeps the client
// trivial.
#pragma once

#include <cstdint>
#include <string_view>

namespace villen::snake {

// 2D integer coordinate / grid cell. Same shape as the upstream `ix2`, minus the
// SDL-era union aliasing we don't need here.
struct ix2 {
    int x = 0;
    int y = 0;

    ix2() = default;
    ix2(int x, int y) : x(x), y(y) {}

    bool operator==(const ix2& o) const { return x == o.x && y == o.y; }
    bool operator!=(const ix2& o) const { return !(*this == o); }
};

// The four cardinal directions a snake can travel. No diagonal, no "stopped":
// every snake always moves one cell per tick (DESIGN-snake §3).
enum class Dir { Up, Down, Left, Right };

// Per-tick movement for a direction, in screen-space (y grows down).
inline ix2 delta(Dir d) {
    switch (d) {
        case Dir::Up:
            return {0, -1};
        case Dir::Down:
            return {0, 1};
        case Dir::Left:
            return {-1, 0};
        case Dir::Right:
            return {1, 0};
    }
    return {0, 0};
}

// Two directions are opposite when one is the 180° reverse of the other. The
// engine refuses a turn into the snake's own neck (DESIGN-snake §4).
inline bool opposite(Dir a, Dir b) {
    return (a == Dir::Up && b == Dir::Down) || (a == Dir::Down && b == Dir::Up) ||
           (a == Dir::Left && b == Dir::Right) || (a == Dir::Right && b == Dir::Left);
}

// Wire name for a direction ("up"|"down"|"left"|"right").
inline const char* dirName(Dir d) {
    switch (d) {
        case Dir::Up:
            return "up";
        case Dir::Down:
            return "down";
        case Dir::Left:
            return "left";
        case Dir::Right:
            return "right";
    }
    return "up";
}

// Parse a wire direction name; returns false (and leaves `out` untouched) for any
// string that isn't one of the four — the exception-free, never-trust-the-client
// discipline the rest of the codebase follows.
inline bool dirFromString(std::string_view s, Dir& out) {
    if (s == "up") {
        out = Dir::Up;
    } else if (s == "down") {
        out = Dir::Down;
    } else if (s == "left") {
        out = Dir::Left;
    } else if (s == "right") {
        out = Dir::Right;
    } else {
        return false;
    }
    return true;
}

// How an AI snake chooses its next direction (DESIGN-snake §7, the kids/grown-ups
// difficulty dial): A* toward the nearest food, a greedy axis-first heuristic, or
// random-but-safe. Ported from the upstream `NavigationType`.
enum class NavType { AStar, Greedy, Random };

}  // namespace villen::snake
