#!/usr/bin/env python3
"""Text-mode sync/DF beacon bench test (field note §8, plan-text-sync-beacon S4).

Validates the hunter-side `decode_sync` slave landed in S4 (commit aa70561):

  1. DF/RSSI continuity (problem #2): the fox emits a MAGIC_SYNC beacon every
     BEACON_MS (200 ms) across the WHOLE loop — render AND the inter-message
     pause — so each hunter sees an `RX S` line at <=250 ms cadence the entire
     cycle, never blinking out (unlike the bursty ~150 ms TextMsg traffic).
  2. Sync slaving (problem #1): each hunter slaves its local morse::Player to the
     beacon's pos_ms. `drift` (local pos - fox pos) stays within ~one dit; a hard
     `seek=1` fires only when it briefly exceeds RESYNC_SLACK_MS. Two hunters that
     both track the same fox pos are, by construction, rendering in unison.
  3. Mid-join: a hunter rebooted mid-clue gets a beacon for a seq it has no text
     for, defers, and on the next TextMsg re-send seeks straight to the live
     position — observable as `RX T mid-join seq=N seek=P` with P>0.

NOT covered here (require a human at the bench): audible unison by ear, and the
physical antenna-pull free-run/recover. Those are reported as MANUAL.

This is a pure-capture test: it assumes the boards are ALREADY flashed with the
S4 build (the caller flashes them). It only sends console config + reboot.

Topology (from scripts/devices.sh --usb at time of writing; re-check if moved):
  - Fox      = stn42  /dev/cu.usbmodem201301  Heltec V4  (msgmode text)
  - Hunter A = stn43  /dev/cu.usbmodem22301   Heltec V4
  - Hunter B = stn26  /dev/cu.usbmodem201401  RAK4631 (nRF52: needs DTR to TX)
"""
import sys, time, threading, re, collections
import serial

WPM  = 18
CLUE = "CQ DE FOX 42 NEAR OLD MILL"
REPEAT_HINT = 12   # fox REPEAT_PAUSE (s) — used only to pace the test waits

PORTS = {
    "fox":     dict(port="/dev/cu.usbmodem201301", dtr=False, sid=42),
    # hunterA is a Heltec V3 (CP2102): it resets on port-open (clean lock/radio
    # state every run) AND its USB-UART bridge survives an MCU reboot, so the
    # mid-join reboot test doesn't drop the port (a V4's native USB CDC does).
    "hunterA": dict(port="/dev/cu.usbserial-4",    dtr=False, sid=38),
    "hunterB": dict(port="/dev/cu.usbmodem201401", dtr=True,  sid=26),
}

# Other foxes on the bench must be silenced or the hunters lock onto the wrong one
# (fox_lock gates decode_sync to a single station). stn73 (Cardputer) is also in
# Fox mode and beaconing once flashed — `stop` halts its TX for the test.
SILENCE = {
    "fox73": dict(port="/dev/cu.usbmodem201201", dtr=False, sid=73),
}

RESULTS = []
def record(step, ok, detail="", skip=False):
    tag = "MANUAL" if skip else ("PASS" if ok else "FAIL")
    RESULTS.append((step, ok, detail, skip))
    print(f"[{tag}] {step}" + (f" — {detail}" if detail else ""), flush=True)


class Node:
    def __init__(self, name, cfg):
        self.name = name
        self.sid = cfg["sid"]
        self.lines = []          # (host_ts, text)
        self._lock = threading.Lock()
        self._run = True
        s = serial.Serial()
        s.port = cfg["port"]; s.baudrate = 115200; s.timeout = 0.1
        s.dtr = cfg["dtr"]; s.rts = False
        s.open()
        self.ser = s
        self.t = threading.Thread(target=self._reader, daemon=True)
        self.t.start()

    def _reader(self):
        buf = b""
        while self._run:
            try:
                buf += self.ser.read(256)
            except Exception:
                break
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                txt = ln.decode("utf-8", "replace").strip("\r\n ")
                if txt:
                    with self._lock:
                        self.lines.append((time.time(), txt))

    def send(self, cmd):
        self.ser.write((cmd + "\n").encode()); self.ser.flush()
        time.sleep(0.3)

    def snapshot(self):
        with self._lock:
            return list(self.lines)

    def reset_buf(self):
        with self._lock:
            self.lines.clear()

    def close(self):
        self._run = False
        time.sleep(0.2)
        try: self.ser.close()
        except Exception: pass


