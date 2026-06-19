// Gamepad input adapter (DESIGN §7 — the architecturally critical milestone).
//
// Mechanically nothing like the pointer: a cursor square is moved with the
// D-pad / left stick, A picks a piece then its destination, B cancels. Yet it
// emits the *identical* plain-data move into the *same* intake the mouse uses —
// `submitMove({from, to})`. Neither adapter knows the other exists; the intake
// cannot tell which produced a move. That source-agnostic seam is what every
// deferred feature (Deck-input routing, reconnection, spectators) relies on.
//
// The Gamepad API is poll-based (only connect/disconnect fire events), so we
// sample it once per animation frame and act on button/stick *edges*.

import { squareName } from "../src/board.js";

const BTN = { A: 0, B: 1, UP: 12, DOWN: 13, LEFT: 14, RIGHT: 15 };
const AXIS_THRESHOLD = 0.5;
const REPEAT_MS = 160; // stick auto-repeat cadence while held

export class GamepadInput {
  constructor(board, game, submitMove) {
    this.board = board;
    this.game = game;
    this.submit = submitMove;
    this.from = null;           // selected source square name, or null
    this.cx = 4;                // cursor file (e-file)
    this.cy = 0;                // cursor rank (rank 1) — white's back rank
    this.prev = {};             // previous button-pressed state, for edge detect
    this.nextRepeat = 0;        // timestamp gate for stick repeat
    this.active = false;

    window.addEventListener("gamepadconnected", () => { this.active = true; });
    requestAnimationFrame((t) => this.tick(t));
    this.paint();
  }

  // Called on every new server state: the prior selection is stale.
  reset() {
    this.from = null;
    this.paint();
  }

  tick(now) {
    const pads = navigator.getGamepads ? navigator.getGamepads() : [];
    const pad = [...pads].find((p) => p);
    if (pad) {
      this.active = true;
      this.handle(pad, now);
    }
    requestAnimationFrame((t) => this.tick(t));
  }

  pressed(pad, idx) {
    return !!(pad.buttons[idx] && pad.buttons[idx].pressed);
  }

  edge(pad, idx) {
    const now = this.pressed(pad, idx);
    const was = this.prev[idx];
    this.prev[idx] = now;
    return now && !was;
  }

  handle(pad, now) {
    // Directional: D-pad edges fire once; stick fires on entry then auto-repeats.
    let dx = 0, dy = 0;
    if (this.edge(pad, BTN.LEFT)) dx = -1;
    if (this.edge(pad, BTN.RIGHT)) dx = 1;
    if (this.edge(pad, BTN.UP)) dy = 1;
    if (this.edge(pad, BTN.DOWN)) dy = -1;

    const ax = pad.axes[0] || 0, ay = pad.axes[1] || 0;
    const stickActive = Math.abs(ax) > AXIS_THRESHOLD || Math.abs(ay) > AXIS_THRESHOLD;
    if (stickActive && now >= this.nextRepeat) {
      if (ax > AXIS_THRESHOLD) dx = 1; else if (ax < -AXIS_THRESHOLD) dx = -1;
      if (ay > AXIS_THRESHOLD) dy = -1; else if (ay < -AXIS_THRESHOLD) dy = 1;
      this.nextRepeat = now + REPEAT_MS;
    } else if (!stickActive) {
      this.nextRepeat = 0;
    }

    if (dx || dy) {
      this.cx = Math.max(0, Math.min(7, this.cx + dx));
      this.cy = Math.max(0, Math.min(7, this.cy + dy));
      this.paint();
    }

    if (this.edge(pad, BTN.A)) this.confirm();
    if (this.edge(pad, BTN.B)) { this.from = null; this.paint(); }
  }

  confirm() {
    const cur = squareName(this.cx, this.cy);
    const legal = this.game.legalMoves;
    if (this.from && legal.some((m) => m.from === this.from && m.to === cur)) {
      // Auto-queen on a promotion so a gamepad-only player can always finish;
      // still a plain {from,to,promotion} object through the shared intake.
      const promo = legal.some((m) => m.from === this.from && m.to === cur && m.promotion);
      this.submit(promo ? { from: this.from, to: cur, promotion: "queen" }
                        : { from: this.from, to: cur });
      this.from = null;
    } else if (legal.some((m) => m.from === cur)) {
      this.from = cur;  // (re)select
    } else {
      this.from = null;
    }
    this.paint();
  }

  paint() {
    if (!this.active) { this.board.highlight("gamepad", []); return; }
    const items = [{ sq: squareName(this.cx, this.cy), cls: "cursor" }];
    if (this.from) {
      items.push({ sq: this.from, cls: "sel" });
      for (const m of this.game.legalMoves) {
        if (m.from !== this.from) continue;
        items.push({ sq: m.to, cls: this.board.hasPiece(m.to) ? "target capture" : "target" });
      }
    }
    this.board.highlight("gamepad", items);
  }
}
