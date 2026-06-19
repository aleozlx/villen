# Villen — driving a seat: browser, CLI & server-side agents (design)

**Status:** design / additive enhancement to the existing host.
**Scope:** how a seat gets *driven* — by a human, a local coding agent, or a
server-side engine/LLM — plus watch-only spectating. Layered on the existing
WebSocket player edge, session/seat model, and single-thread loop; the engine stays
pure (DESIGN §9.1) and all wire format stays on the player edge (§9.5). Additive:
extend the current programs, don't rewrite them.

---

## 1. Three ways a seat is driven (the mainstream)

| Path | Who | Reaches the host via | Auth | Host cost |
|---|---|---|---|---|
| **Browser ↔ WebSocket** | humans | the served web UI | connection-bound seat (exists) | none — already built |
| **CLI ↔ WebSocket** | local coding agents (Claude Code, Codex) | `villen move` / `villen watch`, a thin WS client | cooperative → SipHash token | a few `state` fields + a wider move check |
| **Server-side agent** | Stockfish / an LLM | the host calls *out* to it | none — the host frames the question | a UCI subprocess (and, for cloud, one async call) |

**The invariant across all three:** the engine is the single authority. Every move,
however it arrives, is validated by `position.apply()` (turn + legality) before it
changes anything (DESIGN §3.2). That alone delivers **"agents never move out of
turn"**; the paths differ only in *who may be a seat* and *how easy submitting is*.

**Watching** is read-only participation, not a fourth path: a human via `/?spectate`,
an agent via `villen watch`. A spectator holds no seat and any move it attempts is
rejected server-side.

---

## 2. Shared mechanics

The `state` message (one serializer, [protocol.cpp](../host/src/protocol.cpp)) stays
**perspective-neutral** and gains three additive fields (browser clients ignore
unknown fields, so nothing breaks):

- `version` — monotonic ply counter; the change cursor and, in the token tier, the
  replay/order bind.
- `game_id` — per-game nonce; rotates on `reset`.
- `lastMove` — the move just applied (the engine keeps no history; the host remembers
  the single last one).

A **seat is owned by whatever drives it**: a live connection (browser), a credential
(CLI agent), or the server (a mover). Admin **Free** re-opens a seat universally.

---

## 3. Browser ↔ WebSocket (humans)

Already built. The host serves the full UI; players connect over WS; seats are
connection-bound with token-free reconnect by name (DESIGN §13). **Add** a watch-only
`http://<host>:<port>/?spectate` that joins as spectator, renders the board read-only
(reusing [board.js](../client/src/board.js)), and is rejected server-side if it tries
to move — closing the lone-player gap in
[`handleProposeMove`](../host/src/session.cpp) (never trust the client, §9.3).

---

## 4. CLI ↔ WebSocket (local coding agents)

`villen watch` / `villen move <mv>` — a small local CLI that Claude Code / Codex shell
out to. It **wraps the existing WS protocol** (no second network surface): each
invocation opens a short-lived WS connection, reads the pushed `state`, sends a
`proposeMove`, correlates the result, and exits with a code. The agent never sees a
key, token, version, or the wire — this is the original precompiled-tool model on
Villen's transport.

Because the connection is **ephemeral** (one per move), the agent's seat is
**credential-owned, not connection-owned** — connect/close never flips it to
`disconnected/held`.

Auth is staged:
- **Cooperative (ship first):** the move carries the claimed `seat` + `version`; the
  host checks `seat == side_to_move ∧ version == expected ∧ legal`. Stops out-of-turn
  and double-move for cooperating agents on a trusted LAN. It does *not* stop claiming
  the opponent's color — that's impersonation, which the token tier closes.
- **Token (later):** the move carries `token = SipHash(key, game_id‖version)`, computed
  inside the CLI; the host verifies against `key[side_to_move]` only, so wrong-turn and
  wrong-identity reject identically. Keys are minted per game and shown in the admin UI
  (never on the wire); admin **Free** rotates a keyed seat's key.

The agent command is identical across both tiers — only the env var (`VILLEN_SEAT` →
`VILLEN_KEY`) and the CLI's internals change.

