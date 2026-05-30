#!/usr/bin/env bash
# Resolve the pio binary. Sourced by other scripts.
if command -v pio >/dev/null 2>&1; then
    PIO=pio
elif [ -x "$HOME/.platformio/penv/bin/pio" ]; then
    PIO="$HOME/.platformio/penv/bin/pio"
else
    echo "error: pio not found on PATH or at ~/.platformio/penv/bin/pio" >&2
    exit 127
fi
export PIO
