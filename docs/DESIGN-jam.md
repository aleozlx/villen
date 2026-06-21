# Villen — the `jam` engine: a clock-synced collaborative groovebox (design & handoff)

> **`villen-jam`** is the sixth engine in Villen's game slot and the first about
> **shared time**: a room makes music together on a shared step-sequencer, the host
> keeps the **authoritative beat** and the **shared pattern**, and every device plays
> the same groove because each one **synthesizes its own audio locally, locked to the
> host's clock**. The non-obvious decision: **no audio ever crosses the wire** — only
> the pattern and the clock do. Names derive mechanically: namespace `villen::jam`,
> lib `villen_jam`, doc `DESIGN-jam.md`, `--engine jam`. *(Renamed from `ensemble`;
> `jam` keeps the plain-noun convention of `filter`/`chat`.)*

**Status:** design / additive, and **shape-proposing** — the collaborative-audio idea
was open, so this picks a concrete, LAN-friendly shape and argues for it (§2).
**Scope:** the slice that proves a **shared-time** engine fits the single-thread loop
— pattern edits in, an authoritative transport advancing on the host's clock,
sample-accurate playback on every device with no server audio at all.
**Audience:** the agent standing this up, having read
[`DESIGN-villen.md`](DESIGN-villen.md), [`DESIGN-snake.md`](DESIGN-snake.md) (the
authoritative clock), and [`DESIGN-canvas.md`](DESIGN-canvas.md) (the shared op-state
and §5 free total-order).

> **Why this engine earns its keep.** `snake` proved the host can keep an
> authoritative *clock*; `canvas` proved it can hold a *shared mutable state* without
> locks. `jam` needs **both at once and tighter** — the clock is now *musical* (drift
> you can *hear*), and the shared state is the score everyone edits. It is the capstone
> of the slate, and it forces a real question the others didn't: how do you keep
> independent devices in audible sync over a jittery LAN? (Answer: §3.)

---

## 1. Context & one-paragraph pitch

A group opens the `jam` client on their phones/tablets and builds a beat together: tap
cells in a grid (tracks × steps — kick, snare, hat, bass, a synth) and the loop plays,
in time, on every device in the room. The host — the same single binary running chess,
`filter`, `chat`, `snake`, `canvas`, and the admin UI — owns the **transport** (BPM,
play/stop, the running bar position) and the **shared pattern**, applies each toggle on
the loop thread, and **broadcasts** it. Crucially, the host **makes no sound**: every
device runs a tiny Web Audio synth that schedules its hits against the host's clock, so
all phones play the same groove in sync. The operator is the bandleader — sets tempo,
hits play, mutes, clears — from the ImGui panel.

---

## 2. The shape decision: stream the score, not the sound

Collaborative audio *sounds* like "send everyone's microphone/instrument audio to a
server, mix, send back." That is the **wrong shape** for a LAN WebSocket appliance, and
worth recording as a decision:

- Real-time group playing needs **<~20 ms** end-to-end or it feels unplayable; a WS
  round trip + jitter + buffering + mixing N streams blows that budget, and you'd be
  building echo cancellation, jitter buffers, and a mixer.
- It fights Villen's grain: large sustained binary streams, server-side DSP, and a
  latency target the architecture wasn't built for.

The shape that *fits*: **stream control, synthesize locally.** Only small messages
cross the wire — pattern toggles and a periodic clock — and each device renders audio
itself with **Web Audio**, scheduled *ahead of time* against a synced clock. This is
how networked music tools actually work, and it turns "impossible latency problem" into
"two easy problems Villen already solves" (a shared op-state + an authoritative clock)
plus one new, well-understood primitive (clock sync, §3). The host stays trivial; the
hard real-time work lives in the browser's audio thread, off the host entirely.

---

## 3. Clock sync (the one genuinely new primitive)

WS messages arrive jittery; audio cannot be jittery. The standard resolution — and the
heart of this engine — is **schedule, don't trigger**: never play a sound when a
message *arrives*; play it at a *pre-computed time* on a synced clock. Two pieces:

