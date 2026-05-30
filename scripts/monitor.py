#!/usr/bin/env python3
"""Serial monitor for the Heltec V4 scanner.

Reads the device at 115200 baud and streams lines to stdout. Works without a
controlling TTY (unlike `pio device monitor`), so it can be backgrounded or
piped. Use Ctrl-C to exit; --duration N exits after N seconds.
"""
from __future__ import annotations

import argparse
import glob
import sys
import time

import serial  # provided by the PlatformIO venv


def find_port() -> str:
    p = find_port_quiet()
    if p is None:
        sys.stderr.write("error: no /dev/cu.usbmodem* device found\n")
        sys.exit(2)
    return p


def find_port_quiet():
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    return candidates[0] if candidates else None


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--port", default=None, help="serial device (default: auto)")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--duration", type=float, default=None,
                   help="exit after N seconds (default: run until Ctrl-C)")
    p.add_argument("--until", default=None,
                   help="exit after a line matching this substring is seen")
    args = p.parse_args()

    deadline = (time.time() + args.duration) if args.duration else None
    s = None
    current_port = None
    try:
        while True:
            if deadline is not None and time.time() >= deadline:
                break
            if s is None:
                # (Re)open the port. If the board is between USB enumerations
                # (e.g. just after RST), retry until it shows up.
                try:
                    current_port = args.port or find_port_quiet()
                    if current_port is None:
                        time.sleep(0.2)
                        continue
                    s = serial.Serial(current_port, args.baud, timeout=0.2)
                    sys.stderr.write(f"# monitor: {current_port} @ {args.baud}\n")
                    sys.stderr.flush()
                except (serial.SerialException, OSError):
                    s = None
                    time.sleep(0.2)
                    continue
            try:
                line = s.readline()
            except (serial.SerialException, OSError):
                # Device went away (USB re-enumeration). Drop the handle and retry.
                try: s.close()
                except Exception: pass
                s = None
                sys.stderr.write("# monitor: device disconnected, waiting...\n")
                sys.stderr.flush()
                continue
            if not line:
                continue
            text = line.decode("utf-8", errors="replace").rstrip("\r\n")
            print(text, flush=True)
            if args.until and args.until in text:
                break
    except KeyboardInterrupt:
        pass
    finally:
        if s is not None:
            try: s.close()
            except Exception: pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
