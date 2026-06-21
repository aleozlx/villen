# Villen — the `filter` engine: real-time mathematical morphology on the APU (design & handoff)

> **`filter`** is the second engine to occupy Villen's game slot. Where chess is
> deterministic, turn-based, shared-state, and broadcast, `filter` is real-time,
> turn-free, per-connection, and point-to-point: each player's browser streams its
> **camera** to the host, the host runs **mathematical-morphology** image operators
> on the **Steam Deck's APU**, and streams **only the processed result** back to
> that same browser. The raw camera is never broadcast, never stored, never leaves
> the LAN. The name is deliberately plain — the engine *is* a filter — and derives
> mechanically: namespace `villen::filter`, lib `villen_filter`, this doc
> `DESIGN-filter.md`.

**Status:** design / additive. A new engine plus the generalized slot it forces.
**Scope:** the full slice that proves a streaming GPU engine fits Villen's spine —
camera in, morphology on the APU, processed frame out, operator-tuned pipeline.
**Audience:** the engineer who has read [`DESIGN-villen.md`](DESIGN-villen.md) and
will stand this up. Assumes the existing host (single binary, single loop, WS
player edge, in-process ImGui admin) and C++/GL familiarity.

> **Why this engine earns its keep.** Villen claims its host is *game-agnostic* —
> "the engine is a swappable slot; the transport, session/seat model, admin UI, and
> client know nothing about which game occupies it" (DESIGN intro, §9). Chess never
> tested that claim, because chess *is* the shape the spine was written around
> (two seats, strict turns, one shared position, full-state broadcast). `filter`
> tests it along every axis chess didn't: **no turns, no seats, no shared state,
> no broadcast, continuous frames instead of discrete moves, and a GPU instead of a
> branch of pure logic.** If the slot survives `filter`, the slot is real. Paying
> that abstraction down is half the point; the live morphology demo is the other
> half.

---

## 1. Context & one-paragraph pitch

A browser on the LAN opens the `filter` client, grants camera access, and starts
pushing downscaled JPEG frames to the host over the existing WebSocket. The host —
the same single C++ binary that runs chess and the in-process admin UI — decodes
each frame, uploads it to the **Steam Deck APU** (AMD Van Gogh, `radeonsi`), runs a
configurable **chain of morphology operators** (erode, dilate, open, close,
gradient, top-hat, …) as GL **compute shaders**, reads the result back, re-encodes
it as JPEG, and sends it to **that one connection**. The browser paints the
processed frame to a canvas. The operator, on the Deck, tunes the pipeline live in
the ImGui admin panel — structuring-element shape and radius, the operator
sequence, thresholds — and every connected feed reflects the change on its next
frame.

The subversion is the Villen house style: a "game engine" that is really a
real-time GPU image processor, the same way the host is a "chess server" that is
really a portable GPU/admin appliance. But the engineering is not a gag — it
stresses transport (binary, high-rate), compute (headless GPU on the APU),
concurrency (per-frame work inside a 60 Hz cooperative loop), and privacy
(per-connection media that must never broadcast). Those are the design's real
content.

---

## 2. Two seams, kept honest (the engine slot, generalized)

DESIGN §9.1 says "the engine is pure — no graphics, no socket, no device code."
A morphology engine on the APU is *intrinsically* graphics/device code, so a naïve
read says it can't be an engine at all. The resolution is to be precise about
**which** seam is pure, because Villen already has two and only conflates them in
prose:

| Seam | Lives in | Purity | Chess instance | `filter` instance |
|---|---|---|---|---|
| **Pure ruleset** | `engine/` | No I/O, no device, deterministic, CI-tested | `chess::Position` (`apply`/`legalMoves`) | `filter::process(frame, params) -> frame` — a **CPU reference** morphology implementation |
| **Engine/session adapter** | `host/` | Binds a ruleset to the WS edge + admin; *may* touch device/transport | `GameServer` (today, hardcoded) | `FilterGame` (CPU reference **or** GPU backend, behind the same interface) |

The pure morphology lives in `engine/filter/` as a **plain-CPU reference** that
takes a pixel buffer and a pipeline description and returns a pixel buffer — no GL,
no sockets, fully deterministic, unit-tested in CI exactly like the chess engine.
That satisfies §9.1 unchanged.

