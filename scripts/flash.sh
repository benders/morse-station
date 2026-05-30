#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/_pio.sh
exec "$PIO" run -t upload "$@"
