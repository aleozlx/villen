# Villen — the game-framework contract: a multi-engine host (single-active)

**Status:** design / forward-looking. Refines the "engine slot" of
[`DESIGN-villen.md`](DESIGN-villen.md) §9.1 into a reusable `IEngine` contract. Not yet
implemented — chess today is compiled *into* the host. **Revised for a multi-engine
host:** one Villen binary carries several **engines** as `IEngine` modules, a **launcher**
runs **one at a time** ([`DESIGN-admin-shell.md`](DESIGN-admin-shell.md)), and each engine
runs a family of **games** (variants) — e.g. `ChessEngine` → regular + fairy chess (§1).
(Earlier drafts said "one game *per binary*"; that submodule shape is now one of two — §1
— with the multi-engine host the chosen Deck face.)
**Scope:** how an engine reuses Villen for rooms + serving without touching Villen's
code, and which of today's choices are load-bearing versus incidental.
**Audience:** someone about to build a second engine on Villen, or about to extract the
framework seam from the current chess-coupled host.

---

## 1. Villen runs engines; engines run games

Today Villen *is* the binary, and chess is linked into it. The "swappable slot" idea
(DESIGN §9.1) becomes a reusable contract with **two layers**:

- An **engine** is the host-facing runtime for a *family* of related games — it
  implements the small `IEngine` interface (§4), and Villen drives it (rooms, seats,
  serving, ticks). `ChessEngine`, `FilterEngine`, `SnakeEngine`, … are engines.
- A **game** is a variant/ruleset an engine runs. `ChessEngine` runs *regular chess*
  **and** *fairy chess*; `CanvasEngine` runs *pixel-art*, *whiteboard*, *pictionary*.
  Games are an engine's content, not separate host modules.

> **One binary carries several engines as `IEngine` modules; a launcher runs one
> engine at a time.** chess, snake, filter, … each implement `IEngine`; the host
> registers their factories and the operator picks one on the Deck
> ([`DESIGN-admin-shell.md`](DESIGN-admin-shell.md)). **Multi-engine host,
> single-active** — exactly one engine runs at any moment.

"One engine **active** at a time" keeps everything simple. In particular it keeps **no
IPC** (§6): the active engine is an in-process object, and since only one runs there is
nothing to isolate it *from*.

```
  TODAY                          MULTI-ENGINE HOST (this doc)
  villen (binary)                villen (binary)
  ├─ rules: chess  ◄ in-tree     ├─ villen lib : WS + admin/launcher + rooms + loop
  ├─ host: WS + admin + session  ├─ engines/   : ChessEngine, SnakeEngine, FilterEngine … : IEngine
  └─ client: chessboard          ├─ client/<engine>/ : each engine's renderer + villen-client.js
                                 └─ launcher   : picks an engine, runs ONE at a time
                                                  (an engine offers its games/variants within)
```

**Concrete engines first; abstract bases later.** An engine need not be a maximally-
abstract archetype up front. There is no `TurnBasedEngine` today — there is a concrete
`ChessEngine` that already plays regular *and* fairy chess; a generic turn-based base is
extracted only **when ≥2 engines clearly share infra** (e.g. a `TickEngine` once both
`SnakeEngine` and a second real-time engine exist). The same goes for the contract
itself: concrete engine classes now, the `IEngine` interface firmed up as the second
engine lands — matching the repo's "don't over-engineer" ethos.

**Why a multi-engine host, not one binary per engine.** Standing an engine up as its
*own* binary is heavier than swapping rules: it also wants its own Steam-library
shortcut, capsule/hero/icon art ([`villen-art-brief.md`](villen-art-brief.md)), and a UI
pass — real overhead only worth paying when something is significant enough to be its
own *product*. Villen's focus is a **pocket game-server demo** — a handful of small
games you carry and switch between — so the lighter, built-in, **streamlined engine
switching** wins. The submodule single-engine shape (an engine in its own repo linking
Villen) stays available for when something *is* that significant; and a more dynamic
**hot-pluggable** engine is a someday-maybe we may toy with — explicitly out of scope
today.

