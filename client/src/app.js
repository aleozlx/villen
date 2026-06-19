// Player client orchestrator.
//
// Holds the rendered game state, wires the transport to the board, and — most
// importantly — owns `submitMove`, the *single move intake* every input adapter
// funnels into (DESIGN §7). Adapters never talk to the network; they only call
// submitMove, which wraps the move in a `proposeMove`. Promotion is resolved
// here so adapters stay device-dumb.

import { Board, squareName } from "./board.js";
import { connect } from "./net.js";
import { heatmapFor, squareColor } from "./heatmap.js";
import { PointerInput } from "../input/pointer.js";
import { GamepadInput } from "../input/gamepad.js";

const game = {
  position: "8/8/8/8/8/8/8/8 w - - 0 1",
  turn: "white",
  legalMoves: [],
  status: "active",
  seats: { white: "open", black: "open" },
  mySeat: null,     // set once seat-claiming lands (step 5)
  check: false,
};

const board = new Board(document.getElementById("board"));
const statusEl = document.getElementById("status");
const seatsEl = document.getElementById("seats");
const promoEl = document.getElementById("promo");

const net = connect(onMessage, onStatus);

// Every input source registers here. Each only needs (board, game, submitMove);
// both are live at once — use whichever (DESIGN §7). Neither knows the other.
const adapters = [
  new PointerInput(board, game, submitMove),
  new GamepadInput(board, game, submitMove),
];

function onStatus(s) {
  if (s === "open") {
    // Claim a seat. After a transient drop we re-ask for the *same* seat by name,
    // so the server hands back the seat it held for us (reconnect, DESIGN §13 #1);
    // on a first connect mySeat is null and the server auto-assigns
    // white/black/spectator.
    net.send({ type: "join", session: "default", seat: game.mySeat || "" });
  } else if (s === "closed") {
    statusEl.textContent = "disconnected — retrying…";
    statusEl.className = "status bad";
  }
}

function onMessage(msg) {
  if (msg.type === "state") {
    game.position = msg.position;
    game.turn = msg.turn;
    game.legalMoves = msg.legalMoves || [];
    game.status = msg.status;
    game.seats = msg.seats || game.seats;
    game.check = !!msg.check;
    board.renderPosition(game.position);
    paintCheck();
    applyHeatmap();
    for (const a of adapters) a.reset();
    renderStatus();
    renderSeats();
  } else if (msg.type === "reject") {
    const m = msg.move ? `${msg.move.from}→${msg.move.to}` : "";
    flash(`rejected ${m} (${msg.reason})`);
    for (const a of adapters) a.reset();
  } else if (msg.type === "sessionUpdate") {
    game.seats = msg.seats || game.seats;
    renderSeats();
  } else if (msg.type === "joined") {
    game.mySeat = msg.seat === "spectator" ? null : msg.seat;
    renderSeats();
  }
}

// THE single move intake. (Both pointer and gamepad call only this.)
function submitMove(move) {
  const isPromotion = game.legalMoves.some(
    (m) => m.from === move.from && m.to === move.to && m.promotion);
  if (isPromotion && !move.promotion) {
    choosePromotion((piece) => sendMove(move, piece));
    return;
  }
  sendMove(move, move.promotion || null);
}

function sendMove(move, promotion) {
  net.send({
    type: "proposeMove",
    session: "default",
    seat: game.mySeat || game.turn,
    move: { from: move.from, to: move.to, promotion: promotion || null },
  });
}

// ---- Promotion chooser ------------------------------------------------------
// Same sprites as the board: the promoting side (game.turn) picks its army, so
// White sees white-*.png and the Red Queen's side red-*.png.
function choosePromotion(done) {
  const army = game.turn === "white" ? "white" : "red";
  const row = promoEl.querySelector(".promo-row");
  row.innerHTML = "";
  promoEl.classList.remove("hidden");

  const finish = (piece) => {
    promoEl.classList.add("hidden");
    document.removeEventListener("keydown", onKey);
    if (piece) done(piece);
  };
  for (const piece of ["queen", "rook", "bishop", "knight"]) {
    const b = document.createElement("button");
    b.title = piece;
    b.setAttribute("aria-label", piece);    // button carries the label…
    const img = document.createElement("img");
    img.src = `/pieces/${army}-${piece}.png`;
    img.alt = "";                            // …so the sprite is presentational (no double announce)
    img.draggable = false;
    b.appendChild(img);
    b.addEventListener("click", () => finish(piece));
    row.appendChild(b);
  }
  const onKey = (e) => {
    const map = { q: "queen", r: "rook", b: "bishop", n: "knight",
                  Enter: "queen", Escape: null };
    if (e.key in map) { e.preventDefault(); finish(map[e.key]); }
  };
  document.addEventListener("keydown", onKey);
}

