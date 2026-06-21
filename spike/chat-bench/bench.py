#!/usr/bin/env python3
# Villen — chat engine Step 7 spike: Deck APU/Vulkan throughput benchmark.
#
# Throwaway risk burn-down (DESIGN-chat.md §14 step 7, acceptance #6). Step 4
# proved Qwen2.5-7B streams via RADV Vulkan on the Vangogh APU; this QUANTIFIES
# it on the actual Deck:
#   - tok/s (decode) and TTFT (time-to-first-token) per model,
#   - Vulkan-vs-CPU (an -ngl sweep: 99 = offload all, 0 = pure CPU),
#   - unified-memory headroom alongside SteamOS + Gamescope,
#   - --parallel multi-client concurrency (continuous batching).
#
# It drives the SAME llama-server config the host spawns (host/src/llama_process.cpp:
# `-ngl 99 --parallel N`, the OpenAI /v1/chat/completions streaming path), so the
# numbers are faithful to what `--engine chat` actually does. The host is a thin
# non-blocking proxy (LlamaClient) that adds negligible overhead, so benchmarking
# llama-server directly measures the APU, which is the whole point.
#
# PURE STDLIB ONLY (urllib/threading/subprocess/json) — the Deck's rootfs is
# read-only, no pip. Python 3 ships with SteamOS. Verify the harness LLM-free on a
# PC against fake_llama_server.py before trusting it on real hardware:
#
#   ./bench.py --llama-bin ./fake_llama_server.py --model /dev/null \
#              --ngl 99 --parallel 1,2 --max-tokens 16 --reps 2
#
# Real Deck run (one model, Vulkan vs CPU, 1 vs 2 slots):
#
#   ./bench.py \
#     --llama-bin /run/media/deck/SD256/LLM/llama-server \
#     --model    /run/media/deck/SD256/LLM/qwen2.5-7b-instruct-q4_k_m.gguf \
#     --ld-library-path /run/media/deck/SD256/LLM \
#     --ngl 99,0 --parallel 1,2 --max-tokens 128 --reps 3
#
# See README.md for the full run matrix, what each number means, and the results
# template to record (acceptance #6: "tok/s is recorded").

import argparse
import json
import os
import socket
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------


def now():
    return time.perf_counter()


def read_meminfo():
    """Parse /proc/meminfo -> {key: kB}. Empty dict off-Linux (PC self-test on
    a non-Linux box would just skip memory numbers)."""
    out = {}
    try:
        with open("/proc/meminfo") as f:
            for line in f:
                parts = line.split()
                if len(parts) >= 2 and parts[0].endswith(":"):
                    try:
                        out[parts[0][:-1]] = int(parts[1])  # value in kB
                    except ValueError:
                        pass
    except OSError:
        pass
    return out


def read_rss_kb(pid):
    """Resident set size of a pid in kB (VmRSS), or None."""
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return None


def port_open(host, port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.5)
    try:
        s.connect((host, port))
        return True
    except OSError:
        return False
    finally:
        s.close()


# ---------------------------------------------------------------------------
# llama-server lifecycle (spawn / health-wait / backend probe / stop)
# ---------------------------------------------------------------------------


