#!/usr/bin/env python3
"""Fox keying-timebase test harness (TODO-fox-timing.md mitigation #1).

A variant of edge_test.py that provisions the fox over its *runtime* console
instead of the boot setup console. The Cardputer (native USB-CDC) resets and
re-enumerates the moment the host opens the port, so the ~2 s setup-console
window is gone before the host re-attaches and edge_test.py's
provision_over_setup() never sees "for setup console". The runtime console uses
the same command handler (serial_console_process) and accepts msg/wpm/farns/
mode/reboot live, so we drive it there and let `reboot` apply the
persisted mode.

Adds, on top of edge_test scoring, a `dn` distribution analysis of the hunter's
`RX E ... dn=` dump (the TX-measured duration of the segment that just ENDED) so
we can quantify keying jitter per the mitigation-#1 test plan:
  - histogram of dn for ~1-unit segments (intra-char gap is the failure locus),
  - worst-case dn for a 1-unit segment,
  - count of 1-unit segments stretched >= the char-gap threshold.

Usage:
  fox_timing_run.py --fox 73 --hunters 43 --wpm 18 --duration 300 \
      --log /tmp/fox-timing-baseline-card.log
"""
from __future__ import annotations

import argparse
import re
import sys
import threading
import time

import serial

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from edge_test import (  # noqa: E402
    Capture, RX_RE, decode_from_lines, resolve_ports, score_decode,
)

# RX E <sid> seq=<n> lvl=<0|1> hb=<0|1> dn=<ms> [dp=<ms>]
RXE_RE = re.compile(r"RX E (\d+) seq=(\d+) lvl=(\d+) hb=(\d+) dn=(\d+)")


def provision_fox_runtime(port: str, msg: str, wpm: int, farns: int | None) -> bool:
    """Provision the fox over the RUNTIME console, then reboot into Fox mode.

    The runtime console answers the same command set as the setup console; we
    avoid the native-USB reset-handshake entirely. `mode 1` persists Fox to NVS
    and `reboot` restarts the board into it."""
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:
        print(f"  [fox] error: cannot open {port}: {e}")
        return False
    try:
        # Let any boot/menu settle so the runtime console is live (opening the
        # port may have reset a native-USB board).
        time.sleep(13.0)
        s.reset_input_buffer()
        cmds = [f"msg {msg}", f"wpm {wpm}"]
        if farns is not None:
            cmds.append(f"farns {farns}")
        cmds += ["mode 1"]
        for c in cmds:
            print(f"  [fox] > {c}")
            s.write((c + "\n").encode())
            s.flush()
            # >= ~1.2s: the nRF52 (Wio/RAK) LittleFS NVS commit is slow; a 0.4s
            # gap lets `reboot` fire before wpm flush, reverting config.
            time.sleep(1.3)
            s.read(s.in_waiting or 1)
        print("  [fox] > reboot")
        s.write(b"reboot\n")
        s.flush()
        return True
    finally:
        try:
            s.close()
        except Exception:
            pass


def enable_debug_runtime(port: str) -> bool:
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:
        print(f"  [hunter] error: cannot open {port}: {e}")
        return False
    try:
        s.write(b"debug on\n")
        s.flush()
        time.sleep(0.3)
        s.read(s.in_waiting or 1)
        print(f"  [hunter] sent `debug on` ({port})")
        return True
    finally:
        try:
            s.close()
        except Exception:
            pass


