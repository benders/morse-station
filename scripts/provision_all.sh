#!/usr/bin/env bash
# Provision EVERY connected station at once over the live runtime serial console
# (no reset). See scripts/provision_all.py for options. Examples:
#   scripts/provision_all.sh --wpm 5 --farns 13 --call KC8HOB --mute on --keymode edge
#   scripts/provision_all.sh --show          # read back every board
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/_pio.sh
# Use the PlatformIO venv's python so pyserial is available without an extra install.
PYTHON="$(dirname "$PIO")/python"
exec "$PYTHON" scripts/provision_all.py "$@"