Exit codes: `0` ok / `1` not-your-turn or lost race / `2` illegal / `3` host
unreachable / `4` bad key (token tier).

---

## 5. Server-side agent (Stockfish / LLM)

Assign a seat to a **mover** in the admin UI (Human / Stockfish / LLM). When that seat
is to move, the host summons a move from the mover, validates it (same
`position.apply()`), then applies + broadcasts. **No auth** is needed: the host only
ever asks the seat whose turn it is, and frames the question itself (FEN + the legal
moves), so there is no inbound channel to abuse and no identity to forge. An
illegal/garbled answer is simply re-asked.

Single-thread handling (DESIGN §5) splits cleanly by mover:
- **Local engine (UCI / Stockfish):** the subprocess's stdout is just another fd in the
  existing `poll()` set — write `position … / go`, read `bestmove`, apply. **No
  threads, no new deps, fully local.** Do this first.
- **Cloud LLM:** a blocking TLS call needs the one §5-sanctioned guarded queue (a worker
  posts the move back to the loop) plus an HTTP/TLS dependency (libcurl) — and the
  position leaves the LAN (privacy/egress, against the no-cloud ethos). A *local* LLM
  (e.g. llama.cpp on the LAN) avoids both the TLS dep and the egress.

This makes a mover a **move oracle the host summons**, which is ideal for "watch
Stockfish vs an LLM" or "play an AI seat from the browser." It is *not* the same as an
autonomous agent running its own session (that's the CLI path, §4).

---

## 6. Rollout

All increments are independent; suggested order:
1. The three `state` fields + `/?spectate`.
2. **Server-side Stockfish** — no auth, fully local, pure §5 fit; the fastest path to AI
   opponents and AI-vs-AI.
3. The **cooperative CLI** (`villen watch`/`move`).
4. The **CLI token tier** (drop `seat`, add `token`; engine predicate changes in the
   same step).

---

## Alternatives & discussions (not mainstream — kept for the record)

**Phone-a-bot (cloud chatbot via human courier).** Cloud Claude/ChatGPT mobile apps
execute in the cloud and **cannot** reach a LAN host — a hard limit, not a gap. But a
human can be the bridge: a "Copy prompt" button puts a framed, legal-move-constrained
question on the clipboard; the human pastes it into the phone's chatbot and pastes the
move back into a UI box (validated against the legal list). Works with any chatbot,
zero infra/auth — and is arguably the most demo-able mode.

**Browser free-ride / "agent FYI" DOM.** A browser-operating agent (Claude Code +
Chrome extension, computer-use) runs its browser *on the LAN*, so it can free-ride the
page's WS + seat with no CLI. Formalize the already-exposed `window.villen`
([app.js:182](../client/src/app.js)) into a stable agent API (`state()`,
`move("e2e4")`, `onChange`), add a JSON `state` mirror in the DOM and a
self-documenting `?agent` mode. This is DESIGN §7's "third input adapter," and it could
make the CLI optional for browser-capable agents.

**Cross-platform CLI builds.** Decouple the CLI from the host binary; build portable
C++ with `zig cc` (kills the glibc trap; one machine → all OSes) or use Go; host-serve
the binaries at `/tools/…`; ship a dependency-free single-file `villen.py` as a
zero-build fallback.

**Hands-free cloud chatbot via relay.** The only way to let a cloud chatbot play
*without* a human is to expose the host (public port or an outbound relay) + a
connector — which breaks LAN-only and no-infra. Rejected; recorded so it reads as a
decision, not an oversight.

---

## Appendix: SipHash (CLI token tier)

`token = SipHash(key, game_id‖version)` — a keyed PRF, ARX-only (add/rotate/xor),
constant-time. Use the native **64-bit** output (never 32-bit — brute-forceable),
rounds ≥ SipHash-1-3. Vendor a small SipHash implementation (~40 lines) shared between
the host (verifies) and the CLI (computes); both must agree on little-endian encoding.
The host is trusted and holds both keys, so non-repudiation is explicitly out of scope
(symmetric PRF, not signatures).
