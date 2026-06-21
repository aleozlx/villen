# CLAUDE.md

Guidance for Claude Code working in this repo. Start with
[`docs/DESIGN-villen.md`](docs/DESIGN-villen.md) — the full design & handoff.

Beyond chess, a slate of **experimental engines** is being designed on top of the
game-agnostic slot (`filter`, `chat`, `snake`, `canvas`, `jam`). The index — with
*how each was chosen* (one architectural axis each) — is
[`docs/DESIGN-engine-roadmap.md`](docs/DESIGN-engine-roadmap.md); the Deck-side
**launcher** that runs one engine at a time is
[`docs/DESIGN-admin-shell.md`](docs/DESIGN-admin-shell.md). These are **design
drafts**, not yet built.

## What this is
Villen is a portable, single-binary game *host* you carry (and now actually
deployed on a Steam Deck — see below): one C++ executable that runs the
authoritative game engine, the session/seat state, a WebSocket server for remote
browser players, and an in-process Dear ImGui admin UI — all in **one thread, one
main loop** (DESIGN §5). Chess is the first engine in a deliberately game-agnostic
slot.

## What runs where (counterintuitive — read this)
- **The browser player client** (`client/`) is plain HTML/CSS/JS over
  HTTP + WebSocket with **no build step and no bundler** (ES modules loaded
  directly). The board, highlights, promotion, *and* the attack-control heatmap
  (`client/src/heatmap.js`) are all client-side DOM/JS. It uses **no SDL2, no
  OpenGL**.
- **SDL2 + OpenGL are server-side only.** The host binary links them (plus Dear
  ImGui, the submodule) purely for its *own* in-process admin window — the
  operator's session/seat table + join QR, shown on the Deck (DESIGN §2, §8). A
  chess server pulling in a GPU UI toolkit looks wrong until you see it's the
  admin *face*, in-process by design — never a client renderer.
- The host also **builds and runs without SDL2/GL**: CMake falls back to a
  server-only host and `main.cpp` runs headless (no admin window) when there's no
  display — e.g. over SSH. So engine/server/client work never needs a GPU or a
  display; only the on-Deck admin UI does.

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
- `tests/` doctest suite · `docs/` design docs + guides · `spike/` Deck smoke spike (→ admin-shell System Info view)
- `third_party/` vendored deps (nlohmann single-header; Dear ImGui is a submodule per DESIGN §8)

## Working on the Steam Deck
**Read [`docs/steamdeck-debugging.md`](docs/steamdeck-debugging.md) before any Deck
build/deploy.** The short version: the Deck has no toolchain and a read-only
rootfs, so you **cross-build on a PC and copy**; if the PC's glibc is newer than
the Deck's, the binary aborts in the loader before `main()` (looks like an instant
crash) — static-link libstdc++ and `.symver`-pin the offending libc symbols, and
verify with `objdump -T <bin> | grep GLIBC_`. The Game-Mode-only risks (Gamescope
window, Steam Input → SDL2 gamepad, GL context) are covered there too.

**This is not hypothetical — the host runs on a Steam Deck now**, as a non-Steam
shortcut launched in Game Mode, living in the `deck` user's home at `~/Villen/`
(`villen` binary + `client/` + `art/` + `run-villen.sh`, which `cd`s to its folder
and runs `./villen --client-dir ./client`). `deploy/Villen/` is the local staging
mirror of that bundle (untracked). Launched over SSH it runs headless (no
`$DISPLAY`), serving players but with no admin window — the ImGui UI appears only
in Game Mode.

**Client-only changes are cheap — no rebuild, no restart.** The host serves
`client/` straight from disk on every HTTP request (`ws_server.cpp`, via
`--client-dir`), so shipping a UI change (the heatmap, e.g.) is just: sync
`client/` into the Deck's `~/Villen/client/` and hard-refresh the browser (ES
modules cache hard). Only changes to the **C++ host** (engine / server / admin UI)
need the cross-build-and-copy dance above.

## Conventions
- Keep the **engine pure** (no graphics/socket/device code) and all network
  concerns on the player WebSocket edge (DESIGN §9).
- C++17, "C with destructors" style: flat, allocation-visible, RAII for handles.
- **Brace every `if`/`else`/`for`/`while`/`do` body, even a single statement, with
  the body on its own line** — no unbraced guard clauses or early returns, and no
  packing it onto the condition line (`if (x) { stmt; }`); `else if` chains stay
  flat (write `else if (...) { }`, not an extra outer brace layer). Guards against
  the `goto fail;`-class bug and keeps later diffs safe.
- **Do not commit local infrastructure details** (personal IPs, hostnames,
  account names, keys). Generic Steam Deck / SteamOS facts are fine.

When **reviewing** a change — whether you are a human reviewer or the CI
`claude-review` bot — apply [`docs/CODE-REVIEW.md`](docs/CODE-REVIEW.md): the
review standard, the Villen architectural invariants, and the full style rules
(including the brace rule above).