The **APU is an acceleration of that reference, not its definition.** The GL-compute
backend lives host-side and must produce output that matches the CPU reference
(bit-exact for integer ops; see §4). This is *exactly* the discipline Villen
already applies to SDL2/OpenGL: DESIGN §2/§8 keep GL "server-side only … for its
own admin face," not as engine logic. `filter` extends that one notch — GL compute
is "server-side only … for the engine's accelerated face" — and gains a free
oracle: CI tests the CPU reference; the GPU backend is validated against it.

**The slot itself.** The host-side adapter is the canonical **`villen::IGame` contract**
([`DESIGN-game-framework.md`](DESIGN-game-framework.md) §4) — `onJoin`/`onLeave`/
`onMessage`/`onTick`/`drawAdmin`, driven through a `Room` handle. `GameServer` today *is*
the chess adapter, hardcoded ([`session.hpp:71`](../host/src/session.hpp) holds a
`chess::Position`); the extraction makes it `ChessGame : villen::IGame`, and `filter`
is a new `FilterGame : villen::IGame`. It's a **multi-game host**: both link into the
one binary and the launcher activates one at a time
([`DESIGN-admin-shell.md`](DESIGN-admin-shell.md)), so the single-binary/single-loop
story (DESIGN §2, §5) is untouched.

`filter` leans on three properties of that contract:

- **`onMessage` carries opaque bytes** — `filter`'s control JSON *and* its binary JPEG
  frames both arrive as the game's payload; the one transport addition is that the WS
  edge must deliver **binary** frames, which it drops today (§5).
- **`onTick`** is where `filter` pumps the GPU and flushes ready results each loop
  iteration (§6) — and must not block.
- **`drawAdmin`** is `filter`'s operator-panel *body* (pipeline controls + per-feed
  stats, §8), hosted inside the shell's chrome.

**What the slot must *not* assume — and chess accidentally taught it to:**

- **No two-seat, turn-based model.** `filter` has no turns and no White/Black. The
  seat/turn logic is chess's, and must live *inside* `ChessGame`, not in the
  shared host. (Today it leaks into `GameServer`; the refactor pushes it down.)
- **No shared authoritative state broadcast to all.** Chess `broadcast`s one
  position to everyone. `filter` answers **each connection privately** with its own
  processed frame. The host's `broadcast` helper stays available but the slot must
  not *require* it.
- **No discrete request/response cadence.** `filter` is a continuous stream with
  backpressure (§5), not one-move-one-state.

---

## 3. The pipeline (the actual morphology)

The engine's "rules" are a **data-described pipeline**: an ordered list of flat
(binary-mask) **structuring-element** operations applied to an image. The operator
edits this list live (§8); the client never decides it (server-authoritative, the
chess §3.2 invariant carried over).

### 3.1 Operators (grayscale, flat structuring element `B`)

For a flat SE `B` (a neighbourhood offset set), on intensity image `f`:

| Operator | Definition | Intuition |
|---|---|---|
| **Erode** `f ⊖ B` | `min_{b∈B} f(x+b)` | shrinks bright regions, grows dark |
| **Dilate** `f ⊕ B` | `max_{b∈B} f(x+b)` | grows bright regions, shrinks dark |
| **Open** `f ∘ B` | `(f ⊖ B) ⊕ B` | removes small bright specks, keeps shapes |
| **Close** `f • B` | `(f ⊕ B) ⊖ B` | fills small dark holes |
| **Gradient** | `(f ⊕ B) − (f ⊖ B)` | edge/outline map — the most demo-friendly |
| **Top-hat** | `f − (f ∘ B)` | bright detail smaller than `B` |
| **Black-hat** | `(f • B) − f` | dark detail smaller than `B` |
| **Threshold** | `f ≥ t ? 255 : 0` | optional binarisation gate for binary morphology |

Composites (open/close/gradient/top-hat/black-hat) decompose into erode/dilate
passes plus a pixel-wise combine, so the GPU backend needs only **two kernels**
(erode, dilate) plus a tiny **combine** kernel (subtract/threshold/invert). The
pipeline is then a flat list of these primitives — e.g. *gradient with a disk-2 SE*
expands to `[dilate(disk,2), erode(disk,2), subtract]`.

### 3.2 Structuring elements

