# Villen — the `canvas` engine: a shared collaborative drawing wall (design & handoff)

> **`villen-canvas`** is the fifth engine in Villen's engine slot and the first built
> around **many writers contending over one shared state**: a room full of iPads,
> tablets, and phones all draw on **one** canvas, and everyone sees everyone's marks
> appear live. Apple-Pencil-natural strokes (pressure → width), touch, and mouse all
> paint into the same authoritative bitmap the host owns and broadcasts. Names derive
> mechanically: namespace `villen::canvas`, lib `villen_canvas`, doc
> `DESIGN-canvas.md`, `--engine canvas`.

**Status:** design / additive. A starting brief for the agent who will build it.
**Scope:** the slice that proves a **high-write-rate, broadcast-fan-out, shared-
mutable-state** engine fits the single-thread loop — strokes in, applied to one
authoritative raster, broadcast out, snapshot-able for late joiners and persistence.
**Audience:** the agent standing this up, having read
[`DESIGN-villen.md`](DESIGN-villen.md) and [`DESIGN-filter.md`](DESIGN-filter.md)
(the `IEngine` slot §2, the binary transport §5, and the vendored stb codec §12).

> **Why this engine earns its keep.** Chess sidesteps write contention with strict
> turns; `filter` and `chat` sidestep it by being **private** per connection. None has
> ever had *many clients mutating one shared thing at once*. `canvas` is exactly that
> — and it surfaces a quietly beautiful payoff of DESIGN §5: the single-thread loop,
> chosen so there'd be **no locks**, *also* hands the canvas a **free total order** on
> concurrent writes. Every stroke from every device is applied by one thread in
> arrival order, so "two people drew the same pixel" resolves to last-writer-wins
> **for free** — no CRDT, no operational transform, no merge logic
> ([`DESIGN-engine-roadmap.md`](DESIGN-engine-roadmap.md)).

---

## 1. Context & one-paragraph pitch

A group opens the `canvas` client on their iPads and draws together on one big shared
wall — a sticker-book, a mural, a game of heads-up-Pictionary. The host — the same
single binary running chess, `filter`, `chat`, `snake`, and the admin UI — owns the
**authoritative bitmap**, applies each incoming stroke to it on the single loop
thread (which serializes all writers automatically), and **broadcasts** the stroke to
every connected client so it appears on every screen at once. A device that joins
late gets a **snapshot** of the current wall (a PNG, encoded with the same stb codec
`filter` vendors) and then the live stroke feed. The operator runs the wall from the
ImGui panel — clear it, freeze it, save/load it, set a kid-safe palette.

It is the mirror image of `filter`'s privacy: `filter` answers each camera privately
and never broadcasts; `canvas` is **deliberately shared and public to the room**, and
broadcast *is* the point.

---

## 2. The model: an authoritative raster + an ordered op-log

The host keeps two things, both serializable (DESIGN §9.2):

- **The raster** — the current canvas as an RGBA bitmap (e.g. 1280×800). The single
  source of truth a snapshot is taken from.
- **The op-log** — the ordered list of recent **ops** (strokes, fills, clears) applied
  to it. The change feed clients subscribe to.

Every write is an **op** applied to the raster *on the loop thread, in arrival order*.
Because one thread applies them, the order is total and identical for everyone — there
is no concurrent mutation to reconcile (§5). The primary op is a **stroke** (an
ordered list of points + color + width, pressure folded into per-point width); a
**fill** (paint-bucket) and **clear** round it out. A 1-px stroke is "pixel art"; a
flood-fill is r/place's tile-paint — the same machinery covers the whole spectrum,
so there is no "stroke vs. pixel-grid" fork, only a brush setting (§11).

**The pure engine (`villen::canvas`)** is the §9.1 ruleset: apply an op to a raster,
deterministically, no I/O. CI tests assert that a fixed op sequence yields a fixed
raster hash — the chess/`filter` oracle pattern again. No GPU is required (rasterizing
a stroke is a cheap CPU line-walk at these sizes; GPU is a later optimization, §12).

---

## 3. §5 gives serialization for free (the crux)

The reason this engine is *easy* where it looks hard:

```cpp
void CanvasEngine::onText/onBinary(ConnId from, op bytes) {
  Op op = parse(bytes);
  raster_.apply(op);          // <-- one thread, arrival order => total order
  log_.push(op);
  ws_.broadcast(encode(op));  // everyone sees it, same order, no merge
}
```

No mutex, no CRDT, no OT, no vector clocks. DESIGN §5 forbade worker threads and
shared mutable state precisely so the host would never need locks; `canvas` cashes
that in as **conflict resolution by construction**. Two iPads coloring the same cell
in the same instant are simply two ops; whichever the loop reads first is applied
first; the second wins; both clients converge because both receive both ops in the
same broadcast order. The hardest-sounding part of a collaborative editor evaporates
because of a decision made for chess.

**Throughput, not contention, is the real load** here — a pen streams points fast.
That is bounded the §-native way: clients **coalesce** points into stroke segments and
send at a capped rate (§4); the host applies + rebroadcasts; the op is small. At LAN
scale with a dozen artists this is trivial, but it is the first engine to make the
*broadcast* path (DESIGN §6) carry sustained volume, which is the point of the test.

---

## 4. Wire contract (player edge — broadcast + snapshot)

Reuses the text path for ops and the **binary path** (`filter` §5) for snapshots.

**Client → server:**
```jsonc
{ "type": "stroke", "pts": [[x,y,pressure], …], "color": "#e23", "w": 4 }
{ "type": "fill",   "at": [x,y], "color": "#1c8" }
{ "type": "join" }                 // request the current snapshot
{ "type": "undoMine" }             // undo this author's last op (op-log scoped)
```
A live stroke streams as several `stroke` ops (coalesced segments) so others see it
draw in real time, not only on pen-up.

**Server → client:**
```jsonc
{ "type": "config", "w": 1280, "h": 800, "palette": ["#e23", "#1c8", …], "maxW": 24 }
{ "type": "op", "seq": 90412, "by": 3, "op": { "type":"stroke", … } }   // broadcast
{ "type": "snapshot", "seq": 90412 }   // text header; the PNG follows as a binary frame
{ "type": "clear", "by": "admin" }
```

**Late join is snapshot-then-subscribe:** on `join`, the host sends the current raster
as a PNG (stb-encoded, the binary frame) tagged with the latest `seq`, then streams
every `op` after it. The client paints the snapshot, then applies ops — converging
exactly, no replay-from-zero. Authority is server-side (chess §3.2): clients send
ops; the host decides order and is the only writer of the raster.

---

## 5. iPad / touch as a first-class client

The browser client (served from the static root, no build step) is **Pointer-Events
native**, which unifies pen/touch/mouse and is what makes it sing on iPad:

- **Pressure & tilt:** `PointerEvent.pressure` → per-point stroke width; the
  Apple-Pencil feel kids and artists expect.
- **High-rate sampling:** `getCoalescedEvents()` captures the pen's full sub-frame
  point stream, then the client **down-samples/coalesces** to a capped send rate (§3)
  — smooth locally, bounded on the wire.
- **Palm rejection / multi-touch:** treat `pointerType:"pen"` as draw, two-finger
  `touch` as pan/zoom, so a resting palm doesn't scribble.
- **No-scroll canvas:** `touch-action: none` + `preventDefault` so drawing doesn't
  pan the page; an explicit pan/zoom gesture for navigating a wall larger than the
  viewport.
- **Local echo:** the client paints its own stroke immediately and reconciles when the
  authoritative `op` echoes back (the host is still the source of truth, but the
  artist never feels lag).

Big swatches, a kid-safe palette, and a fat eraser keep it friendly.

---

## 6. Admin UI (the operator runs the wall)

`CanvasEngine::drawAdmin()`, gamepad-navigable: a live thumbnail of the wall; active
artists + op-rate + raster size; **Clear**, **Freeze** (read-only), **Save/Load**
(persist the PNG + op-log, §9.2), a **palette / max-width** editor (kid-safe limits),
and light **moderation** — because the op-log records authorship, the operator can
**undo one artist's recent ops** without a full clear. The admin reads/mutates the
`CanvasEngine` directly, in-process (DESIGN §9.4).

---

## 7. Shared, not private (the deliberate inversion)