class Server:
    """Spawns and supervises one llama-server, mirroring the host's argv
    (llama_process.cpp). Captures stderr to a log so we can read the backend
    banner (Vulkan device vs llvmpipe vs CPU-only) — acceptance #6."""

    def __init__(self, args, ngl, parallel, logpath):
        self.args = args
        self.ngl = ngl
        self.parallel = parallel
        self.logpath = logpath
        self.proc = None
        self.logfile = None

    def spawn(self):
        argv = [self.args.llama_bin]
        if self.args.model:
            argv += ["-m", self.args.model]
        argv += ["--host", self.args.host, "--port", str(self.args.port)]
        argv += ["-ngl", str(self.ngl), "--parallel", str(self.parallel)]
        # A context window large enough for `parallel` slots * a few-k each; the
        # host caps per-conversation context (§5), the server splits -c across slots.
        argv += ["-c", str(self.args.ctx)]
        argv += self.args.extra
        env = dict(os.environ)
        if self.args.ld_library_path:
            prev = env.get("LD_LIBRARY_PATH", "")
            env["LD_LIBRARY_PATH"] = (
                self.args.ld_library_path + (":" + prev if prev else "")
            )
        self.logfile = open(self.logpath, "wb")
        self.logfile.write(("$ " + " ".join(argv) + "\n").encode())
        self.logfile.flush()
        self.proc = subprocess.Popen(
            argv, stdout=self.logfile, stderr=subprocess.STDOUT, env=env
        )

    def wait_ready(self, timeout_s):
        """Poll GET /health until 200 (or the model fails to load). First load of
        a 7B Q4 off the SD card is I/O-bound — ~110s observed — so the default
        timeout is generous."""
        deadline = now() + timeout_s
        url = f"http://{self.args.host}:{self.args.port}/health"
        while now() < deadline:
            if self.proc.poll() is not None:
                return False, f"server exited (code {self.proc.returncode}) before ready"
            try:
                with urllib.request.urlopen(url, timeout=2) as r:
                    if r.status == 200:
                        return True, "ok"
            except (urllib.error.URLError, OSError):
                pass
            time.sleep(1.0)
        return False, f"not ready after {timeout_s}s"

    def backend(self):
        """Read the captured log and classify the compute backend. Returns
        (label, detail-lines). label in {vulkan, cpu, llvmpipe, unknown}.

        The Deck's llama.cpp build (b9744) enumerates devices as
        `- Vulkan0 : AMD Custom GPU 0405 (RADV VANGOGH) ...` and, at default
        verbosity, does NOT print the older `ggml_vulkan:` / `offloaded N/M
        layers to GPU` banners. So we key off the device-enumeration line plus
        the requested -ngl: a real RADV/Vulkan device line at -ngl>0 = `vulkan`;
        `llvmpipe` anywhere = the software-GL failure (§4); -ngl 0 = `cpu` (the
        Vulkan device may still be *enumerated* at -ngl 0, but nothing offloads)."""
        try:
            with open(self.logpath, "r", errors="replace") as f:
                text = f.read()
        except OSError:
            return "unknown", []
        hits = []
        saw_llvmpipe = False
        saw_vulkan_dev = False
        for ln in text.splitlines():
            low = ln.lower()
            if "llvmpipe" in low:
                saw_llvmpipe = True
                hits.append(ln.strip())
            elif "vulkan" in low:
                # Any Vulkan device/backend line — `ggml_vulkan: ...` or the b9744
                # device enumeration `- Vulkan0 : <vendor> ...`. Vendor-agnostic on
                # purpose: AMD prints `(RADV VANGOGH)`, but an NVIDIA/Intel GPU on a
                # PC self-test won't, and we still want `vulkan`. `llvmpipe`
                # (software GL) is matched above with higher precedence, so reaching
                # here means a real hardware Vulkan device.
                saw_vulkan_dev = True
                hits.append(ln.strip())
            elif "offloaded" in low and "layer" in low:
                hits.append(ln.strip())
        if saw_llvmpipe:
            label = "llvmpipe"  # software GL — the failure mode §4 warns about
        elif self.ngl > 0 and saw_vulkan_dev:
            label = "vulkan"
        elif self.ngl == 0:
            label = "cpu"  # -ngl 0 = the intended CPU path (device may still enumerate)
        else:
            label = "unknown"
        return label, hits[:8]

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        if self.logfile:
            self.logfile.close()


# ---------------------------------------------------------------------------
# One streamed generation = one measurement
# ---------------------------------------------------------------------------


