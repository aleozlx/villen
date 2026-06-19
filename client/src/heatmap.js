// Attack heatmap — a pure, client-side visualization of board *control*.
//
// For every square it counts how many White and how many Black pieces *attack*
// it (a square is "controlled" whether the occupant is friend or foe — a
// defended square is controlled too, which is what a tactician cares about),
// then maps (whiteCount, blackCount) to a four-state palette: White's control
// is blue, the Red Queen's side is red, squares both armies fight over turn
// purple/magenta, and squares no one attacks stay solid white. Deeper colour =
// more attackers.
//
// This stays entirely on the client and touches neither the engine nor the wire
// contract: the server already broadcasts full FEN state (DESIGN §6), and the
// heatmap is derived from exactly that string — no new message, no engine change.
// Pure functions only; no DOM, no network.

import { squareName } from "./board.js";

const ROOK_DIRS = [[1, 0], [-1, 0], [0, 1], [0, -1]];
const BISHOP_DIRS = [[1, 1], [1, -1], [-1, 1], [-1, -1]];
const KNIGHT_OFF = [[1, 2], [2, 1], [-1, 2], [-2, 1], [1, -2], [2, -1], [-1, -2], [-2, -1]];
const KING_OFF = [...ROOK_DIRS, ...BISHOP_DIRS];

function inBounds(r, c) {
  return r >= 0 && r < 8 && c >= 0 && c < 8;
}

// FEN placement field -> board[rank][file], where rank 0 is rank 1 (White's
// home rank). The walk mirrors board.js's renderPosition exactly, so the two
// can never disagree about where a piece sits.
function parsePlacement(fen) {
  const board = Array.from({ length: 8 }, () => Array(8).fill(null));
  const placement = (fen || "").split(" ")[0];
  let rank = 7, file = 0;
  for (const ch of placement) {
    if (ch === "/") { rank--; file = 0; }
    else if (ch >= "1" && ch <= "8") file += +ch;
    else {
      if (inBounds(rank, file)) {
        board[rank][file] = {
          type: ch.toUpperCase(),                       // P N B R Q K
          color: ch === ch.toLowerCase() ? "b" : "w",
        };
      }
      file++;
    }
  }
  return board;
}

// Squares the piece on (r,c) attacks/controls, ignoring friend-vs-foe. A slider
// stops at the first blocker but still controls the blocker's square.
function attacksFrom(board, r, c) {
  const piece = board[r][c];
  if (!piece) return [];
  const out = [];
  const slide = (dirs) => {
    for (const [dr, dc] of dirs) {
      let nr = r + dr, nc = c + dc;
      while (inBounds(nr, nc)) {
        out.push([nr, nc]);
        if (board[nr][nc]) break;     // blocked, but this square is still controlled
        nr += dr; nc += dc;
      }
    }
  };
  const leap = (offs) => {
    for (const [dr, dc] of offs) {
      const nr = r + dr, nc = c + dc;
      if (inBounds(nr, nc)) out.push([nr, nc]);
    }
  };
  switch (piece.type) {
    case "P": {
      // White attacks toward rank 8 (increasing rank); Black toward rank 1.
      const dir = piece.color === "w" ? 1 : -1;
      for (const dc of [-1, 1]) {
        const nr = r + dir, nc = c + dc;
        if (inBounds(nr, nc)) out.push([nr, nc]);
      }
      break;
    }
    case "N": leap(KNIGHT_OFF); break;
    case "B": slide(BISHOP_DIRS); break;
    case "R": slide(ROOK_DIRS); break;
    case "Q": slide([...ROOK_DIRS, ...BISHOP_DIRS]); break;
    case "K": leap(KING_OFF); break;
    default: break;
  }
  return out;
}

// Per-square (white, black) control counts for the whole board.
function controlMap(board) {
  const w = Array.from({ length: 8 }, () => Array(8).fill(0));
  const b = Array.from({ length: 8 }, () => Array(8).fill(0));
  for (let r = 0; r < 8; r++) {
    for (let c = 0; c < 8; c++) {
      const p = board[r][c];
      if (!p) continue;
      for (const [ar, ac] of attacksFrom(board, r, c)) {
        if (p.color === "w") w[ar][ac]++; else b[ar][ac]++;
      }
    }
  }
  return { w, b };
}

// OKLCH -> sRGB. Perceptual lightness stays monotonic, so "more Black control =
// darker" actually holds visually (a plain HSL ramp would not).
function oklchToRgb(L, C, hDeg) {
  const h = (hDeg * Math.PI) / 180;
  const a = C * Math.cos(h), bb = C * Math.sin(h);
  const l_ = L + 0.3963377774 * a + 0.2158037573 * bb;
  const m_ = L - 0.1055613458 * a - 0.0638541728 * bb;
  const s_ = L - 0.0894841775 * a - 1.291485548 * bb;
  const l = l_ ** 3, m = m_ ** 3, s = s_ ** 3;
  const R = +4.0767416621 * l - 3.3077115913 * m + 0.2309699292 * s;
  const G = -1.2684380046 * l + 2.6097574011 * m - 0.3413193965 * s;
  const B = -0.0041960863 * l - 0.7034186147 * m + 1.707614701 * s;
  const g = (x) => {
    x = x <= 0.0031308 ? 12.92 * x : 1.055 * x ** (1 / 2.4) - 0.055;
    return Math.max(0, Math.min(255, Math.round(x * 255)));
  };
  return `rgb(${g(R)}, ${g(G)}, ${g(B)})`;
}

// (white, black) control counts -> fill colour. Every square gets one: an
// unattacked square is solid white (not transparent), so the heatmap fully
// owns the board's colour while it's on.
export function squareColor(w, b) {
  if (w === 0 && b === 0) return "#f4f4f4";          // unattacked — solid white
  if (w > 0 && b === 0) {
    // White's side: blue, deeper with more attackers.
    const t = Math.min(1, w / 4);
    return oklchToRgb(0.80 - 0.23 * t, 0.09 + 0.10 * t, 256);
  }
  if (b > 0 && w === 0) {
    // Red Queen's side: red, deeper with more attackers.
    const t = Math.min(1, b / 4);
    return oklchToRgb(0.70 - 0.23 * t, 0.14 + 0.09 * t, 27);
  }
  // Contested -> purple/magenta, charged by how fiercely fought.
  const fierce = Math.min(1, Math.min(w, b) / 3);
  return oklchToRgb(0.60 - 0.15 * fierce, 0.15 + 0.10 * fierce, 328);
}

// Whole-board heatmap for a FEN: one { sq, color, w, b } per square. `color` is
// always a solid fill (white where neither side controls). `w`/`b` are the
// attacker counts.
export function heatmapFor(fen) {
  const board = parsePlacement(fen);
  const { w, b } = controlMap(board);
  const cells = [];
  for (let r = 0; r < 8; r++) {
    for (let c = 0; c < 8; c++) {
      cells.push({
        sq: squareName(c, r),
        color: squareColor(w[r][c], b[r][c]),
        w: w[r][c],
        b: b[r][c],
      });
    }
  }
  return cells;
}
