#!/usr/bin/env python3
"""TX-power bench comparison for the fox-hunt stations.

Drives a 3-station bench test over USB serial (the reset-free runtime console,
same command set as the BLE NUS):

  * one station is the HUNTER (center), with `debug on` so it prints
    `RX K/E <src_id> ... rssi=<dbm>` for every packet it copies;
  * each FOX in turn is rebooted into Fox mode, then its TX power is swept
    LO/MED/HI/MAX *live* (the `pwr N` command retunes the SX1262 in place when
    the station is already a fox), and the hunter's per-packet RSSI for that
    source id is collected at each level.

Output is a table of median/min/max RSSI (dBm) and packet count per fox per
power level, so you can compare e.g. a Heltec (FEM) fox against the Wio (bare
SX1262) fox at matched chip power.

Run unattended:
  ~/.platformio/penv/bin/python scripts/tx_bench.py \
      --hunter 42 --foxes 43,115 --levels 0,1,2,3 --capture 12

Notes:
  * Foxes are returned to Hunter mode at the end so nothing keeps transmitting.
  * Only one fox transmits at a time.
  * RSSI here is raw chip getRSSI() (no noise-floor/SNR on this branch); for a
    same-hunter same-distance A/B it is a valid relative EIRP proxy.
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

RX_RE = re.compile(r"^RX [KE]\s+(\d+)\b.*\brssi=(-?\d+)")


def ports() -> list[str]:
    return sorted(glob.glob("/dev/cu.usbmodem*"))


def open_port(path: str) -> serial.Serial:
    return serial.Serial(path, BAUD, timeout=0.2)


def cmd(s: serial.Serial, line: str, read_t: float = 1.2) -> str:
    """Send one console line, return whatever comes back within read_t."""
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
        # Expected on `reboot`: the USB CDC drops mid-read (esp. the nRF52
        # boards, which fully re-enumerate). Return whatever we got.
        pass
    return "\n".join(out)


def identify() -> dict[int, str]:
    """Map station id -> port path via the reset-free `show` command."""
    m = {}
    for p in ports():
        try:
            s = open_port(p)
        except Exception as e:
            print(f"  ! {p}: open failed {e}", file=sys.stderr)
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
    """After a reboot the USB CDC re-enumerates; rescan until the id reappears."""
    end = time.time() + timeout
    while time.time() < end:
        time.sleep(1.0)
        m = identify()
        if station_id in m:
            return m[station_id]
    raise TimeoutError(f"station {station_id} did not come back within {timeout}s")


def set_mode_reboot(station_id: int, port: str, mode: int) -> str:
    """mode: 0=Hunter 1=Fox 2=Livekey. Persists, reboots, returns new port."""
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


def capture_rssi(hunter_port: str, src_id: int, secs: float) -> list[int]:
    """Read the hunter for `secs`, collect rssi for packets from src_id."""
    vals = []
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
                vals.append(int(mo.group(2)))
    finally:
        s.close()
    return vals


def summarize(vals: list[int]) -> str:
    if not vals:
        return "   n=0   (no packets)"
    return (f"   n={len(vals):<3} med={statistics.median(vals):>5.0f}  "
            f"min={min(vals):>4}  max={max(vals):>4}")


def sweep_fox(hunter_port: str, fox_id: int, fox_port: str,
              levels: list[int], capture: float, settle: float,
              pa: str = "keep") -> dict[int, list[int]]:
    res = {}
    s = open_port(fox_port)
    try:
        time.sleep(0.4)
        if pa in ("on", "off"):
            # V4 FEM PA toggle (testing aid). No-op / "n/a" on non-FEM foxes
            # (e.g. the Wio). Lets a bench A/B +22 (pa off) vs ~+28 (pa on).
            print(f"    [{fox_id}] pa {pa}: {cmd(s, f'pa {pa}').strip()}")
        for lv in levels:
            print(f"    [{fox_id}] pwr {lv} ({PWR_NAMES[lv]}): "
                  f"{cmd(s, f'pwr {lv}').strip()}")
            time.sleep(settle)
            s.close()  # free the port; capture reads the hunter only
            vals = capture_rssi(hunter_port, fox_id, capture)
            res[lv] = vals
            print(f"      fox {fox_id} @ {PWR_NAMES[lv]}:{summarize(vals)}")
            s = open_port(fox_port)
            time.sleep(0.3)
    finally:
        try:
            s.close()
        except Exception:
            pass
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--hunter", type=int, required=True)
    ap.add_argument("--foxes", required=True, help="comma list, e.g. 43,115")
    ap.add_argument("--levels", default="0,1,2,3")
    ap.add_argument("--capture", type=float, default=12.0)
    ap.add_argument("--settle", type=float, default=2.5)
    ap.add_argument("--pa", choices=["keep", "on", "off"], default="keep",
                    help="V4 FEM PA state for the sweep: off=+22dBm, on=~+28dBm "
                         "(no-op on non-FEM foxes). Run once each way and diff.")
    args = ap.parse_args()

    foxes = [int(x) for x in args.foxes.split(",")]
    levels = [int(x) for x in args.levels.split(",")]

    print("# identifying stations ...")
    m = identify()
    for sid, p in sorted(m.items()):
        print(f"  station {sid} -> {p}")
    if args.hunter not in m:
        print(f"!! hunter {args.hunter} not found", file=sys.stderr)
        return 2
    for f in foxes:
        if f not in m:
            print(f"!! fox {f} not found", file=sys.stderr)
            return 2

    # Hunter: ensure Hunter mode + debug on. (Assume already booted as hunter;
    # we do not reboot it, so debug stays on for the whole run.)
    hp = m[args.hunter]
    s = open_port(hp)
    try:
        time.sleep(0.4)
        print(f"# hunter {args.hunter}: {cmd(s, 'debug on').strip()}")
        print(f"# hunter {args.hunter}: {cmd(s, 'show').strip()}")
    finally:
        s.close()

    results: dict[int, dict[int, list[int]]] = {}
    for f in foxes:
        print(f"\n# === fox {f}: boot into Fox mode ===")
        fp = set_mode_reboot(f, m[f], 1)
        time.sleep(2.0)  # let it start transmitting
        results[f] = sweep_fox(hp, f, fp, levels, args.capture, args.settle,
                               pa=args.pa)
        print(f"# === fox {f}: back to Hunter mode ===")
        set_mode_reboot(f, fp, 0)

    # Summary table
    print("\n" + "=" * 60)
    print("TX POWER BENCH — median RSSI (dBm) at hunter "
          f"{args.hunter}, n=packets  [pa={args.pa}]")
    print("=" * 60)
    hdr = "level".ljust(10) + "".join(f"fox {f}".rjust(16) for f in foxes)
    print(hdr)
    for lv in levels:
        row = PWR_NAMES[lv].ljust(10)
        for f in foxes:
            vals = results[f].get(lv, [])
            if vals:
                cell = f"{statistics.median(vals):>5.0f} (n={len(vals)})"
            else:
                cell = "  -- (n=0)"
            row += cell.rjust(16)
        print(row)
    print("=" * 60)
    if len(foxes) == 2:
        print(f"\nDelta (fox {foxes[0]} - fox {foxes[1]}) median RSSI:")
        for lv in levels:
            a = results[foxes[0]].get(lv, [])
            b = results[foxes[1]].get(lv, [])
            if a and b:
                d = statistics.median(a) - statistics.median(b)
                print(f"  {PWR_NAMES[lv]:<10} {d:+.0f} dB")
            else:
                print(f"  {PWR_NAMES[lv]:<10} (missing data)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
