#!/usr/bin/env bash
# Deploy the staged Villen bundle (deploy/Villen/, built by tools/build-deck.sh)
# to a Steam Deck over SSH. Build/verify only — it never launches anything; the
# Game-Mode run is a manual step from the non-Steam shortcut.
#
# The SSH target is NOT baked in (no infra in the repo). Set it via DECK:
#       DECK=deck@<deck-host> tools/deploy-to-deck.sh
# 'deck' is the stock SteamOS account; the host is your LAN address or ssh alias.
#
# It is deliberately non-destructive:
#   - backs up the remote villen to villen.bak before overwriting
#   - rsyncs client/ WITHOUT --delete, so it ADDS client/filter/ and leaves any
#     device-local files in place
#   - ships only the generic run-villen-*.sh launchers; it never overwrites a
#     run-villen.sh, which is operator config (engine choice, model paths,
#     LD_LIBRARY_PATH) that lives only on the device.
set -euo pipefail

DECK="${DECK:?set DECK=deck@<deck-host> (the stock account is 'deck')}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/deploy/Villen"
DST="Villen"   # relative to the deck account's home (~/Villen)

[ -x "$SRC/villen" ] || { echo "!! no staged bundle at $SRC — run tools/build-deck.sh first"; exit 1; }

echo ">>> deploying $SRC  ->  $DECK:~/$DST"

echo ">>> [1/4] stop any running villen / llama-server (can't overwrite a running exe)"
# -x matches the process *name* so the killing shell never self-matches
# (the -f full-cmdline footgun, debugging guide §3.2).
ssh "$DECK" 'pkill -x villen 2>/dev/null; pkill -x llama-server 2>/dev/null; sleep 0.5;
             echo "    villen still running: $(pgrep -xc villen || echo 0)"'

echo ">>> [2/4] back up the current remote binary -> villen.bak"
ssh "$DECK" "mkdir -p ~/$DST; [ -f ~/$DST/villen ] && cp -f ~/$DST/villen ~/$DST/villen.bak && echo '    backed up' || echo '    (no prior binary)'"

echo ">>> [3/4] sync binary + client/ (no --delete) + generic launchers"
rsync -a "$SRC/villen" "$DECK:~/$DST/villen"
rsync -a "$SRC/client/" "$DECK:~/$DST/client/"
rsync -a "$SRC"/run-villen-*.sh "$DECK:~/$DST/"
[ -f "$SRC/tls-proxy.sh" ] && rsync -a "$SRC/tls-proxy.sh" "$DECK:~/$DST/"
ssh "$DECK" "chmod +x ~/$DST/villen ~/$DST/run-villen-*.sh ~/$DST/tls-proxy.sh 2>/dev/null; true"

echo ">>> [4/4] verify the binary loads on the Deck (glibc check)"
ssh "$DECK" "cd ~/$DST &&
  ldd ./villen 2>&1 | grep -iE 'not found' &&
    { echo '    !! MISSING SYMBOLS — would crash-quit in Game Mode'; exit 1; } ||
    echo '    libs resolve OK';
  echo -n '    highest GLIBC needed: '; objdump -T ./villen 2>/dev/null | grep -oE 'GLIBC_[0-9.]+' | sort -V -u | tail -1"

cat <<NEXT

>>> deployed. Manual Game-Mode test:
    - Point a non-Steam shortcut's exec at  ~/$DST/run-villen-filter.sh
      (Start In = ~/$DST so villen.log lands in the bundle), or use
      run-villen-launcher.sh to pick the engine from the admin shell.
    - Launch from Game Mode. Confirm in ~/$DST/villen.log:
          filter: GPU backend ready: AMD Custom GPU 0405 (radeonsi, vangogh, ...)
      (radeonsi, NOT llvmpipe).
    - On a phone on the same (non-isolating) WiFi, open the printed
      http://<deck-host>:9002 , allow the camera, watch the morphology come back.
    Rollback if needed:  cp ~/$DST/villen.bak ~/$DST/villen
NEXT