Flat SEs, selectable shape + radius `r` (so the neighbourhood is `(2r+1)²` at most):

- **Box** `(2r+1)×(2r+1)` — **separable**: a box erosion/dilation factors into a
  horizontal then a vertical pass, and each axis is O(1)/pixel via the van
  Herk–Gil-Werman running min/max. The MVP backend does the direct `(2r+1)²`
  neighbourhood loop (trivial on the APU at these resolutions); the separable
  fast path is an optimization noted for §16.
- **Cross / plus** — 4-neighbourhood extended to radius `r`.
- **Disk** — Euclidean `‖b‖ ≤ r`; the visually "roundest" result.

Edges use **clamp-to-edge** (replicate border), so the frame border is well-defined
without wraparound artefacts.

### 3.3 Colour

Morphology on a vector (RGB) pixel has no canonical min/max (no total order on
colours). MVP default: **per-channel independent** morphology — treat R, G, B as
three grayscale images. It is embarrassingly parallel on the GPU, and the slight
channel-fringing on edges reads as a feature, not a bug, for a live demo.
Alternatives recorded in §15: operate on **luma** and recolour from the original
(no fringing), or **grayscale-only** output.

### 3.4 Why this maps perfectly to the APU

Every primitive is a **local neighbourhood reduction** (min or max over `B`) plus
pixel-wise arithmetic — the canonical data-parallel workload. One GPU thread per
output pixel, each reading a small window. At 320×240 the whole pipeline is a
handful of dispatches over 77 k pixels: **sub-millisecond** on Van Gogh. The APU is
not a stunt here; it is the natural executor, and the per-frame budget is dominated
by JPEG codec on the CPU (§5), not the morphology.

---

## 4. Compute backend — EGL-surfaceless + GL compute

### 4.1 A headless GPU context, independent of the admin window

The admin GL context is created inside `runAdminLoop` and only exists when a
display is present ([`admin_ui.cpp:159`](../host/src/admin_ui.cpp),
[`main.cpp:76`](../host/src/main.cpp) gates it on `$DISPLAY`). `filter` must process
frames **even headless over SSH** (the host's "serves players headless" property,
CLAUDE.md). So the backend owns its **own** context, created via **surfaceless
EGL** on the DRM render node:

- Open `/dev/dri/renderD128`, create an EGL display via
  `EGL_MESA_platform_surfaceless` / `EGL_EXT_platform_device`, request an OpenGL
  (or GLES 3.1+) **core context with compute** — no window, no X/Wayland, no
  display server. `radeonsi` exposes GL 4.6 / GLES 3.2; compute shaders need only
  4.3 / 3.1.
- This context is **separate** from the admin SDL2/GL context. On the single
  thread, the backend `eglMakeCurrent`s its context around each dispatch; the admin
  loop `SDL_GL_MakeCurrent`s back for its frame. Two contexts, one thread, no
  sharing — clean, and `filter` works identically whether the admin window is up
  (Game Mode) or not (SSH).
- **Sanity check (steamdeck-debugging §4):** log `GL_RENDERER` at startup and
  assert it is the real `radeonsi` (`AMD Custom GPU 0405 … vangogh`), **not**
  `llvmpipe`. `llvmpipe` means software rasterisation — the APU thesis silently
  failed. A `--engine filter` startup that finds only `llvmpipe` should warn loudly
  and fall back to the CPU reference rather than pretend.

### 4.2 The kernels

Two textures, ping-ponged across the pipeline. Per primitive, one compute dispatch:

```glsl
#version 430
layout(local_size_x = 16, local_size_y = 16) in;
layout(r8ui, binding = 0) uniform readonly  uimage2D src;  // per channel, or rgba8
layout(r8ui, binding = 1) uniform writeonly uimage2D dst;
uniform int  uRadius;
uniform int  uShape;   // 0 box, 1 cross, 2 disk
uniform bool uErode;   // false = dilate
void main() {
  ivec2 p = ivec2(gl_GlobalInvocationID.xy);
  ivec2 sz = imageSize(src);
  if (p.x >= sz.x || p.y >= sz.y) return;
  uint acc = uErode ? 255u : 0u;
  for (int dy = -uRadius; dy <= uRadius; ++dy)
  for (int dx = -uRadius; dx <= uRadius; ++dx) {
    if (uShape == 1 && dx != 0 && dy != 0) continue;          // cross
    if (uShape == 2 && dx*dx + dy*dy > uRadius*uRadius) continue; // disk
    ivec2 q = clamp(p + ivec2(dx, dy), ivec2(0), sz - 1);     // clamp-to-edge
    uint v = imageLoad(src, q).r;
    acc = uErode ? min(acc, v) : max(acc, v);
  }
  imageStore(dst, p, uvec4(acc));
}
```

