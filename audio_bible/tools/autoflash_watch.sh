#!/bin/bash
# Wait for the ESP32-S3 USB port to reappear, then flash the current build and
# capture serial logs once. Lets us catch a brief device reconnection while the
# user is away. Writes results to /tmp/auto_*.log and exits.
#
# Usage: autoflash_watch.sh [max_wait_seconds]
set -u
PROJ="/Users/sabinjose/Developer/Projects/Embedded/esp32-tiny-bible/audio_bible"
MAX="${1:-10800}"   # default 3h
source ~/esp/esp-idf-v5.3.2/export.sh >/dev/null 2>&1
cd "$PROJ" || exit 1

elapsed=0
echo "[watch] waiting up to ${MAX}s for /dev/cu.usbmodem*" > /tmp/auto_status.log
while [ "$elapsed" -lt "$MAX" ]; do
  PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
  if [ -n "${PORT:-}" ]; then
    echo "[watch] device appeared at $PORT after ${elapsed}s" >> /tmp/auto_status.log
    sleep 2
    idf.py -p "$PORT" flash > /tmp/auto_flash.log 2>&1
    rc=$?
    echo "[watch] flash rc=$rc" >> /tmp/auto_status.log
    if [ "$rc" -eq 0 ]; then
      python tools/read_serial.py "$PORT" 30 > /tmp/auto_serial.log 2>&1
      echo "[watch] captured serial ($(wc -l < /tmp/auto_serial.log) lines)" >> /tmp/auto_status.log
    fi
    echo "[watch] done" >> /tmp/auto_status.log
    exit 0
  fi
  sleep 5
  elapsed=$((elapsed + 5))
done
echo "[watch] timed out after ${MAX}s, device never appeared" >> /tmp/auto_status.log