---

## 2. Two audiences — and the admin console is *not* a contract surface

An engine author programs against exactly **two** surfaces, plus one schema they own
both ends of:

1. **The server interface** (`villen::IEngine`, §4) — what the host needs from any
   engine to run a room.
2. **The client core** (`villen-client.js`) — connect / join / reconnect / send /
   receive, factored out of today's [`net.js`](../client/src/net.js); the engine
   writes its renderer on top.
3. **The engine's own wire payload** — opaque to Villen, identical on both ends.

The **in-process admin console** (today's Dear ImGui UI, DESIGN §8) is an *operator*
face — the launcher, create/observe rooms, manage seats, the join URL + QR,
diagnostics. Most of it is **game-agnostic**: the litmus test is that the shell renders
a room's membership and the launcher list **without knowing which game it is**, reading
only Villen's generic membership state plus an optional game-supplied `statusLine()`
(§4). **But a game may also contribute its own operator panel** via the optional
`drawAdmin()` (§4) — `filter`'s pipeline editor, `chat`'s model selector, `jam`'s tempo
— which the shell hosts *inside its own chrome* while that game is active. The boundary:
the shell owns the chrome + launcher + roster; the engine owns only its panel body and
draws no chrome ([`DESIGN-admin-shell.md`](DESIGN-admin-shell.md) §8).

(The README's "at a glance" diagram draws the console as a peer box to the engine
and gateway. Under this framing it belongs *inside* the Villen library as an
operator I/O, not on the seam the game author touches.)

---

## 3. What the framework does — three jobs, none game-aware

| Job | Who owns it | In today's code |
|---|---|---|
| **Serve** the game's client assets over HTTP | Villen | `WsServer::setStaticRoot` — already game-agnostic ([ws_server.hpp](../host/src/ws_server.hpp)) |
| **Membership** — connections + the seat lifecycle | Villen | the seat state machine in [session.cpp](../host/src/session.cpp): open / connected / disconnected-held / spectator, token-free reconnect-by-name, admin Free (DESIGN §13). **The reusable crown jewel.** |
| **Distribute** the game's own protocol messages | Villen relays; **game defines** | today fused with chess in `proto` (§7) |

Everything Villen does is the *envelope*: which connection, which seat, which
room, and the join/leave/reconnect lifecycle. It never interprets the *payload*
(moves, state). That split — **Villen owns the envelope, the engine owns the
payload** — is what lets an unknown future engine drop in: Villen compiles today
without naming a single game concept.

---

## 4. The contract

The defining principle: **Villen never constructs a gameplay message.** It hands the
engine a `Room` handle and notifies it of membership events; the engine emits its own
protocol bytes through that handle.

```cpp
namespace villen {

enum class Delivery { Reliable, Unreliable };   // the transport seam (§5.3)

// The handle Villen gives the game. Villen never reads the bytes flowing through.
struct Room {
  void send(ConnId, std::string_view bytes, Delivery = Delivery::Reliable);
  void broadcast(std::string_view bytes, Delivery = Delivery::Reliable);
  SeatId seatOf(ConnId) const;          // membership Villen owns
  // ... roster / occupancy queries for the game's own logic
};

// What every engine implements. No game concept appears inside Villen itself.
struct IEngine {
  virtual ~IEngine() = default;
  virtual SeatRoster seats() = 0;                  // declares the shape; Villen assigns/holds
  virtual void onJoin   (Room&, ConnId, SeatId) = 0;  // game pushes its own state to the joiner
  virtual void onLeave  (Room&, ConnId, SeatId) = 0;
  virtual void onMessage(Room&, ConnId, SeatId, std::string_view) = 0;  // the game's protocol
  virtual void onTick   (Room&, uint64_t now) {}   // real-time games only; default no-op
  virtual const char* statusLine() { return ""; }  // generic admin roster line; optional
  virtual void drawAdmin() {}                       // optional operator panel body (admin-shell §8)
  virtual void reset() {}                           // admin "new game"; optional
};

// One IEngine instance per room (§5.2). Single-room is just "max rooms = 1".
struct IEngineFactory {
  virtual ~IEngineFactory() = default;
  virtual std::unique_ptr<IEngine> create() = 0;
  virtual const char* name()      = 0;   // shown in the launcher
  virtual const char* clientDir() = 0;   // this game's client assets to serve
};

// The host registers several engines; the launcher activates ONE at a time.
struct Config {
  std::vector<std::unique_ptr<IEngineFactory>> engines;   // the launcher's menu
  std::string startGame = "";   // optional: boot straight into one (kiosk / --engine)
};

void run(Config);   // boots the launcher; operator picks a game (DESIGN-admin-shell)

}  // namespace villen
```

**The only thing Villen parses on the wire is the join/seat envelope** (`join`
with an optional seat name + room id → `joined` with the assigned seat). Once a
connection is seated, every subsequent message is an opaque payload delivered
verbatim to `IEngine::onMessage`. So DESIGN §6's `proposeMove`/`state`/FEN contract
becomes **chess's** protocol, living in the chess game module — not Villen's. This
is the natural reading of §9.5 ("all wire format on the player edge"): the
*game-specific* format lives in the game; Villen owns only the envelope format.

---

## 5. Incidental, not load-bearing (design choices that are degenerate cases)

Three of today's apparent constraints are not fundamental. The contract is shaped
so each is the *degenerate case* of a more general option — you never bake in a
ceiling, and you never pay for the general case until a game needs it.

### 5.1 Turn-based ⊂ real-time

"Whose turn" is not load-bearing — it is a gate inside the game's `onMessage`,
nothing more. A real-time game (Snake) instead advances the world on `onTick`. The
*one* real blocker is that today's loop is event-driven — `ws.poll(100)` only
wakes on packets ([main.cpp](../host/src/main.cpp)). The framework loop must wake
on a **fixed timestep** so `onTick` fires with no input. Turn-based games simply
don't implement `onTick`.

### 5.2 Single-room ⊂ multi-room

Today there is one hardcoded session, `"default"`. Whether one Deck hosts one room
or several rooms of the same game (three chess matches at a gathering) is settled
by making `IEngine` a **per-room instance from a factory**: single-room is "max rooms
= 1," same shape. Still one process, still no IPC — multiple rooms are just multiple
`IEngine` instances in the one binary. (Different *engines* are different factories the
launcher picks among — the multi-engine host, §1 — still one active at a time.)

### 5.3 WebSocket ⊂ transport-agnostic (and why unreliable is safe here)

Today's transport is a hand-rolled RFC 6455 **WebSocket** ([ws_server.cpp](../host/src/ws_server.cpp));
DESIGN §4 lists P2P/WebRTC as a deferred non-goal. WebRTC's value is
**unreliable/unordered data channels** — lower latency for high-rate state sync,
no TCP head-of-line blocking — which is itself evidence the ambition is real-time.

Two facts make the swap clean rather than a rewrite:

- It drops in behind the existing **poll-shaped seam** (DESIGN §9.5); `IEngine` and
  the seat lifecycle are transport-agnostic and unaffected. The *one* interface
  accommodation is the `Delivery` class on `Room::send`/`broadcast` (§4), so
  snapshots/lifecycle go reliable and high-rate state can go unreliable — without
  that hint, an unreliable transport buys nothing.
- The existing **full-snapshot broadcast** (DESIGN §6.2 — clients render only from
  full state, never deltas) is exactly what makes an unreliable channel *safe*:
  drop a packet, the next snapshot corrects it. Keep games snapshot-based and
  lossy delivery costs nothing. The trap is the opposite — moving to deltas over
  an unreliable channel forces a reliability sublayer (sequence numbers, periodic
  keyframes).

WebRTC is also not a *simplification*: it needs a signaling rendezvous (typically
an HTTP/WS endpoint on the host — so today's server doesn't disappear) and a
native stack on the host (libdatachannel to stay dependency-light; Google's
libwebrtc would violate the one-small-binary thesis). Adopt it for latency, not
for simplicity.

---

## 6. No IPC, single process (the engine-isolation question, settled)

A recurring question is whether the engine should run as a separate, crash-isolated
process with shared memory. Under **one engine active at a time**, the answer is **no**,
for the cleanest possible reason: only ever one game runs in the process, so there is
nothing to isolate it *from* (switching engines tears one down before starting the next,
[`DESIGN-admin-shell.md`](DESIGN-admin-shell.md) §4). Process separation would reintroduce exactly
the IPC/serialization/lifecycle cost that the single-process, single-thread,
no-locks thesis (DESIGN §5) was built to avoid.

Isolation only earns its keep for *untrusted, heavy, or polyglot* engines — a
downloaded third-party `.so`, a real-time sim that would stall the loop, or a
non-C++ engine. Those are out of scope here; if one ever arrives, the right tool is
a subprocess speaking a message protocol (the game is deterministic and its state
is serializable, so a crashed child restarts from a snapshot — see DESIGN §9.2),
**not** shared memory.

---

## 7. What this means for the current code (the degchessing list)

The extraction is mostly mechanical; the membership logic is already generic.

**Already game-agnostic — ships as the library as-is:**
- [ws_server.hpp](../host/src/ws_server.hpp) — pure transport + static serving.
- The seat lifecycle in [session.cpp](../host/src/session.cpp) — generic once
  decoupled from the two hardcoded `white_`/`black_` seats.

**Where chess leaks — what the seam has to generalize:**
- `GameServer` hard-types `chess::Position`, two seats named white/black, and
  derives turn from `position_.sideToMove()` ([session.hpp](../host/src/session.hpp)).
  → becomes a generic `Room` driving an injected `IEngine`; turn logic moves into the
  game.
- `proto::SeatStatus` is `std::array<std::string, 2>` — **two** seats baked into
  the type; `proto::state()` takes a `chess::Position`; `Incoming.move` is
  `std::optional<chess::Move>` ([protocol.hpp](../host/src/protocol.hpp)). → Villen
  keeps only the join/seat envelope; `state`/`proposeMove` move into the chess
  module as opaque payloads.
- The admin UI reads chess specifics ("Turn: black"). → reads the generic roster +
  `statusLine()`.

---

## 8. What a downstream game repo looks like (Snake)

```
snake/
  third_party/villen/      # Villen as a submodule (rooms + serving + admin + loop)
  engine/                  # pure Snake rules — no I/O (mirrors today's engine/)
  src/game.cpp             # class SnakeEngine : villen::IEngine  (6 methods)
  client/
    index.html
    villen-client.js       # from Villen: connect / join / reconnect / send / recv
    snake.js               # the game's own renderer + protocol payloads
  main.cpp                 # villen::run({ .staticRoot="client", .factory=makeSnake() })
```

The author writes the rules, the renderer, and a payload schema. They write **no**
WebSocket code, **no** seat-lifecycle code, **no** admin UI — all of that is the
submodule. Snake is real-time, so `SnakeEngine` implements `onTick` and leaves the
"whose turn" gate empty; chess would do the reverse.

---

## 9. Relationship to the other docs

- [`DESIGN-villen.md`](DESIGN-villen.md) is the master design and current truth
  (single binary, chess in-tree, WebSocket, one session). This doc refines its
  §9.1 "engine slot" into a contract and states which of its choices are
  incidental.
- [`DESIGN-admin-shell.md`](DESIGN-admin-shell.md) is the operator shell that hosts
  this contract: the launcher that picks an engine, the per-engine `drawAdmin()` panels, and
  the one-engine-active lifecycle. [`DESIGN-engine-roadmap.md`](DESIGN-engine-roadmap.md)
  indexes the games being designed against it (filter/chat/snake/canvas/jam).
- [`DESIGN-spectator-and-agent-api.md`](DESIGN-spectator-and-agent-api.md) layers
  alternative *seat drivers* (CLI, server-side agents) on the same player edge;
  orthogonal to this doc — those drivers sit on the envelope, below any single
  game's payload.
</content>
</invoke>
