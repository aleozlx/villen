// Player client orchestrator.
//
// Holds the rendered game state, wires the transport to the board, and — most
// importantly — owns `submitMove`, the *single move intake* every input adapter
// funnels into (DESIGN §7). Adapters never talk to the network; they only call
// submitMove, which wraps the move in a `proposeMove`. Promotion is resolved
// here so adapters stay device-dumb.

import { Board, squareName } from "./board.js";
import { connect } from "./net.js";
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
    // Claim a seat (server auto-assigns white, then black, else spectator).
    net.send({ type: "join", session: "default" });
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
const PROMO_GLYPHS = {
  white: { queen: "♕", rook: "♖", bishop: "♗", knight: "♘" },
  black: { queen: "♛", rook: "♜", bishop: "♝", knight: "♞" },
};

function choosePromotion(done) {
  const glyphs = PROMO_GLYPHS[game.turn] || PROMO_GLYPHS.white;
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
    b.textContent = glyphs[piece];
    b.title = piece;
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
