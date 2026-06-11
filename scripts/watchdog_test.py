#!/usr/bin/env python
# Hardware watchdog validation. For one running station over USB:
#   1. clear the bootlog ring,
#   2. issue `stall` (spins loop() with no WDT feed -> should wedge then reboot),
#   3. confirm the device goes silent (wedged) and the USB CDC port re-enumerates,
#   4. reconnect and read `bootlog` to confirm the latest boot reason is the
#      watchdog (TASK_WDT on ESP32, WATCHDOG on nRF52).
# Exit 0 = PASS (watchdog reboot observed), 1 = FAIL.
#
# Usage: watchdog_test.py <port> [--timeout-budget 25]
import sys, time, os, glob, serial

port = sys.argv[1]
WDT_CAUSES = ("TASK_WDT", "WATCHDOG")   # ESP32 / nRF52 labels from reset_reason_label()


def drain(s, secs, echo_prefix=None):
    """Read for `secs`, return accumulated text; tolerate a port drop (reboot)."""
    buf, deadline = "", time.time() + secs
    while time.time() < deadline:
        try:
            chunk = s.read(512).decode("utf-8", "replace")
        except (serial.SerialException, OSError):
            buf += "\n<<port dropped (device rebooting)>>\n"
            break
        if chunk:
            buf += chunk
            if echo_prefix:
                for ln in chunk.splitlines():
                    if ln.strip():
                        print(f"{echo_prefix}{ln.rstrip()}")
            deadline = time.time() + 0.6
    return buf


def open_retry(path, tries=30, gap=0.5):
    """Wait for the CDC port to re-appear after a reboot, then open it."""
    for _ in range(tries):
        if os.path.exists(path):
            try:
                return serial.Serial(path, 115200, timeout=0.1)
            except (serial.SerialException, OSError):
                pass
        time.sleep(gap)
    return None


print(f"## watchdog test on {port}")

# --- phase 0: clear the bootlog so we read a fresh post-reboot reason ---
s = serial.Serial(port, 115200, timeout=0.1)
s.reset_input_buffer()
s.write(b"bootlog clear\r\n"); s.flush()
drain(s, 1.5)

# --- phase 1: stall, watch it wedge and reboot ---
# NB: a busy-spinning loop() starves the USB-CDC task, so the host may see the
# port drop a few seconds BEFORE the 8 s watchdog actually fires. We therefore
# ignore the drop and just wait a fixed budget (> watchdog timeout) before
# reconnecting — racing the reconnect against the drop reads bootlog too early.
print(">> sending `stall` (expect ~8s wedge then a watchdog reboot)")
t_stall = time.time()
s.write(b"stall\r\n"); s.flush()
out = drain(s, 4, echo_prefix="   ")   # just long enough to catch the ack line
try:
    s.close()
except Exception:
    pass

if "# stalling" not in out:
    print("FAIL: device never acknowledged `stall` (old firmware? no watchdog cmd)")
    sys.exit(1)
if "watchdog did NOT fire" in out:
    print("FAIL: stall ran to completion — watchdog never fired")
    sys.exit(1)

# --- phase 2: wait past the watchdog timeout, then poll bootlog until the node
# has finished rebooting (boot can take ~10s+ on the Cardputer: splash + on-device
# keyboard window + 5s menu idle) and answers the console again. ---
WAIT_S = 12
print(f">> waiting {WAIT_S}s for the watchdog to fire and the node to reboot ...")
time.sleep(WAIT_S)

log = ""
hit = False
for attempt in range(12):          # ~ up to 36s of post-reboot boot slack
    s = open_retry(port)
    if s is None:
        print(f"FAIL: port {port} did not re-appear after reboot")
        sys.exit(1)
    s.reset_input_buffer()
    s.write(b"bootlog\r\n"); s.flush()
    log = drain(s, 3.0)
    s.close()
    if "reason=" in log or any(c in log for c in WDT_CAUSES):
        break                      # device is up and answered the console
    time.sleep(2.0)                # still booting (menu/splash) — retry

for ln in log.splitlines():
    if ln.strip():
        print(f"   {ln.rstrip()}")
print("   --- end bootlog ---")

elapsed = time.time() - t_stall
hit = any(c in log for c in WDT_CAUSES)
print(f">> elapsed {elapsed:.1f}s; bootlog watchdog cause present: {hit}")
if hit:
    print("PASS: watchdog rebooted the wedged node")
    sys.exit(0)
print("FAIL: bootlog does not show a watchdog reset cause")
sys.exit(1)
