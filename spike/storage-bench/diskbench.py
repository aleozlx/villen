#!/usr/bin/env python3
# Villen — internal-NVMe-vs-SD-card read benchmark for the chat engine's
# model-switch cost (companion to ../chat-bench/, DESIGN-chat.md §5/§14).
#
# WHY: switching the resident LLM means re-reading a ~4.4 GB GGUF off disk; the
# Step 7 spike measured that at ~60 s cold off the Steam Deck's micro-SD card.
# The Deck also has a 512 GB internal NVMe with ~126 GB free. This tool quantifies
# how much faster a cold model load would be from internal storage, so we can
# decide whether to relocate the GGUFs (operator action) — the only lever that
# turns a ~60 s switch into a ~few-second one.
#
# WHAT it measures, per path: sequential read bandwidth COLD (page cache evicted)
# and WARM (cached ceiling), plus the projected cold-load time for a GGUF of a
# given size. Write bandwidth is reported as a by-product of creating the test file.
#
# HOW it gets a cold read WITHOUT ROOT: the Deck's `deck` user can't
# `echo 3 > /proc/sys/vm/drop_caches` (no root, no passwordless sudo). Instead we
# use posix_fadvise(POSIX_FADV_DONTNEED), which drops *this file's* clean pages
# from the cache — root-free, file-scoped. (Caveat: it's advisory and only evicts
# clean pages, so we fsync at creation and evict on a fresh fd right before each
# cold read. On ext4 it's reliable; exFAT/odd filesystems may evict less fully —
# if cold ≈ warm on the SD card, that's the signal it didn't fully evict.)
#
# PURE STDLIB (os/time/argparse) — the Deck rootfs is read-only, no pip; Python 3
# ships with SteamOS. Verify the harness runs on any Linux PC first:
#
#   ./diskbench.py --self-test
#
# Real Deck run (DEFERRED to a quiet window — the Deck is a shared device):
#   copy a GGUF to internal, then point one --path at each disk:
#
#   cp /run/media/deck/SD256/LLM/Qwen2.5-7B-Instruct-Q4_K_M.gguf ~/qwen.gguf
#   ./diskbench.py --file ~/qwen.gguf \
#                  --also /run/media/deck/SD256/LLM/Qwen2.5-7B-Instruct-Q4_K_M.gguf
#   # or synthetic, same size on each disk:
#   ./diskbench.py --path ~ --path /run/media/deck/SD256/LLM --size-mb 4505

import argparse
import os
import sys
import time

MIB = 1024 * 1024


def fmt_mbps(bytes_n, secs):
    if not secs:
        return "—"
    return f"{bytes_n / MIB / secs:7.1f}"


def gb(bytes_n):
    return f"{bytes_n / MIB / 1024:.2f}"


def backing_device(path):
    """Best-effort (device, fstype) backing `path`, via the longest matching
    mountpoint in /proc/mounts. Returns ('?', '?') if it can't be resolved."""
    ap = os.path.abspath(path)
    best = ("?", "?", "")
    try:
        with open("/proc/mounts") as f:
            for line in f:
                parts = line.split()
                if len(parts) < 3:
                    continue
                dev, mnt, fstype = parts[0], parts[1], parts[2]
                if (ap == mnt or ap.startswith(mnt.rstrip("/") + "/")) and len(mnt) >= len(best[2]):
                    best = (dev, fstype, mnt)
    except OSError:
        pass
    return best[0], best[1]


def make_test_file(path, size_bytes):
    """Write a non-sparse, low-compressibility file of size_bytes and fsync it.
    Reuses an existing file of the right size. Returns (path, write_seconds)."""
    if os.path.exists(path) and os.path.getsize(path) == size_bytes:
        return path, None
    block = os.urandom(MIB)  # one random MiB, reused — incompressible within a block
    written = 0
    t0 = time.perf_counter()
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        while written < size_bytes:
            n = min(len(block), size_bytes - written)
            os.write(fd, block[:n])
            written += n
        os.fsync(fd)
    finally:
        os.close(fd)
    return path, time.perf_counter() - t0


def read_once(path, bs, evict_first):
    """Sequentially read the whole file in bs-byte chunks; return (seconds, bytes).
    If evict_first, drop the file's pages from cache (cold read)."""
    fd = os.open(path, os.O_RDONLY)
    try:
        size = os.fstat(fd).st_size
        if evict_first:
            try:
                os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
            except (AttributeError, OSError):
                pass  # platform without fadvise — cold will read ~= warm
        try:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_SEQUENTIAL)
        except (AttributeError, OSError):
            pass
        total = 0
        t0 = time.perf_counter()
        while True:
            chunk = os.read(fd, bs)
            if not chunk:
                break
            total += len(chunk)
        return time.perf_counter() - t0, total
    finally:
        os.close(fd)


