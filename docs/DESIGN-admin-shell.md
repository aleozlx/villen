# Villen — the admin shell: launcher, system-info, and engine views (design & coordination)

> The in-process admin UI today draws **one** thing: the chess session table
> ([`admin_ui.cpp`](../host/src/admin_ui.cpp)), and the engine is chosen by a startup
> flag. With several engines coming ([`DESIGN-engine-roadmap.md`](DESIGN-engine-roadmap.md)),
> the operator on the Deck needs a way to **pick and run one at a time** without SSH or
> command-line flags. This doc specifies that shell: a **launcher** home view, a
> **system-info** view (absorbing the throwaway `spike/deck`), and per-engine views,
> with **exactly one engine active at a time**.

**Status:** design / **coordination — settle before fanning out engine work.** Every
engine's `drawAdmin()` plugs into this shell, so its contract (§8) must be fixed first.
**Scope:** the top-level structure of the operator UI on the Deck and how it selects,
runs, and tears down a single engine.
**Audience:** the agent building the shell, **and** every agent building an engine (read
§8). Assumes [`DESIGN-filter.md`](DESIGN-filter.md) §2 (the `IEngine` slot) and
[`DESIGN-villen.md`](DESIGN-villen.md) §5 (the single loop).

---

## 1. The question this answers

`filter` §2 introduced engine selection as a **startup flag** (`--engine chess|filter|
…`). That is fine for a PC, CI, or a kiosk, but on the Deck in **Game Mode** there is no
shell prompt and the operator wants to switch games without re-launching from a
terminal. So the GUI itself must let the operator pick. The model the operator already
sketched, and that this doc adopts:

- the **first view is a launcher**, not a session table;
- it can open a **system-info** view (a button), and an **engine** view (pick a game);
- **one engine launches at a time** — no concurrent multi-launch (that's the lobby idea,
  deliberately deferred — [roadmap §6](DESIGN-engine-roadmap.md); we are not building an
  OS).

---

## 2. The shell = a top-level view router in the one loop

No new threads, no new windows: the existing single ImGui loop
([`admin_ui.cpp:195`](../host/src/admin_ui.cpp)) gains a **current view** enum and draws
one of three views per frame. "Opens other windows" means **switches the active view**,
not OS windows — under Gamescope there is one fullscreen surface.

```
            ┌──────────────────────────────────────────┐
   boot ───►│  LAUNCHER  (home)                         │
            │   [chess] [filter] [chat] [snake]         │
            │   [canvas] [jam]      ·  pick one ──────► ENGINE view
            │   · System Info ───────────────────────► SYSINFO view
            │   · Quit                                  │
            └──────────────────────────────────────────┘
   ENGINE view ── Home ──► LAUNCHER        SYSINFO view ── Back ──► LAUNCHER
```

- **Launcher (home, the default view).** A grid/list of the available engines (name +
  one-line + later an icon); selecting one **constructs and starts** that engine and
  switches to its view. Plus a **System Info** button and **Quit**.
- **System Info.** The diagnostics the `spike/deck` program prints — SDL video driver,
  `GL_VERSION`/`GL_RENDERER` (the **real `radeonsi`, not `llvmpipe`** check,
  steamdeck-debugging §4), controllers, LAN IP/port, build/glibc info — promoted into a
  **permanent operator view**, reachable from the launcher (§7).
- **Engine view.** The active engine's `drawAdmin()` body, wrapped in shared **chrome**:
  a **Home** affordance (stop the engine → launcher), the **join URL + QR**, and the
  **live connection count**. The engine draws only its own body (§8).

---

## 3. One engine active at a time (the load-bearing constraint)

Exactly one `IEngine` is instantiated at any moment; the launcher is the state where
**none** is. This is not a limitation to apologize for — it is correct for the appliance:

- **Resource exclusivity.** `filter` wants the APU for GL compute; `chat` wants it for
  Vulkan inference + a `llama-server` subprocess holding gigabytes of model; `snake`/
  `jam` want the loop's tick budget. Running two heavy engines at once would contend for
  the one APU and the 16 GB unified memory (DESIGN-chat §5). One-at-a-time sidesteps all
  of it.
- **The single loop stays simple** (DESIGN §5): one engine's `tick()`/`onMessage`, never
  two interleaved.
- **It's why the lobby/multi-session multiplexer is deferred** ([roadmap §6](DESIGN-engine-roadmap.md)):
  concurrent sessions edge toward a window-manager/OS, far more than the single-active
  model needs now.

---

## 4. Lifecycle & host wiring (reconciling with `IEngine`)

The host owns the `WsServer`, a **nullable** `IEngine* active_`, and the `view_` state.

```cpp
// pick from the launcher
void startEngine(EngineKind k) {
  active_ = makeEngine(k, ws_);                 // construct (e.g. FilterEngine opens its
  ws_.setCallbacks(routeTo(active_));           //  EGL ctx; ChatEngine spawns llama-server)
  broadcastEngineChanged(k);                    // tell connected clients (§5)
  view_ = View::Engine;
}
// Home from the engine view
void stopEngine() {
  ws_.setCallbacks(routeTo(nullptr));           // inbound -> "no engine" replies
  active_.reset();                              // destruct: free GPU ctx / kill subprocess
  broadcastEngineChanged(none);
  view_ = View::Launcher;
}
```

- **Clean teardown is a hard requirement** on every engine's destructor: `filter`
  releases its EGL context, `chat` terminates and reaps `llama-server`, `snake`/`canvas`/
  `jam` drop their state. The next engine must start from a clean slate. (This is the
  one real cost of in-process switching — see the rejected alternative in §10.)
- **`--engine X` still works**: with the flag, the host **boots straight into engine X**
  and the launcher is skipped (kiosk / headless / CI / dev). With **no flag and a
  display**, the host boots to the **launcher**. Headless with no flag keeps today's
  behaviour (server loop, no UI) — pick a default engine or require the flag.

---

## 5. Connected clients across a switch

A browser is connected over WS while the operator changes engines. The host announces
the active engine so the page can show the right thing:

```jsonc
{ "type": "engine", "name": "snake" }     // sent on connect and on every switch
{ "type": "engine", "name": null }        // launcher / no engine running
```

- The served landing page reads this and **loads the matching client view module**
  (each engine ships its own `client/<engine>/` view; the board, the canvas, the jam
  grid, …). On `null` it shows a friendly "no game running — ask the host to start one."
- **Simplest MVP:** a switch may just drop player connections and let them reconnect into
  the new engine's view; live migration of an in-flight connection is a polish item
  (§11).

Authority and privacy are unchanged per engine (each engine keeps its own contract —
chess/snake/canvas/jam broadcast, filter/chat are private).

---

## 6. Input & navigation on the Deck (a real care-point)

The shell is gamepad-navigable (`NavEnableGamepad`, already set
[`admin_ui.cpp:183`](../host/src/admin_ui.cpp)): D-pad to move, **A** to launch, **B**/a
Home button to go back. But there is a genuine clash to design around:

> **When an engine consumes the gamepad as *gameplay* (snake), the operator's "Home"
> button can't also be a gameplay button.** The admin nav and the engine's player input
> would fight over the same pad.

Options (decide in the shell, not per engine): reserve a **non-gameplay route home** —
the Steam/Quick-Access overlay, a touchscreen-only on-screen **Home** chip the shell
always draws, or a deliberate **chord** (e.g. hold Start) the engine never uses. The
shell owns this; engines must not bind it (§8). Flagged as the top open question (§11).

---

## 7. Retiring `spike/deck` into the System-Info view (the cleanup)