A second tiny kernel does the pixel-wise **combine** (`a−b` clamped, threshold,
invert) for gradient/top-hat/black-hat. RGB is three single-channel passes, or one
`rgba8` pass with vectorised min/max.

### 4.3 The CPU reference is the contract

`engine/filter/` contains the same pipeline in plain C++ (the §2 pure ruleset). The
GPU output **must equal** the CPU output for integer operators (min/max/subtract on
8-bit are exact on both); a unit test runs a fixed pipeline over a fixed input and
asserts byte-equality (within a 0–1 LSB tolerance only if a fast separable path
ever introduces a different rounding). This makes the GPU backend *verifiable*, not
trusted, and lets CI exercise the engine with no GPU at all (it tests the
reference; the GPU path is compiled-but-skipped where no render node exists — the
same conditional-availability pattern the admin UI already uses for SDL2/GL,
[`host/CMakeLists.txt:29`](../host/CMakeLists.txt)).

---

## 5. Transport & wire contract (binary frames on the player edge)

`filter` keeps **all** network format on the player WS edge (DESIGN §9.5), exactly
like chess — but it needs a **binary** path the current server throws away.

### 5.1 The one transport change

`parseFrames` currently delivers only text frames and explicitly drops binary
("binary frames are not part of the player protocol; ignored",
[`ws_server.cpp:362`](../host/src/ws_server.cpp)). Add a binary callback and a
binary send; nothing else in the WS server changes:

- `Callbacks` gains `onBinary(ConnId, std::string_view)`; `parseFrames` routes
  `kOpBinary` final frames to it (text still goes to `onText`).
- Add `WsServer::sendBinary(ConnId, std::string_view)` — `queueFrame` already
  handles 64-bit lengths and never masks server→client, so this is a one-liner
  wrapper over `queueFrame(c, kOpBinary, …)`.
- The `1 << 20` (1 MiB) frame cap ([`ws_server.cpp:329`](../host/src/ws_server.cpp))
  comfortably holds a downscaled JPEG (320×240 ≈ 15–30 KB). Keep the cap; raise it
  only if higher resolutions are enabled later. Client masking stays mandatory.

This binary path is **generic** — any future media engine reuses it (a structure
that protects deferred features, §11).

### 5.2 Messages

**Control (JSON text), server-authoritative:**

```jsonc
// Server -> client, on connect and whenever the operator edits the pipeline.
{ "type": "filterConfig",
  "outW": 320, "outH": 240,            // the size the client should capture/encode to
  "format": "jpeg", "quality": 70,
  "color": "perChannel",               // perChannel | luma | gray
  "pipeline": [                        // the ordered operator chain (§3)
    { "op": "dilate", "se": "disk", "r": 2 },
    { "op": "erode",  "se": "disk", "r": 2 },
    { "op": "subtract" }               // => morphological gradient
  ] }

// Client -> server, optional: ask for a named preset; server stays the authority.
{ "type": "requestPreset", "preset": "gradient" }
```

**Media (binary frames):** an 8-byte little-endian header then the JPEG bytes, both
directions:

```
[uint32 seq][uint16 width][uint16 height][ JPEG payload … ]
```

- **Client → server:** the captured, downscaled camera frame. `seq` is the
  client's monotonically increasing frame counter.
- **Server → client:** the processed result, echoing the source `seq` so the client
  can drop out-of-order/stale results and measure round-trip.

### 5.3 Backpressure (bounds the loop's work)

A WiFi camera feed can outrun decode+process+encode. The rule, both ends:
**keep only the latest.**

- Server: per connection, retain at most **one** undecoded inbound frame; if a newer
  one arrives before the older is processed, **drop the older**. At most **one
  in-flight** GPU job per connection. This caps the single-thread loop's per-iteration
  work regardless of how hard a client pushes.
