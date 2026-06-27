// Villen — the `snake` browser client (DESIGN-snake §8).
//
// A dumb renderer + intent terminal: it paints the host's authoritative grid from
// each `state` broadcast and sends only a direction (never a position — the server
// owns the world, §5). All sources of input — arrow keys / WASD, an on-screen
// D-pad, a touch swipe, and the Gamepad API — funnel into one submitInput(dir)
// (DESIGN-snake §6/§8). The transport idiom mirrors net.js/filter.js: a plain
// text WebSocket back to the page's own origin, auto-reconnecting.
//
// Smoothness: the world ticks at ~10 Hz but we render at ~60; snakes slide one
// cell between the two latest states (sim rate != render rate, §4), snapping over
// a wrap so a snake never streaks across the board.

const statusEl = document.getElementById("status");
const scoresEl = document.getElementById("scores");
const canvas = document.getElementById("board");
const ctx = canvas.getContext("2d");

// Server-authoritative config (DESIGN-snake §5); defaults until `config` arrives.
let config = { w: 32, h: 20, tickMs: 100, wrap: true };
let myId = -1;            // our snake id (== seat index), -1 while spectating
let myColor = "#e8edf2";

// The two latest snapshots, for one-cell interpolation between ticks (§4/§8).
let prev = null;
let cur = null;
let curAt = 0;            // performance.now() when `cur` arrived

let ws = null;
let lastSent = null;      // throttle: don't resend the same direction repeatedly

function setStatus(s) { statusEl.textContent = s; }

// --- transport ---------------------------------------------------------------

