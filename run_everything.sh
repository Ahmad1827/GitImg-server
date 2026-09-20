#!/bin/bash
set -e
cd ~/proiecte/gitimg

make clean && make

killall -9 gitimgd 2>/dev/null || true
fuser -k -9 8080/tcp 2>/dev/null || true

cat << 'CONF' > server.conf
BIND_HOST=0.0.0.0
PORT=8080
STORAGE_DIR=.gitimgd
CONF

nohup ./gitimgd server.conf > server.log 2>&1 &
sleep 2

mkdir -p ~/ArtTest
cd ~/ArtTest

wget -q -O concept.png "https://www.google.com/images/branding/googlelogo/1x/googlelogo_color_272x92dp.png"

~/proiecte/gitimg/gitimg push "Initial concept frame"

echo "=== Human-readable log ==="
~/proiecte/gitimg/gitimg log

echo "=== JSON log for WisdomPark ==="
~/proiecte/gitimg/gitimg log --json
