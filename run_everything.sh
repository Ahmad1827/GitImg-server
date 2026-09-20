#!/bin/bash
set -e
cd ~/proiecte/gitimg

killall -9 gitimgd cloudflared 2>/dev/null || true
fuser -k -9 8080/tcp 2>/dev/null || true

WSL_IP=$(hostname -I | awk '{print $1}')
if [ -f /mnt/c/Windows/System32/netsh.exe ] && [ -n "$WSL_IP" ]; then
    /mnt/c/Windows/System32/netsh.exe interface portproxy delete v4tov4 listenport=8080 listenaddress=0.0.0.0 >/dev/null 2>&1 || true
    /mnt/c/Windows/System32/netsh.exe interface portproxy add v4tov4 listenport=8080 listenaddress=0.0.0.0 connectport=8080 connectaddress="$WSL_IP" >/dev/null 2>&1 || true
fi

if [ ! -f gitimgd ] || [ ! -f gitimg ]; then
    make
fi

cat << 'CONF' > server.conf
BIND_HOST=0.0.0.0
PORT=8080
STORAGE_DIR=.gitimgd
CONF

mkdir -p .gitimgd/users .gitimgd/sessions .gitimgd/repos .gitimgd/manifests .gitimgd/commits .gitimgd/objects .gitimgd/thumbnails

nohup ./gitimgd server.conf > server.log 2>&1 &

for i in {1..10}; do
    if curl -s http://127.0.0.1:8080/health | grep -q "healthy"; then
        break
    fi
    sleep 0.5
done

if [ ! -f .gitimgd/users/ahmad.user ]; then
    curl -s -X POST http://127.0.0.1:8080/auth/register \
      -H "Content-Type: application/json" \
      -d '{"username":"ahmad","email":"ahmad@example.com","password":"mypassword123"}' > /dev/null
fi

./gitimg login ahmad mypassword123 127.0.0.1:8080 > /dev/null 2>&1

rm -f cloudflared.log
nohup cloudflared tunnel --url http://127.0.0.1:8080 > cloudflared.log 2>&1 &

TUNNEL_URL=""
for i in {1..20}; do
    TUNNEL_URL=$(grep -o 'https://[-a-zA-Z0-9.]*\.trycloudflare\.com' cloudflared.log 2>/dev/null | head -n 1 || true)
    if [ -n "$TUNNEL_URL" ]; then
        break
    fi
    sleep 0.5
done

echo ""
echo "=== GitImg Stack Started ==="
echo "WSL Internal IP: $WSL_IP"
echo "Local Daemon: http://127.0.0.1:8080"
if [ -n "$TUNNEL_URL" ]; then
    echo "Cloudflare Tunnel: $TUNNEL_URL"
    echo "Web Dashboard: https://ahmad1827.github.io/gitimg/?user=ahmad&daemon=$TUNNEL_URL"
else
    echo "Tunnel pending. Check 'cat ~/proiecte/gitimg/cloudflared.log' for the URL."
fi
echo "============================"
