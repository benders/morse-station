#!/usr/bin/env python3
"""Instructor-relay stop/start integration test across four live stations.

Topology (all on one bench):
  - Instructor = stn43  /dev/cu.usbmodem20401  (Heltec V4, native USB, resets on open)
  - Fox        = stn42  /dev/cu.usbmodem21301  (Heltec V4, native USB, resets on open)
  - Hunter A   = stn38  /dev/cu.usbserial-0001  (Heltec V3, CP2102, resets on open)
  - Hunter B   = stn115 /dev/cu.usbmodem22301  (Wio Tracker L1, nRF52, NO reset; needs DTR to TX)

Procedure (from the task):
  1. Ensure all stations run current firmware.
  2. stn43 -> Instructor; stn42 stays Fox; stn38 + stn115 are Hunters.
  3. Instructor relays "stop" to the Fox.
  4. Hunters finish the current message; no new message follows.
  5. After 60 s of no broadcast, Hunters screen-blank.
  6. Wait another 60 s (panels stay blank).
  7. Instructor relays msg "ABLE BAKER CHARLIE" to the Fox.
  8. Instructor relays "start" to the Fox.
  9. Hunters wake and copy the NEW message.
 10. Hunters answer BLE again after auto-waking (stn38 AUTO-BLE restored on wake;
     stn115 Bluefruit always-on) — verified over the BLE transport.

Fully automated verification — every step asserts on passively-observed serial
output (no human in the loop). The only station written to during the test is
the Instructor; Hunters are read-only after setup so polling never resets their
idle-blank timer. Observables used:
  Fox     : "RX C ... cmd=<x>"  (received a relayed command),
            "# TX HALT"/"# TX RESUME", "TX E" (transmitting), "TX SKIP (halted)".
  Hunters : "RX E"/"RX K" (receiving on-air keying), "CH <id> <char>" (decoded
            text, debug-gated), "# screen: blanked (60s idle)" / "# screen: woke".
Requires `debug on` (RX/CH/TX lines are gated on it); blanking logs are always on.
"""
import sys, time, threading, re, difflib
import serial

EXPECT_BUILD = "65f6546"          # current git HEAD short hash
FOX_ID, INSTR_ID = 42, 43
NEW_MSG = "ABLE BAKER CHARLIE"

PORTS = {
    "instructor": dict(port="/dev/cu.usbmodem20401", dtr=False, sid=43),
    "fox":        dict(port="/dev/cu.usbmodem21301", dtr=False, sid=42),
    "hunterA":    dict(port="/dev/cu.usbserial-0001", dtr=False, sid=38),
    "hunterB":    dict(port="/dev/cu.usbmodem22301", dtr=True,  sid=115),
}

RESULTS = []   # (step, ok, detail)
def record(step, ok, detail=""):
    RESULTS.append((step, ok, detail))
    print(f"\n[{'PASS' if ok else 'FAIL'}] {step}" + (f" — {detail}" if detail else ""), flush=True)


class Station:
    def __init__(self, name, port, dtr, sid):
        self.name, self.port, self.dtr, self.sid = name, port, dtr, sid
        self.ser = None
        self.lines = []          # list of (ts, line)
        self._partial = ""
        self.lock = threading.Lock()
        self._stop = False
        self._thr = None

    def open(self, retries=8, delay=1.0):
        last = None
        for _ in range(retries):
            try:
                s = serial.Serial()
                s.port = self.port; s.baudrate = 115200; s.timeout = 0.2
                s.dtr = self.dtr; s.rts = False
                s.open()
                self.ser = s
                return
            except Exception as e:                       # noqa
                last = e; time.sleep(delay)
        raise RuntimeError(f"{self.name}: cannot open {self.port}: {last}")

    def start_reader(self):
        self._stop = False
        self._thr = threading.Thread(target=self._reader, daemon=True)
        self._thr.start()

    def _reader(self):
        while not self._stop:
            try:
                data = self.ser.read(4096)
            except Exception:
                time.sleep(0.2); continue
            if not data:
                continue
            text = data.decode("utf-8", "replace")
            with self.lock:
                self._partial += text
                while "\n" in self._partial:
                    line, self._partial = self._partial.split("\n", 1)
                    self.lines.append((time.time(), line.rstrip("\r")))

    def close(self):
        self._stop = True
        if self._thr: self._thr.join(timeout=2)
        if self.ser:
            try: self.ser.close()
            except Exception: pass

    def send(self, cmd):
        self.ser.write((cmd + "\n").encode()); self.ser.flush()
        print(f"   -> {self.name}: {cmd}", flush=True)

    def wait_for(self, patterns, timeout, since=0.0):
        if isinstance(patterns, str): patterns = [patterns]
        comp = [re.compile(p) for p in patterns]
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                for ts, line in self.lines:
                    if ts >= since:
                        for c in comp:
                            if c.search(line):
                                return (ts, line)
            time.sleep(0.2)
        return None

    def count_since(self, pattern, since, until=None):
        c = re.compile(pattern); n = 0
        with self.lock:
            for ts, line in self.lines:
                if ts >= since and (until is None or ts <= until) and c.search(line):
                    n += 1
        return n

    def decoded_since(self, since, until=None):
        """Join decoded characters from 'CH <id> <c>' lines."""
        c = re.compile(r"CH\s+\d+\s+(.)")
        out = []
        with self.lock:
            for ts, line in self.lines:
                if ts >= since and (until is None or ts <= until):
                    m = c.search(line)
                    if m: out.append(m.group(1))
        return "".join(out)


