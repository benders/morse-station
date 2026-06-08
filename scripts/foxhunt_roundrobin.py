#!/usr/bin/env python3
"""Round-robin fox-hunt link test over USB serial (edge protocol).

Each station in the set takes a turn as the FOX for a fixed window (default
60 s) while every OTHER station listens as a HUNTER. All ports are captured
concurrently (one reader thread each):

  * the FOX, with `debug on`, prints `TX E seq=... hb=...` for every edge
    packet it transmits  -> the "sent" count;
  * each HUNTER, with `debug on`, prints
    `RX E <fox> seq=... lvl=... hb=... dn=... dp=... rssi=<dbm>` for every
    edge packet it copies -> the "received" count + per-packet RSSI.

Packet loss for a (fox, hunter) pair is therefore 1 - received/sent over the
same window — a direct measure, not a seq-gap estimate (edge seq is only 8-bit
and wraps). RSSI is the hunter's raw chip getRSSI() (a valid relative proxy at
matched distance; absolute values are NOT comparable across receiver families —
e.g. nRF52 boards read lower than Heltec, see the RSSI-offset note).

All stations are configured for the **edge** keymode and identical traffic
(message/wpm) up front so only the radio path differs between rounds.

Raw per-packet rows are written to CSV; a summary table is printed at the end.
Every station is returned to **Hunter** mode before exit.

Usage:
  ~/.platformio/penv/bin/python scripts/foxhunt_roundrobin.py \
      --stations 42,43,115,26 --duration 60 --pwr 0 --csv foxhunt.csv
"""
from __future__ import annotations
import argparse
import csv
import glob
import re
import statistics
import sys
import threading
import time
import serial  # PlatformIO venv

BAUD = 115200

TX_RE = re.compile(r"^TX E seq=(\d+) lvl=(\d+) hb=(\d+)")
RX_RE = re.compile(
    r"^RX E (\d+) seq=(\d+) lvl=(\d+) hb=(\d+) dn=\d+ dp=\d+ rssi=(-?\d+)")


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
        pass  # USB CDC drops on reboot (esp. nRF52 re-enumerate) — return what we got
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
            time.sleep(0.4)
            r = cmd(s, "show", 1.5)
            mo = re.search(r"\bid=(\d+)", r)
            if mo:
                m[int(mo.group(1))] = p
        finally:
            s.close()
    return m


def wait_for_station(station_id: int, timeout: float = 30.0) -> str:
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


def configure(station_id: int, port: str, msg: str, wpm: int, pwr: int) -> None:
    """Persist edge keymode + identical traffic on a station (no reboot)."""
    s = open_port(port)
    try:
        time.sleep(0.4)
        for c in (f"keymode edge", f"wpm {wpm}", f"msg {msg}", f"pwr {pwr}"):
            print(f"    [{station_id}] {c}: {cmd(s, c).strip()}")
    finally:
        s.close()


# --- concurrent capture --------------------------------------------------

def reader(port: str, observer_id: int, fox_id: int, duration: float,
           rows: list, lock: threading.Lock) -> None:
    """Capture one port for `duration` s, parse TX/RX edge lines into `rows`."""
    try:
        s = open_port(port)
    except Exception as e:
        print(f"  ! reader {port} open failed: {e}", file=sys.stderr)
        return
    local = []
    try:
        s.reset_input_buffer()
        end = time.time() + duration
        while time.time() < end:
            ln = s.readline()
            if not ln:
                continue
            t = time.time()
            line = ln.decode("utf-8", "replace").strip()
            mt = TX_RE.match(line)
            if mt and observer_id == fox_id:
                # fox's own transmit log
                local.append((f"{t:.3f}", fox_id, "tx", observer_id,
                              int(mt.group(1)), int(mt.group(3)),
                              int(mt.group(2)), ""))
                continue
            mr = RX_RE.match(line)
            if mr and int(mr.group(1)) == fox_id and observer_id != fox_id:
                local.append((f"{t:.3f}", fox_id, "rx", observer_id,
                              int(mr.group(2)), int(mr.group(4)),
                              int(mr.group(3)), int(mr.group(5))))
    finally:
        s.close()
    with lock:
        rows.extend(local)


def run_round(fox_id: int, ids: list[int], idmap: dict[int, str],
              duration: float, rows: list) -> dict[int, str]:
    """One fox-round: flip fox to Fox mode, capture all ports, restore map."""
    print(f"\n=== FOX {fox_id} ===")
    idmap[fox_id] = set_mode_reboot(fox_id, idmap[fox_id], 1)  # -> Fox
    idmap = identify()  # refresh: reboot may re-enumerate ports

    # debug on everywhere (runtime-only; the fox just rebooted so re-assert all)
    for sid in ids:
        s = open_port(idmap[sid])
        try:
            time.sleep(0.3)
            cmd(s, "debug on", 0.6)
        finally:
            s.close()

    lock = threading.Lock()
    threads = []
    for sid in ids:
        t = threading.Thread(target=reader,
                             args=(idmap[sid], sid, fox_id, duration, rows, lock))
        t.start()
        threads.append(t)
    print(f"    capturing {duration:.0f}s on {len(ids)} ports ...")
    for t in threads:
        t.join()

    idmap[fox_id] = set_mode_reboot(fox_id, idmap[fox_id], 0)  # back to Hunter
    return identify()


