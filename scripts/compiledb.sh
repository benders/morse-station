#!/usr/bin/env bash
# Regenerate compile_commands.json so clangd sees the ESP32 toolchain include
# paths. Run this after changing platformio.ini or lib_deps.
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/_pio.sh
exec "$PIO" run -t compiledb
