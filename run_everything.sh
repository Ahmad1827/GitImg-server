#!/bin/bash
cd ~/proiecte/gitimg

make clean && make

killall -9 gitimgd 2>/dev/null
fuser -k -9 8080/tcp 2>/dev/null

cat << 'EOF' > server.conf
BIND_HOST=0.0.0.0
PORT=8080
STORAGE_DIR=.gitimgd
EOF

nohup ./gitimgd server.conf > server.log 2>&1 &
sleep 2

mkdir -p ~/ArtTest
cd ~/ArtTest

wget -q -O concept.jpg "https://www.google.com/images/branding/googlelogo/1x/googlelogo_color_272x92dp.png"

~/proiecte/gitimg/gitimg login ahmad mypassword123 127.0.0.1:8080

~/proiecte/gitimg/gitimg push "Uploaded character concept"