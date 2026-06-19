# Villen — visual identity & art brief

A brief to hand to an image model (e.g. ChatGPT / DALL·E) to generate Villen's
cover, banner, logo, and icon. Generated files go in [`../rc/`](../rc/) (see the
filenames in §5). **Read §4 (constraints) before generating — this project
deliberately uses only original art.**

---

## 1. What Villen is (so the art means something)

Villen is a **portable game server you carry**: a single native binary on a
handheld that quietly *is* the authoritative game host and serves remote players
from their own browsers over the local network — no cloud, no accounts. Chess is
its first game.

**The core metaphor (use this).** The name nods to a dragon of folklore that
lives disguised as an unremarkable traveler — ordinary on the surface, something
rarer underneath. That is the whole brand: **a humble everyday handheld that is
secretly a powerful host.** Ordinary shell, mythic core.

Mood words: understated, premium, a little mythic, quietly powerful, calm
confidence. **Not** loud, not busy, not "gamer-RGB."

---

## 2. Visual direction

- **Primary concept:** a plain, generic handheld game device (original design —
  *not* any real product) that reveals a dragon underneath: a dragon coiled
  inside/behind it, or the device casting a dragon-shaped shadow, or scales/embers
  showing through a cracked-open "ordinary" surface.
- **Secondary motif:** chess. A knight or king silhouette, a board receding to a
  horizon, pieces that subtly double as the dragon's form (e.g. a knight whose
  mane is a wing). Use sparingly — one clear idea per asset.
- **"One binary / local" idea (optional accent):** faint circuit traces or a
  short-range signal radiating to small browser/phone glyphs — the "host serves
  the room" story. Keep it a background whisper, not the subject.
- **Composition:** strong, readable **silhouette** that survives at thumbnail
  size; generous negative space; one focal point.

### Palette (pick one, state it in the prompt)
- **A — "Ember in the slate":** charcoal/graphite base (`#15181D`, `#23272E`) with
  a single warm ember accent (`#E2603B` / `#F0A34A`) for "the dragon underneath."
- **B — "Verdant traveler":** deep ink/teal base (`#0F1A1C`, `#15282B`) with a cool
  emerald-jade accent (`#2FBF8F`) and a thin gold edge (`#D9B45B`).

Two-tone-plus-accent; avoid rainbow gradients.

### Typography (wordmark)
- Clean geometric or humanist **grotesk**, medium weight, slightly condensed.
- One subtle "disguise" detail in the wordmark — a single scale, a small wing
  serif, or a notch — that you only notice on a second look. Otherwise plain.
- All-lowercase "villen" reads calmer; title-case "Villen" reads more product.
  Provide both if generating the logo.

### Style
- Flat-with-depth / modern vector poster feel; subtle grain or soft rim-light ok.
- **Avoid:** photorealism, stock-3D render look, cluttered fantasy splash art,
  lens flares, heavy bevels.

---

## 3. Deliverables & sizes

| Asset | Size (px) | Notes |
|---|---|---|
| Wordmark / logo | square + wide, **transparent PNG** | with and without the dragon mark |
| App / library icon | 512×512, 256×256 | strong silhouette, works tiny |
| Vertical cover ("capsule") | 600×900 | portrait key art + wordmark |
| Header / banner | 460×215 and **1280×640** | wide; 1280×640 doubles as the GitHub social banner |
| Hero / page background | 1920×620 | wide cinematic, wordmark off-center, lots of negative space |
| README banner | 1280×320 (or reuse 1280×640) | slim top-of-readme strip |

Generate at 2× and downscale for crisp small sizes.

---

## 4. Constraints (important)

- **Original artwork only.** No copyrighted or trademarked characters,
  franchises, monsters, or styles. The repo has already scrubbed direct
  fantasy-franchise references for copyright reasons — keep it that way. The
  dragon is a *generic, original* dragon-in-disguise, not anyone's IP.
- **No real-product or company marks.** Do **not** render the Steam logo, Valve
  marks, the actual Steam Deck industrial design, Nintendo/Sony/Xbox devices, or
  any chess-set brand. The handheld is a clean, invented silhouette.
- Must read well at **thumbnail** size and on both light and dark backgrounds
  (provide transparent-background variants for the logo/icon).
- MIT-licensed project; the art becomes project branding, so keep it
  self-consistent across assets (same dragon, same palette, same wordmark).

---

## 5. Output filenames (save into `rc/`)

```
rc/logo.png                 # wordmark + mark, transparent
rc/logo-wordmark.png        # wordmark only, transparent
rc/icon-512.png             # app/library icon
rc/cover-capsule-600x900.png
rc/banner-1280x640.png      # also the GitHub social banner
rc/header-460x215.png
rc/hero-1920x620.png
rc/readme-banner-1280x320.png
```

---

## 6. Ready-to-paste prompts

**Logo / wordmark**
> Minimal vector logo for a software project called "Villen". A clean geometric
> grotesk lowercase wordmark "villen" in off-white, paired with a small original
> mark: a coiled dragon hidden inside a plain handheld-device silhouette,
> readable as both. One subtle scale detail worked into a letter. Palette: deep
> charcoal background with a single warm ember accent. Flat, premium, lots of
> negative space, transparent background. No existing franchise or brand marks.

**Vertical cover (600×900)**
> Portrait key art, 600×900, for "Villen", a portable game host. Central image: a
> plain, invented handheld game device, calm and ordinary on the surface, with an
> original dragon revealed coiled behind/within it and faint ember glow leaking
> through. A faint chessboard recedes below; a knight piece echoes the dragon's
> wing. Charcoal/graphite palette with one ember accent. Understated, mythic,
> premium poster style, strong silhouette, generous negative space, wordmark
> "villen" lower third. Original art only — no real products, logos, or franchise
> characters.

**Wide banner / social (1280×640)**
> Wide cinematic banner, 1280×640, for "Villen". Off-center: an invented handheld
> casting a large dragon-shaped shadow across a dark surface, faint short-range
> signal arcs reaching small browser/phone glyphs (players on a local network).
> Two-tone slate palette with a single ember (or emerald) accent. Calm, premium,
> lots of empty space on the wordmark side; place "villen" wordmark left. Flat
> vector-with-depth, no franchise or brand marks.

Adjust palette line per §2 (A or B). Keep the *same* dragon and wordmark across
every prompt for consistency.
