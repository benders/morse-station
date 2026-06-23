#!/usr/bin/env python
# Resilient serial capture that survives a reboot or USB unplug/replug: when the
# port vanishes it waits and reopens (the port may re-enumerate to a different
# usbmodem name). Every received line is timestamped and appended to the log
# file; connect/disconnect transitions are marked.
#
# Two uses:
#   - Cardputer brownout test (battery power-cycles).
#   - Type-time panic capture (TODO.md C2 / instructor-UI plan Risk B): run this,
#     then type heavily on the keyboard; if the board reboots, the ESP32
#     backtrace printed to USB CDC just before reset lands in the log instead of
#     dying with the re-enumerating port.
import glob, time, datetime, sys

LOG = sys.argv[1] if len(sys.argv) > 1 else '/tmp/station73_serial.log'

def now():
    return datetime.datetime.now().strftime('%H:%M:%S.%f')[:-3]

def find_port():
    ports = sorted(glob.glob('/dev/cu.usbmodem*'))
    return ports[0] if ports else None

def log(f, msg):
    f.write(f'[{now()}] {msg}\n'); f.flush()

import serial  # pyserial from the pio venv

with open(LOG, 'a') as f:
    log(f, '=== capture started ===')
    buf = b''
    while True:
        port = find_port()
        if not port:
            time.sleep(0.5); continue
        try:
            s = serial.Serial(port, 115200, timeout=0.3)
        except Exception as e:
            time.sleep(0.5); continue
        log(f, f'=== connected {port} ===')
        try:
            while True:
                data = s.read(4096)
                if data:
                    buf += data
                    while b'\n' in buf:
                        line, buf = buf.split(b'\n', 1)
                        text = line.decode('utf-8', 'replace').rstrip('\r')
                        if text:
                            log(f, text)
        except Exception:
            log(f, '=== disconnected (port closed) ===')
            try: s.close()
            except Exception: pass
            buf = b''
            time.sleep(0.5)
