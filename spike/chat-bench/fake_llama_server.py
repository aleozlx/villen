#!/usr/bin/env python3
# Villen — fake llama-server for the Step 7 spike's self-test (NO model, NO GPU).
#
# Mimics just enough of llama-server's surface to exercise bench.py end-to-end on
# a PC — the project's "CI stays LLM-free" discipline (DESIGN-chat.md §13). It:
#   - accepts the host's argv (-m/-ngl/--parallel/-c/...), reading only --host/--port,
#   - prints a fake Vulkan banner to stderr so bench's backend probe has something
#     to classify (clearly marked STUB — this is not a real device),
#   - serves GET /health -> 200,
#   - serves POST /v1/chat/completions (stream) emitting token chunks on a timer,
#     then a final chunk carrying `timings` + `usage`, then `data: [DONE]`,
#   - is threaded, so the --parallel concurrency path actually runs N streams.
#
# It is NOT a model and reports nothing about real throughput — it only proves the
# harness's spawn / health-wait / SSE parse / concurrency / memory-sample / report
# logic is correct before the harness ever touches the Deck.

import json
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HOST = "127.0.0.1"
PORT = 8099
TOK_DELAY_S = 0.02       # fake per-token decode time
PROMPT_EVAL_S = 0.10     # fake prompt-processing delay before first token


def parse_argv(argv):
    host, port, ngl = HOST, PORT, 99
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--host" and i + 1 < len(argv):
            host = argv[i + 1]; i += 2
        elif a == "--port" and i + 1 < len(argv):
            port = int(argv[i + 1]); i += 2
        elif a == "-ngl" and i + 1 < len(argv):
            ngl = int(argv[i + 1]); i += 2
        else:
            i += 1  # ignore -m / --parallel / -c / everything else
    return host, port, ngl


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass  # quiet

    def do_GET(self):
        if self.path in ("/health", "/props"):
            if self.path == "/health":
                body = b'{"status":"ok"}'
            else:  # /props — what bench.py's attach mode reads (model + slots)
                body = json.dumps({
                    "model_path": "/stub/Qwen2.5-7B-Instruct-Q4_K_M.gguf",
                    "total_slots": 1,
                }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def do_POST(self):
        if self.path not in ("/v1/chat/completions", "/completion"):
            self.send_response(404)
            self.end_headers()
            return
        length = int(self.headers.get("Content-Length", "0"))
        try:
            req = json.loads(self.rfile.read(length) or b"{}")
        except json.JSONDecodeError:
            req = {}
        n = int(req.get("max_tokens", req.get("n_predict", 16)))
        n = max(1, min(n, 4096))

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()

        def send(obj):
            self.wfile.write(b"data: " + json.dumps(obj).encode() + b"\n\n")
            self.wfile.flush()

        t0 = time.perf_counter()
        time.sleep(PROMPT_EVAL_S)  # fake prompt eval (drives TTFT)
        prompt_ms = (time.perf_counter() - t0) * 1000.0
        dec0 = time.perf_counter()
        for k in range(n):
            time.sleep(TOK_DELAY_S)
            try:
                send({"choices": [{"index": 0, "finish_reason": None,
                                   "delta": {"content": f"tok{k} "}}]})
            except (BrokenPipeError, ConnectionResetError):
                return
        predicted_ms = (time.perf_counter() - dec0) * 1000.0
        # Final chunk with llama-server-shaped timings + usage, then [DONE].
        send({
            "choices": [{"index": 0, "finish_reason": "length", "delta": {}}],
            "usage": {"prompt_tokens": 12, "completion_tokens": n,
                      "total_tokens": 12 + n},
            "timings": {
                "prompt_n": 12, "prompt_ms": round(prompt_ms, 2),
                "prompt_per_second": round(12 / (prompt_ms / 1000.0), 2) if prompt_ms else None,
                "predicted_n": n, "predicted_ms": round(predicted_ms, 2),
                "predicted_per_second": round(n / (predicted_ms / 1000.0), 2) if predicted_ms else None,
            },
        })
        self.wfile.write(b"data: [DONE]\n\n")
        self.wfile.flush()


def main():
    host, port, ngl = parse_argv(sys.argv[1:])
    # Fake backend banner so bench.py's backend probe has something to classify:
    # a Vulkan device + offload line when -ngl > 0, nothing (CPU path) at -ngl 0 —
    # mirroring how real llama-server only prints the offload banner when offloading.
    if ngl > 0:
        sys.stderr.write("ggml_vulkan: 0 = STUB Radeon (RADV VANGOGH-FAKE) | this is a test stub, not a GPU\n")
        sys.stderr.write("load_tensors: offloaded 29/29 layers to GPU [STUB]\n")
    else:
        sys.stderr.write("load_tensors: CPU-only, 0 layers offloaded [STUB]\n")
    sys.stderr.flush()
    srv = ThreadingHTTPServer((host, port), Handler)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