- Client: render only the newest result; discard any with a `seq` older than one
  already shown.

Frames are **lossy by design** — dropping a frame is correct, not an error. This is
what keeps a 60 Hz cooperative loop (§6) from ever falling behind.

---

## 6. Concurrency — `filter` inside the one loop (DESIGN §5)

The host stays single-thread, single-loop, no locks. `filter` slots into the
existing shape ([`main.cpp:90`](../host/src/main.cpp) headless;
[`admin_ui.cpp:195`](../host/src/admin_ui.cpp) windowed) as `engine.tick()`:

```cpp
while (running) {
    ws.poll(/*timeout*/);     // drains inbound; onBinary stashes latest frame/conn
    engine.tick();            // for each conn with a pending frame:
                              //   decode JPEG (CPU) -> upload -> dispatch pipeline
                              //   -> readback -> encode JPEG (CPU) -> sendBinary
    // (windowed only) beginImGuiFrame(); engine.drawAdmin(); render();
}
```

**Does the GPU "block"?** Not meaningfully at 320×240: dispatch + `glReadPixels`
with a fence is sub-millisecond. The real per-frame cost is **JPEG decode + encode
on the CPU** (a few ms each with stb). At, say, 2 feeds × 15 fps that is ~60–120 ms
of codec work per second on one thread — comfortable, and bounded hard by the
drop-to-latest rule (§5.3). If profiling ever shows the codec starving the loop,
the DESIGN §5 escape hatch applies precisely as written: introduce **exactly one**
guarded queue (a codec/GPU worker that posts finished frames back to the loop) — and
nowhere else. We design for the synchronous path and leave that door visible, not
open.

**Two GL contexts, one thread.** `tick()` makes the EGL compute context current
around its dispatches; the windowed admin loop makes the SDL2 context current
around its frame. No sharing, no concurrency — the same "no shared mutable state"
guarantee §5 buys for chess.

---

## 7. Sessions, "seats", and authority (mapped honestly)

`filter` has no turns and no opposed seats, so forcing chess's two-seat model would
be a lie. The honest mapping:

- A **connection is a self-contained feed** — it is both the source (camera) and
  the sink (processed result). There is no seat to own, no opponent, no turn order.
  Each feed is independent; ten browsers are ten private pipelines sharing one
  operator-set config.
- A **session is the operator's configuration scope**: the pipeline (§3) the
  operator has dialed in. The MVP runs a single `default` session whose config
  applies to every feed — the operator "running the cabinet." (Per-feed config
  overrides are a clean later extension, §16.)
- **Authority is unchanged in spirit** (chess §3.2): the **server owns the
  pipeline**. A client may *request* a preset; the server decides. There is no
  client-trusted state, because there is no client-authored state to trust — the
  client only contributes pixels, which it inherently owns (its own camera).

This is the clean validation of §2's claim that the slot must not assume
seats/turns: `ChessGame` keeps the seat machinery; `FilterGame` simply doesn't
implement it, and the host doesn't miss it.

---

## 8. The in-process admin UI (operator tunes the pipeline live)

When the `filter` engine is loaded, `engine.drawAdmin()` replaces the chess session
table with a **pipeline console** — ImGui's home turf, and gamepad-navigable on the
Deck (DESIGN §8, the `NavEnableGamepad` path already wired in
[`admin_ui.cpp:183`](../host/src/admin_ui.cpp)):

- **Pipeline editor:** an ordered list of operators; add/remove/reorder; per-stage
  combos for op (erode/dilate/open/close/gradient/top-hat/black-hat/threshold), SE
  shape (box/cross/disk), and a radius slider; a threshold slider where relevant; an
  invert toggle. Editing pushes a fresh `filterConfig` (§5.2) to every feed.
- **Presets:** one-click "Gradient", "Open-3 disk", "Edge + threshold", etc.
- **Live stats per feed:** fps in/out, last frame KB, decode ms, GPU ms, encode ms,
  round-trip; plus the global `GL_RENDERER` string so the operator can *see* it is
  the real APU and not `llvmpipe` (§4.1).
- **Optional thumbnail:** the most-recent processed frame for one feed, uploaded as
  a texture, so the operator on the Deck sees what players see. (Cheap; behind a
  toggle to avoid an always-on upload.)