def run_stream(args, prompt):
    """POST /v1/chat/completions (stream) and time it. Returns a dict with
    client-measured TTFT + decode tok/s AND the server's own `timings` block
    (llama-server reports prompt/predicted tok/s authoritatively). ignore_eos +
    cache_prompt:false make every rep generate exactly max_tokens after a real
    prompt eval — clean, comparable numbers."""
    url = f"http://{args.host}:{args.port}/v1/chat/completions"
    body = json.dumps({
        "messages": [{"role": "user", "content": prompt}],
        "max_tokens": args.max_tokens,
        "temperature": 0.7,
        "stream": True,
        "stream_options": {"include_usage": True},
        "cache_prompt": False,   # force a real prompt eval each rep (fair TTFT)
        "ignore_eos": True,      # always emit exactly max_tokens (fair tok/s)
    }).encode()
    req = urllib.request.Request(
        url, data=body, headers={"Content-Type": "application/json"}
    )

    t_start = now()
    t_first = None
    n_chunks = 0
    server_timings = None
    usage = None
    err = None
    try:
        with urllib.request.urlopen(req, timeout=args.req_timeout) as resp:
            for raw in resp:
                line = raw.decode("utf-8", "replace").strip()
                if not line.startswith("data:"):
                    continue
                payload = line[len("data:"):].strip()
                if payload == "[DONE]":
                    break
                try:
                    obj = json.loads(payload)
                except json.JSONDecodeError:
                    continue
                # llama-server attaches `timings`/`usage` to the final chunk(s).
                if isinstance(obj.get("timings"), dict):
                    server_timings = obj["timings"]
                if isinstance(obj.get("usage"), dict):
                    usage = obj["usage"]
                choices = obj.get("choices") or []
                if choices:
                    delta = choices[0].get("delta") or {}
                    content = delta.get("content")
                    if content:
                        if t_first is None:
                            t_first = now()
                        n_chunks += 1
    except (urllib.error.URLError, OSError) as e:
        err = str(e)
    t_end = now()

    # Token count: prefer the server's predicted_n, else usage, else chunk count.
    n_tok = None
    if server_timings and "predicted_n" in server_timings:
        n_tok = server_timings["predicted_n"]
    elif usage and "completion_tokens" in usage:
        n_tok = usage["completion_tokens"]
    else:
        n_tok = n_chunks

    ttft = (t_first - t_start) if t_first is not None else None
    # Client decode tok/s over the streaming window (first->last token), the
    # span the user perceives as "typing". Needs >=2 tokens to be meaningful.
    decode_tps_client = None
    if t_first is not None and n_tok and n_tok > 1 and t_end > t_first:
        decode_tps_client = (n_tok - 1) / (t_end - t_first)

    return {
        "ttft_s": ttft,
        "decode_tps_client": decode_tps_client,
        "n_tokens": n_tok,
        "wall_total_s": t_end - t_start,
        "t_start": t_start,
        "t_end": t_end,
        "server_timings": server_timings,
        "error": err,
    }


# ---------------------------------------------------------------------------
# Memory sampler (worst-case headroom during generation)
# ---------------------------------------------------------------------------


class MemSampler(threading.Thread):
    """Polls MemAvailable + the server's RSS while requests run; keeps the worst
    (lowest) headroom and the peak RSS — the unified-memory budget (§5)."""

    def __init__(self, pid, interval=0.25):
        super().__init__(daemon=True)
        self.pid = pid
        self.interval = interval
        self._stop_evt = threading.Event()
        self.min_avail_kb = None
        self.peak_rss_kb = None

    def run(self):
        while not self._stop_evt.is_set():
            mi = read_meminfo()
            avail = mi.get("MemAvailable")
            if avail is not None:
                if self.min_avail_kb is None or avail < self.min_avail_kb:
                    self.min_avail_kb = avail
            rss = read_rss_kb(self.pid)
            if rss is not None:
                if self.peak_rss_kb is None or rss > self.peak_rss_kb:
                    self.peak_rss_kb = rss
            self._stop_evt.wait(self.interval)

    def stop(self):
        self._stop_evt.set()
        self.join(timeout=2)


# ---------------------------------------------------------------------------
# Shared stream-driving + metric summary (used by both spawn and attach modes)
# ---------------------------------------------------------------------------


