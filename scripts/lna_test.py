#!/usr/bin/env python3
"""A/B the V4.3 RX LNA (CSD select) on a hunter while a fox transmits.

Fox -> Hunter over USB serial (reset-free runtime console, same as tx_bench):
  * boot the fox into Fox mode and hold it at a fixed TX power (default LO so
    the link is NOT saturated -- the LNA's contribution is only visible when
    there is headroom below the RSSI ceiling);
  * on the hunter, `debug on` so it prints `RX E <src> seq=<n> ... rssi=<dbm>`;
  * alternate the hunter's `lna on` / `lna off` across several rounds, capturing
    per-packet rssi + seq for each window, then report median rssi, packet
    count (a rough rate proxy) and seq-gap loss per state.

Alternating (not one long on then one long off) averages out slow fading so the
on/off delta isn't an artifact of drift.

  ~/.platformio/penv/bin/python scripts/lna_test.py \
      --fox 42 --hunter 43 --pwr 0 --rounds 3 --capture 10

Restores: lna on, fox back to Hunter mode.
"""
from __future__ import annotations

import argparse
import glob
import re
import statistics
import sys
import time

import serial  # PlatformIO venv

BAUD = 115200
PWR_NAMES = {0: "LO(-9)", 1: "MED(+2)", 2: "HI(+14)", 3: "MAX(+22)"}
# RX E 42 seq=12 lvl=1 hb=0 dn=240 dp=0 rssi=-41   (also tolerates RX K)
RX_RE = re.compile(r"^RX [KE]\s+(\d+)\b.*\bseq=(\d+)\b.*\brssi=(-?\d+)")


def ports() -> list[str]:
    return sorted(glob.glob("/dev/cu.usbmodem*"))


def open_port(path: str) -> serial.Serial:
    return serial.Serial(path, BAUD, timeout=0.2)


def cmd(s: serial.Serial, line: str, read_t: float = 1.2) -> str:
    out = []
    try:
        s.reset_input_buffer()
        s.write((line + "\n").encode())
        s.flush()
        end = time.time() + read_t
        while time.time() < end:
            ln = s.readline()
            if ln:
                out.append(ln.decode("utf-8", "replace").rstrip())
    except (serial.SerialException, OSError):
        pass
    return "\n".join(out)


def identify() -> dict[int, str]:
    m = {}
    for p in ports():
        try:
            s = open_port(p)
        except Exception:
            continue
        try:
            time.sleep(0.5)
            r = cmd(s, "show", 1.5)
            mo = re.search(r"\bid=(\d+)", r)
            if mo:
                m[int(mo.group(1))] = p
        finally:
            s.close()
    return m


def wait_for_station(station_id: int, timeout: float = 25.0) -> str:
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(1.0)
        m = identify()
        if station_id in m:
            return m[station_id]
    raise TimeoutError(f"station {station_id} did not come back within {timeout}s")


def set_mode_reboot(station_id: int, port: str, mode: int) -> str:
    s = open_port(port)
    try:
        time.sleep(0.4)
        print(f"    [{station_id}] mode {mode}: {cmd(s, f'mode {mode}').strip()}")
        cmd(s, "reboot", 0.6)
    finally:
        try:
            s.close()
        except Exception:
            pass
    return wait_for_station(station_id)


def capture(hunter_port: str, src_id: int, secs: float) -> tuple[list[int], list[int]]:
    """Return (rssi_list, seq_list) for packets from src_id over `secs`."""
    rssis, seqs = [], []
    s = open_port(hunter_port)
    try:
        s.reset_input_buffer()
        end = time.time() + secs
        while time.time() < end:
            ln = s.readline()
            if not ln:
                continue
            mo = RX_RE.match(ln.decode("utf-8", "replace").strip())
            if mo and int(mo.group(1)) == src_id:
                seqs.append(int(mo.group(2)))
                rssis.append(int(mo.group(3)))
    finally:
        s.close()
    return rssis, seqs


def loss_pct(seqs: list[int]) -> float:
    """Rough loss over the received span, handling 8-bit seq wrap."""
    if len(seqs) < 2:
        return float("nan")
    span = 1
    for a, b in zip(seqs, seqs[1:]):
        d = (b - a) % 256
        span += d if d else 1  # d==0 -> duplicate/no advance, count as 1 slot
    got = len(seqs)
    return 100.0 * (1.0 - got / span) if span else float("nan")


def summarize(rssis: list[int], seqs: list[int]) -> str:
    if not rssis:
        return "n=0  (no packets)"
    return (f"n={len(rssis):<3} med={statistics.median(rssis):>5.0f} "
            f"min={min(rssis):>4} max={max(rssis):>4}  loss={loss_pct(seqs):4.0f}%")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fox", type=int, required=True)
    ap.add_argument("--hunter", type=int, required=True)
    ap.add_argument("--pwr", type=int, default=0, help="fox TX level 0..3 (default 0=LO)")
    ap.add_argument("--rounds", type=int, default=3)
    ap.add_argument("--capture", type=float, default=10.0)
    ap.add_argument("--settle", type=float, default=1.5)
    args = ap.parse_args()

    print("# identifying stations ...")
    m = identify()
    for sid, p in sorted(m.items()):
        print(f"  station {sid} -> {p}")
    for need in (args.fox, args.hunter):
        if need not in m:
            print(f"!! station {need} not found", file=sys.stderr)
            return 2

    hp = m[args.hunter]
    s = open_port(hp)
    try:
        time.sleep(0.4)
        print(f"# hunter {args.hunter}: {cmd(s, 'debug on').strip()}")
        print(f"# hunter {args.hunter}: {cmd(s, 'show').strip()}")
    finally:
        s.close()

    print(f"\n# === fox {args.fox}: boot into Fox mode @ {PWR_NAMES[args.pwr]} ===")
    fp = set_mode_reboot(args.fox, m[args.fox], 1)
    time.sleep(2.0)
    s = open_port(fp)
    try:
        print(f"    [{args.fox}] pwr {args.pwr}: {cmd(s, f'pwr {args.pwr}').strip()}")
    finally:
        s.close()

    agg = {"on": ([], []), "off": ([], [])}
    print(f"\n# alternating lna on/off, {args.rounds} rounds x {args.capture:.0f}s")
    for r in range(args.rounds):
        for state in ("on", "off"):
            s = open_port(hp)
            try:
                cmd(s, f"lna {state}")
            finally:
                s.close()
            time.sleep(args.settle)
            rs, sq = capture(hp, args.fox, args.capture)
            agg[state][0].extend(rs)
            agg[state][1].extend(sq)
            print(f"  round {r+1} lna {state:<3}: {summarize(rs, sq)}")

    # restore
    s = open_port(hp)
    try:
        cmd(s, "lna on")
    finally:
        s.close()
    print(f"\n# === fox {args.fox}: back to Hunter mode ===")
    set_mode_reboot(args.fox, fp, 0)

    print("\n" + "=" * 56)
    print(f"LNA A/B @ hunter {args.hunter}, fox {args.fox} {PWR_NAMES[args.pwr]} "
          f"(aggregated over {args.rounds} rounds)")
    print("=" * 56)
    for state in ("on", "off"):
        rs, sq = agg[state]
        print(f"  lna {state:<3}: {summarize(rs, sq)}")
    if agg["on"][0] and agg["off"][0]:
        d = statistics.median(agg["on"][0]) - statistics.median(agg["off"][0])
        print(f"\n  median RSSI delta (on - off) = {d:+.0f} dB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
