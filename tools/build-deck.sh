#!/usr/bin/env bash
# Cross-build the full Villen host for the Steam Deck and stage a deploy bundle.
#
# The Deck has no toolchain and a read-only rootfs, so we build on a PC and copy
# (docs/steamdeck-debugging.md). The Game-Mode build links the ImGui admin UI,
# whose geometry code pulls in sqrtf/acosf/atan2f — on a PC with glibc > the
# Deck's these bind a too-new symbol version and the loader aborts before main()
# (the "crash-quit"). So we force-include the .symver pin (spike/deck/glibc_compat.h)
# and static-link the C++ runtime, then gate on the highest GLIBC symbol the
# binary ends up needing.
#
# Output: deploy/Villen/  (gitignored) — villen + client/ + run-villen-*.sh.
# No infra details here; the operator's device-specific launcher (model paths,
# LD_LIBRARY_PATH, etc.) lives only on the Deck / in the gitignored bundle.
#
# Usage:   tools/build-deck.sh           # build + stage
#          DECK_GLIBC=2.41 tools/build-deck.sh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

BUILD_DIR="${BUILD_DIR:-build-deck}"
BUNDLE="$ROOT/deploy/Villen"
GLIBC_COMPAT="$ROOT/spike/deck/glibc_compat.h"
DECK_GLIBC="${DECK_GLIBC:-2.41}"   # SteamOS glibc; binary must need nothing newer

[ -f "$ROOT/third_party/imgui/imgui.cpp" ] || {
    echo "!! third_party/imgui submodule missing — run:"
    echo "   git submodule update --init third_party/imgui"; exit 1; }

echo ">>> [1/4] configure ($BUILD_DIR, Release, glibc-compat + static libstdc++)"
cmake -S . -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DVILLEN_BUILD_TESTS=OFF \
    -DCMAKE_C_FLAGS="-include $GLIBC_COMPAT" \
    -DCMAKE_CXX_FLAGS="-include $GLIBC_COMPAT" \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc" >/dev/null
grep -q "filter GPU backend enabled" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null || true

echo ">>> [2/4] build villen"
cmake --build "$BUILD_DIR" --target villen

BIN="$BUILD_DIR/host/villen"
echo ">>> [3/4] glibc gate (need <= $DECK_GLIBC)"
HIGH="$(objdump -T "$BIN" | grep -oE 'GLIBC_[0-9.]+' | sort -V -u | tail -1)"
echo "    highest GLIBC symbol needed: $HIGH"
# string-sort-safe numeric compare of the two dotted versions
lowest="$(printf '%s\n%s\n' "${HIGH#GLIBC_}" "$DECK_GLIBC" | sort -V | head -1)"
[ "$lowest" = "${HIGH#GLIBC_}" ] || {
    echo "!! $HIGH is newer than the Deck's $DECK_GLIBC — it would crash-quit."
    echo "   Add the offending symbols to spike/deck/glibc_compat.h and rebuild."
    exit 1; }
echo "    OK."

echo ">>> [4/4] stage $BUNDLE"
rm -rf "$BUNDLE"
mkdir -p "$BUNDLE"
cp "$BIN" "$BUNDLE/villen"; chmod +x "$BUNDLE/villen"
cp -r "$ROOT/client" "$BUNDLE/client"

# Two generic launchers. The device-specific one (chat with model paths /
# LD_LIBRARY_PATH) is NOT generated here — it's operator config, kept on the Deck.
cat > "$BUNDLE/run-villen-filter.sh" <<'SH'
#!/usr/bin/env bash
# Boot straight into the filter engine (real-time morphology on the APU).
cd "$(dirname "$0")" || exit 1
./villen --engine filter --client-dir ./client --port 9002 "$@" 2>&1 | tee villen.log
SH
cat > "$BUNDLE/run-villen-launcher.sh" <<'SH'
#!/usr/bin/env bash
# Open the admin-shell launcher (operator picks chess / filter / chat at runtime).
cd "$(dirname "$0")" || exit 1
./villen --client-dir ./client --port 9002 "$@" 2>&1 | tee villen.log
SH
chmod +x "$BUNDLE"/run-villen-*.sh

echo ">>> done."
echo "    bundle:  $BUNDLE   ($(du -sh "$BUNDLE" | cut -f1))"
echo "    deploy:  DECK=<user@host> tools/deploy-to-deck.sh"
