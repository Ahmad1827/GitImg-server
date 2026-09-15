#!/usr/bin/env bash

fuser -k 8080/tcp 2>/dev/null

cd "$(dirname "$0")"

./gitimgd server.conf > gitimgd.log 2>&1 &
DAEMON_PID=$!

cloudflared tunnel --url http://127.0.0.1:8080 > cloudflared.log 2>&1 &
CF_PID=$!

cleanup() {
    echo ""
    echo "Shutting down GitImg daemon and tunnel..."
    kill $DAEMON_PID 2>/dev/null
    kill $CF_PID 2>/dev/null
    fuser -k 8080/tcp 2>/dev/null
    exit 0
}
trap cleanup SIGINT SIGTERM EXIT

echo "Starting services and acquiring tunnel endpoint..."

TUNNEL_URL=""
for i in {1..30}; do
    TUNNEL_URL=$(grep -o 'https://[a-zA-Z0-9-]\+\.trycloudflare\.com' cloudflared.log | head -n 1)
    if [ -n "$TUNNEL_URL" ]; then
        break
    fi
    sleep 1
done

if [ -z "$TUNNEL_URL" ]; then
    echo "Failed to acquire Cloudflare Tunnel URL. Check cloudflared.log for details."
    exit 1
fi

DASHBOARD_URL="https://ahmad1827.github.io/gitimg/?daemon=${TUNNEL_URL}"

echo "=========================================================="
echo " Daemon PID:    $DAEMON_PID"
echo " Tunnel URL:    $TUNNEL_URL"
echo " Dashboard:     $DASHBOARD_URL"
echo "=========================================================="
echo "Launching live dashboard..."

if command -v explorer.exe >/dev/null 2>&1; then
    explorer.exe "$DASHBOARD_URL"
elif command -v wslview >/dev/null 2>&1; then
    wslview "$DASHBOARD_URL"
fi

echo "GitImg is live. Press Ctrl+C to terminate all services."
wait $CF_PID