The admin remains *in-process* and privileged-by-construction (DESIGN §9.4): no
admin socket, it reads/mutates the `FilterGame` directly on the same thread.

---

## 9. The browser client

A new client view (served from the same static root,
[`ws_server.cpp:259`](../host/src/ws_server.cpp); client-only changes stay
rebuild-free per CLAUDE.md). It reuses the transport idiom of
[`net.js`](../client/src/net.js) with a binary path added:

1. **Capture:** `getUserMedia({ video: true })` → a hidden `<video>`. Each tick,
   draw the video to an offscreen `<canvas>` sized to the server's `outW×outH`
   (downscale happens here, for free).
2. **Encode + send:** `canvas.convertToBlob({ type: "image/jpeg", quality })` →
   prepend the 8-byte header (§5.2) → `ws.send(arrayBuffer)`. Pace to the config'd
   rate; never queue more than one unsent frame (client-side drop-to-latest).
3. **Receive + paint:** `ws.binaryType = "arraybuffer"`; on a binary message, strip
   the header, `createImageBitmap(blob)` → draw to the visible canvas. On a text
   message, it's `filterConfig` — update capture size / rate / quality.
4. **"Only the processed result."** The visible canvas shows **only** the server's
   output. The raw local camera is *not* rendered (or, at most, a tiny opt-in
   self-preview thumbnail, **off by default** so the demo lands: you see yourself
   only after the APU has transformed you).

No build step, ES modules loaded directly (CLAUDE.md). The transport change is
small enough to either extend `net.js` (branch on `typeof e.data`) or add a sibling
`media.js`; the doc leaves that to the implementer.

---

## 10. Privacy & the secure-context constraint (read before deploying)

`filter` moves a webcam over the wire — the most sensitive payload Villen has
carried — so two things are load-bearing, not optional:

### 10.1 Privacy rules (enforced by design, not policy)

- **Per-connection, never broadcast.** A processed frame is sent only to the
  connection that produced it (`sendBinary(id, …)`, never `broadcast`). The chess
  habit of broadcasting authoritative state is *wrong* here and the slot must not
  force it (§2).
- **Never stored.** Frames live in memory for the duration of one `tick()` and are
  overwritten by the next. No disk, no log of pixels. (Stats are numbers, not
  images.)
- **Never leaves the LAN.** Same no-cloud ethos as the rest of Villen: the APU does
  the work; nothing is uploaded anywhere. (This is a privacy *advantage* over any
  cloud vision API and worth stating to players.)

### 10.2 `getUserMedia` needs a secure context — and the client is plain HTTP

Browsers gate camera access (and `createImageBitmap` of camera data) behind a
**secure context**: HTTPS, or `localhost`. The Villen client is served over plain
HTTP on a LAN IP ([`net.js:11`](../client/src/net.js)), and **HTTPS is a DESIGN §4
non-goal.** So out of the box, `getUserMedia` on `http://<deck-ip>:<port>` is
**blocked** — a hard browser policy, not a Villen bug. `filter` reopens HTTPS as a
real requirement. Options, cheapest first:

1. **`localhost` only (dev):** works untouched for same-machine testing on the PC.
2. **Chrome insecure-origin allowlist (spike/dev on real devices):**
   `chrome://flags/#unsafely-treat-insecure-origin-as-secure` →
   add `http://<deck-ip>:<port>`. Per-browser, manual; fine for the demo, not for
   walk-up players.
3. **Self-signed TLS / `wss` (the real fix):** terminate TLS in the host (or a tiny
   front) so the client is `https://`; `net.js` already prefers `wss` under
   `https:`. This is the proper path and the first genuine reason to lift the §4
   HTTPS deferral — scoped to this engine, recorded as a decision, not a surprise.

The MVP slice can proceed on (1)+(2); (3) is the first item in this engine's own
"open questions" (§16). The doc flags it **up front** because discovering it at
deploy time on the Deck would look like a mystery failure, exactly the kind
steamdeck-debugging.md exists to pre-empt.

---

## 11. Structures that protect deferred features

Cheap now, painful to retrofit (mirrors DESIGN §9):

1. **The pure CPU reference stays the contract.** Morphology is defined in
   `engine/filter/`, testable with no GPU; the APU backend is an accelerator
   validated against it. Keeps §9.1 true and CI GPU-free.