// ---- Status / seats UI ------------------------------------------------------
function renderStatus() {
  let text, cls = "status";
  const mover = game.turn === "white" ? "White" : "Black";
  const winner = game.turn === "white" ? "Black" : "White";
  if (game.status === "checkmate") { text = `Checkmate — ${winner} wins`; cls += " win"; }
  else if (game.status === "stalemate") { text = "Stalemate — draw"; cls += " win"; }
  else if (game.status === "draw") { text = "Draw"; cls += " win"; }
  else { text = `${mover} to move${game.check ? " — check" : ""}`; if (game.check) cls += " bad"; }
  statusEl.textContent = text;
  statusEl.className = cls;
}

function renderSeats() {
  seatsEl.innerHTML = "";
  for (const seat of ["white", "black"]) {
    const status = game.seats[seat] || "open";
    const el = document.createElement("div");
    el.className = "seat " + status + (game.mySeat === seat ? " mine" : "");
    el.innerHTML = `<span class="dot"></span>${seat}: ${status}`;
    seatsEl.appendChild(el);
  }
}

// Paint the in-check king (status channel; independent of input channels).
function paintCheck() {
  if (!game.check) { board.highlight("status", []); return; }
  const king = findKing(game.position, game.turn);
  board.highlight("status", king ? [{ sq: king, cls: "check" }] : []);
}

function findKing(fen, color) {
  const target = color === "white" ? "K" : "k";
  const placement = fen.split(" ")[0];
  let rank = 7, file = 0;
  for (const ch of placement) {
    if (ch === "/") { rank--; file = 0; }
    else if (ch >= "1" && ch <= "8") file += +ch;
    else { if (ch === target) return squareName(file, rank); file++; }
  }
  return null;
}

// ---- Attack heatmap HUD -----------------------------------------------------
// A view-only overlay (DESIGN keeps the engine pure; this derives everything
// from the FEN we already render). Two independent toggles — tint the board by
// who controls each square, and show the raw white/black attacker counts —
// persisted so a player's preference survives a reload/reconnect.
const heatBtn = document.getElementById("heatmap-toggle");
const countsBtn = document.getElementById("counts-toggle");
const legendEl = document.getElementById("legend");

const heat = {
  on: loadPref("villen.heatmap", false),
  counts: loadPref("villen.heatmapCounts", false),
};

function applyHeatmap() {
  if (heat.on) board.setHeatmap(heatmapFor(game.position), { showCounts: heat.counts });
  else board.clearHeatmap();
}

function syncHeatHud() {
  heatBtn.setAttribute("aria-pressed", String(heat.on));
  countsBtn.setAttribute("aria-pressed", String(heat.counts));
  countsBtn.hidden = !heat.on;
  legendEl.hidden = !heat.on;
}

// Swatches drawn with the very same palette function the board uses, so the
// legend can never drift from what's painted. (2, 0) / (0, 2) / (2, 2) / (0, 0)
// sample each state mid-ramp.
function buildLegend() {
  const items = [
    [squareColor(2, 0), "White controls"],
    [squareColor(0, 2), "Red controls"],
    [squareColor(2, 2), "Contested"],
    [squareColor(0, 0), "Unattacked"],
  ];
  legendEl.innerHTML = "";
  for (const [color, label] of items) {
    const item = document.createElement("span");
    item.className = "legend-item";
    item.innerHTML = `<span class="legend-swatch" style="background:${color}"></span>${label}`;
    legendEl.appendChild(item);
  }
}

heatBtn.addEventListener("click", () => {
  heat.on = !heat.on;
  savePref("villen.heatmap", heat.on);
  syncHeatHud();
  applyHeatmap();
});
countsBtn.addEventListener("click", () => {
  heat.counts = !heat.counts;
  savePref("villen.heatmapCounts", heat.counts);
  syncHeatHud();
  applyHeatmap();
});

function loadPref(key, fallback) {
  try { const v = localStorage.getItem(key); return v === null ? fallback : v === "true"; }
  catch { return fallback; }
}
function savePref(key, value) {
  try { localStorage.setItem(key, String(value)); } catch { /* ignore */ }
}

buildLegend();
syncHeatHud();
applyHeatmap();

let flashTimer = null;
function flash(text) {
  statusEl.textContent = text;
  statusEl.className = "status bad";
  clearTimeout(flashTimer);
  flashTimer = setTimeout(renderStatus, 1600);
}

// Expose a tiny hook so later input adapters (gamepad, step 4) can register
// without app.js needing to know their internals up front.
window.villen = { board, game, submitMove, adapters };