1. **Estimate the host clock (NTP-lite).** The client periodically pings the host
   (`{type:"clockReq", t0}` → `{type:"clockRes", t0, tServer}`), measures round-trip,
   and estimates `offset = tServer + rtt/2 − tClient`. A few samples, keep the low-RTT
   ones — millisecond-class on a LAN. The host broadcasts its **transport epoch**:
   "beat 0 of the current play started at host-time T, BPM = B."
2. **Look-ahead scheduling (Web Audio's "tale of two clocks").** A ~25 ms timer in the
   client looks ~100 ms into the future, computes which steps fall in that window from
   `(hostNow = AudioContext.currentTime + offset, BPM, epoch)`, and schedules each hit
   at its exact `AudioContext` time. Because hits are queued **ahead** against the
   synced clock, WS jitter never reaches the audio — only the (small, slowly-varying)
   offset error does, and it's continuously corrected.

The result: every device's beat 17 fires at the same wall-clock instant ± a few ms,
tight enough to groove. This NTP-lite + look-ahead pair is a **reusable timed-engine
primitive** (§11).

---

## 4. The model & the §5 fit (the host stays trivial)

Host state, both serializable (DESIGN §9.2):

- **Transport** — `{ bpm, playing, epochBeat, epochHostMs, steps, swing }`.
- **Pattern** — a grid of cells `{track, step} -> {on, velocity, note?}`; edited by
  **ops** exactly like `canvas` (§3 there), serialized by the single thread into a free
  total order (last toggle wins, everyone converges).

```cpp
void JamGame::onText(ConnId from, msg) {
  if (toggle)   { pattern_.apply(toggle); ws_.broadcast(encode(toggle)); }   // canvas-style
  if (transport && operatorOrAllowed) { transport_.set(...); broadcastTransport(); }
  if (clockReq) ws_.send(from, clockRes(now()));                              // §3
}
void JamGame::tick() {                      // snake-style, but musical & light
  if (transport_.playing) advanceBarCounterFromWallClock();  // for the admin display only
  // No audio here. The host never synthesizes. Clients do.
}
```

The host does **no audio, no DSP, no codec, no subprocess, no GPU** — it is a
synchronized shared-state + clock server, the **lightest** engine of the slate on the
host side. All the real-time difficulty is delegated to Web Audio in the browser, where
it belongs.

---

## 5. Wire contract (player edge — broadcast + clock)

```jsonc
// Client -> server
{ "type": "toggle", "track": 1, "step": 6, "on": true, "vel": 0.8 }
{ "type": "transport", "set": { "bpm": 110, "playing": true } }   // operator (or allowed)
{ "type": "clockReq", "t0": 123456 }
{ "type": "join" }

// Server -> client
{ "type": "config", "tracks": ["kick","snare","hat","bass","synth"], "steps": 16 }
{ "type": "pattern", "cells": [ … ] }                 // snapshot on join
{ "type": "op", "seq": 8821, "toggle": { … } }        // broadcast pattern edit
{ "type": "transport", "bpm": 110, "playing": true,
  "epochBeat": 0, "epochHostMs": 990210, "swing": 0.0 }
{ "type": "clockRes", "t0": 123456, "tServer": 990233 }
```

Authority (chess §3.2): the host owns transport + pattern order; clients send intent. By
default the **operator owns the transport** (tempo/play) while **everyone edits the
pattern** (a kid-safe band); a "free-for-all transport" toggle is a one-line policy
choice (§11).

---

## 6. Instruments (client-side, synthesized, no assets)

The client ships a tiny Web Audio kit — **synthesized**, so there are no sample files to
load or license: kick (pitch-swept sine + click), snare (filtered noise burst), hat
(high-passed noise), bass (saw + lowpass envelope), a simple poly synth. Each **track**
is one voice; each **cell** triggers it at its step with the cell's velocity. Melodic
tracks can carry a `note` per cell for a shared bassline/riff. It is tap-a-grid-hear-a-
beat — friendly enough for kids, deep enough to jam.

---

## 7. Admin UI (the bandleader)

`JamGame::drawAdmin()`, gamepad-navigable: the playing step highlighted live; **BPM**,
**play/stop**, **swing**, **steps (16/32)**, **clear**, per-track **mute**; active
players + edit rate; and the measured **sync spread** across clients (max offset) so the
operator can see the room is locked. In-process, direct state access (DESIGN §9.4). The
host shows the transport; it still makes no sound. (The shell chrome — Home, join URL/QR,
connection count — is provided by [`DESIGN-admin-shell.md`](DESIGN-admin-shell.md); this
panel is just the engine-view body.)

---

## 8. Build, deps, and order

- **No new dependencies — on the host or the client.** The host gains no audio library
  (it never synthesizes); the client uses the browser's built-in **Web Audio**. This is
  the cheapest engine to stand up.
- **CMake:** `villen_jam` pure engine lib (transport + pattern + ops + tests, always
  built, CI-tested, no I/O); the `JamGame` adapter compiles into the host.

**Build order (smallest-spine-first):**
1. **Pure engine + tests** — pattern ops + transport math (beat ↔ time); deterministic
   tests (a fixed op/transport sequence → a fixed schedule). *No host.*
2. **Host shared-pattern + broadcast** — toggles applied + broadcast (canvas-style);
   transport broadcast; bar counter on the loop. Scripted WS client edits.
3. **Client synth on a STUB local clock** — Web Audio kit + look-ahead scheduler playing
   the pattern from a fixed local tempo. *Proves audio + scheduling with no sync yet.*
4. **Clock sync (§3)** — NTP-lite offset + transport epoch; schedule against host time.
   *Two browsers now play in audible sync — the milestone.*
5. **Multi-user shared pattern + transport authority** — many editors, operator tempo.
6. **Admin bandleader** — tempo/play/mute/clear + the sync-spread readout.
7. **Sync spike** — measure the offset spread across real devices (phones/iPads) on the
   LAN; tune look-ahead/ping cadence until the room grooves.

---

## 9. Acceptance criteria

1. `engine/jam/` builds and its deterministic transport/pattern tests pass in headless
   CI.
2. `--engine jam` selects it; other engines are unaffected.
3. Two or more devices play the **same pattern in audible sync**; toggling a cell is
   heard by everyone on the next loop; a tempo change keeps the room locked.
4. A device joining mid-jam snaps to the **current bar** and is in time within a beat.
5. The host produces **no audio** and links **no audio library**; all sound is
   client-side Web Audio.
6. The operator drives tempo/play/mute and sees the cross-device sync spread.

---

## 10. Rejected alternatives

- **Stream raw audio / a WebRTC mesh / server-side mixing.** The latency-and-complexity
  trap §2 exists to avoid; also pulls in WebRTC/SDP/ICE (a DESIGN §4 non-goal). Stream
  the score, synthesize locally.
- **Trigger sounds on message arrival.** Guarantees jitter — schedule ahead against the
  synced clock instead (§3).
- **Peer-to-peer clock (everyone agrees among themselves).** The host is the natural
  metronome; host-authoritative sync is simpler and matches Villen's model.
- **Ship audio samples.** Synthesize the kit (§6) — no files, no licensing, instant
  load.
- **Per-connection private audio** (the `filter`/`chat` model). A jam is shared;
  broadcast is the point (as in `canvas`).

---

## 11. Open questions

- **Transport ownership policy** — operator-only vs. free-for-all tempo/play; a "take
  the wheel" hand-off.
- **Swing / quantize / per-track length** (polyrhythms) — pure-engine data.
- **Melodic input** — a shared on-screen keyboard/scale lane for basslines and riffs.
- **Looping / song sections / recording** the jam to replay or export (leans on §9.2).
- **Per-user instrument ownership** ("you're the drummer") vs. fully shared editing.
- **The clock-sync primitive as shared infrastructure** — `snake` and any future timed
  engine could borrow the NTP-lite + look-ahead pair (§3).
- **Sub-millisecond sync via WebRTC DataChannel** if the LAN-WS offset ever proves too
  loose for tight genres — a later, optional tightening, not the MVP.
- **MIDI hardware** (Web MIDI) as an input source alongside the touch grid.
