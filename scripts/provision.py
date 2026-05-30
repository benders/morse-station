#!/usr/bin/env python3
"""Provision a Heltec V4 unit's NVS settings over serial.

Drives the firmware's boot setup console (see run_setup_console in main.cpp) to
store the operator callsign, fox message, and/or station id. The firmware only
opens the setup window for ~2 s right after boot, so this script resets the
board (best-effort DTR/RTS pulse), waits for the prompt, sends 's' to enter the
console, then issues the commands.

Examples:
    scripts/provision.sh --call N0CALL --msg "DE N0CALL FOX BY THE OAK" --id 5
    scripts/provision.sh --show          # just read back current values

If the auto-reset doesn't catch the window, the script will ask you to tap RST.
"""
from __future__ import annotations

import argparse
import glob
import sys
import time

import serial  # provided by the PlatformIO venv

PROMPT_HINT = "for setup console"
REPL_PROMPT = "setup>"


def find_port() -> str:
    candidates = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not candidates:
        sys.stderr.write("error: no /dev/cu.usbmodem* device found\n")
        sys.exit(2)
    return candidates[0]


def pulse_reset(s: serial.Serial) -> None:
    """Best-effort board reset. On boards where EN is wired to DTR this restarts
    the sketch; on native-USB-CDC boards it's a no-op and we fall back to RST."""
    try:
        s.setDTR(False); s.setRTS(True); time.sleep(0.1)
        s.setRTS(False); time.sleep(0.1)
        s.setDTR(True)
    except Exception:
        pass


def read_until(s: serial.Serial, needle: str, timeout: float, echo: bool = True) -> bool:
    """Read lines until one contains needle or timeout elapses. Echoes device
    output to stdout. Returns True if needle was seen."""
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = s.read(256)
        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            text = line.decode("utf-8", errors="replace").rstrip("\r")
            if echo and text:
                print(f"  {text}")
            if needle in text:
                return True
        # needle may arrive on a line without a trailing newline (the prompt)
        tail = buf.decode("utf-8", errors="replace")
        if needle in tail:
            if echo and tail.strip():
                print(f"  {tail.strip()}")
            return True
    return False


def send_cmd(s: serial.Serial, cmd: str) -> None:
    print(f"> {cmd}")
    s.write((cmd + "\n").encode())
    s.flush()
    read_until(s, REPL_PROMPT, timeout=3.0)


def main() -> int:
    p = argparse.ArgumentParser(description="Provision Heltec V4 NVS over serial.")
    p.add_argument("--port", default=None, help="serial device (default: auto)")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--call", default=None, help="operator callsign")
    p.add_argument("--msg", default=None, help="fox message text")
    p.add_argument("--id", type=int, default=None, help="station id (1..254)")
    p.add_argument("--show", action="store_true", help="read back values and exit")
    p.add_argument("--reset-timeout", type=float, default=12.0,
                   help="seconds to wait for the boot setup prompt")
    args = p.parse_args()

    if not (args.call or args.msg or args.id is not None or args.show):
        p.error("nothing to do: pass --call/--msg/--id or --show")

    port = args.port or find_port()
    s = serial.Serial(port, args.baud, timeout=0.2)
    print(f"# provision: {port} @ {args.baud}")

    try:
        pulse_reset(s)
        print("# waiting for boot setup prompt (tap RST if nothing happens)...")
        if not read_until(s, PROMPT_HINT, timeout=args.reset_timeout):
            sys.stderr.write("error: never saw the setup prompt; tap RST and retry\n")
            return 1

        # Enter the console.
        s.write(b"s\n"); s.flush()
        if not read_until(s, REPL_PROMPT, timeout=3.0):
            sys.stderr.write("error: console did not open after 's'\n")
            return 1

        if args.call is not None:
            send_cmd(s, f"call {args.call}")
        if args.msg is not None:
            send_cmd(s, f"msg {args.msg}")
        if args.id is not None:
            send_cmd(s, f"id {args.id}")

        send_cmd(s, "show")
        send_cmd(s, "done")
        print("# done")
        return 0
    finally:
        try: s.close()
        except Exception: pass


if __name__ == "__main__":
    sys.exit(main())