`filter` and `chat` are private by design; `canvas` is the opposite and the doc says
so plainly: the wall is **public to the room**, broadcast to all, and persisted by the
operator. There is no per-connection secrecy to protect — only **moderation** (kids):
the operator's clear/freeze/undo-by-author controls, a bounded palette, and no text
entry, are the safety surface. (If a wall is ever meant to be private to a session,
that is an access scope on the session, not a change to the engine.)

---

## 8. Build, deps, and order

- **One reused dependency:** `third_party/stb` (already vendored for `filter`) for
  PNG/JPEG snapshot encode/decode. **No GPU, no subprocess.** Host-side raster ops are
  plain CPU.
- **CMake:** `villen_canvas` pure engine lib (raster + op-log + tests, always built,
  CI-tested, no I/O), like `villen_engine`/`villen_filter`/`villen_snake`; the
  `CanvasEngine` adapter compiles into the host.

**Build order (smallest-spine-first):**
1. **Pure engine + tests** — `apply(op, raster)`; op-log; deterministic raster-hash
   tests. *No host.*
2. **Host raster + broadcast** — apply ops on the loop, broadcast `op`; a scripted WS
   client draws. The shared-state spine, no UI.
3. **Browser client, one pointer** — draw + send strokes, render broadcasts. The
   shared wall is visible across two tabs.
4. **Snapshot late-join** — stb-encode the raster as a PNG binary frame on `join`,
   then subscribe. A third tab joins mid-mural and converges.
5. **iPad polish** — Pointer-Events, pressure, coalesced sampling, pan/zoom, palm
   rejection (§5).
6. **Admin** — thumbnail, clear/freeze/save/load, palette, undo-by-author.

---

## 9. Acceptance criteria

1. `engine/canvas/` builds and its deterministic raster-hash tests pass in headless
   CI.
2. `--engine canvas` selects it; other engines are unaffected.
3. Several iPads/tablets draw on one wall simultaneously; every device shows the same
   marks in the same order, with **no merge artifacts** (the §5 total-order property).
4. A device joining late receives a **snapshot** and converges exactly to the live
   wall.
5. Apple-Pencil pressure varies stroke width; palm/scroll don't interfere; drawing
   feels lag-free (local echo).
6. The operator clears/freezes/saves the wall and undoes one artist's ops from the
   admin UI.

---

## 10. Rejected alternatives

- **CRDT / operational transform.** Unnecessary — the single-thread loop already
  gives a total order (§3). Adding a merge algorithm would be solving a problem
  DESIGN §5 already dissolved.
- **Client-authoritative raster** (each client its own bitmap, gossip to peers). No —
  the host owns the wall (chess §3.2); a server raster is what makes late-join,
  snapshots, persistence, and moderation trivial.
- **A "pixel-grid mode" as a separate engine.** Folded in as a brush setting (1-px
  stroke / flood-fill, §2); same machinery, no fork.
- **Replay the whole op-log on join** instead of a snapshot. O(history); the raster
  snapshot is O(1) to send and is what the host already holds. Keep a *bounded* recent
  op-log for live sync only.
- **Stream the canvas as video** (the `filter` move). Wrong layer — send *ops*, render
  locally, snapshot for join; vector-crisp and a fraction of the bandwidth.
- **Per-connection privacy** (the `filter`/`chat` model). Inverted on purpose — the
  wall is shared; the slot supports both stances (DESIGN-filter §2).

---

## 11. Open questions

- **Pan/zoom & an infinite/tiled canvas** — a wall bigger than one screen; tile the
  raster and snapshot per-tile (the binary-tile transport generalizes).
- **Layers & per-author undo depth** — the op-log supports more than last-op undo;
  how deep, and per-author vs. global.
- **GPU-accelerated compositing** — at large sizes/many layers, rasterize strokes on
  the APU (the `filter` EGL context), a natural cross-engine reuse; unnecessary for
  the MVP.
- **Vector vs. raster persistence** — save the PNG (simple, lossy on re-edit) vs. the
  op-log (replayable, unbounded). Likely both: PNG for view, op-log for edit.
- **Export / print** the finished wall; a "gallery" of saved walls (leans on §9.2).
- **Pictionary / game modes** on top of the wall — a prompt, a guesser, a timer — the
  shared canvas as a substrate for actual games.
