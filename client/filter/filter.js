// Villen — the `filter` browser client (DESIGN-filter §9).
//
// A dumb capture/display terminal: it streams downscaled JPEG camera frames to
// the host and paints the host's *processed* reply. All the compute is the
// host's APU (§15) — this file does no image processing. The transport idiom
// mirrors net.js, with a binary path added: text frames are control JSON
// (filterConfig), binary frames are media (§5.2).

const statusEl = document.getElementById("status");
const noteEl = document.getElementById("note");
const statsEl = document.getElementById("stats");
const outCanvas = document.getElementById("out");
const outCtx = outCanvas.getContext("2d");
const selfVideo = document.getElementById("self");

// Server-authoritative capture parameters (§5.2); defaults until filterConfig.
let config = { outW: 320, outH: 240, quality: 70, fps: 15 };

// Offscreen capture canvas, sized to the config'd output (downscale is free).
const cap = document.createElement("canvas");
const capCtx = cap.getContext("2d", { willReadFrequently: true });

let ws = null;
let seq = 0;            // our monotonically increasing frame counter (§5.2)
let lastShownSeq = -1;  // newest reply accepted for decode; drop staler ones (§5.3)
let lastDrawnSeq = -1;  // newest reply actually painted; decode is async so two
                        // accepted replies can resolve out of order (§5.3)
let videoEl = null;     // the live <video> camera source, when granted
let useTestPattern = false;
let sending = false;    // client-side drop-to-latest: never queue >1 unsent frame

// round-trip + rate bookkeeping for the on-screen stats line.
const sentAt = new Map();
let rttMs = 0, fpsIn = 0, fpsOut = 0;
let inWin = 0, outWin = 0, winStart = performance.now();

function setStatus(s) { statusEl.textContent = s; }

// --- transport ---------------------------------------------------------------

