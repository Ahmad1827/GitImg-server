#!/bin/bash
WSL_IP=$(hostname -I | awk '{print $1}')
powershell.exe -Command "Start-Process powershell -Verb RunAs -ArgumentList '-NoProfile -WindowStyle Hidden -Command netsh interface portproxy delete v4tov4 listenport=8080 listenaddress=0.0.0.0; netsh interface portproxy add v4tov4 listenport=8080 listenaddress=0.0.0.0 connectport=8080 connectaddress=$WSL_IP'"
echo "Mapped Windows :8080 -> WSL $WSL_IP:8080"

# Replace this with your actual server command:
~/proiecte/gitimg/run_everything.sh
