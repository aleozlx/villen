# Villen — the engine roadmap: how we choose what to build (design & index)

> Villen's host is a **game-agnostic slot** (DESIGN intro, §9; the `IEngine` seam in
> [`DESIGN-filter.md`](DESIGN-filter.md) §2). That means the interesting question is
> never "what's a cool app to put in the slot?" but **"what *axis* of the architecture
> does this engine stress that nothing else does?"** An engine earns its place by
> proving the spine survives a load it hasn't faced. This doc records the **method**,
> the **coverage so far**, **why the current slate was chosen**, and serves as the
> **index** to every engine design doc.

**Status:** living index + rationale. Update it whenever an engine is added or a new
axis is identified.
**Audience:** anyone proposing or building a Villen engine.

---

## 1. The method: pick by axis, not by app

Each engine is justified by the **stress axis** it adds. Two apps that exercise the
same axes teach the architecture the same lesson; two that look similar but stress
different axes are both worth building. So every proposal answers five questions
(§7), the first being: *what does this make the spine do that it has never done?*

The axes that matter for Villen's single-binary / single-thread / WS-edge / in-process
-admin spine:

- **Cadence** — what advances the world: a discrete action, a real-time input, or the
  host's **own clock**?
- **Authority clock** — does state move with *no* client input (a server tick)?
- **State distribution** — one **shared** state broadcast to all, or **private** state
  per connection?
- **Write contention** — how many writers mutate the *same* state at once, and how is
  conflict resolved?
- **Host compute** — pure CPU logic, the **GPU/APU**, an **external blocking** process,
  or **none** (work pushed to clients)?
- **Transport** — text/JSON or **binary**; bursty or sustained.
- **Latency tolerance** — irrelevant, comfortable, real-time, or *audible*.

---

## 2. Coverage matrix (all engines)

| Engine | Cadence | Server clock | State | Write contention | Host compute | Transport | The new thing it proves |
|---|---|---|---|---|---|---|---|
| **chess** | discrete move | no | shared, broadcast | 2 seats, strict turns (none) | pure CPU logic | text | the spine itself: authority, legality, turn order |
| **filter** | real-time frame | no | **private** per conn | N independent (none) | **GPU/APU** (sub-ms) | **binary** | a streaming, per-connection, GPU engine; device code on the edge |
| **chat** | prompt | no | private per conn | N independent (none) | **external blocking** (subprocess) | text | seconds-long blocking work kept off the §5 loop |
| **snake** | **server tick** | **yes** | shared, broadcast | N players, real-time (input-buffered) | pure CPU (in-loop) | text | an **authoritative clock + netcode** in the loop |
| **canvas** | high-rate event | no | shared, broadcast | N writers, **high** (last-write-wins) | pure CPU raster | text + binary | **many writers on one state**; §5 gives free total order |
| **jam** | **server tick** (musical) | **yes** | shared, broadcast | N writers, high | **none** (client audio) | text | **shared *time*** — tight cross-device sync; zero host audio |

Read top to bottom, each row is a load the rows above never imposed. The first three
were the existing/early slate; the bottom three are the current additions (§3).

---

## 3. Why the current slate (snake, canvas, jam)

After chess/`filter`/`chat`, two axes were conspicuously **unproven**, plus one that
combined them:

1. **No engine had a server clock.** chess/`filter`/`chat` are all *purely reactive* —
   the world is frozen until a client acts. Nothing forced the §5 loop to advance state
   on a timer, buffer asynchronous inputs, or broadcast at a fixed rate — i.e.
   **netcode**. This is the single most consequential unproven claim in the design,
   because the authoritative real-time loop is what "it's actually a game host"
   ultimately rests on. → **[`snake`](DESIGN-snake.md)** (a port of the author's
   existing Snake) fills it, kids-friendly and multi-controller, and as a bonus is the
   first real use of the long-deferred "route the Deck's own controls into a seat"
   (DESIGN §13).
2. **No engine had write contention.** chess avoids it with turns; `filter`/`chat` avoid
   it by being private. Nothing tested *many clients mutating one shared thing at once*.
   → **[`canvas`](DESIGN-canvas.md)** fills it, and surfaces the payoff that the §5
   single-thread loop hands a shared editor a **free total order** (no CRDT/OT).
   iPad-native.
3. **The synthesis — shared *time*.** Combine an authoritative clock with a shared
   editable state and tighten the latency target until drift is *audible*, and you get
   collaborative music. → **[`jam`](DESIGN-jam.md)** fills it with the shape "stream the
   score, synthesize locally," adding one reusable primitive (clock sync) and **zero**
   host-side audio.

These three were chosen to **maximize new axis coverage with minimal overlap**: two add
the server clock (one as a game, one as a metronome), two add write contention, and
`jam` alone adds tight shared time. Together with the first three, the matrix (§2) is
strikingly complete: turn-based, real-time-private-media, blocking-external,
real-time-authoritative-tick, concurrent-shared-mutation, and synchronized-time are all
covered.

---

## 4. Index of design docs

**The spine & cross-cutting:**
- [`DESIGN-villen.md`](DESIGN-villen.md) — the architecture: single binary, single loop
  (§5), WS player edge (§6), in-process admin, the structures that protect deferred
  features (§9). **Read first.**
- [`DESIGN-spectator-and-agent-api.md`](DESIGN-spectator-and-agent-api.md) — how a seat
  gets *driven* (browser / CLI agent / server-side mover) and watch-only spectating.
