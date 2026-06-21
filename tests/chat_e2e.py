#!/usr/bin/env python3
"""Villen chat engine end-to-end harness (DESIGN-chat.md §18).

Drives the REAL host binary in spawn mode against the LLM-free fake llama-server
(spike/chat-bench/fake_llama_server.py) over a player WebSocket, exercising the
§3.A transport path the C++ unit tests can't reach:

  1. transport   — spawn llama-server, pass the /health gate, then chatSend streams
                   chatDelta tokens and ends with chatDone.
  2. malformed   — junk / mistyped / incomplete client frames are answered with
                   chatError "bad_message" and never crash the single-thread host.
  3. crash-reset — SIGKILL the llama child; the host reaps it, respawns it (§3.A
                   crash isolation), and a fresh chatSend works again.

No model, no GPU — it uses the fake server, so it runs in CI (the "CI stays
LLM-free" discipline, §13). Pure Python stdlib (no `websockets`/`requests`).

Run standalone (after a host build):

    tests/chat_e2e.py --villen build/host/villen \\
                      --fake-llama spike/chat-bench/fake_llama_server.py

Exit code 0 = all scenarios passed; non-zero on the first failure (with the host's
stderr tail printed for triage). CMake wires this as the `chat_e2e` ctest.
"""

import argparse
import base64
import hashlib
import json
import os
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import time

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"  # RFC 6455, mirrors ws_server.cpp

# --- a minimal RFC 6455 client (the browser player speaks the same wire) -------


class WsClient:
    """Just enough WebSocket to talk to villen's WsServer: standard handshake,
    masked client text frames (the server drops unmasked ones), unmasked server
    frames, ping/pong, close. Buffers raw bytes and parses whole frames."""

    def __init__(self, host, port, timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.buf = b""
        self._handshake(host, port)

    def _handshake(self, host, port):
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            "GET / HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n"
        )
        self.sock.sendall(req.encode())
        # Read the response headers (terminated by a blank line).
        while b"\r\n\r\n" not in self.buf:
            self._fill()
        head, _, rest = self.buf.partition(b"\r\n\r\n")
        self.buf = rest
        status = head.split(b"\r\n", 1)[0]
        if b"101" not in status:
            raise RuntimeError(f"handshake failed: {status!r}")
        expect = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        accept = None
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"sec-websocket-accept:"):
                accept = line.split(b":", 1)[1].strip().decode()  # value is case-sensitive base64
                break
        if accept != expect:
            raise RuntimeError(f"handshake Sec-WebSocket-Accept mismatch (got {accept!r})")

    def _fill(self):
        chunk = self.sock.recv(4096)
        if not chunk:
            raise ConnectionError("server closed the connection")
        self.buf += chunk

    def send(self, obj):
        payload = json.dumps(obj).encode()
        n = len(payload)
        header = bytearray([0x81])  # FIN + text opcode
        if n < 126:
            header.append(0x80 | n)
        elif n <= 0xFFFF:
            header.append(0x80 | 126)
            header += struct.pack(">H", n)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", n)
        mask = os.urandom(4)
        header += mask
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def _next_frame(self):
        """Return (opcode, payload_bytes) for one complete frame, reading as needed."""
        while True:
            while len(self.buf) < 2:
                self._fill()
            b0, b1 = self.buf[0], self.buf[1]
            opcode = b0 & 0x0F
            length = b1 & 0x7F
            off = 2
            if length == 126:
                while len(self.buf) < 4:
                    self._fill()
                length = struct.unpack(">H", self.buf[2:4])[0]
                off = 4
            elif length == 127:
                while len(self.buf) < 10:
                    self._fill()
                length = struct.unpack(">Q", self.buf[2:10])[0]
                off = 10
            masked = b1 & 0x80
            mask = b""
            if masked:  # servers don't mask, but be correct
                while len(self.buf) < off + 4:
                    self._fill()
                mask = self.buf[off:off + 4]
                off += 4
            while len(self.buf) < off + length:
                self._fill()
            payload = self.buf[off:off + length]
            self.buf = self.buf[off + length:]
            if masked:
                payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
            return opcode, payload

    def recv_json(self, deadline):
        """Next text message as a parsed JSON object, honoring an absolute deadline.
        Replies to pings; raises TimeoutError past the deadline."""
        fragment = b""
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("no message before deadline")
            self.sock.settimeout(remaining)
            opcode, payload = self._next_frame()
            if opcode == 0x9:  # ping -> pong
                self._send_control(0xA, payload)
                continue
            if opcode == 0xA:  # pong
                continue
            if opcode == 0x8:  # close
                raise ConnectionError("server sent close")
            if opcode in (0x1, 0x0):  # text or continuation
                fragment += payload
                # WsServer sends each message as a single FIN frame; if it ever
                # fragments, opcode 0x0 continues until we have valid JSON.
                try:
                    return json.loads(fragment.decode())
                except (json.JSONDecodeError, UnicodeDecodeError):
                    continue
            # binary (0x2) isn't part of the player protocol; skip it.

    def _send_control(self, opcode, payload):
        header = bytearray([0x80 | opcode, 0x80 | len(payload)])
        mask = os.urandom(4)
        header += mask
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def close(self):
        try:
            self._send_control(0x8, b"")
        except OSError:
            pass
        try:
            self.sock.close()
        except OSError:
            pass


