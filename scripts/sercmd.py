#!/usr/bin/env python
# Minimal runtime serial console helper: send one or more console commands to a
# station over USB and print everything it streams back.
# Native-USB boards (usbmodem*) answer without a reset; the Heltec V3
# (usbserial*/CP2102) is rebooted by the open and read once it boots — see
# station_serial.open_console.
# Usage: sercmd.py <port> "<cmd>" [more cmds...] [--wait SECONDS]
import sys, time
from station_serial import open_console

args = sys.argv[1:]
wait = 2.5
if "--wait" in args:
    i = args.index("--wait")
    wait = float(args[i + 1])
    del args[i:i + 2]

port, cmds = args[0], args[1:]
s = open_console(port)
try:
    for c in cmds:
        s.write((c + "\r\n").encode())
        s.flush()
        time.sleep(0.2)
    deadline = time.time() + wait
    buf = ""
    while time.time() < deadline:
        chunk = s.read(512).decode("utf-8", "replace")
        if chunk:
            buf += chunk
            deadline = time.time() + 0.6  # extend while data flows
    sys.stdout.write(buf)
finally:
    s.close()
