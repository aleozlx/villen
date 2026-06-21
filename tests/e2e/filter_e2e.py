#!/usr/bin/env python3
"""End-to-end test for the `filter` engine media path (DESIGN-filter §13 step 3-4).

Drives a running Villen host (started with `--engine filter`) over the real
WebSocket player edge: joins as a feed, streams a JPEG frame in, and checks the
processed reply. This exercises the whole slice the unit tests can't — the binary
transport (§5.1), JPEG decode/encode, and the morphology pipeline in the loop —
plus the per-connection privacy guarantee (§10.1).

Not part of ctest (needs a running host + Python deps); run via run_filter_e2e.sh.

  python3 filter_e2e.py --uri ws://127.0.0.1:9100/
"""
import argparse
import asyncio
import io
import json
import struct
import sys

import numpy as np
import websockets
from PIL import Image

HEADER = struct.Struct("<IHH")  # [u32 seq][u16 w][u16 h]  (§5.2)


def make_jpeg(w, h):
    """A hard vertical step edge: left half black, right half white. Morphological
    gradient turns this into ~black everywhere with a bright line at the seam, so
    the reply is unmistakably *processed*, not echoed."""
    a = np.zeros((h, w, 3), np.uint8)
    a[:, w // 2 :, :] = 255
    buf = io.BytesIO()
    Image.fromarray(a, "RGB").save(buf, "JPEG", quality=90)
    return buf.getvalue(), a


def decode(jpeg):
    return np.asarray(Image.open(io.BytesIO(jpeg)).convert("RGB"))


async def recv_until(ws, pred, timeout=4.0):
    """Read messages until `pred(msg)` is truthy; return that message."""
    loop = asyncio.get_event_loop()
    end = loop.time() + timeout
    while loop.time() < end:
        msg = await asyncio.wait_for(ws.recv(), timeout=end - loop.time())
        if pred(msg):
            return msg
    raise TimeoutError("predicate not satisfied in time")


def is_config(msg):
    if not isinstance(msg, str):
        return False
    try:
        return json.loads(msg).get("type") == "filterConfig"
    except Exception:
        return False


async def join(ws):
    await ws.send(json.dumps({"type": "join"}))
    cfg = json.loads(await recv_until(ws, is_config))
    return cfg


async def roundtrip(ws, w, h, seq=1):
    jpeg, src = make_jpeg(w, h)
    await ws.send(HEADER.pack(seq, w, h) + jpeg)
    reply = await recv_until(ws, lambda m: isinstance(m, (bytes, bytearray)))
    rseq, rw, rh = HEADER.unpack(reply[:8])
    out = decode(bytes(reply[8:]))
    return rseq, rw, rh, out, src


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--uri", default="ws://127.0.0.1:9100/")
    args = ap.parse_args()
    failures = []

    def check(name, ok, detail=""):
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}{(' — ' + detail) if detail else ''}")
        if not ok:
            failures.append(name)

    # 1. Round-trip: a frame in comes back processed (gradient, the default).
    async with websockets.connect(args.uri, max_size=4 * 1024 * 1024) as ws:
        cfg = await join(ws)
        w, h = cfg["outW"], cfg["outH"]
        check("filterConfig advertises jpeg", cfg.get("format") == "jpeg", str(cfg.get("format")))
        check("default pipeline is non-empty", len(cfg.get("pipeline", [])) >= 1,
              json.dumps(cfg.get("pipeline")))

        rseq, rw, rh, out, src = await roundtrip(ws, w, h, seq=7)
        check("reply echoes source seq", rseq == 7, f"got {rseq}")
        check("reply dimensions match", (rw, rh) == (w, h), f"{rw}x{rh}")

        src_mean = float(src.mean())          # ~127 (half white)
        out_mean = float(out.mean())          # gradient of flats -> ~0
        out_max = int(out.max())              # the edge line -> bright
        check("output is processed, not echoed (low mean)", out_mean < src_mean * 0.5,
              f"src_mean={src_mean:.0f} out_mean={out_mean:.0f}")
        check("gradient edge is present (high max)", out_max > 120, f"out_max={out_max}")

        # 2. requestPreset switches the server-authoritative pipeline (§5.2).
        await ws.send(json.dumps({"type": "requestPreset", "preset": "edge"}))
        newcfg = json.loads(await recv_until(ws, is_config))
        ops = [s["op"] for s in newcfg.get("pipeline", [])]
        check("requestPreset 'edge' adds a threshold stage", "threshold" in ops, str(ops))

    # 3. Privacy: a second feed must NOT receive the first feed's reply (§10.1).
    async with websockets.connect(args.uri) as a, websockets.connect(args.uri) as b:
        await join(a)
        await join(b)
        # A sends a frame; B should see only text (config/envelopes), no binary.
        jpeg, _ = make_jpeg(64, 48)
        await a.send(HEADER.pack(1, 64, 48) + jpeg)
        got_a = await roundtrip_first_binary(a)
        got_b_binary = await any_binary_within(b, 1.5)
        check("the sender receives its processed frame", got_a)
        check("a second feed receives NONE of it (per-connection)", not got_b_binary)

    print()
    if failures:
        print(f"FAILED: {len(failures)} check(s): {', '.join(failures)}")
        return 1
    print("All filter e2e checks passed.")
    return 0


async def roundtrip_first_binary(ws):
    try:
        await recv_until(ws, lambda m: isinstance(m, (bytes, bytearray)), timeout=3.0)
        return True
    except Exception:
        return False


async def any_binary_within(ws, timeout):
    try:
        await recv_until(ws, lambda m: isinstance(m, (bytes, bytearray)), timeout=timeout)
        return True
    except Exception:
        return False


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
