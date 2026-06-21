# Villen — the `chat` engine: local LLM chat via llama.cpp (design & handoff)

> **`villen-chat`** is the third engine to occupy Villen's engine slot. A browser on
> the LAN opens a chat view, types, and the host streams a reply **token by token**
> from a quantized 7–8B model running **locally on the Steam Deck** under
> `llama.cpp` — no cloud, no accounts, nothing leaves the LAN. Names derive
> mechanically: namespace `villen::chat`, lib `villen_chat`, this doc
> `DESIGN-chat.md`. Target models: **Llama 3.1 8B Instruct, Qwen2.5 7B Instruct,
> Mistral 7B Instruct**, as GGUF, one resident at a time, operator-selectable.

**Status:** design / additive. Intended as a starting brief for the agent who will
build it. The **integration model is decided — 3.A, subprocess `llama-server`**
(§3); remaining decisions are recommended-with-rationale and recorded as forks.
**Scope:** the slice that proves a *long-running, blocking, streaming* engine fits
Villen's single-thread spine — prompt in, tokens out, operator-tuned model + params.
**Audience:** the engineer/agent standing this up, having read
[`DESIGN-villen.md`](DESIGN-villen.md), [`DESIGN-filter.md`](DESIGN-filter.md), and
[`DESIGN-spectator-and-agent-api.md`](DESIGN-spectator-and-agent-api.md). The slot
abstraction and per-connection streaming come from the `filter` doc; the "local LLM
on the LAN" idea comes from the spectator doc §5.

> **Why this engine earns its keep.** Chess proved the *deterministic, turn-based*
> axis. `filter` proved the *real-time, per-connection, GPU* axis. `chat` proves the
> one axis both ducked: **work that genuinely blocks for seconds.** DESIGN §5 wrote
> the spine single-threaded and reserved exactly one escape hatch — "if a future
> feature genuinely blocks, introduce exactly one guarded queue at that point" — and
> [`DESIGN-spectator-and-agent-api.md`](DESIGN-spectator-and-agent-api.md) §5 named
> the case: "a local LLM (e.g. llama.cpp on the LAN)." `chat` is that case made
> real, and the whole design turns on honouring §5 rather than breaking it.

---

## 1. Context & one-paragraph pitch

A browser opens the `chat` client, sees which model is loaded, and sends a message.
The host — the same single C++ binary that runs chess, `filter`, and the in-process
admin UI — hands the conversation to a **local `llama.cpp` inference process** on
the Deck (Van Gogh APU, Vulkan-offloaded), and streams the generated tokens back to
**that one connection** as they are produced. The browser appends each delta, so the
reply types itself out. The operator, on the Deck, picks the active model (Llama 3.1
8B / Qwen2.5 7B / Mistral 7B), sets sampling params and a system prompt, and watches
throughput and queue depth in the ImGui admin panel.

Nothing is uploaded anywhere: the APU does the generation, the prompts and replies
stay in RAM on the LAN, and there are no logs by default — the strongest privacy
story Villen has (a local model is *categorically* more private than any cloud chat
API, and worth saying to users). The same inference subsystem is the backend the
spectator doc's "server-side LLM **mover**" wants for driving a chess seat (§12), so
`chat` and "play an AI opponent" share one engine underneath.

---

## 2. Where it sits in the slot (and what's actually "pure")

`chat` is a `ChatEngine : villen::IEngine` (the canonical contract,
[`DESIGN-game-framework.md`](DESIGN-game-framework.md) §4 — `onJoin`/`onLeave`/
`onMessage`/`onTick`/`drawAdmin`, via a `Room` handle), one engine among several the
launcher runs one at a time ([`DESIGN-admin-shell.md`](DESIGN-admin-shell.md)). Chat is
text/JSON, so it needs only the **text** WS path — not even the binary path `filter`
added.

The two-seam honesty (`filter` §2) bends further here, and the doc says so plainly:

| Axis | chess | filter | **chat** |
|---|---|---|---|
| Pure ruleset (`engine/`, CI-tested) | `chess::Position` | `filter::process` CPU reference | **conversation state + per-model prompt templating** |
| Determinism | total | total (GPU == CPU ref) | **none** (sampling is stochastic) |
| Heavy executor | none | APU (sub-ms) | **llama.cpp (seconds, blocking, streaming)** |
| Cadence | one move → one state | one frame → one result | **one prompt → a stream of tokens** |
| Routing | broadcast | per-connection | **per-connection** |