def analyze_dn(lines: list[str], wpm: int) -> dict:
    """Bucket RX E dn values. `dn` is the duration of the segment that ENDED;
    for a key-up (lvl=1) edge that ended segment was a GAP, for a key-down
    (lvl=0) edge it was an element. Heartbeats (hb=1) carry elapsed-so-far, not
    a completed segment, so exclude them.

    At `wpm`, one unit = 1200/wpm ms. We classify each GAP as intra-char (~1
    unit), char-gap (~3 units) or word-gap (~7 units) by NEAREST unit-multiple,
    not by a crude single threshold (a clean 3-unit char gap is NOT a stretched
    1-unit gap). The failure locus is a 1-unit intra-char gap getting *stretched*
    toward/over the decoder's char-gap threshold (~2 units): so among gaps whose
    nearest multiple is 1 unit, we report the worst dn and count any measured at
    >= 2 units (which the decoder would mis-read as a char gap)."""
    unit = 1200.0 / wpm
    twounit = 2.0 * unit
    gaps_1unit = []     # gaps whose nearest unit-multiple is 1 unit (intra-char)
    all_gaps = []
    all_elems = []
    stretched = 0       # 1-unit gaps measured >= 2 units (would mis-decode)
    for text in lines:
        m = RXE_RE.search(text)
        if not m:
            continue
        lvl = int(m.group(3))
        hb = int(m.group(4))
        dn = int(m.group(5))
        if hb:
            continue
        if lvl == 1:        # entered key-up -> the segment that ended was a GAP
            all_gaps.append(dn)
            nearest_units = round(dn / unit) if unit else 0
            if nearest_units <= 1:   # nominal intra-char gap (1 unit)
                gaps_1unit.append(dn)
                if dn >= twounit:    # stretched into char-gap territory
                    stretched += 1
        else:               # entered key-down -> the segment that ended was an ELEMENT
            all_elems.append(dn)

    def stats(xs):
        if not xs:
            return {"n": 0}
        xs2 = sorted(xs)
        return {
            "n": len(xs2),
            "min": xs2[0],
            "max": xs2[-1],
            "median": xs2[len(xs2) // 2],
        }

    # Honest distribution: count of each distinct dn value (proves quantization).
    from collections import Counter
    hist = dict(sorted(Counter(
        int(RXE_RE.search(t).group(5)) for t in lines
        if RXE_RE.search(t) and int(RXE_RE.search(t).group(4)) == 0
    ).items()))

    return {
        "unit_ms": unit,
        "twounit_ms": twounit,
        "gaps_1unit": stats(gaps_1unit),
        "gaps_1unit_worst": max(gaps_1unit) if gaps_1unit else None,
        "stretched_1unit": stretched,
        "all_gaps": stats(all_gaps),
        "all_elems": stats(all_elems),
        "dn_hist": hist,
    }


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--fox", type=int, required=True)
    p.add_argument("--hunters", required=True)
    p.add_argument("--msg", default="PARIS PARIS PARIS")
    p.add_argument("--wpm", type=int, default=18)
    p.add_argument("--farns", type=int, default=None)
    p.add_argument("--duration", type=float, default=300.0)
    p.add_argument("--log", default=None)
    p.add_argument("--pass-ratio", type=float, default=0.6)
    args = p.parse_args()

    hunter_ids = [int(x) for x in args.hunters.split(",") if x.strip()]
    log_path = args.log or f"/tmp/fox-timing-{int(time.time())}.log"
    print(f"# fox_timing_run: fox={args.fox} hunters={hunter_ids} "
          f"msg={args.msg!r} wpm={args.wpm} "
          f"duration={args.duration}s log={log_path}")

    ports = resolve_ports([args.fox] + hunter_ids)

    print(f"\n# provisioning fox {args.fox} @ {ports[args.fox]} (runtime console)")
    if not provision_fox_runtime(ports[args.fox], args.msg, args.wpm,
                                 args.farns):
        sys.stderr.write("error: fox provisioning failed; aborting\n")
        return 2

    print("\n# waiting 14s for fox reboot into Fox mode...")
    time.sleep(14.0)

    print("\n# enabling debug on hunters")
    for sid in hunter_ids:
        enable_debug_runtime(ports[sid])

    print(f"\n# capturing {args.duration}s -> {log_path}")
    log_lock = threading.Lock()
    captures = {}
    with open(log_path, "w") as log_fh:
        for sid in hunter_ids:
            cap = Capture(sid, ports[sid], f"hunter {sid}")
            captures[sid] = cap
            cap.start(args.duration, log_lock, log_fh)
        for sid in hunter_ids:
            captures[sid].join()

    print(f"\n# === results (expected {args.msg!r}, "
          f"pass-ratio >= {args.pass_ratio}) ===")
    all_pass = True
    for sid in hunter_ids:
        cap = captures[sid]
        print(f"\n--- hunter {sid} ({ports[sid]}) ---")
        if cap.error:
            print(f"  FAIL: capture error -- {cap.error}")
            all_pass = False
            continue
        decoded = decode_from_lines(cap.lines)
        ratio = score_decode(decoded, args.msg)
        verdict = "PASS" if ratio >= args.pass_ratio else "FAIL"
        if verdict == "FAIL":
            all_pass = False
        print(f"  decoded : {decoded!r}")
        print(f"  ratio   : {ratio:.2f}  ->  {verdict}")
        dn = analyze_dn(cap.lines, args.wpm)
        print(f"  dn analysis (unit={dn['unit_ms']:.1f}ms, "
              f"2-unit={dn['twounit_ms']:.1f}ms):")
        print(f"    1-unit gaps (intra-char): {dn['gaps_1unit']}")
        print(f"    worst 1-unit gap dn     : {dn['gaps_1unit_worst']} ms")
        print(f"    stretched (>= 2 units)  : {dn['stretched_1unit']}")
        print(f"    all gaps                : {dn['all_gaps']}")
        print(f"    all elements            : {dn['all_elems']}")
        print(f"    dn histogram (non-hb)   : {dn['dn_hist']}")
        rx = [l for l in cap.lines if RX_RE.search(l)]
        print(f"    RX E/K lines captured   : {len(rx)}")

    print(f"\nSUMMARY: {'PASS' if all_pass else 'FAIL'} log={log_path}")
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
