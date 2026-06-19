# Villen — the game-framework contract: one game per binary

**Status:** design / forward-looking. Refines the "engine slot" of
[`DESIGN-villen.md`](DESIGN-villen.md) §9.1 into a reusable contract. Not yet
implemented — chess today is compiled *into* the host; this describes the
inversion that lets a future game compile the host *into itself*.
**Scope:** how an unknown future game (Snake, a card game, an action game) reuses
Villen for rooms + serving without touching Villen's code, and which of today's
choices are load-bearing versus incidental.
**Audience:** someone about to build a second game on Villen, or about to extract
the framework seam from the current chess-coupled host.

---

## 1. The inversion: Villen is a library, not a host that contains chess

Today Villen *is* the binary, and chess is linked into it. The end state of the
"swappable slot" idea (DESIGN §9.1) flips that dependency:

> **Villen becomes a submodule that a game depends on.** Each game is its own
> repository and its own binary; it links Villen for rooms, seats, serving, and
> the admin face, and supplies the rules + client. A Villen-based binary serves
> **one game type** — the Snake binary serves Snake, the chess binary serves
> chess. There is no multi-game host.

"One game type per binary" is the decision that keeps everything else simple. In
particular it **eliminates IPC** (§6): the game is an in-process object, not a
separate process, because there is no second game to isolate it from.

```
  TODAY                              END STATE (this doc)
  villen (binary)                    snake (binary)
  ├─ engine: chess  ◄── in-tree      ├─ villen/  ◄── submodule (rooms+serving+admin)
  ├─ host: WS + admin + session      ├─ engine: snake rules
  └─ client: chessboard              ├─ src/game.cpp  : implements villen::IGame
                                     ├─ client/       : snake renderer + villen-client.js
                                     └─ main.cpp      : villen::run({...})
```

---

## 2. Two audiences — and the admin console is *not* a contract surface

A game author programs against exactly **two** surfaces, plus one schema they own
both ends of:

1. **The server interface** (`villen::IGame`, §4) — what the host needs from any
   game to run a room.
2. **The client core** (`villen-client.js`) — connect / join / reconnect / send /
   receive, factored out of today's [`net.js`](../client/src/net.js); the game
   writes its renderer on top.
3. **The game's own wire payload** — opaque to Villen, identical on both ends.

The **in-process admin console** (today's Dear ImGui UI, DESIGN §8) is **not** on
this list. It is an *operator* face — create/observe rooms, manage seats, show the
join URL + QR, surface diagnostics — and a game author never implements or even
sees it. The litmus test for a correctly drawn boundary: **the admin console can
render a room without knowing which game it is.** It reads only Villen's generic
membership state plus an optional game-supplied status string (§4); it never
touches the game's protocol.

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
(moves, state). That split — **Villen owns the envelope, the game owns the
payload** — is what lets an unknown future game drop in: Villen compiles today
without naming a single game concept.

---

## 4. The contract

The defining principle: **Villen never constructs a game message.** It hands the
game a `Room` handle and notifies it of membership events; the game emits its own
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

// What every game implements. No game concept appears inside Villen itself.
struct IGame {
  virtual ~IGame() = default;
  virtual SeatRoster seats() = 0;                  // declares the shape; Villen assigns/holds
  virtual void onJoin   (Room&, ConnId, SeatId) = 0;  // game pushes its own state to the joiner
  virtual void onLeave  (Room&, ConnId, SeatId) = 0;
  virtual void onMessage(Room&, ConnId, SeatId, std::string_view) = 0;  // the game's protocol
  virtual void onTick   (Room&, uint64_t now) {}   // real-time games only; default no-op
  virtual const char* statusLine() { return ""; }  // for the admin console only; optional
  virtual void reset() {}                           // admin "new game"; optional
};

// One IGame instance per room (§5.2). Single-room is just "max rooms = 1".
struct IGameFactory {
  virtual ~IGameFactory() = default;
  virtual std::unique_ptr<IGame> create() = 0;
};

struct Config {
  std::string staticRoot = "client";
  std::unique_ptr<IGameFactory> factory;
};

void run(Config);   // e.g. run({ .staticRoot = "client", .factory = makeSnake() });

}  // namespace villen
```

**The only thing Villen parses on the wire is the join/seat envelope** (`join`
with an optional seat name + room id → `joined` with the assigned seat). Once a
connection is seated, every subsequent message is an opaque payload delivered
verbatim to `IGame::onMessage`. So DESIGN §6's `proposeMove`/`state`/FEN contract
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
by making `IGame` a **per-room instance from a factory**: single-room is "max rooms
= 1," same shape. Still one process, still no IPC — multiple rooms are just
multiple `IGame` instances in the one binary.

### 5.3 WebSocket ⊂ transport-agnostic (and why unreliable is safe here)

Today's transport is a hand-rolled RFC 6455 **WebSocket** ([ws_server.cpp](../host/src/ws_server.cpp));
DESIGN §4 lists P2P/WebRTC as a deferred non-goal. WebRTC's value is
**unreliable/unordered data channels** — lower latency for high-rate state sync,
no TCP head-of-line blocking — which is itself evidence the ambition is real-time.

Two facts make the swap clean rather than a rewrite:

- It drops in behind the existing **poll-shaped seam** (DESIGN §9.5); `IGame` and
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
process with shared memory. Under "one game type per binary," the answer is **no**,
for the cleanest possible reason: there is only ever one game in the process, so
there is nothing to isolate it *from*. Process separation would reintroduce exactly
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
  → becomes a generic `Room` driving an injected `IGame`; turn logic moves into the
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
  src/game.cpp             # class SnakeGame : villen::IGame  (6 methods)
  client/
    index.html
    villen-client.js       # from Villen: connect / join / reconnect / send / recv
    snake.js               # the game's own renderer + protocol payloads
  main.cpp                 # villen::run({ .staticRoot="client", .factory=makeSnake() })
```

The author writes the rules, the renderer, and a payload schema. They write **no**
WebSocket code, **no** seat-lifecycle code, **no** admin UI — all of that is the
submodule. Snake is real-time, so `SnakeGame` implements `onTick` and leaves the
"whose turn" gate empty; chess would do the reverse.

---

## 9. Relationship to the other docs

- [`DESIGN-villen.md`](DESIGN-villen.md) is the master design and current truth
  (single binary, chess in-tree, WebSocket, one session). This doc refines its
  §9.1 "engine slot" into a contract and states which of its choices are
  incidental.
- [`DESIGN-spectator-and-agent-api.md`](DESIGN-spectator-and-agent-api.md) layers
  alternative *seat drivers* (CLI, server-side agents) on the same player edge;
  orthogonal to this doc — those drivers sit on the envelope, below any single
  game's payload.
</content>
</invoke>
