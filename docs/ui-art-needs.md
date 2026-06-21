# Villen — UI art needs (hand-off checklist)

> Quick list of **art elements worth having instead of text** for the Deck launcher,
> the admin shell, and the per-engine views. Not a spec — just *what* to generate, so
> it can be farmed out (e.g. to ChatGPT) in parallel while the UI code is built. We'll
> compact/refine this later.
>
> **Already have** (don't regenerate): Steam-library art in `rc/` (capsule, hero,
> header, icon, wordmark), `client/logo-wordmark.png`, and the chess piece sprites in
> `client/pieces/`.
>
> **Global style** (keep it loose for now): flat, high-contrast, legible at small size
> on the Deck panel **and** as finger-size touch targets (the admin UI is touch-first,
> [`DESIGN-admin-shell.md`](DESIGN-admin-shell.md) §6); one consistent icon set;
> transparent background (SVG preferred, or PNG @2x); on-theme with the disguised-dragon
> / pocket-handheld vibe ([`villen-art-brief.md`](villen-art-brief.md)).

## ⭐ Highest value (do first)

**Launcher — per-engine tiles/icons** (so the home screen is icons, not a text list):
- [x] `chess` — board/king or the archbishop (bishop+knight) nod
- [x] `filter` — camera + morphology/grid motif
- [x] `snake` — snake + apple
- [x] `canvas` — brush / paint splotch
- [x] `jam` — step-sequencer grid / music note
- [x] `chat` — speech bubble (local-LLM, no cloud)

**Shell chrome** (persistent across views):
- [x] **Home** chip (touch "back to launcher")
- [x] **System Info** button (gear / info)
- [x] **Quit / power**
- [x] **Players-connected** indicator (people / plug); + tiny seat-status glyphs for
      connected / disconnected / open (today: colored dots — glyphs optional)

## Per-engine control glyphs (medium value)

- [x] **jam** — transport: play / stop / pause; **mute**; metronome/tempo; per-track
      instrument glyphs: kick, snare, hat, bass, synth
- [x] **canvas** — brush, eraser, palette/color swatches, **clear**, undo, pan/zoom
- [x] **filter** — camera/record, **privacy/lock** (raw never leaves device), pipeline
      add/remove; optional per-operator glyphs (erode, dilate, open, close, gradient,
      top-hat)
- [x] **chat** — send, stop-generation, new-conversation; small **model badges**
      (Llama / Qwen / Mistral)
- [x] **snake** — food/apple sprite, snake-head (cells can otherwise be plain shapes)
- [ ] **chess** — pieces already exist; only need **fairy-piece** sprites when fairy
      chess lands (archbishop / chancellor, etc.)

## States & misc (nice to have)

- [x] **"No engine running"** / idle-attract illustration for the launcher
- [x] Loading spinner / progress
- [x] Error / offline glyph
- [x] System-Info view: Steam-Deck / APU / GPU / Wi-Fi glyphs (minor)
- [x] "Scan to join" framing around the QR code (optional)

## Notes for whoever generates these

- Deliver each icon standalone (one concept per file), square, on transparent bg.
- Two weights if easy: a line/outline version and a filled version (states: idle vs
  active).
- Keep the per-engine tiles visually a *set* (same grid, same stroke weight) so the
  launcher reads as one cabinet, not six clip-arts.
