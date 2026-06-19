# Villen

[![CI](https://github.com/aleozlx/villen/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/aleozlx/villen/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)

> A portable **game server you carry** — a single native binary that runs the
> authoritative game, hosts the session, and serves remote players from their own
> browsers over the local network. No cloud, no accounts.

The architecture is **not chess-specific**: Villen is a generic host for
deterministic, turn-based, seat-based games. **Chess is the first game** built on
it — chosen because its rules engine stresses the spine (legality, end states,
turn order) without distractions. The engine is a swappable slot; the transport,
session/seat model, admin UI, and dual-input player client know nothing about
which game occupies it.

The name is a diminutive of **Villentretenmerth**, the golden dragon of
Sapkowski's *Sword of Destiny* who lives disguised as the ordinary man "Borch" —
fitting for a server that presents as an everyday handheld app and is something
rarer underneath. See [`DESIGN-villen.md`](DESIGN-villen.md) for the full design.

## Architecture at a glance

The host is **one C++ executable** containing, in a single thread, the engine,
the authoritative session/seat state, a WebSocket server for remote players, and
an in-process Dear ImGui admin UI that reads and mutates that state directly — no
admin socket, no IPC.

```
ONE NATIVE BINARY (eventually on a Steam Deck)
├─ engine                       pure rules; no I/O          (engine/)
├─ session / seat state         in-memory, authoritative    (host/)
├─ WS server  ◄───────────────  remote player browsers      (host/, client/)
└─ ImGui loop  ──reads/mutates──►  session state directly   (host/)
        (SDL2 + GL3 backend; gamepad-navigable)
```

The only network boundary is **remote players' browsers**, speaking
JSON-over-WebSocket. The admin UI *is* the server with a face. A single 60 Hz
loop pumps the network and the UI on one thread, so there is no shared-state
locking (DESIGN §5).

## Repository layout

| Path | What |
|---|---|
| `engine/` | Pure chess engine — rules only, no I/O. Unit-tested in isolation. |
| `tests/`  | doctest suite (perft + special-rule coverage). |
| `host/`   | The native binary: WS server + in-process ImGui admin UI. |
| `client/` | Browser player client (pointer **and** gamepad input adapters). |
| `DESIGN-villen.md` | Full design & implementation handoff. |

## Build

Requires a C++17 compiler, CMake ≥ 3.16, and (for the host) SDL2 + OpenGL.

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
| `VILLEN_BUILD_HOST`  | `ON` | Build the native host (needs SDL2 + OpenGL; fetches uWebSockets). |

## MVP build order (DESIGN §11)

- [x] **1.** Pure chess engine, standalone + unit tests
- [ ] **2.** µWS server in a bare main loop (one hardcoded session)
- [ ] **3.** Browser player client, pointer-only
- [ ] **4.** Gamepad adapter into the same move intake (architectural milestone, §7)
- [ ] **5.** Seats + two browsers, turn enforcement, rejection path
- [ ] **6.** Deck smoke spike *(on real hardware — see DESIGN §11.1; not buildable in CI)*
- [ ] **7.** ImGui admin shell in the same binary

## License

[MIT](LICENSE) © 2026 Alex Yang
