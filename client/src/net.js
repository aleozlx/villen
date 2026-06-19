// WebSocket transport — the player edge of the §6 contract. Connects back to the
// same origin that served this page (so opening http://host:port "just works"),
// parses incoming JSON, and auto-reconnects. It knows the wire format but nothing
// about chess or the UI.

export function connect(onMessage, onStatus) {
  let ws;
  let closed = false;

  function open() {
    const proto = location.protocol === "https:" ? "wss" : "ws";
    ws = new WebSocket(`${proto}://${location.host}/`);
    ws.onopen = () => onStatus && onStatus("open");
    ws.onclose = () => {
      onStatus && onStatus("closed");
      if (!closed) setTimeout(open, 1000);  // simple reconnect (DESIGN defers polish)
    };
    ws.onmessage = (e) => {
      let msg;
      try { msg = JSON.parse(e.data); } catch { return; }
      onMessage(msg);
    };
  }
  open();

  return {
    send(obj) {
      if (ws && ws.readyState === WebSocket.OPEN) ws.send(JSON.stringify(obj));
    },
    close() { closed = true; ws && ws.close(); },
  };
}
