#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/_pio.sh
# Prefer the BLE venv: it has pyserial (and bleak, unused here but harmless).
# Fall back to the PlatformIO venv (pyserial only) if it's absent.
if [ -x tools/blevenv/bin/python ]; then
    PYTHON=tools/blevenv/bin/python
else
    PYTHON="$(dirname "$PIO")/python"
fi
exec "$PYTHON" scripts/edge_test.py "$@"
