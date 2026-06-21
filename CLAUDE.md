# CLAUDE.md

Guidance for Claude Code working in this repo. Start with
[`docs/DESIGN-villen.md`](docs/DESIGN-villen.md) — the full design & handoff.

Beyond chess, a slate of **experimental engines** sits on the game-agnostic slot
(`filter`, `chat`, `snake`, `canvas`, `jam`). **`filter` (real-time mathematical
morphology on the Deck's APU, [`docs/DESIGN-filter.md`](docs/DESIGN-filter.md)) and
`chat` (a local LLM via a managed llama-server, [`docs/DESIGN-chat.md`](docs/DESIGN-chat.md))
are now built and shipping**; `snake`/`canvas`/`jam` are still design drafts. The
roadmap — with *how each was chosen* (one architectural axis each) — is
[`docs/DESIGN-engine-roadmap.md`](docs/DESIGN-engine-roadmap.md); the **launcher**
that runs one engine at a time (also built) is
[`docs/DESIGN-admin-shell.md`](docs/DESIGN-admin-shell.md).

## What this is
Villen is a portable, single-binary game *host* you carry (and now actually
deployed on a Steam Deck — see below): one C++ executable that runs the
authoritative game engine, the session/seat state, a WebSocket server for remote
browser players, and an in-process Dear ImGui admin UI — all in **one thread, one
main loop** (DESIGN §5). Chess was the first engine in a deliberately game-agnostic
slot; the host now carries **chess, filter, and chat** behind the `villen::IEngine`
contract and runs one at a time (the launcher picks; `--engine NAME` boots straight
into one for kiosk/CI).

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
- **`filter` adds a *second*, different server-side GPU use — compute, not UI.**
  Surfaceless EGL + GLES 3.1 **compute** shaders on the render node
  (`/dev/dri/renderD128`) run the morphology on the APU, in a headless context
  *independent* of the admin window's OpenGL context (two GL contexts, one thread).
  It's validated **byte-exact** against the pure-CPU reference in `engine/filter/`
  (`./villen --filter-selftest` is that oracle), and guarded like the admin UI:
  absent EGL/GLES, filter degrades to the CPU reference. (`chat` offloads to the
  same APU too, but out-of-process — a llama-server child over Vulkan.) Browser
  clients still touch no GPU: filter's client only captures a camera frame and
  paints the host's processed reply.
- The host also **builds and runs without SDL2/GL**: CMake falls back to a
  server-only host and `main.cpp` runs headless (no admin window) when there's no
  display — e.g. over SSH. So engine/server/client work never needs a GPU or a
  display; only the on-Deck admin UI does.

## Build
```bash
# Engine + tests only (headless, no SDL2/GL — fast inner loop for engine work):
cmake -S . -B build -DVILLEN_BUILD_HOST=OFF && cmake --build build && ctest --test-dir build
# Full host (needs SDL2 + OpenGL; admin UI's ImGui is a submodule). THIS is what CI
# builds — on gcc AND clang — and tests, so it's the config that gates merges; it
# compiles all host code (engine adapters, filter GPU backend, chat backend) and runs
# every ctest suite (engine_tests, filter_tests, chat_tests, integration_tests,
# chat_e2e). The headless line above is only a dev shortcut and does NOT cover host/
# code. The filter GPU path is exercised separately on real hardware: ./villen
# --filter-selftest (byte-exact vs the CPU reference; CI has no render node).
git submodule update --init third_party/imgui   # once, if not cloned --recursive
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build && ctest --test-dir build
```

## Layout
- `engine/` pure rules, no I/O — one pure core per engine (`engine/` chess rules,
  `engine/filter/` morphology, `engine/chat/` conversation+prompt) · `host/` the
  native binary, with engine adapters under `host/src/engines/<name>/` · `client/`
  browser player, one subdir per engine (`client/filter/`, `client/chat/`)
- `tests/` doctest suite · `docs/` design docs + guides · `tools/` dev/build scripts
  (incl. the tracked Deck cross-build + deploy) · `spike/` Deck/bench spikes
- `third_party/` vendored deps (nlohmann + stb single-headers; Dear ImGui is a submodule per DESIGN §8)

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
(`villen` binary + `client/` + `art/` + a `run-villen*.sh` wrapper, which `cd`s to
its folder and runs `./villen --client-dir ./client`, optionally `--engine NAME`).
`deploy/Villen/` is the local staging mirror of that bundle (**untracked** — the
binary and the device-specific launcher carry infra details). Launched over SSH it
runs headless (no `$DISPLAY`), serving players but with no admin window — the ImGui
UI appears only in Game Mode.

**The cross-build + deploy is captured in two tracked, infra-free scripts** (prefer
them over hand-rolling cmake flags): [`tools/build-deck.sh`](tools/build-deck.sh)
(Release cross-build + the `.symver`/static-libstdc++ treatment + `objdump` glibc
gate, stages `deploy/Villen/`) and [`tools/deploy-to-deck.sh`](tools/deploy-to-deck.sh)
(`DECK=user@host` rsync; backs up the remote binary, syncs `client/` *without*
`--delete`, and never overwrites the operator's device-specific `run-villen.sh`).

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
  the `goto fail;`-class bug and keeps later diffs safe. The checked-in
  `.clang-format` enforces this (and the rest of the house style); run
  `clang-format -i` on files you add, but don't bulk-reformat pre-existing files.
  See [`docs/CODE-REVIEW.md`](docs/CODE-REVIEW.md) for the dev-binary setup.
- **Do not commit local infrastructure details** (personal IPs, hostnames,
  account names, keys). Generic Steam Deck / SteamOS facts are fine.

When **reviewing** a change — whether you are a human reviewer or the CI
`claude-review` bot — apply [`docs/CODE-REVIEW.md`](docs/CODE-REVIEW.md): the
review standard, the Villen architectural invariants, and the full style rules
(including the brace rule above).
