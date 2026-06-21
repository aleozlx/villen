# Villen

[![CI](https://github.com/aleozlx/villen/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/aleozlx/villen/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

![Villen — portable game server](rc/readme-banner-1280x320.png)

> A portable **game server you carry** — a single native binary that runs the
> authoritative game, hosts the session, and serves remote players from their own
> browsers over the local network. No cloud, no accounts.

Villen is a generic host for
deterministic, turn-based, seat-based games. **Chess is the first game** built on
it — chosen because its rules engine stresses the spine (legality, end states,
turn order) without distractions. The engine is a swappable slot; the transport,
session/seat model, admin UI, and dual-input player client know nothing about
which game occupies it.

The name nods to a dragon of fantasy lore that lives disguised as an
unremarkable traveler — fitting for a server that presents as an everyday
handheld app and is something rarer underneath. See
[`docs/DESIGN-villen.md`](docs/DESIGN-villen.md) for the full design.

## Architecture at a glance

![Future-facing Villen architecture](docs/villen-future-architecture.svg)

The host is **one C++ executable** containing, in a single thread, a pluggable
**Game Engine**, the authoritative session/seat state, a WebSocket server for
remote players, and an in-process Dear ImGui admin UI that reads and mutates
that state directly — no admin socket, no IPC. Chess is the first engine, not
the limit of the architecture.

The only network boundary is **remote players' browsers**, speaking
JSON-over-WebSocket. The admin UI *is* the server with a face. A single 60 Hz
loop pumps the network and the UI on one thread, so there is no shared-state
locking (DESIGN §5).

The in-process admin UI (session/seat table, join URL + QR), reflecting a player
connected over WebSocket on the same thread:

![Villen admin UI](docs/admin-ui.png)

## Experimental engines (design drafts)

The slot is game-agnostic, so chess is only the first occupant. These are
**design drafts** (not yet built) — each chosen to stress a *different* axis of
the architecture. See [`docs/DESIGN-engine-roadmap.md`](docs/DESIGN-engine-roadmap.md)
for the full rationale, coverage matrix, and the "pick by axis, not by app"
selection method.

| Engine | What it is | Axis it stresses |
|---|---|---|
| [`filter`](docs/DESIGN-filter.md) | live camera → mathematical-morphology on the Deck's **APU** → processed frame back to the browser | streaming GPU compute, per-connection privacy, binary transport |
| [`chat`](docs/DESIGN-chat.md) | **local LLM chat** via llama.cpp (Llama 3.1 8B / Qwen2.5 7B / Mistral 7B) | seconds-long blocking work kept off the single loop |
| [`snake`](docs/DESIGN-snake.md) | a **real-time multiplayer arena** (port of [aleozlx/snake](https://github.com/aleozlx/snake)) — kids-friendly, wrap-around | an authoritative server clock + netcode |
| [`canvas`](docs/DESIGN-canvas.md) | a **shared collaborative drawing wall** (iPad-native) | many writers on one shared state |
| [`jam`](docs/DESIGN-jam.md) | a **clock-synced collaborative groovebox** — devices synthesize audio locally, in sync | tight cross-device shared *time* |

The Deck-side **launcher** that starts one of these at a time (plus a system-info
view) is designed in [`docs/DESIGN-admin-shell.md`](docs/DESIGN-admin-shell.md). A
forward-looking note on whether the appliance could patch its own minor bugs is in
[`docs/DESIGN-self-hotfix.md`](docs/DESIGN-self-hotfix.md).

## Repository layout

| Path | What |
|---|---|
| `engine/` | Pure chess engine — rules only, no I/O. Unit-tested in isolation. |
| `tests/`  | doctest suite (perft + special-rule coverage). |
| `host/`   | The native binary: WS server + in-process ImGui admin UI. |
| `client/` | Browser player client (pointer **and** gamepad input adapters). |
| `docs/`   | Design & handoff doc, architecture diagram, Steam Deck debugging guide, art brief. |
| `spike/`  | Throwaway Deck smoke-spike sources — slated to fold into the admin shell's **System Info** view ([`docs/DESIGN-admin-shell.md`](docs/DESIGN-admin-shell.md) §7). |

## Build

Requires a C++17 compiler, CMake ≥ 3.16, and (for the host) SDL2 + OpenGL.

The host's admin UI compiles Dear ImGui from the `third_party/imgui` git
submodule, so clone with `--recursive` (or initialise it after cloning). The
engine-only build below doesn't need it.

```bash
git clone --recursive https://github.com/aleozlx/villen
# already cloned without --recursive? initialise the submodule:
git submodule update --init third_party/imgui
```

```bash
# Debian/Ubuntu host dependencies
sudo apt-get install -y cmake ninja-build libsdl2-dev libgl1-mesa-dev zlib1g-dev

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Engine-only (no SDL2/OpenGL needed), e.g. for CI on a headless box:

```bash
cmake -S . -B build -DVILLEN_BUILD_HOST=OFF
cmake --build build && ctest --test-dir build
```

| CMake option | Default | Effect |
|---|---|---|
| `VILLEN_BUILD_TESTS` | `ON` | Build and register the engine unit tests. |
| `VILLEN_BUILD_HOST`  | `ON` | Build the native host. Uses SDL2 + OpenGL + Dear ImGui for the admin UI; degrades to a server-only host if SDL2/OpenGL are absent. |

## Run

```bash
./build/host/villen --port 9002        # opens the admin window if a display exists
./build/host/villen --port 9002 --headless   # server only (no window)
```

Then open `http://<host-ip>:9002` in a browser (the admin window shows the URL
and a QR code). Two browsers can each claim a seat and play; on one browser you
can move with the mouse **or** a gamepad interchangeably.

| Flag | Effect |
|---|---|
| `--port N` | TCP port for the player WebSocket + HTTP client (default 9002). |
| `--headless` | Run the server loop without opening the admin window. |
| `--client-dir DIR` | Serve the browser client from `DIR` (defaults to the source tree). |

## MVP build order (DESIGN §11)

- [x] **1.** Pure chess engine, standalone + unit tests (perft-validated)
- [x] **2.** WebSocket server in a bare main loop (one hardcoded session)
- [x] **3.** Browser player client, pointer input
- [x] **4.** Gamepad adapter into the same move intake (architectural milestone, §7)
- [x] **5.** Seats + two browsers, turn enforcement, rejection path
- [x] **6.** Deck smoke spike *(on real hardware — see DESIGN §11.1; not buildable in CI)*
- [x] **7.** ImGui admin shell in the same binary

> **Transport note:** the design names µWebSockets as the player transport. The
> server here is a small, self-contained RFC 6455 implementation driven by the
> single main-loop `poll()` (DESIGN §5), kept behind a poll-shaped seam so µWS
> can drop in later without touching the engine or session layers (§9.5).
> Performance is a non-issue at LAN/chess message volume.

## Beyond the MVP

Increments past the load-bearing spine (DESIGN §13):

- [x] **Reconnection & seat lifecycle** — a dropped player's seat is *held*
  (`disconnected`) instead of vacated, so a mid-game drop never hands the side to
  the opponent. Token-free recovery: a transient drop reclaims the seat by name,
  or the host re-opens it from the admin UI's per-seat **Free** control (§13 #1).

## License

[MIT](LICENSE) © 2026 Alex Yang
