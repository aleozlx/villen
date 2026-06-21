# Villen UI art

This directory is the standalone icon library requested by
[`docs/ui-art-needs.md`](../../docs/ui-art-needs.md).  Every asset is a 64 × 64,
transparent-background SVG with a `title`, a 4 px rounded stroke, and the shared
*Ember in the slate* palette:

- outline / idle: `#F0A34A` (ember gold)
- active accents: `#E2603B` (ember red)
- active-tile foreground: `#15181D` (graphite)

The six `engine-*.svg` launcher tiles also have `*-active.svg` inverted variants.
All other controls use the same artwork for normal and pressed state; tint the
button/container, not the glyph, to retain its touch-size contrast.

## File groups

- `engine-*`: launcher tiles (`chess`, `filter`, `snake`, `canvas`, `jam`, `chat`)
- `home`, `system-info`, `power`, `players`, `seat-*`: shared shell chrome
- `jam-*`: transport, tempo, mute, and instrument controls
- `brush`, `eraser`, `palette`, `clear`, `undo`, `pan-zoom`: canvas controls
- `camera-record`, `privacy-lock`, `pipeline-*`, `erode`, `dilate`, `open`,
  `close`, `gradient`, `top-hat`: filter controls
- `send`, `stop-generation`, `new-conversation`, `model-*`: chat controls
- `apple`, `snake-head`: snake sprites
- `idle`, `spinner`, `error-offline`, `system-*`, `wifi`, `qr-frame`: shared states

Regenerate after editing the source geometry with:

```sh
node tools/generate-ui-art.mjs
```
