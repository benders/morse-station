#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/_pio.sh
# Use the PlatformIO venv's python so pyserial is available without an extra install.
PYTHON="$(dirname "$PIO")/python"
exec "$PYTHON" scripts/monitor.py "$@"
