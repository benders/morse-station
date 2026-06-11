#!/usr/bin/env python
# Instructor remote-control end-to-end test.
# Concurrently watches fox + instructor + hunters while the instructor relays a
# `msg` change to the fox over GFSK. Prints tagged, timestamped lines so we can
# see: instructor ACK, fox RX-C (command received), and hunter CH (decoded text
# of the NEW message keyed by the fox).
import sys, time, threading, serial, datetime

PORTS = {
    "FOX42":   "/dev/cu.usbmodem20301",
    "INSTR73": "/dev/cu.usbmodem21101",
    "HUNT43":  "/dev/cu.usbmodem21301",
    "HUNT115": "/dev/cu.usbmodem22301",
}
NEW_MSG = "ACK OK 73"
RUN_S   = 35.0

def ts():
    return datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]

stop = threading.Event()
locks = threading.Lock()
handles = {}

def reader(tag, s, deadline):
    buf = ""
    while not stop.is_set() and time.time() < deadline:
        try:
            chunk = s.read(256).decode("utf-8", "replace")
        except Exception:
            break
        if not chunk:
            continue
        buf += chunk
        while "\n" in buf:
            line, buf = buf.split("\n", 1)
            line = line.rstrip("\r")
            if line.strip() == "":
                continue
            with locks:
                print(f"{ts()} [{tag:7}] {line}", flush=True)

def send(s, cmd):
    s.write((cmd + "\r\n").encode())
    s.flush()

# Open all ports
for tag, port in PORTS.items():
    handles[tag] = serial.Serial(port, 115200, timeout=0.15)
    handles[tag].reset_input_buffer()

deadline = time.time() + RUN_S
threads = []
for tag, s in handles.items():
    t = threading.Thread(target=reader, args=(tag, s, deadline), daemon=True)
    t.start()
    threads.append(t)

# Enable debug on fox + hunters so we see RX-C and decoded CH lines.
for tag in ("FOX42", "HUNT43", "HUNT115"):
    send(handles[tag], "debug on")
time.sleep(1.0)

# Baseline: confirm fox current message, then relay the change.
print(f"{ts()} [HOST   ] --- relaying 'msg {NEW_MSG}' to fox 42 via instructor 73 ---", flush=True)
send(handles["INSTR73"], f"relay 42 msg {NEW_MSG}")

# Let the burst/ack happen and the fox key the new message a few cycles.
for t in threads:
    t.join()

print(f"{ts()} [HOST   ] --- run complete ---", flush=True)
for s in handles.values():
    s.close()