# --- host process management ---------------------------------------------------


def free_port():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


class Host:
    """The villen binary in headless spawn mode, managing the fake llama-server."""

    def __init__(self, villen, fake_llama):
        self.villen = villen
        self.fake_llama = fake_llama
        self.ws_port = free_port()
        self.llama_port = free_port()
        self.proc = None
        self.stderr_path = None

    def start(self):
        # execvp runs the fake server via its shebang, so it must stay executable
        # across a fresh checkout (git tracks the bit, but make it robust).
        try:
            os.chmod(self.fake_llama, 0o755)
        except OSError:
            pass
        # Host stdout+stderr to a temp file (not the build/source tree); tailed on
        # failure for triage, removed in stop().
        self.stderr_path = os.path.join(tempfile.gettempdir(), f"villen_chat_e2e_{self.ws_port}.log")
        self._log = open(self.stderr_path, "w+b")
        args = [
            self.villen,
            "--engine", "chat",
            "--headless",
            "--port", str(self.ws_port),
            "--llama-bin", self.fake_llama,
            "--llama-url", f"127.0.0.1:{self.llama_port}",
            "--model", "/dev/null",  # fake server ignores -m; satisfies the spawn path
            "--llama-parallel", "2",
        ]
        self.proc = subprocess.Popen(args, stdout=self._log, stderr=self._log)
        self._wait_ws_listening()

    def _wait_ws_listening(self, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"host exited early (code {self.proc.returncode})")
            try:
                socket.create_connection(("127.0.0.1", self.ws_port), 0.5).close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("host never accepted connections")

    def connect(self, timeout=10.0):
        return WsClient("127.0.0.1", self.ws_port, timeout=timeout)

    def llama_child_pids(self):
        """PIDs of the spawned fake llama-server: the direct children of the villen
        host (the only process it forks in this config). /proc/<pid>/stat field 4 is
        the ppid — comm (field 2) may contain spaces/parens, so read past the last
        ')'. Linux-only, like the host (CI + the Deck are Linux)."""
        if not self.proc:
            return []
        parent = self.proc.pid
        pids = []
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                with open(f"/proc/{entry}/stat") as f:
                    fields = f.read().rsplit(")", 1)[1].split()
                ppid = int(fields[1])  # [0]=state, [1]=ppid
            except (OSError, IndexError, ValueError):
                continue
            if ppid == parent:
                pids.append(int(entry))
        return pids

    def stderr_tail(self, n=40):
        if not self.stderr_path or not os.path.exists(self.stderr_path):
            return "(no host log)"
        try:
            with open(self.stderr_path, "r", errors="replace") as f:
                return "".join(f.readlines()[-n:])
        except OSError:
            return "(host log unreadable)"

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
        if getattr(self, "_log", None):
            self._log.close()
        if self.stderr_path and os.path.exists(self.stderr_path):
            try:
                os.remove(self.stderr_path)
            except OSError:
                pass


# --- scenarios -----------------------------------------------------------------


def recv_until(ws, types, budget):
    """Read frames until one whose "type" is in `types`, skipping the rest (e.g. the
    connect-time engineAnnounce, joined, or unrelated chatConfig broadcasts)."""
    deadline = time.monotonic() + budget
    while True:
        msg = ws.recv_json(deadline)
        if msg.get("type") in types:
            return msg


def wait_ready(ws, budget=25.0):
    """Join (claim spectator membership) and read until the engine reports the
    backend is up. join's chatConfig reflects live readiness; if the spawned model
    is still loading, the onTick false→true transition broadcasts another."""
    deadline = time.monotonic() + budget
    ws.send({"type": "join", "session": "default", "seat": ""})
    while True:
        msg = ws.recv_json(deadline)
        if msg.get("type") == "chatConfig" and msg.get("ready"):
            return msg


def collect_reply(ws, conv_id, text, budget=30.0):
    """Send one user turn and drain the stream. Returns (deltas, done_or_error)."""
    ws.send({"type": "chatSend", "convId": conv_id, "text": text})
    deltas, terminal = [], None
    deadline = time.monotonic() + budget
    while terminal is None:
        msg = ws.recv_json(deadline)
        kind = msg.get("type")
        if kind == "chatDelta" and msg.get("convId") == conv_id:
            deltas.append(msg.get("delta", ""))
        elif kind in ("chatDone", "chatError") and msg.get("convId") == conv_id:
            terminal = msg
    return deltas, terminal


def scenario_transport(host):
    ws = host.connect()
    try:
        wait_ready(ws)  # spawn + /health gate before the first turn
        deltas, terminal = collect_reply(ws, "c1", "hello there")
        assert terminal["type"] == "chatDone", f"expected chatDone, got {terminal}"
        assert deltas, "stream produced no chatDelta tokens"
        assert "".join(deltas).strip(), "concatenated reply was empty"
        assert terminal.get("tokens", 0) > 0, f"chatDone reported 0 tokens: {terminal}"
        print(f"  transport: {len(deltas)} deltas, "
              f"stopReason={terminal.get('stopReason')}, tokens={terminal.get('tokens')}")
    finally:
        ws.close()


