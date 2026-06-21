#!/usr/bin/env bash
# Self-signed TLS terminator so a phone gets a SECURE CONTEXT (HTTPS) for the
# camera while traffic stays on the LAN (no public tunnel). It is the interim
# answer to the filter engine's getUserMedia secure-context gate
# (docs/steamdeck-debugging.md §5.1); real TLS in the host is deferred
# (DESIGN-filter §16). Needs socat + openssl (both present on SteamOS).
#
# Forwards  https://<lan-ip>:$TLS_PORT  ->  the local villen server on $HTTP_PORT,
# byte-level, so HTTP and WebSocket (wss) both pass through unchanged. On the phone
# open https://<lan-ip>:$TLS_PORT and accept the one-time self-signed cert warning.
#
#   HTTP_PORT=9002 TLS_PORT=8443 tools/tls-proxy.sh
#
# The :8443 default matches TLS_PROXY_PORT in client/filter/filter.js, which links
# straight to this URL when it detects an insecure context.
set -euo pipefail

HTTP_PORT="${HTTP_PORT:-9002}"
TLS_PORT="${TLS_PORT:-8443}"
CERT="${CERT:-villen-tls.pem}"

command -v socat   >/dev/null || { echo "tls-proxy: socat not found"; exit 1; }
command -v openssl >/dev/null || { echo "tls-proxy: openssl not found"; exit 1; }

IP=$(ip route get 1.1.1.1 2>/dev/null | awk '{print $7; exit}') || true
IP="${IP:-127.0.0.1}"

if [ ! -f "$CERT" ]; then
    echo "tls-proxy: generating self-signed cert for $IP -> $CERT"
    tmpk=$(mktemp); tmpc=$(mktemp)
    openssl req -x509 -newkey rsa:2048 -nodes -days 825 \
        -keyout "$tmpk" -out "$tmpc" \
        -subj "/CN=$IP" -addext "subjectAltName=IP:$IP" >/dev/null 2>&1
    cat "$tmpc" "$tmpk" > "$CERT"
    rm -f "$tmpk" "$tmpc"
fi

echo "tls-proxy: https://$IP:$TLS_PORT  ->  127.0.0.1:$HTTP_PORT   (Ctrl-C to stop)"
exec socat OPENSSL-LISTEN:"$TLS_PORT",reuseaddr,fork,cert="$CERT",verify=0 \
           TCP:127.0.0.1:"$HTTP_PORT"