def bench_path(path, size_bytes, bs, reps, gguf_bytes, keep, given_file):
    """Create-or-reuse a test file at `path`, then time cold and warm reads."""
    is_dir = os.path.isdir(path)
    testfile = path if given_file else os.path.join(path, ".diskbench-testfile.bin")
    dev, fstype = backing_device(testfile)
    write_s = None
    if given_file:
        size_bytes = os.path.getsize(testfile)
    else:
        _, write_s = make_test_file(testfile, size_bytes)

    cold = []
    warm = []
    for _ in range(reps):
        cs, n = read_once(testfile, bs, evict_first=True)
        cold.append(cs)
        ws, _ = read_once(testfile, bs, evict_first=False)
        warm.append(ws)

    # Best (fastest) read = least contended sample.
    cold_s, warm_s = min(cold), min(warm)
    cold_mbps = size_bytes / MIB / cold_s if cold_s else 0
    proj_load_s = (gguf_bytes / MIB / cold_mbps) if cold_mbps else None

    if not given_file and not keep:
        try:
            os.remove(testfile)
        except OSError:
            pass

    return {
        "path": path,
        "device": dev,
        "fstype": fstype,
        "size_bytes": size_bytes,
        "write_mbps": (size_bytes / MIB / write_s) if write_s else None,
        "cold_s": cold_s,
        "warm_s": warm_s,
        "cold_mbps": cold_mbps,
        "warm_mbps": size_bytes / MIB / warm_s if warm_s else 0,
        "proj_load_s": proj_load_s,
    }


def main():
    ap = argparse.ArgumentParser(
        description="Villen storage-bench — internal NVMe vs SD-card read speed "
                    "for the chat model-switch cost.")
    ap.add_argument("--path", action="append", default=[],
                    help="directory to test (a temp file is written there); repeat "
                         "for several disks, e.g. --path ~ --path /run/media/deck/SD256/LLM")
    ap.add_argument("--file", default="",
                    help="benchmark an EXISTING file in place (e.g. a real GGUF) "
                         "instead of a synthetic temp file")
    ap.add_argument("--also", action="append", default=[],
                    help="extra existing file(s) to benchmark alongside --file")
    ap.add_argument("--size-mb", type=int, default=1024,
                    help="synthetic test-file size in MiB (default 1024; use ~4505 "
                         "to mimic a 4.4 GB 7B-Q4 GGUF)")
    ap.add_argument("--bs-kb", type=int, default=1024, help="read block size in KiB")
    ap.add_argument("--reps", type=int, default=3, help="cold/warm read repetitions")
    ap.add_argument("--gguf-gb", type=float, default=4.4,
                    help="GGUF size to project a cold model-load time for")
    ap.add_argument("--keep", action="store_true",
                    help="don't delete synthetic test files afterwards")
    ap.add_argument("--self-test", action="store_true",
                    help="run a tiny /tmp benchmark to verify the harness on a PC")
    args = ap.parse_args()

    if args.self_test:
        args.path = ["/tmp"]
        args.size_mb = 64
        args.reps = 2
        print("storage-bench SELF-TEST (/tmp, 64 MiB) — verifying harness only\n")

    files = ([args.file] if args.file else []) + list(args.also)
    if not args.path and not files:
        ap.error("give at least one --path or --file (or --self-test)")

    size_bytes = args.size_mb * MIB
    bs = args.bs_kb * 1024
    gguf_bytes = int(args.gguf_gb * 1024 * MIB)

    targets = [(p, False) for p in args.path] + [(f, True) for f in files]
    rows = []
    for target, is_file in targets:
        kind = "file" if is_file else "dir "
        print(f"benchmarking [{kind}] {target} ...", flush=True)
        try:
            rows.append(bench_path(target, size_bytes, bs, args.reps,
                                   gguf_bytes, args.keep, is_file))
        except OSError as e:
            print(f"  ! failed: {e}", file=sys.stderr)
            rows.append({"path": target, "error": str(e)})

    print(f"\n{'path':<34} {'device':<16} {'fs':<6} {'size':>7} "
          f"{'cold MB/s':>10} {'warm MB/s':>10} {'load ' + str(args.gguf_gb) + 'GB':>12}")
    print("-" * 100)
    for r in rows:
        if r.get("error"):
            print(f"{r['path']:<34} ERROR: {r['error']}")
            continue
        load = f"{r['proj_load_s']:.1f}s" if r["proj_load_s"] else "—"
        print(f"{r['path']:<34} {r['device']:<16} {r['fstype']:<6} "
              f"{gb(r['size_bytes']):>5}GB {fmt_mbps(r['size_bytes'], r['cold_s']):>10} "
              f"{fmt_mbps(r['size_bytes'], r['warm_s']):>10} {load:>12}")

    ok = [r for r in rows if not r.get("error")]
    if len(ok) >= 2:
        ok.sort(key=lambda r: r["cold_mbps"], reverse=True)
        fast, slow = ok[0], ok[-1]
        if slow["cold_mbps"]:
            speedup = fast["cold_mbps"] / slow["cold_mbps"]
            saved = (slow["proj_load_s"] or 0) - (fast["proj_load_s"] or 0)
            print(f"\nFastest cold read: {fast['path']} "
                  f"({fast['cold_mbps']:.0f} MB/s) — {speedup:.1f}× the slowest "
                  f"({slow['path']}, {slow['cold_mbps']:.0f} MB/s).")
            print(f"Projected model-switch saving for a {args.gguf_gb} GB GGUF: "
                  f"~{saved:.0f}s ({slow['proj_load_s']:.0f}s → {fast['proj_load_s']:.0f}s cold).")

    if args.self_test:
        print("\nSELF-TEST OK — harness runs. (Numbers are /tmp, not a real disk "
              "comparison.) Run on the Deck with real --path/--file for the verdict.")


if __name__ == "__main__":
    main()