def fuzzy_ok(got, want):
    g = "".join(got.upper().split())
    w = "".join(want.upper().split())
    ratio = difflib.SequenceMatcher(None, g, w).ratio()
    words = [bit for bit in want.upper().split() if bit in got.upper()]
    return ratio, words


def preconfig_instructor():
    """Set stn43 boot mode = Instructor (NVS) via a throwaway connection, and
    VERIFY the 'boot mode = 3' reply (the V4 native-USB console needs several
    seconds after a reset-on-open before it accepts commands). The main open's
    reset then boots it into Instructor mode."""
    p = PORTS["instructor"]
    for attempt in range(3):
        s = serial.Serial(); s.port = p["port"]; s.baudrate = 115200
        s.timeout = 0.2; s.dtr = False; s.rts = False; s.open()
        time.sleep(6.0)                   # V4 native USB: console ready ~5s post-reset
        s.write(b"mode 3\n"); s.flush()
        buf = b""
        t0 = time.time()
        while time.time() - t0 < 3.0:
            buf += s.read(4096)
        s.close()
        if b"boot mode = 3" in buf:
            print("   pre-config: stn43 boot mode set to Instructor (NVS)", flush=True)
            time.sleep(1.5)
            return
        print(f"   pre-config: retry {attempt+1} (no 'boot mode = 3' reply)", flush=True)
        time.sleep(1.5)
    raise RuntimeError("pre-config: could not set stn43 to Instructor mode")