There is **no deterministic oracle for generation** — unlike `filter`, you cannot
unit-test the model's output against a reference. So the pure, testable core is
deliberately *thin and upstream of the model*: the **conversation state machine**
(append turns, reset, cap context) and the **prompt assembly** (render a
conversation to the exact string/messages a given model expects). Those are
deterministic and get the same CI treatment as chess. Everything stochastic lives
behind the inference boundary (§3) and is integration-tested, not unit-tested.

---

## 3. The blocking-inference problem (the crux — §5's escape hatch, realized)

Token generation takes **seconds** and arrives **incrementally**. It cannot run
inside the cooperative ~60 Hz loop: a synchronous `generate()` would freeze the
admin UI, chess, and every other chat. Two sanctioned realizations of DESIGN §5,
both already idiomatic in Villen:

### 3.A Subprocess `llama-server` + non-blocking streaming socket in `poll()`  *(decided)*

Run `llama.cpp`'s built-in **`llama-server`** (OpenAI-compatible) as a managed
child process. The host opens a **non-blocking** local TCP connection to it, POSTs
`/v1/chat/completions` with `"stream": true`, and reads the **SSE** token deltas
incrementally — the response socket fd simply **joins the existing `poll()` set**,
exactly as [`DESIGN-spectator-and-agent-api.md`](DESIGN-spectator-and-agent-api.md)
§5 does for Stockfish ("the subprocess's stdout is just another fd in the existing
`poll()` set"). The host parses each `data: {…}` chunk, pulls
`choices[0].delta.content`, and forwards it to the right WS connection as a
`chatDelta` (§7).

- **Zero new threads in the host.** It stays single-threaded; the "blocking" lives
  entirely in another process; the host only ever does non-blocking byte-shuffling.
  This is the *cleanest* §5 fit — no guarded queue needed at all, because there is
  no second thread to guard against.
- **Crash isolation.** A model OOM or a `llama.cpp` bug kills `llama-server`, **not**
  the appliance. The host sees EOF on the fd, reports `chatError`, and restarts the
  child. Chess, `filter`, and the admin UI are unaffected.
- **Less code to own.** `llama-server` brings mature streaming, continuous batching,
  multi-slot concurrency, and chat-template application for free. The host's job
  shrinks to **orchestrate + proxy**, not "embed an inference engine."
- **Easier to test, and the boundary is just text.** The host↔`llama-server` channel
  is plain JSON-over-HTTP with an SSE token stream over loopback — trivially
  inspectable and **mockable**: the HTTP/SSE client and the whole streaming spine can
  be tested against a tiny stub server (or a recorded SSE transcript) with **no model
  and no GPU**, extending the "CI stays LLM-free" story (§13). Embedding (3.B) gives
  no such seam — you'd be mocking `libllama` itself.
- **Cost:** a second long-running process (bends "single binary" to *single
  foreground binary + one managed inference child*), a small hand-rolled
  non-blocking HTTP/1.1 + SSE client (~150 lines, reusing the socket idiom already
  in [`ws_server.cpp`](../host/src/ws_server.cpp)), and shipping the `llama-server`
  binary alongside the host.

### 3.B Embedded `libllama` + one worker thread + one guarded queue  *(recorded alternative — not chosen)*

Link `libllama` (+`ggml` + the Vulkan backend) into the host; run the
`llama_decode` token loop on **one** worker thread that posts each token to **one**
mutex-guarded queue; the main loop drains the queue each iteration and forwards.
Exactly one thread, exactly one queue — the literal §5 escape hatch.

- **Keeps one binary** (the thesis, DESIGN §2), in-process control.
- **Cost:** the host now has a thread and shared state (the §5 thing we avoided
  everywhere else); **no crash isolation** (an inference crash takes the whole
  appliance); and a markedly heavier cross-build (the glibc trap of
  steamdeck-debugging §2 now also covers `ggml`/Vulkan in *your* binary).

**Decision: 3.A.** It keeps the host single-threaded (the property §5 fought to
preserve), isolates the riskiest, memory-hungriest code in a process that can be
restarted, lets the implementing agent write a proxy instead of an inference engine,
and — decisively — makes the inference boundary a **plain-text, mockable seam** that
the test suite can drive with no model or GPU. 3.B remains fully specified above so
the fork is a cheap flip if single-binary purity is ever judged to outweigh
isolation and testability.

### 3.C Talking to the spawned server directly (a 3.A bonus, for debugging)

Because 3.A runs a *real* OpenAI-compatible `llama-server` (not an embedded
library), the resident model is reachable directly — independent of Villen's wire
protocol — which is handy for debugging, scripting, or a quick CLI chat:

- **Raw API / web UI.** The server binds loopback (`127.0.0.1:<llama-port>`, the
  `--llama-url` port). `curl .../v1/chat/completions` speaks the OpenAI schema, and
  `llama-server` serves its own browser UI at `/`. It is loopback-only **by
  design** — Villen is the sole LAN edge (§6/§9.5), so the inference port is never
  bound to `0.0.0.0`; reach it off-box with an SSH tunnel, not by exposing it.
- **`llama-cli`.** The same llama.cpp build ships a CLI; pointed at the GGUF with
  `-ngl N` it is an interactive REPL. It loads its **own** copy of the model,
  though — a second resident model the one-model-at-a-time budget (§5) doesn't
  account for, so stop Villen's instance (or expect the memory pressure) first.

This falls out of the architecture rather than being a feature to build: it's a
direct consequence of choosing a subprocess over an embedded library (3.A vs 3.B).

---

## 4. Models & prompt templating (the pure, testable core)

The three target models use **three different** chat formats and stop tokens:

| Model | Format | Turn delimiters | Stop |
|---|---|---|---|
| **Llama 3.1 8B Instruct** | Llama-3 headers | `<|start_header_id|>{role}<|end_header_id|>\n\n … <|eot_id|>` | `<|eot_id|>` |
| **Qwen2.5 7B Instruct** | ChatML | `<|im_start|>{role}\n … <|im_end|>` | `<|im_end|>` |
| **Mistral 7B Instruct** | Mistral `[INST]` | `<s>[INST] {user} [/INST]` (system folded into the first user turn on classic variants) | `</s>` |

Re-implementing and maintaining three template strings invites drift. So:

- **Primary:** let inference apply the template. With **3.A**, the host sends
  *structured* `messages: [{role, content}, …]` to `/v1/chat/completions` and
  `llama-server` renders the model's template from the GGUF's
  `tokenizer.chat_template` metadata and reports the correct stop tokens. The host
  never touches raw special-token strings — even cleaner than `filter`'s pixel
  buffers.
- **Fallback / override:** keep a tiny in-host template table (the three rows above)
  for GGUFs whose embedded template is missing or wrong, selectable per model. This
  table — pure string assembly — is the **unit-tested** core (§2): "given this
  conversation and this model id, produce exactly this prompt."

The host owns **conversation state**: per connection, a list of `{role, content}`
turns, plus reset, plus a **context cap** (§5). The system prompt is operator-set
(§9), prepended as the `system` turn.

---

## 5. Memory & the one-model-at-a-time rule (a hard constraint)

The Deck has **16 GB unified** memory shared between CPU, the APU, SteamOS, and
Gamescope. Budget, roughly:

- A 7–8B model at **Q4_K_M** ≈ **4.4–4.9 GB** of weights.
- **KV cache** grows with context length (~order 1 GB at a few-k tokens, fp16;
  far more at full 128k — so **cap context** to, say, 4k–8k for memory, not because
  the model can't do more).
- SteamOS + Gamescope + the Villen binary ≈ **2–3 GB**.

⇒ **One model resident at a time.** Three at once (~15 GB of weights alone) does not
fit. The operator selects the active model in the admin UI; switching unloads the
current model and loads the next (**a few seconds**). With **3.A** the simplest
mechanism is **one model per `llama-server` invocation; switch = restart the child**
with a new `-m`/`-hf` (and a tuned `-ngl`, §6). Concurrent *conversations* share the
one resident model via `llama-server` slots (§8); concurrent *models* are out.

---

## 6. APU vs CPU inference (and contention with the other engines)

- **Vulkan offload (recommended).** `llama.cpp`'s **Vulkan** backend runs on the
  Van Gogh RDNA2 iGPU via RADV — the same `radeonsi`/Mesa stack steamdeck-debugging
  §4 confirms is real (not `llvmpipe`). Offload layers with `-ngl` tuned to fit the
  §5 memory budget. On unified memory the win is GPU *compute* on prompt-processing
  and decode, not a copy avoidance, but it is still **meaningfully faster** than CPU.
- **CPU fallback.** The 4-core/8-thread Zen2 runs 7B Q4 **usably but slowly**; keep
  it as the fallback when Vulkan is unavailable or contended. **ROCm is not viable on
  Van Gogh — Vulkan is the path.**
- **Ballpark, to confirm in the spike (§14):** order ~10–20 tok/s Vulkan vs ~4–8
  tok/s CPU on 7B Q4. Treat as a hypothesis to measure, not a spec.
- **Contention.** The APU also serves `filter`'s compute and the admin GL window.
  An appliance loads **one engine at a time** (`--engine …`), so `chat` and `filter`
  don't fight over the GPU simultaneously; the only co-tenant is Gamescope + the
  admin UI, which is light. Note this so nobody tries to run heavy `filter` and heavy
  `chat` in one process.

---

## 7. Wire contract (player edge — text/JSON, streaming deltas)

Reuses the **text** WS path (DESIGN §6 / §9.5; no binary needed). Per connection,
private, never broadcast (the `filter` §7 stance). The streaming deltas are chat's
analog of `filter`'s per-frame results.

**Client → server:**
```jsonc
{ "type": "chatSend",  "convId": "c1", "text": "explain morphology in one line" }
{ "type": "chatReset", "convId": "c1" }   // start a fresh conversation
{ "type": "chatStop",  "convId": "c1" }   // cancel the in-flight generation
```

**Server → client:**
```jsonc
// On connect and whenever the operator changes model/params (server-authoritative).
{ "type": "chatConfig", "model": "qwen2.5-7b-instruct",
  "models": ["llama-3.1-8b-instruct","qwen2.5-7b-instruct","mistral-7b-instruct"],
  "contextMax": 8192, "ready": true }

{ "type": "chatDelta", "convId": "c1", "msgId": 7, "delta": "Morphology is" }
{ "type": "chatDone",  "convId": "c1", "msgId": 7,
  "stopReason": "eos", "tokens": 23, "tps": 14.2 }      // eos | length | stopped
{ "type": "chatError", "convId": "c1", "reason": "model_busy" }  // queued | model_busy | backend_down | bad_message
```

Authority (chess §3.2, carried over): the **server owns model + sampling params**;
the client supplies prompts and may *request* nothing it can't already type. Each
`chatSend` is gated by turn-of-generation per `convId` (one in-flight generation per
conversation; a second send while generating is rejected or queued, §8).

---

## 8. Concurrency of multiple chatters (queue, don't drop)

`filter` drops stale frames (lossy is correct for video). Chat is the opposite:
**tokens are never dropped** (a half-answer is wrong), so excess *requests* **queue**.

- With **3.A**, `llama-server --parallel N` exposes **N slots** with continuous
  batching: up to N conversations generate concurrently, interleaved, on the one
  resident model. The host maps each active `convId` to a slot/streaming request.
- Beyond N (bounded by KV memory, §5), further `chatSend`s **queue FIFO**; the host
  replies `chatError: "queued"` (or a `chatQueued` position) until a slot frees.
- The single-thread host just **multiplexes streams** — it reads each child socket
  non-blocking and fans deltas out to the matching WS connection. `llama-server` does
  the batching; the host does the routing.

Pick a small `N` (e.g. 2–4) sized to the memory budget; surface it and the queue
depth in the admin UI (§9).

---

## 9. The in-process admin UI (operator runs the model)

`ChatEngine::drawAdmin()` replaces the chess table with a **model console** —
ImGui's home turf, gamepad-navigable on the Deck (the `NavEnableGamepad` path in
[`admin_ui.cpp:183`](../host/src/admin_ui.cpp)):

- **Model:** combo of the three; **Load/Switch** (shows progress + the
  unload→reload latency); resident model, **memory used**, context window, the
  `llama-server` health (PID, `-ngl`, Vulkan vs CPU, last error, **Restart**).
- **Generation params:** temperature, top-p, top-k, max tokens, repeat penalty, and
  the **system prompt** — all server-authoritative, pushed to clients as `chatConfig`.
- **Live stats:** active conversations, **tok/s**, tokens used, **queue depth**,
  slots in use; a **Stop all** / **Unload** control.
- **Privacy by default:** the admin shows **metadata, not message content** (chat is
  sensitive, §11). A content peek, if ever added, is an explicit opt-in toggle.

In-process and privileged-by-construction (DESIGN §9.4): it reads/mutates the
`ChatEngine` directly on the same thread, no admin socket.

---

## 10. The browser client

A chat view served from the static root
([`ws_server.cpp:259`](../host/src/ws_server.cpp); client-only changes stay
rebuild-free, CLAUDE.md), reusing the [`net.js`](../client/src/net.js) text
transport unchanged:

- A scrolling **message list**, an **input box** (Enter to send), a **Stop** button,
  and **New conversation**. The active **model name** is shown.
- **Streaming render:** on each `chatDelta`, append `delta` to the current assistant
  message; on `chatDone`, finalize (and show tok/s if desired). A blinking caret
  while generating.
- Plain-text rendering first; **markdown/code-block** rendering is a deferred polish
  (§17) — don't block the spine on it.
- No build step, ES modules loaded directly (CLAUDE.md).

---

## 11. Privacy & licensing

- **Per-connection, never broadcast, never persisted, never off the LAN.**
  Conversations live in RAM for their session; no transcript is written to disk by
  default; the APU does the work locally. This is the same ethos as `filter`'s
  camera (§10 there) and the strongest possible privacy posture for an LLM — state
  it to users as a feature.
- **Admin sees metadata, not content** (§9).
- **Weights are operator-supplied, not shipped.** The host ships *no* GGUFs (size +
  licensing); the operator drops model files into a configured directory. Note the
  licenses so the operator complies: **Qwen2.5 — Apache-2.0**, **Mistral 7B —
  Apache-2.0**, **Llama 3.1 — Meta Llama 3.1 Community License** (acceptable-use +
  attribution terms). Qwen2.5's permissive license makes it the natural first model
  to wire (§14).

---

## 12. Structures that protect deferred features

Cheap now, painful to retrofit (mirrors DESIGN §9, `filter` §11):

1. **The inference boundary is a swappable seam, not chat-specific.** Whether 3.A or
   3.B, expose it as "given a conversation + params, stream tokens." That same seam
   is what the spectator doc §5 **server-side LLM mover** needs to drive a chess
   seat (frame the question as FEN + legal moves, parse a move from the completion).
   Keep it engine-neutral so `chat` and "play the AI" share one backend — do **not**
   bake chat's message shapes into it.
2. **The conversation/template core stays pure & CI-tested** (§2), independent of
   which backend or model is live.
3. **Per-connection routing and the streaming-delta pattern** are reused from
   `filter`; a future multi-user/group-chat or tool-calling mode rides them with no
   transport change.
4. **The `IEngine` seam keeps the subprocess/thread plumbing inside `ChatEngine`** —
   `main`, `ws_server`, and the loop stay engine-agnostic, so the inference
   subsystem never leaks into the host spine.

---

## 13. Build & dependencies

- **Approach 3.A:** ship the **`llama-server`** binary (cross-built on PC for the
  Deck, or a compatible upstream release — the **glibc trap of steamdeck-debugging
  §2 applies to it too**: check `objdump -T` ≤ the Deck's glibc, static-link or
  symver-pin) + operator-supplied **GGUF** files (not committed, like the art
  assets). The host gains a **small non-blocking HTTP/1.1 + SSE client** (hand-rolled
  in the [`ws_server.cpp`](../host/src/ws_server.cpp) socket style — **not** libcurl,
  whose blocking calls would stall the loop; non-blocking-fd-in-`poll()` is the
  whole point of 3.A) and a **subprocess manager** (spawn/health/restart).
- **CMake (mirrors the conditional-availability pattern of the admin UI,
  [`host/CMakeLists.txt:29`](../host/CMakeLists.txt)):**
  - `villen_chat` — the **pure conversation + templating core + tests**, always
    built, always CI-tested, **no llama dependency**, like `villen_engine` and
    `villen_filter`.
  - The host integration (subprocess + HTTP/SSE client) compiles into the host;
    absent a model/`llama-server` at runtime, `--engine chat` reports
    `backend_down` cleanly rather than crashing (degrade, don't fail).
- **CI stays LLM-free and GPU-free:** it builds and tests the pure core *and* the
  HTTP/SSE client + streaming spine against a **stub server** (a few canned
  `data:` chunks over loopback) — no model, no GPU (§3.A). The real backend is
  integration-tested only where a model + `llama-server` exist.
- **Approach 3.B** instead links `libllama`+`ggml`+Vulkan into the host — heavier
  cross-build, the glibc/Vulkan trap now in your own binary.

---

## 14. Build order (smallest-spine-first, mirrors DESIGN §11 / `filter` §13)

1. **Pure core + tests.** `engine/chat/`: conversation state (append/reset/cap) and
   the three-model prompt-templating table; doctest cases asserting exact rendered
   prompts and stop tokens per model. No llama, no host. *Proves the deterministic
   part.*
2. **`ChatEngine` + WS messages against a STUB generator.** Wire `chatSend`/
   `chatDelta`/`chatDone` to a stub that echoes "you said: …" token-by-token on a
   timer. Browser client renders the stream. *Proves the streaming spine end to end
   with zero inference and zero GPU.*
3. **Subprocess + non-blocking SSE client.** Spawn `llama-server`, health-check,
   open the streaming connection, parse SSE deltas, fd in `poll()`. Still model-
   agnostic plumbing. *Proves the §3.A transport.*
4. **First real model — Qwen2.5 7B** (Apache-2.0, §11). Real streaming end to end.
5. **Model switching + the other two models;** verify each template/stop-token
   (Llama-3 headers, ChatML, Mistral `[INST]`) renders correctly via the GGUF
   template, fallback table where needed.
6. **Admin console** (§9): model select/switch, params, stats, queue, restart.
7. **Deck APU spike (throwaway, the §11.1-style risk burn-down).** *On the actual
   Deck:* confirm `llama-server` runs with **Vulkan** (real `radeonsi`/RADV, not
   `llvmpipe`), measure **tok/s** Vulkan vs CPU and **memory headroom** alongside
   Gamescope, and confirm a **phone on the LAN** can chat. **Purpose: retire the
   highest-uncertainty risks — APU inference throughput and the unified-memory
   budget — early, on disposable code.**

Steps 1–2 deliver the full streaming UX with **no inference and no Deck risk** (all
on the PC); steps 3–5 add the real backend; step 7 retires the APU/memory unknowns.

**Status (first Deck bring-up).** Steps 1–6 are implemented; 1–5 are verified on the
device. Qwen2.5-7B-Instruct-Q4_K_M streams end to end over **RADV Vulkan** on the
Vangogh APU (real token streaming, multi-turn context, reset, and llama-server
crash-restart all confirmed), and live model switching across all three template
families (Llama-3 headers / ChatML / Mistral `[INST]`) renders cleanly with no
leaked special tokens (step 5). The admin console — model select/switch, params,
stats, queue, restart (step 6) — is built. Step 7's Deck APU/Vulkan throughput
spike harness has landed (`spike/chat-bench/`); recording its tok/s and
memory-headroom numbers on the device (§15.6) is the remaining acceptance item.
Engineering refinements found while building are tracked in §18.

---

## 15. Acceptance criteria (definition of done)

1. `engine/chat/` pure core builds and its template/state tests pass in headless CI
   (no GPU, no model).
2. `--engine chess` and `--engine filter` are unchanged; `--engine chat` is selected
   the same way.
3. A browser on the LAN sends a message and sees the reply **stream in token by
   token** from the resident model; **Stop** cancels mid-generation.
4. The operator switches the model in the admin UI; new conversations use the new
   model, with the correct template and stop tokens (no leaked `<|im_end|>` /
   `<|eot_id|>` / `</s>` in output).
5. Two browsers chat **concurrently** and privately; neither sees the other's
   conversation; beyond the slot count, extra requests **queue** rather than drop.
6. On the Deck, `llama-server` reports **Vulkan/`radeonsi`** (not `llvmpipe`), one
   model fits in memory with Gamescope running, and tok/s is recorded.
7. Killing `llama-server` produces a clean `chatError`/restart — chess, `filter`, and
   the admin UI keep running (crash isolation, §3.A).

---

## 16. Rejected alternatives (why the design is what it is)

- **Cloud LLM API.** Breaks no-cloud/no-egress and ships prompts off the LAN —
  already rejected in [`DESIGN-spectator-and-agent-api.md`](DESIGN-spectator-and-agent-api.md)
  §5. Recorded so it reads as a decision: the APU does the work, locally.
- **Embedded `libllama` + worker thread (3.B)** instead of the subprocess. The
  single-binary-purist path; rejected as *primary* for losing crash isolation,
  inflating the cross-build, and offering no mockable seam to test against (you'd be
  mocking `libllama` itself). Kept as the documented fork (§3.B).
- **Blocking HTTP (libcurl) on the main thread.** Would freeze the loop for the whole
  generation — the exact §5 sin. Non-blocking SSE with the fd in `poll()` is the
  point.
- **All three models resident.** ~15 GB of weights alone won't fit 16 GB unified
  (§5). One resident, operator-switchable.
- **Our own chat templates as the source of truth.** Three hand-maintained formats
  drift from upstream; let the GGUF/`llama-server` apply them, keep our table as a
  tested fallback (§4).
- **Dropping tokens under load** (the `filter` move). Wrong for text — queue requests
  instead (§8).
- **Shipping the weights.** Size + licensing; operator-supplied (§11).
- **Persisting transcripts by default.** Against the privacy posture (§11);
  off unless explicitly enabled.

---

## 17. Open questions to revisit *after* the MVP (do not block)

- **Tool / function calling** — let the model *act*: "start a new chess game",
  "switch to the gradient filter", surfacing the host's own engines as tools. The
  highest-leverage follow-on and a uniquely Villen demo. Its most ambitious (and most
  dangerous) extension — the model patching Villen's own minor bugs — is explored in
  [`DESIGN-self-hotfix.md`](DESIGN-self-hotfix.md); ship it last, behind the others.
- **Unify with the chess LLM mover** (spectator §5): one inference subsystem behind
  both `chat` and an AI seat. Keep the §12 seam neutral so this is additive.
- **Model-switch latency / hot-swap** — keep a model warm vs. restart-to-switch;
  pre-load on operator hover.
- **Fair multi-user queueing & KV-memory limits** at higher concurrency; per-user
  rate limits.
- **Quantization choice** (Q4_K_M vs Q5/Q6/Q8) — the speed/quality/memory triangle on
  the Deck; possibly per-model.
- **Speculative decoding / a small draft model** for throughput, memory permitting.
- **RAG / grounding** against operator-provided docs (still fully local).
- **Markdown/code rendering** and copy-buttons in the client (§10).
- **Context-cap policy** — sliding window vs. summarization when a long conversation
  exceeds the capped context.
- **Secure context** — chat itself needs no camera/mic, so it is **not** blocked by
  the `getUserMedia` secure-context wall `filter` hit (§10 there); plain HTTP on the
  LAN works. (If voice input is ever added, that wall returns.)

---

## 18. Implementation refinements (engineering follow-ups, post-Step-4)

Concrete refinements surfaced while building steps 1–4 — distinct from the §17
*product* questions; these are known engineering debts with a clear shape.

- **Generation socket in the main `poll()` set — DONE.** The llama generation fds
  are folded into `WsServer::poll()` (`IEngine::collectPollFds` → `Host` →
  `LlamaClient::collectFds`), so an inbound token ends the poll block at once
  instead of waiting out the ~100 ms `onTick` timeout. Before this, the SSE drain
  ran only at the tick cadence, capping perceived streaming smoothness; now the
  loop wakes on the token. The fds are watched read-only as wakeup hints — the
  engine still does the actual read in `onTick`/`pump()`. (The admin/Game-Mode loop
  already spun at vsync, so this mainly fixes the headless/SSH path.)
- **Fully-async `/health` probe.** `LlamaProcess::probeHealth` is a bounded but
  *blocking* GET (~60 ms worst case), and only while the model loads (probing stops
  once ready). A non-blocking, across-ticks state machine — or reusing
  `LlamaClient` — would remove even that brief stall. Low priority: startup-only.
- **Python e2e harnesses into CI.** The transport / spawn / crash-restart and
  malformed-payload harnesses (a stub `llama-server` and a fake binary) run locally
  but aren't wired into CI, which is C++-only (doctest). They'd catch wire/protocol
  regressions the unit tests can't; needs a CI job that builds the host and runs
  them headless.
