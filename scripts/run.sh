#!/usr/bin/env bash
# Flash, wait for the board's USB CDC to re-enumerate, then monitor.
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/_pio.sh

"$PIO" run -t upload

# After flashing, the ESP32-S3 native USB CDC disappears briefly during reset
# and the kernel re-enumerates it. Wait up to ~5s for it to come back.
for i in $(seq 1 50); do
    if compgen -G "/dev/cu.usbmodem*" > /dev/null; then
        break
    fi
    sleep 0.1
done
sleep 0.3  # small extra grace so the firmware's Serial.begin has time

PYTHON="$(dirname "$PIO")/python"
exec "$PYTHON" scripts/monitor.py --until DONE --duration 15 "$@"
