# Villen — the `snake` engine: a real-time, server-authoritative arena (design & handoff)

> **`villen-snake`** is the fourth engine in Villen's game slot and the first that
> moves on **its own clock**: the server ticks the world at a fixed rate whether or
> not anyone presses anything, simulates every snake authoritatively, and broadcasts
> the new grid to all players each tick. It is a **port of the author's existing
> Snake** ([github.com/aleozlx/snake](https://github.com/aleozlx/snake)) — the
> clean, event-driven `snake2` — kept **kids-friendly** and **multi-controller**,
> with **boundaries removed** (the grid wraps; hitting an edge is not game over).
> Names derive mechanically: namespace `villen::snake`, lib `villen_snake`, doc
> `DESIGN-snake.md`, `--engine snake`.

**Status:** design / additive. A starting brief for the agent who will build it,
porting an existing codebase into Villen's spine.
**Scope:** the slice that proves a **server-authoritative, fixed-timestep, real-time
multiplayer** engine fits the single-thread loop — inputs in, a tick advances the
world, state broadcasts out.
**Audience:** the agent standing this up, having read
[`DESIGN-villen.md`](DESIGN-villen.md) and [`DESIGN-filter.md`](DESIGN-filter.md)
(for the generalized `IEngine` slot, §2 there), and able to read the upstream Snake.

> **Why this engine earns its keep.** Chess advances only on a move; `filter` only
> on a frame; `chat` only on a prompt — **every existing engine is purely reactive**,
> the world frozen until a client acts. `snake` is the first with an **authoritative
> server clock**: the loop must advance state on a timer, buffer asynchronous inputs
> between ticks, and broadcast at a fixed rate. That is the one thing the §5 loop has
> never been asked to carry, and the most consequential unknown left in the
> architecture (see [`DESIGN-engine-roadmap.md`](DESIGN-engine-roadmap.md)).

---

## 1. Context & one-paragraph pitch

A handful of kids open the `snake` client on their phones/tablets (or grab the
Deck's own controllers), each claims a snake, and they all play **at once** in one
shared arena. The host — the same single binary that runs chess, `filter`, `chat`,
and the admin UI — owns the authoritative grid, ticks it ~8–12 times a second,
applies each player's latest direction, moves every snake, resolves food and
collisions **leniently** (this is for kids: edges wrap, you don't instantly lose for
brushing a wall), and **broadcasts** the new grid to every client, which renders it.
The operator tunes speed, grid size, and forgiveness from the ImGui admin panel, and
can drop in **A\* AI snakes** (ported from the upstream `algorithm/`) to fill the
arena for solo play.

This is a **port, not a rewrite**: the upstream `snake2` already has the right
shape — an event-driven core whose `GAME_TICK` drives the simulation, a grid it
serialized whole into a memory-mapped circular buffer each frame, multi-
`SDL_GameController` input, and A\* pathfinding for AI. (Provenance: the upstream
Snake was itself authored by a Claude Code **Sonnet-tier** agent in **2025**, so the
port agent will meet familiar AI-written C++ — a fitting lineage, an earlier-tier
agent's game ported onto an architecture these docs design.) Villen keeps the
*simulation*, and swaps the *edges*: SDL input → WebSocket direction messages, the
OpenGL/shader renderer → a browser canvas, the circular-buffer grid dump → the WS
state broadcast, and the SDL event loop's `GAME_TICK` → Villen's single-thread loop
as the authoritative clock.

---

## 2. What ports, and what gets dropped

| Upstream `snake2` piece | Fate in Villen |
|---|---|
| Grid + snakes + food + tick logic (the simulation) | **Ported** → the pure engine `engine/snake/` (`villen::snake`), no I/O |
| `EventSystem` `GAME_TICK` driving the sim | **Reshaped** → a fixed-timestep tick inside the §5 loop (§4) |
| Whole-grid serialize → memory-mapped circular buffer | **Reshaped** → whole-grid serialize → WS `state` broadcast (§5) |
| Multi-`SDL_GameController` input | **Reshaped** → per-player WS `input` messages; *plus* the Deck's local controllers as players (§6) |
| OpenGL/GLAD/shaders/font renderer | **Dropped** → the browser renders the grid on a canvas (§8) |
| A\* pathfinding (`algorithm/`) | **Ported** → server-side AI snakes (§7), the real-time analog of the spectator doc's "mover" |
| `ix2` / `fx3` value types | **Reused** as-is (tiny, dependency-free) |

The simulation is the asset; the SDL2/OpenGL scaffolding the upstream needed to *be*
an app is exactly what Villen already provides, so it falls away.

---

## 3. The pure engine (`villen::snake`)

The §9.1 pure ruleset (as established for chess and `filter`): no SDL, no GL, no
sockets, deterministic, CI-tested.

```cpp
// engine/include/villen/snake/world.hpp (sketch)
namespace villen::snake {
enum class Dir { Up, Down, Left, Right };
struct Snake { int id; std::vector<ix2> body; Dir dir; bool alive; int score; };
struct World {
  int w, h;                       // grid; edges WRAP (no boundary death)
  std::vector<Snake> snakes;
  std::vector<ix2> food;
  unsigned tick;
  // Advance one tick: apply each snake's latest queued direction, move, wrap,
  // eat, resolve collisions per the (lenient) rule set. Deterministic.
  void step(const std::unordered_map<int, Dir>& inputs);
  static World create(int w, int h, const Rules&);
};
}
```

**Kids-friendly rules (data, so the operator can soften them, §7):**

- **Wrap, don't die.** Moving off an edge re-enters the opposite side (toroidal grid).
  This is the headline change from classic Snake and from the upstream.
- **Gentle collisions.** Configurable: self-collision and snake-vs-snake either
  **respawn** the shorter snake small (forgiving, default) rather than ending the
  game, or are off entirely in the youngest "free play" mode. Nobody is ever
  *spectating their own death* for long.
- **Always something to eat.** Maintain a target food count so the board never feels
  empty; growth is bounded so one expert kid can't fill the arena.

**Determinism & tests:** `step()` is a pure function of `(World, inputs)`. The
CI test replays a fixed input script over a fixed seed and asserts the exact grid —
the same oracle discipline as chess legality and `filter`'s reference. No GPU, no
display, runs in headless CI alongside `villen_engine` and `villen_filter`.

---

## 4. The authoritative clock inside the §5 loop (the crux)

Snake's simulation is **cheap** (move a few dozen cells, check collisions), so unlike
`chat` it needs **no worker thread** — it runs *directly in the single loop*, which
is exactly what makes it a clean test of §5 rather than a violation of it. The loop
grows a **fixed-timestep accumulator**:

```cpp
// ChatEngine had tick() do nothing heavy; SnakeEngine::tick() advances the world.
void SnakeEngine::tick() {
  acc_ += loopDtMs();                       // wall time since last iteration
  while (acc_ >= tickMs_) {                 // tickMs_ ≈ 80–125ms (8–12 Hz)
    world_.step(drainLatestInputs());       // one authoritative step
    broadcastState();                       // full grid to everyone (§5)
    acc_ -= tickMs_;
  }
}
```

- **Sim rate ≠ render rate.** The world ticks at ~8–12 Hz (snake's natural pace);
  the admin window still renders at ~60 Hz; the browser interpolates/animates between
  the discrete grid states it receives. Decoupling these is the netcode lesson chess
  never had to teach.
- **Input buffering.** Between ticks, a player may send several directions; the
  engine keeps **only the latest legal one** per player (you can't reverse into your
  own neck), applied at the next `step()`. This is the real-time analog of `filter`'s
  drop-to-latest, for control instead of pixels.
- **Still single-threaded, still lock-free** (§5). The clock is just the loop noticing
  wall-time has crossed a tick boundary. No `chat`-style guarded queue is needed
  because nothing blocks.

---

## 5. Wire contract (player edge — broadcast, like chess)

`snake` is a **shared world**, so — like chess and unlike `filter`/`chat` — it
**broadcasts** one authoritative state to everyone (DESIGN §6). Text/JSON is fine at
snake grid sizes; a compact binary grid (reusing `filter`'s binary path) is an
optional optimization for large arenas.

**Client → server:**
```jsonc
{ "type": "join",  "name": "Mia" }                 // server assigns a snake id + color
{ "type": "input", "dir": "up" }                   // up | down | left | right
{ "type": "leave" }
```

**Server → client:**
```jsonc
{ "type": "joined", "snakeId": 3, "color": "#39d" }
{ "type": "config", "w": 32, "h": 20, "tickMs": 100, "wrap": true }
// Broadcast every tick. Compact: snakes as cell lists, food as cells, scores.
{ "type": "state", "tick": 1487,
  "snakes": [ { "id": 3, "cells": [[10,4],[10,5]], "dir": "up", "alive": true, "score": 6 }, … ],
  "food":   [ [2,9], [30,1] ] }
```

Authority is server-side, unconditional (chess §3.2): the client sends *intent*
(a direction), never a position. There is no client-trusted state to forge.

---

## 6. Players, multi-controller, and the Deck's own pads

`snake` has **N symmetric players**, no turns — each `join` allocates a snake; the
arena holds as many as the grid comfortably fits (operator-capped). The
"multi-controller" heritage ports two ways, both funnelling into one intake exactly
as DESIGN §7 demands:

- **Browser players** send `input` over WS (phones/tablets — touch-swipe, on-screen
  D-pad, arrow keys, **and** the Gamepad API, reusing the chess client's gamepad
  adapter pattern, §7).
- **The Deck's own controllers** drive snakes locally — and this is the first real
  use of the long-deferred *"route the Deck's native controls into a game seat"*
  (DESIGN §4 non-goal, §13 open question). The admin UI already reads the Deck
  gamepad via SDL (`admin_ui.cpp`); `snake` lets a local pad **be a player** instead
  of only navigating the admin panel. Couch co-op + remote phones in one arena.

Every source emits the same `{dir}` into the same per-player buffer; the engine never
knows whether a turn came from a thumb-swipe, an arrow key, a Deck stick, or an AI.

---

## 7. AI snakes (the upstream A\*, as real-time movers)

Port `algorithm/pathfinding` as **server-side AI players**: each AI snake is a
"mover" the host steps each tick (the real-time cousin of the spectator doc's
Stockfish/LLM mover, [`DESIGN-spectator-and-agent-api.md`](DESIGN-spectator-and-agent-api.md)
§5). On each tick, before `step()`, the host asks each AI for a direction (A\* toward
the nearest food, with the greedy/naive fallbacks the upstream already has). This:

- fills the arena so a **single kid plays a lively board**;
- makes **AI-vs-AI** a screensaver-grade demo on the Deck;
- exercises the difficulty knob (A\* vs. greedy vs. random) as a kids/grown-ups dial.

Because the AI is in-process and synchronous (A\* over a 32×20 grid is microseconds),
it needs no thread and no queue — it's just another input source feeding `step()`.

---

## 8. Browser client & admin UI

**Client** (served from the static root; no build step): render the grid on a
`<canvas>` from each `state` broadcast, animating snakes sliding between ticks for
smoothness; show scores and your snake's color. Input via **touch-swipe / on-screen
D-pad / arrow keys / Gamepad API**, all calling one `submitInput(dir)` (DESIGN §7).
Big, friendly, high-contrast — it's for kids.

**Admin** (`SnakeEngine::drawAdmin()`, gamepad-navigable): the arena at a glance —
players + scores + AI count + tick rate; controls for **start/pause/reset**, **grid
size**, **speed** (`tickMs`), **wrap on/off**, **collision leniency**, and **add/remove
AI snake**. The operator runs the playground.

---

## 9. Build, deps, and order

- **No new dependencies.** No GPU, no subprocess, no codec — `snake` is the
  **lightest** of the new engines. The pure engine is plain C++; the host adapter
  reuses the existing WS edge and the client's gamepad code.
- **CMake:** `villen_snake` pure engine lib (always built, CI-tested, no I/O), like
  `villen_engine`/`villen_filter`; the `SnakeEngine` adapter compiles into the host.
- **Cross-build/glibc** caveats are unchanged (steamdeck-debugging.md §2) but trivial
  here — no exotic symbols.

**Build order (smallest-spine-first):**
1. **Pure engine + tests** — port `snake2`'s sim into `engine/snake/`; deterministic
   `step()`; replay tests (wrap, eat, lenient collisions). *No host.*
2. **Tick in the loop + broadcast** — `SnakeEngine::tick()` accumulator; broadcast
   `state`; drive it from the headless loop. Verify with a scripted WS client.
3. **Browser client, one input source** — canvas render + arrow keys. The loop is
   visible.
4. **Multi-input + multi-player** — touch/D-pad/Gamepad adapters; several browsers in
   one arena.
5. **AI snakes** — port A\*; fill the board; AI-vs-AI.
6. **Deck controllers as players** — the §6 local-pad-into-seat milestone.
7. **Admin console** — speed/size/leniency/AI controls.

---

## 10. Acceptance criteria

1. `engine/snake/` builds and its deterministic replay tests pass in headless CI.
2. `--engine snake` selects it; chess/`filter`/`chat` are unaffected.
3. The world **advances on the server clock** with no input, and several browsers
   play simultaneously in one shared arena, each rendering the same broadcast state.
4. Edges **wrap** (no boundary game-over); collisions are **lenient** per the operator
   setting; the game stays fun without resets.
5. A snake can be driven by **touch, keyboard, the Gamepad API, the Deck's own
   controller, and an A\* AI** — all interchangeably (DESIGN §7).
6. The operator changes speed/size/leniency and adds an AI snake live from the admin
   UI.

---

## 11. Rejected alternatives

- **Client-authoritative movement.** No — the server owns the world (chess §3.2);
  clients send intent only, or kids would teleport/cheat by accident.
- **Stream a rendered video of the board** (the `filter` move). Wrong layer — send the
  *grid state*, render client-side; it's tiny, sharp, and zoom-independent.
- **Per-tick deltas instead of the full grid.** Premature at snake sizes; full-state
  broadcast is idempotent and simple (the chess §6 rationale). Add deltas only for a
  huge arena.
- **Keep the upstream OpenGL renderer server-side.** That was for a standalone app;
  Villen's clients render. The server draws nothing for snake.
- **Per-player private state** (the `filter`/`chat` model). Snake is a *shared* world;
  broadcast is correct here, and the slot supports both (DESIGN-filter §2).

---

## 12. Open questions

- **Lag/interpolation polish:** client-side interpolation is enough at 8–12 Hz on a
  LAN; revisit prediction only if needed.
- **Spectators & a kiosk view:** a read-only big-screen arena (the Deck's display)
  while kids play from phones — collapses into the uniform client (DESIGN §13).
- **Scoreboard/tournament persistence** — leans on serializable state (§9.2); a small
  durable high-score table.
- **The Deck's gyro as input** — the upstream supported gyroscope; a tilt-to-steer
  mode is a fun kids option once local-pad-into-seat lands (§6).
- **Power-ups / variants** — speed tiles, multi-food, team mode — all pure-engine
  data, the fairy-pieces analog for snake.
