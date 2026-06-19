# CLAUDE.md

Guidance for Claude Code working in this repo. Start with
[`docs/DESIGN-villen.md`](docs/DESIGN-villen.md) — the full design & handoff.

## What this is
Villen is a portable, single-binary game *host* you carry (eventually on a Steam
Deck): one C++ executable that runs the authoritative game engine, the
session/seat state, a WebSocket server for remote browser players, and an
in-process Dear ImGui admin UI — all in **one thread, one main loop** (DESIGN §5).
Chess is the first engine in a deliberately game-agnostic slot.

## Build
```bash
# Engine + tests only (headless, no SDL2/GL — what CI runs):
cmake -S . -B build -DVILLEN_BUILD_HOST=OFF && cmake --build build && ctest --test-dir build
# Full host (needs SDL2 + OpenGL; admin UI's ImGui is a submodule):
git submodule update --init third_party/imgui   # once, if not cloned --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

## Layout
- `engine/` pure rules, no I/O · `host/` the native binary · `client/` browser player
- `tests/` doctest suite · `docs/` design doc + guides · `spike/` kept Deck smoke spike
- `third_party/` vendored deps (nlohmann single-header; Dear ImGui is a submodule per DESIGN §8)

## Working on the Steam Deck
**Read [`docs/steamdeck-debugging.md`](docs/steamdeck-debugging.md) before any Deck
build/deploy.** The short version: the Deck has no toolchain and a read-only
rootfs, so you **cross-build on a PC and copy**; if the PC's glibc is newer than
the Deck's, the binary aborts in the loader before `main()` (looks like an instant
crash) — static-link libstdc++ and `.symver`-pin the offending libc symbols, and
verify with `objdump -T <bin> | grep GLIBC_`. The Game-Mode-only risks (Gamescope
window, Steam Input → SDL2 gamepad, GL context) are covered there too.

## Conventions
- Keep the **engine pure** (no graphics/socket/device code) and all network
  concerns on the player WebSocket edge (DESIGN §9).
- C++17, "C with destructors" style: flat, allocation-visible, RAII for handles.
- **Do not commit local infrastructure details** (personal IPs, hostnames,
  account names, keys). Generic Steam Deck / SteamOS facts are fine.