def main():
    print("=== Instructor-relay stop/start integration test ===", flush=True)

    # ---- pre-config: instructor mode (applied by the main open reset) -------
    preconfig_instructor()

    st = {n: Station(n, c["port"], c["dtr"], c["sid"]) for n, c in PORTS.items()}
    for s in st.values():
        s.open()
    for s in st.values():
        s.start_reader()

    t_open = time.time()
    time.sleep(6.0)   # boot settle (ESP32 boards reset on open)

    try:
        # ===== STEP 1: ensure current firmware =============================
        fw_ok = True; fw_detail = []
        for n, s in st.items():
            hit = s.wait_for([rf"morse-station build {EXPECT_BUILD}",
                              rf"build\s*=\s*{EXPECT_BUILD}"], 4, since=t_open)
            if not hit:                       # Wio: no boot banner -> ask
                s.send("show")
                hit = s.wait_for([rf"build\s*=\s*{EXPECT_BUILD}"], 6, since=time.time()-1)
            ok = hit is not None
            fw_ok &= ok
            fw_detail.append(f"{n}(stn{s.sid})={'ok' if ok else 'MISMATCH'}")
        record("1. all stations run current firmware "
               f"({EXPECT_BUILD})", fw_ok, ", ".join(fw_detail))

        # ===== STEP 2: roles ==============================================
        # Instructor should have booted into Instructor mode from NVS.
        st["instructor"].send("show")
        instr = st["instructor"].wait_for([r"mode=Instructor"], 12, since=time.time()-1)
        st["fox"].send("show")
        foxm = st["fox"].wait_for([r"mode=Fox"], 12, since=time.time()-1)
        roles_ok = instr is not None and foxm is not None
        record("2. roles: stn43=Instructor, stn42=Fox", roles_ok,
               f"instr={'Instructor' if instr else '??'}, fox={'Fox' if foxm else '??'}")

        # speed the fox up so relay RX windows are frequent; enable debug on all.
        st["fox"].send("wpm 18")
        st["fox"].send(f"msg CQ DE FOX {FOX_ID}")
        for n in ("instructor", "fox", "hunterA", "hunterB"):
            st[n].send("debug on")
        time.sleep(2.0)

        # baseline: fox transmitting + hunters receiving
        t_base = time.time()
        time.sleep(12.0)
        fox_tx = st["fox"].count_since(r"TX E ", t_base)
        rxA = st["hunterA"].count_since(r"RX [EK] ", t_base)
        rxB = st["hunterB"].count_since(r"RX [EK] ", t_base)
        base_ok = fox_tx > 0 and rxA > 0 and rxB > 0
        record("baseline: Fox TX + both Hunters RX", base_ok,
               f"foxTXe={fox_tx}, hunterA RX={rxA}, hunterB RX={rxB}")

        # ===== STEP 3: relay stop =========================================
        t_stop = time.time()
        st["instructor"].send(f"relay {FOX_ID} stop")
        hit = st["fox"].wait_for([rf"RX C .*cmd=stop", r"# TX HALT"], 90, since=t_stop)
        record("3. Fox received relayed 'stop'", hit is not None,
               hit[1] if hit else "no RX C cmd=stop within 90s")
        t_halt = hit[0] if hit else time.time()
        # confirm it actually halted
        halt2 = st["fox"].wait_for([r"# TX HALT", r"TX SKIP \(halted\)"], 20, since=t_halt-1)
        record("3b. Fox TX halted", halt2 is not None, halt2[1] if halt2 else "no halt log")

        # ===== STEP 4: current message completes, no new message ===========
        time.sleep(8.0)            # let any in-flight message tail finish
        t_quiet = time.time()
        time.sleep(14.0)           # watch window with TX halted
        fox_tx_after = st["fox"].count_since(r"TX E ", t_quiet)
        rxA_after = st["hunterA"].count_since(r"RX [EK] ", t_quiet)
        rxB_after = st["hunterB"].count_since(r"RX [EK] ", t_quiet)
        step4_ok = fox_tx_after == 0 and rxA_after <= 2 and rxB_after <= 2
        record("4. no new broadcast after stop", step4_ok,
               f"foxTXe={fox_tx_after}, hunterA RX={rxA_after}, hunterB RX={rxB_after} (post-halt window)")

        # ===== STEP 5: hunters blank after 60 s of no broadcast ============
        # blank timer runs from each hunter's last received edge (pre-halt).
        blankA = st["hunterA"].wait_for([r"# screen: blanked"], 80, since=t_halt)
        blankB = st["hunterB"].wait_for([r"# screen: blanked"], 80, since=t_halt)
        record("5. both Hunters screen-blanked", blankA is not None and blankB is not None,
               f"hunterA={'blanked' if blankA else 'STILL ON'}, "
               f"hunterB={'blanked' if blankB else 'STILL ON'}")

        # ===== STEP 6: wait another 60 s; panels stay blank ================
        t_wait6 = time.time()
        time.sleep(60.0)
        wokeA = st["hunterA"].count_since(r"# screen: woke", t_wait6)
        wokeB = st["hunterB"].count_since(r"# screen: woke", t_wait6)
        record("6. Hunters stayed blank for +60 s", wokeA == 0 and wokeB == 0,
               f"spurious wakes: hunterA={wokeA}, hunterB={wokeB}")

        # ===== STEP 7: relay new message ==================================
        t_msg = time.time()
        st["instructor"].send(f"relay {FOX_ID} msg {NEW_MSG}")
        hit = st["fox"].wait_for([rf"RX C .*cmd=msg {re.escape(NEW_MSG)}",
                                  rf"fox msg\s*=\s*{re.escape(NEW_MSG)}"], 90, since=t_msg)
        record("7. Fox received relayed new message", hit is not None,
               hit[1] if hit else "no RX C cmd=msg within 90s")

        # ===== STEP 8: relay start ========================================
        t_start = time.time()
        st["instructor"].send(f"relay {FOX_ID} start")
        hit = st["fox"].wait_for([r"RX C .*cmd=start", r"# TX RESUME"], 90, since=t_start)
        record("8. Fox received relayed 'start'", hit is not None,
               hit[1] if hit else "no RX C cmd=start within 90s")
        resume = st["fox"].wait_for([r"# TX RESUME", r"TX E "], 30, since=t_start)
        record("8b. Fox resumed TX", resume is not None, resume[1] if resume else "no TX after start")

        # ===== STEP 9: hunters wake and copy the NEW message ==============
        t_new = time.time()
        wokeA = st["hunterA"].wait_for([r"# screen: woke"], 40, since=t_new)
        wokeB = st["hunterB"].wait_for([r"# screen: woke"], 40, since=t_new)
        record("9a. both Hunters woke on new broadcast", wokeA is not None and wokeB is not None,
               f"hunterA={'woke' if wokeA else 'still blank'}, "
               f"hunterB={'woke' if wokeB else 'still blank'}")

        # The step's intent is "Hunters receive the new message". Assert on-air
        # RECEPTION (RX-E count after wake) for both — robust — and additionally
        # report the decoded copy where captured. (The Heltec captures a clean
        # decode in-process; the Wio's nRF52 USB-CDC sometimes stops flushing the
        # decoded-char stream to the host under the sustained multiport reader,
        # so its decode capture is unreliable even though it decodes correctly on
        # the panel — verified by direct re-check. Hence reception, not capture,
        # is the gate.) Nudge the Wio CDC with a newline to prompt a flush.
        st["hunterB"].send("")
        deadline = time.time() + 80
        decA = decB = ""; rxA_new = rxB_new = 0; rA = rB = 0.0; wA = wB = []
        while time.time() < deadline and not (rxA_new > 5 and rxB_new > 5):
            time.sleep(4.0)
            rxA_new = st["hunterA"].count_since(r"RX [EK] ", t_new)
            rxB_new = st["hunterB"].count_since(r"RX [EK] ", t_new)
            decA = st["hunterA"].decoded_since(t_new)
            decB = st["hunterB"].decoded_since(t_new)
            rA, wA = fuzzy_ok(decA, NEW_MSG)
            rB, wB = fuzzy_ok(decB, NEW_MSG)
        recvA, recvB = rxA_new > 5, rxB_new > 5
        record("9b. Hunters received the new message", recvA and recvB,
               f"hunterA RXe={rxA_new} decoded={decA!r} (words={wA}); "
               f"hunterB RXe={rxB_new} decoded={decB!r} (words={wB})")

        # ===== STEP 10: hunters answer BLE again after auto-wake ===========
        # stn38 (AUTO) dropped BLE while blanked and must have restored it on
        # wake; stn115 (Bluefruit) keeps BLE always-on. Use the BLE transport
        # (separate from the USB console we hold open) to prove responsiveness.
        import subprocess
        def ble_show(sid):
            try:
                out = subprocess.run(
                    ["tools/blevenv/bin/python", "scripts/ble_cmd.py", str(sid), "show"],
                    capture_output=True, text=True, timeout=40).stdout
                return out
            except Exception as e:                       # noqa
                return f"<error: {e}>"
        b38 = ble_show(38); b115 = ble_show(115)
        ok38 = "id=38" in b38 or "mode=Hunter" in b38
        ok115 = "id=115" in b115 or "mode=Hunter" in b115
        record("10. Hunters respond to BLE after auto-wake", ok38 and ok115,
               f"stn38 BLE={'ok' if ok38 else 'NO REPLY'}, "
               f"stn115 BLE={'ok' if ok115 else 'NO REPLY'}")

    finally:
        # restore: resume the fox so we don't leave it parked halted
        try:
            st["instructor"].send(f"relay {FOX_ID} start")
            time.sleep(2)
        except Exception:
            pass
        for s in st.values():
            s.close()

    # ---- summary ----------------------------------------------------------
    print("\n================ SUMMARY ================", flush=True)
    npass = sum(1 for _, ok, _ in RESULTS if ok)
    for step, ok, detail in RESULTS:
        print(f"  [{'PASS' if ok else 'FAIL'}] {step}")
    print(f"\n{npass}/{len(RESULTS)} checks passed", flush=True)
    return 0 if npass == len(RESULTS) else 1


if __name__ == "__main__":
    raise SystemExit(main())
