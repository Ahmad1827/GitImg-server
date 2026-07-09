#!/bin/bash

echo "=== GitImg Automated Launcher ==="

# 1. Kill old servers and critically WAIT for Linux to release Port 8080
echo "[1] Shutting down old background servers..."
killall -9 gitimgd 2>/dev/null
fuser -k -9 8080/tcp 2>/dev/null
sleep 3  

# 2. Start the server
echo "[2] Starting new Server daemon..."
./gitimgd server.conf > server.log 2>&1 &
SERVER_PID=$!
sleep 2  

# 3. Verify server survived the boot
if ! kill -0 $SERVER_PID 2>/dev/null; then
    echo "CRITICAL ERROR: Server crashed instantly. Port 8080 might still be blocked."
    echo "Server Error Log:"
    cat server.log
    exit 1
fi
echo "[3] Server successfully running on Port 8080 (PID: $SERVER_PID)!"

# 4. Connect the client and push
echo "[4] Registering User and Pushing Artwork..."
mkdir -p ~/ArtTest && cd ~/ArtTest

# Log in (this triggers the Auto-Register magic we just added)
~/proiecte/gitimg/gitimg login ahmad mypassword123 127.0.0.1:8080

# Push the art
~/proiecte/gitimg/gitimg push "Uploaded character concept"

echo "=========================================="
echo "SUCCESS! Everything is fully operational."
echo "Keep this terminal open to keep the server alive."
echo "Open your browser to: http://127.0.0.1:8080/ahmad/ArtTest"
echo "=========================================="

# Keep the script alive holding the background server
wait $SERVER_PID
