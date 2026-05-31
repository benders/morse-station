#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/_pio.sh
# Regenerate compile_commands.json so clangd stays in sync, but only when the
# build graph could have changed: missing db, or platformio.ini / any source
# file is newer than the db. A plain rebuild skips this (~1s saved).
db=compile_commands.json
if [ ! -f "$db" ] || \
   [ platformio.ini -nt "$db" ] || \
   [ -n "$(find src -newer "$db" -type f 2>/dev/null -print -quit)" ]; then
    "$PIO" run -t compiledb "$@"
fi
exec "$PIO" run "$@"
