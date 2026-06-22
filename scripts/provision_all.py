#!/usr/bin/env python3
"""Provision EVERY connected station at once over the live runtime console.

Unlike provision.py (single board, boot setup REPL, reset required), this drives
the firmware's non-blocking runtime serial console (serial_console_process in
main.cpp) on every /dev/cu.usbmodem* in parallel-ish sequence. No reset: the
station keeps running its current mode while the commands are applied. Both
consoles share handle_setup_command(), so call/wpm/farns/mute/msgmode all write
through to NVS and survive a power cycle.

Examples:
    scripts/provision_all.sh --wpm 5 --farns 13 --call KC8HOB --mute on --msgmode text
    scripts/provision_all.sh --show          # just read back every board

nRF52840 boards (Wio/RAK) use native USB CDC and echo a command behind; the
final `show` is the source of truth, so we always issue it last and print it.
"""
from __future__ import annotations

import argparse
import glob
import sys
import time

import serial  # pyserial (PlatformIO venv) or tools/blevenv


def find_ports() -> list[str]:
    return sorted(glob.glob("/dev/cu.usbmodem*"))


def drain(s: serial.Serial, quiet: float) -> str:
    """Read until the port goes `quiet` seconds without new bytes."""
    end = time.time() + quiet
    out = b""
    while time.time() < end:
        chunk = s.read(256)
        if chunk:
            out += chunk
            end = time.time() + quiet
    return out.decode("utf-8", errors="replace")


def build_cmds(args: argparse.Namespace) -> list[str]:
    cmds: list[str] = []
    # Order matters: set overall wpm before farns (Farnsworth char speed must be
    # >= overall), and apply call/msg/id before the read-back.
    if args.wpm is not None:
        cmds.append(f"wpm {args.wpm}")
    if args.farns is not None:
        cmds.append(f"farns {args.farns}")
    if args.call is not None:
        cmds.append(f"call {args.call}")
    if args.msg is not None:
        cmds.append(f"msg {args.msg}")
    if args.id is not None:
        cmds.append(f"id {args.id}")
    if args.mute is not None:
        cmds.append(f"mute {args.mute}")
    if args.msgmode is not None:
        cmds.append(f"msgmode {args.msgmode}")
    cmds.append("show")          # always read back, last
    return cmds


def provision(port: str, cmds: list[str], quiet: float) -> bool:
    print(f"\n===== {port} =====")
    try:
        s = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:                       # noqa: BLE001
        print(f"  ! open failed: {e}")
        return False
    try:
        time.sleep(0.4)
        drain(s, quiet)                          # flush any pending output
        for cmd in cmds:
            s.write((cmd + "\n").encode())
            s.flush()
            for ln in drain(s, quiet).splitlines():
                ln = ln.strip()
                if ln:
                    print(f"  [{cmd}] {ln}")
        return True
    finally:
        try:
            s.close()
        except Exception:
            pass


def main() -> int:
    p = argparse.ArgumentParser(description="Provision all connected stations over the live console.")
    p.add_argument("--call", default=None, help="operator callsign")
    p.add_argument("--msg", default=None, help="fox message text")
    p.add_argument("--id", type=int, default=None, help="station id (1..254)")
    p.add_argument("--wpm", type=int, default=None, help="overall keying speed, 5..40")
    p.add_argument("--farns", type=int, default=None, help="Farnsworth char speed, >= --wpm")
    p.add_argument("--mute", choices=["on", "off"], default=None, help="sidetone mute")
    p.add_argument("--msgmode", choices=["keyed", "text"], default=None,
                   help="fox canned-message delivery mode")
    p.add_argument("--show", action="store_true", help="read back values only")
    p.add_argument("--quiet", type=float, default=1.2,
                   help="per-command idle-drain seconds (raise for slow nRF52 CDC)")
    args = p.parse_args()

    have_setting = any(v is not None for v in
                       (args.call, args.msg, args.id, args.wpm, args.farns,
                        args.mute, args.msgmode))
    if not have_setting and not args.show:
        p.error("nothing to do: pass --call/--msg/--id/--wpm/--farns/--mute/--msgmode or --show")

    ports = find_ports()
    if not ports:
        sys.stderr.write("error: no /dev/cu.usbmodem* device found\n")
        return 2

    cmds = build_cmds(args)
    print(f"# provisioning {len(ports)} station(s): {', '.join(cmds[:-1]) or '(show only)'}")
    ok = all(provision(port, cmds, args.quiet) for port in ports)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