def med(vals):
    vals = sorted(v for v in vals if v is not None)
    if not vals:
        return None
    n = len(vals)
    return vals[n // 2] if n % 2 else (vals[n // 2 - 1] + vals[n // 2]) / 2


def drive_streams(args, parallel):
    """Single-stream (`reps` sequential -> stable median) when parallel<=1, else
    `parallel` concurrent streams at once (continuous batching, §8)."""
    if parallel <= 1:
        return [run_stream(args, args.prompt + f" (rep {i})") for i in range(args.reps)]
    results = [None] * parallel
    threads = []

    def worker(idx):
        results[idx] = run_stream(args, args.prompt + f" (slot {idx})")

    for i in range(parallel):
        th = threading.Thread(target=worker, args=(i,))
        threads.append(th)
        th.start()
    for th in threads:
        th.join()
    return results


def summarize_streams(streams):
    """Reduce a batch of run_stream() results to medians + aggregate throughput.
    Decode tok/s is server-authoritative (`predicted_per_second`) when present,
    else the client-side wall-clock estimate."""
    ok = [s for s in streams if s and not s.get("error")]
    errs = [s.get("error") for s in streams if s and s.get("error")]

    def decode_of(s):
        st = s.get("server_timings")
        if st and st.get("predicted_per_second"):
            return st["predicted_per_second"]
        return s.get("decode_tps_client")

    ttfts = [s["ttft_s"] for s in ok]
    decodes = [decode_of(s) for s in ok]
    prompt_tps = [(s["server_timings"] or {}).get("prompt_per_second") for s in ok]

    # Aggregate system throughput: total tokens over the wall span from the first
    # request's start to the last request's end (meaningful for the concurrent case).
    aggregate_tps = None
    if ok:
        span_start = min(s["t_start"] for s in ok)
        span_end = max(s["t_end"] for s in ok)
        total_tok = sum((s["n_tokens"] or 0) for s in ok)
        if span_end > span_start:
            aggregate_tps = total_tok / (span_end - span_start)

    return {
        "streams_ok": len(ok),
        "streams_err": errs,
        "ttft_s_median": round(med(ttfts), 3) if med(ttfts) is not None else None,
        "decode_tps_median": round(med(decodes), 2) if med(decodes) is not None else None,
        "decode_tps_per_stream": [round(d, 2) if d else None for d in decodes],
        "prompt_tps_median": round(med(prompt_tps), 1) if med(prompt_tps) is not None else None,
        "aggregate_tps": round(aggregate_tps, 2) if aggregate_tps is not None else None,
    }


def fetch_props(host, port):
    """GET /props off a running llama-server: model path + total slots (the
    --parallel it was launched with). Best-effort."""
    try:
        with urllib.request.urlopen(f"http://{host}:{port}/props", timeout=3) as r:
            obj = json.loads(r.read().decode("utf-8", "replace"))
        return {
            "model": os.path.basename(obj.get("model_path", "") or "") or "?",
            "total_slots": obj.get("total_slots"),
        }
    except (urllib.error.URLError, OSError, json.JSONDecodeError, ValueError):
        return {"model": "?", "total_slots": None}


def gb(kb):
    return f"{kb/1024/1024:.2f}GB" if kb else "?"


# ---------------------------------------------------------------------------
# One (model, ngl, parallel) cell of the matrix — spawns its own llama-server
# ---------------------------------------------------------------------------


def run_cell(args, model_path, ngl, parallel):
    label = f"{os.path.basename(model_path)} ngl={ngl} parallel={parallel}"
    print(f"\n=== {label} ===", flush=True)

    if port_open(args.host, args.port):
        print(f"  ! port {args.port} already in use — stop the stray server first",
              file=sys.stderr)
        return {"label": label, "error": "port in use"}

    mem_before = read_meminfo().get("MemAvailable")
    logpath = os.path.join(
        args.logdir,
        f"llama-{os.path.basename(model_path)}-ngl{ngl}-p{parallel}.log",
    )
    cell_args = argparse.Namespace(**vars(args))
    cell_args.model = model_path
    srv = Server(cell_args, ngl, parallel, logpath)
    srv.spawn()
    t_load0 = now()
    ok, msg = srv.wait_ready(args.load_timeout)
    load_s = now() - t_load0
    if not ok:
        print(f"  ! server not ready: {msg} (see {logpath})", file=sys.stderr)
        srv.stop()
        return {"label": label, "error": msg, "log": logpath}

    backend, backend_lines = srv.backend()
    mem_after_load = read_meminfo().get("MemAvailable")
    print(f"  backend: {backend}  | load {load_s:.1f}s  | log {logpath}")
    for bl in backend_lines:
        print(f"    {bl}")
    if backend == "llvmpipe":
        print("  ! WARNING: llvmpipe = software GL, NOT the real RADV driver (§4)",
              file=sys.stderr)

    # Warmup (compiles shaders / fills caches; not measured).
    for _ in range(max(0, args.warmup)):
        run_stream(cell_args, args.prompt)

    sampler = MemSampler(srv.proc.pid)
    sampler.start()
    streams = drive_streams(cell_args, parallel)
    sampler.stop()

    cell = {
        "label": label,
        "model": os.path.basename(model_path),
        "ngl": ngl,
        "parallel": parallel,
        "backend": backend,
        "load_s": round(load_s, 1),
        "mem_avail_before_kb": mem_before,
        "mem_avail_after_load_kb": mem_after_load,
        "mem_avail_min_kb": sampler.min_avail_kb,
        "server_rss_peak_kb": sampler.peak_rss_kb,
        "log": logpath,
    }
    cell.update(summarize_streams(streams))
    srv.stop()
    time.sleep(args.cooldown)  # let MemAvailable recover before the next cell

    print(f"  TTFT median:     {cell['ttft_s_median']} s")
    print(f"  decode tok/s:    {cell['decode_tps_median']} (per-stream {cell['decode_tps_per_stream']})")
    if parallel > 1:
        print(f"  aggregate tok/s: {cell['aggregate_tps']} (system throughput, {cell['streams_ok']} slots)")
    print(f"  prompt tok/s:    {cell['prompt_tps_median']}")
    print(f"  mem: avail before {gb(mem_before)} -> after-load {gb(mem_after_load)} "
          f"-> min-during-gen {gb(sampler.min_avail_kb)} | server RSS peak {gb(sampler.peak_rss_kb)}")
    if cell["streams_err"]:
        print(f"  ! {len(cell['streams_err'])} stream error(s): {cell['streams_err']}", file=sys.stderr)
    return cell


# ---------------------------------------------------------------------------
# Attach mode — measure an ALREADY-RUNNING llama-server (no spawn, §8 probe)
# ---------------------------------------------------------------------------


def run_attached(args, parallel):
    """Drive a server that's already up (e.g. the live host's child on :8099).
    No spawn, no load timing, no -ngl sweep — the server's config is fixed. The
    backend can't be read from a log we don't own, so it's recorded from --note
    (the operator states what the server was launched with). Memory headroom is
    the *live appliance* state (one model + the host resident), and a 2+ probe
    shows how the server's actual slot count handles concurrency."""
    props = fetch_props(args.host, args.port)
    model = props["model"]
    label = f"{model} attached :{args.port} concurrency={parallel}"
    print(f"\n=== {label} ===", flush=True)

    try:
        with urllib.request.urlopen(
                f"http://{args.host}:{args.port}/health", timeout=3) as r:
            if r.status != 200:
                return {"label": label, "error": f"/health {r.status}"}
    except (urllib.error.URLError, OSError) as e:
        return {"label": label, "error": f"unreachable: {e}"}

    backend = args.note or "attached (backend per live launch)"
    print(f"  backend: {backend}  | server slots (/props total_slots): {props['total_slots']}")
    if props["total_slots"] is not None and parallel > props["total_slots"]:
        print(f"  note: concurrency {parallel} > {props['total_slots']} slot(s) — "
              f"excess requests queue/serialize (§8), no batching gain expected")

    for _ in range(max(0, args.warmup)):
        run_stream(args, args.prompt)

    sampler = MemSampler(args.server_pid) if args.server_pid else None
    if sampler:
        sampler.start()
    mem_before = read_meminfo().get("MemAvailable")
    streams = drive_streams(args, parallel)
    mem_min = None
    rss_peak = None
    if sampler:
        sampler.stop()
        mem_min = sampler.min_avail_kb
        rss_peak = sampler.peak_rss_kb

    cell = {
        "label": label,
        "model": model,
        "ngl": "live",
        "parallel": parallel,
        "backend": backend,
        "load_s": None,
        "total_slots": props["total_slots"],
        "mem_avail_before_kb": mem_before,
        "mem_avail_after_load_kb": mem_before,
        "mem_avail_min_kb": mem_min,
        "server_rss_peak_kb": rss_peak,
        "log": None,
    }
    cell.update(summarize_streams(streams))
    time.sleep(args.cooldown)

    print(f"  TTFT median:     {cell['ttft_s_median']} s")
    print(f"  decode tok/s:    {cell['decode_tps_median']} (per-stream {cell['decode_tps_per_stream']})")
    if parallel > 1:
        print(f"  aggregate tok/s: {cell['aggregate_tps']} (across {cell['streams_ok']} concurrent reqs)")
    print(f"  prompt tok/s:    {cell['prompt_tps_median']}")
    print(f"  mem: avail {gb(mem_before)} (live appliance) -> min-during-gen {gb(mem_min)} "
          f"| server RSS peak {gb(rss_peak)}")
    if cell["streams_err"]:
        print(f"  ! {len(cell['streams_err'])} stream error(s): {cell['streams_err']}", file=sys.stderr)
    return cell


# ---------------------------------------------------------------------------
# Markdown report
# ---------------------------------------------------------------------------


def write_markdown(cells, path, args):
    def gb(kb):
        return f"{kb/1024/1024:.2f}" if kb else "—"

    src = f"attached `{args.host}:{args.port}`" if args.attach else f"llama-bin `{args.llama_bin}`"
    lines = []
    lines.append("# Villen chat Step 7 — Deck APU/Vulkan throughput results\n")
    lines.append(f"- source: {src}")
    lines.append(f"- max_tokens: {args.max_tokens}  reps: {args.reps}  ctx: {args.ctx}")
    lines.append(f"- prompt: `{args.prompt}`\n")
    lines.append("| model | ngl | par | backend | TTFT s | decode tok/s | agg tok/s | prompt tok/s | min avail GB | RSS GB | load s |")
    lines.append("|---|--:|--:|---|--:|--:|--:|--:|--:|--:|--:|")
    for c in cells:
        if c.get("error") and "backend" not in c:
            lines.append(f"| {c.get('model', c['label'])} | | | ERROR: {c['error']} |||||||")
            continue
        lines.append(
            f"| {c['model']} | {c['ngl']} | {c['parallel']} | {c['backend']} "
            f"| {c['ttft_s_median']} | {c['decode_tps_median']} | {c['aggregate_tps'] or '—'} "
            f"| {c['prompt_tps_median']} | {gb(c['mem_avail_min_kb'])} "
            f"| {gb(c['server_rss_peak_kb'])} | {c['load_s'] if c['load_s'] is not None else '—'} |"
        )
    lines.append("")
    lines.append("Backend must read **vulkan** (RADV VANGOGH), never **llvmpipe** "
                 "(software GL) — acceptance #6.\n")
    with open(path, "w") as f:
        f.write("\n".join(lines))
    print(f"\nMarkdown summary -> {path}")


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------


def parse_list(s, conv):
    return [conv(x) for x in s.split(",") if x.strip() != ""]


def main():
    ap = argparse.ArgumentParser(
        description="Villen chat Step 7 — Deck APU/Vulkan throughput spike.")
    ap.add_argument("--llama-bin", default="",
                    help="path to llama-server (or fake_llama_server.py for self-test); "
                         "required unless --attach")
    ap.add_argument("--attach", default="",
                    help="HOST:PORT of an ALREADY-RUNNING llama-server to measure "
                         "without spawning (e.g. the live host's child). Skips the "
                         "-ngl sweep; --parallel becomes a client-side concurrency probe")
    ap.add_argument("--server-pid", type=int, default=0,
                    help="PID of the attached server, for RSS sampling (attach mode)")
    ap.add_argument("--note", default="",
                    help="backend note recorded in attach mode, e.g. "
                         "'vulkan/RADV VANGOGH (live -ngl 99)'")
    ap.add_argument("--model", action="append", default=[],
                    help="GGUF path; repeat for several models (tok/s across models)")
    ap.add_argument("--ld-library-path", default="",
                    help="prepended to LD_LIBRARY_PATH for the spawned server "
                         "(the Deck's Vulkan llama-server needs its bundled libs)")
    ap.add_argument("--ngl", default="99",
                    help="comma list of -ngl values to sweep (e.g. 99,0 = Vulkan vs CPU)")
    ap.add_argument("--parallel", default="1",
                    help="comma list of --parallel slot counts (e.g. 1,2,4)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8099)
    ap.add_argument("--ctx", type=int, default=4096, help="-c context window (§5)")
    ap.add_argument("--max-tokens", type=int, default=128,
                    help="decode tokens per request (ignore_eos forces exactly this)")
    ap.add_argument("--reps", type=int, default=3,
                    help="sequential reps for the single-stream (parallel=1) case")
    ap.add_argument("--warmup", type=int, default=1,
                    help="unmeasured warmup runs per cell")
    ap.add_argument("--prompt", default="Explain morphology in three short sentences.")
    ap.add_argument("--load-timeout", type=int, default=300,
                    help="seconds to wait for /health (first SD-card load is slow)")
    ap.add_argument("--req-timeout", type=int, default=180)
    ap.add_argument("--cooldown", type=float, default=2.0,
                    help="seconds between cells (let MemAvailable recover)")
    ap.add_argument("--logdir", default="./bench-logs")
    ap.add_argument("--out", default="", help="results JSON path (default results-<ts>.json)")
    ap.add_argument("--extra", nargs=argparse.REMAINDER, default=[],
                    help="everything after --extra is passed verbatim to llama-server")
    args = ap.parse_args()

    args.parallel = parse_list(args.parallel, int)
    args.ngl = parse_list(args.ngl, int)
    os.makedirs(args.logdir, exist_ok=True)

    cells = []
    if args.attach:
        # Attach mode: measure a running server; no spawn, no -ngl sweep.
        host, _, port = args.attach.partition(":")
        args.host = host or args.host
        args.port = int(port) if port else args.port
        print("Villen chat Step 7 spike — ATTACH mode:")
        print(f"  server   : {args.host}:{args.port}  (no spawn; live config)")
        print(f"  concurrency probe : {args.parallel}")
        for parallel in args.parallel:
            cells.append(run_attached(args, parallel))
    else:
        if not args.llama_bin:
            ap.error("--llama-bin is required (or use --attach HOST:PORT)")
        if not args.model:
            ap.error("at least one --model is required")
        print("Villen chat Step 7 spike — matrix:")
        print(f"  models   : {[os.path.basename(m) for m in args.model]}")
        print(f"  ngl      : {args.ngl}")
        print(f"  parallel : {args.parallel}")
        print(f"  cells    : {len(args.model) * len(args.ngl) * len(args.parallel)}")
        for model in args.model:
            for ngl in args.ngl:
                for parallel in args.parallel:
                    cells.append(run_cell(args, model, ngl, parallel))

    ts = time.strftime("%Y%m%d-%H%M%S")
    out = args.out or f"results-{ts}.json"
    with open(out, "w") as f:
        json.dump({
            "meta": {
                "timestamp": ts,
                "llama_bin": args.llama_bin,
                "max_tokens": args.max_tokens,
                "reps": args.reps,
                "ctx": args.ctx,
                "prompt": args.prompt,
            },
            "cells": cells,
        }, f, indent=2)
    print(f"\nResults JSON -> {out}")
    write_markdown(cells, out.rsplit(".", 1)[0] + ".md", args)

    # Non-zero exit if any cell hit llvmpipe or failed — makes a bad run obvious.
    bad = [c for c in cells if c.get("error") or c.get("backend") == "llvmpipe"]
    if bad:
        print(f"\n{len(bad)} cell(s) failed or ran on software GL.", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
