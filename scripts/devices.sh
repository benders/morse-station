#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/_pio.sh
# Prefer the BLE venv: it has both pyserial AND bleak, so --ble works too. Fall
# back to the PlatformIO venv (pyserial only; --ble unavailable) if it's absent.
if [ -x tools/blevenv/bin/python ]; then
    PYTHON=tools/blevenv/bin/python
else
    PYTHON="$(dirname "$PIO")/python"
fi
exec "$PYTHON" scripts/devices.py "$@"