def summarize(rows: list, ids: list[int]) -> None:
    sent = {f: 0 for f in ids}
    recv: dict[tuple[int, int], list[int]] = {}
    for (_, fox, role, obs, seq, hb, lvl, rssi) in rows:
        if role == "tx":
            sent[fox] += 1
        else:
            recv.setdefault((fox, obs), []).append(int(rssi))

    print("\n" + "=" * 68)
    print("ROUND-ROBIN FOX-HUNT — edge protocol, packet loss + RSSI")
    print("=" * 68)
    print(f"{'fox':>4} {'hunter':>7} {'sent':>6} {'recv':>6} {'loss%':>7} "
          f"{'rssi med':>9} {'min':>5} {'max':>5}")
    print("-" * 68)
    for fox in ids:
        for hunter in ids:
            if hunter == fox:
                continue
            n_sent = sent[fox]
            vals = recv.get((fox, hunter), [])
            n_recv = len(vals)
            loss = (1 - n_recv / n_sent) * 100 if n_sent else float("nan")
            if vals:
                med, lo, hi = statistics.median(vals), min(vals), max(vals)
                rs = f"{med:>9.0f} {lo:>5} {hi:>5}"
            else:
                rs = f"{'—':>9} {'—':>5} {'—':>5}"
            print(f"{fox:>4} {hunter:>7} {n_sent:>6} {n_recv:>6} "
                  f"{loss:>7.1f} {rs}")
    print("=" * 68)
    print("loss% = 1 - recv/sent (fox TX E vs hunter RX E, same window).")
    print("rssi = hunter raw chip dBm; NOT comparable across receiver families")
    print("(nRF52 reads lower than Heltec — relative within a column only).")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--stations", required=True,
                    help="comma list of station ids, e.g. 42,43,115,26")
    ap.add_argument("--duration", type=float, default=60.0,
                    help="seconds each fox transmits (default 60)")
    ap.add_argument("--pwr", type=int, default=0,
                    help="fox TX power index 0..3 (default 0=LO)")
    ap.add_argument("--wpm", type=int, default=13)
    ap.add_argument("--msg", default="PARIS PARIS PARIS")
    ap.add_argument("--csv", default=None,
                    help="raw per-packet CSV path (default foxhunt_<ts>.csv)")
    args = ap.parse_args()

    ids = [int(x) for x in args.stations.split(",")]
    csv_path = args.csv or f"foxhunt_{time.strftime('%Y%m%d_%H%M%S')}.csv"

    print("# identifying stations ...")
    idmap = identify()
    missing = [i for i in ids if i not in idmap]
    if missing:
        print(f"FATAL: stations not found on USB: {missing}\n"
              f"  found: {sorted(idmap)}", file=sys.stderr)
        return 1
    print("  " + ", ".join(f"{i}->{idmap[i].rsplit('/', 1)[-1]}" for i in ids))

    print("\n# configuring edge keymode + identical traffic ...")
    for sid in ids:
        configure(sid, idmap[sid], args.msg, args.wpm, args.pwr)

    rows: list = []
    try:
        for fox in ids:
            idmap = run_round(fox, ids, idmap, args.duration, rows)
    finally:
        # Belt-and-suspenders: make sure nothing is left transmitting.
        print("\n# restoring all stations to Hunter mode ...")
        idmap = identify()
        for sid in ids:
            if sid in idmap:
                try:
                    s = open_port(idmap[sid])
                    time.sleep(0.3)
                    r = cmd(s, "show", 1.0)
                    s.close()
                    mo = re.search(r"\bmode=(\w+)", r)
                    if mo and mo.group(1).lower() != "hunter":
                        idmap[sid] = set_mode_reboot(sid, idmap[sid], 0)
                except Exception as e:
                    print(f"  ! {sid}: {e}", file=sys.stderr)

    # write raw CSV
    with open(csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["ts_unix", "fox", "role", "observer", "seq", "hb",
                    "lvl", "rssi_dbm"])
        w.writerows(sorted(rows, key=lambda r: r[0]))
    print(f"\n# raw CSV: {csv_path}  ({len(rows)} rows)")

    summarize(rows, ids)
    return 0


if __name__ == "__main__":
    sys.exit(main())
