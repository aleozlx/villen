// Board UI — rendering only. It knows how to draw a position and how to paint
// highlights, and it reports square clicks. It knows nothing about rules,
// networking, or which input device is driving it.
//
// Highlights are layered into named "channels" (e.g. "pointer", "gamepad",
// "status"). Each input adapter owns its own channel, so two adapters can be
// live at once without clobbering each other's highlights — and without either
// adapter needing to know the other exists (DESIGN §7).

const GLYPH = {
  K: "♔", Q: "♕", R: "♖", B: "♗", N: "♘", P: "♙",
  k: "♚", q: "♛", r: "♜", b: "♝", n: "♞", p: "♟",
};

const FILES = "abcdefgh";

export function squareName(file, rank) {
  return FILES[file] + (rank + 1);
}

export class Board {
  constructor(el) {
    this.el = el;
    this.squares = {};       // "e4" -> square element
    this.channels = {};      // channel -> [{ sq, cls }]
    this.clickHandlers = [];

    // White at the bottom: render rank 8 (top) down to rank 1 (bottom).
    for (let rank = 7; rank >= 0; rank--) {
      for (let file = 0; file < 8; file++) {
        const name = squareName(file, rank);
        const sq = document.createElement("div");
        sq.className = "square " + ((file + rank) % 2 ? "light" : "dark");
        sq.dataset.sq = name;
        sq.addEventListener("click", () => this._emitClick(name));
        this.el.appendChild(sq);
        this.squares[name] = sq;
      }
    }
  }

  onSquareClick(fn) { this.clickHandlers.push(fn); }
  _emitClick(name) { for (const fn of this.clickHandlers) fn(name); }

  // Render piece glyphs from the placement field of a FEN string.
  renderPosition(fen) {
    for (const name in this.squares) {
      const sq = this.squares[name];
      const piece = sq.querySelector(".piece");
      if (piece) piece.remove();
    }
    const placement = fen.split(" ")[0];
    let rank = 7, file = 0;
    for (const ch of placement) {
      if (ch === "/") { rank--; file = 0; }
      else if (ch >= "1" && ch <= "8") file += +ch;
      else {
        const span = document.createElement("span");
        span.className = "piece" + (ch === ch.toLowerCase() ? " black" : "");
        span.textContent = GLYPH[ch] || "";
        this.squares[squareName(file, rank)].appendChild(span);
        file++;
      }
    }
  }

  hasPiece(name) { return !!this.squares[name]?.querySelector(".piece"); }

  // ── Attack heatmap ─────────────────────────────────────────────────────────
  // A control-tint layer *under* the pieces and highlight channels. We override
  // each square's background inline (a null colour reverts to the .light/.dark
  // CSS so the board's own colours show where nobody attacks). The selection
  // highlight still wins via `background … !important`, and the box-shadow /
  // ::after highlights layer on top, so input UX is untouched. Counts (optional)
  // ride along as a small badge per square. `cells` comes from heatmap.js.
  setHeatmap(cells, { showCounts = false } = {}) {
    this.el.classList.add("heatmap");
    for (const { sq, color, w, b } of cells) {
      const el = this.squares[sq];
      if (!el) continue;
      el.style.background = color || "";        // null -> revert to .light/.dark
      this._setCount(el, showCounts && (w || b) ? `${w} / ${b}` : null);
    }
  }

  clearHeatmap() {
    if (!this.el.classList.contains("heatmap")) return;  // already clear: skip the 64-square sweep
    this.el.classList.remove("heatmap");
    for (const name in this.squares) {
      const el = this.squares[name];
      el.style.background = "";
      this._setCount(el, null);
    }
  }

  _setCount(el, text) {
    let badge = el.querySelector(".count");
    if (!text) { badge?.remove(); return; }
    if (!badge) {
      badge = document.createElement("span");
      badge.className = "count";
      el.appendChild(badge);
    }
    badge.textContent = text;
  }

  // Replace the contents of one highlight channel and repaint.
  highlight(channel, items) {
    this.channels[channel] = items || [];
    this._repaint();
  }

  _repaint() {
    for (const name in this.squares) {
      const sq = this.squares[name];
      sq.classList.remove("sel", "target", "capture", "cursor", "check");
    }
    for (const channel in this.channels) {
      for (const { sq, cls } of this.channels[channel]) {
        const el = this.squares[sq];
        if (!el) continue;
        for (const c of cls.split(" ")) el.classList.add(c);
      }
    }
  }
}
