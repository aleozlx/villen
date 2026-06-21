#!/usr/bin/env bash
# Build the host, start it on a throwaway port with --engine filter (headless),
# run the Python e2e checks against it, then stop it. DESIGN-filter §13 step 3-4.
#
#   tests/e2e/run_filter_e2e.sh [build_dir] [port]
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="${1:-$ROOT/build}"
PORT="${2:-9100}"
BIN="$BUILD/host/villen"

if [[ ! -x "$BIN" ]]; then
  echo "host binary not found at $BIN — build first:" >&2
  echo "  cmake -S '$ROOT' -B '$BUILD' -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build '$BUILD'" >&2
  exit 2
fi

echo "starting host: --engine filter --headless --port $PORT"
"$BIN" --engine filter --headless --port "$PORT" --client-dir "$ROOT/client" >/tmp/villen_filter_e2e.log 2>&1 &
SRV=$!
trap 'kill "$SRV" 2>/dev/null || true' EXIT

# Wait for the port to accept connections.
for _ in $(seq 1 50); do
  if (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null; then exec 3>&- 3<&-; break; fi
  sleep 0.1
done

python3 "$ROOT/tests/e2e/filter_e2e.py" --uri "ws://127.0.0.1:$PORT/"
rc=$?
echo "(host log: /tmp/villen_filter_e2e.log)"
exit $rc