The Deck smoke spike (DESIGN §11 step 6, `spike/deck/`) exists to print what the Deck
exposes — SDL driver, `GL_RENDERER`, controllers — to screen and a logfile. The README
already calls it *"the seed for Step 7's diagnostics window."* This view **is** that
window: lift the spike's probing into the **System Info** view as a permanent operator
panel (renderer sanity, controller list, network IP/port, build + glibc info). Once the
view exists and shows the same facts, `spike/deck` has done its job and can be deleted
(or kept purely as reference). This finally closes out the spike instead of leaving it
indefinitely "kept."

---

## 8. The `drawAdmin()` contract (what engine authors must follow)

So engines built in parallel stay consistent, fix this now:

1. An engine implements `drawAdmin()` to draw **only its own panel body** — controls and
   stats for that engine. It does **not** open a top-level/fullscreen window, draw a
   Home/Back button, or render the join URL/QR/connection count — **the shell owns all
   chrome and navigation.**
2. An engine must **not** bind the shell's reserved "go Home" input (§6).
3. An engine's **constructor acquires** its resources (GPU ctx, subprocess, state) and
   its **destructor fully releases** them; construct/destruct may happen many times in
   one process as the operator switches (§4).
4. `drawAdmin()` is a **no-op when headless** (no display); the engine still runs (serves
   players) without it, exactly as the host does today.

---

## 9. Build order

1. **View router + launcher** — add `view_` and a launcher listing only the engines that
   exist (chess today); selecting chess shows today's table inside the engine view with
   shared chrome. Prove launcher ↔ chess navigation.
2. **System Info view** — port `spike/deck`'s probes; reachable from the launcher (§7).
3. **Runtime construct/teardown** — `start/stopEngine` with ws-callback rerouting; prove
   a clean chess teardown → launcher → chess again (no leaks).
4. **`engine` announce + client routing** — the landing page loads the active engine's
   view module (§5), chess first.
5. **Wire each engine** into the launcher as it lands (filter/chat/snake/canvas/jam),
   each honouring §8.

---

## 10. Rejected alternatives

- **Multi-launch / a lobby running several engines at once.** Deferred (§3, roadmap §6):
  it's an OS/window-manager's job, not what the appliance needs. One active engine.
- **Relaunch-the-binary-per-engine** (a supervisor wrapper picks the engine, each runs in
  a fresh process). *Tempting* — it gives crash isolation and a guaranteed-clean slate
  (no teardown bugs), and dovetails with the self-hotfix A/B wrapper
  ([`DESIGN-self-hotfix.md`](DESIGN-self-hotfix.md)) and `chat`'s subprocess. **Not
  chosen as primary** because it breaks the single-binary/single-loop in-process model
  (the launcher itself would have to live in the wrapper, and switching means a process
  restart + reconnect every time). Recorded as a real fork: if in-process teardown proves
  leaky (a stuck EGL context, a zombie `llama-server`), relaunch-per-engine is the escape
  hatch.
- **Keep `--engine` as the only selector, no launcher.** Bad Deck UX — no terminal in
  Game Mode. The launcher is the Game-Mode face; the flag stays for kiosk/headless/CI.
- **Each engine draws its own window/chrome/home.** Inconsistent and causes the §6 nav
  clash. The shell owns chrome; engines own bodies (§8).

---

## 11. Open questions

- **The gamepad Home affordance when an engine owns the pad** (snake) — overlay vs.
  touchscreen Home chip vs. reserved chord (§6). The first thing to settle in build.
- **Client-side multi-engine routing** — one landing page that swaps view modules vs. a
  redirect per engine; how an in-flight connection migrates vs. reconnects (§5).
- **In-process teardown reliability** — verify `filter`'s EGL context and `chat`'s
  `llama-server` fully release between switches; if not, escalate to relaunch-per-engine
  (§10).
- **Kiosk mode** — `--engine X` could also *lock* the launcher (a fixed-purpose
  appliance: "this Deck is the snake cabinet").
- **Remembering the last engine** and an idle "attract" view on the launcher.
- **Per-engine icons/art** for the launcher (the `rc/` art set already exists).
