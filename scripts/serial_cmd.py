#!/usr/bin/env python3
"""Minimal serial-console driver for unattended power-management tests.

Usage:
  serial_cmd.py PORT [--settle SECS] [--read SECS] CMD ["CMD2" ...]

Opens PORT at 115200, waits --settle seconds for any boot/menu to finish (the
runtime console — serial_console_process — only accepts commands once a mode is
running), then for each CMD writes it + newline and prints whatever comes back
within a short window. With --read it just streams the port for SECS and exits
(used to capture a hunter's decoded Morse, which it prints to Serial live).
"""
import sys, time, serial

def main():
    a = sys.argv[1:]
    port = a.pop(0)
    settle = 12.0
    read_secs = None
    dtr = False
    cmds = []
    while a:
        t = a.pop(0)
        if t == "--settle":   settle = float(a.pop(0))
        elif t == "--read":   read_secs = float(a.pop(0))
        elif t == "--dtr":    dtr = True   # nRF52 TinyUSB CDC only flushes TX with DTR asserted
        else:                 cmds.append(t)

    # ESP32 native-USB / CP2102 boards reset on open regardless; the nRF52 (Wio/
    # RAK) does not reset on DTR, but its USB-CDC needs DTR asserted to send.
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = dtr
    s.rts = False
    s.open()

    t0 = time.time()
    while time.time() - t0 < settle:
        chunk = s.read(4096)
        if chunk:
            sys.stdout.write(chunk.decode("utf-8", "replace"))
            sys.stdout.flush()

    for c in cmds:
        print(f"\n>>> {c}")
        s.write((c + "\n").encode())
        s.flush()
        t0 = time.time()
        while time.time() - t0 < 2.5:
            chunk = s.read(4096)
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()

    if read_secs is not None:
        print(f"\n--- streaming {port} for {read_secs}s ---")
        t0 = time.time()
        while time.time() - t0 < read_secs:
            chunk = s.read(4096)
            if chunk:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
    s.close()

if __name__ == "__main__":
    main()