function connect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/`);
  ws.binaryType = "arraybuffer";

  ws.onopen = () => {
    setStatus("connected");
    ws.send(JSON.stringify({ type: "join" }));  // a feed joins; server replies config
  };
  ws.onclose = () => { setStatus("disconnected — retrying…"); setTimeout(connect, 1000); };
  ws.onmessage = (e) => {
    if (typeof e.data === "string") onText(e.data);
    else onBinary(e.data);
  };
}

function onText(text) {
  let msg;
  try { msg = JSON.parse(text); } catch { return; }
  if (msg.type === "filterConfig") {
    config.outW = msg.outW ?? config.outW;
    config.outH = msg.outH ?? config.outH;
    config.quality = msg.quality ?? config.quality;
    cap.width = config.outW;
    cap.height = config.outH;
    outCanvas.width = config.outW;
    outCanvas.height = config.outH;
    setStatus(`streaming · ${pipelineSummary(msg.pipeline)}`);
  }
  // "engine"/"joined"/"sessionUpdate" envelopes need no action for a feed.
}

function pipelineSummary(pipeline) {
  if (!Array.isArray(pipeline) || pipeline.length === 0) return "identity";
  return pipeline.map((s) => s.op + (s.se ? `(${s.se}${s.r ?? ""})` : "")).join(" → ");
}

function onBinary(buf) {
  const view = new DataView(buf);
  if (buf.byteLength < 8) return;
  const rseq = view.getUint32(0, true);
  // width/height at offsets 4,6 (uint16 LE) are informational; the JPEG carries
  // its own dimensions.
  if (rseq <= lastShownSeq) return;  // stale / out of order -> drop (§5.3)
  lastShownSeq = rseq;

  const t0 = sentAt.get(rseq);
  if (t0 !== undefined) {
    rttMs = performance.now() - t0;
    // The server drops frames under load (§5.3), so those seqs never get a reply.
    // seqs only grow, so anything <= rseq is now answered or abandoned: clearing
    // up to rseq keeps sentAt from leaking the dropped ones.
    for (const s of sentAt.keys()) if (s <= rseq) sentAt.delete(s);
  }

  const blob = new Blob([new Uint8Array(buf, 8)], { type: "image/jpeg" });
  createImageBitmap(blob).then((bmp) => {
    // Decode is async: a stale frame can resolve after a newer one already painted.
    // Re-check against the last *drawn* seq so we never overwrite newer with older.
    if (rseq <= lastDrawnSeq) { bmp.close(); return; }
    lastDrawnSeq = rseq;
    if (outCanvas.width !== bmp.width) outCanvas.width = bmp.width;
    if (outCanvas.height !== bmp.height) outCanvas.height = bmp.height;
    outCtx.drawImage(bmp, 0, 0, outCanvas.width, outCanvas.height);
    bmp.close();
    outWin++;
  }).catch(() => {});
}

// --- capture + send ----------------------------------------------------------

function drawTestPattern(ctx, w, h, t) {
  // A synthetic source so the pipeline is demoable with no camera (§16). Moving
  // bars + a disc give morphology plenty of edges to chew on.
  ctx.fillStyle = "#000";
  ctx.fillRect(0, 0, w, h);
  for (let i = 0; i < 6; i++) {
    const x = ((t * 0.04 + i * w / 6) % w);
    ctx.fillStyle = `hsl(${(i * 60 + t * 0.05) % 360} 80% 55%)`;
    ctx.fillRect(x, 0, w / 14, h);
  }
  ctx.fillStyle = "#fff";
  ctx.beginPath();
  ctx.arc(w / 2 + Math.cos(t * 0.002) * w * 0.25, h / 2, h * 0.18, 0, Math.PI * 2);
  ctx.fill();
}

function captureFrame(t) {
  const w = cap.width, h = cap.height;
  if (useTestPattern || !videoEl || videoEl.readyState < 2) {
    drawTestPattern(capCtx, w, h, t);
  } else {
    capCtx.drawImage(videoEl, 0, 0, w, h);
  }
}

function sendLoop(t) {
  if (ws && ws.readyState === WebSocket.OPEN && !sending && ws.bufferedAmount < 256 * 1024) {
    captureFrame(t);
    sending = true;
    cap.toBlob((blob) => {
      sending = false;
      if (!blob) return;
      blob.arrayBuffer().then((jpeg) => {
        const out = new Uint8Array(8 + jpeg.byteLength);
        const dv = new DataView(out.buffer);
        const s = (seq = (seq + 1) >>> 0);
        dv.setUint32(0, s, true);
        dv.setUint16(4, cap.width, true);
        dv.setUint16(6, cap.height, true);
        out.set(new Uint8Array(jpeg), 8);
        if (ws.readyState === WebSocket.OPEN) {
          ws.send(out.buffer);
          sentAt.set(s, performance.now());
          inWin++;
        }
      });
    }, "image/jpeg", config.quality / 100);
  }
  tickStats();
  setTimeout(() => requestAnimationFrame(sendLoop), 1000 / config.fps);
}

function tickStats() {
  const now = performance.now();
  if (now - winStart >= 1000) {
    fpsIn = inWin; fpsOut = outWin; inWin = 0; outWin = 0; winStart = now;
  }
  statsEl.textContent =
    `in ${fpsIn} fps · out ${fpsOut} fps · rtt ${rttMs.toFixed(0)} ms · ${config.outW}×${config.outH} q${config.quality}`;
}

// --- camera / sources --------------------------------------------------------

async function startCamera() {
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    // Secure-context gate (§10.2): camera is blocked on plain http://<lan-ip>.
    noteEl.innerHTML =
      "Camera needs a secure context. Open via <b>localhost</b>, enable the " +
      "Chrome insecure-origin flag for this host, or use TLS — meanwhile the " +
      "<b>test pattern</b> works anywhere.";
    useTestPattern = true;
    document.getElementById("testToggle").checked = true;
    return;
  }
  try {
    const stream = await navigator.mediaDevices.getUserMedia({ video: true, audio: false });
    videoEl = document.createElement("video");
    videoEl.autoplay = true; videoEl.playsInline = true; videoEl.muted = true;
    videoEl.srcObject = stream;
    await videoEl.play();
    selfVideo.srcObject = stream;  // wired up but hidden until self-preview is on
  } catch (err) {
    noteEl.textContent = `Camera unavailable (${err.name}); using the test pattern.`;
    useTestPattern = true;
    document.getElementById("testToggle").checked = true;
  }
}

// --- controls ----------------------------------------------------------------

for (const btn of document.querySelectorAll("button[data-preset]")) {
  btn.addEventListener("click", () => {
    if (ws && ws.readyState === WebSocket.OPEN)
      ws.send(JSON.stringify({ type: "requestPreset", preset: btn.dataset.preset }));
  });
}
document.getElementById("selfToggle").addEventListener("change", (e) => {
  selfVideo.style.display = e.target.checked ? "block" : "none";
});
document.getElementById("testToggle").addEventListener("change", (e) => {
  useTestPattern = e.target.checked;
});

// --- boot --------------------------------------------------------------------

connect();
startCamera();
requestAnimationFrame(sendLoop);
