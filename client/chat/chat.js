// Villen chat view (DESIGN-chat §10). Renders the streaming reply: on each
// chatDelta append to the current assistant bubble; on chatDone finalize (with
// tok/s). Server owns the model + params; the client only sends prompts (§7).
// Reuses the host's text WS transport unchanged (§10).
import { connect } from "../src/net.js";

const convId = "c1"; // one conversation in the MVP client (§10)

const log = document.getElementById("log");
const input = document.getElementById("input");
const sendBtn = document.getElementById("send");
const stopBtn = document.getElementById("stop");
const newBtn = document.getElementById("new");
const modelEl = document.getElementById("model");
const statusEl = document.getElementById("status");

let assistantEl = null; // the bubble currently streaming, if any
let generating = false;

const net = connect(onMessage, onStatus);

function onStatus(s) {
  if (s === "open") {
    setStatus("connected", "ok");
    // Join to register the connection so the engine pushes chatConfig (§7).
    net.send({ type: "join", session: "default", seat: "" });
  } else {
    setStatus("disconnected — retrying…", "bad");
  }
}

function onMessage(msg) {
  switch (msg.type) {
    case "engine": // which engine is active (admin-shell §5)
      if (msg.name && msg.name !== "chat")
        setStatus(`active engine is "${msg.name}", not chat`, "bad");
      break;
    case "chatConfig":
      modelEl.textContent = msg.model || "(unknown model)";
      break;
    case "chatDelta":
      if (msg.convId === convId) appendDelta(msg.delta || "");
      break;
    case "chatDone":
      if (msg.convId === convId) finishAssistant(msg);
      break;
    case "chatError":
      addBubble("error", `⚠ ${msg.reason}`);
      endGenerating();
      break;
  }
}

function send() {
  const text = input.value.trim();
  if (!text || generating) return;
  addBubble("user", text);
  input.value = "";
  autosize();
  assistantEl = addBubble("assistant streaming", "");
  generating = true;
  setControls();
  net.send({ type: "chatSend", convId, text });
}

function appendDelta(delta) {
  if (!assistantEl) assistantEl = addBubble("assistant streaming", "");
  assistantEl.firstChild.nodeValue += delta;
  scrollDown();
}

function finishAssistant(msg) {
  if (assistantEl) {
    assistantEl.classList.remove("streaming");
    const m = document.createElement("div");
    m.className = "tokmeta";
    const tps = typeof msg.tps === "number" ? msg.tps.toFixed(1) : "?";
    m.textContent = `${msg.tokens ?? "?"} tok · ${tps} tok/s · ${msg.stopReason}`;
    assistantEl.appendChild(m);
  }
  endGenerating();
}

function endGenerating() {
  assistantEl = null;
  generating = false;
  setControls();
}

function addBubble(cls, text) {
  const el = document.createElement("div");
  el.className = `bubble ${cls}`;
  el.appendChild(document.createTextNode(text)); // text node stays firstChild
  log.appendChild(el);
  scrollDown();
  return el;
}

function setControls() {
  sendBtn.disabled = generating;
  stopBtn.disabled = !generating;
}

function setStatus(text, cls) {
  statusEl.textContent = text;
  statusEl.className = `status ${cls || ""}`;
}

function scrollDown() {
  log.scrollTop = log.scrollHeight;
}

function autosize() {
  input.style.height = "auto";
  input.style.height = Math.min(input.scrollHeight, 160) + "px";
}

sendBtn.onclick = send;
stopBtn.onclick = () => {
  if (generating) net.send({ type: "chatStop", convId });
};
newBtn.onclick = () => {
  net.send({ type: "chatReset", convId });
  log.replaceChildren();
  endGenerating();
};
input.addEventListener("input", autosize);
input.addEventListener("keydown", (e) => {
  if (e.key === "Enter" && !e.shiftKey) {
    e.preventDefault();
    send();
  }
});

setControls();
