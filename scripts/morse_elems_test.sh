#!/usr/bin/env bash
# Host unit test for morse::Player::elems_elapsed() across resync() seeks
# (field note §8 freeze regression). The per-element display reveal slaves to
# elems_elapsed() instead of a free-running edge counter: a sync-beacon resync()
# can jump the render forward OR back, and a counter would over/under-count
# across the jump. A BACKWARD seek under the old counter pushed it past the total
# element count, so the reveal cursor ran off the string end and the dit/dah line
# FROZE while the tone kept beeping (observed on every hunter). This test proves
# elems_elapsed() stays in [0, total] and tracks position across seeks.
#
# Pure host build (no hardware): stubs Arduino.h's millis() and compiles
# src/morse.cpp directly. Run: bash scripts/morse_elems_test.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
mkdir -p "$TMP/stub"
cat > "$TMP/stub/Arduino.h" <<'EOF'
#pragma once
#include <stdint.h>
static inline unsigned long millis() { return 0; }
EOF
cat > "$TMP/t.cpp" <<'EOF'
#include "morse.h"
#include <cstdio>
int main(){
    morse::Player p; p.begin(5,13); p.start("SOS");   // 9 elements: ... --- ...
    int fails = 0;

    p.resync(0, 9999999);
    int tot = p.elems_elapsed();
    printf("total elems (finished) = %d (expect 9)\n", tot);
    if (tot != 9) { printf("FAIL total\n"); fails++; }

    int prev = -1;
    for (uint32_t pos = 0; pos <= 8000; pos += 25){
        p.resync(0, pos);
        int e = p.elems_elapsed();
        if (e < 0 || e > 9){ printf("FAIL bound pos=%u e=%d\n",pos,e); fails++; break; }
        if (e < prev){ printf("FAIL non-monotonic pos=%u e=%d prev=%d\n",pos,e,prev); fails++; break; }
        prev = e;
    }
    printf("forward sweep: monotonic, max e=%d\n", prev);

    // freeze regression: a backward seek MUST drop the count (a free-running
    // counter would keep climbing past the total -> cursor overshoot -> freeze).
    p.resync(0, 9999999); int hi = p.elems_elapsed();
    p.resync(0, 50);      int lo = p.elems_elapsed();
    printf("after end e=%d, after backward-seek e=%d\n", hi, lo);
    if (!(hi == 9 && lo < hi && lo >= 0)){ printf("FAIL backward-seek\n"); fails++; }

    p.resync(0, 100000); if (p.elems_elapsed() > 9){ printf("FAIL overshoot\n"); fails++; }

    printf(fails ? "\nSOME FAILED (%d)\n" : "\nALL PASS\n", fails);
    return fails ? 1 : 0;
}
EOF
g++ -std=c++17 -I "$ROOT/src" -I "$TMP/stub" "$TMP/t.cpp" "$ROOT/src/morse.cpp" -o "$TMP/t"
"$TMP/t"
