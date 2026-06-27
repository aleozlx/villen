// Villen — the `snake` engine: AI movers (DESIGN-snake §7).
//
// A direct port of the upstream `algorithm/pathfinding` (A* + greedy/naive
// fallbacks), reshaped from "standalone helpers over a bordered grid" to
// "wrap-aware direction for one AI snake in a World". The upstream signed the
// occupancy check out to a C callback + void* context; here the World *is* the
// context, queried through its public API, so the math stays pure and
// deterministic and runs in headless CI (DESIGN-snake §7 "in-process and
// synchronous … no thread and no queue").
//
// The headline reshape is toroidal: on Villen's wrapping grid (DESIGN-snake §3)
// neighbours and the heuristic cross edges, so a snake chases food the short way
// round the torus.
#pragma once

#include "villen/snake/types.hpp"
#include "villen/snake/world.hpp"

namespace villen::snake {

// The direction this AI snake should travel next tick, per its nav algorithm
// (A* toward the nearest food, greedy axis-first, or random-but-safe). Never
// returns a reversal into the snake's own neck; falls back through greedy → any
// safe move → "keep going" so it always yields a legal-ish direction. Pure: a
// function of (World, snake) plus, for NavType::Random, a deterministic hash of
// the tick so replays stay exact.
Dir aiDirection(const World& world, const Snake& self);

// Toroidal Manhattan distance between two cells, honouring wrap (the A* heuristic
// and nearest-food pick). Exposed for tests.
int gridDistance(const World& world, ix2 a, ix2 b);

}  // namespace villen::snake