2. **Pipeline is data, not code.** Operators are a serialisable list; new operators
   (skeletonise, hit-or-miss, reconstruction) drop in as data + one kernel.
3. **The binary transport path is generic.** `onBinary`/`sendBinary` aren't
   filter-specific; any future media engine (audio, depth, a second camera)
   reuses them.
4. **Per-connection routing is the hinge** for later multi-feed features —
   compositing several cameras, an operator "wall" of all feeds, or per-feed
   pipelines — none of which need a transport change.
5. **The `IGame` seam keeps device code out of the host spine.** GL/EGL lives
   inside `FilterGame`; `main`, `ws_server`, and the loop stay engine-agnostic,
   so a third engine doesn't touch them.

---

## 12. Build & dependencies

- **New runtime libs (present on the Deck, steamdeck-debugging §1):** `libEGL`,
  `libgbm`, GLVND. The surfaceless EGL context needs no X/Wayland.
- **New vendored dep:** `third_party/stb/` — `stb_image.h` + `stb_image_write.h`
  (single-header, public-domain), matching the nlohmann single-header vendoring
  convention (DESIGN §8). JPEG decode/encode, no system dependency.
- **CMake shape (mirrors the admin-UI guard,
  [`host/CMakeLists.txt:29`](../host/CMakeLists.txt)):**
  - `villen_filter` — the **pure CPU reference + tests**, always built, always
    CI-tested, like `villen_engine`. No GPU, no GL.
  - The **GPU backend** compiles into the host, guarded by `find_package` for EGL;
    absent EGL → the host still builds and the engine runs on the CPU reference
    (slower, but correct), the same "degrade, don't fail" stance the admin UI takes
    when SDL2/GL is missing.
- **The glibc trap still applies** (steamdeck-debugging §2): cross-build on PC,
  `-static-libstdc++ -static-libgcc`, `.symver`-pin anything newer than the Deck's
  glibc, verify with `objdump -T`. The new code paths don't change that drill; the
  stb/EGL symbols are checked the same way.
- **CI** (engine-only, headless, [`CMakeLists.txt`](../CMakeLists.txt)) gains the
  `villen_filter` reference tests and stays GPU-free.

---

## 13. Build order (smallest-spine-first, mirrors DESIGN §11)

1. **CPU morphology reference + unit tests.** `engine/filter/`: pipeline over a
   pixel buffer; doctest cases for each operator on tiny fixtures (a single bright
   pixel erodes away; gradient of a step is an edge). No host, no GPU. *Proves the
   math.*
2. **Generalize the slot.** Extract `IGame`; refactor `GameServer` →
   `ChessGame` with zero behaviour change (chess tests still pass). Add
   `onBinary`/`sendBinary` to the WS server with a unit/echo test. *Pure
   plumbing; shippable alone.*
3. **Media loop, no processing.** `FilterGame` that decodes the inbound JPEG and
   echoes it straight back (identity pipeline). Browser client captures, sends,
   paints. *Proves camera-in → host → camera-out end to end, the riskiest
   transport path, before any GPU.*
4. **CPU reference in the loop.** Run the actual pipeline on the decoded frame via
   `engine/filter/`. Live morphology — on the CPU — visible in the browser. *Proves
   the engine without GPU risk.*
5. **GPU backend (EGL + GL compute).** Swap the executor behind `FilterGame`;
   assert GPU output matches the CPU reference (§4.3). *The APU milestone.*
6. **Admin pipeline console.** `drawAdmin()` editor + presets + per-feed stats
   (§8). Operator tunes it live.
7. **Deck APU spike (throwaway, the §11.1-style risk burn-down).** *On the actual
   Deck:* a minimal surfaceless-EGL program over **SSH** that creates the context,
   prints `GL_RENDERER`, and asserts real `radeonsi` (not `llvmpipe`); runs one
   erode dispatch and times it. *In the same sitting,* confirm a phone with the
   secure-origin flag (§10.2) can grant camera and see a processed frame from the
   Deck. **Purpose: retire the two highest-uncertainty risks — headless APU compute
   and the camera secure-context — while the code is disposable.**