def main():
    print("# opening ports (ESP32 native USB resets on open; settling)...", flush=True)
    nodes = {n: Node(n, c) for n, c in PORTS.items()}
    silence = {}
    for n, c in SILENCE.items():
        try:
            silence[n] = Node(n, c)
        except Exception as e:
            print(f"# warn: could not open {n} to silence it: {e}", flush=True)
    time.sleep(12)   # boot + mode-start before the console accepts commands

    fox, ha, hb = nodes["fox"], nodes["hunterA"], nodes["hunterB"]

    # Halt every other fox so the hunters lock onto our test fox only.
    for s in silence.values():
        s.send("stop")

    # --- configure ---------------------------------------------------------
    # Main phase runs at WPM (18). The mid-join phase needs a render LONGER than a
    # hunter's ~9s boot, so it switches to MJ_WPM (slow). Both restored at the end.
    print("# configuring fox (text mode) + hunters (debug)...", flush=True)
    fox.send(f"wpm {WPM}")
    fox.send(f"msg {CLUE}")
    fox.send("msgmode text")
    fox.send("debug on")
    fox.send("start")
    for h in (ha, hb):
        h.send("debug on")

    # Flush a full cycle first: a boot-time render at the OLD (NVS) wpm can bleed
    # into the capture and skew cadence/drift. Wait one wpm-18 cycle, then clear
    # every buffer so the capture window holds only steady-state wpm-18 traffic.
    print("# flushing one cycle, then aligning capture...", flush=True)
    time.sleep(25)
    for n in nodes.values():
        n.reset_buf()

    # --- capture a few full cycles (>=2 renders for both hunters) -----------
    CAP = 45
    print(f"# capturing {CAP}s (>=2 fox cycles)...", flush=True)
    t_cap0 = time.time()
    time.sleep(CAP)

    # --- mid-join: reboot hunterA so its boot completes DURING a render ----
    # A hunter only mid-joins if its FIRST contact with a clue is a beacon (no
    # text yet). That needs boot (~9s) to finish mid-render, so slow the fox right
    # down (MJ_WPM => ~20s render). `debug` does NOT persist across reboot, so we
    # spam `debug on\n` into hunterA's input during boot — those bytes buffer and
    # are consumed in the first console iterations, turning debug back on BEFORE
    # the mid-join seek fires (beacon sets want_text_seq, the next TextMsg resend
    # <=2s later triggers the seek), so the `RX T mid-join` line is observed.
    MJ_WPM = 6
    print(f"# mid-join: slowing fox (wpm {MJ_WPM}) for a long render...", flush=True)
    fox.send(f"wpm {MJ_WPM}")
    time.sleep(2*REPEAT_HINT)   # let the new wpm take at the next cycle top

    print("# waiting for a fox cycle top (TX T) to time the reboot...", flush=True)
    mark = time.time()
    def saw_txt_after(ts):
        return any(t.startswith("TX T ") and tt >= ts for tt, t in fox.snapshot())
    deadline = time.time() + 60
    while time.time() < deadline and not saw_txt_after(mark):
        time.sleep(0.2)
    print("# cycle top seen — rebooting hunterA + holding debug on...", flush=True)
    # NB: do NOT reset_buf here — the mid-join analysis filters by ts>=mj0, and
    # wiping the buffer would erase hunterA's main-capture beacons before analysis.
    ha.send("reboot")
    mj0 = time.time()
    # spam debug-on through the boot so it's live before the seek
    for _ in range(120):                 # ~12s of 100ms ticks covers the ~9s boot
        try:
            ha.ser.write(b"debug on\n"); ha.ser.flush()
        except Exception:
            pass
        time.sleep(0.1)
    time.sleep(20)   # remainder of the long render: beacon + resend -> seek

    # restore defaults so the bench is left at the documented baseline
    fox.send(f"wpm {WPM}")
    for s in silence.values():
        s.send("start")     # un-halt the other fox(es)
        s.close()

    for n in nodes.values():
        n.close()

    # ===================== analysis =======================================
    print("\n===== analysis =====", flush=True)

    # 0. fox actually in text mode + beaconing
    fox_lines = [t for _, t in fox.snapshot()]
    txs = [t for t in fox_lines if t.startswith("TX S ")]
    record("0. fox emits TX S sync beacons", len(txs) > 10,
           f"{len(txs)} TX S lines")

    def rx_s(node, t_lo, t_hi):
        return [(ts, t) for ts, t in node.snapshot()
                if t.startswith("RX S ") and t_lo <= ts <= t_hi]

    cap_lo, cap_hi = t_cap0 + 2, t_cap0 + CAP
    for h in (ha, hb):
        beacons = rx_s(h, cap_lo, cap_hi)
        n = len(beacons)
        record(f"1a. {h.name} receives RX S beacons", n > 20,
               f"{n} beacons in {CAP}s")
        if n < 2:
            continue
        # cadence: inter-arrival gaps
        gaps = [ (beacons[i][0]-beacons[i-1][0])*1000 for i in range(1, n) ]
        gaps.sort()
        med = gaps[len(gaps)//2]
        p90 = gaps[int(len(gaps)*0.9)]
        mx  = gaps[-1]
        # <=250ms required; allow occasional double-gap from a dropped beacon
        ok = med <= 260 and p90 <= 450
        record(f"1b. {h.name} beacon cadence (DF/RSSI continuity)", ok,
               f"median={med:.0f}ms p90={p90:.0f}ms max={mx:.0f}ms")
        # presence across BOTH render (pos!=65535) and pause (pos==65535)
        idle = sum(1 for _, t in beacons if "pos=65535" in t)
        rend = n - idle
        record(f"1c. {h.name} beacons span render AND pause", idle > 0 and rend > 0,
               f"render={rend} idle/pause={idle}")
        # drift bounded + seeks rare
        drifts = [int(m.group(1)) for _, t in beacons
                  if (m := re.search(r"drift=(-?\d+)", t))]
        seeks = sum(1 for _, t in beacons if "seek=1" in t)
        if drifts:
            amax = max(abs(d) for d in drifts)
            # most beacons should be within ~2 dits of the fox; seeks the minority
            within = sum(1 for d in drifts if abs(d) <= 2*int(1200/WPM))
            frac = within/len(drifts)
            record(f"1d. {h.name} render stays slaved to fox", frac >= 0.8,
                   f"|drift|max={amax}ms within2dit={frac*100:.0f}% seeks={seeks}")

    # 2. both hunters tracking the SAME fox seq concurrently (=> unison basis)
    def seqs(node, t_lo, t_hi):
        out = set()
        for ts, t in rx_s(node, t_lo, t_hi):
            m = re.search(r"RX S \d+ seq=(\d+)", t)
            if m and "pos=65535" not in t:
                out.add(int(m.group(1)))
        return out
    sa, sb = seqs(ha, cap_lo, cap_hi), seqs(hb, cap_lo, cap_hi)
    common = sa & sb
    record("2. both hunters render the same clue seqs (unison basis)",
           len(common) >= 1, f"A={sorted(sa)} B={sorted(sb)} common={sorted(common)}")

    # 3. mid-join: hunterA, after reboot, seeks into a live render
    mj = [t for ts, t in ha.snapshot()
          if ts >= mj0 and "mid-join" in t]
    seekpos = []
    for t in mj:
        m = re.search(r"seek=(\d+)", t)
        if m: seekpos.append(int(m.group(1)))
    record("3. hunterA mid-join seeks to live pos after reboot",
           any(p > 0 for p in seekpos),
           f"mid-join lines={mj}")

    # manual-only checks
    record("4. audible unison by ear (2 hunters beep together)", True, "", skip=True)
    record("5. antenna-pull free-run then snap back on next beacon", True, "", skip=True)

    # ===================== summary ========================================
    print("\n===== summary =====", flush=True)
    npass = sum(1 for _, ok, _, sk in RESULTS if ok and not sk)
    nfail = sum(1 for _, ok, _, sk in RESULTS if not ok and not sk)
    nman  = sum(1 for *_, sk in RESULTS if sk)
    for step, ok, detail, sk in RESULTS:
        tag = "MANUAL" if sk else ("PASS" if ok else "FAIL")
        print(f"  [{tag}] {step}", flush=True)
    print(f"\n{npass} passed, {nfail} failed, {nman} manual", flush=True)
    return 1 if nfail else 0


if __name__ == "__main__":
    sys.exit(main())