function connect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/`);
  ws.onopen = () => {
    setStatus("joining…");
    lastSent = null;  // fresh socket: don't let a stale throttle swallow the first input
    ws.send(JSON.stringify({ type: "join" }));  // auto-assign a seat (a snake)
  };
  ws.onclose = () => {
    setStatus("disconnected — retrying…");
    setTimeout(connect, 1000);  // simple reconnect (DESIGN defers polish)
  };
  ws.onmessage = (e) => {
    let msg;
    try { msg = JSON.parse(e.data); } catch { return; }
    onMessage(msg);
  };
}

function onMessage(msg) {
  switch (msg.type) {
    case "config":
      config.w = msg.w ?? config.w;
      config.h = msg.h ?? config.h;
      config.tickMs = msg.tickMs ?? config.tickMs;
      config.wrap = msg.wrap ?? config.wrap;
      resizeCanvas();
      break;
    case "you":
      myId = msg.id ?? -1;
      if (msg.color) myColor = msg.color;
      setStatus(myId >= 0 ? "you're in — go!" : "spectating (arena full)");
      break;
    case "state":
      prev = cur;
      cur = msg;
      curAt = performance.now();
      renderScores(msg);
      break;
    // "engine" / "joined" / "sessionUpdate" envelopes need no action here.
  }
}

function submitInput(dir) {
  if (!dir || myId < 0) return;        // spectators send nothing (§5)
  if (dir === lastSent) return;        // collapse repeats; server keeps the latest
  lastSent = dir;
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify({ type: "input", dir }));
  }
}

// --- layout ------------------------------------------------------------------

let cell = 20;

function resizeCanvas() {
  // Fit the grid into the viewport while keeping square cells (zoom-independent,
  // the §11 rationale for sending state not pixels).
  const maxW = Math.min(window.innerWidth * 0.96, 1100);
  const maxH = window.innerHeight * 0.66;
  cell = Math.max(6, Math.floor(Math.min(maxW / config.w, maxH / config.h)));
  canvas.width = config.w * cell;
  canvas.height = config.h * cell;
}
window.addEventListener("resize", resizeCanvas);

// --- render ------------------------------------------------------------------

// Interpolate a cell from its previous position to its current one, snapping when
// the step wrapped an edge (so the snake never slides the long way across).
function lerpCell(p, c, t) {
  const dx = c[0] - p[0];
  const dy = c[1] - p[1];
  if (Math.abs(dx) > 1 || Math.abs(dy) > 1) return [c[0], c[1]];  // wrapped: snap
  return [p[0] + dx * t, p[1] + dy * t];
}

function prevBodyOf(id) {
  if (!prev || !prev.snakes) return null;
  const s = prev.snakes.find((s) => s.id === id);
  return s ? s.cells : null;
}

function roundRect(x, y, w, h, r) {
  ctx.beginPath();
  ctx.roundRect(x + 1, y + 1, w - 2, h - 2, r);
  ctx.fill();
}

function draw() {
  requestAnimationFrame(draw);
  pollGamepad();

  ctx.fillStyle = "#0a0c11";
  ctx.fillRect(0, 0, canvas.width, canvas.height);
  if (!cur) return;

  // subtle grid dots so the arena reads as a board
  ctx.fillStyle = "#11151c";
  for (let y = 0; y < config.h; y++) {
    for (let x = 0; x < config.w; x++) {
      ctx.fillRect(x * cell + cell / 2 - 1, y * cell + cell / 2 - 1, 2, 2);
    }
  }

  // food
  for (const f of cur.food || []) {
    ctx.fillStyle = "#ff5d5d";
    ctx.beginPath();
    ctx.arc(f[0] * cell + cell / 2, f[1] * cell + cell / 2, cell * 0.32, 0, Math.PI * 2);
    ctx.fill();
  }

  const t = Math.min(1, (performance.now() - curAt) / config.tickMs);

  for (const s of cur.snakes || []) {
    const pcells = prevBodyOf(s.id);
    const r = Math.max(2, cell * 0.28);
    for (let i = 0; i < s.cells.length; i++) {
      const c = s.cells[i];
      const p = pcells && pcells[i] ? pcells[i] : c;  // grew/new -> snap
      const [ix, iy] = lerpCell(p, c, t);
      // Head is full colour; body fades back; your snake gets a bright ring.
      ctx.fillStyle = i === 0 ? s.color : shade(s.color, 0.62);
      roundRect(ix * cell, iy * cell, cell, cell, r);
      if (i === 0) {
        if (s.id === myId) {
          ctx.strokeStyle = "#ffffff";
          ctx.lineWidth = Math.max(2, cell * 0.12);
          ctx.beginPath();
          ctx.roundRect(ix * cell + 1, iy * cell + 1, cell - 2, cell - 2, r);
          ctx.stroke();
        }
        drawEyes(ix, iy, s.dir);
      }
    }
  }
}

function drawEyes(ix, iy, dir) {
  const cx = ix * cell + cell / 2;
  const cy = iy * cell + cell / 2;
  const off = cell * 0.18;
  const fwd = { up: [0, -1], down: [0, 1], left: [-1, 0], right: [1, 0] }[dir] || [0, 0];
  // two eyes, nudged toward the travel direction
  const perp = [fwd[1], fwd[0]];
  ctx.fillStyle = "#0a0c11";
  for (const s of [-1, 1]) {
    const ex = cx + fwd[0] * off + perp[0] * off * s;
    const ey = cy + fwd[1] * off + perp[1] * off * s;
    ctx.beginPath();
    ctx.arc(ex, ey, Math.max(1.4, cell * 0.09), 0, Math.PI * 2);
    ctx.fill();
  }
}

// Darken a #rrggbb toward black by factor k (1 = unchanged, 0 = black).
function shade(hex, k) {
  const n = parseInt(hex.slice(1), 16);
  const r = Math.round(((n >> 16) & 255) * k);
  const g = Math.round(((n >> 8) & 255) * k);
  const b = Math.round((n & 255) * k);
  return `rgb(${r},${g},${b})`;
}

function renderScores(state) {
  const sorted = [...(state.snakes || [])].sort((a, b) => b.score - a.score);
  scoresEl.innerHTML = "";
  for (const s of sorted) {
    const el = document.createElement("span");
    el.className = "score" + (s.id === myId ? " me" : "");
    const sw = document.createElement("span");
    sw.className = "swatch";
    sw.style.background = s.color;
    el.appendChild(sw);
    el.appendChild(document.createTextNode(`${s.name} ${s.score}`));
    scoresEl.appendChild(el);
  }
}

// --- input: keys / D-pad / swipe / gamepad, all into submitInput -------------

const KEYS = {
  ArrowUp: "up", ArrowDown: "down", ArrowLeft: "left", ArrowRight: "right",
  w: "up", s: "down", a: "left", d: "right",
  W: "up", S: "down", A: "left", D: "right",
};
window.addEventListener("keydown", (e) => {
  const dir = KEYS[e.key];
  if (dir) {
    e.preventDefault();
    submitInput(dir);
  }
});

for (const btn of document.querySelectorAll(".dpad button")) {
  const fire = (e) => { e.preventDefault(); submitInput(btn.dataset.dir); };
  btn.addEventListener("pointerdown", fire);
}

// Touch swipe on the board.
let touchStart = null;
canvas.addEventListener("touchstart", (e) => {
  const t = e.changedTouches[0];
  touchStart = { x: t.clientX, y: t.clientY };
}, { passive: true });
canvas.addEventListener("touchend", (e) => {
  if (!touchStart) return;
  const t = e.changedTouches[0];
  const dx = t.clientX - touchStart.x;
  const dy = t.clientY - touchStart.y;
  touchStart = null;
  if (Math.abs(dx) < 18 && Math.abs(dy) < 18) return;  // a tap, not a swipe
  if (Math.abs(dx) > Math.abs(dy)) submitInput(dx > 0 ? "right" : "left");
  else submitInput(dy > 0 ? "down" : "up");
}, { passive: true });

// Gamepad API: D-pad buttons + left stick, polled each frame and debounced so a
// held stick doesn't spam (DESIGN-snake §6, reusing the chess gamepad idiom).
let lastPadDir = null;
function pollGamepad() {
  const pads = navigator.getGamepads ? navigator.getGamepads() : [];
  let dir = null;
  for (const p of pads) {
    if (!p) continue;
    const b = p.buttons;
    if (b[12] && b[12].pressed) dir = "up";
    else if (b[13] && b[13].pressed) dir = "down";
    else if (b[14] && b[14].pressed) dir = "left";
    else if (b[15] && b[15].pressed) dir = "right";
    const ax = p.axes[0] || 0, ay = p.axes[1] || 0;
    if (!dir && Math.max(Math.abs(ax), Math.abs(ay)) > 0.5) {
      dir = Math.abs(ax) > Math.abs(ay) ? (ax > 0 ? "right" : "left")
                                        : (ay > 0 ? "down" : "up");
    }
    if (dir) break;
  }
  if (dir && dir !== lastPadDir) submitInput(dir);
  lastPadDir = dir;  // edge-triggered: only fire on change
}

// --- boot --------------------------------------------------------------------

resizeCanvas();
connect();
requestAnimationFrame(draw);