Steps 1–4 prove the engine with **zero GPU and zero Deck risk** (all on the PC);
step 5 adds the APU; step 7 retires the Deck-specific unknowns early, exactly the
DESIGN §11.1 discipline.

---

## 14. Acceptance criteria (definition of done)

1. `engine/filter/` reference builds and its unit tests pass in headless CI (no
   GPU).
2. The host runs `--engine chess` with **identical** behaviour to today (the slot
   refactor is invisible).
3. With `--engine filter`, a browser on the LAN (secure context per §10.2) grants
   camera, and its canvas shows the **processed** frame; the raw camera is never
   shown (default) and never sent to any other connection.
4. The operator changes the pipeline in the admin UI (e.g. erode → gradient) and the
   browser's output reflects it within one frame, with no client reload.
5. On the Deck, the backend reports the **real `radeonsi`** renderer (not
   `llvmpipe`) and the GPU output matches the CPU reference on a fixed test frame.
6. Two simultaneous feeds each see only their **own** processed camera; neither
   receives the other's pixels.
7. Under a fast push, frames are dropped (drop-to-latest) and the loop never falls
   behind or stalls the admin UI.

---

## 15. Rejected alternatives (why the design is what it is)

- **Process the camera client-side in WebGL.** Defeats the entire thesis — the
  point is *the server's APU* does the work. Recorded so it reads as a decision: the
  host is the compute appliance; the browser is a dumb capture/display terminal.
- **WebRTC / `MediaRecorder` / a real video codec instead of per-frame JPEG.** Lower
  bandwidth and smoother, but drags in SDP/ICE or a server-side video decoder and a
  streaming state machine — large surface for a LAN demo. Per-frame JPEG over the
  existing WS is dependency-light (one stb header), trivially backpressured
  (drop-to-latest), and *good enough* at LAN distances. Revisit only if frame rate
  demands it (§16).
- **Raw 8-bit grayscale frames (no codec).** The lighter alternative we did **not**
  pick: a downscaled luma buffer fits the 1 MiB cap with no stb dependency, but
  loses colour and detail. We chose **JPEG colour** for visual impact; raw-luma
  stays the documented fallback if codec CPU cost ever bites a weaker host.
- **Reuse the admin SDL2/GL context for compute.** Couples `filter` to the admin
  window — then it can't process headless over SSH. An independent surfaceless EGL
  context (§4.1) keeps the engine display-agnostic, which the host requires.
- **Vulkan / OpenCL for the compute.** Vulkan is more explicit and future-proof but
  a large boilerplate lift for one author; OpenCL (rusticl) availability on stock
  read-only SteamOS is less certain. GL compute reuses the project's existing
  GL/GLSL idiom and is known-good on `radeonsi`. (Operator decision; recorded.)
- **Broadcast processed frames like chess broadcasts state.** Wrong by privacy and
  by semantics — each feed is private. The slot keeps `broadcast` available for
  engines that want it and never *requires* it.
- **A second binary / separate process for the media engine.** Breaks the
  single-binary thesis (DESIGN §2) and re-introduces IPC. `filter` is just another
  `IGame` in the same process and loop.

---

## 16. Open questions to revisit *after* the MVP (do not block)

- **TLS / `wss` for walk-up camera access (§10.2).** The first real reason to lift
  the DESIGN §4 HTTPS deferral. Self-signed in-host vs. a tiny reverse proxy;
  certificate UX on phones. The MVP rides the dev-flag; this makes it walk-up.
- **Async GPU/codec via the one §5-sanctioned queue,** if profiling shows JPEG codec
  starving the loop at higher feed counts/resolution. Designed-for, not built.
- **Separable / van Herk box morphology** (§3.2) and bigger SEs at higher resolution
  once the spine holds.
- **Per-feed pipelines** (each connection its own config) and an operator **wall**
  compositing all feeds — both enabled by per-connection routing (§11), no transport
  change.
- **More operators as data:** hit-or-miss, skeleton, morphological reconstruction,
  distance transform — each a kernel + a pipeline entry.
- **Binary morphology mode** (threshold-gate front, AND/OR neighbourhood) surfaced
  as a one-switch preset for the starkest, most legible output.
- **A non-camera source** (screen share via `getDisplayMedia`, or a still image)
  through the identical pipeline — the transport and engine don't care where pixels
  come from.
