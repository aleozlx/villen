# Storage bench — internal NVMe vs SD card, for the model-switch cost

A pure-stdlib Python read-bandwidth benchmark that answers one question for the
[`chat`](../../docs/DESIGN-chat.md) engine: **would moving the GGUFs off the Steam
Deck's micro-SD card onto its internal NVMe make model-switching cheap?**

The [Step 7 spike](../chat-bench/) measured that switching the resident LLM costs
**~60 s cold**, almost all of it spent re-reading a ~4.4 GB GGUF off the SD card
(`load s` column). The Deck also has a **512 GB internal NVMe with ~126 GB free**.
This tool quantifies the cold-read bandwidth of each disk and projects the resulting
model-load time, so the relocate-or-not decision is a number, not a guess.

> **Status: built, NOT yet run on the Deck.** Verified on a PC (`--self-test` plus a
> real-filesystem cold/warm gap). The on-Deck internal-vs-SD numbers are deferred to a
> future quiet window (the Deck is a shared device — see [`../chat-bench`](../chat-bench/)).

## What it measures

Per path: sequential read bandwidth **cold** (page cache evicted) and **warm** (cached
ceiling), plus the **projected cold-load time** for a GGUF of a given size. Write
bandwidth falls out of creating the test file.

## The no-root trick (why this works on the Deck)

A true cold read needs the page cache dropped, but the Deck's `deck` user has **no root
and no passwordless sudo**, so `echo 3 > /proc/sys/vm/drop_caches` is out. Instead the
tool calls **`posix_fadvise(POSIX_FADV_DONTNEED)`** — a stdlib syscall that evicts *one
file's* clean pages from cache, root-free and file-scoped. The file is `fsync`-ed at
creation and evicted on a fresh fd right before each cold read.

Caveat: `fadvise` is advisory and only evicts clean pages — reliable on ext4, possibly
partial on exFAT. **If cold ≈ warm on the SD card, that's the signal it didn't fully
evict** (treat the warm number as a lower bound on the gap, not as "the SD card is fast").

## Files

- `diskbench.py` — the benchmark (stdlib only: `os` / `time` / `argparse`).
- `.diskbench-testfile.bin`, `*.bin`, results — run output (git-ignored; synthetic test
  files are auto-deleted unless `--keep`).

## Verify on a PC first

```bash
./diskbench.py --self-test          # tiny /tmp run — confirms the harness works
./diskbench.py --path ~ --size-mb 512   # real disk — shows the cold vs warm gap
```

(`--self-test` writes to a private `mkdtemp` dir, usually under `/tmp` = `tmpfs` = RAM,
so cold ≈ warm there — it proves the mechanics, not a disk. Point `--path` at a real
filesystem to see eviction actually bite.)

## Run on the Deck (deferred)

Compare the real model file on each disk — copy it to internal, then benchmark both:

```bash
cp /run/media/deck/SD256/LLM/Qwen2.5-7B-Instruct-Q4_K_M.gguf ~/qwen.gguf
./diskbench.py --file ~/qwen.gguf \
               --also /run/media/deck/SD256/LLM/Qwen2.5-7B-Instruct-Q4_K_M.gguf
```

Or synthetic files of the same size on each disk (no 4.4 GB copy needed):

```bash
./diskbench.py --path ~ --path /run/media/deck/SD256/LLM --size-mb 4505 --reps 3
```

The summary line reports the cold-read speedup and the **projected model-switch saving**
for a 4.4 GB GGUF (e.g. "~55 s → ~4 s"). If internal is many times faster — as expected
for NVMe vs micro-SD — relocating the GGUFs to `~/LLM` (operator action; the host's
`--model-path`/`--models-dir` just point elsewhere) turns a ~60 s cold switch into a
near-instant one.

## Record the run

Paste the summary into the chat-engine notes and update project memory with the
internal-vs-SD cold bandwidths + the projected switch time, so the relocate decision is
on record.
