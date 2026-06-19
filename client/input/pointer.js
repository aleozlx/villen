// Pointer input adapter (DESIGN §7).
//
// Click a piece to select it, click a destination to move. It emits the move
// into the shared intake `submitMove({from, to})` — the exact same call the
// gamepad adapter makes. It owns only its own "pointer" highlight channel and
// has no awareness of any other input source.

export class PointerInput {
  constructor(board, game, submitMove) {
    this.board = board;
    this.game = game;
    this.submit = submitMove;
    this.from = null;
    board.onSquareClick((sq) => this.click(sq));
  }

  // Called on every new server state: the prior selection is stale.
  reset() {
    this.from = null;
    this.paint();
  }

  click(sq) {
    const legal = this.game.legalMoves;
    // Second click on a legal destination -> emit the move.
    if (this.from && sq !== this.from &&
        legal.some((m) => m.from === this.from && m.to === sq)) {
      this.submit({ from: this.from, to: sq });
      this.from = null;
      this.paint();
      return;
    }
    // Otherwise (re)select if this square has any legal move; else clear.
    if (legal.some((m) => m.from === sq)) {
      this.from = this.from === sq ? null : sq;
    } else {
      this.from = null;
    }
    this.paint();
  }

  paint() {
    const items = [];
    if (this.from) {
      items.push({ sq: this.from, cls: "sel" });
      for (const m of this.game.legalMoves) {
        if (m.from !== this.from) continue;
        items.push({ sq: m.to, cls: this.board.hasPiece(m.to) ? "target capture" : "target" });
      }
    }
    this.board.highlight("pointer", items);
  }
}
