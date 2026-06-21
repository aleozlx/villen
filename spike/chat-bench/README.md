# Chat engine Step 7 — Deck APU/Vulkan throughput spike

A pure-stdlib Python benchmark that **quantifies** local LLM inference on the Steam
Deck's Van Gogh APU, for [`DESIGN-chat.md`](../../docs/DESIGN-chat.md) §14 **step 7**
(acceptance criterion #6: *"on the Deck, `llama-server` reports Vulkan/`radeonsi`,
one model fits in memory with Gamescope running, and tok/s is recorded"*).

**Step 4 proved the path** — Qwen2.5-7B streams via RADV Vulkan on the APU. **Step 7
measures it:**

1. **tok/s (decode) & TTFT (time-to-first-token)** per model — the numbers a user feels.
2. **Vulkan vs CPU** — an `-ngl` sweep (`99` = offload all layers, `0` = pure CPU).
3. **Unified-memory headroom** — `MemAvailable` while a model is resident, the §5
   "does one model fit alongside SteamOS + Gamescope?" budget.
4. **`--parallel` multi-client concurrency** — continuous batching across N slots (§8):
   per-stream vs aggregate system throughput as slots increase.

It is **throwaway** in spirit (a §11.1-style risk burn-down), but kept like
[`../deck/`](../deck/) is — as the **recorded throughput baseline** and the seed for
the admin console's **Live stats** panel (tok/s, queue depth — §9).

> **Status:** harness complete and **verified LLM-free on a PC** (spawn + attach
> paths, `vulkan`/`cpu`/`llvmpipe` classification, concurrency, memory sampling, the
> report). The **on-Deck numbers are recorded separately** — fill the results table
> below by running it during a quiet window (the Deck is a shared device; don't
> benchmark while other sessions are loading models on it, or the throughput/memory
> numbers are meaningless). Step 4 already established the headline single-stream
> figure: **Qwen2.5-7B Q4_K_M, RADV Vulkan, ~9–11 tok/s, TTFT ~1.1 s.**

## Why benchmark `llama-server` directly

The host is a thin **non-blocking proxy** (`LlamaClient` pumps bytes; `LlamaProcess`
spawns the child — `host/src/llama_process.cpp`) and adds negligible overhead. The
thing under test is the **APU**, so this driver spawns `llama-server` with the host's
*exact* argv (`-ngl 99 --parallel N`, the OpenAI `/v1/chat/completions` streaming
path) and measures the server. The numbers are faithful to what `--engine chat` does.

Decode throughput and TTFT come from llama-server's own `timings` block
(`predicted_per_second`, `prompt_per_second`) — server-authoritative — cross-checked
against client-side wall-clock. `ignore_eos` + `cache_prompt:false` make every
request do a real prompt eval and emit exactly `--max-tokens` tokens, so cells are
comparable.

## Files

- `bench.py` — the driver (pure stdlib: urllib / threading / subprocess / json).
- `fake_llama_server.py` — an LLM-free, GPU-free stub that mimics llama-server's
  `/health` + streaming `/v1/chat/completions` (with `timings`). Lets you verify the
  whole harness on a PC before it touches the Deck — the project's "CI stays
  LLM-free" discipline (§13). It is **not** a model and reports nothing real.
- `bench-logs/`, `results-*.json`, `results-*.md` — run output (git-ignored).

## Verify on a PC first (no model, no GPU)

```bash
./bench.py --llama-bin ./fake_llama_server.py --model /dev/null \
           --ngl 99,0 --parallel 1,2 --max-tokens 16 --reps 2
```

A healthy self-test classifies `backend: vulkan` for `-ngl 99` and `backend: cpu`
for `-ngl 0`, fires 2 concurrent streams for `--parallel 2`, samples memory, and
writes `results-*.{json,md}`. (The stub's tok/s are meaningless — only the harness
mechanics are under test here.)

## Run on the Deck

Prereqs on the Deck (see [`../../docs/steamdeck-debugging.md`](../../docs/steamdeck-debugging.md)
and the deploy notes): a **Vulkan-enabled `llama-server`** and the **GGUF** files,
e.g. under `/run/media/deck/SD256/LLM/` (operator-supplied, §11 — not shipped). The
Deck has `python3`; the script needs nothing else.

Copy this folder over (`scp -r spike/chat-bench deck@deck:~/`) and run. **One model,
Vulkan vs CPU, 1 vs 2 slots** — the core matrix:

```bash
./bench.py \
  --llama-bin /run/media/deck/SD256/LLM/llama-server \
  --model    /run/media/deck/SD256/LLM/qwen2.5-7b-instruct-q4_k_m.gguf \
  --ld-library-path /run/media/deck/SD256/LLM \
  --ngl 99,0 --parallel 1,2 --max-tokens 128 --reps 3
```

**All three models** (tok/s across models, Vulkan only — pass each `--model`; one is
resident at a time, §5, and the driver loads/unloads per cell):

```bash
./bench.py \
  --llama-bin /run/media/deck/SD256/LLM/llama-server \
  --ld-library-path /run/media/deck/SD256/LLM \
  --model /run/media/deck/SD256/LLM/qwen2.5-7b-instruct-q4_k_m.gguf \
  --model /run/media/deck/SD256/LLM/Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf \
  --model /run/media/deck/SD256/LLM/mistral-7b-instruct-v0.2.Q4_K_M.gguf \
  --ngl 99 --parallel 1,2,4 --max-tokens 128 --reps 3
```

Notes:
- **First load of a 7B Q4 off the SD card is I/O-bound (~110 s)** — `--load-timeout`
  defaults to 300 s. Subsequent loads (page cache warm) are faster.
- Run with **Gamescope up** (Game Mode, or a desktop session) to measure headroom
  under realistic co-tenancy (§6). The memory numbers are only meaningful on the Deck.
- `--ngl 0` (CPU) on a 7–8B model is **slow** — expect minutes per cell; trim
  `--max-tokens`/`--reps` for the CPU rows if impatient.

## Attach mode — measure the already-running host

If the live host (`--engine chat`) is up, its managed `llama-server` is on loopback
`:8099`. Rather than spawn a second copy (which needs the memory the resident model
already holds), point the bench at it — no spawn, no `-ngl` sweep (the server's
config is fixed), and `--parallel` becomes a **client-side concurrency probe**:

```bash
LPID=$(pgrep -f 'llama-server.*8099' | head -1)
./bench.py --attach 127.0.0.1:8099 --server-pid "$LPID" \
           --note "vulkan/RADV VANGOGH (live -ngl 99 --parallel 1)" \
           --parallel 1,2 --max-tokens 96 --reps 3
```

It reads `/props` for the model + `total_slots`, samples `MemAvailable` and the
server's RSS (the **live-appliance** memory picture), and — if you probe more
concurrency than the server has slots — shows how excess requests **queue/serialize**
(§8) rather than batch. To see batching *speedup* you need a server launched with
`--parallel ≥2`, which is the spawn-mode matrix above. `--note` records the backend,
since attach mode can't read a server log it doesn't own.

## Reading the results

`results-*.md` is a table; `results-*.json` has every per-stream number. Columns:

| Column | Meaning |
|---|---|
| **backend** | `vulkan` (RADV VANGOGH) — the goal. `llvmpipe` = software GL (a **fail**, §4). `cpu` = `-ngl 0`. |
| **TTFT s** | wall time to the first token — prompt-processing latency the user waits through. |
| **decode tok/s** | per-stream generation speed (server `predicted_per_second`, median). The headline number. |
| **agg tok/s** | concurrent only: total tokens/s across all slots — system throughput from batching. |
| **prompt tok/s** | prompt-eval speed (`prompt_per_second`); how fast long prompts ingest. |
| **min avail GB** | lowest `MemAvailable` during generation — headroom alongside SteamOS + Gamescope. |
| **RSS GB** | the `llama-server` process's peak resident memory. |
| **load s** | model load time (SD-card I/O bound on first load). |

The driver **exits non-zero** if any cell ran on `llvmpipe` or failed to load — a bad
run is loud, not silent.

## Pass criteria (acceptance #6)

- [ ] Every Vulkan cell reads **`backend: vulkan`**, never `llvmpipe`.
- [ ] One 7–8B Q4 model resident leaves **comfortable headroom** (`min avail` well
      clear of 0) with Gamescope running — confirming the §5 one-model-at-a-time budget.
- [ ] **tok/s recorded** for each model, Vulkan vs CPU; Vulkan is meaningfully faster
      (the §6 hypothesis: ~10–20 Vulkan vs ~4–8 CPU on 7B Q4 — confirm or correct it).
- [ ] `--parallel 2+` shows **aggregate > per-stream** (batching helps) while
      per-stream degrades gracefully — sizing the §8 slot count to the memory budget.

Also confirm the **non-measured** half of step 7 manually: a **phone on the LAN**
opens `http://<deck-ip>:<port>/chat/` against the live host (not this spike) and
chats — watching for AP client isolation (steamdeck-debugging §5).

## Record the run

Paste the `results-*.md` table into the chat-engine notes and update the project
memory with the headline numbers (model, Vulkan tok/s, TTFT, memory headroom, slot
scaling) so Step 7 is "recorded" per acceptance #6.