def scenario_malformed(host):
    ws = host.connect()
    try:
        # Each malformed frame must draw a chatError "bad_message" — and crucially
        # must not crash the single-thread host (the next frame still gets served).
        for bad in (
            "this is not json",                   # not parseable
            {"type": 123},                        # mistyped "type"
            {"type": "chatSend"},                 # missing convId/text
            {"type": "chatSend", "convId": "x"},  # missing text
        ):
            if isinstance(bad, str):
                _send_text_raw(ws, bad)
            else:
                ws.send(bad)
            # Skip the connect-time engineAnnounce/joined frames; the bad frame's
            # only effect is a single chatError.
            msg = recv_until(ws, {"chatError"}, 10.0)
            assert msg.get("reason") == "bad_message", f"unexpected reason for {bad!r}: {msg}"
        # The host is still alive and well-behaved: a valid turn still streams.
        wait_ready(ws)
        _, terminal = collect_reply(ws, "c2", "still alive?")
        assert terminal["type"] == "chatDone", f"host unhealthy after malformed input: {terminal}"
        print("  malformed: 4 bad frames rejected with bad_message, host still serving")
    finally:
        ws.close()


def scenario_crash_restart(host):
    ws = host.connect()
    try:
        # Make sure a model is up and a baseline turn works.
        wait_ready(ws)
        _, terminal = collect_reply(ws, "pre", "warm up")
        assert terminal["type"] == "chatDone", f"baseline turn failed: {terminal}"

        # Kill the llama child out from under the host (a crash, §3.A).
        pids = host.llama_child_pids()
        assert pids, "could not find the spawned fake llama-server to kill"
        for pid in pids:
            os.kill(pid, signal.SIGKILL)
        print(f"  crash-restart: SIGKILLed llama child pid(s) {pids}")

        # The host reaps the dead child (WNOHANG), backs off, respawns, and the new
        # one passes /health. Poll a fresh turn until the backend recovers.
        recovered = False
        deadline = time.monotonic() + 60.0
        attempt = 0
        while not recovered and time.monotonic() < deadline:
            attempt += 1
            # backend_down returns a chatError immediately; a recovered backend
            # streams a full reply (~10s here), so the per-turn budget must cover it.
            _, terminal = collect_reply(ws, f"post{attempt}", "are you back?", budget=25.0)
            if terminal["type"] == "chatDone":
                recovered = True
            else:
                # backend_down while the replacement loads — expected; retry.
                time.sleep(1.0)
        assert recovered, "host did not recover a working backend after the crash"
        new_pids = host.llama_child_pids()
        print(f"  crash-restart: recovered on attempt {attempt}; new child pid(s) {new_pids}")
    finally:
        ws.close()


# Small helpers used by scenario_malformed -------------------------------------


def _send_text_raw(ws, text):
    """Send an arbitrary (non-JSON) text frame, masked, bypassing json.dumps."""
    payload = text.encode()
    n = len(payload)
    header = bytearray([0x81])
    if n < 126:
        header.append(0x80 | n)
    elif n <= 0xFFFF:
        header.append(0x80 | 126)
        header += struct.pack(">H", n)
    else:
        header.append(0x80 | 127)
        header += struct.pack(">Q", n)
    mask = os.urandom(4)
    header += mask
    masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    ws.sock.sendall(bytes(header) + masked)


# --- main ----------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--villen", required=True, help="path to the built villen host binary")
    ap.add_argument("--fake-llama", required=True,
                    help="path to spike/chat-bench/fake_llama_server.py")
    args = ap.parse_args()

    if sys.platform != "linux":
        print(f"chat_e2e: skipping on {sys.platform} (Linux-only: /proc, fork/exec)")
        return 0
    for path, what in ((args.villen, "host binary"), (args.fake_llama, "fake llama-server")):
        if not os.path.exists(path):
            print(f"chat_e2e: {what} not found: {path}", file=sys.stderr)
            return 2

    host = Host(args.villen, args.fake_llama)
    scenarios = [
        ("transport", scenario_transport),
        ("malformed", scenario_malformed),
        ("crash-restart", scenario_crash_restart),
    ]
    try:
        host.start()
        print(f"chat_e2e: host up on ws :{host.ws_port}, llama :{host.llama_port}")
        for name, fn in scenarios:
            print(f"[{name}]")
            fn(host)
            print(f"[{name}] PASS")
    except Exception as exc:  # noqa: BLE001 — top-level harness reporter
        print(f"\nchat_e2e: FAIL — {type(exc).__name__}: {exc}", file=sys.stderr)
        print("\n--- host stderr tail ---", file=sys.stderr)
        print(host.stderr_tail(), file=sys.stderr)
        return 1
    finally:
        host.stop()
    print("\nchat_e2e: all scenarios passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
