#!/usr/bin/env bash
# Provision a unit's NVS settings (callsign / fox message / station id) over
# serial. See scripts/provision.py for options. Examples:
#   scripts/provision.sh --call N0CALL --msg "DE N0CALL FOX BY THE OAK" --id 5
#   scripts/provision.sh --show
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/_pio.sh
# Use the PlatformIO venv's python so pyserial is available without an extra install.
PYTHON="$(dirname "$PIO")/python"
exec "$PYTHON" scripts/provision.py "$@"