- [`DESIGN-admin-shell.md`](DESIGN-admin-shell.md) — the Deck-side operator shell: a
  **launcher** that starts **one engine at a time**, a system-info view (absorbing the
  `spike/deck` diagnostics), and per-engine views. **Coordination doc — read before
  building any engine's `drawAdmin()` (it fixes that contract).**
- [`DESIGN-self-hotfix.md`](DESIGN-self-hotfix.md) — forward-looking: could the appliance
  patch its own minor bugs? (A control plane on top of `chat` tool-calling, not an
  engine.)

**Engines (by the axis each owns):**
- [`chess`](DESIGN-villen.md) — *the spine itself.* **Built.** (Defined in the main
  design doc; the only engine in the repo today.)
- [`DESIGN-filter.md`](DESIGN-filter.md) — *streaming GPU/APU, per-connection.* Design.
- [`DESIGN-chat.md`](DESIGN-chat.md) — *blocking external inference.* Design.
- [`DESIGN-snake.md`](DESIGN-snake.md) — *authoritative clock + netcode.* Design (a port
  of [github.com/aleozlx/snake](https://github.com/aleozlx/snake)).
- [`DESIGN-canvas.md`](DESIGN-canvas.md) — *shared-write contention.* Design.
- [`DESIGN-jam.md`](DESIGN-jam.md) — *synchronized shared time.* Design.

The generalized `IEngine` slot these all plug into is specified in
[`DESIGN-filter.md`](DESIGN-filter.md) §2. An engine is selected at startup by
`--engine chess|filter|chat|snake|canvas|jam`, or — on the Deck, at runtime — from the
**launcher**, which runs exactly **one engine at a time**
([`DESIGN-admin-shell.md`](DESIGN-admin-shell.md); concurrent multi-engine is the
deferred lobby idea, §6).

---

## 5. Recurring patterns (what makes a good Villen engine)

Every engine above reuses the same handful of moves; a new engine that *can't* be
expressed in them is a warning sign:

1. **A pure ruleset in `engine/`, CI-tested against an oracle** — `chess::Position`,
   `filter::process` (vs. CPU reference), `snake::step`, `canvas::apply`, `jam`
   transport math. Determinism + tests is how device/blocking code stays out of the
   §9.1 pure core.
2. **§5 single-thread is an asset, not a constraint** — it gives `canvas`/`jam` a free
   total order on concurrent writes, and `snake` a lock-free authoritative tick. The one
   engine that genuinely blocks (`chat`) uses the single sanctioned escape hatch (a
   subprocess, fd in `poll()`), nothing more.
3. **Shared-broadcast *or* private-per-connection, both first-class** — chess/snake/
   canvas/jam broadcast; filter/chat are private. The slot must assume neither
   (DESIGN-filter §2).
4. **Reuse the edges** — the binary transport (`filter`), the stb codec (`filter` →
   `canvas`), the gamepad adapter (`chess` → `snake`), the mover pattern (spectator →
   `snake` AI), the clock-sync primitive (`jam` → future timed engines).
5. **The admin is in-process and gamepad-navigable** — each engine draws its own
   `drawAdmin()` *body* into the shell ([`DESIGN-admin-shell.md`](DESIGN-admin-shell.md));
   the operator runs the cabinet on the Deck.

---

## 6. Runners-up & deferred (recorded so the slate reads as a decision)

- **A lobby / multi-session multiplexer.** Running **several engine instances at once**
  (many chess games, or chess + canvas) would attack the single-hardcoded-`"default"`-
  session assumption (DESIGN §13 "multi-session management"). **Noted, but deliberately
  not soon:** multiplexing concurrent sessions edges toward building a little
  OS/window-manager, which is far more complexity than the **one-engine-at-a-time**
  model needs right now (that single-active stance is exactly what the launcher adopts,
  [`DESIGN-admin-shell.md`](DESIGN-admin-shell.md) §3). Park it until a concrete need
  appears; we are not trying to build an OS.
- **An untrusted-input sandbox** (shared REPL / regex / SQL playground) — the
  *adversarial / resource-limit* axis, and a natural sparring partner for
  [`self-hotfix`](DESIGN-self-hotfix.md). Deferred for its security surface; pick it up
  only when sandboxing is explicitly the goal.
- **More media** — audio *voice* (vs. `jam`'s synth), depth/second-camera, screen-share
  via `getDisplayMedia`. Each rides the binary transport; none adds a *new* axis beyond
  `filter`, so lower priority.
- **A durability/persistence engine** — a leaderboard, a saved-game library, a wiki.
  More a cross-cutting concern (the §9.2 serializable-state claim) than a flashy engine;
  fold persistence into existing engines first.

---

## 7. How to propose the next engine (the checklist)

1. **Axis:** what does it make the spine do that §2's matrix doesn't already cover? If
   nothing, it's a variant, not a new engine — build it as a mode of an existing one.
2. **Pure ruleset:** what is the deterministic, CI-testable core, and its oracle?
3. **Distribution:** shared-broadcast or private-per-connection?
4. **Blocking:** does any step block for more than a frame? If so, it's the `chat`
   pattern (subprocess + fd in `poll()`), and say so.
5. **New primitive:** does it need something genuinely new (a clock-sync, a codec, a
   sandbox)? Name it, and check whether an existing engine already has it.

If the answers all land in §5's recurring patterns, it will fit the spine. If one
*can't* be answered in those terms, that's exactly the design conversation to have
before writing code.
