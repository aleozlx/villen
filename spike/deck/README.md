# Deck smoke spike (kept — diagnostics-window seed)

A ~150-line SDL2 + OpenGL3 + Dear ImGui program written for **MVP build step 6**
(see [`../../docs/DESIGN-villen.md`](../../docs/DESIGN-villen.md) §11). Its job was
to retire the Deck-specific risks (Gamescope compositing, Steam Input → SDL2
gamepad, GL context under Gamescope) against disposable code *before* the real
admin UI exists. It passed on real hardware — all of §11.1 #1–#4 cleared.

It was meant to be throwaway, but it's kept because it's the natural **seed for
Step 7's in-admin "diagnostics" window**: it already enumerates the GL driver,
the controllers Steam Input exposes, and the SDL video backend, and draws a
gamepad-navigable ImGui table — exactly the panel a "what does this Deck see?"
diagnostics view needs.

It carries **no** engine / server / network / infra code — pure windowing,
input, and GL probing.

## Layout
- `main.cpp` — the spike: window + GL context, ImGui frame, a dummy sessions
  table, one gamepad-navigable button, and on-screen + logfile diagnostics.
- `glibc_compat.h` — `.symver` pin so a binary built on a newer-glibc PC still
  loads on the Deck (see the debugging guide).
- `CMakeLists.txt` — standalone build, **not** part of the main `villen` build.

## Building
Needs SDL2 + OpenGL dev headers and Dear ImGui. The ImGui clone is gitignored
(it becomes a `third_party/imgui` submodule in Step 7, per DESIGN §8); for a
standalone build, drop ImGui at `spike/deck/imgui/`:

```bash
git clone --depth 1 https://github.com/ocornut/imgui spike/deck/imgui
cmake -S spike/deck -B build-spike -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-spike
```

Deploying to a Steam Deck is **not** a plain copy — see
[`../../docs/steamdeck-debugging.md`](../../docs/steamdeck-debugging.md).
